#include "world/props/prop_system.h"

#include "core/camera.h"
#include "core/logger.h"
#include "world/world_system.h"
#include "world/props/prop_scatter.h"    // scatterPropsNear + IPropSphereField (scatter GLOBAL)
#include "world/props/prop_collider.h"   // colliders por PARTE derivados del esqueleto del arbol
#include "world/planet/planetary_system.h"
#include "world/planet/planet.h"
#include "physics/physics_engine.h"
#include "renderer/fallback_texture.h"
#include "renderer/shader.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gpu_scope.h"
#include "tools/profiler.h"
#include "tools/procgraph/tree_mesh.h"
#include "tools/procgraph/prop_mesh.h"
#include "tools/procgraph/clump_mesh.h"           // bakeClumpMesh + kClumpFootprintM (cúmulos de lejanía)
#include "tools/procgraph/tree_textures.h"   // TreeCombineRGBNode (bake de material per-pixel)
#include "tools/procgraph/proc_texture.h"    // evaluateToRGBA / evaluateToNormalMap / createRHIFromRGBA

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>   // glm::mat4_cast
#include <glm/gtx/quaternion.hpp>   // glm::rotation (up del prototipo → dir radial)

namespace Haruka::World {

using Haruka::RHI::valid;
using Renderer::FallbackTex;
using Renderer::fallbackTexture;

namespace {

// Bits de `matPBR.w`: que slots de textura estan REALMENTE atados este draw (mismo esquema que el
// pase de escena y que prop_inst.frag). Potencias de 2 → exacto en float.
enum MaterialTexBit { kTexAlbedo = 1, kTexNormal = 2, kTexMetallic = 4, kTexRoughness = 8, kTexAO = 16 };

// UBO del pase de PROPS instanciados (binding 6) — espejo de `PropParams` en prop_inst.vert/frag.
// Los escalares del material son del PROTOTIPO (compartido por sus instancias), por eso viven aqui.
struct PropParams {
    glm::vec3 wind;   float time;       // viento del clima (mundo, m/s) · segundos
    glm::vec4 matPBR;                   // x=metallic y=roughness z=ao w=mascara de texturas (bits)
    glm::vec4 originRel;                // origen del scatter relativo a la camara (ver m_gpu)
    glm::vec4 aerial;                   // perspectiva aerea (lib/aerial.glsl): x = 1/L · y = dia
};
static_assert(sizeof(PropParams) == 64, "PropParams std140 size mismatch");
/// UBO del pase de sombras de props: matriz de luz + el mismo origen relativo.
struct PropShadowUBO {
    glm::mat4 lightSpace;
    glm::vec4 originRel;
};
static_assert(sizeof(PropShadowUBO) == 80, "PropShadowUBO std140 size mismatch");

// Vertice del PROTOTIPO (binding 0): Pos(0)/Normal(1)/Color(2)/Uv(9)/Parte(11). El `Vertex` de
// escena no tiene canal de color, asi que el prototipo usa su propio layout (el Uv a 9 no choca con
// el stream de instancia que ocupa loc 3-8; la 10 es la mascara de roturas, por instancia).
struct PropVertex {
    glm::vec3 pos, normal, color;
    glm::vec2 uv;
    /// PARTE del esqueleto (tronco = 0, ramas = 1..n). Va como float: el valor es un indice pequeño
    /// y `float` lo representa exacto; como PATRON DE BITS en un R32F caeria en los denormales.
    float partId = 0.0f;
};

// Material PER-PIXEL del prototipo: el MISMO grafo de texturas del editor de node graph (perlin →
// altura → albedo por rampa + normal por derivadas + AO/roughness desde la altura), horneado a CPU
// y subido a GPU sin pasar por disco. El albedo MODULA el color de vertice (que lleva la identidad
// del material: corteza/copa, muro/tejado, tinte de roca).
struct PropMaterialBake {
    RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
    float metallicS = 0.0f, roughnessS = 0.5f, aoS = 1.0f;
    uint32_t mask = 0u;
};

PropMaterialBake bakePropPrototypeMaterial(uint32_t seed, int size = 128) {
    using namespace Haruka::Tools::ProcGraph;
    PropMaterialBake m;

    // Tono por defecto: calido-vegetal neutro de bajo contraste (el color de vertice manda).
    const glm::vec3 tintA(1.04f, 1.00f, 0.92f), tintB(0.80f, 0.76f, 0.70f);
    Graph g;
    int h  = g.emplaceNode<PerlinNode>((int)seed, 7.0f);
    int nr = g.emplaceNode<MapRangeNode>(-1.0f, 1.0f, 0.0f, 1.0f);
    g.connect(h, 0, nr, 0);
    auto ramp = [&](float a, float b) -> int {
        auto n = std::make_unique<ColorRampNode>();
        n->setStops({{0.0f, a}, {1.0f, b}});
        return g.addNode(std::move(n));
    };
    int rR = ramp(tintA.r, tintB.r);
    int rG = ramp(tintA.g, tintB.g);
    int rB = ramp(tintA.b, tintB.b);
    int combine = g.emplaceNode<TreeCombineRGBNode>();
    g.connect(rR, 0, combine, 0);
    g.connect(rG, 0, combine, 1);
    g.connect(rB, 0, combine, 2);
    if (!g.compile()) return m;

    // Albedo PRE-GAMMA: la textura sube RGBA8 sin tag sRGB y prop_inst.frag linealiza con
    // `pow(tex, 2.2)`. Guardar c^(1/2.2) hace que el muestreo recupere el tono horneado exacto.
    int cmap[4] = {0, 1, 2, -1};
    RGBAImage alb = evaluateToRGBA(g, combine, 0, size, size, 0, 0, 1, cmap);
    const float invGamma = 1.0f / 2.2f;
    for (size_t i = 0; i < alb.pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const float v = alb.pixels[i + (size_t)c] / 255.0f;
            alb.pixels[i + (size_t)c] = (uint8_t)(std::pow(v, invGamma) * 255.0f);
        }
        alb.pixels[i + 3] = 255;
    }

    // Normal desde la altura (derivadas), AO y roughness por-pixel desde la misma altura.
    RGBAImage nrm = evaluateToNormalMap(g, nr, 0, size, size, 0, 0, 1, 1.5f);
    RGBAImage ao(size, size);
    RGBAImage rough(size, size);
    const float dark  = 0.45f;                 // AO: hendiduras (h baja) oscuras
    const float rLo   = 0.55f, rHi = 0.90f;    // roughness: valles mas lisos, crestas asperas
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float hh = g.evaluate(nr, 0, (float)x, (float)y, 0).asFloat();
            const uint8_t av = (uint8_t)(glm::clamp(1.0f - hh * dark, 0.0f, 1.0f) * 255.0f);
            ao.setPixel(x, y, av, av, av);
            const uint8_t rv = (uint8_t)((rLo + (rHi - rLo) * hh) * 255.0f);
            rough.setPixel(x, y, rv, rv, rv);
        }
    }

    m.albedo    = createRHIFromRGBA(alb);
    m.normal    = createRHIFromRGBA(nrm);
    m.roughness = createRHIFromRGBA(rough);
    m.ao        = createRHIFromRGBA(ao);
    if (valid(m.albedo))    m.mask |= kTexAlbedo;
    if (valid(m.normal))    m.mask |= kTexNormal;
    if (valid(m.roughness)) m.mask |= kTexRoughness;
    if (valid(m.ao))        m.mask |= kTexAO;
    m.metallicS = 0.0f; m.roughnessS = rLo; m.aoS = 1.0f;
    return m;
}

// El campo del planeta visto por el scatter: la MISMA ecologia/cota por direccion que pinta el
// terreno (`TerrestrialPlanet`), redirigida por direccion.
struct PlanetSphereField : Haruka::Planet::IPropSphereField {
    const Haruka::Planet::TerrestrialPlanet* planet = nullptr;

    Haruka::FieldSample sampleAt(const glm::vec3& dir) const override {
        return planet ? planet->fieldSampleAt(dir) : Haruka::FieldSample{};
    }
    float heightAt(const glm::vec3& dir) const override {
        return planet ? (float)planet->sampleHeight(dir) : 0.0f;
    }
    float mapDensityAt(const glm::vec3& dir, const std::string& mapPath) const override {
        return planet ? planet->densityMapAt(mapPath, dir) : 1.0f;
    }
    std::string zoneAt(const glm::vec3& dir) const override {
        return planet ? planet->zoneNameAt(dir) : std::string{};
    }
    std::string layerAt(const glm::vec3& dir) const override {
        return planet ? planet->materialNameAt(dir) : std::string{};
    }
};

// Hornea la malla de un prototipo a un detalle dado. ⚠️ La clasificacion y los parametros del
// arbol viven en `prop_collider.h`: el COLLIDER se deriva del mismo esqueleto que esta malla. Si el
// bake decidiera por su cuenta que es un arbol y con que altura, chocarias con otro arbol.
Haruka::Tools::ProcGraph::TreeMeshData bakePrototypeMesh(const InstancedPrototype& proto, float detail) {
    Haruka::Tools::ProcGraph::TreeMeshData tm;
    // CÚMULO de lejanía: el prefijo `clump#` lo firma — bake APLANADO (la escala de instancia
    // estira la celda por el ancho; si fuera alto también parecería un rascacielos).
    if (Haruka::Planet::isClumpProto(proto.name)) {
        const Haruka::Planet::PropTreeParams tp = Haruka::Planet::propTreeParams(proto.name);
        return Haruka::Tools::ProcGraph::bakeClumpMesh(
            proto.meshSeed, tp.style, tp.height, tp.trunkR, tp.canopy,
            Haruka::Tools::ProcGraph::kClumpFootprintM, detail);
    }
    const Haruka::Planet::PropShapeKind shape = Haruka::Planet::propShapeKind(proto.name);
    if (shape == Haruka::Planet::PropShapeKind::Rock) {
        tm = Haruka::Tools::ProcGraph::bakeRockMesh((int)proto.meshSeed, 1.0f, 0.72f);
    } else if (shape == Haruka::Planet::PropShapeKind::House) {
        tm = Haruka::Tools::ProcGraph::bakeHouseMesh((int)proto.meshSeed, 1.0f);
    } else {
        const Haruka::Planet::PropTreeParams tp = Haruka::Planet::propTreeParams(proto.name);
        Haruka::Tools::ProcGraph::Graph g;
        const int tree = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
            (int)proto.meshSeed, tp.height, tp.trunkR, tp.canopy, tp.segments, detail, tp.style);
        g.compile();
        Haruka::Tools::ProcGraph::bakeTreeMesh(g, tree, tm);   // vacio si el nodo no es valido
    }
    return tm;
}

// `HARUKA_NOPROPS=1` APAGA EL CAMINO ENTERO (scatter, colliders y dibujo), como biseccion.
bool noProps() {
    static const bool s = [] { const char* e = std::getenv("HARUKA_NOPROPS"); return e && e[0] == '1'; }();
    return s;
}

} // namespace

// ── vida ──────────────────────────────────────────────────────────────────────────────────────

PropSystem::~PropSystem() { shutdown(); }

void PropSystem::shutdown() {
    // El worker de re-siembra puede estar leyendo el campo del planeta mientras soltamos la GPU:
    // esperarle AQUÍ, antes de destruir nada (el futuro de un `std::async` también bloquea al
    // destruirse, pero mejor unir con la cola del campo viva).
    if (m_scatterBusy && m_scatterFut.valid()) {
        try { m_scatterFut.get(); }
        catch (const std::exception& e) { HARUKA_LOGE("PropSystem", "shutdown: re-siembra abortada: %s", e.what()); }
        m_scatterBusy = false;
        m_scatterFut  = {};
    }
    RHI::Device* dev = RHI::device();
    if (dev) {
        for (auto& [name, pg] : m_protoMesh) {
            for (int l = 0; l < PrototypeGpu::kLods; ++l) {
                if (valid(pg.vbo[l])) dev->destroy(pg.vbo[l]);
                if (valid(pg.ebo[l])) dev->destroy(pg.ebo[l]);
            }
            for (RHI::TextureHandle* t : { &pg.albedo, &pg.normal, &pg.metallic, &pg.roughness, &pg.ao })
                if (valid(*t)) dev->destroy(*t);
        }
        for (auto& b : m_paramsUBOs) if (valid(b)) dev->destroy(b);
        if (valid(m_shadowUBO)) dev->destroy(m_shadowUBO);
        if (valid(m_instPSO))   dev->destroy(m_instPSO);
        if (valid(m_shadowPSO)) dev->destroy(m_shadowPSO);
    }
    m_protoMesh.clear(); m_paramsUBOs.clear();
    m_shadowUBO = {}; m_instPSO = {}; m_shadowPSO = {};
    m_instancing.reset();
}

// ── prototipos ────────────────────────────────────────────────────────────────────────────────

const PropSystem::PrototypeGpu* PropSystem::prototypeGpu(const InstancedPrototype& proto) {
    auto it = m_protoMesh.find(proto.name);
    if (it != m_protoMesh.end()) return &it->second;
    RHI::Device* dev = RHI::device();
    if (!dev) return nullptr;

    // ESCALERA DE DETALLE. Los numeros salen de medir la malla, no a ojo:
    //   detalle 1.00 -> 464 triangulos (la de siempre) · 0.60 -> 130 · 0.45 -> 40
    // Con la distribucion real del scatter (casi todo esta lejos) esto lleva los 3,24 M de
    // triangulos de arbol a ~0,27 M: -92 %. Roca y casa no tienen mando de detalle (126 y 22
    // triangulos): sus tres niveles salen identicos y el camino de dibujo queda uniforme.
    static constexpr float kLodDetail[PrototypeGpu::kLods] = { 1.0f, 0.60f, 0.45f };
    PrototypeGpu pg;
    bool anyLevel = false;
    for (int lod = 0; lod < PrototypeGpu::kLods; ++lod) {
        const Haruka::Tools::ProcGraph::TreeMeshData tm = bakePrototypeMesh(proto, kLodDetail[lod]);
        if (tm.positions.empty()) continue;
        std::vector<PropVertex> verts;
        verts.reserve(tm.positions.size());
        for (size_t vi = 0; vi < tm.positions.size(); ++vi) {
            PropVertex pv;
            pv.pos    = tm.positions[vi];
            pv.normal = (vi < tm.normals.size()) ? tm.normals[vi] : glm::vec3(0, 1, 0);
            pv.color  = (vi < tm.colors.size())  ? tm.colors[vi]  : glm::vec3(1.0f);
            pv.uv     = (vi < tm.uvs.size())     ? tm.uvs[vi]     : glm::vec2(0.0f);
            // Roca/casa no traen partId (sin esqueleto): parte 0, la que se lleva el prop entero.
            pv.partId = (vi < tm.partId.size()) ? (float)tm.partId[vi] : 0.0f;
            verts.push_back(pv);
        }
        pg.vbo[lod] = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(PropVertex), verts.data());
        pg.ebo[lod] = dev->createBuffer(RHI::BufferUsage::Index, tm.indices.size() * sizeof(unsigned int), tm.indices.data());
        pg.indexCount[lod]  = (uint32_t)tm.indices.size();
        pg.vertexCount[lod] = (uint32_t)verts.size();
        anyLevel = true;
    }
    if (!anyLevel) return nullptr;   // bake fallido: se reintenta el frame siguiente

    // Material PER-PIXEL, una vez, COMPARTIDO por los niveles.
    const PropMaterialBake pm = bakePropPrototypeMaterial(proto.meshSeed);
    pg.albedo = pm.albedo; pg.normal = pm.normal; pg.metallic = pm.metallic;
    pg.roughness = pm.roughness; pg.ao = pm.ao;
    pg.metallicS = pm.metallicS; pg.roughnessS = pm.roughnessS; pg.aoS = pm.aoS; pg.mask = pm.mask;
    return &m_protoMesh.emplace(proto.name, pg).first->second;
}

// Las matrices de las instancias, UNA vez por scatter. El origen es la camara en ese momento: las
// posiciones relativas caben en float con precision de mm (el scatter llega a 6 km) y el shader
// suma `originRel` (origen − camara actual), una resta en double por frame.
void PropSystem::rebuildGpu(const glm::dvec3& planetC, double planetR, Core::Camera* camera) {
    const auto& insts = m_registry.instances();
    m_origin = camera ? glm::dvec3(camera->position) : planetC;
    m_gpu.resize(insts.size());
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::dvec3 originToCenter = planetC - m_origin;
    for (size_t i = 0; i < insts.size(); ++i) {
        const auto& io = insts[i];
        const glm::vec3 posF = glm::vec3(originToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
        glm::mat4 m = glm::translate(glm::mat4(1.0f), posF);
        m *= glm::mat4_cast(glm::rotation(up, io.dir));
        // ⚠️ El giro de yaw va sobre el eje LOCAL, no sobre `io.dir`. `glm::rotate` POST-multiplica:
        // el eje se interpreta en el espacio de `m`, que ya lleva la rotacion up→dir. Pasarle
        // `io.dir` TUMBABA el objeto tanto como lo giraba ("los arboles estan tumbados").
        m = glm::rotate(m, io.yaw, up);
        m = glm::scale(m, glm::vec3(io.scale));
        InstanceDataFloat& inst = m_gpu[i];
        inst.model     = m;
        inst.color     = glm::vec4(io.tint, 1.0f);
        inst.scale     = glm::vec3(io.scale);
        inst.breakMask = (float)io.breakMask;   // ramas que ya no estan
    }
}

// ── scatter ───────────────────────────────────────────────────────────────────────────────────

void PropSystem::refreshScatter(Core::Camera* camera, PlanetarySystem* planets, Physics::PhysicsEngine* physics) {
    // ⚠️ AQUI DECIA QUE EL ANCLAJE DE LOS PROPS ESTABA "17x MAS BASTO QUE EL RENDER A 5 km" Y YA NO
    // ES CIERTO: medido, altura del prop contra la del suelo DIBUJADO es sub-pixel en todo el rango y
    // exacta dentro de 150 m (0,16 px a 600 m). Lo que NO cubre esa medida es el reparto del scatter.
    if (!m_enabled || noProps()) {
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        m_scatterDebug.clear();
        m_planet.clear();
        m_lastCam = {1e300, 1e300, 1e300};
        return;
    }
    if (!camera || !planets) return;
    const glm::dvec3 camPos = glm::dvec3(camera->position);
    // Reloj del tramo adaptivo. Se actualiza al LANZAR un worker y al APLICAR su resultado (abajo):
    // la velocidad se mide SIEMPRE desde el último evento, no desde el arranque del proceso (antes
    // nunca se tocaba y la adaptación a la velocidad era agua muerta).
    static std::chrono::steady_clock::time_point s_lastT = std::chrono::steady_clock::now();

    glm::dvec3 planetC; double planetR = 0.0;
    if (!planets->getActivePlanet(planetC, planetR)) {
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        m_planet.clear();
        return;
    }
    const Haruka::Planet::TerrestrialPlanet* planet = planets->activeTerrestrial();
    const std::string pname = planets->getActivePlanetName();
    if (!planet) {                                   // planeta orbital sin superficie = sin props
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        m_planet.clear();
        m_lastCam = {1e300, 1e300, 1e300};
        return;
    }
    if (pname != m_planet) {
        m_registry.reset();   // planeta nuevo: re-registrar prototipos
        m_planet = pname;
    }

    // ── RE-SIEMBRA ASINCRONA ────────────────────────────────────────────────────────────────
    // `scatterPropsNear` (~61k celdas × muestreo del campo) era el grueso de los tirones de 181 ms
    // al cruzar un tramo. Ahora corre en un hilo worker: SOLO lee el CAMPO (el bake de alturas y los
    // mapas son inmutables tras `bakeHeightMap`; "seguro desde el hilo async", planetary_system.cpp:
    // 1241) y una copia de la tabla de capas. Nunca toca el registro ni la GPU. El frame muestra el
    // anillo anterior mientras calcula y publica el resultado cuando esté listo. El profiler es
    // thread_local, así que el trabajo del worker NO aparece en el árbol del frame (correcto: no
    // es trabajo del frame).
    //
    // ⚠️ EL ACK SE RETRASA ADREDE A LA PUBLICACIÓN Y SE RE-ANCLA A LA CÁMARA ACTUAL. `m_lastCam`
    // se fija al APLICAR el resultado, no al lanzarlo — pero a la cámara de AHORA (camPos), no al
    // punto de lanzamiento: el tramo siguiente se mide desde donde está el jugador, y el anillo
    // nuevo se re-genera centrado en él. Antes se anclaba a `m_scatterCam` (~180 ms atrás) y al
    // volar el anillo acababa quedando un tramo entero detrás ("se ve lejos y no se propaga"). Si
    // durante el vuelo la cámara YA cruzó otro tramo (aceleración brusca) se relanza AL INSTANTE,
    // centrado en la cámara actual, en vez de esperar a la siguiente muestra.
    bool relaunchNow = false;
    { HARUKA_PROFILE("re-siembra.aplica");   // checkout del worker + apply + re-lanzamiento rapido
    if (m_scatterBusy) {
        if (m_scatterFut.valid() &&
            m_scatterFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                ScatterResult res = m_scatterFut.get();
                m_scatterBusy = false;
                m_scatterFut  = {};
                // El resultado vive en el planeta con el que se lanzó: si el usuario cambió de
                // planeta durante el vuelo se descarta (el nuevo ya disparará su re-siembra).
                if (m_scatterPlanet == pname && pname == m_planet) {
                    applyScatterResult(std::move(res), planetC, planetR, camera, physics, planet);
                    m_voxVersion = planet->vox().version();   // ACK del campo: ya reflejado
                    m_lastCam    = camPos;                    // ACK: RE-ANCLA el tramo a la cámara ACTUAL
                }
                // Velocidad fresca: `s_lastT` se quedó en el lanzamiento del anillo que acaba de
                // aterrizar. Si la cámara ya cruzó OTRO tramo durante el vuelo, relanzar ahora.
                const double flightS = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - s_lastT).count();
                const double distLaunch = glm::length(camPos - m_scatterCam);
                const double speedL     = flightS > 1e-3 ? distLaunch / flightS : 0.0;
                s_lastT = std::chrono::steady_clock::now();
                relaunchNow = (m_scatterPlanet == pname && pname == m_planet) &&
                              distLaunch > std::max(30.0, speedL * 1.5);
            } catch (const std::exception& e) {
                HARUKA_LOGE("PropSystem", "re-siembra async fallo: %s", e.what());
                m_scatterBusy = false;
                m_scatterFut  = {};
            }
        }
        if (!relaunchNow) return;   // en vuelo (o recién aplicada): aguantar el anillo anterior
    }
    }   // re-siembra.aplica (tambien sale si se vuelve en vuelo)

    // Solo re-enumera cuando la camara cruza un tramo desde la ultima muestra. El scatter (~61k
    // celdas × muestreo del campo) es caro (~121 ms). El tramo es ADAPTATIVO a la velocidad: al
    // andar bastan 30 m; al volar, ~1,5 s de viaje. Las celdas son MUNDIALES y deterministas.
    // Y cuando el CAMPO cambia (una boca que carga, un trazo): los props que estaban sobre el
    // agujero tienen que irse, aunque no te hayas movido. SEGURO DESPUES del checkout del worker:
    // si está en vuelo aplicamos el resultado ANTES de decidir si hay que relanzar.
    if (!relaunchNow) {
        const auto nowT = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(nowT - s_lastT).count();
        const double distCam = glm::length(camPos - m_lastCam);
        const double speed   = elapsed > 1e-3 ? distCam / elapsed : 0.0;
        const double refreshM = std::max(30.0, speed * 1.5);
        const bool tooSoon = elapsed < 0.5;
        const bool campoCambio = planet->vox().version() != m_voxVersion;
        if ((distCam < refreshM || tooSoon) && !campoCambio && !m_rescatter && !m_planet.empty())
            return;
    }

    // El controlador de carga pidio re-sembrar por un cambio de RADIO: este intento la cubre.
    m_rescatter = false;

    { HARUKA_PROFILE("re-siembra.lanza");   // preparacion (registro de prototipos, copias) + lanzar worker
    PlanetSphereField field;
    field.planet = planet;
    const Haruka::Planet::PropLayerTable& table = planet->propLayers();
    if (table.layers.empty()) return;   // sin capas declaradas = sin props

    // UN prototipo por NOMBRE (el mesh de la capa, o cada variante × cada semilla "conifer#2").
    // `meshSeed` = hash32 de planeta+nombre: un arbol es SIEMPRE el mismo arbol. Los prototipos de
    // árbol (excepto hierba) registran TAMBIÉN sus CÚMULOS (`clump#<proto>#k`): las celdas gruesas
    // de lejanía instalan una masa por celda en vez de individuos (la malla del cúmulo se hornea
    // bajo este mismo nombre). Es DERIVADO (shouldClumpFar), no un flag del JSON.
    auto registerProto = [&](const std::string& pn) {
        for (int i = 0; i < m_registry.prototypeCount(); ++i)
            if (m_registry.prototype(i).name == pn) return;
        InstancedPrototype p;
        p.name = pn;
        uint32_t h = 2166136261u;
        for (const char c : m_planet) h = (h ^ (uint8_t)c) * 16777619u;
        for (const char c : pn)       h = (h ^ (uint8_t)c) * 16777619u;
        p.meshSeed = Haruka::Tools::ProcGraph::hash32(h);
        p.lodLevel = 0;
        m_registry.addPrototype(p);
    };
    for (const auto& L : table.layers) {
        for (const std::string& pn : L.prototypeNames()) {
            registerProto(pn);
            if (Haruka::Planet::shouldClumpFar(pn))
                for (int k = 0; k < Haruka::Planet::kClumpKinds; ++k)
                    registerProto(Haruka::Planet::clumpProtoName(pn, k));
        }
    }
    if (m_registry.prototypeCount() == 0) return;

    Haruka::Planet::PropScatterParams params;
    params.radius   = planetR;
    params.seed     = planet->config().seed ? planet->config().seed : 1u;
    params.maxProps = 300000;
    params.fadeInM  = kRingFadeM;
    // ⚠️ NO hay horizonte de cúmulos "por carga" NI "por autor" AQUÍ. La fusión sale del bioma/clima
    // (la capa sólo se coloca donde su ecología la acepta) + el NIVEL de celda del cubo (lo decide
    // shouldClumpFar dentro del scatter): el contenido de una celda es función de (celda, seed del
    // planeta, config) y nada más — reproducible entre jugadores y sincronizable al destruirse. La
    // dinámica del presupuesto se queda en la RENDERIZACIÓN (umbrales por píxel, cull), que no toca
    // lo que existe en el mundo.
    // Banda exterior por carga: el radio lo manda el controlador (`m_ringM`); las coronas se
    // estiran con el — celdas ~proporcionales al radio, asi la densidad por area no explota y el
    // recuento crece logaritmico (no cuadratico). Con m_ringM=6000 (arranque, piso) es EXACTAMENTE
    // el anillo historico {1000/12, 3000/40, 6000/120}: en reposo nada cambia.
    params.lods.clear();
    {
        struct B { float rad; float cell; };
        const std::vector<B> kBase = { {1000.0f, 12.0f}, {3000.0f, 40.0f}, {6000.0f, 120.0f} };
        for (const B& b : kBase)
            if (b.rad <= m_ringM) params.lods.push_back({ b.rad, b.cell, 0.30f });
        for (float rad = 9000.0f; rad <= m_ringM; rad += 3000.0f)
            params.lods.push_back({ rad, 120.0f * (rad / 6000.0f), 0.30f });   // ceil gruesa y creciente
        if (params.lods.empty()) params.lods.push_back({ m_ringM, 12.0f, 0.30f });
    }

    // El worker construye ADEMÁS las InstancedObject finales (prototipo + yaw + estado) y sus MATRICES
    // GPU: se le pasan copias del nombre→índice y del estado roto. `m_state` la toca el juego en el
    // hilo de render; esta copia es la foto de lanzamiento, y `applyScatterResult` vuelve a cuadrar
    // el estado ACTUAL y el filtro de bocas al aplicar (no puede ir aquí: `surfaceCut` lee el vox).
    std::unordered_map<std::string, int> protoByName;
    for (int i = 0; i < m_registry.prototypeCount(); ++i)
        protoByName.emplace(m_registry.prototype(i).name, i);
    const std::unordered_map<uint32_t, StateDelta> stateSnap = m_state;
    const uint32_t planetSeed = params.seed;

    m_scatterBusy  = true;
    m_scatterPlanet = pname;
    m_scatterCam    = camPos;
    s_lastT = std::chrono::steady_clock::now();   // el tramo adaptivo se mide desde ESTE lanzamiento
    m_scatterFut = std::async(std::launch::async,
        [field, camPos, planetC, planetR, params, table, protoByName, stateSnap, planetSeed]() {
            // ⚠️ CAPAS MUERTAS. Cada celda SORTEA entre las capas que la aceptan, asi que una capa a
            // cero ya no puede ser "otra se la comio": sus condiciones no pasan en ninguna celda, o
            // ninguna de sus variantes vive ahi. El desglose (una vez) dice cual de los factores.
            static bool s_statsReported = false;
            Haruka::Planet::PropScatterStats stats;
            const std::vector<Haruka::Planet::ScatteredProp> placed =
                Haruka::Planet::scatterPropsNear(field, camPos, planetC, params, table,
                                                s_statsReported ? nullptr : &stats);
            if (!s_statsReported && !placed.empty()) {
                s_statsReported = true;
                for (size_t li = 0; li < table.layers.size(); ++li) {
                    const auto& L = table.layers[li];
                    if (L.mesh.empty() || stats.placed[li] > 0) continue;
                    HARUKA_LOGW("PropScatter",
                        "capa %zu ('%s', mesh '%s'): 0 props en %d celdas. Descartes: sumergido=%d densidad=%d sin_variante=%d · coverage=%d de los cuales "
                        "zona=%d when=%d humedad=%d temp=%d pendiente=%d mapa=%d",
                        li, L.name.c_str(), L.mesh.c_str(), stats.offered[li],
                        stats.noSubmerged[li], stats.noDensity[li], stats.noVariant[li], stats.noCoverage[li],
                        stats.failZone[li], stats.failWhen[li], stats.failHum[li],
                        stats.failTemp[li], stats.failSlope[li], stats.failMap[li]);
                    if (stats.failHum[li] > 0)
                        HARUKA_LOGW("PropScatter", "   humedad: la capa pide [%.2f, %.2f] · el terreno da [%.2f, %.2f]",
                                    L.humMin, L.humMax, stats.humLo, stats.humHi);
                    if (stats.failTemp[li] > 0)
                        HARUKA_LOGW("PropScatter", "   temp: la capa pide [%.1f, %.1f] C · el terreno da [%.1f, %.1f]",
                                    L.tempMin, L.tempMax, stats.tempLo, stats.tempHi);
                    if (stats.failSlope[li] > 0)
                        HARUKA_LOGW("PropScatter", "   pendiente: la capa pide [%.2f, %.2f] · el terreno da [%.2f, %.2f]",
                                    L.slopeMin, L.slopeMax, stats.slopeLo, stats.slopeHi);
                    if (stats.failZone[li] > 0)
                        HARUKA_LOGW("PropScatter", "   zona: la capa declara %zu zona(s) y es un filtro DURO — "
                                    "fuera de ellas no instala NADA. La escena declara las zonas en surfaceConfig.",
                                    L.zones.size());
                }
                // Tabla bioma × capa del primer scatter: la medida de "los props siguen al bioma".
                std::string line = "biomas en el primer scatter (celdas · colocadas por capa):";
                for (int b = 0; b < (int)Haruka::Planet::Biome::COUNT; ++b) {
                    if (stats.cellsByBiome[b] == 0) continue;
                    line += "\n   " + std::string(Haruka::Planet::biomeKey((Haruka::Planet::Biome)b))
                          + " " + std::to_string(stats.cellsByBiome[b]);
                    for (size_t li = 0; li < table.layers.size(); ++li) {
                        if (table.layers[li].mesh.empty()) continue;
                        line += " · " + table.layers[li].mesh + "=" + std::to_string(
                            stats.placedByBiome[li * (size_t)Haruka::Planet::Biome::COUNT + (size_t)b]);
                    }
                }
                HARUKA_LOGI("PropScatter", "%s", line.c_str());
            }

            // Fase de transformación (antes estaba en `applyScatterResult`, en el hilo de render, y
            // eran los ~50 ms del tiron): prototipo por nombre, yaw determinista y estado inicial,
            // y las MATRICES GPU — todo contra copias capturadas, nunca el registro ni la GPU.
            // ⚠️ El filtro de BOCA no va aquí: `surfaceCut` lee el VOX VIVIENTE, que el hilo de
            // render está streameando/rehorneando; leerlo en paralelo es una raza. Se filtra al
            // aplicar, contra el vox actual.
            std::vector<Haruka::InstancedObject> objs;
            objs.reserve(placed.size());
            for (const auto& sp : placed) {
                auto itP = protoByName.find(sp.mesh);
                if (itP == protoByName.end()) continue;

                Haruka::InstancedObject io;
                io.prototype = itP->second;
                io.dir       = sp.dir;
                io.heightM   = sp.heightM;
                io.scale     = sp.scale;
                io.tint      = sp.tint;
                io.yaw = Haruka::Tools::ProcGraph::WhiteNode::hashFloat((int)sp.cellSeed, 500, 0, planetSeed) * 6.2831853f;
                io.seed  = sp.cellSeed;
                io.state = sp.state;
                auto itS = stateSnap.find(io.seed);
                if (itS != stateSnap.end()) {
                    io.state     = itS->second.state;
                    io.regrow    = itS->second.regrow;
                    io.breakMask = itS->second.breakMask;
                }
                objs.push_back(io);
            }
            // La MITAD del coste del aplicar eran las ~200k matrices (rebuildGpu, ~30 ms). El worker
            // las construye CONTRA EL ORIGEN DE LANZAMIENTO (`camPos`); el pase de color compensa
            // cada frame con `originRel = m_origin - camD`, así que el origen no necesita ser la
            // cámara actual (ya se quedaba viejo entre re-siembras).
            std::vector<InstanceDataFloat> gpu;
            gpu.resize(objs.size());
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            const glm::dvec3 originToCenter = planetC - camPos;   // origin = cámara de lanzamiento
            for (size_t i = 0; i < objs.size(); ++i) {
                const auto& io = objs[i];
                const glm::vec3 posF = glm::vec3(originToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
                glm::mat4 m = glm::translate(glm::mat4(1.0f), posF);
                m *= glm::mat4_cast(glm::rotation(up, io.dir));
                m = glm::rotate(m, io.yaw, up);
                m = glm::scale(m, glm::vec3(io.scale));
                InstanceDataFloat& inst = gpu[i];
                inst.model     = m;
                inst.color     = glm::vec4(io.tint, 1.0f);
                inst.scale     = glm::vec3(io.scale);
                inst.breakMask = (float)io.breakMask;
            }
            return ScatterResult{ std::move(objs), std::move(gpu), camPos };
        });
    }   // re-siembra.lanza
}

void PropSystem::applyScatterResult(ScatterResult res,
                                    const glm::dvec3& planetC, double planetR,
                                    Core::Camera* camera, Physics::PhysicsEngine* physics,
                                    const Haruka::Planet::TerrestrialPlanet* planet) {
    const glm::dvec3 camPos = camera ? glm::dvec3(camera->position) : glm::dvec3(planetC);
    // El scatter regenera las instancias desde CERO: el estado marcado por el juego (destruido,
    // rama arrancada) se conserva por semilla en `m_state`, que sobrevive a que el prop salga del
    // radio. Se sincroniza primero lo que haya en el registro por si algo lo toco sin pasar por
    // `breakAt`, y luego el mapa manda.
    { HARUKA_PROFILE("re-siembra.sync");   // solo guarda los NO-Alive: barrido barato
      for (const auto& io : m_registry.instances()) {
          if (io.state == (uint32_t)InstancedObjectState::Alive && io.breakMask == 0u) continue;
          m_state[io.seed] = StateDelta{ io.seed, io.state, io.breakMask, io.regrow };
      }
    }
    // UN pase en el hilo de render: filtra las BOCAS contra el VOX VIVIENTE (pueden haber cambiado
    // durante el vuelo; por eso no puede ir al worker) y cuadra el estado ACTUAL (el worker usó un
    // snapshot de lanzamiento — lo roto a mitad de vuelo no resucita). `gpu` va en paralelo: la
    // matriz NO depende del estado (solo la `breakMask` del stream), así que se re-etiqueta aquí.
    std::vector<Haruka::InstancedObject> objs;
    objs.reserve(res.objs.size());
    // BUFFER PERSISTENTE para `m_gpu`: un vector nuevo por aplicar era un mmap de ~17 MB por
    // re-siembra; en el primer barrido tras el apply sus páginas estaban aún frías (TLB/cache) y
    // `model[3]` se leía a ~112 ns/instancia (~20 ms) vs ~9 ns/instancia en régimen. Al reusar
    // la capacidad (solo se agranda si el anillo crece), las páginas quedan residentes y el
    // sweep se mantiene en ~2 ms incluso el frame que aplica.
    if (m_gpu.capacity() < res.gpu.size()) m_gpu.reserve(res.gpu.size());
    m_gpu.clear();
    size_t sobreBoca = 0;
    // ⚠️ EL FILTRO DE BOCA ES ~50 MS SI SE HACE POR INSTANCIA. `surfaceCut` paga el muestreo de
    // COTA del planeta (sampleHeight, ~0,4 µs) ANTES de mirar si hay chunk, y con ~130k props lo
    // único que importa es saber dónde el resultado puede ser ≠0. `surfaceCut` sólo puede recortar
    // donde el chunk del cinturón del probe tiene `d` (lo quitado: cuevas, trazos, edición) — si `d`
    // está vacío el chunk es todo roca y devuelve 0 por construcción. Un gate que admitiera
    // "cualquier chunk cargado" seguía pagando la muestra para todos los props del disco visible en
    // el spawn (~22 ms): la malla del terreno carga chunks en todas partes aunque sean roca maciza.
    // Se colecta una vez la lista de columnas con un chunk de HUECO y `surfaceCut` solo se llama en
    // las que existen; en el resto devuelve 0 por construcción (ver `VoxWorld::loadedCutColumns`).
    const std::unordered_set<Haruka::VoxKey, Haruka::VoxKeyHash> voxCols = planet->vox().loadedCutColumns();
    // EL FILTRO (boca + estado) EN PARALELO: es O(N) con dos lookups por instancia (~13 ms a 178k
    // en un hilo) pero SOLO LEE (`voxCols`, `m_state`, `res.gpu`) → se parte en K trozos y cada uno
    // acumula en buffers propios; el hilo de render concatena en orden sobre el nuevo anillo. La
    // generación de matrices GPU ya corre en el worker; esto es lo único del apply que queda aquí.
    const size_t N = res.objs.size();
    const size_t K = std::min<size_t>(8u, std::max<size_t>(1u, N / 20000u));
    std::atomic<size_t> aSobreBoca(0);
    auto filtroRango = [&](size_t b, size_t e) {
        std::vector<Haruka::InstancedObject> lo; lo.reserve(e - b);
        std::vector<InstanceDataFloat> li;       li.reserve(e - b);
        for (size_t i = b; i < e; ++i) {
            Haruka::InstancedObject io = res.objs[i];
            if (!voxCols.empty()) {
                const Haruka::VoxKey ck = planet->vox().columnAt(glm::dvec3(io.dir));
                if (voxCols.count(ck) != 0 && planet->vox().surfaceCut(glm::dvec3(io.dir)) > 0.5f) {
                    aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue;
                }
            }
            if (!m_state.empty()) {
                auto it = m_state.find(io.seed);
                if (it != m_state.end()) {
                    io.state     = it->second.state;
                    io.regrow    = it->second.regrow;
                    io.breakMask = it->second.breakMask;
                }
            }
            InstanceDataFloat inst = res.gpu[i];
            inst.breakMask = (float)io.breakMask;
            lo.push_back(std::move(io));
            li.push_back(inst);
        }
        return std::pair{ std::move(lo) , std::move(li) };
    };
    if (K > 1) {
        std::vector<std::future<std::pair<std::vector<Haruka::InstancedObject>, std::vector<InstanceDataFloat>>>> futs;
        futs.reserve(K);
        for (size_t ki = 0; ki < K; ++ki) {
            const size_t b = N * ki / K, e = N * (ki + 1) / K;
            if (b == e) continue;
            futs.push_back(std::async(std::launch::async, [&, b, e]() { return filtroRango(b, e); }));
        }
        for (auto& f : futs) {
            auto pr = f.get();
            std::vector<Haruka::InstancedObject>& lo = pr.first;
            std::vector<InstanceDataFloat>& li = pr.second;
            m_gpu.insert(m_gpu.end(), li.begin(), li.end());   // `m_gpu` MANTIENE su capacidad persistente
            objs.insert(objs.end(), lo.begin(), lo.end());
        }
    } else {
        for (size_t i = 0; i < N; ++i) {
            Haruka::InstancedObject io = res.objs[i];
            if (!voxCols.empty()) {
                const Haruka::VoxKey ck = planet->vox().columnAt(glm::dvec3(io.dir));
                if (voxCols.count(ck) != 0 && planet->vox().surfaceCut(glm::dvec3(io.dir)) > 0.5f) { aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue; }
            }
            if (!m_state.empty()) {
                auto it = m_state.find(io.seed);
                if (it != m_state.end()) {
                    io.state     = it->second.state;
                    io.regrow    = it->second.regrow;
                    io.breakMask = it->second.breakMask;
                }
            }
            InstanceDataFloat inst = res.gpu[i];
            inst.breakMask = (float)io.breakMask;
            objs.push_back(io);
            m_gpu.push_back(inst);
        }
    }
    sobreBoca = aSobreBoca.load(std::memory_order_relaxed);
    m_origin = res.origin;                    // las matrices del worker son relativas a ESTE origen
    { HARUKA_PROFILE("re-siembra.filtro");
      m_registry.setInstances(std::move(objs));
    }
    if (sobreBoca > 0) HARUKA_LOGDIAG("Vox", "props retirados por estar sobre una boca: %zu", sobreBoca);

    // Los props COLISIONAN: se alimenta aqui porque el scatter es quien cambia el conjunto.
    { HARUKA_PROFILE("re-siembra.colliders");
      refreshColliders(planetC, planetR, camPos, physics);
    }
}

// ── pase de color ─────────────────────────────────────────────────────────────────────────────

void PropSystem::draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) {
    // `HARUKA_NOPROPS=1` salta el pase (A/B de atribucion de GPU: apagar pases y restar).
    if (noProps() || !m_enabled || m_registry.prototypeCount() <= 0 || !ctx || !f.camera) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    HARUKA_PROFILE("scene.prop.instanced"); HARUKA_GPU_SCOPE("scene.prop.instanced");

    if (!valid(m_instPSO)) {
        const std::string vs = Shader::baseDir() + "shaders/prop_inst.vert";
        const std::string fs = Shader::baseDir() + "shaders/prop_inst.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)sizeof(PropVertex) };  // binding 0 = prototipo
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(PropVertex, pos),    RHI::Format::RGB32F, 0 },
            { 1, (uint32_t)offsetof(PropVertex, normal), RHI::Format::RGB32F, 0 },
            { 2, (uint32_t)offsetof(PropVertex, color),  RHI::Format::RGB32F, 0 },
            { 9, (uint32_t)offsetof(PropVertex, uv),     RHI::Format::RG32F,  0 },
            { 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 },
        };
        Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_instPSO = dev->createPipeline(pd);
    }
    if (!valid(m_instPSO)) return;

    if ((int)m_paramsUBOs.size() < m_registry.prototypeCount()) {
        const size_t was = m_paramsUBOs.size();
        m_paramsUBOs.resize((size_t)m_registry.prototypeCount());
        for (size_t i = was; i < m_paramsUBOs.size(); ++i)
            m_paramsUBOs[i] = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PropParams), nullptr, RHI::BufferMemory::Dynamic);
    }

    // Viento del CLIMA (m/s, marco del observador) y reloj del balanceo.
    glm::vec3 windWorld(0.0f);
    if (f.world) windWorld = f.world->getWind(glm::dvec3(f.camera->position));
    // ⚠️ ESTE RELOJ ES DE PARED, no el de simulacion: la fase del balanceo depende de cuanto tardo
    // en cargar el proceso, y eso metia un 20 % de diferencia entre dos capturas identicas de
    // Vulkan. `HARUKA_PROP_TIME=<segundos>` lo fija para capturas comparables.
    static const auto s_t0 = std::chrono::steady_clock::now();
    static const float s_fixed = [] { const char* e = std::getenv("HARUKA_PROP_TIME"); return e ? (float)std::atof(e) : -1.0f; }();
    const float propTime = (s_fixed >= 0.0f) ? s_fixed
                         : std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
    // La hierba densa se mece con EL MISMO viento y el mismo reloj que los arboles.
    if (Haruka::Planet::TerrestrialPlanet* tp = f.planets ? f.planets->activeTerrestrialMut() : nullptr)
        tp->setGrassWind(windWorld, propTime);

    if (!m_instancing) { m_instancing = std::make_unique<Renderer::GPUInstancing>(); m_instancing->init(20000); }
    ctx->bindPipeline(m_instPSO);
    ctx->bindUniformBuffer(0, f.perFrameUBO);

    // Frustum culling con esfera contra los 4 planos laterales + CULL POR TAMAÑO EN PANTALLA
    // (sub-pixel) + LOD por RADIO EN PIXELES (no en metros: un prop grande conserva detalle mas
    // lejos, sin tablas por tipo, y sale de la misma division que el descarte sub-pixel). Los
    // umbrales los pone el controlador de carga (abajo): historicos 90 px -> nivel 0 (464 tris),
    // 25 px -> nivel 1 (130), resto -> nivel 2 (40), pero con holgura de GPU suben a nivel 0 todo
    // lo que no sea sub-pixel.
    const glm::vec3 camF = f.camera->getFront();
    const glm::vec3 camU = f.camera->getUp();
    const glm::vec3 camR = glm::normalize(glm::cross(camF, camU));
    const float tanV = std::tan(glm::radians(f.camera->zoom * 0.5f));
    const float tanH = tanV * std::max(f.aspect, 0.01f);
    const float vpPx = std::max((float)f.heightPx, 1.0f);

    // ── LOD POR CARGA (no por distancia) ───────────────────────────────────────────────────────
    // En vez de tabla fija, el detalle lo decide la CARGA del pase: con holgura todo lo que no sea
    // sub-pixel se dibuja a nivel 0 (la distancia NO limita); si el pase se pasa del presupuesto,
    // los umbrales en px suben y pierde nivel PRIMERO lo mas pequeno en pantalla (= lo mas lejano),
    // que es lo que menos se nota. La distancia SOLO ordena el gasto; cuantos ms se gastan lo pone
    // este controlador sobre el scope del frame anterior.
    //
    // Senal: ms del scope `scene.prop.instanced` (solo Vulkan: `RHI::gpuScopes()` trae la medicion
    // resuelta del frame ANTERIOR; GL no tiene timestamps y vuelve vacia → nos quedamos en fijo).
    // Control: EMA (~1 s) + paso cada 0.5 s + zona muerta, para que no patalée (el popping global
    // del que huimos). Prioridad: SOBRE presupuesto → baja el detalle (pierde nivel primero lo mas
    // pequeno en pantalla = lo mas lejano; el tramo cercano esta protegido) y el radio solo se
    // recorta como ultimo recurso; HOLGURA → sube el detalle hasta 1.0 y, ya a tope, DIBUJA MAS:
    // extiende el radio (re-siembra con celdas mas gruesas, recuento log). Nunca se pasa del
    // presupuesto: es la definicion de "no saturar la GPU".
    // `HARUKA_PROP_LOAD=0` lo apaga (comportamiento historico) · `HARUKA_PROP_BUDGET_MS=<n>`
    // presupuesto del pase · `HARUKA_PROP_BUDGET_KI=<n>` paso por unidad de error ·
    // `HARUKA_PROP_DETAIL=<0..1000>` fuerza el detalle (A/B; 1000 = todo nivel 0) ·
    // `HARUKA_PROP_DETAIL_MIN=<permil>` suelo del detalle por debajo del historico (def. -1000):
    // cuando ni 90/25 px alcanza el presupuesto, baja mas (umbrales mas altos) hasta llegar ·
    // `HARUKA_PROP_RING=<m>` techo del radio (<=6000 = bloqueado al piso historico).
    static const bool   s_loadEnv    = [] { const char* e = std::getenv("HARUKA_PROP_LOAD");      return !(e && e[0] == '0'); }();
    static const double s_budgetMs   = [] { const char* e = std::getenv("HARUKA_PROP_BUDGET_MS"); return e ? std::atof(e) : kPropBudgetMs; }();
    static const double s_loadKi     = [] { const char* e = std::getenv("HARUKA_PROP_BUDGET_KI"); return e ? std::atof(e) : kLoadKi; }();
    static const int    s_loadDetail = [] { const char* e = std::getenv("HARUKA_PROP_DETAIL");    return e ? std::atoi(e) : -1; }();
    static const float  s_detailMin  = [] { const char* e = std::getenv("HARUKA_PROP_DETAIL_MIN"); return e ? (float)std::atoi(e) / 1000.0f : kDetailMin; }();
    static const float  s_ringMax    = [] { const char* e = std::getenv("HARUKA_PROP_RING");      return e ? std::max(kRingFloorM, (float)std::atoi(e)) : kRingMaxM; }();
    static double       s_lastRingOp = -1e18;   // reloj diag del ultimo movimiento de radio
    if (s_loadEnv) {
        double propMs = 0.0;
        if (RHI::Device* dev = RHI::device())
            for (const auto& s : dev->gpuScopes())
                if (s.name == kPropScope) { propMs = s.ms; break; }
        if (propMs > 0.0) {
            m_loadDriven = true;
            // semilla: el primer muestreo ES el estado, y el detalle arranca en EQUILIBRIO
            // (budget/lectura) — no desde 1.0. "Todo a nivel 0" el primer frame cargado-de-GPU
            // seria un pico de triangulos de entrada que el controlador luego pagaría en popping,
            // justo lo que intentamos evitar con la rampa lenta.
            if (m_propMsSmooth <= 0.0) {
                m_propMsSmooth = propMs;
                m_propDetail   = (float)std::clamp(s_budgetMs / std::max(propMs, 1e-3), (double)s_detailMin, 1.0);
            } else {
                m_propMsSmooth += (propMs - m_propMsSmooth) * 0.05;          // tau≈1 s a 60 fps
            }
            if (f.diagClock - m_lastLoadStep >= kLoadStepS) {
                m_lastLoadStep = f.diagClock;
                const double err = (m_propMsSmooth - s_budgetMs) / s_budgetMs;   // >0 = sobre presupuesto
                if (err > 0.06) {
                    // SOBRE presupuesto → bajar el DETALLE: pierde nivel primero lo mas pequeno en
                    // pantalla (= lo mas lejano) y lo cercano esta protegido por la garantia
                    // LOD0. Es el mango de la GPU: el radio NO se recorta aqui — los props lejanos
                    // ya son sub-pixel (no se dibujan) y una re-siembra de recorte añadiria un
                    // refundido justo en el frame saturado. Solo como ULTIMO recurso (detalle ya en
                    // el suelo y aun sobre) se recorta el radio, lento, para aliviar el barrido.
                    m_propDetail = std::clamp(m_propDetail - (float)(s_loadKi * err), s_detailMin, 1.0f);
                    if (m_propDetail <= s_detailMin && m_ringM > kRingFloorM &&
                        f.diagClock - s_lastRingOp >= kRingPaceS) {
                        s_lastRingOp = f.diagClock;
                        m_rescatter  = true;
                        m_ringM      = std::max(kRingFloorM, m_ringM - kRingStepM);
                    }
                } else if (err < -0.06) {
                    // HOLGURA → sube el detalle hasta el maximo.
                    m_propDetail = std::clamp(m_propDetail - (float)(s_loadKi * err), s_detailMin, 1.0f);
                }
                // ── RADIO: DESACOPLADO DEL DETALLE ──
                // Las bandas lejanas son casi exclusivamente sub-pixel (el cull por tamaño ni las
                // dibuja) o LOD2 de 40 tris: coste GPU ~nulo. Que la GPU este saturada por el tramo
                // CERCANO (y el detalle bajando) NO debe negar "dibuja mas de lejos": el radio se
                // estira hasta su techo a su propio ritmo — el coste real del radio es CPU (barrido
                // O(N) + arena), y lo limite la cota `maxProps` del scatter. Solo se recorta como
                // ultimo recurso (arriba), cuando ni en el suelo de detalle se llega al presupuesto.
                if (m_ringM < s_ringMax && f.diagClock - s_lastRingOp >= kRingPaceS) {
                    s_lastRingOp = f.diagClock;
                    m_rescatter  = true;
                    m_ringM      = std::min(s_ringMax, m_ringM + kRingStepM);
                }
            }
        } else {
            m_loadDriven = false;   // sin medicion (GL): umbrales fijos de siempre
        }
    }
    if (s_loadDetail >= 0) m_propDetail = (float)s_loadDetail / 1000.0f;
    const float kLodPx0 = m_loadDriven ? (1.0f - m_propDetail) * 90.0f : 90.0f;
    const float kLodPx1 = m_loadDriven ? (1.0f - m_propDetail) * 25.0f : 25.0f;
    // /!\\ con detalle 1.0 los umbrales son 0 px: kLod0 = kPixPerM/0 = +inf y todo lo que paso el
    // cull sub-pixel (>1 px) cumple dist^2 <= radio^2 * inf → nivel 0. Ahi no hay division por cero
    // en el cull (el sub-pixel ya se descarto antes); solo flota a inf, que es exactamente el LOD0.
    static double s_lastLoadLog = -1e18;
    if (f.diagClock - s_lastLoadLog >= 2.0) {
        s_lastLoadLog = f.diagClock;
        HARUKA_LOGDIAG("PropLoad", "ms=%.2f (budget %.1f) detalle=%.2f umbral-px=%d/%d anillo=%dm%s",
                       m_propMsSmooth, s_budgetMs, m_loadDriven ? (double)m_propDetail : -1.0,
                       (int)std::lround(kLodPx0), (int)std::lround(kLodPx1),
                       (int)std::lround(m_ringM), m_loadDriven ? "" : " [fijo: sin timestamps]");
    }
    // Culling en ESPACIO AL CUADRADO: px = (radio/dist)/tanV*vpPx, y las comparaciones
    // (sub-pixel (1 px) y LOD) se reescriben como dist^2 comparado con (radio * k)^2. Con
    // 178k instancias en el registro, el `sqrt` del length y la division del px por instancia
    // eran el grueso del barrido (~22 ms en el frame tras la re-siembra).
    const float kPixPerM   = vpPx / tanV;                  // px = radio * kPixPerM / dist
    const float kPixPerM2  = kPixPerM * kPixPerM;          // sub-pixel: dist^2 > radio^2 * kPixPerM2
    const float kNearM2    = kNearM * kNearM;              // prioridad cercana (LOD0 garantizado)
    const float kLod02     = kPixPerM / kLodPx0;           // LOD0: dist^2 <= radio^2 * kLod02^2
    const float kLod12     = kPixPerM / kLodPx1;           // LOD1: dist^2 <= radio^2 * kLod12^2
    const float kLod02Sq   = kLod02 * kLod02;
    const float kLod12Sq   = kLod12 * kLod12;
    // `HARUKA_PROP_LOD=0..2` fuerza el nivel para TODOS los props (A/B de un fallo por nivel).
    static const int s_forceLod = [] { const char* e = std::getenv("HARUKA_PROP_LOD"); return e ? std::atoi(e) : -1; }();
    auto propCull = [&](const glm::vec3& posF, float radius, int& outLod) -> uint8_t {
        outLod = 0;
        const float d2 = glm::dot(posF, posF);
        if (d2 <= 1e-6f) return 0;
        const float fwd = glm::dot(posF, camF);
        if (fwd < -radius) return 1;                      // detras de la camara
        if (std::abs(glm::dot(posF, camR)) > fwd * tanH + radius) return 1;
        if (std::abs(glm::dot(posF, camU)) > fwd * tanV + radius) return 1;
        const float r2 = radius * radius;
        if (d2 > r2 * kPixPerM2) return 2;                // sub-pixel (px < kMinPropPixels)
        // PRIORIDAD CERCANA: dentro de `kNearM` se dibuja SIEMPRE a nivel 0, haga lo que haga el
        // controlador de carga. Son pocos y baratos, y el player los ve de cerca: "lo mas cercano
        // al 100 %" no se negocia ni cuando el pase va sobre presupuesto.
        if (d2 <= kNearM2) { outLod = 0; return 0; }
        outLod = (d2 <= r2 * kLod02Sq) ? 0 : (d2 <= r2 * kLod12Sq ? 1 : 2);
        if (s_forceLod >= 0) outLod = std::min(s_forceLod, PrototypeGpu::kLods - 1);
        return 0;
    };

    const glm::dvec3 camD = glm::dvec3(f.camera->position);
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!(f.planets && f.planets->getActivePlanet(planetC, planetR))) planetC = glm::dvec3(0.0);
    const int protoCount = m_registry.prototypeCount();
    const int kLods = PrototypeGpu::kLods;
    // Radio de cull por PROTOTIPO (a escala 1): constante por nombre (`propCullRadiusM`), así que es
    // reproducible entre jugadores — la roca aguanta hasta el borde del anillo, el cúmulo usa su
    // huella, y el árbol se queda en su ~1 px natural. Un barrido antes del bucle: protoCount ≈ decenas.
    std::vector<float> cullRadius((size_t)protoCount, 8.0f);
    for (int pi = 0; pi < protoCount; ++pi)
        cullRadius[(size_t)pi] = Haruka::Planet::propCullRadiusM(m_registry.prototype(pi).name);

    // Buckets por (prototipo, nivel), reusados entre frames: UN barrido O(N) del registro (antes
    // era O(prototipos × instancias) y era el grueso de los 39 ms del pase).
    if ((int)m_buckets.size() != protoCount * kLods) {
        m_buckets.resize((size_t)(protoCount * kLods));
        m_aliveCounts.resize((size_t)protoCount);
        m_drawnCounts.resize((size_t)protoCount);
    }
    for (int pi = 0; pi < protoCount; ++pi) {
        for (int l = 0; l < kLods; ++l) m_buckets[(size_t)(pi * kLods + l)].clear();
        m_aliveCounts[(size_t)pi] = 0;
        m_drawnCounts[(size_t)pi] = 0;
    }

    // Snapshot de debug para la jerarquia del editor, cada 10 frames (~6 Hz): 40k entradas por
    // frame costaban ~5 ms y es una vista de debug.
    static int s_dbgFrame = 0;
    const bool snapshotDbg = m_debugEnabled && (++s_dbgFrame % 10 == 1);
    if (snapshotDbg) {
        m_scatterDebug.clear();
        m_scatterDebug.reserve((size_t)protoCount);
        for (int pi = 0; pi < protoCount; ++pi) {
            PropPrototypeDebug dbg; dbg.name = m_registry.prototype(pi).name;
            m_scatterDebug.push_back(std::move(dbg));
        }
    }

    const int bufCap = m_instancing->getMaxInstances();
    std::vector<int> aliveSeenPerProto((size_t)protoCount, 0);
    if (m_gpu.size() != m_registry.instances().size()) rebuildGpu(planetC, planetR, f.camera);
    // El arena de instancias se reserva UNA vez por frame con el tope del frame: el pase principal y el
    // de sombras comparten este instancer y cada uno sub-asigna lo suyo, o sea 2× el anillo como
    // tope duro. Sin margen, la acumulacion cruzaba el tope tras cada re-siembra y `upload` recreaba
    // el arena (hasta 157..671 ms). El +64k extra es relleno para que el segundo pase no roce el borde.
    // TOPE DURO ADAPTATIVO: se fija al MÁXIMO HISTÓRICO del anillo, no al tamaño del frame actual.
    // El primer frame no vale (el registro aún está vacío y congelar 2×0+256k hacía que el arena se
    // llenara al llegar el scatter y RECORTAR los props: "faltan las rocas y los árboles"). Cada
    // re-siembra que agranda el anillo mueve el tope hacia arriba — los reallocs solo pasan en
    // frames de gen (gigantescos de por sí); en juego estable el tope queda fijo y `upload` no
    // vuelve a llamar a vkAllocateMemory (~110 ms c/u en este portátil híbrido).
    const size_t ringNow = m_registry.instances().size();
    if (ringNow > m_arenaHiRing) { m_arenaHiRing = ringNow; m_instancing->setHardCap(m_arenaHiRing * 2 + 262144); }
    m_instancing->reserve(m_registry.instances().size() * 2 + 65536);
    const glm::vec3 originRel = glm::vec3(m_origin - camD);

    // SONDA: cuanto cuesta el barrido (cullar + agrupar), medida cada 2 s.
    using PClock = std::chrono::steady_clock;
    const auto tSweep0 = PClock::now();
    size_t nSwept = 0, nKept = 0;
    const auto& insts = m_registry.instances();
    const size_t nInst = insts.size();
    for (uint32_t ii = 0; ii < (uint32_t)nInst; ++ii) {
        // Prefetch a 8 instancias de `m_gpu` y del registro: en el primer barrido tras el apply
        // las páginas están frías y la latencia de cada `model[3]`/`io` dominaba (~20 ms).
        if (ii + 8 < nInst) {
            __builtin_prefetch(&m_gpu[ii + 8], 0, 3);
            __builtin_prefetch(&insts[ii + 8], 0, 3);
        }
        const auto& io = insts[ii];
        ++nSwept;
        const int pi = io.prototype;
        if (pi < 0 || pi >= protoCount) continue;
        const bool alive = io.state == (uint32_t)InstancedObjectState::Alive;
        uint8_t cull = 0;
        if (alive) {
            ++m_aliveCounts[(size_t)pi];
            const glm::vec3 posF = glm::vec3(m_gpu[ii].model[3]) + originRel;
            int lod = 0;
            cull = propCull(posF, io.scale * cullRadius[(size_t)pi], lod);
            if (cull == 0) { m_buckets[(size_t)(pi * kLods + lod)].push_back(ii); ++nKept; }
        }
        if (snapshotDbg) {
            PropPrototypeDebug& dbg = m_scatterDebug[(size_t)pi];
            ++dbg.totalInstances;
            PropInstanceDebug idbg;
            idbg.seed = io.seed; idbg.state = io.state; idbg.cullReason = cull;
            idbg.culled   = alive && cull != 0;
            idbg.rendered = alive && cull == 0 && aliveSeenPerProto[(size_t)pi] < bufCap;
            if (idbg.rendered) ++dbg.renderedInstances;
            if (alive && cull == 0) ++aliveSeenPerProto[(size_t)pi];
            dbg.instances.push_back(idbg);
        }
    }
    {
        static double s_lastLog = -1e9;
        const double ms = std::chrono::duration<double, std::milli>(PClock::now() - tSweep0).count();
        if (f.diagClock - s_lastLog > 2.0) {
            s_lastLog = f.diagClock;
            HARUKA_LOGDIAG("PropCost", "barrido+cull+matrices: %.2f ms para %zu instancias -> %zu dibujadas (%.0f ns/instancia)",
                           ms, nSwept, nKept, nSwept ? ms * 1e6 / (double)nSwept : 0.0);
        }
    }

    for (int pi = 0; pi < protoCount; ++pi) {
        const InstancedPrototype& proto = m_registry.prototype(pi);
        if (proto.name.empty()) continue;
        const PrototypeGpu* pgp = prototypeGpu(proto);
        if (!pgp) continue;
        const PrototypeGpu& pg = *pgp;
        if (!valid(pg.vbo[0]) || pg.indexCount[0] == 0) continue;

        // Per-pixel REAL del prototipo: se marca ANTES del bucket vacio (un prototipo sin instancias
        // visibles este frame no deja de tener material per-pixel en la jerarquia del editor).
        if (snapshotDbg) m_scatterDebug[(size_t)pi].hasPerPixel = pg.mask != 0u;

        // Stats TOTALES (sin cull) con la malla de nivel 0: el numero contra el que comparar el LOD.
        const int aliveN = m_aliveCounts[(size_t)pi];
        stats.totalVertices  += (int)pg.vertexCount[0] * aliveN;
        stats.totalTriangles += (int)(pg.indexCount[0] / 3) * aliveN;
        ++stats.totalDrawCalls;

        PropParams pp{};
        pp.wind = windWorld; pp.time = propTime;
        pp.matPBR = glm::vec4(pg.metallicS, pg.roughnessS, pg.aoS, (float)pg.mask);
        pp.originRel = glm::vec4(originRel, 0.0f);
        pp.aerial    = f.aerial;

        // UN DRAW POR NIVEL: el instancing sigue intacto y lo que baja es la geometria por instancia.
        int drawnTotal = 0;
        for (int lod = 0; lod < kLods; ++lod) {
            auto& bucket = m_buckets[(size_t)(pi * kLods + lod)];
            if (bucket.empty()) continue;
            if (!valid(pg.vbo[lod]) || pg.indexCount[lod] == 0) continue;
            { HARUKA_PROFILE("prop.gather"); m_instancing->setInstancesGather(m_gpu.data(), bucket); }
            const int drawnN = m_instancing->getInstanceCount();
            drawnTotal += drawnN;
            stats.renderedVertices  += (int)pg.vertexCount[lod] * drawnN;
            stats.renderedTriangles += (int)(pg.indexCount[lod] / 3) * drawnN;

            ctx->bindPipeline(m_instPSO);   // re-bind (creacion perezosa de buffers)
            ctx->bindVertexBuffer(pg.vbo[lod], 0);
            ctx->bindIndexBuffer(pg.ebo[lod]);
            // ⚠️ SE ATAN LOS CINCO SLOTS SIEMPRE: en Vulkan un descriptor sin atar es basura (props
            // GRISES, visto en RenderDoc como `u_matMetallic` sin recurso). El relleno lo ignora el
            // shader por la mascara, asi que no cambia el resultado en GL.
            ctx->bindTexture(0, valid(pg.albedo)    ? pg.albedo    : fallbackTexture(FallbackTex::White));
            ctx->bindTexture(1, valid(pg.normal)    ? pg.normal    : fallbackTexture(FallbackTex::Normal));
            ctx->bindTexture(2, valid(pg.metallic)  ? pg.metallic  : fallbackTexture(FallbackTex::Metallic));
            ctx->bindTexture(3, valid(pg.roughness) ? pg.roughness : fallbackTexture(FallbackTex::White));
            ctx->bindTexture(4, valid(pg.ao)        ? pg.ao        : fallbackTexture(FallbackTex::White));
            const RHI::BufferHandle ppUbo = (pi < (int)m_paramsUBOs.size()) ? m_paramsUBOs[(size_t)pi] : RHI::BufferHandle{};
            if (valid(ppUbo)) {
                HARUKA_PROFILE("prop.ubo.update");
                dev->updateBuffer(ppUbo, 0, sizeof(pp), &pp);
                ctx->bindUniformBuffer(6, ppUbo);
            }
            { HARUKA_PROFILE("prop.upload+draw"); m_instancing->render(ctx, pg.indexCount[lod], 1); }
            ++stats.renderedDrawCalls;
        }
        m_drawnCounts[(size_t)pi] = drawnTotal;
    }

    // Diagnóstico no-diag (protegido por s_logProps cada ~2 s): tamaño REAL del registro y lotes
    // subidos. Despeja la duda del log del tope (¿el arena se queda corto o hay MÁS registro?).
    {
        static double s_lastLog2 = -1e9;
        if (f.diagClock - s_lastLog2 > 2.0) {
            s_lastLog2 = f.diagClock;
            size_t noVacios = 0;
            for (int li = 0; li < protoCount * kLods; ++li)
                if (!m_buckets[(size_t)li].empty()) ++noVacios;
            HARUKA_LOGI("PropInst",
                        "registro=%zu (arena hi-ring=%zu) · protoCount=%d · buckets no vacios=%zu · lotes subidos=%d · arena %zu/%zu",
                        m_registry.instances().size(), m_arenaHiRing, protoCount, noVacios,
                        m_instancing->ringSize(), m_instancing->arenaUsed(), m_instancing->arenaCap());
        }
    }

    // Diagnostico cada 120 frames: por prototipo vivos / en bucket / dibujados, y la instancia de
    // nivel 0 mas cercana (distingue "esta a 300 m" de "mide 2 cm" de "esta bajo tierra").
    {
        static int s_diagFrame = 0;
        if (++s_diagFrame % 120 == 1 && Haruka::diagLogs()) {
            std::string line; char tmp[96];
            for (int di = 0; di < protoCount; ++di) {
                std::snprintf(tmp, sizeof(tmp), " p%d:%d/[%d+%d+%d]/%d", di, m_aliveCounts[(size_t)di],
                              (int)m_buckets[(size_t)(di * kLods + 0)].size(), (int)m_buckets[(size_t)(di * kLods + 1)].size(),
                              (int)m_buckets[(size_t)(di * kLods + 2)].size(), m_drawnCounts[(size_t)di]);
                line += tmp;
            }
            HARUKA_LOGDIAG("PropDiag", "protoCount=%d instances=%zu%s", protoCount, m_registry.instances().size(), line.c_str());
            for (int di = 0; di < protoCount; ++di) {
                const auto& b0 = m_buckets[(size_t)(di * kLods + 0)];
                if (b0.empty()) continue;
                float best = 1e30f; glm::vec3 bp(0.0f); float bs = 0.0f;
                for (const uint32_t bi : b0) {
                    const glm::vec3 pp3 = glm::vec3(m_gpu[bi].model[3]) + originRel;
                    const float d = glm::length(pp3);
                    if (d < best) { best = d; bp = pp3; bs = m_gpu[bi].scale.x; }
                }
                auto itM = m_protoMesh.find(m_registry.prototype(di).name);
                HARUKA_LOGDIAG("PropDiag", "  %s: nivel0 mas cercano a %.1f m (cam-rel %.1f %.1f %.1f · escala %.2f) · vbo0 verts %u idx %u",
                    m_registry.prototype(di).name.c_str(), best, bp.x, bp.y, bp.z, bs,
                    itM != m_protoMesh.end() ? itM->second.vertexCount[0] : 0u,
                    itM != m_protoMesh.end() ? itM->second.indexCount[0] : 0u);
            }
        }
    }
    if (valid(f.scenePSO)) ctx->bindPipeline(f.scenePSO);   // restaura el PSO de escena (el cierre lo asume)
}

// ── sombras ───────────────────────────────────────────────────────────────────────────────────

void PropSystem::drawShadows(RHI::Context* ctx, const glm::mat4& lightSpace, Core::Camera* camera,
                             PlanetarySystem* planets) {
    if (!ctx || !camera || !m_instancing) return;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount <= 0 || m_registry.instances().empty()) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    // PSO de solo profundidad, con el MISMO layout de instancia que el pase de color.
    if (!valid(m_shadowPSO)) {
        const std::string vs = Shader::baseDir() + "shaders/prop_depth_inst.vert";
        const std::string fs = Shader::baseDir() + "shaders/depth_only.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)sizeof(PropVertex) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(PropVertex, pos), RHI::Format::RGB32F, 0 },
            // La parte hace falta TAMBIEN aqui: sin ella una rama arrancada seguiria proyectando.
            { 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 },
        };
        Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        // SIN back-face culling: un tronco de una sola capa dejaria de proyectar por la mitad.
        pd.cull         = RHI::CullMode::None;
        m_shadowPSO = dev->createPipeline(pd);
        HARUKA_LOGI("PropShadow", "pipeline de profundidad instanciada: %s",
                    valid(m_shadowPSO) ? "ok" : "FALLO (los props no proyectan sombra)");
    }
    if (!valid(m_shadowPSO)) return;

    glm::dvec3 planetC(0.0); double planetR = 0.0;
    if (!planets || !planets->getActivePlanet(planetC, planetR)) return;
    const glm::dvec3 camD = glm::dvec3(camera->position);
    if (m_gpu.size() != m_registry.instances().size()) rebuildGpu(planetC, planetR, camera);
    // Ídem del pase principal: el tope adaptativo se actualiza ANTES de cualquier reserva.
    const size_t ringNow = m_registry.instances().size();
    if (ringNow > m_arenaHiRing) { m_arenaHiRing = ringNow; m_instancing->setHardCap(m_arenaHiRing * 2 + 262144); }
    m_instancing->reserve(m_registry.instances().size() * 2 + 65536);
    const glm::vec3 originRel = glm::vec3(m_origin - camD);

    if (!valid(m_shadowUBO))
        m_shadowUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PropShadowUBO), nullptr, RHI::BufferMemory::Dynamic);
    PropShadowUBO su; su.lightSpace = lightSpace; su.originRel = glm::vec4(originRel, 0.0f);
    dev->updateBuffer(m_shadowUBO, 0, sizeof(su), &su);

    // Caja de sombra ±42 m con margen 1,6×: un prop justo fuera puede proyectar dentro.
    const float kShadowBoxM = 42.0f * 1.6f;
    const float kBox2 = kShadowBoxM * kShadowBoxM;

    ctx->bindPipeline(m_shadowPSO);
    ctx->bindUniformBuffer(0, m_shadowUBO);

    m_shadowBuckets.resize((size_t)protoCount);
    for (auto& b : m_shadowBuckets) b.clear();
    const auto& insts = m_registry.instances();
    const size_t nInstS = insts.size();
    for (uint32_t ii = 0; ii < (uint32_t)nInstS; ++ii) {
        if (ii + 8 < nInstS) {
            __builtin_prefetch(&m_gpu[ii + 8], 0, 3);
            __builtin_prefetch(&insts[ii + 8], 0, 3);
        }
        const auto& io = insts[ii];
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;
        const glm::vec3 posF = glm::vec3(m_gpu[ii].model[3]) + originRel;
        if (glm::dot(posF, posF) > kBox2) continue;      // fuera de la caja de la luz
        m_shadowBuckets[(size_t)io.prototype].push_back(ii);
    }
    for (int pi = 0; pi < protoCount; ++pi) {
        const InstancedPrototype& proto = m_registry.prototype(pi);
        if (proto.name.empty()) continue;
        auto itMesh = m_protoMesh.find(proto.name);
        if (itMesh == m_protoMesh.end()) continue;      // malla aun sin hornear
        const PrototypeGpu& pg = itMesh->second;
        // LAS SOMBRAS USAN UN NIVEL FIJO, el intermedio: solo aporta la silueta y todo esta cerca.
        const int kShadowLod = 1;
        const int sl = (pg.indexCount[kShadowLod] > 0 && valid(pg.vbo[kShadowLod])) ? kShadowLod : 0;
        if (pg.indexCount[sl] == 0 || !valid(pg.vbo[sl]) || !valid(pg.ebo[sl])) continue;
        const auto& bucket = m_shadowBuckets[(size_t)pi];
        if (bucket.empty()) continue;
        m_instancing->setInstancesGather(m_gpu.data(), bucket);
        ctx->bindVertexBuffer(pg.vbo[sl], 0);
        ctx->bindIndexBuffer(pg.ebo[sl]);
        m_instancing->render(ctx, pg.indexCount[sl], 1);
    }
}

// ── estado y rotura ───────────────────────────────────────────────────────────────────────────

std::vector<PropSystem::StateDelta> PropSystem::serializeState() const {
    std::vector<StateDelta> out;
    out.reserve(m_state.size());
    for (const auto& [seed, st] : m_state) out.push_back(st);
    return out;
}

void PropSystem::restoreState(const std::vector<StateDelta>& deltas) {
    m_state.clear();
    for (const auto& d : deltas) m_state[d.seed] = d;
    m_lastCam = { 1e300, 1e300, 1e300 };   // fuerza un re-scatter con el estado de esta partida
}

PropSystem::Hit PropSystem::breakAt(const glm::dvec3& center, double radius, Core::Camera* camera,
                                    PlanetarySystem* planets, Physics::PhysicsEngine* physics) {
    Hit best;
    if (!planets) return best;
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!planets->getActivePlanet(planetC, planetR)) return best;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount == 0) return best;

    // Distancia de un punto a una caja orientada: la misma prueba que hace la fisica, no una
    // esfera envolvente (un tronco es mucho mas alto que ancho).
    auto distToOBB = [](const Haruka::Planet::PropWorldOBB& o, const glm::dvec3& p) {
        const glm::dvec3 d = p - o.center;
        glm::dvec3 l(glm::dot(d, glm::dvec3(o.rot[0])), glm::dot(d, glm::dvec3(o.rot[1])), glm::dot(d, glm::dvec3(o.rot[2])));
        const glm::dvec3 c = glm::clamp(l, -o.halfExtents, o.halfExtents);
        return glm::length(l - c);
    };
    std::vector<std::vector<Haruka::Planet::PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi)
        protoParts[(size_t)pi] = Haruka::Planet::propColliderParts(m_registry.prototype(pi).name, m_registry.prototype(pi).meshSeed);

    // El punto MAS CERCANO, no el primero que toque: rama y tronco se solapan en la axila.
    double bestD = radius;
    size_t bestIdx = (size_t)-1;
    const auto& insts = m_registry.instances();
    for (size_t ii = 0; ii < insts.size(); ++ii) {
        const auto& io = insts[ii];
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const double reach = radius + 12.0 * (double)io.scale;   // alto maximo razonable de un prop
        if (glm::length(wp - center) > reach) continue;
        const auto boxes = Haruka::Planet::propWorldColliders(protoParts[(size_t)io.prototype], io.dir, io.heightM,
                                                              io.scale, io.yaw, planetC, planetR, io.breakMask);
        for (const auto& b : boxes) {
            if (!b.breakable) continue;
            const double d = distToOBB(b, center);
            if (d >= bestD) continue;
            bestD = d; bestIdx = ii;
            best.hit = true; best.seed = io.seed;
            best.prototype = m_registry.prototype(io.prototype).name;
            best.partId = b.partId; best.trunk = (b.partId == 0);
            best.lengthM = b.length; best.rBottomM = b.rBottom; best.rTopM = b.rTop;
            best.pos = b.center;
        }
    }
    if (!best.hit || bestIdx == (size_t)-1) return best;

    // El tronco se lleva el arbol entero; una rama solo pone su bit y el arbol sigue en pie.
    if (best.trunk) {
        m_registry.setStateBySeed(best.seed, InstancedObjectState::Destroyed);
    } else if (best.partId >= 0 && best.partId < 32) {
        m_registry.setBreakBitBySeed(best.seed, (uint32_t)best.partId);
        if (bestIdx < m_gpu.size()) m_gpu[bestIdx].breakMask = (float)m_registry.instances()[bestIdx].breakMask;
    }
    StateDelta& st = m_state[best.seed];
    st.seed = best.seed;
    if (best.trunk) st.state = (uint32_t)InstancedObjectState::Destroyed;
    else if (best.partId >= 0 && best.partId < 32) st.breakMask |= (1u << (uint32_t)best.partId);

    // Los colliders se rehacen ya: si no, seguirias chocando con la rama que acabas de arrancar.
    refreshColliders(planetC, planetR, camera ? glm::dvec3(camera->position) : glm::dvec3(0.0), physics);
    return best;
}

// ── colliders ─────────────────────────────────────────────────────────────────────────────────

// MALLAS DE COLISION DE LOS PROTOTIPOS: la geometria REAL del prop, partida por parte, registrada
// UNA vez por (prototipo, parte). Cada parte es un cuerpo aparte, y la rota simplemente no se
// coloca. La COPA no genera colision (material 1 = follaje): se filtra por material, no por parte.
const std::vector<int>& PropSystem::meshShapesFor(int protoIdx, Physics::PhysicsEngine* phys) {
    auto it = m_meshShapes.find(protoIdx);
    if (it != m_meshShapes.end()) return it->second;
    std::vector<int>& ids = m_meshShapes[protoIdx];
#ifdef HARUKA_MOD_PHYSICS
    if (!phys || protoIdx < 0 || protoIdx >= m_registry.prototypeCount()) return ids;
    const auto& proto = m_registry.prototype(protoIdx);
    // A DETALLE MAXIMO: la colision no depende del LOD.
    const Haruka::Tools::ProcGraph::TreeMeshData tm = bakePrototypeMesh(proto, 1.0f);
    if (tm.positions.empty() || tm.indices.empty()) return ids;

    int maxPart = 0;
    for (unsigned char p : tm.partId) maxPart = std::max(maxPart, (int)p);
    const int nParts = tm.partId.empty() ? 1 : (maxPart + 1);
    ids.assign((size_t)nParts, -1);

    std::vector<float> verts; std::vector<uint32_t> idx;
    for (int part = 0; part < nParts; ++part) {
        verts.clear(); idx.clear();
        std::vector<int> remap(tm.positions.size(), -1);
        for (size_t t = 0; t + 2 < tm.indices.size(); t += 3) {
            const unsigned i0 = tm.indices[t], i1 = tm.indices[t + 1], i2 = tm.indices[t + 2];
            if (!tm.partId.empty() && (int)tm.partId[i0] != part) continue;
            if (!tm.materialId.empty() && tm.materialId[i0] != 0) continue;   // solo corteza
            for (unsigned vi : { i0, i1, i2 }) {
                if (remap[vi] < 0) {
                    remap[vi] = (int)(verts.size() / 3);
                    verts.push_back(tm.positions[vi].x); verts.push_back(tm.positions[vi].y); verts.push_back(tm.positions[vi].z);
                }
                idx.push_back((uint32_t)remap[vi]);
            }
        }
        if (idx.size() < 3) continue;
        ids[(size_t)part] = phys->registerPropMesh(verts.data(), verts.size() / 3, idx.data(), idx.size());
        // Copia en CPU para el alambre: los mismos triangulos, en el mismo marco local.
        auto& cpu = m_meshCpu[protoIdx];
        if ((int)cpu.size() <= part) cpu.resize((size_t)part + 1);
        cpu[(size_t)part].clear();
        cpu[(size_t)part].reserve(idx.size());
        for (uint32_t vi : idx) cpu[(size_t)part].push_back(glm::vec3(verts[vi * 3 + 0], verts[vi * 3 + 1], verts[vi * 3 + 2]));
    }
    HARUKA_LOGD("PropCollider", "malla de colision '%s': %d parte(s) registradas", proto.name.c_str(), (int)ids.size());
#else
    (void)phys; (void)protoIdx;
#endif
    return ids;
}

void PropSystem::refreshColliders(const glm::dvec3& planetC, double planetR, const glm::dvec3& camPos,
                                  Physics::PhysicsEngine* phys) {
#ifdef HARUKA_MOD_PHYSICS
    if (!phys) return;
    // ⚠️ RADIO ACOTADO: cada `addPropOBB` sube `m_staticsVersion` y Jolt recrea todos los cuerpos
    // estaticos. El radio tiene que superar la deriva entre refrescos del scatter (30 m) + margen.
    constexpr double kColliderRadiusM = 96.0;
    const double r2 = kColliderRadiusM * kColliderRadiusM;

    const int protoCount = m_registry.prototypeCount();
    std::vector<std::vector<Haruka::Planet::PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi) {
        const auto& proto = m_registry.prototype(pi);
        protoParts[(size_t)pi] = Haruka::Planet::propColliderParts(proto.name, proto.meshSeed);
    }

    const auto t0 = std::chrono::steady_clock::now();
    phys->clearPropOBBs();
    phys->clearPropCones();
    phys->clearPropMeshInstances();
    size_t nProps = 0, nBoxes = 0;
    std::vector<size_t> perProtoProps((size_t)protoCount, 0), perProtoBoxes((size_t)protoCount, 0);
    // `HARUKA_PROP_BOXES=1` vuelve a las primitivas (caja/cono) por si hace falta comparar.
    static const bool s_forcePrims = [] { const char* e = std::getenv("HARUKA_PROP_BOXES"); return e && e[0] == '1'; }();
    for (const auto& io : m_registry.instances()) {
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;   // tocon: no estorba
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const glm::dvec3 d  = wp - camPos;
        if (glm::dot(d, d) > r2) continue;
        ++nProps;
        const auto boxes = Haruka::Planet::propWorldColliders(protoParts[(size_t)io.prototype], io.dir, io.heightM,
                                                              io.scale, io.yaw, planetC, planetR, io.breakMask);
        perProtoProps[(size_t)io.prototype]++;
        for (const auto& b : boxes) {
            // LA MALLA REAL DEL PROP, no primitivas: con caja o cono queda holgura o se atraviesa.
            const std::vector<int>& shapeIds = meshShapesFor(io.prototype, phys);
            const int partIdx = (b.partId >= 0) ? b.partId : 0;
            const int shapeId = (!s_forcePrims && partIdx < (int)shapeIds.size()) ? shapeIds[(size_t)partIdx] : -1;
            if (shapeId >= 0) {
                // La malla esta en el marco local sin escalar y con la base en el origen; la base de
                // rotacion es la MISMA que usa el render (`propInstanceBasis`).
                phys->addPropMeshInstance(shapeId, wp, Haruka::Planet::propInstanceBasis(io.dir, io.yaw), (double)io.scale);
            } else if (b.isCone && !s_forcePrims) {
                phys->addPropCone(b.center, b.halfExtents.y, b.rTop, b.rBottom, b.rot);
            } else {
                phys->addPropOBB(b.center, b.halfExtents, b.rot);
            }
            ++nBoxes;
            perProtoBoxes[(size_t)io.prototype]++;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    static uint32_t s_refreshN = 0;
    ++s_refreshN;
    // Sonda de ESCALA (una vez): el tamaño del collider contra el de la malla dibujada.
    {
        static bool s_once = false;
        if (!s_once) {
            for (const auto& io : m_registry.instances()) {
                if (io.prototype < 0 || io.prototype >= protoCount) continue;
                if (m_registry.prototype(io.prototype).name.find("tree") == std::string::npos) continue;
                const auto& pp = protoParts[(size_t)io.prototype];
                if (pp.empty()) break;
                s_once = true;
                HARUKA_LOGD("PropCollider", "ESCALA: io.scale=%.3f · tronco malla r=%.3f h=%.3f -> collider r=%.3f halfH=%.3f (x%.2f)",
                            io.scale, pp[0].radiusA, pp[0].length, pp[0].radiusA * io.scale, pp[0].half.y * io.scale, io.scale);
                break;
            }
        }
    }
    std::string desglose;
    for (int pi = 0; pi < protoCount; ++pi)
        desglose += " · " + m_registry.prototype(pi).name + "=" + std::to_string(perProtoProps[(size_t)pi]) + "props/" +
                    std::to_string(perProtoBoxes[(size_t)pi]) + "cajas";
    HARUKA_LOGD("PropCollider", "#%u · %zu props -> %zu cajas (%.2f ms)%s", s_refreshN, nProps, nBoxes, ms, desglose.c_str());
#else
    (void)planetC; (void)planetR; (void)camPos; (void)phys;
#endif
}

} // namespace Haruka::World
