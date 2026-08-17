// Application — render pipeline.
// The per-frame work: build the render queue, the deferred/forward object pass,
// terrain/islands/water passes, the standalone post-processing composite and the
// GPU timer. Lifecycle/orchestration lives in application.cpp; the GL asset
// caches in application_assets.cpp.

// glm/gtx/* (usado por glm::rotation del pase de props) exige la macro en GLM moderno.
#define GLM_ENABLE_EXPERIMENTAL

#include <chrono>
#include <cmath>
#include <cstdlib>   // getenv: HARUKA_COLLISION_WIRE
#include "application.h"
#include "application_internal.h"
#include "rhi/rhi_context.h"   // ruta PSO: comandos de dibujo del frame (bloom migrado)

#include <algorithm>

#include "core/sky_ambient.h"   // ambiente integrado del MISMO cielo que se dibuja
#include "core/logger.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>   // glm::mat4_cast (orientar el prototipo al suelo, pase de props)
#include <glm/gtx/quaternion.hpp>   // glm::rotation (eje→cuaternión: up del prototipo → dir radial)

#include "game/planetary_system.h"
#include "core/planet/terrain_lod.h"   // TERRAIN_COLLIDE_UNIFORM_M (radio del alambre de colision)
#include "core/components/mesh_renderer_component.h"
#include "core/components/material_component.h"
#include "renderer/model.h"
#include "renderer/simple_mesh.h"
#include "tools/object_types.h"
#include "tools/profiler.h"
#include "settings/settings_manager.h"
#include "io/image_writer.h"
#include "core/asset_paths.h"
#include "core/planet/prop_scatter.h"   // scatterPropsNear + IPropSphereField (scatter GLOBAL de props)
#include "core/planet/prop_collider.h"  // colliders por PARTE derivados del esqueleto del árbol
#include "tools/procgraph/tree_mesh.h"
#include "tools/procgraph/prop_mesh.h"
#include "tools/procgraph/tree_textures.h"   // TreeCombineRGBNode (bake de material per-pixel de props)
#include "tools/procgraph/proc_texture.h"    // evaluateToRGBA / evaluateToNormalMap / createRHIFromRGBA

#include <vector>
#include <string>
#include <unordered_map>
#include <utility>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace Haruka { namespace Core {

using AppInternal::g_sceneRenderQueue;

// std140-compatible structs mirroring the UBO declarations in the shaders.
// Any change to the GLSL UBO layout must be reflected here.
namespace {
struct alignas(16) PerFrameUBOData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 cameraPos;      float _pad0;
    glm::vec3 sunDirection;   float _pad1;
    glm::vec3 sunLightColor;  float ambientStrength;
    int enableHDR;
    int enableBloom;
    int enableSSAO;
    int enableIBL;
    int enableShadows;
    int _pad3[3];
    // Luz de luna (2ª luz, añadida al final → no mueve offsets previos). Los shaders
    // que no la usan pueden omitir estos campos de su bloque UBO.
    glm::vec3 moonDirection;  float moonIntensity;  // dir hacia la Luna + brillo (fase)
    glm::vec3 moonLightColor; float _pad4;          // color azulado tenue
};
static_assert(sizeof(PerFrameUBOData) == 240, "PerFrameUBOData std140 size mismatch");

struct alignas(16) PerObjectUBOData {
    glm::mat4 model;
    glm::vec4 baseColorAndPlanetRadius; // rgb=color, a=planetRadius
    glm::vec4 planetCenterAndFlag;      // xyz=planetCenter, w=useProceduralTerrain
    // MATERIAL del objeto (MaterialComponent). Va al FINAL a propósito: planet.frag y preview.*
    // declaran su propio bloque con los tres campos de arriba y siguen leyendo los mismos
    // offsets; solo los shaders que quieran material declaran el bloque completo.
    glm::vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas (bits)
    glm::vec4 materialEmission;         // rgb=emisión, a=libre
};
static_assert(sizeof(PerObjectUBOData) == 128, "PerObjectUBOData std140 size mismatch");

// Bits de `materialPBR.w`: qué slots de textura están REALMENTE atados este draw. El shader no
// puede preguntárselo a un sampler (uno sin atar lee negro, que es un valor válido), así que la
// presencia viaja explícita. Los valores son potencias de 2 → w es exacto en float hasta 2^24.
enum MaterialTexBit {
    kTexAlbedo    = 1,
    kTexNormal    = 2,
    kTexMetallic  = 4,
    kTexRoughness = 8,
    kTexAO        = 16,
};

// UBO del bloom (binding 2) — antes eran uniforms sueltos (glUniform1f/1i), que NO existen en
// Vulkan. Compartido por bloom_extract.frag y bloom_blur.frag (cada pase usa un campo). std140
// alinea el bloque a vec4 → padding explícito a 16 B para que C++ y GLSL coincidan.
struct BloomParams {
    float threshold;   // bright-pass: umbral de luminancia
    float horizontal;  // blur: 1 = horizontal, 0 = vertical
    float _pad0, _pad1;
};
static_assert(sizeof(BloomParams) == 16, "BloomParams std140 size mismatch");

// UBO del present/composite (binding 3) — antes uniforms sueltos (glUniform1i/2f/1f). Flags como
// FLOAT (no int/bool): el empaquetado de int/bool en std140 difiere entre GL y Vulkan. std140
// redondea el bloque a múltiplo de 16 → 5 floats ocupan 32 B, no 20: el padding es OBLIGATORIO
// o C++ y GLSL leen campos desalineados.
struct PresentParams {
    float texel[2];        // 0..8    1.0 / sceneResolution
    float bloomStrength;   // 8..12
    float fxaa;            // 12..16  >0.5 = on
    float bloom;           // 16..20  >0.5 = on
    float _pad[3];         // 20..32  (relleno std140)
};
static_assert(sizeof(PresentParams) == 32, "PresentParams std140 size mismatch");

// UBO del pase de PROPS instanciados (binding 6) — espejo de `PropParams` en prop_inst.vert/frag.
// vec3+float (16 B) + vec4 (16 B) → 32 B std140. Los escalares del material son del PROTOTIPO
// (compartido por todas sus instancias), por eso viven aquí y no por-instancia.
struct PropParams {
    glm::vec3 wind;   float time;       // viento del clima (mundo, m/s) · segundos
    glm::vec4 matPBR;                   // x=metallic y=roughness z=ao w=máscara de texturas (bits)
};
static_assert(sizeof(PropParams) == 32, "PropParams std140 size mismatch");

// Material PER-PIXEL del prototipo de props: el MISMO grafo de texturas del editor de node graph
// (perlin → altura → albedo por rampa + normal por derivadas + AO/roughness desde la altura),
// horneado a CPU y subido a GPU SIN pasar por disco. El albedo MODULA el color de vértice (que
// sigue llevando la identidad del material: corteza/copa, muro/tejado, tinte de roca), así el
// per-pixel no pelea con el arte horneado en la malla. Devuelve la máscara en `mask` (0 = falló).
struct PropMaterialBake {
    Haruka::RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
    float   metallicS  = 0.0f;
    float   roughnessS = 0.5f;
    float   aoS        = 1.0f;
    uint32_t mask      = 0u;
};

static PropMaterialBake bakePropPrototypeMaterial(uint32_t seed, int size = 128) {
    using namespace Haruka::Tools::ProcGraph;
    PropMaterialBake m;

    // Tono por defecto: cálido-vegetal neutro de bajo contraste (el color de vértice manda).
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
    const float rLo   = 0.55f, rHi = 0.90f;    // roughness: valles más lisos, crestas ásperas
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float hh = g.evaluate(nr, 0, (float)x, (float)y, 0).asFloat();
            const uint8_t av = (uint8_t)(glm::clamp(1.0f - hh * dark, 0.0f, 1.0f) * 255.0f);
            ao.setPixel(x, y, av, av, av);
            const uint8_t rv = (uint8_t)((rLo + (rHi - rLo) * hh) * 255.0f);
            rough.setPixel(x, y, rv, rv, rv);
        }
    }

    m.albedo   = createRHIFromRGBA(alb);
    m.normal   = createRHIFromRGBA(nrm);
    m.roughness = createRHIFromRGBA(rough);
    m.ao       = createRHIFromRGBA(ao);
    if (Haruka::RHI::valid(m.albedo))    m.mask |= kTexAlbedo;
    if (Haruka::RHI::valid(m.normal))    m.mask |= kTexNormal;
    if (Haruka::RHI::valid(m.roughness)) m.mask |= kTexRoughness;
    if (Haruka::RHI::valid(m.ao))        m.mask |= kTexAO;
    m.metallicS = 0.0f; m.roughnessS = rLo; m.aoS = 1.0f;
    return m;
}

// UBO del pase de CONSTRUCCIÓN (binding 6) — espejo de `ConstParams` en construction_inst.frag.
// Solo los escalares/máscara del material del GRUPO (el color por pieza va por instancia).
struct ConstParams {
    glm::vec4 matPBR;   // x=metallic y=roughness z=ao w=máscara de texturas (bits)
};
static_assert(sizeof(ConstParams) == 16, "ConstParams std140 size mismatch");

// Material PER-PIXEL de un grupo de piezas instanciadas (objetos NPC/construcción): los MISMOS
// slots que el pase de escena (MaterialComponent). mask == 0 → solo color por instancia.
struct ConstGroupMaterial {
    Haruka::RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
    float   metallicS  = 0.0f;
    float   roughnessS = 0.5f;
    float   aoS        = 1.0f;
    uint32_t mask      = 0u;
};

// Resuelve el material de un objeto instanciado → texturas cargadas + escalares + máscara.
// Sin MaterialComponent (o sin slots) → mascara 0 (el aspecto de siempre por color de instancia).
static ConstGroupMaterial resolveInstancedMaterial(const Haruka::MaterialComponent* mat) {
    ConstGroupMaterial m;
    if (!mat) return m;
    m.metallicS = mat->metallic; m.roughnessS = mat->roughness; m.aoS = mat->ao;
    auto bind = [&](const char* key, uint32_t unit, uint32_t bit, Haruka::RHI::TextureHandle& out) {
        (void)unit;
        auto it = mat->textures.find(key);
        if (it == mat->textures.end() || it->second.empty()) return;
        Haruka::RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
        if (!Haruka::RHI::valid(tex)) return;
        out = tex; m.mask |= bit;
    };
    bind("albedo",    0, kTexAlbedo,    m.albedo);
    bind("normal",    1, kTexNormal,    m.normal);
    bind("metallic",  2, kTexMetallic,  m.metallic);
    bind("roughness", 3, kTexRoughness, m.roughness);
    bind("ao",        4, kTexAO,        m.ao);
    return m;
}

// Clave de material para el agrupamiento: las rutas de texturas + los escalares. Dos piezas con la
// misma malla y la MISMA clave comparten un draw (mismo bind de texturas y misma máscara).
static std::string instancedMaterialKey(const Haruka::MaterialComponent* mat) {
    std::string k;
    if (!mat) return k;
    static const char* kSlots[] = {"albedo", "normal", "metallic", "roughness", "ao"};
    for (const char* s : kSlots) {
        auto it = mat->textures.find(s);
        if (it != mat->textures.end() && !it->second.empty()) { k += s; k += '='; k += it->second; k += ';'; }
    }
    k += "m"; { uint32_t bits; std::memcpy(&bits, &mat->metallic, 4); k += std::to_string(bits); }
    k += "r"; { uint32_t bits; std::memcpy(&bits, &mat->roughness, 4); k += std::to_string(bits); }
    k += "a"; { uint32_t bits; std::memcpy(&bits, &mat->ao, 4);        k += std::to_string(bits); }
    return k;
}

// Grupo de piezas instanciadas (NPC/construcción): MISMA malla + MISMO material → un draw con un
// bind de texturas. `meshKey` = modelPath, o "prim:N" para primitivas de edificio (cubos tejidos).
struct ConstInstGroup {
    std::string  meshKey;
    int          primitive = -1;
    ConstGroupMaterial mat;
    std::vector<Haruka::InstanceDataFloat> instances;
};

// Vértice del PROTOTIPO de props (binding 0): Pos(0)/Normal(1)/Color(2)/Uv(9). El `Vertex` de
// escena no tiene canal de color, así que el prototipo usa su propio layout (los shaders lo
// leen en esas locations; el Uv a 9 no choca con el stream de instancia que ocupa loc 3-8).
struct PropVertex {
    glm::vec3 pos, normal, color;
    glm::vec2 uv;
    /// PARTE del esqueleto (tronco = 0, ramas = 1..n). Va como float y no como entero porque el
    /// valor es un índice pequeño y `float` lo representa exacto, mientras que meterlo como PATRÓN
    /// DE BITS en un R32F lo dejaría en el rango de los denormales, que la GPU puede vaciar a cero.
    float partId = 0.0f;
};

// UBO del pase de LLUVIA (binding 5). x=tiempo · y=lluvia[0,1] · z=aspecto · w=inclinación(viento).
struct RainParams { glm::vec4 p; };
static_assert(sizeof(RainParams) == 16, "RainParams std140 size mismatch");

// UBO del cielo (binding 5) — compartido por sky.vert (la mat4) y sky.frag (el resto). Antes eran
// uniforms sueltos (glUniformMatrix4fv/3fv/1f). Truco std140: un vec3 tiene alineación 16 pero
// tamaño 12 → el float que le SIGUE cabe en el mismo slot de 16 B. Por eso los pares vec3+float.
struct SkyParams {
    glm::mat4 invViewProjRot; //   0..64
    glm::vec3 sunDir;         //  64..76
    float     sunElev;        //  76..80  (relleno del vec3 anterior)
    glm::vec3 up;             //  80..92
    float     atmo;           //  92..96
    glm::vec3 sunColor;       //  96..108
    float     time;           // 108..112  (segundos; para el MOVIMIENTO de las nubes)
    glm::vec4 weather;        // 112..128  x=humedad[0,1] · y=tempC · z=precipitación[0,1] · w=COBERTURA de nube[0,1]
    // El VIENTO del clima, en el plano tangente del observador (este/norte). Antes las nubes derivaban
    // con una constante hardcodeada (0.012, 0.006) y el follaje con su propio reloj → cada cosa soplaba
    // por su lado. Un solo vector para todos. w = 1 si lo que cae es NIEVE (nube gris ≠ nube de nieve).
    glm::vec4 wind;           // 128..144  x=este(m/s) · y=norte(m/s) · z=racha · w=esNieve
    // GEOMETRÍA DEL PLANETA para las capas de nube. Sin esto el cielo solo puede proyectar las nubes
    // sobre un plano falso (`tangente / (t + 0.22)`), que no tiene altitud: todas las capas se
    // comprimen igual hacia el horizonte y ninguna hace paralaje respecto a otra. Con radio y
    // altitud se puede intersecar el rayo con una CAPA REAL a `R + h`, que es lo que hace que una
    // nube baja pase por encima deprisa y un cirro apenas se mueva.
    glm::vec4 planet;         // 144..160  x=radio(m) · y=altitud de la cámara(m) · z=base de nube(m) · w libre
};
static_assert(sizeof(SkyParams) == 160, "SkyParams std140 size mismatch");

/** @brief UBO del pase VOLUMÉTRICO de nubes (binding 5). Declaración gemela de la de
 *  `cloud_vol.vert`/`cloud_vol.frag` — GLSL exige el bloque IDÉNTICO en las dos etapas.
 *
 *  Lleva DOS inversas y no una: `invViewProjRot` (solo rotación) para reconstruir la dirección del
 *  rayo, e `invViewProj` (completa, cámara-relativa) para deshacer la profundidad de la escena y
 *  saber a qué distancia hay geometría. El pase de cielo solo necesitaba la primera porque, al ser
 *  fondo, no tenía que respetar nada de lo que hubiera delante. */
struct CloudParams {
    glm::mat4 invViewProjRot; //   0..64
    glm::vec4 planetC;        //  64..80   xyz=centro del planeta RELATIVO a la cámara · w=radio(m)
    glm::vec4 slab;           //  80..96   x=base(m) · y=techo(m) · z=cobertura · w=precipitación
    glm::vec4 sun;            //  96..112  xyz=hacia el Sol · w=elevación
    glm::vec4 sunColor;       // 112..128  rgb=color del sol · a=día[0,1]
    glm::vec4 wind;           // 128..144  xy=deriva del campo · z=tiempo(s) · w=altitud del ojo(m)
    glm::vec4 misc;           // 144..160  x=atmósfera · y=pasos · z=escala · w=EXTINCIÓN por metro
};
static_assert(sizeof(CloudParams) == 160, "CloudParams std140 size mismatch");

/** @brief EXTINCIÓN por metro del volumen de nube: el mando de la DENSIDAD.
 *
 *  Empezó en 0.004 y la nube se veía translúcida — se leía el cielo a través de ella, que es lo que
 *  delata que no hay cuerpo. Subir esto hace que la profundidad óptica llegue antes a saturación
 *  (Beer-Lambert), o sea que atravesar el mismo espesor tape más.
 *
 *  ⚠️ BAJÓ de 0.014 a 0.008 y NO es que la nube tape menos: cambió lo que multiplica. Antes la
 *  densidad era `max(campo − umbral, 0)`, que sobre el campo real vale 0,05-0,21 — un factor de
 *  escala accidental que había que compensar aquí. Ahora `harukaCloudStrength` normaliza a [0,1], el
 *  núcleo de una nube vale 1 y esto vuelve a ser un coeficiente por metro de verdad. Medido con la
 *  fórmula nueva: con 0.008 un cúmulo de 1060 m sale opaco (alpha 0,96 en el cénit) y los bordes
 *  siguen suaves porque la suavidad la da la rampa del umbral, no la transparencia global. */
constexpr float kCloudExtinction = 0.008f;

/** @brief Pasos del raymarch de nubes. Deliberadamente POCOS: esto corre a pantalla completa, y lo
 *  que hace falta para que la nube deje de leerse como calcomanía es que el ESPESOR exista, no que
 *  la integral sea exacta. Es el primer número a tocar si el pase pesa. */
/// ⚠️ SUBIÓ de 24 a 64, y el permiso lo dio una MEDIDA, no una intuición: en el HUD del juego
/// `renderFrameContent` marcaba **1,42 ms** con este pase por debajo del umbral de 0,5 ms del panel,
/// o sea que costaba menos del 35 % de un frame ya barato. Con 24 pasos las nubes LEJANAS seguían
/// planas (el paso igualaba al tamaño de la nube a los 4,3 km); con 64 y un crecimiento más lento
/// la estructura se resuelve hasta los 15,3 km. Es el primer número a bajar si el pase pesa.
constexpr int kCloudSteps = 64;

/// `HARUKA_CLOUD_VOL=0` apaga el pase VOLUMÉTRICO de cúmulos. Vive aquí y no dentro del pase porque
/// lo consultan DOS sitios que tienen que estar de acuerdo: el propio pase y el conmutador que le
/// dice a `sky.frag` si debe pintar el cúmulo plano de fondo. Si solo lo mirara el pase, apagarlo
/// dejaría el cielo SIN cúmulos —ni volumétricos ni planos— y el A/B no compararía lo que dice.
static bool cloudVolumetricOff() {
    static const bool s_off = [] { const char* e = std::getenv("HARUKA_CLOUD_VOL");
                                   return e && std::atoi(e) == 0; }();
    return s_off;
}
} // namespace

void Application::setupQuad() {
    if (RHI::valid(m_quadBuf)) return;

    constexpr float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };

    RHI::Device* dev = RHI::device();
    m_quadBuf = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quadVertices), quadVertices);
}

void Application::buildRenderQueue() {
    HARUKA_PROFILE("buildRenderQueue");

    if (!_currentScene) { g_sceneRenderQueue.clear(); return; }

    // Cache the STATIC scene classification (kind/lod per object), which only
    // changes when the object set changes — not every frame. The expensive part
    // is the per-object string lowercasing + substring matching; transforms are
    // read live from the shared_ptr at draw time, so movement still updates.
    const auto& objects = _currentScene->getAllObjects();
    if (m_renderQueueDirty || objects.size() != m_renderQueueObjCount) {
        // INCREMENTAL: solo se clasifican los objetos NUEVOS. Clasificar es la parte cara (comparar
        // cadenas de tipo, mirar el mapa de propiedades) y antes se rehacía la escena ENTERA cada vez
        // que cambiaba el nº de objetos — con escombros apareciendo y caducando, eso es a todas horas.
        // Ahora un derrumbe cuesta clasificar sus 48 piezas, no reclasificar la ciudad.
        // Se identifica por UID, no por puntero: una dirección liberada se reutiliza para el objeto
        // siguiente, y entonces se daría por "ya clasificado" algo nuevo. Además así la fase de borrado
        // NO desreferencia punteros que pueden estar colgando.
        m_presentObjects.clear();
        m_presentObjects.reserve(objects.size());
        for (const auto& o : objects) if (o) m_presentObjects.insert(o->uid);

        // 1) Fuera los que ya no están en la escena (intercambio con el último → sin desplazar nada).
        if (m_staticQueueCount > g_sceneRenderQueue.size()) m_staticQueueCount = g_sceneRenderQueue.size();
        for (std::size_t k = 0; k < m_staticQueueCount; ) {
            if (m_presentObjects.count(g_sceneRenderQueue[k].uid)) { ++k; continue; }
            m_queuedObjects.erase(g_sceneRenderQueue[k].uid);
            g_sceneRenderQueue[k] = g_sceneRenderQueue[m_staticQueueCount - 1];
            --m_staticQueueCount;
        }
        g_sceneRenderQueue.resize(m_staticQueueCount);

        // 2) Entran los nuevos (los únicos que se clasifican).
        for (const auto& o : objects) {
            if (!o || m_queuedObjects.count(o->uid)) continue;
            Haruka::RenderCommand c = Haruka::classifySceneObject(*o);
            if (c.kind != Haruka::RenderKind::None) g_sceneRenderQueue.push_back(c);
            m_queuedObjects.insert(o->uid);    // se marca aunque no se dibuje: no reclasificarlo cada vez
        }
        m_staticQueueCount    = g_sceneRenderQueue.size();
        m_renderQueueObjCount = objects.size();
        m_renderQueueDirty    = false;
    } else {
        // Quita solo lo que se añadió el frame anterior (fantasmas de red). Sin copias ni realloc.
        g_sceneRenderQueue.resize(m_staticQueueCount);
    }

#ifdef HARUKA_NETWORK
    m_ghostObjects.clear();
    for (const auto& ghost : m_dgs.pollGhosts()) {
        Haruka::SceneObject obj;
        obj.name = "ghost_" + std::to_string(ghost.uuid);
        obj.type = "Character";
        obj.position = Haruka::WorldPos(
            ghost.chunkX * Haruka::Units::KM + ghost.pos[0],
            ghost.chunkY * Haruka::Units::KM + ghost.pos[1],
            ghost.chunkZ * Haruka::Units::KM + ghost.pos[2]
        );
        m_ghostObjects.push_back(std::move(obj));
    }
    for (const auto& obj : m_ghostObjects) {
        g_sceneRenderQueue.push_back({ &obj, Haruka::RenderKind::Primitive, Haruka::PrimitiveType::CAPSULE });
    }
#endif

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = _iTotalDrawCalls;
}

// === BLOOM: primer pase migrado a PSO/Context (ruta Vulkan) ===================================
// Antes: glUseProgram + glBindVertexArray + glUniform1f/1i + glBindTexture + glDrawArrays.
// Ahora: pipelines horneados (shader × vertex-layout × estado) + comandos por RHI::Context. Los
// uniforms SUELTOS pasaron al UBO BloomParams (binding 2) — glUniform* no existe en Vulkan.
RHI::TextureHandle Application::renderBloom(RHI::TextureHandle srcColorTex) {
    // Bloom runs at HALF resolution: ~4x fewer pixels through the blur ping-pong
    // for a near-identical look (bloom is low-frequency). The composite samples it
    // at full-res UV and linear-upscales.
    const int bw = std::max(1, m_postW / 2), bh = std::max(1, m_postH / 2);
    RHI::Device* dev = RHI::device();

    // (Re)create the two ping-pong color targets when the size changes.
    if (!RHI::valid(m_bloomPass[0]) || m_bloomW != bw || m_bloomH != bh) {
        if (RHI::valid(m_bloomPass[0])) { dev->destroy(m_bloomPass[0]); dev->destroy(m_bloomPass[1]); }
        for (int i = 0; i < 2; ++i) {
            RHI::RenderTargetDesc d;
            d.width = bw; d.height = bh; d.colorFormats = { RHI::Format::RGBA16F };
            d.colorFilter = RHI::Filter::Linear; d.hasDepth = false;
            m_bloomPass[i] = dev->createRenderTarget(d);
            m_bloomTexH[i] = dev->getColorTexture(m_bloomPass[i], 0);
        }
        m_bloomW = bw; m_bloomH = bh;
    }
    if (m_quadBuf.id == 0) setupQuad();   // crea el VBO del quad (el VAO lo aporta el PSO)

    // Pipelines horneados UNA vez: shader + vertex layout + estado de rasterizado. El VAO y el
    // glUseProgram viven DENTRO del pipeline → el llamador ya no toca GL.
    if (!RHI::valid(m_bloomExtractPSO)) {
        // OJO: createPipeline hace un ifstream CRUDO de la ruta — NO la resuelve. Hay que
        // enraizarla con el base dir de assets (lo que hace Shader por dentro). Pasar
        // "shaders/x.vert" a pelo → fichero no encontrado → shader 0 → programa sin linkar.
        const std::string vsPath = Shader::baseDir() + "shaders/screenquad.vert";
        const std::string fsExtract = Shader::baseDir() + "shaders/bloom_extract.frag";
        const std::string fsBlur    = Shader::baseDir() + "shaders/bloom_blur.frag";

        RHI::PipelineDesc pd;
        pd.vertexPath              = vsPath.c_str();
        pd.vertexLayout.strides     = { (uint32_t)(4 * sizeof(float)) };
        pd.vertexLayout.attributes = {
            { 0, 0,                 RHI::Format::RG32F },   // aPos (NDC xy)
            { 1, 2 * sizeof(float), RHI::Format::RG32F },   // aTexCoords
        };
        pd.topology     = RHI::PrimitiveTopology::TriangleStrip;
        pd.depth.test   = false;  pd.depth.write = false;   // pase fullscreen: sin depth
        pd.blend.enable = false;

        pd.fragmentPath   = fsExtract.c_str();
        m_bloomExtractPSO = dev->createPipeline(pd);
        pd.fragmentPath   = fsBlur.c_str();
        m_bloomBlurPSO    = dev->createPipeline(pd);

        m_bloomUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(BloomParams), nullptr,
                                       RHI::BufferMemory::Dynamic);
    }
    // Si los pipelines no se pudieron crear (shader ausente/roto — createPipeline ya lo logueó),
    // degrada a SIN bloom en vez de dibujar con un programa invalido (o reintentar cada frame).
    if (!RHI::valid(m_bloomExtractPSO) || !RHI::valid(m_bloomBlurPSO)) {
        static bool s_warned = false;
        if (!s_warned) { HARUKA_LOGW("Bloom", "pipelines PSO no disponibles → bloom desactivado"); s_warned = true; }
        return {};   // handle invalido → el composite lo detecta y compone sin bloom
    }

    // GL: beginFrame() es un getter puro del Context (el swap lo sigue haciendo la app en endFrame
    // propio). Cuando el frame entero pase por el RHI, begin/endFrame subirán al bucle de render.
    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues keep;                    // el quad cubre el target entero → no hace falta clear
    keep.clearColor = false; keep.clearDepth = false;

    // OJO (Vulkan): en GL cada draw se ejecuta al vuelo, así que reescribir el MISMO UBO entre
    // draws es correcto. En Vulkan habrá que usar offsets dinámicos o un UBO por draw (los comandos
    // se graban y se ejecutan después). Anotado para cuando entre el VKContext.
    auto setParams = [&](float threshold, float horizontal) {
        const BloomParams p{ threshold, horizontal, 0.0f, 0.0f };
        dev->updateBuffer(m_bloomUBO, 0, sizeof(p), &p);
    };

    // 1. Bright-pass: scene color -> tex[0].
    setParams(Haruka::SettingsManager::get().graphics().bloomThreshold, 0.0f);
    ctx->beginRenderPass(m_bloomPass[0], keep);   // bindea FBO + viewport(bw,bh)
    ctx->bindPipeline(m_bloomExtractPSO);
    ctx->bindVertexBuffer(m_quadBuf);
    ctx->bindUniformBuffer(2, m_bloomUBO);
    ctx->bindTexture(0, srcColorTex);
    ctx->draw(4);

    // 2. Separable Gaussian: N iterations of horizontal+vertical, ping-ponging
    //    tex[1] <-> tex[0]. Result ends up in tex[0].
    RHI::TextureHandle src = m_bloomTexH[0];
    // El radio del halo sale de aquí (σ ∝ √N). Ya no es un literal: es ajuste, junto al umbral y la
    // fuerza. Acotado 1..8 porque por debajo de 1 no hay blur y por encima de 8 el halo se come la
    // pantalla sin aportar nada — y ambos extremos son fáciles de escribir por error en el .json.
    const int iterations = std::clamp(Haruka::SettingsManager::get().graphics().bloomIterations, 1, 8);
    for (int i = 0; i < iterations; ++i) {
        setParams(0.0f, 1.0f);                          // horizontal -> tex[1]
        ctx->beginRenderPass(m_bloomPass[1], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, m_bloomUBO);
        ctx->bindTexture(0, src);
        ctx->draw(4);

        setParams(0.0f, 0.0f);                          // vertical -> tex[0]
        ctx->beginRenderPass(m_bloomPass[0], keep);
        ctx->bindPipeline(m_bloomBlurPSO);
        ctx->bindVertexBuffer(m_quadBuf);
        ctx->bindUniformBuffer(2, m_bloomUBO);
        ctx->bindTexture(0, m_bloomTexH[1]);
        ctx->draw(4);
        src = m_bloomTexH[0];
    }
    ctx->endRenderPass();

    return m_bloomTexH[0];   // handle: el composite (también PSO) lo bindea por el Context
}

void Application::renderFrameContent() {
    // Editor path calls this externally (no standalone loop) → reset here.
    // Standalone loop resets at its own frame boundary so game.onUpdate is measured.
    if (_editorTarget) Haruka::Profiler::get().newFrame();

    // ── AUDITORÍA DE RENDIMIENTO (`HARUKA_PROFILE_DUMP=segundos`) ───────────────────────────────
    //
    // El panel del HUD colapsa todo lo que baja de 0,5 ms ("+8 < 0.5 ms"), y para auditar eso es
    // justo lo que hay que ver: un scope de 0,3 ms que entra 200 veces por frame no sale en el panel
    // y es medio frame. Esto vuelca el árbol ENTERO al log, con tiempo propio (inclusivo menos
    // hijos), que es lo que señala al culpable en vez de al que lo contiene.
    //
    // Al log y no al HUD porque una auditoría se compara entre corridas, y para eso hace falta texto
    // que se pueda diffear, no una ventana que hay que fotografiar.
    {
        static const double s_dumpEvery = [] {
            const char* e = std::getenv("HARUKA_PROFILE_DUMP");
            return e ? atof(e) : 0.0;
        }();
        if (s_dumpEvery > 0.0) {
            static double s_last = -1e9;
            static int    s_frames = 0;
            ++s_frames;
            if (m_diagClock - s_last > s_dumpEvery) {
                s_last = m_diagClock;
                HARUKA_LOGI("Perf", "arbol del frame (media de %d frames):\n%s", s_frames,
                            Haruka::Profiler::get().dumpTree(1).c_str());
                s_frames = 0;
            }
        }
    }

    HARUKA_PROFILE("renderFrameContent");
    const uint32_t width  = _window ? _window->getWidth()  : static_cast<uint32_t>(m_editorViewportW);
    const uint32_t height = _window ? _window->getHeight() : static_cast<uint32_t>(m_editorViewportH);

    // GPU timer disabled during RHI migration

    // --- Standalone post-processing target ---------------------------------
    // Route the 3D scene through an offscreen HDR target when any screen-space
    // effect or non-1.0 render scale is active. With everything off this stays
    // false → the scene renders straight to the screen exactly as before.
    const auto& gpost  = Haruka::SettingsManager::get().graphics();
    const float rscale = std::min(std::max(gpost.renderScale, 0.5f), 2.0f);
    const bool  wantFXAA = (gpost.antialiasing == Haruka::Settings::AntialiasingMode::FXAA);
    const bool  wantBloom = gpost.bloom;
    const int   renderW  = std::max(1, (int)(width  * rscale));
    const int   renderH  = std::max(1, (int)(height * rscale));
    m_postActive = !_editorTarget && (rscale != 1.0f || wantFXAA || wantBloom);
    if (m_postActive) {
        if (!_postScene || m_postW != renderW || m_postH != renderH) {
            _postScene = std::make_unique<HDR>((unsigned)renderW, (unsigned)renderH);
            m_postW = renderW; m_postH = renderH;
        }
    }
    // Mecánica celeste: el WorldSystem usa el planeta REAL (su PlanetarySystem propio
    // está vacío). Le pasamos centro/radio del planeta activo y avanzamos día/noche +
    // luna + marea + viento cada frame (WorldSystem::update no se llama).
    if (_worldSystem && _planetarySystem) {
        glm::dvec3 pc; double pr;
        if (_planetarySystem->getActivePlanet(pc, pr))
            _worldSystem->setActivePlanet(pc, pr);
        { HARUKA_PROFILE("world.advanceCelestial");
          _worldSystem->advanceCelestial(deltaTime > 0.0f ? (double)deltaTime : 0.016); }
    }

    // NEAR PLANE DINÁMICO: pegado a cualquier superficie (alt ≤ 2 km) near=0.1 (precisión cercana
    // para terreno/objetos/ítem en mano). En órbita el near sube con la altitud → la precisión del
    // depth lejano mejora ~millones× → se acaba el z-fight agua↔lecho desde el espacio. Usa el
    // planeta MÁS CERCANO (no el home) para no recortar superficies cercanas en un sobrevuelo.
    if (_camera && _planetarySystem) {
        HARUKA_PROFILE("camera.nearPlane(recorre planetas)");
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        double nearestAlt = 1e30;
        for (const auto& pl : _planetarySystem->getPlanets()) {
            double a = glm::length(camD - pl.position) - pl.radius;
            if (a < nearestAlt) nearestAlt = a;
        }
        // ⚠️ ARREGLADO: el umbral era 2 km y la pendiente 0,05, o sea que a 10 km el near valía
        // **400 m**. Todo lo que estuviera a menos de eso se recortaba — y eso incluye las dos cosas
        // que el jugador tiene siempre delante: el ÍTEM EN LA MANO (~0,5 m) y EL SUELO QUE PISA.
        // Bastaba subir a 2 km para quedarse sin manos y sin terreno cercano, mirando el mundo por un
        // agujero. El síntoma se leía como "el terreno desaparece", pero no desaparecía: se recortaba.
        //
        // El criterio estaba mal planteado: usaba la ALTITUD como si midiera "qué lejos está lo que
        // dibujo", cuando lo que decide el near es la distancia a lo MÁS CERCANO que hay que ver, y
        // eso a ras de cámara es medio metro pase lo que pase.
        //
        // Por qué se puede subir tanto el umbral: el depth es **D32F con reversed-Z** (clear a 0,
        // test GREATER) en todos los targets. Esa combinación concentra la precisión donde hace falta
        // y es justo la que hace innecesario inflar el near — que es la técnica que se usaba ANTES de
        // tener reversed-Z. Con 30 km de umbral el z-fight agua↔lecho que esto vino a arreglar sigue
        // cubierto (aparecía desde órbita, no desde 2 km) y el juego a pie o volando bajo conserva
        // manos y suelo.
        //
        // El tope de 200 m evita además que en órbita alta el near se dispare a `alt/2`, que era lo
        // que hacía imposible dibujar NADA cercano — una nave, una estación, tu propio vehículo.
        float dynNear = 0.1f;
        if (nearestAlt > 30000.0)
            dynNear = glm::clamp((float)((nearestAlt - 30000.0) * 0.02), 0.1f, 200.0f);
        _camera->setNearPlane(dynNear);
    }

    // Props del mundo: refresca el scatter global si la cámara cruzó un tramo (caché por posición).
    // Debe correr ANTES del render pass de escena, donde el pase instanciado lee m_propRegistry.
    { HARUKA_PROFILE("prop.scatter(re-siembra)"); refreshPropScatter(); }

    // Cielo atmosférico: color por elevación solar + altitud (azul de día → cálido al
    // amanecer/atardecer → oscuro de noche → negro en el espacio). Fallback oscuro.
    glm::vec3 sky(0.01f);
    if (_worldSystem && _camera) { HARUKA_PROFILE("world.getSkyColor");
        sky = _worldSystem->getSkyColor(glm::dvec3(_camera->position)); }
    RHI::Device* frameDev = RHI::device();
    RHI::Context* frameCtx = nullptr;
    { HARUKA_PROFILE("rhi.beginFrame"); frameCtx = frameDev ? frameDev->beginFrame() : nullptr; }
    RHI::RenderPassHandle sceneTargetPass;
    if (_editorTarget) {
        sceneTargetPass = _editorTarget->getPass();
    } else if (m_postActive && _postScene) {
        sceneTargetPass = _postScene->getPass();
    }

    // CLEAR DEL FRAME, incondicional y en UN SOLO SITIO. Antes lo hacía el pase de cielo, que vive
    // bajo tres condiciones (`_worldSystem && _camera && _planetarySystem`, que haya planeta ACTIVO
    // y que el PSO del cielo compilara). Una escena sin planeta —la típica del editor: unas
    // primitivas y una luz— no cumplía la segunda, así que no se limpiaba NI color NI profundidad:
    // con reversed-Z el test es GREATER, el z-buffer conservaba el del frame anterior y a partir
    // del segundo frame ningún fragmento a la misma profundidad volvía a pasar → viewport negro.
    // El cielo, cuando corre, reabre este mismo pase SIN clear y pinta encima.
    if (frameCtx) {
        HARUKA_PROFILE("frame.clear(pass)");
        RHI::ClearValues frameClear;
        frameClear.clearColor = true;
        frameClear.color[0] = sky.r; frameClear.color[1] = sky.g; frameClear.color[2] = sky.b;
        frameClear.color[3] = 1.0f;
        frameClear.clearDepth = true; frameClear.depth = 0.0f;   // reversed-Z: lejos = 0
        frameCtx->beginRenderPass(sceneTargetPass, frameClear);
        if (!RHI::valid(sceneTargetPass))            // el pass a pantalla NO fija viewport
            frameCtx->setViewport(0, 0, (int)width, (int)height);
        frameCtx->endRenderPass();
    }

    // ── TRABAJO DE COMPUTE, ANTES DE ABRIR NINGÚN RENDER PASS ───────────────────────────────────
    //
    // ⚠️ El orden es OBLIGATORIO, no una optimización. `vkCmdDispatch` dentro de una instancia de
    // render pass es ILEGAL en Vulkan, y el culling de parches del terreno se despachaba dentro del
    // pase de escena porque en OpenGL eso es legal y corriente. Cerraba el programa, y el síntoma
    // despistaba: en una captura de RenderDoc los draws salían BIEN uno a uno —el replay los ejecuta
    // aislados— mientras en ejecución el dispositivo se perdía a mitad de frame y quedaba la
    // pantalla negra. Si alguien mueve esta llamada dentro de un pase, vuelve el mismo fallo.
    if (_camera && _planetarySystem) {
        HARUKA_PROFILE("frame.compute.prepare");
        _planetarySystem->prepareSimplePlanets(glm::dvec3(_camera->position));
    }

    // Pase de cielo procedural (gradiente + sol + estrellas) como FONDO: triángulo
    // fullscreen SIN escribir profundidad → el terreno/objetos se pintan encima.
    if (_worldSystem && _camera && _planetarySystem) {
        HARUKA_PROFILE("frame.sky+clima+sol(UBO)");
        glm::dvec3 pc; double pr;
        if (_planetarySystem->getActivePlanet(pc, pr)) {
            // === PASE 3 MIGRADO A PSO/Context ===
            // Sin VBO: el triángulo fullscreen sale de gl_VertexID → pipeline SIN vertex layout
            // (createPipeline crea igualmente el VAO vacío que bindPipeline ata) y draw(3).
            RHI::Device* skyDev = RHI::device();
            if (!RHI::valid(m_skyPSO)) {
                const std::string vsPath = Shader::baseDir() + "shaders/sky.vert";
                const std::string fsPath = Shader::baseDir() + "shaders/sky.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vsPath.c_str();
                pd.fragmentPath = fsPath.c_str();
                pd.topology     = RHI::PrimitiveTopology::Triangles;  // vertexLayout vacío a propósito
                // Fondo: sin depth test → GL tampoco ESCRIBE depth (con el test off no se actualiza
                // el z-buffer), así que el terreno/objetos se pintan encima sin más.
                pd.depth.test   = false;  pd.depth.write = false;
                pd.blend.enable = false;
                m_skyPSO = skyDev->createPipeline(pd);
                m_skyUBO = skyDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SkyParams), nullptr,
                                                RHI::BufferMemory::Dynamic);
            }
            // Si el pipeline no se creó (shader roto), saltar el cielo: el clearColor deja un fondo
            // de respaldo y el resto de la escena renderiza normal.
            if (RHI::valid(m_skyPSO)) {
            const glm::dvec3 camD = glm::dvec3(_camera->position);
            glm::dvec3 up = camD - pc; double ul = glm::length(up);
            up = (ul > 1e-9) ? up / ul : glm::dvec3(0, 1, 0);
            glm::vec3 sunDir  = _worldSystem->getDominantLightDirection(camD);
            glm::vec3 sunCol  = _worldSystem->getDominantLightColor(camD);
            float     sunElev = (float)glm::dot(glm::dvec3(sunDir), up);
            float     alt     = (float)(ul - pr);
            float     atmo    = 1.0f - glm::smoothstep(0.0f, (float)(pr * 0.02), alt);

            float aspectS = (float)(m_postActive ? renderW : width)
                          / (float)(m_postActive ? renderH : height);
            glm::mat4 viewRot  = glm::mat4(glm::mat3(_camera->getViewMatrix()));
            glm::mat4 invVPRot = glm::inverse(_camera->getProjectionMatrix(aspectS) * viewRot);

            static const auto s_skyT0 = std::chrono::steady_clock::now();
            const float skyTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_skyT0).count();

            // ── CLIMA: se LEE del mundo, no se inventa aquí ─────────────────────────────────────
            // Antes este bloque calculaba la lluvia con `0.5+0.5·sin(t·0.05)` mientras sky.frag
            // calculaba SUS nubes con `fbm(t·0.008)`. Dos fórmulas sin relación → llovía con el
            // cielo despejado. Ahora ambos leen el MISMO `WeatherSample`: la cobertura que el shader
            // dibuja es la que produjo la lluvia, y la precipitación existe solo bajo esa nube.
            Haruka::WeatherSample wx;
            if (_planetarySystem) wx = _planetarySystem->weatherAt(camD);
            float precip = wx.precip;               // lluvia O nieve: las dos encapotan el cielo
            float rain   = wx.rainAmount();         // solo LLUVIA: es lo que dibuja el pase de gotas
            float cover  = wx.cloudCover;
            if (m_rainOverride >= 0.0f) {           // consola `rain` (pruebas): fuerza el temporal
                rain   = m_rainOverride;            // ...y CON él la nube que lo justifica, o volvería
                precip = m_rainOverride;            //    a verse lluvia con cielo azul (el bug de antes)
                cover  = glm::max(cover, rain);
            }
            m_rainAmount = rain;                                    // gotas
            m_snowAmount = (m_rainOverride >= 0.0f) ? 0.0f : wx.snowAmount();   // copos
            m_windVec    = wx.wind;                                 // el ÚNICO viento del mundo

            // MOJADO DEL SUELO: se INTEGRA, no se copia de la lluvia. Mojarse cuesta ~30 s de
            // chaparrón; secarse, minutos. Esa asimetría es la que hace que el suelo siga oscuro
            // (y brillante) un rato después de escampar, en vez de apagarse con la última gota.
            {
                static auto s_wetT = std::chrono::steady_clock::now();
                const auto  now    = std::chrono::steady_clock::now();
                const float dtW    = glm::clamp(std::chrono::duration<float>(now - s_wetT).count(), 0.0f, 0.25f);
                s_wetT = now;
                m_groundWetness += dtW * (rain > 0.01f ? rain / 30.0f : -1.0f / 240.0f);
                m_groundWetness  = glm::clamp(m_groundWetness, 0.0f, 1.0f);

                // NIEVE ACUMULADA: cuaja mientras nieva y FUNDE con la temperatura del sitio. No es
                // "hace frío ⇒ suelo blanco": si no ha nevado no hay nieve, y si sube la temperatura
                // se va aunque siga siendo invierno. Fundir es MUCHO más lento que cuajar.
                const float melt = glm::smoothstep(0.0f, 6.0f, wx.tempC) / 420.0f;
                m_snowAccum += dtW * (m_snowAmount > 0.01f ? m_snowAmount / 45.0f : -melt);
                m_snowAccum  = glm::clamp(m_snowAccum, 0.0f, 1.0f);
            }

            // Viento del clima proyectado al marco del observador (este/norte) → nubes y lluvia
            // inclinan HACIA EL MISMO LADO. `wind` viene en el marco local del planeta.
            glm::vec3 eastW = glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(up)));
            if (!std::isfinite(eastW.x)) eastW = glm::vec3(1, 0, 0);
            const glm::vec3 northW = glm::cross(glm::vec3(up), eastW);
            const glm::vec2 windEN(glm::dot(wx.wind, eastW), glm::dot(wx.wind, northW));
            m_windSlant = glm::clamp(windEN.x * 0.02f, -0.45f, 0.45f);   // para el pase de lluvia

            SkyParams sp{};
            sp.invViewProjRot = invVPRot;
            sp.sunDir         = sunDir;
            sp.sunElev        = sunElev;
            sp.up             = glm::vec3(up);
            sp.atmo           = atmo;
            sp.sunColor       = sunCol;
            sp.time           = skyTime;   // para el desplazamiento de las nubes
            sp.weather        = glm::vec4(wx.humidity, wx.tempC, precip, cover);
            sp.wind           = glm::vec4(windEN.x, windEN.y, glm::length(wx.wind),
                                          wx.isSnow() ? 1.0f : 0.0f);
            // Radio del planeta y altitud del ojo sobre el nivel del mar. Se calculan en DOUBLE y
            // solo el resultado baja a float: la resta `|cam − centro| − R` es entre magnitudes de
            // ~6,37e6, y en float el resultado quedaría cuantizado a medio metro.
            {
                glm::dvec3 pcD(0.0); double prD = 0.0;
                float rad = 0.0f, alt = 0.0f;
                if (_planetarySystem && _planetarySystem->getActivePlanet(pcD, prD) && prD > 0.0) {
                    rad = (float)prD;
                    alt = (float)(glm::length(glm::dvec3(_camera->position) - pcD) - prD);
                }
                // z = BASE DE LA NUBE del clima (`WeatherSample::cloudBaseM`), no una constante del
                // shader: es la misma altura que ya define el techo de la lluvia, así que la panza
                // de los cúmulos y el punto donde nacen las gotas coinciden por construcción.
                // w = CONMUTADOR del cúmulo, con la convención que documenta `sky.frag`:
                //   0  → lo dibuja el pase VOLUMÉTRICO; aquí solo van cirro y altocúmulo.
                //  >0  → no hay pase volumétrico, y el valor ES el techo del cúmulo plano. Antes
                //        estaba cableado a `base + 1700 m`, o sea el mismo desarrollo vertical con
                //        buen tiempo que con tormenta; ahora lo trae el clima (~430 m de estrato
                //        frente a >3800 m de cumulonimbo).
                sp.planet = glm::vec4(rad, alt, wx.cloudBaseM,
                                      (m_volumetricClouds && !cloudVolumetricOff()) ? 0.0f
                                                                                  : wx.cloudTopM);
            }
            skyDev->updateBuffer(m_skyUBO, 0, sizeof(sp), &sp);

            RHI::Context* ctx = skyDev->beginFrame();
            // SIN clear: el frame ya se limpió arriba (color de cielo + depth), y este triángulo
            // fullscreen cubre la pantalla entera de todas formas.
            // sceneTargetPass ya resuelve _editorTarget (viewport del IDE) o _postScene; si
            // ninguno está activo queda inválido = backbuffer de pantalla. Usarlo aquí es lo
            // que hace que la escena caiga DENTRO del RenderTarget del editor.
            RHI::ClearValues sceneClear;
            sceneClear.clearColor = false; sceneClear.clearDepth = false;
            ctx->beginRenderPass(sceneTargetPass, sceneClear);
            if (!RHI::valid(sceneTargetPass))                        // el pass a pantalla NO fija viewport
                ctx->setViewport(0, 0, (int)width, (int)height);
            { HARUKA_PROFILE("sky.draw");
            ctx->bindPipeline(m_skyPSO);                             // programa + depth off (no escribe z)
            ctx->bindUniformBuffer(5, m_skyUBO);
            ctx->draw(3);
            }                                            // sin vertex buffer: gl_VertexID
            ctx->endRenderPass();

            } // if (RHI::valid(m_skyPSO))
        }
    }

    _iTotalDrawCalls    = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = 0;
    _iTotalVertices     = 0;
    _iTotalTriangles    = 0;
    _iRenderedVertices  = 0;
    _iRenderedTriangles = 0;

    if (_currentScene && _camera) {
        HARUKA_PROFILE("scene.setup(UBOs+shaders+luces)");
        // Lazy-create UBOs (dinámicos; el update por frame sigue con glBufferSubData/glBindBufferBase).
        RHI::Device* uboDev = RHI::device();
        if (!RHI::valid(m_uboPerFrameH)) {
            m_uboPerFrameH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
        }
        if (!RHI::valid(m_uboPerObjectH)) {
            m_uboPerObjectH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);
        }

        const bool useFinalLook = getRenderFeatureHDR() || getRenderFeatureBloom()
                                || getRenderFeatureSSAO() || getRenderFeatureIBL()
                                || getRenderFeatureShadows();
        if (!_mainShader || _mainShaderUsesFinalLook != useFinalLook) {
            _mainShader = std::make_unique<Shader>(
                "shaders/simple.vert",
                useFinalLook ? "shaders/final.frag" : "shaders/preview.frag"
            );
            _mainShaderUsesFinalLook = useFinalLook;
        }
        // (Los programas de planet.* y water.* los poseen ahora los PSO del TerrainRenderer y del
        //  WaterRenderer — cada pase ata su propio pipeline; ya no hay Shader suelto que bindear.)

        // === PASE 4 MIGRADO A PSO/Context: los OBJETOS de la escena. ===
        // El pipeline hornea shader + layout `Vertex` + estado (depth on). Se recrea solo si
        // cambia la variante de fragment (final.frag <-> preview.frag).
        if (!RHI::valid(m_scenePSO) || m_scenePSOFinalLook != useFinalLook) {
            if (RHI::valid(m_scenePSO)) uboDev->destroy(m_scenePSO);
            const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
            const std::string fsPath = Shader::baseDir() +
                (useFinalLook ? "shaders/final.frag" : "shaders/preview.frag");
            using V = Haruka::Renderer::Vertex;
            RHI::PipelineDesc pd;
            pd.vertexPath              = vsPath.c_str();
            pd.fragmentPath            = fsPath.c_str();
            pd.vertexLayout.strides     = { (uint32_t)(sizeof(V)) };
            pd.vertexLayout.attributes = {
                { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
                { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
                { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
                { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
                { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
            };
            pd.topology     = RHI::PrimitiveTopology::Triangles;
            pd.depth.test   = true;  pd.depth.write = true;   // escena sólida
            pd.blend.enable = false;
            pd.cull         = RHI::CullMode::Back;   // lo hacía el glEnable(GL_CULL_FACE) global
            m_scenePSO = uboDev->createPipeline(pd);
            m_scenePSOFinalLook = useFinalLook;
        }

        RHI::Context* sceneCtx = uboDev->beginFrame();
        if (RHI::valid(m_scenePSO)) {
            // El target de escena ya está bindeado y limpiado arriba → reabrir el MISMO pass SIN
            // clear (borraría el cielo ya pintado).
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            sceneCtx->beginRenderPass(sceneTargetPass, keep);
            if (!RHI::valid(sceneTargetPass))           // el pass a pantalla NO fija viewport
                sceneCtx->setViewport(0, 0, (int)width, (int)height);
            sceneCtx->bindPipeline(m_scenePSO);         // programa + depth on (absorbe glUseProgram)
            sceneCtx->bindUniformBuffer(0, m_uboPerFrameH);
            sceneCtx->bindUniformBuffer(1, m_uboPerObjectH);
        }

        // Upload per-frame UBO
        const float      aspect       = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::vec3  cameraOrigin = glm::vec3(_camera->position);

        PerFrameUBOData frameData{};
        frameData.view            = glm::mat4(glm::mat3(_camera->getViewMatrix()));
        frameData.projection      = _camera->getProjectionMatrix(aspect);
        frameData.cameraPos       = cameraOrigin;
        // Sun direction and color come from the WorldSystem (supports multiple stars).
        // Lazy sync in case the scene was loaded before the WorldSystem was ready.
        if (_worldSystem && _currentScene && _worldSystem->getBodies().empty())
            _worldSystem->syncFromScene(*_currentScene);

        if (_worldSystem && !_worldSystem->getBodies().empty())
            frameData.sunDirection  = _worldSystem->getDominantLightDirection(glm::dvec3(cameraOrigin));
        else
            frameData.sunDirection  = glm::vec3(-0.55f, 0.69f, -0.41f); // reasonable fallback
        frameData.sunLightColor = (_worldSystem && !_worldSystem->getBodies().empty())
            ? _worldSystem->getDominantLightColor(glm::dvec3(cameraOrigin))
            : glm::vec3(1.0f, 0.98f, 0.95f);
        // Ambiente por ATMÓSFERA: de día luz ambiental clara, de noche casi nada (la
        // luna aporta el relleno). Suaviza la transición con la elevación solar.
        //
        // El ambiente EN COLOR que sale del cielo integrado vive aquí, en el ámbito de la función:
        // lo calcula el bloque de abajo y lo consume el planeta un poco más allá.
        static glm::vec3 s_skyAmbient{0.15f, 0.18f, 0.24f};
        static glm::vec3 s_shCoef[9]{};   // los 9 coeficientes, para el fragment
        {
            float el  = _worldSystem ? _worldSystem->getSunElevation(glm::dvec3(cameraOrigin)) : 1.0f;
            float day = glm::smoothstep(-0.10f, 0.25f, el);
            // Suelo nocturno subido (0.04→0.11): el lado noche era casi negro → parecía "media
            // planeta sin dibujar". Ahora se ve TENUE (luz de estrellas/cielo), sigue siendo noche.
            frameData.ambientStrength = glm::mix(0.11f, 0.28f, day);

            // ── EL AMBIENTE SALE DEL CIELO QUE SE DIBUJA ───────────────────────────────────────
            //
            // ⚠️ `ambientStrength` era un escalar cosido a mano, y MEDIDO estaba entre 5 y 8 veces
            // por debajo de la luz que el cielo entrega de verdad: con sol alto daba
            // (0.154, 0.182, 0.238) contra (0.714, 1.372, 2.611) del cielo. Por eso cualquier
            // superficie sin sol directo caía a 0,043 de luminancia bajo un cielo de 0,60 y se leía
            // como un agujero negro — no era un problema de sombras, era el ambiente.
            //
            // `sky_ambient.cpp` integra la MISMA paleta que dibuja `sky.frag` en armónicos
            // esféricos. Estaba escrito, documentado y sin llamar por nadie.
            //
            // ⚠️ Devuelve IRRADIANCIA (pasa de 1 en el azul), no un multiplicador de albedo: meterla
            // tal cual quemaría el planeta. La normalización lambertiana es dividir por π — así
            // `albedo · ambiente` vuelve a ser la radiancia saliente correcta.
            //
            // Se recalcula por tramos de elevación solar: la cuadratura esférica no es cara, pero
            // rehacerla cada frame no aporta nada cuando el sol se mueve en minutos.
            // ⚠️ NO va en `PerFrameUBOData`: ese bloque tiene layout std140 fijado por un
            // `static_assert` y por su gemelo en GLSL. Añadirle un campo desalinea el UBO de TODOS
            // los shaders que lo leen. El ambiente del cielo lo consume el planeta por su propia
            // vía (`setSunLight` → `SimplePlanetUBO`), así que vive fuera de ese struct.
            {
                static Haruka::SkySH s_sh;
                static float         s_shElev = -999.0f;
                if (std::abs(el - s_shElev) > 0.01f) {
                    s_shElev = el;
                    s_sh = Haruka::skyAmbientSH(el, 0.0f);
                    for (int i = 0; i < 9; ++i) s_shCoef[i] = s_sh.coef[i];
                }
                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                s_skyAmbient = Haruka::skyAmbientEval(s_sh, up, up)
                             * (1.0f / 3.14159265358979323846f);
            }
        }
        // El terreno del planeta usa la MISMA luz que el cielo: sin esto el SimplePlanet
        // iluminaba con una dirección fija y no respondía al sol que se ve en el cielo.
        if (_planetarySystem)
            _planetarySystem->setSunLight(frameData.sunDirection, frameData.sunLightColor,
                                          s_skyAmbient);
            // Y los 9 coeficientes, para que el fragment evalúe el ambiente POR NORMAL. Sin esto el
            // ambiente es una constante y todo lo que está en sombra sale plano (ver `sky_sh.glsl`).
            _planetarySystem->setSkyAmbientSH(s_shCoef);
        // Viento atmosférico → arrastre aerodinámico de la física (por cuerpo, barato).
        if (_physicsEngine && _worldSystem)
            _physicsEngine->setWind(_worldSystem->getWind(glm::dvec3(cameraOrigin)));
        // Avanza el motor de física con TIMESTEP FIJO (determinista). Hoy solo procesa cuerpos
        // DINÁMICOS (el jugador aún es kinemático → no afecta); lo activa de verdad la Fase 2.
        if (_physicsEngine)
            _physicsEngine->advance(deltaTime > 0.0f ? (double)deltaTime : 0.016);
        // Luz de luna (2ª luz): dirección + brillo por fase (WorldSystem), color azulado.
        {
            glm::vec3 moonDir(0.0f, 1.0f, 0.0f); float moonI = 0.0f;
            if (_worldSystem) _worldSystem->getMoonLight(glm::dvec3(cameraOrigin), moonDir, moonI);
            frameData.moonDirection  = moonDir;
            frameData.moonIntensity  = moonI;
            frameData.moonLightColor = glm::vec3(0.55f, 0.62f, 0.85f); // azul plateado
        }
        // Only advertise a feature if its GPU resources are actually allocated.
        // Enabling a flag without the corresponding FBO/texture bound causes
        // undefined behaviour in final.frag (samples from empty texture units).
        frameData.enableHDR       = (int)(getRenderFeatureHDR()     && _hdr    != nullptr);
        frameData.enableBloom     = (int)(getRenderFeatureBloom()   && _bloom  != nullptr);
        frameData.enableSSAO      = (int)(getRenderFeatureSSAO()    && _ssao   != nullptr);
        frameData.enableIBL       = (int)(getRenderFeatureIBL()     && _ibl    != nullptr);
        frameData.enableShadows   = (int)(getRenderFeatureShadows() && _shadow != nullptr);

        uboDev->updateBuffer(m_uboPerFrameH, 0, sizeof(PerFrameUBOData), &frameData);

        // ── DIAGNÓSTICO DE LOS CUERPOS EMISORES ─────────────────────────────────────────────────
        //
        // "¿El sol no se renderiza?" no se contesta leyendo el código: una estrella es un objeto de
        // escena como cualquier otro (esfera emisiva, `castLight` → exenta del cull por distancia) y
        // el cielo solo pinta el resplandor, no el disco. Si no se ve, la causa está en uno de tres
        // sitios y los tres son números: detrás de la cámara, fuera del frustum, o subpíxel.
        //
        // ⚠️ Recorre TODAS las estrellas, sin `break`. El motor es explícitamente multi-estrella
        // (`getDominantLightDirection` suma la contribución de cada una pesada por 1/dist²), así que
        // un diagnóstico que se pare en la primera mentiría justo en el caso que importa: un sistema
        // binario donde la que no sale es la segunda.
        m_diagClock += (deltaTime > 0.0f ? (double)deltaTime : 0.016);
        if (_worldSystem && m_diagClock - m_lastStarLog > 5.0) {
            const glm::dvec3 camD = glm::dvec3(_camera->position);
            for (const auto& b : _worldSystem->getBodies()) {
                if (b.type != Haruka::ObjectType::STAR) continue;
                const glm::dvec3 rel  = b.worldPos - camD;
                const double     dist = glm::length(rel);
                if (dist < 1.0) continue;
                // Semidiámetro aparente y su tamaño en PÍXELES con el FOV vertical actual.
                const double angRad = std::atan2((double)b.radius, dist);
                const double pxDiam = 2.0 * angRad / glm::radians((double)_camera->zoom) * (double)height;
                // ¿Dónde cae en pantalla? MISMO camino que el draw: view rotation-only + proj.
                const glm::vec4 clip = frameData.projection * frameData.view
                                     * glm::vec4(glm::vec3(rel), 1.0f);
                const bool      front = clip.w > 0.0f;
                const glm::vec3 ndc   = front ? glm::vec3(clip) / clip.w : glm::vec3(0.0f);
                // Elevación de ESTA estrella sobre el horizonte local, no la del conjunto.
                double elev = 0.0;
                if (_planetarySystem) {
                    glm::dvec3 pc; double pr = 0.0;
                    if (_planetarySystem->getActivePlanet(pc, pr)) {
                        const glm::dvec3 upv = camD - pc;
                        const double ul = glm::length(upv);
                        if (ul > 1e-9) elev = glm::dot(rel / dist, upv / ul);
                    }
                }
                HARUKA_LOGDIAG("Star",
                    "'%s' dist=%.3e m radio=%.3e m diam=%.1f px | delante=%d ndc=(%.2f,%.2f) "
                    "depth=%.3e | elev=%.2f",
                    b.name.c_str(), dist, (double)b.radius, pxDiam, (int)front,
                    ndc.x, ndc.y, (double)ndc.z, elev);
            }
            m_lastStarLog = m_diagClock;
        }

        // Cull face handled by the pipeline

        // TOTAL = geometría que el frame podía dibujar ANTES de culling; RENDERED = lo que de verdad
        // entró en el draw. Solo divergen donde hay cull de verdad: props (frustum/sub-pixel + tope
        // del buffer) y agua (caras tras el planeta). El resto (objetos, piezas, terreno) dibuja
        // todo lo que llega, así que TOTAL == RENDERED ahí.
        int renderedDrawCalls = 0;
        int renderedVertices  = 0;
        int renderedTriangles = 0;
        int totalDrawCalls = 0;
        int totalVertices  = 0;
        int totalTriangles = 0;

        // Piezas de CONSTRUCCIÓN (prop "construction"/"building"): en vez de 1 draw por pieza, se
        // acumulan por (malla, material) y se dibujan INSTANCIADAS tras el bucle (1 draw por grupo).
        // El material del grupo es el del MaterialComponent del objeto (per-pixel, MISMO aspecto que
        // el pase de escena); dos piezas con la misma malla y el mismo material comparten el draw.
        std::unordered_map<std::string, ConstInstGroup> constGroups;

        { HARUKA_PROFILE("scene.objects.draw");
        for (const auto& command : g_sceneRenderQueue) {
            const auto* obj = command.object;
            if (!obj) continue;

            // "Emisivo" (estrella, sin sombreado) = el cuerpo EMITE luz. Así un
            // CelestialBody que NO emite (la Luna) queda SOMBREADO por el Sol → se
            // ven sus cráteres y fases, en vez de salir a pleno brillo.
            const bool isStar = obj->flags.castLight;

            // Distance LOD/cull for scene objects (they have no chunk LOD like terrain):
            // skip far props/models so they don't cost draw calls at any range. Scale-aware
            // (bigger objects stay visible farther). Stars/celestial bodies are never culled
            // (they must show from orbit/space).
            if (!isStar) {
                double objDist  = glm::length(obj->position - _camera->position);
                double maxScale = std::max({obj->scale.x, obj->scale.y, obj->scale.z});
                double maxDist  = 500.0 + maxScale * 300.0;   // 1 m → ~800 m, 50 m → ~15 km
                if (objDist > maxDist) continue;
            }

            glm::vec3 baseColor = glm::vec3(obj->color);
            if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.75f, 0.76f, 0.80f);

            // Stars emit from their light component color, not the default object color.
            if (isStar && obj->components.contains("light")) {
                const auto& lc = obj->components["light"];
                if (lc.contains("color") && lc["color"].is_array() && lc["color"].size() >= 3)
                    baseColor = glm::vec3(lc["color"][0].get<float>(),
                                         lc["color"][1].get<float>(),
                                         lc["color"][2].get<float>());
            }

            // El albedo del material TIÑE el color de la pieza (misma regla que el pase de escena:
            // `baseColor × mat.albedo`). El material entero (texturas + escalares) viaja con el grupo.
            const Haruka::MaterialComponent* mat = obj->material ? obj->material.get() : nullptr;
            const glm::vec3 instColor = baseColor * (mat ? mat->albedo : glm::vec3(1.0f));

            // Pieza de construcción → al pase INSTANCIADO (no draw individual). Mismo model/color que
            // llevaría por-objeto, así se ve idéntica; el cull de arriba ya se aplicó.
            if (command.instanced && command.kind == Haruka::RenderKind::Model && !obj->modelPath.empty()) {
                Haruka::InstanceDataFloat inst;
                inst.model = AppInternal::getTransformMatrix(*obj, _camera->position);
                inst.color = glm::vec4(instColor, 1.0f);
                inst.scale = glm::vec3(obj->scale);
                const std::string key = obj->modelPath + "|" + instancedMaterialKey(mat);
                ConstInstGroup& g = constGroups[key];
                if (g.instances.empty()) {
                    g.meshKey = obj->modelPath; g.primitive = -1;
                    g.mat = resolveInstancedMaterial(mat);
                }
                g.instances.push_back(inst);
                continue;
            }
            // Primitiva de construcción/edificio → al mismo pase instanciado, agrupada por (primitiva,
            // material): un edificio tejido son cientos o miles de cubos —las tablas/sillares del muro—
            // y por-objeto serían cientos o miles de draws.
            if (command.instanced && command.kind == Haruka::RenderKind::Primitive) {
                Haruka::InstanceDataFloat inst;
                inst.model = AppInternal::getTransformMatrix(*obj, _camera->position);
                inst.color = glm::vec4(instColor, 1.0f);
                inst.scale = glm::vec3(obj->scale);
                const std::string key = std::string("prim:") + std::to_string((int)command.primitive) +
                                        "|" + instancedMaterialKey(mat);
                ConstInstGroup& g = constGroups[key];
                if (g.instances.empty()) {
                    g.meshKey.clear(); g.primitive = (int)command.primitive;
                    g.mat = resolveInstancedMaterial(mat);
                }
                g.instances.push_back(inst);
                continue;
            }

            PerObjectUBOData objData{};
            objData.model                    = AppInternal::getTransformMatrix(*obj, _camera->position);
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
            // w = 0: normal, 1: procedural terrain, 2: emissive star (no diffuse shading)
            objData.planetCenterAndFlag      = glm::vec4(0.0f, 0.0f, 0.0f, isStar ? 2.0f : 0.0f);
            // Sin MaterialComponent: dieléctrico mate sin texturas → el aspecto de siempre.
            objData.materialPBR              = glm::vec4(0.0f, 0.5f, 1.0f, 0.0f);
            objData.materialEmission         = glm::vec4(0.0f);

            // --- MATERIAL del objeto (albedo/normal/metallic/roughness/ao) -------------------
            // Los slots del MaterialComponent son los MISMOS que hornea el editor de node graph
            // del IDE, así que un grafo bakeado se ve aquí sin más paso intermedio.
            if (obj->material) {
                const Haruka::MaterialComponent& mat = *obj->material;
                // El color del material TIÑE el del objeto: un material no debe borrar el tinte
                // por objeto (dos props del mismo material se distinguen por su color).
                objData.baseColorAndPlanetRadius = glm::vec4(baseColor * mat.albedo, 1.0f);
                objData.materialEmission         = glm::vec4(mat.emission, 0.0f);

                int texMask = 0;
                auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
                    auto it = mat.textures.find(key);
                    if (it == mat.textures.end() || it->second.empty()) return;
                    RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
                    if (!RHI::valid(tex)) return;
                    sceneCtx->bindTexture(unit, tex);
                    texMask |= bit;
                };
                bindSlot("albedo",    0, kTexAlbedo);
                bindSlot("normal",    1, kTexNormal);
                bindSlot("metallic",  2, kTexMetallic);
                bindSlot("roughness", 3, kTexRoughness);
                bindSlot("ao",        4, kTexAO);

                objData.materialPBR = glm::vec4(mat.metallic, mat.roughness, mat.ao, (float)texMask);
            }

            uboDev->updateBuffer(m_uboPerObjectH, 0, sizeof(PerObjectUBOData), &objData);

            switch (command.kind) {
                case Haruka::RenderKind::Model: {
                    Model* model = AppInternal::getOrLoadModelCached(obj->modelPath);
                    if (!model) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    // La carga PEREZOSA de arriba crea mallas, y Mesh::setupMesh() deja el VAO a 0
                    // → desbindearía el VAO del pipeline JUSTO antes de dibujar (glDrawElements con
                    // VAO 0 = GL_INVALID_OPERATION). Rebindeamos el pipeline después de cargar.
                    // Desaparece cuando la creación de recursos deje de tocar el estado global.
                    sceneCtx->bindPipeline(m_scenePSO);
                    model->drawRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += model->getVertexCount();
                    renderedTriangles += model->getTriangleCount();
                    ++totalDrawCalls;
                    totalVertices  += model->getVertexCount();
                    totalTriangles += model->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::MeshComponent: {
                    if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    sceneCtx->bindPipeline(m_scenePSO);   // ver nota del caso Model (estado GL)
                    obj->meshRenderer->renderRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += obj->meshRenderer->getResidentVertexCount();
                    renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                    ++totalDrawCalls;
                    totalVertices  += obj->meshRenderer->getResidentVertexCount();
                    totalTriangles += obj->meshRenderer->getResidentTriangleCount();
                    break;
                }
                case Haruka::RenderKind::Primitive: {
                    SimpleMesh* primitiveMesh = AppInternal::getPrimitiveMesh(command.primitive);
                    if (!primitiveMesh) break;
                    if (!RHI::valid(m_scenePSO)) break;
                    sceneCtx->bindPipeline(m_scenePSO);   // ver nota del caso Model (estado GL)
                    primitiveMesh->drawRHI(*sceneCtx);
                    ++renderedDrawCalls;
                    renderedVertices  += primitiveMesh->getVertexCount();
                    renderedTriangles += primitiveMesh->getTriangleCount();
                    ++totalDrawCalls;
                    totalVertices  += primitiveMesh->getVertexCount();
                    totalTriangles += primitiveMesh->getTriangleCount();
                    break;
                }
                case Haruka::RenderKind::None:
                    break;
            }
        }
        } // scene.objects.draw

        // --- Pase INSTANCIADO de piezas de construcción (mismo render pass/depth que la escena) ---
        if (!constGroups.empty() && RHI::valid(m_scenePSO)) {
            HARUKA_PROFILE("scene.construction.instanced");
            if (!RHI::valid(m_constInstPSO)) {                    // PSO instanciado (una vez)
                using V = Haruka::Renderer::Vertex;
                const std::string vs = Shader::baseDir() + "shaders/construction_inst.vert";
                const std::string fs = Shader::baseDir() + "shaders/construction_inst.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vs.c_str();
                pd.fragmentPath = fs.c_str();
                pd.vertexLayout.strides    = { (uint32_t)sizeof(V) };            // binding 0 = Vertex
                pd.vertexLayout.attributes = {
                    { 0, (uint32_t)offsetof(V, Position), RHI::Format::RGB32F, 0 },
                    { 1, (uint32_t)offsetof(V, Normal),   RHI::Format::RGB32F, 0 },
                    { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F,  0 },   // UV → material per-pixel
                };
                Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                pd.depth.test   = true;  pd.depth.write = true;
                pd.blend.enable = false;
                pd.cull         = RHI::CullMode::Back;
                m_constInstPSO  = uboDev->createPipeline(pd);
            }
            if (RHI::valid(m_constInstPSO)) {
                if (!_instancing) { _instancing = std::make_unique<GPUInstancing>(); _instancing->init(20000); }
                if (!RHI::valid(m_constParamsUBO)) {              // UBO del material del grupo (binding 6)
                    m_constParamsUBO = uboDev->createBuffer(RHI::BufferUsage::Uniform,
                                                            sizeof(ConstParams), nullptr,
                                                            RHI::BufferMemory::Dynamic);
                }
                sceneCtx->bindPipeline(m_constInstPSO);
                sceneCtx->bindUniformBuffer(0, m_uboPerFrameH);   // view/proj + luces (mismo UBO que la escena)

                // El buffer de instancias es FIJO (`addInstance` descarta al pasarse), y un edificio
                // tejido puede traer miles de piezas → se dibuja POR LOTES del tamaño del buffer, así
                // no desaparece ninguna. `fill` sube el lote [off, off+n) y el llamador dispara el draw.
                const std::size_t cap = (std::size_t)std::max(1, _instancing->getMaxInstances());
                auto fillChunk = [&](const std::vector<Haruka::InstanceDataFloat>& src, std::size_t off) {
                    const std::size_t n = std::min(cap, src.size() - off);
                    _instancing->clear();
                    for (std::size_t i = 0; i < n; ++i)
                        _instancing->addInstance(src[off + i].model, src[off + i].color, src[off + i].scale);
                };
                // Enlaza las texturas del material del grupo + su UBO (escalares/máscara) → per-pixel.
                auto bindGroupMaterial = [&](const ConstGroupMaterial& gm) {
                    // Mismo motivo que en el pase de props: en Vulkan un slot sin atar es basura.
                    sceneCtx->bindTexture(0, RHI::valid(gm.albedo)    ? gm.albedo
                                             : fallbackTexture(FallbackTex::White));
                    sceneCtx->bindTexture(1, RHI::valid(gm.normal)    ? gm.normal
                                             : fallbackTexture(FallbackTex::Normal));
                    sceneCtx->bindTexture(2, RHI::valid(gm.metallic)  ? gm.metallic
                                             : fallbackTexture(FallbackTex::Metallic));
                    sceneCtx->bindTexture(3, RHI::valid(gm.roughness) ? gm.roughness
                                             : fallbackTexture(FallbackTex::White));
                    sceneCtx->bindTexture(4, RHI::valid(gm.ao)        ? gm.ao
                                             : fallbackTexture(FallbackTex::White));
                    ConstParams cp;
                    cp.matPBR = glm::vec4(gm.metallicS, gm.roughnessS, gm.aoS, (float)gm.mask);
                    uboDev->updateBuffer(m_constParamsUBO, 0, sizeof(cp), &cp);
                    sceneCtx->bindUniformBuffer(6, m_constParamsUBO);
                };

                for (auto& kv : constGroups) {
                    ConstInstGroup& g = kv.second;
                    if (g.instances.empty()) continue;
                    // Malla del modelo (pieza de obra) o primitiva de edificio (cubos tejidos).
                    Model* model = g.primitive < 0 ? AppInternal::getOrLoadModelCached(g.meshKey) : nullptr;
                    SimpleMesh* pm = g.primitive >= 0
                                     ? AppInternal::getPrimitiveMesh((Haruka::PrimitiveType)g.primitive) : nullptr;
                    if (g.primitive < 0 && !model) continue;
                    if (g.primitive >= 0 && (!pm || pm->getIndexCount() == 0 ||
                                             !RHI::valid(pm->vertexBuffer()) || !RHI::valid(pm->indexBuffer()))) continue;
                    // La carga/creación PEREZOSA de arriba crea recursos GL y deja el VAO a 0 → habría
                    // desbindeado el VAO del pipeline justo antes del draw (GL_INVALID_OPERATION).
                    // Re-bindear tras resolver la malla. Misma trampa que documenta el pase de escena.
                    sceneCtx->bindPipeline(m_constInstPSO);
                    bindGroupMaterial(g.mat);
                    for (std::size_t off = 0; off < g.instances.size(); off += cap) {
                        const std::size_t n = std::min(cap, g.instances.size() - off);
                        fillChunk(g.instances, off);
                        if (model) {
                            model->drawInstancedRHI(*sceneCtx, *_instancing, 1);   // 1 draw por malla del modelo
                            ++renderedDrawCalls;
                            // Piezas de OBRA (casas/puentes): contaban 0 en el panel — se cuentan
                            // aquí por instancia del lote (TOTAL == RENDERED: sin cull, todas entran).
                            renderedVertices  += model->getVertexCount() * (int)n;
                            renderedTriangles += model->getTriangleCount() * (int)n;
                            ++totalDrawCalls;
                            totalVertices  += model->getVertexCount() * (int)n;
                            totalTriangles += model->getTriangleCount() * (int)n;
                        } else {
                            sceneCtx->bindVertexBuffer(pm->vertexBuffer(), 0);
                            sceneCtx->bindIndexBuffer(pm->indexBuffer());
                            _instancing->render(sceneCtx, (uint32_t)pm->getIndexCount(), 1);
                            ++renderedDrawCalls;
                            renderedVertices  += pm->getVertexCount()   * (int)n;
                            renderedTriangles += pm->getTriangleCount() * (int)n;
                            ++totalDrawCalls;
                            totalVertices  += pm->getVertexCount()   * (int)n;
                            totalTriangles += pm->getTriangleCount() * (int)n;
                        }
                    }
                }
                sceneCtx->bindPipeline(m_scenePSO);   // restaura el PSO de escena (el cierre del pass lo asume)
            }
        }

        // --- Pase INSTANCIADO de PROPS del mundo (mismo render pass/depth que la escena) -------
        // Objetos del scatter global (árboles/rocas/…) con un PROTOTIPO compartido por tipo: una
        // malla (con color de vértice + UV) y un material del node graph (albedo/normal/…), y por
        // instancia solo {transform, tinte, escala, estado}. 1 draw por prototipo vía GPUInstancing.
        if (m_propScatterEnabled && m_propRegistry.prototypeCount() > 0 && RHI::valid(m_scenePSO)) {
            HARUKA_PROFILE("scene.prop.instanced");
            if (!RHI::valid(m_propInstPSO)) {                 // PSO instanciado de props (una vez)
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
                    // loc 11 = parte del esqueleto. (La 10 la ocupa la máscara de roturas, que va
                    // en el stream de INSTANCIA — ver appendInstanceLayout.)
                    { 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 },
                };
                Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                pd.depth.test   = true;  pd.depth.write = true;
                pd.blend.enable = false;
                pd.cull         = RHI::CullMode::Back;
                m_propInstPSO  = uboDev->createPipeline(pd);
            }

            // UBO del pase (binding 6): viento + tiempo + escalares del material del prototipo.
            // UNO POR PROTOTIPO: en Vulkan los draws se graban y se ejecutan después, así que un
            // buffer compartido haría que todos leyeran el último material escrito. Ver la nota en
            // `application.h`.
            if ((int)m_propParamsUBOs.size() < m_propRegistry.prototypeCount()) {
                const size_t was = m_propParamsUBOs.size();
                m_propParamsUBOs.resize((size_t)m_propRegistry.prototypeCount());
                for (size_t i = was; i < m_propParamsUBOs.size(); ++i)
                    m_propParamsUBOs[i] = uboDev->createBuffer(RHI::BufferUsage::Uniform,
                                                              sizeof(PropParams), nullptr,
                                                              RHI::BufferMemory::Dynamic);
            }

            // Viento del CLIMA (m/s, marco del observador) y tiempo del cielo — el mismo que mueve
            // las nubes (sky.frag). Sin WorldSystem: viento en calma.
            glm::vec3 windWorld(0.0f, 0.0f, 0.0f);
            if (_worldSystem)
                windWorld = _worldSystem->getWind(glm::dvec3(_camera->position));
            static const auto s_propT0 = std::chrono::steady_clock::now();
            const float propTime = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - s_propT0).count();

            if (RHI::valid(m_propInstPSO)) {
                if (!_instancing) { _instancing = std::make_unique<GPUInstancing>(); _instancing->init(20000); }
                sceneCtx->bindPipeline(m_propInstPSO);
                sceneCtx->bindUniformBuffer(0, m_uboPerFrameH);    // view/proj + luces (mismo UBO que la escena)

                // Frustum culling: base de la cámara + medias tangentes de los FOV. Se calculan UNA vez
                // por frame y las instancias se testean contra los 4 planos laterales con una esfera
                // (radio ~ tamaño del prop) → no se sube al buffer lo que no puede verse. Sin esto, en
                // órbita o mirando lejos se dibujaban las 20000 del buffer aunque estuvieran fuera de
                // pantalla (y el tope del buffer robaba instancias visibles a las no visibles).
                const glm::vec3 camF  = _camera->getFront();
                const glm::vec3 camU  = _camera->getUp();
                const glm::vec3 camR  = glm::normalize(glm::cross(camF, camU));
                const float tanV = std::tan(glm::radians(_camera->zoom * 0.5f));
                const float tanH = tanV * std::max(aspect, 0.01f);
                // Test de esfera contra los 4 planos laterales del frustum. El radio cubre el prop
                // entero (árbol ~6.5 m, casa/roca menos; ×scale) con margen para no hacer "pop" en el
                // borde de pantalla. Lo usan el draw y el snapshot de debug (mismo criterio).
                //
                // Además, CULL POR TAMAÑO EN PANTALLA (sub-pixel): en órbita un árbol (radio ~6.5 m)
                // cubre <1 px y se dibujaba igual, saturando el buffer con ~20000 instancias invisibles.
                // El tamaño angular del prop (radio/distancia, en fracción del medio FOV) → píxeles →
                // si cubre < kMinPropPixels se culla. Esto NO es un gate por altura: un prop GIGANTE
                // (radio de km) crece su tamaño angular con su radio y se ve desde cualquier órbita.
                const float vpPx = std::max((float)height, 1.0f);
                const float kMinPropPixels = 1.0f;   // radio ≥ ~1 px en pantalla (diámetro ~2 px)
                // Umbrales del LOD, en RADIO EN PÍXELES — no en metros. Es lo correcto: lo que decide
                // cuánta geometría hace falta es el tamaño en pantalla, así que un prop grande conserva
                // detalle más lejos y uno pequeño lo pierde antes, sin tablas por tipo. Y sale de la
                // misma división que ya hacía el descarte sub-píxel: el LOD no cuesta ni una operación.
                //
                //   ≥ 90 px de radio -> nivel 0 (464 tris)   ~232 m para un árbol de 8 m
                //   ≥ 25 px          -> nivel 1 (130 tris)   ~835 m
                //   resto            -> nivel 2 ( 40 tris)   hasta los 6 km del scatter
                const float kLodPx0 = 90.0f, kLodPx1 = 25.0f;
                auto propCull = [&](const glm::vec3& posF, float radius, int& outLod) -> uint8_t {
                    outLod = 0;
                    const float dist = glm::length(posF);
                    if (dist <= 1e-3f) return 0;
                    const float fwd = glm::dot(posF, camF);
                    if (fwd < -radius) return 1;                      // detrás de la cámara
                    if (std::abs(glm::dot(posF, camR)) > fwd * tanH + radius) return 1;
                    if (std::abs(glm::dot(posF, camU)) > fwd * tanV + radius) return 1;
                    const float px = (radius / dist) / tanV * vpPx;
                    if (px < kMinPropPixels) return 2;                // sub-pixel
                    outLod = (px >= kLodPx0) ? 0 : (px >= kLodPx1 ? 1 : 2);
                    return 0;
                };

                // Centro/radio del planeta y cámara: se calculan UNA vez (antes `getActivePlanet` se
                // llamaba dentro del bucle, por instancia).
                const glm::dvec3 camD = glm::dvec3(_camera->position);
                glm::dvec3 planetC; double planetR = 6371000.0;
                const bool hasPlanet = _planetarySystem && _planetarySystem->getActivePlanet(planetC, planetR);
                if (!hasPlanet) planetC = glm::dvec3(0.0);
                const glm::vec3 up(0.0f, 1.0f, 0.0f);
                const int protoCount = m_propRegistry.prototypeCount();

                // Buckets de instancias VISIBLES por prototipo (reusados entre frames). Antes el
                // bucle era O(prototipos × instancias): por cada prototipo se recorría el registro
                // ENTERO filtrando `io.prototype != pi`. Con ~61k celdas del scatter y ~4 prototipos
                // eso son ~160k iteraciones/frame con cull+mat4 cada una — el grueso de los ~39 ms
                // que medía `scene.prop.instanced`. Ahora se barre UNA vez (O(N)) y se agrupa.
                static std::vector<std::vector<Haruka::InstanceDataFloat>> s_propBuckets;
                static std::vector<int> s_aliveCounts;
                static std::vector<int> s_drawnCounts;
                // Un bucket por (prototipo, nivel): el draw instanciado necesita que todas las
                // instancias de un comando compartan malla, y la malla ahora depende del nivel.
                const int kLods = PropPrototypeGpu::kLods;
                if ((int)s_propBuckets.size() != protoCount * kLods) {
                    s_propBuckets.resize((size_t)(protoCount * kLods));
                    s_aliveCounts.resize((size_t)protoCount);
                    s_drawnCounts.resize((size_t)protoCount);
                }
                for (int pi = 0; pi < protoCount; ++pi) {
                    for (int l = 0; l < kLods; ++l) s_propBuckets[(size_t)(pi * kLods + l)].clear();
                    s_aliveCounts[(size_t)pi] = 0;
                    s_drawnCounts[(size_t)pi] = 0;
                }

                // Snapshot de debug para la jerarquía del editor: se reconstruye cada 10 frames
                // (~6 Hz) — 40k entradas/instancia con asignaciones por frame cuestan ~5 ms y el
                // árbol es una vista de debug; a 6 Hz amortiza a <1 ms. El motor sigue marcando
                // hasPerPixel en el draw, solo que con la misma cadencia que el snapshot.
                static int s_propDbgFrame = 0;
                const bool snapshotDbg = m_propDebugEnabled && (++s_propDbgFrame % 10 == 1);
                if (snapshotDbg) {
                    m_propScatterDebug.clear();
                    m_propScatterDebug.reserve((size_t)protoCount);
                    for (int pi = 0; pi < protoCount; ++pi) {
                        PropPrototypeDebug dbg;
                        dbg.name = m_propRegistry.prototype(pi).name;
                        m_propScatterDebug.push_back(std::move(dbg));
                    }
                }

                // --- 1. Barrido ÚNICO sobre las instancias (O(N)): cull + transform + agrupar por
                // prototipo (y, si el editor lo pide, el snapshot de debug en línea con el MISMO
                // cull, sin repetirlo).
                const int bufCap = _instancing->getMaxInstances();
                const glm::dvec3 camToCenter = planetC - camD;   // una resta dvec por frame, no por instancia
                std::vector<int> aliveSeenPerProto((size_t)protoCount, 0);

                // SONDA: el pase cuesta 21,4 ms de los 33 del frame y barre 36 621 instancias para
                // dibujar 6 792. "Optimizar el culling" sin saber si el tiempo se va en CULLAR, en
                // construir matrices o en subir buffers es tirar a ciegas — y son tres remedios
                // distintos. Se mide una vez cada 2 s para no pagar el reloj por frame.
                using PClock = std::chrono::steady_clock;
                const auto tSweep0 = PClock::now();
                size_t nSwept = 0, nKept = 0;

                for (const auto& io : m_propRegistry.instances()) {
                    ++nSwept;
                    const int pi = io.prototype;
                    if (pi < 0 || pi >= protoCount) continue;
                    const bool alive = io.state == (uint32_t)Haruka::InstancedObjectState::Alive;

                    uint8_t cull = 0;
                    if (alive) {
                        ++s_aliveCounts[(size_t)pi];
                        const glm::vec3 posF = glm::vec3(
                            camToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
                        int lod = 0;
                        cull = propCull(posF, io.scale * 8.0f, lod);
                        if (cull == 0) {
                            glm::mat4 m = glm::translate(glm::mat4(1.0f), posF);
                            m *= glm::mat4_cast(glm::rotation(up, io.dir));
                            // ⚠️ El giro de yaw va sobre el eje LOCAL, no sobre `io.dir`.
                            // `glm::rotate(m, a, eje)` POST-multiplica: el eje se interpreta en el
                            // espacio de `m`, que ya lleva la rotación up→dir. Pasarle `io.dir`
                            // giraba en torno a `R·dir`, un eje que NO es la vertical del prop —
                            // así que el yaw (aleatorio por celda, 0..2π) TUMBABA el objeto tanto
                            // como lo giraba. Era el "los árboles están tumbados": los de yaw≈0
                            // salían de pie y el resto caídos en ángulos arbitrarios.
                            // El eje local que la rotación manda a `dir` es +Y (el `up` de arriba),
                            // así que girar sobre +Y en local ES girar sobre la vertical en mundo.
                            m = glm::rotate(m, io.yaw, up);
                            m = glm::scale(m, glm::vec3(io.scale));
                            Haruka::InstanceDataFloat inst;
                            inst.model = m;
                            inst.color = glm::vec4(io.tint, 1.0f);
                            inst.scale = glm::vec3(io.scale);
                            inst.breakMask = (float)io.breakMask;   // ramas que ya no están
                            s_propBuckets[(size_t)(pi * kLods + lod)].push_back(inst);
                            ++nKept;
                        }
                    }

                    if (snapshotDbg) {
                        PropPrototypeDebug& dbg = m_propScatterDebug[(size_t)pi];
                        ++dbg.totalInstances;
                        PropInstanceDebug idbg;
                        idbg.seed = io.seed;
                        idbg.state = io.state;
                        idbg.cullReason = cull;
                        idbg.culled = alive && cull != 0;
                        idbg.rendered = alive && cull == 0 && aliveSeenPerProto[(size_t)pi] < bufCap;
                        if (idbg.rendered) ++dbg.renderedInstances;
                        if (alive && cull == 0) ++aliveSeenPerProto[(size_t)pi];
                        dbg.instances.push_back(idbg);
                    }
                }
                {
                    static double s_lastPropLog = -1e9;
                    const double ms = std::chrono::duration<double, std::milli>(
                                          PClock::now() - tSweep0).count();
                    m_diagClock += 0.0;   // (el reloj ya lo avanza el diagnóstico de estrellas)
                    if (m_diagClock - s_lastPropLog > 2.0) {
                        s_lastPropLog = m_diagClock;
                        HARUKA_LOGDIAG("PropCost",
                            "barrido+cull+matrices: %.2f ms para %zu instancias -> %zu dibujadas "
                            "(%.0f ns/instancia)", ms, nSwept, nKept,
                            nSwept ? ms * 1e6 / (double)nSwept : 0.0);
                    }
                }

                for (int pi = 0; pi < protoCount; ++pi) {
                    const Haruka::InstancedPrototype& proto = m_propRegistry.prototype(pi);
                    if (proto.name.empty()) continue;

                    // 1. Malla prototipo (una vez): horneada con la seed del tipo. La cache la clavea
                    //    el NOMBRE del prototipo (el "tipo" del scatter): árbol por TreeMeshNode, y roca/
                    //    casa por los bakes procedurales de prop_mesh.h (misma paridad determinista).
                    auto itMesh = m_propProtoMesh.find(proto.name);
                    if (itMesh == m_propProtoMesh.end()) {
                        PropPrototypeGpu pg;
                        // ESCALERA DE DETALLE. Los números salen de medir la malla, no a ojo:
                        //   detalle 1.00 -> 464 triángulos   (la de siempre, bit a bit)
                        //   detalle 0.60 -> 130
                        //   detalle 0.45 ->  40
                        // Saltos de ~3,5× por nivel. Con la distribución real del scatter (el área crece
                        // con r², así que casi todo está lejos) esto lleva los 3,24 M de triángulos de
                        // árbol a ~0,27 M: **-92 %**.
                        static constexpr float kLodDetail[PropPrototypeGpu::kLods] = { 1.0f, 0.60f, 0.45f };
                        bool anyLevel = false;
                        for (int lod = 0; lod < PropPrototypeGpu::kLods; ++lod) {
                        Haruka::Tools::ProcGraph::TreeMeshData tm;
                        // ⚠️ La clasificación y los parámetros del árbol viven en `prop_collider.h`,
                        // no aquí. El COLLIDER se deriva del mismo esqueleto que esta malla: si el
                        // bake decidiera por su cuenta qué es un árbol y con qué altura, chocarías
                        // con un árbol distinto del que ves.
                        const Haruka::Planet::PropShapeKind shape =
                            Haruka::Planet::propShapeKind(proto.name);
                        if (shape == Haruka::Planet::PropShapeKind::Rock) {
                            tm = Haruka::Tools::ProcGraph::bakeRockMesh((int)proto.meshSeed, 1.0f, 0.72f);
                        } else if (shape == Haruka::Planet::PropShapeKind::House) {
                            tm = Haruka::Tools::ProcGraph::bakeHouseMesh((int)proto.meshSeed, 1.0f);
                        } else {
                            const Haruka::Planet::PropTreeParams tp = Haruka::Planet::propTreeParams();
                            Haruka::Tools::ProcGraph::Graph g;
                            int tree = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
                                (int)proto.meshSeed, tp.height, tp.trunkR, tp.canopy, tp.segments,
                                kLodDetail[lod]);
                            g.compile();
                            if (!Haruka::Tools::ProcGraph::bakeTreeMesh(g, tree, tm)) {
                                // bakeTreeMesh devuelve la malla en `out` solo si el nodo es válido;
                                // el dato queda vacío si falla el dynamic_cast.
                            }
                        }
                        if (!tm.positions.empty()) {
                            // Sube a GPU con el layout del prototipo (Pos/Normal/Color/Uv).
                            std::vector<PropVertex> verts;
                            verts.reserve(tm.positions.size());
                            for (size_t vi = 0; vi < tm.positions.size(); ++vi) {
                                PropVertex pv;
                                pv.pos    = tm.positions[vi];
                                pv.normal = (vi < tm.normals.size()) ? tm.normals[vi]
                                                                      : glm::vec3(0, 1, 0);
                                pv.color  = (vi < tm.colors.size()) ? tm.colors[vi]
                                                                    : glm::vec3(1.0f);
                                pv.uv     = (vi < tm.uvs.size()) ? tm.uvs[vi] : glm::vec2(0.0f);
                                // Roca/casa no traen partId (su bake no tiene esqueleto): parte 0,
                                // que es la que se lleva el prop entero al romperlo.
                                pv.partId = (vi < tm.partId.size()) ? (float)tm.partId[vi] : 0.0f;
                                verts.push_back(pv);
                            }
                            pg.vbo[lod] = uboDev->createBuffer(RHI::BufferUsage::Vertex,
                                                          verts.size() * sizeof(PropVertex), verts.data());
                            pg.ebo[lod] = uboDev->createBuffer(RHI::BufferUsage::Index,
                                                          tm.indices.size() * sizeof(unsigned int),
                                                          tm.indices.data());
                            pg.indexCount[lod]  = (uint32_t)tm.indices.size();
                            pg.vertexCount[lod] = (uint32_t)verts.size();
                            anyLevel = true;
                        }
                        }   // fin del bucle de niveles
                        if (anyLevel) {
                            // ROCA Y CASA no tienen mando de detalle (126 y 22 triángulos: el 3 % y el
                            // 0,01 % del frame), así que sus tres niveles salen idénticos. Se dejan: el
                            // desperdicio son unos KB de VRAM y el camino de dibujo queda uniforme, sin
                            // un caso especial por tipo de prop.
                            //
                            // 2. Material PER-PIXEL del prototipo (una vez, COMPARTIDO por los niveles:
                            //    es el mismo prototipo, solo cambia la densidad de la malla).
                            PropMaterialBake pm = bakePropPrototypeMaterial(proto.meshSeed);
                            pg.albedo    = pm.albedo;
                            pg.normal    = pm.normal;
                            pg.metallic  = pm.metallic;
                            pg.roughness = pm.roughness;
                            pg.ao        = pm.ao;
                            pg.metallicS = pm.metallicS;
                            pg.roughnessS = pm.roughnessS;
                            pg.aoS       = pm.aoS;
                            pg.mask      = pm.mask;
                            itMesh = m_propProtoMesh.emplace(proto.name, pg).first;
                        }
                    }
                    if (itMesh == m_propProtoMesh.end()) continue;   // bake fallido
                    const PropPrototypeGpu& pg = itMesh->second;
                    if (!RHI::valid(pg.vbo[0]) || pg.indexCount[0] == 0) continue;

                    // Per-pixel REAL del prototipo (mask != 0 → texturas enlazadas en el draw). Se marca
                    // ANTES del `continue` de bucket vacío: un prototipo sin instancias visibles este
                    // frame no deja de tener material per-pixel en la jerarquía del editor.
                    if (snapshotDbg)
                        m_propScatterDebug[(size_t)pi].hasPerPixel = pg.mask != 0u;

                    // Stats TOTALES (lo que el frame podía dibujar sin cull) con la malla de nivel 0:
                    // es el número con el que comparar el ahorro del LOD.
                    const int aliveN = s_aliveCounts[(size_t)pi];
                    totalVertices  += pg.vertexCount[0] * aliveN;
                    totalTriangles += (pg.indexCount[0] / 3) * aliveN;
                    ++totalDrawCalls;

                    // Material PER-PIXEL del PROTOTIPO: compartido por los niveles (es el mismo
                    // prototipo, solo cambia la densidad), así que el UBO y las texturas se preparan UNA
                    // vez fuera del bucle de niveles.
                    PropParams pp{};
                    pp.wind = windWorld;
                    pp.time = propTime;
                    pp.matPBR = glm::vec4(pg.metallicS, pg.roughnessS, pg.aoS, (float)pg.mask);

                    // UN DRAW POR NIVEL. El instancing sigue intacto —cada comando lleva todas las
                    // instancias de su nivel— y lo que baja es la geometría por instancia. Son 3 draws
                    // por prototipo en el peor caso: nada al lado de los millones de triángulos que
                    // ahorra. Los niveles vacíos no cuestan draw.
                    int drawnTotal = 0;
                    for (int lod = 0; lod < kLods; ++lod) {
                        auto& bucket = s_propBuckets[(size_t)(pi * kLods + lod)];
                        if (bucket.empty()) continue;
                        if (!RHI::valid(pg.vbo[lod]) || pg.indexCount[lod] == 0) continue;
                        _instancing->setInstances(bucket);
                        const int drawnN = _instancing->getInstanceCount();
                        drawnTotal += drawnN;
                        renderedVertices  += pg.vertexCount[lod] * drawnN;
                        renderedTriangles += (pg.indexCount[lod] / 3) * drawnN;

                        sceneCtx->bindPipeline(m_propInstPSO);   // re-bind (creación perezosa de buffers)
                        sceneCtx->bindVertexBuffer(pg.vbo[lod], 0);
                        sceneCtx->bindIndexBuffer(pg.ebo[lod]);
                        // ⚠️ SE ATAN LOS CINCO SLOTS SIEMPRE. Antes solo se ataba el que existía, y
                        // en OpenGL eso vale: un sampler sin atar lee negro y el guard `hasTex()`
                        // del shader lo ignora. En Vulkan el descriptor queda INDEFINIDO y
                        // muestrearlo es basura — los props salían GRISES, sin el verde de la copa
                        // ni el marrón del tronco (se vio en RenderDoc como `u_matMetallic` sin
                        // recurso). El relleno es una textura blanca de 1x1; el shader sigue
                        // ignorándola por la máscara, así que no cambia el resultado en GL.
                        sceneCtx->bindTexture(0, RHI::valid(pg.albedo)    ? pg.albedo
                                                 : fallbackTexture(FallbackTex::White));
                        sceneCtx->bindTexture(1, RHI::valid(pg.normal)    ? pg.normal
                                                 : fallbackTexture(FallbackTex::Normal));
                        sceneCtx->bindTexture(2, RHI::valid(pg.metallic)  ? pg.metallic
                                                 : fallbackTexture(FallbackTex::Metallic));
                        sceneCtx->bindTexture(3, RHI::valid(pg.roughness) ? pg.roughness
                                                 : fallbackTexture(FallbackTex::White));
                        sceneCtx->bindTexture(4, RHI::valid(pg.ao)        ? pg.ao
                                                 : fallbackTexture(FallbackTex::White));
                        const RHI::BufferHandle ppUbo =
                            (pi < (int)m_propParamsUBOs.size()) ? m_propParamsUBOs[(size_t)pi]
                                                                : RHI::BufferHandle{};
                        if (RHI::valid(ppUbo)) {
                            uboDev->updateBuffer(ppUbo, 0, sizeof(pp), &pp);
                            sceneCtx->bindUniformBuffer(6, ppUbo);
                        }
                        _instancing->render(sceneCtx, pg.indexCount[lod], 1);
                        ++renderedDrawCalls;
                    }
                    s_drawnCounts[(size_t)pi] = drawnTotal;
                }

                // [DIAGNÓSTICO TEMPORAL] cada 120 frames: N instancias, y por prototipo vivos /
                // en bucket / dibujados. Pínchalo: si N=0 el registro está vacío (scatter), si
                // vivos>0 y bucket=0 el cull lo está tumbando todo, si bucket>0 pero no hay draw
                // el problema está en el bucle de dibujo/PSO. Quitar tras localizar.
                {
                    static int s_diagFrame = 0;
                    if (++s_diagFrame % 120 == 1) {
                        std::string line;
                        char tmp[96];
                        for (int di = 0; di < protoCount; ++di) {
                            int b0 = (int)s_propBuckets[(size_t)(di * kLods + 0)].size();
                            int b1 = (int)s_propBuckets[(size_t)(di * kLods + 1)].size();
                            int b2 = (int)s_propBuckets[(size_t)(di * kLods + 2)].size();
                            std::snprintf(tmp, sizeof(tmp), " p%d:%d/[%d+%d+%d]/%d", di,
                                          s_aliveCounts[(size_t)di], b0, b1, b2,
                                          s_drawnCounts[(size_t)di]);
                            line += tmp;
                        }
                        HARUKA_LOGDIAG("PropDiag", "protoCount=%d instances=%zu%s", protoCount,
                                    m_propRegistry.instances().size(), line.c_str());
                    }
                }
                sceneCtx->bindPipeline(m_scenePSO);   // restaura el PSO de escena (el cierre lo asume)
            }
        }

        if (RHI::valid(m_scenePSO)) sceneCtx->endRenderPass();

        // Terrain streaming: update LOD + render planet chunks
        if (_planetarySystem) {
            {
                HARUKA_PROFILE("planetary.update(LOD+stream)");
                _planetarySystem->syncFromScene(*_currentScene);
                _planetarySystem->update(deltaTime > 0.0f ? deltaTime : 0.016, glm::dvec3(_camera->position));
            }

            // --- SHADOW PASS (sun): the game's casters (props) project depth into a
            // depth map; the terrain receives it. All camera-relative (camera at origin).
            glm::mat4 lightSpace(1.0f);
            bool shadowsOn = false;
            // Read the LIVE setting (config panel + save): 0=Off,1=Low,2=Medium,3=High.
            int sq = (int)Haruka::SettingsManager::get().graphics().shadowQuality;
            if (_camera && _gameInterface && _gameInterface->onRenderShadow && sq > 0) {
                unsigned res = (sq == 1) ? 1024u : (sq == 2) ? 2048u : 4096u; // resolution per quality
                if (!_shadow || _shadow->shadowWidth != res) _shadow = std::make_unique<Shadow>(res, res);
                glm::vec3 sunDir = glm::normalize(frameData.sunDirection); // hacia el sol
                glm::vec3 lup = (std::abs(sunDir.y) < 0.95f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
                // TIGHT area around the player → more resolution per metre (sharpness).
                // ±42 m: at 4096 ≈ 2 cm/texel (was ±75 → 3.7 cm, pixels were visible).
                const float D = 90.0f, S = 42.0f;
                glm::mat4 lView = glm::lookAt(sunDir * D, glm::vec3(0.0f), lup); // luz desde el sol
                glm::mat4 lProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
                lightSpace = lProj * lView;

                RHI::ClearValues shadowClear;
                shadowClear.clearColor = false; shadowClear.clearDepth = true; shadowClear.depth = 1.0f;
                { HARUKA_PROFILE("shadow.pass");
                sceneCtx->beginRenderPass(_shadow->pass(), shadowClear);
                _gameInterface->onRenderShadow(lightSpace, glm::vec3(_camera->position));
                renderPropShadows(sceneCtx, lightSpace);   // ← los props del motor, ver abajo
                shadowsOn = true;
                sceneCtx->endRenderPass();
                }
            }

            // --- MÁSCARA DE EXPOSICIÓN AL CIELO (pase CENITAL) -----------------------------------
            // El mismo truco que el shadow map, pero con la "luz" en el CÉNIT: lo que aparece aquí
            // es lo que tienes ENCIMA. Es la pieza que el depth buffer no puede dar — la gota que cae
            // entre tu cara y el tejado no tiene nada DELANTE, así que el depth no la descarta; lo que
            // la descarta es saber que hay tejado ARRIBA. Una sola máscara sirve para las tres cosas:
            // nada de lluvia bajo cubierto, la silueta SECA bajo cualquier collider, y dónde cuaja la
            // nieve. Reusa el hook `onRenderShadow` del juego (props, edificios, piezas colocadas):
            // no hay que registrar casters nuevos, ya son los mismos.
            // Se hace SOLO cuando precipita: sin lluvia ni nieve no la lee nadie y sería un pase de
            // profundidad regalado cada frame.
            m_skyMaskOn = false;
            if (_camera && _gameInterface && _gameInterface->onRenderShadow &&
                (m_rainAmount > 0.01f || m_snowAmount > 0.01f || m_groundWetness > 0.01f)) {
                if (!m_skyMask) m_skyMask = std::make_unique<Shadow>(kSkyMaskRes, kSkyMaskRes);
                const glm::dvec3 camD2 = glm::dvec3(_camera->position);
                glm::dvec3 upD(0, 1, 0);
                {                   glm::dvec3 pc2; double pr2;
                    if (_planetarySystem->getActivePlanet(pc2, pr2)) {
                        const glm::dvec3 r = camD2 - pc2; const double rl = glm::length(r);
                        if (rl > 1e-9) upD = r / rl;
                    } }
                const glm::vec3 zen = glm::vec3(upD);
                glm::vec3 zup = (std::abs(zen.y) < 0.95f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                // Ortográfica MIRANDO HACIA ABAJO desde muy arriba (D), cubriendo ±S alrededor del
                // jugador. D generoso: un tejado o la copa de un árbol a 40 m tiene que entrar, o
                // "sin cubierta" saldría falso justo bajo lo que más tapa.
                const float D = 140.0f, S = (float)kSkyMaskExtentM;
                const glm::mat4 zView = glm::lookAt(zen * D, glm::vec3(0.0f), zup);
                const glm::mat4 zProj = glm::ortho(-S, S, -S, S, 1.0f, 2.0f * D);
                m_skySpace = zProj * zView;

                {
                RHI::ClearValues maskClear;
                maskClear.clearColor = false; maskClear.clearDepth = true; maskClear.depth = 1.0f;
                { HARUKA_PROFILE("skymask.pass");
                sceneCtx->beginRenderPass(m_skyMask->pass(), maskClear);
                _gameInterface->onRenderShadow(m_skySpace, glm::vec3(_camera->position));
                m_skyMaskOn = true;
                sceneCtx->endRenderPass();
                }
                }
            }

            // El SUELO ya sabe que está mojado (`m_groundWetness` se integra arriba) pero hasta ahora
            // eso no llegaba al shader del terreno: el suelo se mojaba en la simulación y no cambiaba
            // de aspecto. Se reparte aquí, DESPUÉS del pase de máscara cenital, porque la silueta seca
            // bajo los árboles sale de esa misma textura — la que la lluvia ya usa por gota.
            if (_planetarySystem) {
                _planetarySystem->setGroundWet(
                    m_groundWetness, m_snowAccum,
                    (m_skyMaskOn && m_skyMask) ? RHI::device()->getDepthTexture(m_skyMask->pass())
                                               : RHI::TextureHandle{},
                    m_skySpace);
            }

            // Restore scene framebuffer + viewport before drawing SimplePlanet/game objects
            // (shadow/sky-mask passes changed both and GL's endRenderPass is a no-op).
            {
                RHI::ClearValues restore; restore.clearColor = false; restore.clearDepth = false;
                sceneCtx->beginRenderPass(sceneTargetPass, restore);
                if (!RHI::valid(sceneTargetPass))
                    sceneCtx->setViewport(0, 0, (int)width, (int)height);
            }

            // --- SimplePlanet terrain rendering ---
            {
                HARUKA_PROFILE("simple_planet.draw");
                const float aspectC = (height > 0u) ? (float)width / (float)height : 1.0f;
                const glm::mat4 projC    = _camera->getProjectionMatrix(aspectC);
                const glm::mat4 viewC    = _camera->getViewMatrix();
                // El suelo cercano que viene de la FÍSICA se entrega ANTES de dibujar: el planeta lo
                // pinta con su propio material (mismo `biome.frag`, mismos bindings), así que lo
                // único distinto respecto al clipmap es de dónde salen los vértices.
                updateNearGroundRing();
                for (size_t i = 0; i < _planetarySystem->getSimplePlanetCount(); ++i) {
                    const auto& sp = _planetarySystem->getSimplePlanet(i);
                    _planetarySystem->renderSimplePlanet(sp.name,
                        glm::dvec3(_camera->position), projC, viewC);
                }
                // ALAMBRE DE LA MALLA DE COLISIÓN, justo DESPUÉS del terreno y con su misma
                // profundidad: así el alambre queda oculto por el relieve donde pasa por debajo y
                // visible donde pasa por encima. Es lo que hace la comparación legible.
                // La vista va SIN traslación porque los vértices ya llegan relativos a la cámara.
                renderCollisionWireframe(sceneCtx, projC * glm::mat4(glm::mat3(viewC)));
            }

            // Stats del TERRENO (malla base + clipmap + agua): se suman a los contadores del frame
            // y se guardan para el panel. TOTAL == RENDERED aquí (el planeta dibuja todo lo que
            // tiene; solo el agua culla las caras tras el planeta, que es lo que refleja el desglose).
            m_terrainStats = _planetarySystem->getTerrainRenderStats();
            totalDrawCalls  += m_terrainStats.drawCalls;
            totalVertices   += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices);
            totalTriangles  += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles);
            renderedDrawCalls += m_terrainStats.drawCalls;
            renderedVertices  += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices);
            renderedTriangles += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles);
        }

        _iRenderedDrawCalls = renderedDrawCalls;
        _iRenderedVertices  = renderedVertices;
        _iRenderedTriangles = renderedTriangles;
        _iTotalDrawCalls    = totalDrawCalls;
        _iTotalVertices     = totalVertices;
        _iTotalTriangles    = totalTriangles;
    }

    if (_gameInterface && _gameInterface->onRenderWorld && _camera) {
        const float      aspect  = (height > 0u) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const glm::mat4  view    = _camera->getViewMatrix();
        const glm::mat4  proj    = _camera->getProjectionMatrix(aspect);
        const glm::vec3  camPos  = glm::vec3(_camera->position);
        _gameInterface->onRenderWorld(view, proj, camPos);
    }

    // --- PRECIPITACIÓN (lluvia / nieve) — EN EL MUNDO, no sobre la imagen ---------------------
    // Va AQUÍ y no al final del frame a propósito: este es el último punto en el que el target de
    // escena y su DEPTH siguen bindeados. Dibujada después del composite, la gota no tendría contra
    // qué probarse y volveríamos al filtro de pantalla que se acaba de quitar.
    if (m_rainAmount > 0.01f || m_snowAmount > 0.01f) {
        HARUKA_PROFILE("precip.draw");
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        glm::dvec3 upD(0, 1, 0);
        // Altura de la BASE DE LA NUBE respecto a la cámara. Sin planeta activo no hay cota de la que
        // colgar la nube, así que se deja sin techo (el valor por defecto de Params).
        float cloudBaseRelCam = 1e9f;
        if (_planetarySystem) {
            glm::dvec3 pc; double pr;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                const glm::dvec3 r = camD - pc; const double rl = glm::length(r);
                if (rl > 1e-9) upD = r / rl;
                // `cloudBaseM` es cota sobre el nivel del mar, igual que la que consume el pase
                // volumétrico (`CloudParams::slab.x`): se le resta la de la cámara para dejarla en la
                // MISMA referencia que usa el shader de gotas (altura sobre la cámara).
                const Haruka::WeatherSample wxP = _planetarySystem->weatherAt(camD);
                cloudBaseRelCam = (float)(wxP.cloudBaseM - (rl - pr));
            }
        }
        Haruka::PrecipitationRenderer::Params pp;
        pp.cloudBaseRelCamM = cloudBaseRelCam;
        pp.cameraPos   = camD;
        pp.up          = glm::vec3(upD);
        pp.lookDir     = _camera->getFront();
        pp.wind        = m_windVec;
        pp.sunColor    = _worldSystem ? _worldSystem->getDominantLightColor(camD) : glm::vec3(1.0f);
        pp.snow        = (m_snowAmount > 0.01f);
        pp.amount      = pp.snow ? m_snowAmount : m_rainAmount;
        // TODO (fase C): `skyVisibility` saldrá de la máscara cenital — hoy 1.0, así que bajo un
        // tejado la gota que está ENTRE el tejado y tu cara sigue viéndose (el depth solo descarta
        // lo que tiene algo DELANTE). Es lo único de "no llueve bajo cubierto" que falta.
        pp.skyVisibility = 1.0f;
        pp.timeSeconds = _planetarySystem ? _planetarySystem->simulationTime() : 0.0;
        pp.viewportHeightPx = (int)(m_postActive ? renderH : height);
        if (m_skyMaskOn && m_skyMask) {          // fase C: cada gota mira si tiene algo ENCIMA
            pp.skyMask  = RHI::device()->getDepthTexture(m_skyMask->pass());
            pp.skySpace = m_skySpace;
        }
        m_precip.render(pp);

    }

#ifdef HARUKA_MOD_FLUIDS
    // --- HITO 2: RÍOS/LAGOS + SPLASH (FluidHost) -------------------------------------------
    // Dibuja en el pase de escena DESPUÉS del planeta y la precipitación: el color/depth de la
    // escena ya están completos, que es lo que el modo superficie del fluido necesita para sembrar
    // refracción y oclusión (blit de escena). El binding 0 se restaura al UBO per-frame del
    // engine (el planeta lo pisa con su SimplePlanetUBO; softbody/fluido esperan PerFrameData).
    if (!_fluidHost) _fluidHost = std::make_unique<Haruka::FluidHost>();
    if (_fluidHost && _planetarySystem && _camera) {
        HARUKA_PROFILE("fluid.setup+render(rios/lagos)");
        glm::dvec3 pc; double pr;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                _fluidHost->planetCenter = pc;
                _fluidHost->planetRadius = pr;
                _fluidHost->hasPlanet    = true;
            }
            _fluidHost->terrainHeightFn = [this](const glm::dvec3& wp) -> double {
                return _planetarySystem->sampleTerrainHeight(wp);
            };
            const auto* tp = _planetarySystem->activeTerrestrial();
            if (tp) {
                _fluidHost->tidalSeaAlongUpFn = [this](const glm::dvec3& dir) -> double {
                    if (!_planetarySystem) return 0.0;
                    const auto* t = _planetarySystem->activeTerrestrial();
                    if (!t) return 0.0;
                    const auto& bodies = t->tidalBodies();
                    if (bodies.empty()) return 0.0;
                    glm::dvec3 pc2; double R;
                    if (!_planetarySystem->getActivePlanet(pc2, R)) return 0.0;
                    const double scale  = (R * R / 9.8) * 15.0;   // misma escala que planet.cpp:2686
                    const double floorD = R * 0.5;                 // mismo suelo que uTide.z
                    double h = 0.0;
                    for (const auto& b : bodies) {
                        const glm::dvec3 surf = dir * R;
                        const glm::dvec3 to   = b.posCenter - surf;
                        const double D = glm::max(glm::length(to), floorD);
                        const double L = glm::max(glm::length(b.posCenter), 1.0);
                        const double c = glm::dot(dir, b.posCenter) / L;
                        h += b.gm * (3.0 * c * c - 1.0) / (2.0 * D * D * D);
                    }
                    return h * scale;
                };
            } else {
                _fluidHost->tidalSeaAlongUpFn = nullptr;
            }
            // El agua interior la DIBUJA EL MAR: el fluido publica su campo y el planeta lo consume.
            // Sin este enganche la sim sigue corriendo (cauces, charcos, lluvia) pero no se ve nada.
            _fluidHost->publishInlandWater =
                [this](const std::vector<float>& surf, int n, const glm::vec3& anchorRelEye,
                       const glm::vec3& tan, const glm::vec3& bit, const glm::vec3& up, float span) {
                    if (!_planetarySystem) return;
                    if (auto* tp = _planetarySystem->activeTerrestrialMut())
                        tp->setInlandWater(&surf, n, anchorRelEye, tan, bit, up, span);
                };
            _fluidHost->rainPerSec = (m_rainAmount > 0.01f) ? m_rainAmount : 0.0f;
            _fluidHost->update(deltaTime > 0.0f ? deltaTime : 0.016f,
                               glm::dvec3(_camera->position));

            int fw = (int)width, fh = (int)height;
            if (m_postActive && _postScene) { fw = m_postW; fh = m_postH; }
            // Binding 0 = PerFrameData (m_uboPerFrameH): las binds de UBO son por contexto GL
            // (globales al mismo device), así que un contexto local recién creado lo deja puesto
            // para los draws de softbody/fluido de abajo.
            if (RHI::Device* dev = RHI::device()) {
                RHI::Context* fluidCtx = dev->beginFrame();
                if (fluidCtx) fluidCtx->bindUniformBuffer(0, m_uboPerFrameH);
            }
            _fluidHost->render(glm::dvec3(_camera->position), fw, fh, sceneTargetPass);
        }
#endif

    // --- NUBES VOLUMÉTRICAS ------------------------------------------------
    //
    // ⚠️ VA AQUÍ, DESPUÉS DE TODA LA GEOMETRÍA, y ese es el cambio de fondo. El cúmulo se pintaba
    // en `sky.frag`, que es un pase de FONDO (sin depth, antes que la escena): eso lo convertía en
    // un telón que cualquier objeto tapaba y, sobre todo, **sin interior** — no se podía atravesar
    // una nube porque un fondo no tiene dentro. Dibujándolo aquí, con la profundidad de la escena
    // a mano, la nube es un cuerpo que se recorre: si la cámara está dentro, el rayo arranca dentro
    // y la pérdida de visibilidad sale de la integración, sin ningún efecto de pantalla añadido.
    // A/B SIN RECOMPILAR: `HARUKA_CLOUD_VOL=0` apaga el pase volumétrico. Con él apagado, `sky.frag`
    // vuelve a pintar el cúmulo PLANO como fondo (ver su conmutador `u_planet.w`). Si el cielo se ve
    // IGUAL con y sin, lo que estás mirando no es el pase volumétrico: son el cirro (8 km) y el
    // altocúmulo (4 km), que son planos A PROPÓSITO —están tan alto que nunca se cruzan— y solo el
    // CÚMULO es volumétrico.
    if (m_volumetricClouds && !cloudVolumetricOff() && _camera && _planetarySystem && _worldSystem) {
        HARUKA_PROFILE("scene.clouds.volumetric");
        RHI::Device* dev = RHI::device();
        glm::dvec3 pcD; double prD = 0.0;
        if (dev && _planetarySystem->getActivePlanet(pcD, prD) && prD > 0.0) {
            const int cw = (int)(m_postActive ? renderW : width);
            const int ch = (int)(m_postActive ? renderH : height);

            if (!RHI::valid(m_cloudPSO)) {
                const std::string vs = Shader::baseDir() + "shaders/cloud_vol.vert";
                const std::string fs = Shader::baseDir() + "shaders/cloud_vol.frag";
                RHI::PipelineDesc pd;
                pd.vertexPath   = vs.c_str();
                pd.fragmentPath = fs.c_str();
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                // NO escribe profundidad: la nube es medio participativo, no una superficie. Si
                // escribiera z, lo que se dibujara después quedaría recortado por una "cáscara" que
                // no existe.
                pd.depth.test   = false;  pd.depth.write = false;
                pd.blend.enable = true;   // se compone sobre la escena con su propia opacidad
                m_cloudPSO = dev->createPipeline(pd);
                m_cloudUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(CloudParams),
                                               nullptr, RHI::BufferMemory::Dynamic);
                HARUKA_LOGI("Clouds", "pase volumetrico: %s",
                            RHI::valid(m_cloudPSO) ? "ok" : "FALLO (se sigue viendo el cielo de fondo)");
            }

            // Copia del DEPTH de la escena. ⚠️ No se puede samplear la profundidad del MISMO target
            // al que se dibuja (realimentación); el fluido ya resolvía esto igual, con `blitDepth`.
            // ⚠️ EL FORMATO DE PROFUNDIDAD LO DICTA LA FUENTE, no este bloque. Un blit de profundidad
            // exige formatos IDÉNTICOS. Con el post-proceso apagado, `sceneTargetPass` es inválido y la
            // escena vive en el BACKBUFFER, cuya profundidad la elige SDL (punto fijo, no D32F): pedir
            // D32F aquí daba `GL_INVALID_OPERATION: Depth formats do not match`, el blit se caía en
            // silencio y esta textura se quedaba SIN ESCRIBIR — o sea las nubes ocluyendo contra basura,
            // que es el "se ven a través del terreno" reportado.
            const RHI::Format srcDepthFmt = RHI::valid(sceneTargetPass) ? RHI::Format::D32F
                                                                        : dev->backbufferDepthFormat();
            if (RHI::valid(m_cloudPSO) && (!RHI::valid(m_cloudDepthRT) ||
                                           m_cloudDepthW != cw || m_cloudDepthH != ch ||
                                           m_cloudDepthFmt != srcDepthFmt)) {
                if (RHI::valid(m_cloudDepthRT)) dev->destroy(m_cloudDepthRT);
                RHI::RenderTargetDesc dd;
                dd.width = cw; dd.height = ch;
                dd.colorFormats = { RHI::Format::R32F };   // no se usa; el target necesita un color
                dd.colorFilter  = RHI::Filter::Nearest;
                dd.hasDepth     = true;
                dd.depthFormat  = srcDepthFmt;
                m_cloudDepthRT = dev->createRenderTarget(dd);
                m_cloudDepthW = cw; m_cloudDepthH = ch; m_cloudDepthFmt = srcDepthFmt;
                // Una línea, solo al (re)crear: dice si el blit de profundidad puede ser legal. Si
                // aquí sale un formato y la escena tiene otro, las nubes ocluyen contra basura y el
                // síntoma es "se ven a través del terreno" — sin más aviso que un GL_INVALID_OPERATION
                // fácil de pasar por alto.
                HARUKA_LOGI("Clouds", "copia de profundidad %dx%d · formato %s (escena: %s)",
                            cw, ch, srcDepthFmt == RHI::Format::D32F ? "D32F" : "D24S8",
                            RHI::valid(sceneTargetPass) ? "target de post" : "BACKBUFFER");
            }

            if (RHI::valid(m_cloudPSO) && RHI::valid(m_cloudDepthRT)) {
                const glm::dvec3 camD = glm::dvec3(_camera->position);
                const Haruka::WeatherSample wx = _planetarySystem->weatherAt(camD);
                float coverC = wx.cloudCover;
                float precC  = wx.precip;
                if (m_rainOverride >= 0.0f) { precC = m_rainOverride; coverC = glm::max(coverC, precC); }

                // SONDA: sin esto, "las nubes no son volumétricas" no se puede separar de "no hay
                // nubes". El pase solo dibuja con cobertura > 1 %, así que un cielo con cirros y sin
                // cúmulos se ve plano y el pase ni se ejecuta. Solo al cambiar de forma apreciable.
                {
                    static float s_lastCov = -1.0f;
                    if (std::abs(coverC - s_lastCov) > 0.05f) {
                        s_lastCov = coverC;
                        HARUKA_LOGI("Clouds", "cobertura=%.2f · precip=%.2f · base=%.0f m · techo=%.0f m"
                                    " · pasos=%d -> el cumulo volumetrico %s",
                                    coverC, precC, wx.cloudBaseM, wx.cloudTopM, kCloudSteps,
                                    coverC > 0.01f ? "SE DIBUJA" : "NO se dibuja (cielo plano: cirro+altocumulo)");
                    }
                }
                if (coverC > 0.01f) {
                    glm::dvec3 upD = camD - pcD; const double ul = glm::length(upD);
                    upD = (ul > 1e-9) ? upD / ul : glm::dvec3(0, 1, 0);
                    const float altEye = (float)(ul - prD);

                    const float aspectC = (ch > 0) ? (float)cw / (float)ch : 1.0f;
                    const glm::mat4 proj = _camera->getProjectionMatrix(aspectC);
                    const glm::mat4 view = _camera->getViewMatrix();

                    CloudParams cp{};
                    // ⚠️ SOLO ROTACIÓN, y es la clave del corte contra la escena. Toda la geometría
                    // se rasteriza cámara-relativa (`frameData.view = mat4(mat3(view))`), así que
                    // deshacer el depth con la vista COMPLETA situaba el punto a la distancia
                    // ABSOLUTA de la cámara al origen —millones de metros en un planeta—, el rayo no
                    // se recortaba nunca y la nube se dibujaba POR ENCIMA DEL TERRENO.
                    cp.invViewProjRot = glm::inverse(proj * glm::mat4(glm::mat3(view)));
                    // El centro del planeta RELATIVO a la cámara: en doble y solo el resultado a
                    // float. La resta directa de magnitudes de ~6,37e6 en float se cuantiza a medio
                    // metro, que sobre la base de la nube se ve como que la capa "respira".
                    cp.planetC = glm::vec4(glm::vec3(pcD - camD), (float)prD);
                    cp.slab    = glm::vec4(wx.cloudBaseM, wx.cloudTopM, coverC, precC);

                    const glm::vec3 sunDir = _worldSystem->getDominantLightDirection(camD);
                    const glm::vec3 sunCol = _worldSystem->getDominantLightColor(camD);
                    const float sunElev = (float)glm::dot(glm::dvec3(sunDir), upD);
                    cp.sun      = glm::vec4(sunDir, sunElev);
                    cp.sunColor = glm::vec4(sunCol, glm::smoothstep(-0.12f, 0.18f, sunElev));

                    static const auto s_cloudT0 = std::chrono::steady_clock::now();
                    const float ct = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - s_cloudT0).count();
                    glm::vec3 eastC = glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(upD)));
                    if (!std::isfinite(eastC.x)) eastC = glm::vec3(1, 0, 0);
                    const glm::vec3 northC = glm::cross(glm::vec3(upD), eastC);
                    cp.wind = glm::vec4(glm::dot(m_windVec, eastC) * ct * 0.0008f,
                                        glm::dot(m_windVec, northC) * ct * 0.0008f,
                                        ct, altEye);

                    // Atmósfera: en órbita no hay nube que atravesar (y el fondo ya se encarga).
                    const float atmoC = 1.0f - glm::smoothstep(0.0f, (float)(prD * 0.02), altEye);
                    // Pasos: pocos. Esto corre a pantalla completa y lo que hace falta es que el
                    // ESPESOR exista, no que la integral sea exacta.
                    // z = ESCALA del campo horizontal, en 1/metros; su inversa es el ANCHO del
                    // cúmulo. Sale de `WeatherSystem` y no de un literal aquí porque forma pareja
                    // con el GROSOR que calcula `sampleAt`: los dos juntos deciden si la nube se lee
                    // como cuerpo o como lámina, y el test `cloud_shape` fija su relación.
                    cp.misc = glm::vec4(atmoC, (float)kCloudSteps,
                                        Haruka::WeatherSystem::kFieldScale, kCloudExtinction);

                    dev->updateBuffer(m_cloudUBO, 0, sizeof(cp), &cp);

                    RHI::Context* cctx = dev->beginFrame();
                    if (cctx) {
                        cctx->blitDepth(sceneTargetPass, m_cloudDepthRT, cw, ch);
                        RHI::ClearValues keep;
                        keep.clearColor = false; keep.clearDepth = false;
                        cctx->beginRenderPass(sceneTargetPass, keep);
                        if (!RHI::valid(sceneTargetPass)) cctx->setViewport(0, 0, cw, ch);
                        cctx->bindPipeline(m_cloudPSO);
                        cctx->bindUniformBuffer(5, m_cloudUBO);
                        cctx->bindTexture(0, dev->getDepthTexture(m_cloudDepthRT));
                        cctx->draw(3);
                        cctx->endRenderPass();
                    }
                }
            }
        }
    }

    // --- Post-processing composite -----------------------------------------
    // Resolve the offscreen scene target to the screen: upscales the render-scaled
    // image and applies the present-time effects (FXAA, and later bloom). With
    // FXAA off and scale 1.0 the scene never took this path (m_postActive false).
    // === PASE 2 MIGRADO A PSO/Context: el present/composite (FXAA + bloom + upscale). ===
    if (m_postActive && _postScene) {
        HARUKA_PROFILE("post.composite(FXAA+bloom+upscale)");
        RHI::Device* dev = RHI::device();
        if (m_quadBuf.id == 0) setupQuad();   // VBO del quad (el VAO lo aporta el PSO)

        // Pipeline horneado 1 vez. Dibuja a PANTALLA → sin depth ni blend.
        if (!RHI::valid(m_presentPSO)) {
            const std::string vsPath = Shader::baseDir() + "shaders/screenquad.vert";
            const std::string fsPath = Shader::baseDir() + "shaders/post_present.frag";
            RHI::PipelineDesc pd;
            pd.vertexPath              = vsPath.c_str();
            pd.fragmentPath            = fsPath.c_str();
            pd.vertexLayout.strides     = { (uint32_t)(4 * sizeof(float)) };
            pd.vertexLayout.attributes = {
                { 0, 0,                 RHI::Format::RG32F },   // aPos
                { 1, 2 * sizeof(float), RHI::Format::RG32F },   // aTexCoords
            };
            pd.topology     = RHI::PrimitiveTopology::TriangleStrip;
            pd.depth.test   = false;  pd.depth.write = false;
            pd.blend.enable = false;
            m_presentPSO = dev->createPipeline(pd);
            m_presentUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PresentParams), nullptr,
                                             RHI::BufferMemory::Dynamic);
        }

        if (RHI::valid(m_presentPSO)) {
            // El bloom se construye ANTES (rebindea FBOs/viewport) y devuelve su handle.
            RHI::TextureHandle bloomTex;
            if (wantBloom) bloomTex = renderBloom(_postScene->getColorTextureHandle());
            const bool haveBloom = wantBloom && RHI::valid(bloomTex);

            PresentParams p{};
            p.texel[0]      = 1.0f / (float)m_postW;
            p.texel[1]      = 1.0f / (float)m_postH;
            p.bloomStrength = gpost.bloomStrength;
            p.fxaa          = wantFXAA  ? 1.0f : 0.0f;
            p.bloom         = haveBloom ? 1.0f : 0.0f;
            dev->updateBuffer(m_presentUBO, 0, sizeof(p), &p);

            RHI::Context* ctx = dev->beginFrame();
            // ⚠️ SE LIMPIA, aunque el quad cubra la pantalla entera. Antes ponía `clearColor=false`
            // razonando justo eso, y en OpenGL es correcto. En Vulkan NO: desde que las render
            // passes tienen variante `loadOp LOAD` (necesaria para que el motor pueda reabrir un
            // pase sin borrar lo dibujado), no limpiar significa CONSERVAR lo que había en ESA
            // imagen del swapchain — que con 3 imágenes rotando es el frame de hace 2 o 3 turnos.
            // Cualquier píxel que el quad no cubra exactamente enseña ese frame viejo: el "frame
            // fantasma". Limpiar aquí no cuesta nada (el quad lo sobreescribe igual) y elimina la
            // posibilidad por construcción.
            RHI::ClearValues clr;
            clr.clearColor = true;
            clr.color[0] = 0.0f; clr.color[1] = 0.0f; clr.color[2] = 0.0f; clr.color[3] = 1.0f;
            clr.clearDepth = true; clr.depth = 0.0f;      // reversed-Z: 0 = lejano

            ctx->beginRenderPass(RHI::RenderPassHandle{}, clr);  // id 0 = backbuffer (pantalla)
            ctx->setViewport(0, 0, (int)width, (int)height);      // el pass a pantalla NO fija viewport
            ctx->bindPipeline(m_presentPSO);
            ctx->bindVertexBuffer(m_quadBuf);
            ctx->bindUniformBuffer(3, m_presentUBO);
            ctx->bindTexture(0, _postScene->getColorTextureHandle());
            // El sampler u_bloomTex se lee SIEMPRE en el shader aunque u_bloom sea 0; bindea algo
            // válido para no dejar la unidad 1 con basura de un pase anterior.
            ctx->bindTexture(1, haveBloom ? bloomTex : _postScene->getColorTextureHandle());
            ctx->draw(4);
            ctx->endRenderPass();
        }

        // RHI migration: GL state transitions removed
    }

    // (Aquí estaba el pase de LLUVIA DE PANTALLA: un triángulo fullscreen con rayas, mezclado sobre
    //  la imagen ya compuesta. Se ha ELIMINADO. Ese pase no tenía profundidad ni posición, así que
    //  llovía dentro de las cuevas, detrás de los muros y bajo los tejados — el "sale donde sea" que
    //  reportó el autor. La precipitación se dibuja ahora COMO GEOMETRÍA dentro del pase de escena
    //  (`PrecipitationRenderer`, justo tras onRenderWorld), sometida al depth buffer del mundo.)

    // Devolver la PANTALLA como framebuffer atado antes de salir. En GL `endRenderPass` es un
    // no-op, así que al terminar la escena sigue atado el FBO del último pase — con `_editorTarget`,
    // el RenderTarget del viewport. Quien nos llama (el IDE) dibuja su ImGui justo después dando por
    // hecho que el framebuffer es la pantalla: sin esto, el UI ENTERO se pinta dentro del target del
    // viewport y el backbuffer no lo escribe nadie, así que la ventana alterna entre dos buffers
    // viejos → parpadea entera.
    // El VIEWPORT lo deja el llamante: en modo editor `_window` es null y `width`/`height` son los
    // del viewport, no los de la ventana — fijarlo aquí sería adivinar. El IDE lo hace con el tamaño
    // real justo antes de su `glClear`.
    if (_editorTarget) {
        if (RHI::Device* dev = RHI::device()) {
            RHI::Context* ctx = dev->beginFrame();
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);   // id 0 = backbuffer (pantalla)
            ctx->endRenderPass();
        }
    }

    // Clean screenshot (no HUD): capture here, after the 3D scene is composited to
    // the screen but BEFORE ImGui draws over it.
    if (!_editorTarget) captureScreenshotIfPending((int)width, (int)height);

    if (_imguiCallback) {
        _imguiCallback();
    }

    // GPU timer disabled during RHI migration
}

// Implementación del campo ESFÉRICO que pide el scatter global: el planeta activo (el mismo que
// pinta el terreno). `TerrestrialPlanet` ya expone la MISMA ecología/cota por dirección; aquí solo
// se redirige por dirección (el parche plano del placer viejo quedó obsoleto).
namespace {
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
} // namespace

// Scatter GLOBAL de props (Todo 5): rellena `m_propRegistry` desde el campo del planeta activo.
// Cada planeta registra sus PROTOTIPOS una vez (uno por `mesh` de sus capas); las INSTANCIAS se
// re-enumeran por demanda con `scatterPropsNear` cuando la cámara cruza un tramo (determinismo por
// CELDA MUNDIAL → el mismo árbol siempre en el mismo sitio, sin parche tangente).
// ── SOMBRAS DE LOS PROPS DEL MOTOR ──────────────────────────────────────────────────────────────
//
// El pase de sombras solo llamaba a `onRenderShadow`, el hook del JUEGO, que dibuja los recursos del
// juego. Los props del scatter los posee y dibuja el MOTOR (`m_propRegistry` + `m_propInstPSO`), así
// que no entraban en el mapa: ni un árbol proyectaba sombra. Quien dibuja la geometría tiene que
// meterla en el mapa de sombras.
//
// ⚠️ CULLING POR LA CAJA DE LA LUZ, no por la cámara. El volumen de sombra es un cubo de ±42 m
// CENTRADO EN LA CÁMARA, así que incluye lo que está DETRÁS de ella — y con el sol a tu espalda son
// justo esos props los que proyectan sombra hacia lo que ves. Reusar los buckets ya culleados por la
// cámara habría perdido esas sombras. Un test de distancia es suficiente y es barato: la caja es
// pequeña, así que de las ~3500 instancias solo unas decenas entran.
// ── EL SUELO CERCANO, DIBUJADO DESDE LA GEOMETRÍA DE LA COLISIÓN ────────────────────────────────
//
// Paso 3 del plan de paridad. El render deja de ser una SEGUNDA evaluación del terreno dentro de
// ±256 m y pasa a dibujar los mismos vértices y los mismos triángulos que Jolt colisiona.
//
// Por qué no basta con afinar el shader: los cuatro nodos de cada quad ya coinciden exactamente
// (`clipmap_vertex_lattice`, 0 de 59 785 fuera de la retícula), pero un quad no es plano y hay que
// partirlo en dos triángulos. Jolt parte por `(i,j)→(i+1,j+1)`; el teselador de la GPU parte por
// donde quiera, porque el spec de OpenGL NO lo fija para `layout(quads, ...)`. La diferencia en el
// centro del quad, medida sobre el terreno real, son 2-4 cm bajo los pies. Mientras el teselador
// esté en el bucle esa disparidad no se puede cerrar, solo acotar.
//
// De momento OPT-IN (`HARUKA_NEAR_RING=1`) y dibujando encima del clipmap sin tocarlo: revertir es
// quitar un draw call. Sombreado mínimo a propósito — lo que hay que juzgar aquí es si la geometría
// coincide con el alambre verde, y un material completo lo escondería.
void Application::updateNearGroundRing() {
#ifdef HARUKA_MOD_PHYSICS
    // ── ENCENDIDO POR DEFECTO, con salida de emergencia ─────────────────────────────────────────
    //
    // Nació opt-in (`HARUKA_NEAR_RING=1`) mientras se validaba que la geometría de la colisión servía
    // para dibujar. Ya está validado, así que el camino por defecto es éste: dentro de ±192 m el
    // suelo que se dibuja son los MISMOS vértices y los MISMOS triángulos que se pisan, y el
    // teselador queda fuera del bucle.
    //
    // ⚠️ La variable se queda, ahora para APAGARLO (`HARUKA_NEAR_RING=0`). No es simetría cosmética:
    // si esto falla, el clipmap ya ha abierto su hueco de 192 m y el fallo se ve como un AGUJERO en
    // el planeta alrededor del jugador. Poder volver al camino de siempre sin recompilar ni revertir
    // es lo que separa un bug molesto de una partida imposible.
    static int s_env = -1;
    if (s_env < 0) {
        const char* e = std::getenv("HARUKA_NEAR_RING");
        s_env = (e && e[0] == '0') ? 0 : 1;
        if (s_env == 0) HARUKA_LOGW("NearGround", "APAGADO por HARUKA_NEAR_RING=0: el suelo cercano "
                                    "vuelve a salir del teselador y la paridad deja de ser exacta");
    }
    if (s_env != 1 || !_physicsEngine || !_camera || !_planetarySystem) return;

    Physics::PhysicsEngine::NearGroundRing ring;
    if (!_physicsEngine->getNearGroundRing(ring)) return;

    // La GEOMETRÍA solo se re-sube cuando la física reconstruye el anillo (al saltar el anclaje del
    // clipmap): son ~16 600 vértices. El ancla relativa al ojo, en cambio, va CADA FRAME.
    static std::vector<glm::vec3>  s_pos;
    static std::vector<uint32_t>   s_tris;
    const bool fresh = (ring.revision != m_nearRingRev);
    if (fresh) {
        m_nearRingRev    = ring.revision;
        m_nearRingAnchor = ring.anchor;
        // ⚠️ RELATIVO AL ANCLA, en float. En coordenadas de mundo (~6,37e6 m) un float tiene ~0,5 m
        // de resolución: el suelo temblaría medio metro y este parche inventaría la disparidad que
        // viene a eliminar. La resta va en DOUBLE y solo el resultado baja a float.
        s_pos.clear(); s_pos.reserve(ring.verts.size());
        uint64_t ck = 1469598103934665603ull;
        for (const auto& v : ring.verts) {
            const glm::vec3 f(v - ring.anchor);
            s_pos.push_back(f);
            for (int c = 0; c < 3; ++c) { uint32_t b; std::memcpy(&b, &f[c], 4); ck = (ck ^ b) * 1099511628211ull; }
        }
        s_tris = ring.tris;
        // El checksum tiene que coincidir con el que imprime `Physics` al publicar. "Son los mismos
        // bytes" es comprobable, así que se comprueba en vez de suponerse.
        HARUKA_LOGI("NearGround", "anillo recibido: rev %llu · %zu vert · %zu tris · celda %.1f m · "
                    "alcance +-%.0f m · checksum %016llx",
                    (unsigned long long)ring.revision, s_pos.size(), s_tris.size() / 3,
                    ring.cell, ring.extent, (unsigned long long)ck);
    }
    if (s_pos.size() < 3) return;

    // Traslación ancla→ojo POR FRAME y en double: los vértices son relativos al ancla (fija entre
    // reconstrucciones) y esto los lleva al ojo (móvil). Sin esta mitad el parche quedaría pegado a
    // la cámara y se deslizaría sobre el terreno al caminar.
    const glm::vec3 anchorRelEye = glm::vec3(m_nearRingAnchor - glm::dvec3(_camera->position));
    glm::dvec3 upD(0.0, 1.0, 0.0);
    glm::dvec3 pc; double pr = 0.0;
    if (_planetarySystem->getActivePlanet(pc, pr) && glm::length(m_nearRingAnchor - pc) > 1e-9)
        upD = glm::normalize(m_nearRingAnchor - pc);

    // (Aquí hubo una sonda que avisaba si el ancla del clipmap y la del anillo caían en celdas
    // distintas. Cazó el fallo —hasta 4,00 m de desfase, un escalón entero— y con él se corrigió el
    // anclaje: el clipmap ahora se ancla DONDE EL ANILLO, así que coinciden por construcción y la
    // sonda medía un invariante que ya no puede romperse. Se retira en vez de dejarla avisando: un
    // instrumento que sigue dando números después de que su objeto desaparezca es peor que ninguno.)

    if (Haruka::Planet::TerrestrialPlanet* tp = _planetarySystem->activeTerrestrialMut())
        tp->setNearGroundRing(fresh ? &s_pos : nullptr, fresh ? &s_tris : nullptr,
                              anchorRelEye, glm::vec3(upD), m_nearRingAnchor);
#endif
}

// ── ALAMBRE DE LA MALLA DE COLISIÓN ─────────────────────────────────────────────────────────────
void Application::setCollisionWireframe(bool on) {
    m_collisionWireOn = on;
#ifdef HARUKA_MOD_PHYSICS
    if (_physicsEngine) _physicsEngine->setCollisionMeshDebug(on);
#endif
    m_dbgLineRev = ~0ull;   // fuerza re-subir los buffers al encender
}

void Application::renderCollisionWireframe(RHI::Context* ctx, const glm::mat4& viewProjRotOnly) {
#ifdef HARUKA_MOD_PHYSICS
    // Se puede encender sin tocar el juego: `HARUKA_COLLISION_WIRE=1`. Es una herramienta de
    // diagnóstico y el juego no tiene por qué exponerla en su UI para poder usarla.
    {
        static int s_env = -1;
        if (s_env < 0) {
            const char* e = std::getenv("HARUKA_COLLISION_WIRE");
            s_env = (e && e[0] == '1') ? 1 : 0;
            if (s_env == 1) {
                HARUKA_LOGI("CollisionWire", "activado por HARUKA_COLLISION_WIRE=1");
                setCollisionWireframe(true);
            }
        }
    }
    if (!m_collisionWireOn || !ctx || !_physicsEngine || !_camera) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    static std::vector<glm::dvec3> s_verts;
    static std::vector<uint32_t>   s_tris;
    glm::dvec3 center(0.0);
    uint64_t rev = 0;
    if (!_physicsEngine->getCollisionMeshDebug(s_verts, s_tris, center, rev)) return;

    // Solo se re-suben los buffers cuando la física reconstruye el parche (una vez cada 48 m de
    // deriva), no cada frame: son ~88 k vértices.
    // ⚠️ La recarga mira TAMBIÉN la versión de estáticos: los OBB de props se rehacen cada ~30 m
    // (mucho más a menudo que la malla del suelo), y sin esto el alambre de los props se quedaba
    // congelado en las cajas del primer refresco — que es exactamente la duda que vino a resolver.
    const uint64_t propVer = _physicsEngine->staticsVersion();
    if (rev != m_dbgLineRev || propVer != m_dbgPropVer) {
        m_dbgLineRev = rev;
        m_dbgPropVer = propVer;
        // ⚠️ SOLO EL ENTORNO CERCANO. El parche de colisión llega a 200 km y sus celdas del borde son
        // kilométricas: dibujarlo entero son 175 k triángulos de alambre que además tapan la pantalla.
        // Lo que hay que poder comparar es el suelo que se pisa, así que se recorta a lo que el bloque
        // uniforme cubre (±256 m, `TERRAIN_COLLIDE_UNIFORM_M`) con algo de margen.
        const double kShowM = Haruka::Planet::TERRAIN_COLLIDE_UNIFORM_M * 1.25;
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        std::vector<glm::vec3> lines;
        lines.reserve(4096);
        for (size_t i = 0; i + 2 < s_tris.size(); i += 3) {
            const glm::dvec3& a = s_verts[s_tris[i]];
            const glm::dvec3& b = s_verts[s_tris[i + 1]];
            const glm::dvec3& c = s_verts[s_tris[i + 2]];
            // Centroide dentro del radio: recortar por triángulo (no por vértice) evita aristas
            // sueltas que salen del borde y despistan.
            // Recorte alrededor del ANCLA, no de la cámara: el buffer se construye una vez por
            // reconstrucción, así que un recorte centrado en la cámara dejaba el disco visible
            // congelado contra una posición vieja y su borde derivaba con el jugador.
            const glm::dvec3 mid = (a + b + c) / 3.0;
            if (glm::length(mid - center) > kShowM) continue;
            // ⚠️ La resta cámara→vértice va en DOUBLE y solo el offset baja a float. Con las
            // posiciones de mundo (~6,37e6) en float, el alambre temblaría casi un metro respecto al
            // terreno — el mismo problema de cancelación que se arregló en el propio terreno, y aquí
            // haría que el instrumento inventara la disparidad que viene a medir.
            // ⚠️ RELATIVO AL CENTRO DE LA MALLA, NO A LA CÁMARA. Este buffer se construye una sola vez
            // por reconstrucción de la física (cada 48 m de deriva) y el shader dibuja con una matriz
            // SIN traslación, o sea que lo que se suba aquí queda rígidamente pegado a la cámara: al
            // caminar, la lámina verde caminaba contigo y se deslizaba sobre el terreno hasta 48 m,
            // cortando el suelo en diagonal. El instrumento inventaba justo la disparidad que venía a
            // medir — y el autotest de abajo no podía verlo, porque audita `s_verts` en coordenadas de
            // mundo, que están bien; lo que estaba mal era el DIBUJO.
            //
            // `center` es el ancla de la malla y no se mueve entre reconstrucciones, así que el offset
            // es estable. La traslación al ojo se aplica por frame, en la matriz (ver más abajo).
            const glm::vec3 fa = glm::vec3(a - center), fb = glm::vec3(b - center), fc = glm::vec3(c - center);
            lines.push_back(fa); lines.push_back(fb);
            lines.push_back(fb); lines.push_back(fc);
            lines.push_back(fc); lines.push_back(fa);
        }
        // ── CAJAS DE LOS PROPS (árboles y rocas) ────────────────────────────────────────────────
        //
        // Sin esto el alambre solo enseñaba el SUELO, así que "¿los árboles tienen collider y dónde?"
        // no era una pregunta que se pudiera mirar — solo deducir. Cada OBB se dibuja con sus 12
        // aristas, en el mismo marco relativo a `center` que el resto del buffer.
        {
            const auto& obbs = _physicsEngine->getPropOBBs();
            for (const auto& o : obbs) {
                if (glm::length(o.center - center) > kShowM) continue;
                // Los 8 vértices de la caja: centro ± cada semieje, en el marco del OBB.
                glm::vec3 v[8];
                for (int k = 0; k < 8; ++k) {
                    const glm::dvec3 sgn((k & 1) ? 1.0 : -1.0,
                                         (k & 2) ? 1.0 : -1.0,
                                         (k & 4) ? 1.0 : -1.0);
                    const glm::dvec3 local = o.halfExtents * sgn;
                    v[k] = glm::vec3((o.center + o.rot * local) - center);
                }
                // 12 aristas del cubo por índices de vértice.
                static const int E[12][2] = {
                    {0,1},{2,3},{4,5},{6,7},   // en X
                    {0,2},{1,3},{4,6},{5,7},   // en Y
                    {0,4},{1,5},{2,6},{3,7}    // en Z
                };
                for (const auto& e : E) { lines.push_back(v[e[0]]); lines.push_back(v[e[1]]); }
            }

            // ── MALLAS DE COLISIÓN (lo que la física usa de verdad) ─────────────────────────────
            //
            // ⚠️ Sin esto el alambre no enseñaba NADA de los props: al pasar a mallas dejaron de
            // existir las cajas y los conos que dibujaba. Un instrumento que se queda ciego al
            // cambiar lo que mide no sirve — es la segunda vez que pasa hoy con este mismo alambre.
            //
            // Se acota a un radio CORTO: son ~500 triángulos por prop y dibujarlos todos serían
            // cientos de miles de líneas. Lo que hace falta es ver la forma alrededor del jugador.
            {
                const double kMeshShowM = 40.0;
                const glm::dvec3 camNow = glm::dvec3(_camera->position);
                for (const auto& mi : _physicsEngine->getPropMeshInstances()) {
                    if (glm::length(mi.center - camNow) > kMeshShowM) continue;
                    // ¿De qué prototipo/parte es esta forma? Se busca por id en el cache.
                    const std::vector<glm::vec3>* tris = nullptr;
                    for (const auto& [proto, ids] : m_propMeshShapes) {
                        for (size_t part = 0; part < ids.size(); ++part) {
                            if (ids[part] != mi.shapeId) continue;
                            auto it = m_propMeshCpu.find(proto);
                            if (it != m_propMeshCpu.end() && part < it->second.size())
                                tris = &it->second[part];
                            break;
                        }
                        if (tris) break;
                    }
                    if (!tris) continue;
                    for (size_t t = 0; t + 2 < tris->size(); t += 3) {
                        glm::vec3 w[3];
                        for (int k = 0; k < 3; ++k) {
                            const glm::dvec3 lp = glm::dvec3((*tris)[t + k]) * mi.scale;
                            w[k] = glm::vec3((mi.center + mi.rot * lp) - center);
                        }
                        lines.push_back(w[0]); lines.push_back(w[1]);
                        lines.push_back(w[1]); lines.push_back(w[2]);
                        lines.push_back(w[2]); lines.push_back(w[0]);
                    }
                }
            }

            // ── CONOS (troncos y ramas) ─────────────────────────────────────────────────────────
            //
            // ⚠️ Se dibujan APARTE porque no están en `propOBBs`: al darles su forma real pasaron a
            // `propCones`, y el alambre —que solo leía las cajas— dejó de enseñar los árboles justo
            // cuando cambiaron de forma. Un instrumento que se queda ciego al tocar lo que mide es
            // peor que no tenerlo.
            //
            // Se pinta el contorno de verdad: dos anillos (base y punta, con SUS radios) y unas
            // generatrices que los unen. Así se ve el taper, que es lo que distingue el cono de la
            // caja que había antes.
            for (const auto& c : _physicsEngine->getPropCones()) {
                if (glm::length(c.center - center) > kShowM) continue;
                const int kSeg = 8;
                glm::vec3 ringBot[kSeg], ringTop[kSeg];
                for (int k = 0; k < kSeg; ++k) {
                    const double a = 6.283185307179586 * (double)k / (double)kSeg;
                    const glm::dvec3 offB(std::cos(a) * c.rBottom, -c.halfHeight, std::sin(a) * c.rBottom);
                    const glm::dvec3 offT(std::cos(a) * c.rTop,     c.halfHeight, std::sin(a) * c.rTop);
                    ringBot[k] = glm::vec3((c.center + c.rot * offB) - center);
                    ringTop[k] = glm::vec3((c.center + c.rot * offT) - center);
                }
                for (int k = 0; k < kSeg; ++k) {
                    const int k2 = (k + 1) % kSeg;
                    lines.push_back(ringBot[k]); lines.push_back(ringBot[k2]);   // anillo de la base
                    lines.push_back(ringTop[k]); lines.push_back(ringTop[k2]);   // anillo de la punta
                    if ((k % 2) == 0) {                                          // generatrices (la mitad)
                        lines.push_back(ringBot[k]); lines.push_back(ringTop[k]);
                    }
                }
            }
        }

        m_dbgLineVerts = (uint32_t)lines.size();
        if (RHI::valid(m_dbgLineVB)) { dev->destroy(m_dbgLineVB); m_dbgLineVB = {}; }
        if (m_dbgLineVerts > 0)
            m_dbgLineVB = dev->createBuffer(RHI::BufferUsage::Vertex,
                                            lines.size() * sizeof(glm::vec3), lines.data());
        // ── AUTOVERIFICACIÓN DEL INSTRUMENTO ────────────────────────────────────────────────────
        //
        // Antes de creerse lo que el alambre enseña hay que saber si el alambre está bien puesto. Y
        // hay una comprobación EXACTA disponible: los vértices de la malla de colisión se generaron
        // muestreando la función de altura en esos puntos, así que **en un vértice la malla ES la
        // función**, sin sagita ni interpolación de por medio. La desviación tiene que ser ~0.
        //
        // Si sale ~0, el alambre está donde dice y cualquier separación que se vea en pantalla es del
        // render. Si NO sale 0, el instrumento está desplazado y todo lo que sugiera es falso — que es
        // exactamente lo que hay que descartar antes de seguir buscando.
        if (_planetarySystem) {
            glm::dvec3 pc; double pr = 0.0;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                // ⚠️ DESCOMPUESTO EN TRES, porque la versión de una sola cifra decía «0,55 m» y no
                // permitía saber de cuál de los tres supuestos venía. Cada columna aísla uno:
                //
                //   triM0 : compara contra `sampleTerrainHeight(v)`, que usa `terrainTriM(0)` FIJO.
                //   triMr : contra el triM que el vértice usó DE VERDAD, `terrainTriM(radio tangente)`.
                //           Si esta baja a ~0 y la anterior no, el desajuste es de LOD, no de posición.
                //   radio : el rango de radios tangentes muestreados, para saber si el filtro de 320 m
                //           está cogiendo lo que se cree que coge (con triM plano ambos han de coincidir).
                double worst = 0.0, sum = 0.0, worstR = 0.0, sumR = 0.0;
                double radMin = 1e300, radMax = 0.0; int n = 0;
                const size_t stride = std::max<size_t>(1, s_verts.size() / 400);
                for (size_t i = 0; i < s_verts.size(); i += stride) {
                    const glm::dvec3& v = s_verts[i];
                    if (glm::length(v - center) > kShowM) continue;
                    const double alt  = glm::length(v - pc) - pr;         // cota del vértice capturado
                    const double func = _planetarySystem->sampleTerrainHeight(v);
                    // Radio TANGENTE del vértice respecto al ancla de la malla: es el argumento con el
                    // que se eligió su triM al construirlo.
                    const glm::dvec3 up = glm::normalize(center - pc);
                    const glm::dvec3 rel = v - center;
                    const double radM = glm::length(rel - up * glm::dot(rel, up));
                    const double funcR = _planetarySystem->sampleTerrainHeight(
                                             v, Haruka::Planet::terrainTriM(radM));
                    const double d = std::abs(alt - func), dR = std::abs(alt - funcR);
                    worst = std::max(worst, d); sum += d;
                    worstR = std::max(worstR, dR); sumR += dR;
                    radMin = std::min(radMin, radM); radMax = std::max(radMax, radM);
                    ++n;
                }
                if (n > 0)
                    HARUKA_LOGI("CollisionWire", "AUTOTEST del alambre: %d vertices · triM0: peor %.4f "
                                "media %.4f · triMr: peor %.4f media %.4f · radio tangente %.0f..%.0f m "
                                "(deberia ser ~0: en un vertice la malla ES la funcion)",
                                n, worst, sum / n, worstR, sumR / n, radMin, radMax);
            }
        }
        HARUKA_LOGI("CollisionWire", "malla de colision: %zu triangulos en total, %u vertices de "
                    "alambre dentro de %.0f m del jugador", s_tris.size() / 3, m_dbgLineVerts, kShowM);
    }
    if (m_dbgLineVerts == 0 || !RHI::valid(m_dbgLineVB)) return;

    // ⚠️ Una sola oportunidad: si el pipeline no se crea, no se reintenta cada frame. La primera
    // versión lo reintentaba y llenó el log con "FALLO" 60 veces por segundo.
    static bool s_dbgLineTried = false;
    if (!RHI::valid(m_dbgLinePSO) && !s_dbgLineTried) {
        s_dbgLineTried = true;
        // ⚠️ La ruta va por `Shader::baseDir()`, no "assets/shaders/..." a pelo. Es el mismo error que
        // el comentario del bloque de bloom advierte: sin la base, el fichero no se encuentra, el shader
        // queda en 0 y el programa no enlaza — que es exactamente lo que pasó al primer intento.
        const std::string vsP = Shader::baseDir() + "shaders/debug_lines.vert";
        const std::string fsP = Shader::baseDir() + "shaders/debug_lines.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vsP.c_str();
        pd.fragmentPath = fsP.c_str();
        pd.vertexLayout.strides    = { (uint32_t)sizeof(glm::vec3) };
        pd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 } };
        pd.topology   = RHI::PrimitiveTopology::Lines;
        // Profundidad ACTIVA pero sin escribir: el alambre se oculta tras el relieve, que es lo que
        // permite ver si pasa por encima o por debajo del suelo dibujado. Con el test apagado flotaría
        // siempre sobre todo y no diría nada.
        pd.depth.test = true;  pd.depth.write = false;
        pd.cull       = RHI::CullMode::None;
        pd.blend.enable = false;
        m_dbgLinePSO = dev->createPipeline(pd);
        HARUKA_LOGI("CollisionWire", "pipeline de alambre: %s",
                    RHI::valid(m_dbgLinePSO) ? "ok" : "FALLO");
    }
    if (!RHI::valid(m_dbgLinePSO)) return;
    if (!RHI::valid(m_dbgLineUBO))
        m_dbgLineUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(glm::mat4) + sizeof(glm::vec4),
                                         nullptr, RHI::BufferMemory::Dynamic);
    // La traslación ancla→ojo se recalcula CADA FRAME y se dobla en la matriz. Es la mitad que faltaba:
    // los vértices son relativos al ancla (fijo) y esto los lleva al ojo (móvil). Antes los vértices ya
    // venían relativos a la cámara de hace hasta 48 m y esta matriz no tenía traslación, así que nada
    // corregía el desplazamiento. La resta va en DOUBLE y solo el resultado baja a float — con las
    // posiciones de mundo (~6,37e6) en float el alambre temblaría casi un metro.
    const glm::vec3 anchorRelEye = glm::vec3(center - glm::dvec3(_camera->position));
    const glm::mat4 vp = glm::translate(viewProjRotOnly, anchorRelEye);
    struct { glm::mat4 vp; glm::vec4 color; } u{ vp, glm::vec4(0.15f, 1.0f, 0.35f, 1.0f) };
    dev->updateBuffer(m_dbgLineUBO, 0, sizeof(u), &u);

    ctx->bindPipeline(m_dbgLinePSO);
    ctx->bindUniformBuffer(0, m_dbgLineUBO);
    ctx->bindVertexBuffer(m_dbgLineVB, 0);
    ctx->draw(m_dbgLineVerts);
#else
    (void)ctx; (void)viewProjRotOnly;
#endif
}

void Application::renderPropShadows(RHI::Context* ctx, const glm::mat4& lightSpace) {
    if (!ctx || !_camera || !_instancing) return;
    const int protoCount = m_propRegistry.prototypeCount();
    if (protoCount <= 0 || m_propRegistry.instances().empty()) return;

    RHI::Device* dev = RHI::device();
    if (!dev) return;

    // PSO de solo profundidad, con el MISMO layout de instancia que el pase de color: si divergiera,
    // las sombras saldrían en otro sitio que los objetos.
    if (!RHI::valid(m_propShadowPSO)) {
        const std::string vs = Shader::baseDir() + "shaders/prop_depth_inst.vert";
        const std::string fs = Shader::baseDir() + "shaders/depth_only.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)sizeof(PropVertex) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(PropVertex, pos), RHI::Format::RGB32F, 0 },
            // La parte hace falta TAMBIÉN aquí: sin ella una rama arrancada seguiría proyectando
            // su sombra, que es la forma más barata de delatar que el árbol no está donde se ve.
            { 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 },
        };
        Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        // SIN back-face culling: un tronco de una sola capa de caras dejaría de proyectar sombra por
        // la mitad. En un pase de profundidad el coste de dibujar las dos caras es despreciable.
        pd.cull         = RHI::CullMode::None;
        m_propShadowPSO = dev->createPipeline(pd);
        HARUKA_LOGI("PropShadow", "pipeline de profundidad instanciada: %s",
                    RHI::valid(m_propShadowPSO) ? "ok" : "FALLO (los props no proyectan sombra)");
    }
    if (!RHI::valid(m_propShadowPSO)) return;

    if (!RHI::valid(m_propShadowUBO))
        m_propShadowUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(glm::mat4),
                                           nullptr, RHI::BufferMemory::Dynamic);
    dev->updateBuffer(m_propShadowUBO, 0, sizeof(lightSpace), &lightSpace);

    // Centro y planeta, para reconstruir la transformación igual que el pase de color.
    glm::dvec3 planetC(0.0); double planetR = 0.0;
    if (!_planetarySystem || !_planetarySystem->getActivePlanet(planetC, planetR)) return;
    const glm::dvec3 camD = glm::dvec3(_camera->position);
    const glm::dvec3 camToCenter = planetC - camD;
    const glm::vec3  up(0.0f, 1.0f, 0.0f);

    // Radio de la caja de sombra con margen: el volumen es ±42 m, y un prop justo fuera puede
    // proyectar dentro. 1,6× cubre eso sin barrer el planeta.
    const float kShadowBoxM = 42.0f * 1.6f;
    const float kBox2 = kShadowBoxM * kShadowBoxM;

    ctx->bindPipeline(m_propShadowPSO);
    ctx->bindUniformBuffer(0, m_propShadowUBO);

    static std::vector<Haruka::InstanceDataFloat> s_shadowBucket;
    for (int pi = 0; pi < protoCount; ++pi) {
        const Haruka::InstancedPrototype& proto = m_propRegistry.prototype(pi);
        if (proto.name.empty()) continue;
        auto itMesh = m_propProtoMesh.find(proto.name);
        if (itMesh == m_propProtoMesh.end()) continue;      // malla aún sin hornear
        const PropPrototypeGpu& pg = itMesh->second;
        // ⚠️ LAS SOMBRAS USAN UN NIVEL FIJO, el intermedio. Es un pase de PROFUNDIDAD: lo único que
        // aporta la malla es su silueta, y la caja de la luz es de ±67 m, así que todo lo que entra está
        // cerca y el nivel 1 (130 triángulos frente a 464) da la misma silueta a efectos prácticos.
        // Además evita un segundo camino de selección por distancia en el pase de sombras.
        const int kShadowLod = 1;
        const int sl = (pg.indexCount[kShadowLod] > 0 && RHI::valid(pg.vbo[kShadowLod])) ? kShadowLod : 0;
        if (pg.indexCount[sl] == 0 || !RHI::valid(pg.vbo[sl]) || !RHI::valid(pg.ebo[sl])) continue;

        s_shadowBucket.clear();
        for (const auto& io : m_propRegistry.instances()) {
            if (io.prototype != pi) continue;
            if (io.state != (uint32_t)Haruka::InstancedObjectState::Alive) continue;
            const glm::vec3 posF = glm::vec3(camToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
            if (glm::dot(posF, posF) > kBox2) continue;      // fuera de la caja de la luz
            glm::mat4 m = glm::translate(glm::mat4(1.0f), posF);
            m *= glm::mat4_cast(glm::rotation(up, io.dir));
            m = glm::rotate(m, io.yaw, up);   // eje LOCAL (ver la nota del pase de color)
            m = glm::scale(m, glm::vec3(io.scale));
            Haruka::InstanceDataFloat inst;
            inst.model = m;
            inst.color = glm::vec4(1.0f);
            inst.scale = glm::vec3(io.scale);
            s_shadowBucket.push_back(inst);
        }
        if (s_shadowBucket.empty()) continue;

        _instancing->setInstances(s_shadowBucket);
        ctx->bindVertexBuffer(pg.vbo[sl], 0);
        ctx->bindIndexBuffer(pg.ebo[sl]);
        _instancing->render(ctx, pg.indexCount[sl], 1);
    }
}

// Texturas de relleno 1x1 para los slots de material ausentes.
//
// ⚠️ EL VALOR NEUTRO NO ES EL MISMO EN TODOS LOS SLOTS, y ponerlo mal se ve. Al principio se rellenó
// todo con BLANCO, y en albedo o AO es neutro pero en METALLIC blanco significa **metal puro**:
// cualquier prop con ese slot ausente se convertía en un espejo, con caras blancas y brillantes
// donde pegaba la luz. Los neutros correctos son:
//     albedo    -> blanco  (multiplica por 1)
//     normal    -> (0.5, 0.5, 1) = normal plana en espacio tangente
//     metallic  -> NEGRO   (0 = dieléctrico; blanco = metal)
//     roughness -> blanco  (1 = totalmente rugoso, sin especular de espejo)
//     ao        -> blanco  (1 = sin oclusión)
Haruka::RHI::TextureHandle Application::fallbackTexture(FallbackTex kind) {
    const size_t k = (size_t)kind;
    if (k < m_fallbackTex.size() && RHI::valid(m_fallbackTex[k])) return m_fallbackTex[k];
    RHI::Device* dev = RHI::device();
    if (!dev) return {};
    if (m_fallbackTex.size() < (size_t)FallbackTex::Count)
        m_fallbackTex.resize((size_t)FallbackTex::Count);

    uint8_t px[4] = { 255, 255, 255, 255 };
    switch (kind) {
        case FallbackTex::Normal:   px[0] = 128; px[1] = 128; px[2] = 255; break;
        case FallbackTex::Metallic: px[0] = px[1] = px[2] = 0;             break;  // 0 = NO metal
        default: break;                                                            // blanco
    }
    RHI::TextureDesc td;
    td.width = 1; td.height = 1;
    td.format = RHI::Format::RGBA8;
    td.filter = RHI::Filter::Nearest;
    td.initialData = px;
    m_fallbackTex[k] = dev->createTexture(td);
    return m_fallbackTex[k];
}

void Application::refreshPropScatter() {
    if (!m_propScatterEnabled) {
        if (m_propRegistry.prototypeCount() > 0) m_propRegistry.reset();
        m_propScatterDebug.clear();
        m_propScatterPlanet.clear();
        m_propScatterLastCam = {1e300, 1e300, 1e300};
        return;
    }
    if (!_camera || !_planetarySystem) return;
    const glm::dvec3 camPos = glm::dvec3(_camera->position);

    glm::dvec3 planetC; double planetR = 0.0;
    if (!_planetarySystem->getActivePlanet(planetC, planetR)) {
        if (m_propRegistry.prototypeCount() > 0) m_propRegistry.reset();
        m_propScatterPlanet.clear();
        return;
    }
    const Haruka::Planet::TerrestrialPlanet* planet = _planetarySystem->activeTerrestrial();
    const std::string pname = _planetarySystem->getActivePlanetName();
    if (!planet) {                                   // planeta orbital sin superficie = sin props
        if (m_propRegistry.prototypeCount() > 0) m_propRegistry.reset();
        m_propScatterPlanet.clear();
        m_propScatterLastCam = {1e300, 1e300, 1e300};
        return;
    }
    if (pname != m_propScatterPlanet) {
        m_propRegistry.reset();   // planeta nuevo: re-registrar prototipos
        m_propScatterPlanet = pname;
    }

    // Solo re-enumera cuando la cámara cruza un tramo desde la última muestra. El scatter (~61k
    // celdas × muestreo del campo) es caro (~121 ms); sin topes re-corría a cada paso al volar.
    // El tramo es ADAPTATIVO a la velocidad: al andar bastan 30 m, pero al volar/órbita el tope
    // fijo de 0,5 s producía un bajón de ~121 ms cada medio segundo (1-8 fps). Con el tramo ~1,5 s
    // de viaje, el refresh se espacia a la vez que la cámara se mueve rápido. Las celdas son
    // MUNDIALES y deterministas: entran/salen por delante/detrás sin "tp", solo con algo de retraso.
    static std::chrono::steady_clock::time_point s_lastT = std::chrono::steady_clock::now();
    const auto nowT = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(nowT - s_lastT).count();
    const double distCam = glm::length(camPos - m_propScatterLastCam);
    const double speed   = elapsed > 1e-3 ? distCam / elapsed : 0.0;
    const double refreshM = std::max(30.0, speed * 1.5);
    const bool tooSoon = elapsed < 0.5;
    if ((distCam < refreshM || tooSoon) &&
        !m_propScatterPlanet.empty() && m_propRegistry.prototypeCount() > 0)
        return;
    m_propScatterLastCam = camPos;
    s_lastT = nowT;

    // El campo real del planeta + la tabla de capas que DECLARA (orden = prioridad de instalación).
    PlanetSphereField field;
    field.planet = planet;
    const Haruka::Planet::PropLayerTable& table = planet->propLayers();
    if (table.layers.empty()) return;   // sin capas declaradas = sin props

    // Registra UN prototipo por mesh de capa (malla compartida por todas sus instancias).
    // `meshSeed` fija el bake determinista de la malla del prototipo (p.ej. bakeTreeMesh).
    for (const auto& L : table.layers) {
        if (L.mesh.empty()) continue;
        bool found = false;
        for (int i = 0; i < m_propRegistry.prototypeCount(); ++i)
            if (m_propRegistry.prototype(i).name == L.mesh) { found = true; break; }
        if (found) continue;
        Haruka::InstancedPrototype p;
        p.name     = L.mesh;
        // meshSeed determinista (hash32 de planeta+mesh): el bake de la malla prototipo (bakeTreeMesh)
        // es reproducible en cualquier plataforma/ejecución — un árbol es SIEMPRE el mismo árbol.
        uint32_t h = 2166136261u;
        for (const char c : m_propScatterPlanet) h = (h ^ (uint8_t)c) * 16777619u;
        for (const char c : L.mesh)             h = (h ^ (uint8_t)c) * 16777619u;
        p.meshSeed = Haruka::Tools::ProcGraph::hash32(h);
        p.lodLevel = 0;
        m_propRegistry.addPrototype(p);
    }
    if (m_propRegistry.prototypeCount() == 0) return;

    // Parámetros del scatter: bandas hasta el horizonte + semilla/radio del planeta activo.
    Haruka::Planet::PropScatterParams params;
    params.radius = planetR;
    params.seed   = planet->config().seed ? planet->config().seed : 1u;

    // Coloca las instancias (deterministas por celda) y las traduce al registro.
    // ⚠️ CAPAS MUERTAS. `scatterPropsNear` recorre las capas EN ORDEN DE PRIORIDAD y hace `break` en
    // la primera que acepta la celda: una capa permisiva colocada arriba se queda con TODAS las
    // celdas y las de abajo no llegan a colocar ni un prop. El autor declara 4 capas, ve props de
    // una sola, y el motor registra los prototipos igual y los deja a cero sin decir nada.
    //
    // Y hay que distinguir DOS causas con arreglos opuestos: una capa que TUVO turno y lo rechazó
    // (sus condiciones están mal) frente a una que NUNCA llegó a tenerlo (otra le come las celdas y
    // hay que reordenar la tabla). Por eso el stats cuenta `offered` aparte de los descartes.
    static bool s_propStatsReported = false;
    Haruka::Planet::PropScatterStats stats;
    const std::vector<Haruka::Planet::ScatteredProp> placed =
        Haruka::Planet::scatterPropsNear(field, camPos, planetC, params, table,
                                         s_propStatsReported ? nullptr : &stats);
    if (!s_propStatsReported && !placed.empty()) {
        s_propStatsReported = true;
        for (size_t li = 0; li < table.layers.size(); ++li) {
            const auto& L = table.layers[li];
            if (L.mesh.empty() || stats.placed[li] > 0) continue;
            if (stats.offered[li] == 0) {
                HARUKA_LOGW("PropScatter",
                    "capa %zu ('%s', mesh '%s'): 0 props y NUNCA tuvo turno — una capa de MAYOR "
                    "prioridad reclama todas las celdas antes. Subela en la tabla o baja la densidad "
                    "de la que va delante.", li, L.name.c_str(), L.mesh.c_str());
            } else {
                // Desglose por FACTOR: cada uno se arregla tocando un campo distinto del JSON.
                HARUKA_LOGW("PropScatter",
                    "capa %zu ('%s', mesh '%s'): 0 props con %d turnos. El problema es SUYO, no del "
                    "orden. Descartes: sumergido=%d densidad=%d · coverage=%d de los cuales "
                    "zona=%d when=%d humedad=%d temp=%d pendiente=%d mapa=%d",
                    li, L.name.c_str(), L.mesh.c_str(), stats.offered[li],
                    stats.noSubmerged[li], stats.noDensity[li], stats.noCoverage[li],
                    stats.failZone[li], stats.failWhen[li], stats.failHum[li],
                    stats.failTemp[li], stats.failSlope[li], stats.failMap[li]);
                // Y lo que PIDE frente a lo que el terreno DA: sin esto sabes que la humedad no
                // pasa, pero no que tu banda pide 0.35-0.95 donde el terreno solo da 0.02-0.14.
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
        }
    }

    // El scatter regenera las instancias desde CERO en cada refresco: preserva el ESTADO que el
    // juego marcó (destruido/rebrotando) por seed determinista — el árbol que tumbaste sigue caído
    // al volver, en vez de reaparecer porque el registro se recalculó.
    // ⚠️ El estado NO se recupera de las instancias vivas, sino de `m_propState`, que sobrevive a
    // que el prop salga del radio del scatter. Antes se leía del propio registro, y eso tenía dos
    // fallos: (a) la condición solo miraba `state != Alive`, así que un árbol VIVO al que le
    // arrancaste una rama perdía su máscara al refrescar; y (b) aunque la mirase, alejarse unos
    // cientos de metros sacaba la instancia del registro y el árbol talado REAPARECÍA.
    // Se sincroniza primero lo que haya en el registro por si algo lo tocó sin pasar por
    // `breakPropAt`, y luego el mapa manda.
    for (const auto& io : m_propRegistry.instances()) {
        if (io.state == (uint32_t)Haruka::InstancedObjectState::Alive && io.breakMask == 0u) continue;
        m_propState[io.seed] = PropStateDelta{ io.seed, io.state, io.breakMask, io.regrow };
    }

    std::vector<Haruka::InstancedObject> objs;
    objs.reserve(placed.size());
    for (const auto& sp : placed) {
        int protoIdx = -1;
        for (int i = 0; i < m_propRegistry.prototypeCount(); ++i)
            if (m_propRegistry.prototype(i).name == sp.mesh) { protoIdx = i; break; }
        if (protoIdx < 0) continue;

        Haruka::InstancedObject io;
        io.prototype = protoIdx;
        io.dir       = sp.dir;
        io.heightM   = sp.heightM;
        io.scale     = sp.scale;
        io.tint      = sp.tint;
        // Yaw determinista por celda (orientación estable, sin volver a mirar el campo).
        io.yaw = Haruka::Tools::ProcGraph::WhiteNode::hashFloat((int)sp.cellSeed, 500, 0, params.seed)
                 * 6.2831853f;
        io.seed  = sp.cellSeed;
        io.state = sp.state;
        auto it = m_propState.find(io.seed);
        if (it != m_propState.end()) {
            io.state     = it->second.state;
            io.regrow    = it->second.regrow;
            io.breakMask = it->second.breakMask;
        }
        objs.push_back(io);
    }
    m_propRegistry.setInstances(std::move(objs));

    // Los props del scatter ahora COLISIONAN. Se alimenta aquí y no por frame porque el scatter es
    // quien cambia el conjunto de instancias: registrar en otro sitio sería mirar una lista que no
    // se ha movido.
    refreshPropColliders(planetC, planetR, camPos);
}

std::vector<Application::PropStateDelta> Application::serializePropState() const {
    std::vector<PropStateDelta> out;
    out.reserve(m_propState.size());
    for (const auto& [seed, st] : m_propState) out.push_back(st);
    return out;
}

void Application::restorePropState(const std::vector<PropStateDelta>& deltas) {
    m_propState.clear();
    for (const auto& d : deltas) m_propState[d.seed] = d;
    // Fuerza un re-scatter: las instancias ya colocadas llevan el estado de la partida ANTERIOR.
    m_propScatterLastCam = { 1e300, 1e300, 1e300 };
}

Application::PropHit Application::breakPropAt(const glm::dvec3& center, double radius) {
    PropHit best;
    if (!_planetarySystem) return best;
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!_planetarySystem->getActivePlanet(planetC, planetR)) return best;

    const int protoCount = m_propRegistry.prototypeCount();
    if (protoCount == 0) return best;

    // Distancia de un punto a una caja orientada: se lleva el punto al marco de la caja y se
    // recorta contra sus semiejes. Es la misma prueba que hace la física, no una aproximación
    // por esfera — un tronco es mucho más alto que ancho y una esfera envolvente lo delataría.
    auto distToOBB = [](const Haruka::Planet::PropWorldOBB& o, const glm::dvec3& p) {
        const glm::dvec3 d = p - o.center;
        glm::dvec3 l(glm::dot(d, glm::dvec3(o.rot[0])),
                     glm::dot(d, glm::dvec3(o.rot[1])),
                     glm::dot(d, glm::dvec3(o.rot[2])));
        const glm::dvec3 c = glm::clamp(l, -o.halfExtents, o.halfExtents);
        return glm::length(l - c);
    };

    std::vector<std::vector<Haruka::Planet::PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi)
        protoParts[(size_t)pi] = Haruka::Planet::propColliderParts(
            m_propRegistry.prototype(pi).name, m_propRegistry.prototype(pi).meshSeed);

    // Se busca el punto MÁS CERCANO, no el primero que toque: con el hacha se golpea entre varias
    // partes (rama y tronco se solapan en la axila) y "el primero de la lista" cortaría la rama
    // cuando apuntabas al fuste.
    double bestD = radius;
    size_t bestIdx = (size_t)-1;
    const auto& insts = m_propRegistry.instances();
    for (size_t ii = 0; ii < insts.size(); ++ii) {
        const auto& io = insts[ii];
        if (io.state != (uint32_t)Haruka::InstancedObjectState::Alive) continue;
        if (io.prototype < 0 || io.prototype >= protoCount) continue;

        // Descarte barato antes de construir las cajas: el prop entero contra la esfera del golpe.
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const double reach = radius + 12.0 * (double)io.scale;   // alto máximo razonable de un prop
        if (glm::length(wp - center) > reach) continue;

        const auto boxes = Haruka::Planet::propWorldColliders(
            protoParts[(size_t)io.prototype], io.dir, io.heightM, io.scale, io.yaw,
            planetC, planetR, io.breakMask);
        for (const auto& b : boxes) {
            if (!b.breakable) continue;
            const double d = distToOBB(b, center);
            if (d >= bestD) continue;
            bestD   = d;
            bestIdx = ii;
            best.hit       = true;
            best.seed      = io.seed;
            best.prototype = m_propRegistry.prototype(io.prototype).name;
            best.partId    = b.partId;
            best.trunk     = (b.partId == 0);
            best.lengthM   = b.length;
            best.rBottomM  = b.rBottom;
            best.rTopM     = b.rTop;
            best.pos       = b.center;
        }
    }
    if (!best.hit || bestIdx == (size_t)-1) return best;

    // Aplicar la rotura. El tronco se lleva el árbol entero (con su copa y sus ramas); una rama
    // solo pone su bit, y el árbol sigue en pie y sigue colisionando por el resto de sus partes.
    if (best.trunk) {
        m_propRegistry.setStateBySeed(best.seed, Haruka::InstancedObjectState::Destroyed);
    } else if (best.partId >= 0 && best.partId < 32) {
        m_propRegistry.setBreakBitBySeed(best.seed, (uint32_t)best.partId);
    }
    // …y al mapa persistente, que es lo que hace que siga roto cuando vuelvas.
    PropStateDelta& st = m_propState[best.seed];
    st.seed = best.seed;
    if (best.trunk) st.state = (uint32_t)Haruka::InstancedObjectState::Destroyed;
    else if (best.partId >= 0 && best.partId < 32) st.breakMask |= (1u << (uint32_t)best.partId);

    // Los colliders se rehacen ya: si no, seguirías chocando con la rama que acabas de arrancar
    // hasta el siguiente refresco del scatter (hasta 30 m de camino).
    refreshPropColliders(planetC, planetR,
                         _camera ? glm::dvec3(_camera->position) : glm::dvec3(0.0));
    return best;
}

// ────────────────────────────────────────────────────────────────────────────────────────────────
// MALLAS DE COLISIÓN DE LOS PROTOTIPOS: la geometría REAL del prop, partida por parte.
//
// ⚠️ Se registran UNA vez por (prototipo, parte). Partirlas por parte no es un capricho: es lo que
// permite seguir rompiendo una rama —cada parte es un cuerpo aparte, y la rota simplemente no se
// coloca— sin renunciar a la forma exacta. El `MeshShape` de Jolt monta un árbol AABB y eso es lo
// caro; con ~300 vértices por prototipo es despreciable, pero por INSTANCIA sería inviable.
//
// La COPA no genera colisión: sus triángulos son follaje (materialId 1) y chocar con hojas se
// siente mal. Se filtra por material, no por parte, porque la copa cuelga del tronco.
const std::vector<int>& Application::propMeshShapesFor(int protoIdx) {
    auto it = m_propMeshShapes.find(protoIdx);
    if (it != m_propMeshShapes.end()) return it->second;

    std::vector<int>& ids = m_propMeshShapes[protoIdx];
    PhysicsEngine* phys = getPhysicsEngine();
    if (!phys || protoIdx < 0 || protoIdx >= m_propRegistry.prototypeCount()) return ids;

    const auto& proto = m_propRegistry.prototype(protoIdx);
    const auto shape = Haruka::Planet::propShapeKind(proto.name);

    // Se hornea la malla del prototipo a DETALLE MÁXIMO: la colisión no depende del LOD.
    Haruka::Tools::ProcGraph::TreeMeshData tm;
    if (shape == Haruka::Planet::PropShapeKind::Rock) {
        tm = Haruka::Tools::ProcGraph::bakeRockMesh((int)proto.meshSeed, 1.0f, 0.72f);
    } else if (shape == Haruka::Planet::PropShapeKind::House) {
        tm = Haruka::Tools::ProcGraph::bakeHouseMesh((int)proto.meshSeed, 1.0f);
    } else {
        const auto tp = Haruka::Planet::propTreeParams();
        Haruka::Tools::ProcGraph::Graph g;
        const int node = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
            (int)proto.meshSeed, tp.height, tp.trunkR, tp.canopy, tp.segments, 1.0f);
        g.compile();
        Haruka::Tools::ProcGraph::bakeTreeMesh(g, node, tm);
    }
    if (tm.positions.empty() || tm.indices.empty()) return ids;

    // ¿Cuántas partes? Para roca/casa, una sola (la malla entera).
    int maxPart = 0;
    for (unsigned char p : tm.partId) maxPart = std::max(maxPart, (int)p);
    const int nParts = tm.partId.empty() ? 1 : (maxPart + 1);
    ids.assign((size_t)nParts, -1);

    std::vector<float>    verts;
    std::vector<uint32_t> idx;
    for (int part = 0; part < nParts; ++part) {
        verts.clear(); idx.clear();
        std::vector<int> remap(tm.positions.size(), -1);
        for (size_t t = 0; t + 2 < tm.indices.size(); t += 3) {
            const unsigned i0 = tm.indices[t], i1 = tm.indices[t + 1], i2 = tm.indices[t + 2];
            // Solo triángulos de ESTA parte, y solo los SÓLIDOS (material 0 = corteza).
            if (!tm.partId.empty() && (int)tm.partId[i0] != part) continue;
            if (!tm.materialId.empty() && tm.materialId[i0] != 0) continue;
            for (unsigned vi : { i0, i1, i2 }) {
                if (remap[vi] < 0) {
                    remap[vi] = (int)(verts.size() / 3);
                    verts.push_back(tm.positions[vi].x);
                    verts.push_back(tm.positions[vi].y);
                    verts.push_back(tm.positions[vi].z);
                }
                idx.push_back((uint32_t)remap[vi]);
            }
        }
        if (idx.size() < 3) continue;
        ids[(size_t)part] = phys->registerPropMesh(verts.data(), verts.size() / 3,
                                                   idx.data(), idx.size());
        // Copia en CPU para el alambre: los mismos triángulos, en el mismo marco local.
        auto& cpu = m_propMeshCpu[protoIdx];
        if ((int)cpu.size() <= part) cpu.resize((size_t)part + 1);
        cpu[(size_t)part].clear();
        cpu[(size_t)part].reserve(idx.size());
        for (uint32_t vi : idx)
            cpu[(size_t)part].push_back(glm::vec3(verts[vi * 3 + 0], verts[vi * 3 + 1],
                                                  verts[vi * 3 + 2]));
    }
    HARUKA_LOGD("PropCollider", "malla de colision '%s': %d parte(s) registradas",
                proto.name.c_str(), (int)ids.size());
    return ids;
}

void Application::refreshPropColliders(const glm::dvec3& planetC, double planetR,
                                       const glm::dvec3& camPos) {
    PhysicsEngine* phys = getPhysicsEngine();
    if (!phys) return;

    // ⚠️ RADIO ACOTADO, y el número no es libre. Cada `addPropOBB` sube `m_staticsVersion`, y eso
    // hace que Jolt DESTRUYA Y RECREE todos los cuerpos estáticos (`syncStatics`). Registrar las
    // ~40k instancias del scatter × 4 partes serían 160k cuerpos recreados cada refresco: no es
    // una opción. El radio tiene que ser mayor que la deriva entre refrescos del scatter (30 m,
    // `refreshM`) más un margen, o llegarías a un árbol antes de que le toque su collider.
    constexpr double kColliderRadiusM = 96.0;
    const double r2 = kColliderRadiusM * kColliderRadiusM;

    // Colliders locales por PROTOTIPO: el esqueleto solo depende de (nombre, meshSeed), así que se
    // deriva una vez por prototipo y no una vez por árbol.
    const int protoCount = m_propRegistry.prototypeCount();
    std::vector<std::vector<Haruka::Planet::PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi) {
        const auto& proto = m_propRegistry.prototype(pi);
        protoParts[(size_t)pi] = Haruka::Planet::propColliderParts(proto.name, proto.meshSeed);
    }

    const auto t0 = std::chrono::steady_clock::now();
    phys->clearPropOBBs();
    phys->clearPropCones();
    phys->clearPropMeshInstances();
    size_t nProps = 0, nBoxes = 0;
    std::vector<size_t> perProtoProps((size_t)protoCount, 0), perProtoBoxes((size_t)protoCount, 0);
    for (const auto& io : m_propRegistry.instances()) {
        if (io.state != (uint32_t)Haruka::InstancedObjectState::Alive) continue;   // tocón: no estorba
        if (io.prototype < 0 || io.prototype >= protoCount) continue;

        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const glm::dvec3 d  = wp - camPos;
        if (glm::dot(d, d) > r2) continue;
        ++nProps;

        const auto boxes = Haruka::Planet::propWorldColliders(
            protoParts[(size_t)io.prototype], io.dir, io.heightM, io.scale, io.yaw,
            planetC, planetR, io.breakMask);
        perProtoProps[(size_t)io.prototype]++;
        for (const auto& b : boxes) {
            // ⚠️ LA MALLA REAL DEL PROP. Nada de primitivas: con caja o cono siempre queda holgura
            // o se atraviesa, y el contorno no es el del objeto. `HARUKA_PROP_BOXES=1` vuelve a las
            // primitivas por si hace falta comparar.
            static const bool s_forcePrims = [] {
                const char* e = std::getenv("HARUKA_PROP_BOXES");
                return e && e[0] == '1';
            }();
            const std::vector<int>& shapeIds = propMeshShapesFor(io.prototype);
            const int partIdx = (b.partId >= 0) ? b.partId : 0;
            const int shapeId = (!s_forcePrims && partIdx < (int)shapeIds.size())
                              ? shapeIds[(size_t)partIdx] : -1;
            if (shapeId >= 0) {
                // La malla está en el marco local SIN escalar y con la base en el origen, así que
                // la instancia solo aporta sitio, giro y escala. `wp` es el punto de superficie y
                // la base de rotación es la MISMA que usa el render (ver `propInstanceBasis`).
                phys->addPropMeshInstance(shapeId, wp,
                                          Haruka::Planet::propInstanceBasis(io.dir, io.yaw),
                                          (double)io.scale);
            } else if (b.isCone && !s_forcePrims) {
                phys->addPropCone(b.center, b.halfExtents.y, b.rTop, b.rBottom, b.rot);
            } else {
                phys->addPropOBB(b.center, b.halfExtents, b.rot);
            }
            ++nBoxes;
            perProtoBoxes[(size_t)io.prototype]++;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    // Sonda: sin esto, "cuántas cajas caben" es una opinión. Reporta CADA refresco, con un contador
    // y la posición: es lo que distingue "los colliders no siguen al jugador" de "los colliders no
    // se generan". Estando quieto solo se refresca una vez, así que la duda solo se resuelve
    // andando — y entonces esta línea tiene que repetirse cada ~30 m recorridos.
    static uint32_t s_refreshN = 0;
    ++s_refreshN;
    // Sonda de ESCALA: compara el tamaño del collider con el de la malla dibujada. Si el árbol se
    // ve gordo y el cono es un hilo, aquí se ve el factor.
    {
        static bool s_once = false;
        if (!s_once) {
            for (const auto& io : m_propRegistry.instances()) {
                if (io.prototype < 0 || io.prototype >= protoCount) continue;
                if (m_propRegistry.prototype(io.prototype).name.find("tree") == std::string::npos) continue;
                const auto& pp = protoParts[(size_t)io.prototype];
                if (pp.empty()) break;
                s_once = true;
                HARUKA_LOGD("PropCollider", "ESCALA: io.scale=%.3f · tronco malla r=%.3f h=%.3f "
                            "-> collider r=%.3f halfH=%.3f (x%.2f)",
                            io.scale, pp[0].radiusA, pp[0].length,
                            pp[0].radiusA * io.scale, pp[0].half.y * io.scale, io.scale);
                break;
            }
        }
    }

    std::string desglose;
    for (int pi = 0; pi < protoCount; ++pi) {
        desglose += " · " + m_propRegistry.prototype(pi).name + "=" +
                    std::to_string(perProtoProps[(size_t)pi]) + "props/" +
                    std::to_string(perProtoBoxes[(size_t)pi]) + "cajas";
    }
    HARUKA_LOGD("PropCollider", "#%u · %zu props -> %zu cajas (%.2f ms)%s",
                s_refreshN, nProps, nBoxes, ms, desglose.c_str());
}

unsigned int Application::getMaterialTextureGL(const std::string& path) {
    RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(path);
    if (!RHI::valid(tex)) return 0;
    RHI::Device* dev = RHI::device();
    return dev ? dev->nativeTexture(tex) : 0;
}

void Application::renderMaterialPreview(const Haruka::MaterialComponent& material,
                                        Haruka::Renderer::RenderTarget& target) {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(target.getPass())) return;

    // PSO propio, no el de la escena: `m_scenePSO` alterna entre final.frag y preview.frag según los
    // ajustes de render del usuario, y una preview que cambia de aspecto al tocar un ajuste global
    // no sirve para juzgar un material. Éste se queda SIEMPRE en el look final.
    if (!RHI::valid(m_matPreviewPSO)) {
        const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
        const std::string fsPath = Shader::baseDir() + "shaders/final.frag";
        using V = Haruka::Renderer::Vertex;
        RHI::PipelineDesc pd;
        pd.vertexPath           = vsPath.c_str();
        pd.fragmentPath         = fsPath.c_str();
        pd.vertexLayout.strides = { (uint32_t)(sizeof(V)) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
            { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
            { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
            { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
            { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
        };
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_matPreviewPSO = dev->createPipeline(pd);
    }
    if (!RHI::valid(m_matPreviewPSO)) return;
    if (!RHI::valid(m_uboPerFrameH))
        m_uboPerFrameH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
    if (!RHI::valid(m_uboPerObjectH))
        m_uboPerObjectH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);

    SimpleMesh* sphere = AppInternal::getPrimitiveMesh(Haruka::PrimitiveType::SPHERE);
    if (!sphere) return;

    // Cámara e iluminación FIJAS: dos materiales solo se pueden comparar si se miran igual. La luz
    // de tres cuartos (arriba-izquierda-delante) es la que enseña a la vez el lado iluminado, el
    // terminador y el rim — que es donde se lee un material.
    // La esfera primitiva es de radio 1 y el FOV es de 35°: a distancia d subtiende asin(1/d), que
    // tiene que caber en el semiángulo de 17.5°. A 2.6 la esfera era MÁS GRANDE que el encuadre y
    // salía recortada en un cuadrado con las esquinas redondeadas. 4.0 → 14.5°, con aire alrededor.
    const float kDist = 4.0f;
    PerFrameUBOData f{};
    f.view       = glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0, 0, kDist), glm::vec3(0), glm::vec3(0, 1, 0))));
    f.projection = glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, 100.0f);
    f.cameraPos  = glm::vec3(0, 0, kDist);
    f.sunDirection    = glm::normalize(glm::vec3(-0.45f, 0.65f, 0.62f));
    f.sunLightColor   = glm::vec3(1.0f, 0.97f, 0.92f);
    f.ambientStrength = 0.25f;
    f.enableHDR = 1; f.enableBloom = 0; f.enableSSAO = 0; f.enableIBL = 0; f.enableShadows = 0;
    f.moonDirection = glm::vec3(0, 1, 0); f.moonIntensity = 0.0f;
    f.moonLightColor = glm::vec3(0.0f);
    dev->updateBuffer(m_uboPerFrameH, 0, sizeof(f), &f);

    PerObjectUBOData o{};
    o.model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -kDist));  // la view solo lleva rotación
    o.baseColorAndPlanetRadius = glm::vec4(material.albedo, 1.0f);
    o.planetCenterAndFlag      = glm::vec4(0.0f);
    o.materialEmission         = glm::vec4(material.emission, 0.0f);

    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues clear;
    clear.clearColor = true;
    clear.color[0] = 0.09f; clear.color[1] = 0.10f; clear.color[2] = 0.12f; clear.color[3] = 1.0f;
    clear.clearDepth = true; clear.depth = 0.0f;   // reversed-Z
    ctx->beginRenderPass(target.getPass(), clear);
    ctx->bindPipeline(m_matPreviewPSO);

    int texMask = 0;
    auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
        auto it = material.textures.find(key);
        if (it == material.textures.end() || it->second.empty()) return;
        RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
        if (!RHI::valid(tex)) return;
        ctx->bindTexture(unit, tex);
        texMask |= bit;
    };
    bindSlot("albedo",    0, kTexAlbedo);
    bindSlot("normal",    1, kTexNormal);
    bindSlot("metallic",  2, kTexMetallic);
    bindSlot("roughness", 3, kTexRoughness);
    bindSlot("ao",        4, kTexAO);
    o.materialPBR = glm::vec4(material.metallic, material.roughness, material.ao, (float)texMask);
    dev->updateBuffer(m_uboPerObjectH, 0, sizeof(o), &o);

    ctx->bindUniformBuffer(0, m_uboPerFrameH);
    ctx->bindUniformBuffer(1, m_uboPerObjectH);
    sphere->drawRHI(*ctx);
    ctx->endRenderPass();

    // Devolver la pantalla: en GL el fin de pase no desata nada y quien llama (el editor) dibuja
    // su ImGui a continuación. Misma razón que en renderFrameContent.
    RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
    ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);
    ctx->endRenderPass();
}

void Application::renderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> elapsed = now - _frameStart;
    _frameStart = now;

    deltaTime = elapsed.count();
    _lastFrameTimeMs = deltaTime * 1000.0f;

#ifdef HARUKA_NETWORK
    if (_currentScene) {
        for (const auto& transfer : m_dgs.pollEntities()) {
            auto obj = std::make_shared<Haruka::SceneObject>();
            obj->name = "entity_" + std::to_string(transfer.uuid);
            obj->type = (transfer.type == DGS::ENT_PLAYER) ? "Character" :
                        (transfer.type == DGS::ENT_NPC)    ? "Character" : "Spacecraft";
            obj->position = Haruka::WorldPos(
                transfer.chunkX * Haruka::Units::KM + transfer.pos[0],
                transfer.chunkY * Haruka::Units::KM + transfer.pos[1],
                transfer.chunkZ * Haruka::Units::KM + transfer.pos[2]
            );
            _currentScene->addLoadedObject(obj);
        }
    }
#endif

    // In standalone mode (run()), the main loop handles swap + FPS.
    // In editor mode (no _window), renderFrame() is called externally
    // and the editor manages the swap.
    buildRenderQueue();
    renderFrameContent();

    if (!_window) return; // editor path — caller handles swap

    if (RHI::device())
        RHI::device()->endFrame();

    _fpsFrameCount++;
    _fpsLastTime += deltaTime;
    if (_fpsLastTime >= 1.0) {
        _lastFps      = static_cast<float>(_fpsFrameCount / _fpsLastTime);
        _fpsFrameCount = 0;
        _fpsLastTime   = 0.0;
    }
}

void Application::requestScreenshot(const std::string& path) {
    m_screenshotPath    = path;
    m_screenshotPending = true;
}

void Application::captureScreenshotIfPending(int width, int height) {
    if (!m_screenshotPending || width <= 0 || height <= 0) return;
    m_screenshotPending = false;

    // ⚠️ EL TAMAÑO SALE DEL FRAMEBUFFER REAL, NO DE LA VENTANA. Medido: con una ventana de
    // 1920x1080 el swapchain de Vulkan era de 1280x720, así que la copia escribía filas de 1280 en
    // un buffer que se leía como de 1920 — la captura salía REPETIDA en horizontal y comprimida en
    // vertical. El tamaño lógico de SDL y el del framebuffer no tienen por qué coincidir (escalado
    // del compositor, HiDPI, o un swapchain creado a otro tamaño).
    if (RHI::Device* dev = RHI::device()) {
        uint32_t fbw = 0, fbh = 0;
        dev->framebufferSize(fbw, fbh);
        if (fbw > 0 && fbh > 0) { width = (int)fbw; height = (int)fbh; }
    }

    // Read the composited back buffer (RGBA8), then flip rows (GL is bottom-up).
    std::vector<unsigned char> buf((size_t)width * height * 4);
    if (RHI::Device* dev = RHI::device()) {
        dev->readPixels(0, 0, width, height, RHI::Format::RGBA8, buf.data());
    }

    // ⚠️ EL VOLTEO ES SOLO DE OPENGL. GL tiene el origen ABAJO-izquierda, así que hay que dar la
    // vuelta a las filas; las imágenes de Vulkan son top-down y ya vienen en el orden bueno.
    // Volteando siempre, la captura de Vulkan salía DEL REVÉS — y como hasta ahora el mundo se veía
    // negro, el fallo estaba escondido: solo se notó cuando por fin hubo algo que mirar.
    const bool flipRows = !RHI::device() || RHI::device()->backend() == RHI::Backend::OpenGL;
    std::vector<unsigned char> flipped((size_t)width * height * 4);
    const size_t stride = (size_t)width * 4;
    if (flipRows) {
        for (int y = 0; y < height; ++y)
            std::memcpy(&flipped[(size_t)y * stride], &buf[(size_t)(height - 1 - y) * stride], stride);
    } else {
        flipped = buf;
    }

    std::string path = m_screenshotPath;
    if (path.empty()) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "screenshots/shot_%Y%m%d_%H%M%S.png", &tm);
        path = name;
    }

    if (Haruka::writePNG(path, width, height, 4, flipped.data()))
        std::cout << "[Application] Screenshot saved: " << path << std::endl;
    else
        std::cerr << "[Application] Screenshot FAILED: " << path << std::endl;
}


}} // namespace Haruka::Core
