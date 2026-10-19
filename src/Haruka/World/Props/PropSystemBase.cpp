#include "Haruka/World/Props/PropSystemBase.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Jobs/JobSystem.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/World/WorldSystem.h"
#include "Haruka/World/Props/PropCollider.h"
#include "Haruka/World/Planet/PlanetarySystem.h"
#include "Haruka/World/Planet/Planet.h"
#include "Haruka/Physics/PhysicsEngine.h"
#include "Haruka/Renderer/FallbackTexture.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/Tools/ProcGraph/TreeMesh.h"
#include "Haruka/Tools/ProcGraph/PropMesh.h"
#include "Haruka/Tools/ProcGraph/ClumpMesh.h"
#include "Haruka/Tools/ProcGraph/TreeTextures.h"
#include "Haruka/Tools/ProcGraph/ProcTexture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <glm/gtc/packing.hpp>
#include <mutex>

namespace Haruka::World::Props {

using Haruka::RHI::valid;
using Renderer::FallbackTex;
using Renderer::fallbackTexture;

namespace {

// Bits de `matPBR.w`: que slots de textura estan REALMENTE atados (mismo esquema que el pase de
// escena y que prop_inst.frag). Potencias de 2 → exacto en float.
enum MaterialTexBit { kTexAlbedo = 1, kTexNormal = 2, kTexMetallic = 4, kTexRoughness = 8, kTexAO = 16 };

/// Color de vértice → RGBA8 SIN RECORTAR. Los tintes de los generadores (ClumpMesh: ×0,70…×1,25 sobre
/// el color base) pueden pasar de 1, y un RGBA8 normal los saturaría. La escala por vértice va en el
/// alfa (hasta 4,0); por debajo de 1 es 1 y el color se guarda tal cual, con la precisión entera.
static uint32_t packPropColor(glm::vec3 c) {
    c = glm::max(c, glm::vec3(0.0f));
    // Misma receta que `InstanceDataFloat::packTint`: la escala se cuantiza primero y el color se
    // divide por la escala YA cuantizada, o los dos errores se suman.
    const float want = glm::clamp(std::max({c.r, c.g, c.b, 1.0f}), 1.0f, 4.0f);
    const float a = std::ceil(want * 0.25f * 255.0f - 1e-4f) / 255.0f;
    return glm::packUnorm4x8(glm::vec4(glm::min(c / (a * 4.0f), glm::vec3(1.0f)), a));
}

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
    Tools::ProcGraph::Graph g;
    int h  = g.emplaceNode<Tools::ProcGraph::PerlinNode>((int)seed, 7.0f);
    int nr = g.emplaceNode<Tools::ProcGraph::MapRangeNode>(-1.0f, 1.0f, 0.0f, 1.0f);
    g.connect(h, 0, nr, 0);
    auto ramp = [&](float a, float b) -> int {
        auto n = std::make_unique<Tools::ProcGraph::ColorRampNode>();
        n->setStops({{0.0f, a}, {1.0f, b}});
        return g.addNode(std::move(n));
    };
    int rR = ramp(tintA.r, tintB.r);
    int rG = ramp(tintA.g, tintB.g);
    int rB = ramp(tintA.b, tintB.b);
    int combine = g.emplaceNode<Tools::ProcGraph::TreeCombineRGBNode>();
    g.connect(rR, 0, combine, 0);
    g.connect(rG, 0, combine, 1);
    g.connect(rB, 0, combine, 2);
    if (!g.compile()) return m;

    // Albedo PRE-GAMMA: la textura sube RGBA8 sin tag sRGB y prop_inst.frag linealiza con
    // `pow(tex, 2.2)`. Guardar c^(1/2.2) hace que el muestreo recupere el tono horneado exacto.
    int cmap[4] = {0, 1, 2, -1};
    Tools::ProcGraph::RGBAImage alb = Tools::ProcGraph::evaluateToRGBA(g, combine, 0, size, size, 0, 0, 1, cmap);
    const float invGamma = 1.0f / 2.2f;
    for (size_t i = 0; i < alb.pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const float v = alb.pixels[i + (size_t)c] / 255.0f;
            alb.pixels[i + (size_t)c] = (uint8_t)(std::pow(v, invGamma) * 255.0f);
        }
        alb.pixels[i + 3] = 255;
    }

    // Normal desde la altura (derivadas), AO y roughness por-pixel desde la misma altura.
    Tools::ProcGraph::RGBAImage nrm = Tools::ProcGraph::evaluateToNormalMap(g, nr, 0, size, size, 0, 0, 1, 1.5f);
    Tools::ProcGraph::RGBAImage ao(size, size);
    Tools::ProcGraph::RGBAImage rough(size, size);
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

    m.albedo    = Tools::ProcGraph::createRHIFromRGBA(alb);
    m.normal    = Tools::ProcGraph::createRHIFromRGBA(nrm);
    m.roughness = Tools::ProcGraph::createRHIFromRGBA(rough);
    m.ao        = Tools::ProcGraph::createRHIFromRGBA(ao);
    if (valid(m.albedo))    m.mask |= kTexAlbedo;
    if (valid(m.normal))    m.mask |= kTexNormal;
    if (valid(m.roughness)) m.mask |= kTexRoughness;
    if (valid(m.ao))        m.mask |= kTexAO;
    m.metallicS = 0.0f; m.roughnessS = rLo; m.aoS = 1.0f;
    return m;
}

// El campo del planeta visto por el scatter: la MISMA ecologia/cota por direccion que pinta el
// terreno (`TerrestrialPlanet`), redirigida por direccion.
struct PlanetSphereField : IPropSphereField {
    const Planet::TerrestrialPlanet* planet = nullptr;

    Planet::FieldSample sampleAt(const glm::vec3& dir) const override {
        return planet ? planet->fieldSampleAt(dir) : Planet::FieldSample{};
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
// arbol viven en `PropCollider.h`: el COLLIDER se deriva del mismo esqueleto que esta malla. Si el
// bake decidiera por su cuenta que es un arbol y con que altura, chocarias con otro arbol.

} // namespace

// ── mallas ──────────────────────────────────────────────────────────────────────────────────

Haruka::Tools::ProcGraph::TreeMeshData PropSystemBase::bakePrototypeMesh(const InstancedPrototype& proto, float detail) {
    Haruka::Tools::ProcGraph::TreeMeshData tm;
    // CÚMULO de lejanía: el prefijo `clump#` lo firma — bake APLANADO (la escala de instancia
    // estira la celda por el ancho; si fuera alto también parecería un rascacielos).
    if (isClumpProto(proto.name)) {
        const PropTreeParams tp = propTreeParams(proto.name);
        // La FORMA del prop base decide el tipo de masa (bosque/pedregal/aldea): el prefijo `clump#`
        // respeta el nombre, y `propShapeKind` lo clasifica por subcadena igual que su individuo.
        return Haruka::Tools::ProcGraph::bakeClumpMesh(
            proto.meshSeed, tp.style, tp.height, tp.trunkR, tp.canopy,
            Haruka::Tools::ProcGraph::kClumpFootprintM, detail,
            (int)propShapeKind(proto.name));
    }
    const PropShapeKind shape = propShapeKind(proto.name);
    if (shape == PropShapeKind::Rock) {
        tm = Haruka::Tools::ProcGraph::bakeRockMesh((int)proto.meshSeed, 1.0f, 0.72f);
    } else if (shape == PropShapeKind::House) {
        tm = Haruka::Tools::ProcGraph::bakeHouseMesh((int)proto.meshSeed, 1.0f);
    } else {
        const PropTreeParams tp = propTreeParams(proto.name);
        Haruka::Tools::ProcGraph::Graph g;
        const int tree = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
            (int)proto.meshSeed, tp.height, tp.trunkR, tp.canopy, tp.segments, detail, tp.style);
        g.compile();
        Haruka::Tools::ProcGraph::bakeTreeMesh(g, tree, tm);   // vacio si el nodo no es valido
    }
    return tm;
}

// ── vida ──────────────────────────────────────────────────────────────────────────────────────

PropSystemBase::~PropSystemBase() { PropSystemBase::shutdown(); }

void PropSystemBase::shutdown() {
    // El worker de re-siembra puede estar leyendo el campo del planeta mientras soltamos la GPU:
    // esperarle AQUÍ, antes de destruir nada.
    if (m_scatterBusy && m_scatterFut.valid()) {
        try { m_scatterFut.get(); }
        catch (const std::exception& e) { HARUKA_LOGE("PropSystemBase", "shutdown: re-siembra abortada: %s", e.what()); }
        m_scatterBusy = false;
        m_scatterFut  = {};
    }
    if (RHI::Device* dev = RHI::device()) {
        for (auto& [name, pg] : m_protoMesh) {
            for (int l = 0; l < PrototypeGpu::kLods; ++l) {
                if (valid(pg.vbo[l])) dev->destroy(pg.vbo[l]);
                if (valid(pg.ebo[l])) dev->destroy(pg.ebo[l]);
            }
            for (RHI::TextureHandle* t : { &pg.albedo, &pg.normal, &pg.metallic, &pg.roughness, &pg.ao })
                if (valid(*t)) dev->destroy(*t);
        }
        for (auto& b : m_paramsUBOs) if (valid(b)) dev->destroy(b);
    }
    m_protoMesh.clear();
    m_paramsUBOs.clear();
}

bool PropSystemBase::propsDisabled() {
    static const bool s = [] { const char* e = std::getenv("HARUKA_NOPROPS"); return e && e[0] == '1'; }();
    return s;
}

// ── prototipos ────────────────────────────────────────────────────────────────────────────────

const PrototypeGpu* PropSystemBase::prototypeGpu(const InstancedPrototype& proto) {
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
            pv.normal = glm::packSnorm3x10_1x2(glm::vec4(
                (vi < tm.normals.size()) ? tm.normals[vi] : glm::vec3(0, 1, 0), 0.0f));
            pv.color  = packPropColor((vi < tm.colors.size()) ? tm.colors[vi] : glm::vec3(1.0f));
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

void PropSystemBase::bindPrototypeMaterial(RHI::Context* ctx, const PrototypeGpu& pg) {
    // ⚠️ SE ATAN LOS CINCO SLOTS SIEMPRE: en Vulkan un descriptor sin atar es basura (props GRISES,
    // visto en RenderDoc como `u_matMetallic` sin recurso). El relleno lo ignora el shader por la
    // mascara, asi que no cambia el resultado en GL.
    ctx->bindTexture(0, valid(pg.albedo)    ? pg.albedo    : fallbackTexture(FallbackTex::White));
    ctx->bindTexture(1, valid(pg.normal)    ? pg.normal    : fallbackTexture(FallbackTex::Normal));
    ctx->bindTexture(2, valid(pg.metallic)  ? pg.metallic  : fallbackTexture(FallbackTex::Metallic));
    ctx->bindTexture(3, valid(pg.roughness) ? pg.roughness : fallbackTexture(FallbackTex::White));
    ctx->bindTexture(4, valid(pg.ao)        ? pg.ao        : fallbackTexture(FallbackTex::White));
}

RHI::BufferHandle PropSystemBase::paramsUBO(RHI::Device* dev, int proto) {
    if (proto < 0 || !dev) return {};
    if ((int)m_paramsUBOs.size() <= proto) m_paramsUBOs.resize((size_t)proto + 1);
    RHI::BufferHandle& b = m_paramsUBOs[(size_t)proto];
    if (!valid(b)) b = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PropParams), nullptr, RHI::BufferMemory::Dynamic);
    return b;
}

PropParams PropSystemBase::makeParams(const PrototypeGpu& pg, const glm::vec3& wind, float time,
                                      const glm::vec3& originRel, const DrawFrame& f) {
    PropParams pp{};
    pp.wind = wind; pp.time = time;
    pp.matPBR    = glm::vec4(pg.metallicS, pg.roughnessS, pg.aoS, (float)pg.mask);
    pp.originRel = glm::vec4(originRel, 0.0f);
    pp.material  = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);   // facetado, sin contraluz (vegetación, rocas, casas)
    pp.weather   = f.weather;
    const RHI::Device* dev = RHI::device();
    pp.flags     = glm::vec4(dev && dev->backend() == RHI::Backend::Vulkan ? 1.0f : 0.0f, 1.0f, 1.0f, 0.0f);
    return pp;
}

glm::vec3 PropSystemBase::frameWind(const DrawFrame& f) {
    return (f.world && f.camera) ? f.world->getWind(glm::dvec3(f.camera->position)) : glm::vec3(0.0f);
}

float PropSystemBase::frameTime() {
    // ⚠️ ESTE RELOJ ES DE PARED, no el de simulacion: la fase del balanceo depende de cuanto tardo
    // en cargar el proceso, y eso metia un 20 % de diferencia entre dos capturas identicas de
    // Vulkan. `HARUKA_PROP_TIME=<segundos>` lo fija para capturas comparables. Es UNO para todos
    // los sistemas: la hierba y el árbol de al lado se mecen con el mismo reloj.
    static const auto s_t0 = std::chrono::steady_clock::now();
    static const float s_fixed = [] { const char* e = std::getenv("HARUKA_PROP_TIME"); return e ? (float)std::atof(e) : -1.0f; }();
    return (s_fixed >= 0.0f) ? s_fixed
                             : std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
}

// ── cull ──────────────────────────────────────────────────────────────────────────────────────

PropSystemBase::Cull PropSystemBase::makeCull(const DrawFrame& f, float lodPx0, float lodPx1) {
    Cull c;
    c.camF = f.camera->getFront();
    c.camU = f.camera->getUp();
    c.camR = glm::normalize(glm::cross(c.camF, c.camU));
    c.tanV = std::tan(glm::radians(f.camera->zoom * 0.5f));
    c.tanH = c.tanV * std::max(f.aspect, 0.01f);
    const float vpPx = std::max((float)f.heightPx, 1.0f);
    // (sub-pixel (1 px) y LOD) se comparan como dist^2 contra (radio * k)^2: el `sqrt` y la division
    // por instancia eran el grueso del barrido.
    const float pixPerM = vpPx / c.tanV;                   // px = radio * pixPerM / dist
    c.pixPerM2 = pixPerM * pixPerM;                        // sub-pixel: dist^2 > radio^2 * pixPerM2
    c.near2    = kNearM * kNearM;                          // prioridad cercana (LOD0 garantizado)
    // /!\ con umbral 0 px, pixPerM/0 = +inf y todo lo que pasa el sub-pixel es nivel 0.
    const float l0 = pixPerM / lodPx0, l1 = pixPerM / lodPx1;
    c.lod02 = l0 * l0;
    c.lod12 = l1 * l1;
    // `HARUKA_PROP_LOD=0..2` fuerza el nivel para TODO (A/B de un fallo por nivel).
    static const int s_forceLod = [] { const char* e = std::getenv("HARUKA_PROP_LOD"); return e ? std::atoi(e) : -1; }();
    c.forceLod = s_forceLod;
    return c;
}

uint8_t PropSystemBase::Cull::operator()(const glm::vec3& posF, float radius, int& outLod) const {
    outLod = 0;
    const float d2 = glm::dot(posF, posF);
    if (d2 <= 1e-6f) return 0;
    const float fwd = glm::dot(posF, camF);
    if (fwd < -radius) return 1;                      // detras de la camara
    if (std::abs(glm::dot(posF, camR)) > fwd * tanH + radius) return 1;
    if (std::abs(glm::dot(posF, camU)) > fwd * tanV + radius) return 1;
    const float r2 = radius * radius;
    if (d2 > r2 * pixPerM2) return 2;                 // sub-pixel
    // PRIORIDAD CERCANA: dentro de `kNearM` se dibuja SIEMPRE a nivel 0, haga lo que haga el
    // controlador de carga. Son pocos y baratos, y se ven de cerca: "lo mas cercano al 100 %" no se
    // negocia ni cuando el pase va sobre presupuesto.
    if (d2 <= near2) { outLod = 0; return 0; }
    outLod = (d2 <= r2 * lod02) ? 0 : (d2 <= r2 * lod12 ? 1 : 2);
    if (forceLod >= 0) outLod = std::min(forceLod, PrototypeGpu::kLods - 1);
    return 0;
}

// ── filtro de superficie ──────────────────────────────────────────────────────────────────────

PropSystemBase::SurfaceFilter PropSystemBase::makeSurfaceFilter(const Planet::TerrestrialPlanet* planet) {
    SurfaceFilter sf;
    sf.planet = planet;
    // ⚠️ EL FILTRO DE BOCA ES ~50 MS SI SE HACE A CIEGAS. `surfaceCut` paga el muestreo de COTA
    // antes de mirar si hay chunk; sólo puede recortar donde el chunk tiene HUECO. Se colectan una
    // vez esas columnas y `surfaceCut` solo se llama en ellas (ver `VoxWorld::loadedCutColumns`).
    if (planet) sf.cutCols = planet->vox().loadedCutColumns();
    return sf;
}

bool PropSystemBase::SurfaceFilter::removes(const glm::vec3& dir) const {
    if (!planet) return false;
    if (!cutCols.empty()) {
        const Vox::VoxKey ck = planet->vox().columnAt(glm::dvec3(dir));
        if (cutCols.count(ck) != 0 && planet->vox().surfaceCut(glm::dvec3(dir)) > 0.5f) return true;
    }
    // CRÁTERES: una explosión despeja lo que había dentro (tierra desnuda), y lo que quedara estaría
    // clavado a la cota de antes, flotando sobre el cuenco.
    return planet->vox().surfaceDeltaM(glm::dvec3(dir)) < -0.05f;
}

// ── re-siembra ────────────────────────────────────────────────────────────────────────────────

void PropSystemBase::refreshScatter(Core::Camera* camera, Planet::PlanetarySystem* planets,
                                    Physics::PhysicsEngine* physics) {
    if (!m_enabled || propsDisabled()) {
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        onCleared();
        m_planet.clear();
        m_lastCam = {1e300, 1e300, 1e300};
        return;
    }
    if (!camera || !planets) return;
    const glm::dvec3 camPos = glm::dvec3(camera->position);

    glm::dvec3 planetC; double planetR = 0.0;
    if (!planets->getActivePlanet(planetC, planetR)) {
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        onCleared();
        m_planet.clear();
        return;
    }
    m_planetC = planetC; m_planetR = planetR;
    const Planet::TerrestrialPlanet* planet = planets->activeTerrestrial();
    const std::string pname = planets->getActivePlanetName();
    if (!planet) {                                   // planeta orbital sin superficie = nada que sembrar
        if (m_registry.prototypeCount() > 0) m_registry.reset();
        onCleared();
        m_planet.clear();
        m_lastCam = {1e300, 1e300, 1e300};
        return;
    }
    onRefresh(physics);
    if (pname != m_planet) {
        m_registry.reset();   // planeta nuevo: re-registrar prototipos
        m_planet = pname;
    }

    // ── RE-SIEMBRA ASINCRONA ────────────────────────────────────────────────────────────────
    // `scatterPropsNear` (~61k celdas × muestreo del campo) era el grueso de los tirones de 181 ms
    // al cruzar un tramo. Ahora corre en un hilo worker: SOLO lee el CAMPO (el bake de alturas y los
    // mapas son inmutables tras `bakeHeightMap`; "seguro desde el hilo async", PlanetarySystem.cpp:
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
                std::shared_ptr<ScatterOutput> res = m_scatterFut.get();
                m_scatterBusy = false;
                m_scatterFut  = {};
                // El resultado vive en el planeta con el que se lanzó: si el usuario cambió de
                // planeta durante el vuelo se descarta (el nuevo ya disparará su re-siembra).
                if (res && m_scatterPlanet == pname && pname == m_planet) {
                    ApplyContext ac{ planetC, planetR, camera, physics, planet };
                    applyScatter(std::move(res), ac);
                    m_voxVersion = planet->vox().version() + planet->vox().craterVersion();   // ACK del campo (y cráteres)
                    m_lastCam    = camPos;                    // ACK: RE-ANCLA el tramo a la cámara ACTUAL
                }
                // Velocidad fresca: si la cámara ya cruzó OTRO tramo durante el vuelo, relanzar ahora.
                const double flightS = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_lastT).count();
                const double distLaunch = glm::length(camPos - m_scatterCam);
                const double speedL     = flightS > 1e-3 ? distLaunch / flightS : 0.0;
                m_lastT = std::chrono::steady_clock::now();
                relaunchNow = (m_scatterPlanet == pname && pname == m_planet) &&
                              distLaunch > std::max(30.0, speedL * 1.5);
            } catch (const std::exception& e) {
                HARUKA_LOGE(systemName(), "re-siembra async fallo: %s", e.what());
                m_scatterBusy = false;
                m_scatterFut  = {};
            }
        }
        if (!relaunchNow) return;   // en vuelo (o recién aplicada): aguantar lo anterior
    }
    }   // re-siembra.aplica

    // Solo re-enumera cuando la camara cruza un tramo desde la ultima muestra. El scatter (~61k
    // celdas × muestreo del campo) es caro (~121 ms). El tramo es ADAPTATIVO a la velocidad: al
    // andar bastan 30 m; al volar, ~1,5 s de viaje. Las celdas son MUNDIALES y deterministas.
    // Y cuando el CAMPO cambia (una boca que carga, un trazo): los props que estaban sobre el
    // agujero tienen que irse, aunque no te hayas movido. SEGURO DESPUES del checkout del worker:
    // si está en vuelo aplicamos el resultado ANTES de decidir si hay que relanzar.
    if (!relaunchNow) {
        const auto nowT = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(nowT - m_lastT).count();
        const double distCam = glm::length(camPos - m_lastCam);
        const double speed   = elapsed > 1e-3 ? distCam / elapsed : 0.0;
        const double refreshM = std::max(30.0, speed * 1.5);
        const bool tooSoon = elapsed < 0.5;
        // + cráteres: uno nuevo tiene que despejar lo que haya dentro (las dos versiones sólo crecen).
        const bool campoCambio = planet->vox().version() + planet->vox().craterVersion() != m_voxVersion;
        if ((distCam < refreshM || tooSoon) && !campoCambio && !m_rescatter && !m_planet.empty())
            return;
    }
    m_rescatter = false;   // este intento cubre lo que el sistema pidiera

    { HARUKA_PROFILE("re-siembra.lanza");   // registro de prototipos + copias + lanzar worker
    PlanetSphereField field;
    field.planet = planet;
    const PropLayerTable& table = planet->propLayers();
    if (table.layers.empty()) return;   // sin capas declaradas = nada que sembrar

    // UN prototipo por NOMBRE (el mesh de la capa, o cada variante × cada semilla "conifer#2").
    // `meshSeed` = hash32 de planeta+nombre: un arbol es SIEMPRE el mismo arbol. Cualquier capa
    // registra TAMBIÉN sus CÚMULOS (`clump#<proto>#k`): las celdas gruesas de lejanía instalan una
    // masa por celda en vez de individuos (la malla del cúmulo se hornea bajo este mismo nombre,
    // por forma: bosque, pedregal, aldea o pradera). Es DERIVADO (shouldClumpFar), no un flag.
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
    // Solo las capas de ESTE sistema; el scatter salta el resto por índice (`layerOwned`).
    std::vector<uint8_t> owned(table.layers.size(), 0);
    for (size_t li = 0; li < table.layers.size(); ++li) {
        const PropLayer& L = table.layers[li];
        if (!ownsLayer(L)) continue;
        owned[li] = 1;
        for (const std::string& pn : L.prototypeNames()) {
            registerProto(pn);
            if (shouldClumpFar(pn))
                for (int k = 0; k < kClumpKinds; ++k)
                    registerProto(clumpProtoName(pn, k));
        }
    }
    if (m_registry.prototypeCount() == 0) return;

    PropScatterParams params;
    params.radius     = planetR;
    params.seed       = planet->config().seed ? planet->config().seed : 1u;
    params.layerOwned = std::move(owned);
    configureScatter(params);

    ScatterContext sc;
    sc.camPos = camPos; sc.planetC = planetC; sc.planetR = planetR; sc.planetSeed = params.seed;
    for (int i = 0; i < m_registry.prototypeCount(); ++i)
        sc.protoByName.emplace(m_registry.prototype(i).name, i);
    ScatterJob job = makeJob(sc);
    if (!job) return;

    m_scatterBusy   = true;
    m_scatterPlanet = pname;
    m_scatterCam    = camPos;
    m_lastT = std::chrono::steady_clock::now();   // el tramo adaptivo se mide desde ESTE lanzamiento
    const std::string who = systemName();
    m_scatterFut = Haruka::Core::Jobs::jobs().submit(Haruka::Core::Jobs::Priority::Streaming,
        [field, camPos, planetC, params, table, job, who]() -> std::shared_ptr<ScatterOutput> {
            // ⚠️ CAPAS MUERTAS. Cada celda SORTEA entre las capas que la aceptan, asi que una capa a
            // cero ya no puede ser "otra se la comio": sus condiciones no pasan en ninguna celda, o
            // ninguna de sus variantes vive ahi. El desglose (una vez POR SISTEMA: cada uno tiene
            // sus capas) dice cual de los factores.
            static std::unordered_set<std::string> s_reported;
            static std::mutex s_reportedMx;
            bool report;
            { std::lock_guard<std::mutex> lk(s_reportedMx); report = s_reported.count(who) == 0; }
            PropScatterStats stats;
            std::vector<ScatteredProp> placed =
                scatterPropsNear(field, camPos, planetC, params, table, report ? &stats : nullptr);
            auto mine = [&](size_t li) { return params.layerOwned.empty() || (li < params.layerOwned.size() && params.layerOwned[li]); };
            if (report && !placed.empty()) {
                { std::lock_guard<std::mutex> lk(s_reportedMx); s_reported.insert(who); }
                for (size_t li = 0; li < table.layers.size(); ++li) {
                    const auto& L = table.layers[li];
                    if (L.mesh.empty() || stats.placed[li] > 0 || !mine(li)) continue;
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
                std::string line = who + ": biomas en el primer scatter (celdas · colocadas por capa):";
                for (int b = 0; b < (int)Planet::Biome::COUNT; ++b) {
                    if (stats.cellsByBiome[b] == 0) continue;
                    line += "\n   " + std::string(Planet::biomeKey((Planet::Biome)b))
                          + " " + std::to_string(stats.cellsByBiome[b]);
                    for (size_t li = 0; li < table.layers.size(); ++li) {
                        if (table.layers[li].mesh.empty() || !mine(li)) continue;
                        line += " · " + table.layers[li].mesh + "=" + std::to_string(
                            stats.placedByBiome[li * (size_t)Planet::Biome::COUNT + (size_t)b]);
                    }
                }
                HARUKA_LOGI("PropScatter", "%s", line.c_str());
            }

            return job(std::move(placed));
        });
    }   // re-siembra.lanza
}

} // namespace Haruka::World::Props
