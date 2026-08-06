// Application — render pipeline.
// The per-frame work: build the render queue, the deferred/forward object pass,
// terrain/islands/water passes, the standalone post-processing composite and the
// GPU timer. Lifecycle/orchestration lives in application.cpp; the GL asset
// caches in application_assets.cpp.

// glm/gtx/* (usado por glm::rotation del pase de props) exige la macro en GLM moderno.
#define GLM_ENABLE_EXPERIMENTAL

#include <chrono>
#include <cmath>
#include "application.h"
#include "application_internal.h"
#include "rhi/rhi_context.h"   // ruta PSO: comandos de dibujo del frame (bloom migrado)

#include <algorithm>

#include "core/logger.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>   // glm::mat4_cast (orientar el prototipo al suelo, pase de props)
#include <glm/gtx/quaternion.hpp>   // glm::rotation (eje→cuaternión: up del prototipo → dir radial)

#include "game/planetary_system.h"
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
};
static_assert(sizeof(SkyParams) == 144, "SkyParams std140 size mismatch");
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
    const int iterations = 5;
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
        _worldSystem->advanceCelestial(deltaTime > 0.0f ? (double)deltaTime : 0.016);
    }

    // NEAR PLANE DINÁMICO: pegado a cualquier superficie (alt ≤ 2 km) near=0.1 (precisión cercana
    // para terreno/objetos/ítem en mano). En órbita el near sube con la altitud → la precisión del
    // depth lejano mejora ~millones× → se acaba el z-fight agua↔lecho desde el espacio. Usa el
    // planeta MÁS CERCANO (no el home) para no recortar superficies cercanas en un sobrevuelo.
    if (_camera && _planetarySystem) {
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        double nearestAlt = 1e30;
        for (const auto& pl : _planetarySystem->getPlanets()) {
            double a = glm::length(camD - pl.position) - pl.radius;
            if (a < nearestAlt) nearestAlt = a;
        }
        float dynNear = 0.1f;
        if (nearestAlt > 2000.0)
            dynNear = glm::clamp((float)((nearestAlt - 2000.0) * 0.05), 0.1f, (float)(nearestAlt * 0.5));
        _camera->setNearPlane(dynNear);
    }

    // Props del mundo: refresca el scatter global si la cámara cruzó un tramo (caché por posición).
    // Debe correr ANTES del render pass de escena, donde el pase instanciado lee m_propRegistry.
    refreshPropScatter();

    // Cielo atmosférico: color por elevación solar + altitud (azul de día → cálido al
    // amanecer/atardecer → oscuro de noche → negro en el espacio). Fallback oscuro.
    glm::vec3 sky(0.01f);
    if (_worldSystem && _camera) sky = _worldSystem->getSkyColor(glm::dvec3(_camera->position));
    RHI::Device* frameDev = RHI::device();
    RHI::Context* frameCtx = frameDev ? frameDev->beginFrame() : nullptr;
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

    // Pase de cielo procedural (gradiente + sol + estrellas) como FONDO: triángulo
    // fullscreen SIN escribir profundidad → el terreno/objetos se pintan encima.
    if (_worldSystem && _camera && _planetarySystem) {
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
            ctx->bindPipeline(m_skyPSO);                             // programa + depth off (no escribe z)
            ctx->bindUniformBuffer(5, m_skyUBO);
            ctx->draw(3);                                            // sin vertex buffer: gl_VertexID
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
        {
            float el  = _worldSystem ? _worldSystem->getSunElevation(glm::dvec3(cameraOrigin)) : 1.0f;
            float day = glm::smoothstep(-0.10f, 0.25f, el);
            // Suelo nocturno subido (0.04→0.11): el lado noche era casi negro → parecía "media
            // planeta sin dibujar". Ahora se ve TENUE (luz de estrellas/cielo), sigue siendo noche.
            frameData.ambientStrength = glm::mix(0.11f, 0.28f, day);
        }
        // El terreno del planeta usa la MISMA luz que el cielo: sin esto el SimplePlanet
        // iluminaba con una dirección fija y no respondía al sol que se ve en el cielo.
        if (_planetarySystem)
            _planetarySystem->setSunLight(frameData.sunDirection, frameData.sunLightColor,
                                          frameData.ambientStrength);
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
                    if (RHI::valid(gm.albedo))    sceneCtx->bindTexture(0, gm.albedo);
                    if (RHI::valid(gm.normal))    sceneCtx->bindTexture(1, gm.normal);
                    if (RHI::valid(gm.metallic))  sceneCtx->bindTexture(2, gm.metallic);
                    if (RHI::valid(gm.roughness)) sceneCtx->bindTexture(3, gm.roughness);
                    if (RHI::valid(gm.ao))        sceneCtx->bindTexture(4, gm.ao);
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
                };
                Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
                pd.topology     = RHI::PrimitiveTopology::Triangles;
                pd.depth.test   = true;  pd.depth.write = true;
                pd.blend.enable = false;
                pd.cull         = RHI::CullMode::Back;
                m_propInstPSO  = uboDev->createPipeline(pd);
            }

            // UBO del pase (binding 6): viento + tiempo + escalares del material del prototipo.
            if (!RHI::valid(m_propParamsUBO)) {
                m_propParamsUBO = uboDev->createBuffer(RHI::BufferUsage::Uniform,
                                                       sizeof(PropParams), nullptr, RHI::BufferMemory::Dynamic);
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
                auto propCull = [&](const glm::vec3& posF, float radius) -> uint8_t {
                    const float dist = glm::length(posF);
                    if (dist <= 1e-3f) return 0;
                    const float fwd = glm::dot(posF, camF);
                    if (fwd < -radius) return 1;                      // detrás de la cámara
                    if (std::abs(glm::dot(posF, camR)) > fwd * tanH + radius) return 1;
                    if (std::abs(glm::dot(posF, camU)) > fwd * tanV + radius) return 1;
                    if ((radius / dist) / tanV * vpPx < kMinPropPixels) return 2;   // sub-pixel
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
                if ((int)s_propBuckets.size() != protoCount) {
                    s_propBuckets.resize((size_t)protoCount);
                    s_aliveCounts.resize((size_t)protoCount);
                    s_drawnCounts.resize((size_t)protoCount);
                }
                for (int pi = 0; pi < protoCount; ++pi) {
                    s_propBuckets[(size_t)pi].clear();
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
                for (const auto& io : m_propRegistry.instances()) {
                    const int pi = io.prototype;
                    if (pi < 0 || pi >= protoCount) continue;
                    const bool alive = io.state == (uint32_t)Haruka::InstancedObjectState::Alive;

                    uint8_t cull = 0;
                    if (alive) {
                        ++s_aliveCounts[(size_t)pi];
                        const glm::vec3 posF = glm::vec3(
                            camToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
                        cull = propCull(posF, io.scale * 8.0f);
                        if (cull == 0) {
                            glm::mat4 m = glm::translate(glm::mat4(1.0f), posF);
                            m *= glm::mat4_cast(glm::rotation(up, io.dir));
                            m = glm::rotate(m, io.yaw, io.dir);
                            m = glm::scale(m, glm::vec3(io.scale));
                            Haruka::InstanceDataFloat inst;
                            inst.model = m;
                            inst.color = glm::vec4(io.tint, 1.0f);
                            inst.scale = glm::vec3(io.scale);
                            s_propBuckets[(size_t)pi].push_back(inst);
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

                for (int pi = 0; pi < protoCount; ++pi) {
                    const Haruka::InstancedPrototype& proto = m_propRegistry.prototype(pi);
                    if (proto.name.empty()) continue;

                    // 1. Malla prototipo (una vez): horneada con la seed del tipo. La cache la clavea
                    //    el NOMBRE del prototipo (el "tipo" del scatter): árbol por TreeMeshNode, y roca/
                    //    casa por los bakes procedurales de prop_mesh.h (misma paridad determinista).
                    auto itMesh = m_propProtoMesh.find(proto.name);
                    if (itMesh == m_propProtoMesh.end()) {
                        Haruka::Tools::ProcGraph::TreeMeshData tm;
                        const std::string& nm = proto.name;
                        const bool isRock  = nm.find("rock")  != std::string::npos ||
                                             nm.find("roca")  != std::string::npos;
                        const bool isHouse = nm.find("house") != std::string::npos ||
                                             nm.find("casa")  != std::string::npos;
                        if (isRock) {
                            tm = Haruka::Tools::ProcGraph::bakeRockMesh((int)proto.meshSeed, 1.0f, 0.72f);
                        } else if (isHouse) {
                            tm = Haruka::Tools::ProcGraph::bakeHouseMesh((int)proto.meshSeed, 1.0f);
                        } else {
                            Haruka::Tools::ProcGraph::Graph g;
                            int tree = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
                                (int)proto.meshSeed, 6.5f, 0.35f, 1.0f, 8);
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
                                verts.push_back(pv);
                            }
                            PropPrototypeGpu pg;
                            pg.vbo = uboDev->createBuffer(RHI::BufferUsage::Vertex,
                                                          verts.size() * sizeof(PropVertex), verts.data());
                            pg.ebo = uboDev->createBuffer(RHI::BufferUsage::Index,
                                                          tm.indices.size() * sizeof(unsigned int),
                                                          tm.indices.data());
                            pg.indexCount = (uint32_t)tm.indices.size();
                            pg.vertexCount = (uint32_t)verts.size();
                            // 2. Material PER-PIXEL del prototipo (una vez): texturas horneadas con la
                            //    seed del tipo y subidas a GPU. mask != 0 → el pase enlaza texturas y
                            //    el editor deja de marcar "⚠ sin per-pixel" en la jerarquía.
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
                    if (!RHI::valid(pg.vbo) || !RHI::valid(pg.ebo) || pg.indexCount == 0) continue;

                    // Per-pixel REAL del prototipo (mask != 0 → texturas enlazadas en el draw). Se marca
                    // ANTES del `continue` de bucket vacío: un prototipo sin instancias visibles este
                    // frame no deja de tener material per-pixel en la jerarquía del editor.
                    if (snapshotDbg)
                        m_propScatterDebug[(size_t)pi].hasPerPixel = pg.mask != 0u;

                    // 2. Stats + buffer del prototipo desde el barrido ÚNICO de arriba. TOTAL = TODAS
                    //    las instancias vivas (lo que el frame podía dibujar sin cull); RENDERED = las
                    //    que entraron al buffer tras el cull + el tope (20000). `setInstances` recorta
                    //    el exceso igual que hacía `addInstance`.
                    const int aliveN = s_aliveCounts[(size_t)pi];
                    totalVertices  += pg.vertexCount * aliveN;
                    totalTriangles += (pg.indexCount / 3) * aliveN;
                    ++totalDrawCalls;
                    if (s_propBuckets[(size_t)pi].empty()) continue;
                    _instancing->setInstances(s_propBuckets[(size_t)pi]);
                    const int drawnN = _instancing->getInstanceCount();
                    s_drawnCounts[(size_t)pi] = drawnN;
                    renderedVertices  += pg.vertexCount * drawnN;
                    renderedTriangles += (pg.indexCount / 3) * drawnN;

                    // 3. Material PER-PIXEL del PROTOTIPO: texturas horneadas (bakePropPrototypeMaterial)
                    //    enlazadas a los slots 0..4; la máscara y los escalares van en `PropParams.matPBR`.
                    //    mask == 0 → fallback al color por vértice (tinte por instancia) sin texturas.
                    PropParams pp{};
                    pp.wind = windWorld;
                    pp.time = propTime;
                    pp.matPBR = glm::vec4(pg.metallicS, pg.roughnessS, pg.aoS, (float)pg.mask);

                    // 4. Un draw con todas las instancias del prototipo. El buffer de instancias es
                    //    fijo (m_maxInstances) y setInstances recorta el exceso — un draw por tipo.
                    sceneCtx->bindPipeline(m_propInstPSO);   // re-bind (creación perezosa de buffers)
                    sceneCtx->bindVertexBuffer(pg.vbo, 0);
                    sceneCtx->bindIndexBuffer(pg.ebo);
                    if (RHI::valid(pg.albedo))    sceneCtx->bindTexture(0, pg.albedo);
                    if (RHI::valid(pg.normal))    sceneCtx->bindTexture(1, pg.normal);
                    if (RHI::valid(pg.metallic))  sceneCtx->bindTexture(2, pg.metallic);
                    if (RHI::valid(pg.roughness)) sceneCtx->bindTexture(3, pg.roughness);
                    if (RHI::valid(pg.ao))        sceneCtx->bindTexture(4, pg.ao);
                    uboDev->updateBuffer(m_propParamsUBO, 0, sizeof(pp), &pp);
                    sceneCtx->bindUniformBuffer(6, m_propParamsUBO);
                    _instancing->render(sceneCtx, pg.indexCount, 1);
                    ++renderedDrawCalls;
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
                            std::snprintf(tmp, sizeof(tmp), " p%d:%d/%d/%d", di,
                                          s_aliveCounts[(size_t)di],
                                          (int)s_propBuckets[(size_t)di].size(),
                                          s_drawnCounts[(size_t)di]);
                            line += tmp;
                        }
                        HARUKA_LOGI("PropDiag", "protoCount=%d instances=%zu%s", protoCount,
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
                sceneCtx->beginRenderPass(_shadow->pass(), shadowClear);
                _gameInterface->onRenderShadow(lightSpace, glm::vec3(_camera->position));
                shadowsOn = true;
                sceneCtx->endRenderPass();
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
                sceneCtx->beginRenderPass(m_skyMask->pass(), maskClear);
                _gameInterface->onRenderShadow(m_skySpace, glm::vec3(_camera->position));
                m_skyMaskOn = true;
                sceneCtx->endRenderPass();
                }
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
                for (size_t i = 0; i < _planetarySystem->getSimplePlanetCount(); ++i) {
                    const auto& sp = _planetarySystem->getSimplePlanet(i);
                    _planetarySystem->renderSimplePlanet(sp.name,
                        glm::dvec3(_camera->position), projC, viewC);
                }
            }

            // Stats del TERRENO (malla base + clipmap + agua): se suman a los contadores del frame
            // y se guardan para el panel. TOTAL == RENDERED aquí (el planeta dibuja todo lo que
            // tiene; solo el agua culla las caras tras el planeta, que es lo que refleja el desglose).
            m_terrainStats = _planetarySystem->getTerrainRenderStats();
            totalDrawCalls  += m_terrainStats.drawCalls;
            totalVertices   += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices + m_terrainStats.waterVertices);
            totalTriangles  += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles + m_terrainStats.waterTriangles);
            renderedDrawCalls += m_terrainStats.drawCalls;
            renderedVertices  += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices + m_terrainStats.waterVertices);
            renderedTriangles += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles + m_terrainStats.waterTriangles);
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
        if (_planetarySystem) {
            glm::dvec3 pc; double pr;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                const glm::dvec3 r = camD - pc; const double rl = glm::length(r);
                if (rl > 1e-9) upD = r / rl;
            }
        }
        Haruka::PrecipitationRenderer::Params pp;
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

    // --- Post-processing composite -----------------------------------------
    // Resolve the offscreen scene target to the screen: upscales the render-scaled
    // image and applies the present-time effects (FXAA, and later bloom). With
    // FXAA off and scale 1.0 the scene never took this path (m_postActive false).
    // === PASE 2 MIGRADO A PSO/Context: el present/composite (FXAA + bloom + upscale). ===
    if (m_postActive && _postScene) {
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
            RHI::ClearValues keep;                       // el quad cubre la pantalla entera
            keep.clearColor = false; keep.clearDepth = false;

            ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);  // id 0 = backbuffer (pantalla)
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
    const std::vector<Haruka::Planet::ScatteredProp> placed =
        Haruka::Planet::scatterPropsNear(field, camPos, planetC, params, table);

    // El scatter regenera las instancias desde CERO en cada refresco: preserva el ESTADO que el
    // juego marcó (destruido/rebrotando) por seed determinista — el árbol que tumbaste sigue caído
    // al volver, en vez de reaparecer porque el registro se recalculó.
    std::unordered_map<uint32_t, std::pair<uint32_t, float>> savedState;
    for (const auto& io : m_propRegistry.instances())
        if (io.state != (uint32_t)Haruka::InstancedObjectState::Alive)
            savedState[io.seed] = { io.state, io.regrow };

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
        auto it = savedState.find(io.seed);
        if (it != savedState.end()) { io.state = it->second.first; io.regrow = it->second.second; }
        objs.push_back(io);
    }
    m_propRegistry.setInstances(std::move(objs));
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

    // Read the composited back buffer (RGBA8), then flip rows (GL is bottom-up).
    std::vector<unsigned char> buf((size_t)width * height * 4);
    if (RHI::Device* dev = RHI::device()) {
        dev->readPixels(0, 0, width, height, RHI::Format::RGBA8, buf.data());
    }

    std::vector<unsigned char> flipped((size_t)width * height * 4);
    const size_t stride = (size_t)width * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(&flipped[(size_t)y * stride], &buf[(size_t)(height - 1 - y) * stride], stride);

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
