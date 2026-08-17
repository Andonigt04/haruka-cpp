/**
 * @file planet.cpp
 * @brief TerrestrialPlanet — la implementación de un planeta que se crea a sí mismo.
 *
 * Migración de `PlanetarySystem::SimplePlanetInternal`: TODO lo que antes vivía dentro del sistema
 * (shaders inline, construcción de malla, mapas de zona/elevación, nivel del mar, texturas de bioma,
 * clipmap, render) es ahora de la clase `TerrestrialPlanet`. El PlanetarySystem solo orquesta.
 */
#include "planet.h"

#include "core/terrain/cube_sphere.h"
#include "core/planet/terrain_detail.h"
#include "core/planet/terrain_lod.h"
#include "renderer/shader.h"            // Shader::baseDir() — raíz de assets para las rutas de shader
#include "core/asset_paths.h"
#include "stb_image.h"
#include "tools/procgraph/proc_math.h"
#include "tools/procgraph/proc_noise.h"
#include "tools/procgraph/proc_climate.h"
#include "tools/procgraph/proc_texture.h"
#include "core/noise_generator.h"
#include "settings/game_settings.h"
#include "settings/settings_manager.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "tools/profiler.h"          // HARUKA_PROFILE: sub-scopes de simple_planet.draw (base/clipmap/agua)
#include "core/logger.h"
#include "io/image_writer.h"
#include <thread>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <utility>
#include <chrono>
#include <unordered_set>
#include <unordered_map>

namespace Haruka { namespace Planet {

using namespace Haruka::Tools::ProcGraph;

// ── Simple Single-Mesh Planet ───────────────────────────────────────────
// Vértice de la malla del planeta: posición, normal de TERRENO (no de esfera), uv placeholder,
// color de bioma y los datos de clima que usa el shader de biomas (elev/km, temp, humedad).
struct SimplePlanetVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float cr, cg, cb;
    float elev, temp, humid;
};

namespace {

// Resolución objetivo (lado largo en px) según la calidad de terreno activa. 0 = usar el maestro
// completo, sin degradar. La escalera antigua era por CARPETAS (`src/16k/8k/4k/hd`) y con Ultra
// caía en `src` (a menudo 512 px). Ahora el MAESTRO es siempre la imagen de mayor área disponible y
// la calidad decide cuánto se reduce antes de subirla a la GPU.
int terrainQualityTarget() {
    switch (SettingsManager::get().graphics().terrainQuality) {
        case Settings::TerrainQuality::Ultra:  return 0;      // maestro completo
        case Settings::TerrainQuality::High:   return 8192;
        case Settings::TerrainQuality::Medium: return 4096;
        default:
        case Settings::TerrainQuality::Low:    return 2048;
    }
}

// Factor de reducción (>=1; 1 = sin cambio) para llevar `size` al objetivo de calidad. Nunca
// sube de resolución: un maestro más pequeño que el objetivo se deja tal cual.
int qualityDownscaleFactor(int size, int target) {
    if (target <= 0) return 1;
    const int f = size / target;
    return f >= 2 ? f : 1;
}

// Downsample por CAJAS: media de cada bloque fxf. Factor entero (nada de alias de muestreo) y la
// trunca la última fila/columna si el tamaño no es múltiplo — en la práctica las texturas de
// terreno son potencias de 2.
void boxDownsample(const unsigned char* src, int sw, int sh, int factor,
                   std::vector<unsigned char>& out) {
    const int dw = sw / factor, dh = sh / factor;
    out.resize((size_t)dw * dh * 4);
    const int np = factor * factor;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i) {
                    const unsigned char* p = src + ((size_t)(y * factor + j) * sw + x * factor + i) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                }
            unsigned char* o = out.data() + ((size_t)y * dw + x) * 4;
            o[0] = (unsigned char)(r / np); o[1] = (unsigned char)(g / np);
            o[2] = (unsigned char)(b / np); o[3] = (unsigned char)(a / np);
        }
    }
}

}  // namespace

// Shaders y UBO compartidos entre TODOS los planetas (estáticos).
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_pipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_texPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_biomePipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_tessPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_cullPipeline;

namespace {
/// Ajuste con OVERRIDE por entorno. El valor de `SettingsManager` es el que manda; la variable solo
/// existe para forzar on/off sin tocar el .ini ni recompilar — que es lo que uno quiere cuando algo
/// se ve mal y hay que saber en UN arranque si es esa feature. Ausente = manda el ajuste.
bool settingWithEnvOverride(bool setting, const char* envVar) {
    if (const char* e = std::getenv(envVar))
        if (e[0] == '0' || e[0] == '1') return e[0] == '1';
    return setting;
}
} // namespace
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_clipPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_oceanPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_oceanFarPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_nearRingPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_wirePipeline;
Haruka::RHI::BufferHandle TerrestrialPlanet::s_ubo;
bool TerrestrialPlanet::s_shadersReady = false;

TerrestrialPlanet::TerrestrialPlanet() = default;
TerrestrialPlanet::~TerrestrialPlanet() { clearGPU(); }

void TerrestrialPlanet::setOrbit(int parent, double a, double ecc, double period, double phase,
                                 const glm::dvec3& u, const glm::dvec3& v) {
    // Sobrecarga HEREDADA: recibe la base (u,v) que el llamador derivó de la posición. Se convierte a
    // elementos para que la órbita pueda precesar — con la base guardada tal cual, el plano quedaba
    // congelado y la órbita era exactamente periódica (ver core/planet/orbit.h).
    Haruka::Planet::OrbitElements el;
    el.a = a; el.e = ecc; el.period = period; el.meanAnom0 = phase;
    Haruka::Planet::orbitElementsFromBasis(u, v, el.incRad, el.nodeRad, el.argPRad);
    Haruka::Planet::defaultPrecession(m_config.seed, el);
    setOrbit(parent, el);
}

void TerrestrialPlanet::setOrbit(int parent, const Haruka::Planet::OrbitElements& el) {
    m_config.orbitParent = parent;
    m_config.orbit       = el;
    // Espejos planos, que son los que expone el API pública y los que lee el editor.
    m_config.orbitA      = el.a;
    m_config.orbitEcc    = el.e;
    m_config.orbitPeriod = el.period;
    m_config.orbitPhase  = el.meanAnom0;
}

void TerrestrialPlanet::ensureShaders() {
    if (s_shadersReady) return;
    s_shadersReady = true;
    RHI::Device* dev = RHI::device();
    if (!dev) { HARUKA_LOGE("SimplePlanet", "no RHI device"); return; }

    // Los shaders del planeta viven en assets/shaders/planet/, no incrustados aquí. El RHI
    // los lee del disco (`PipelineDesc::*Path`) y resuelve sus #include contra el mismo
    // directorio, que es lo que permite compartir lib/terrain_detail.glsl con la CPU.
    //
    // ⚠️ Las rutas tienen que seguir VIVAS mientras se llama a createPipeline: PipelineDesc
    // guarda `const char*`, no copia. Por eso son locales de esta función y no temporales.
    const std::string shaderDir = Shader::baseDir() + "shaders/planet/";
    const std::string pBiomeFrag       = shaderDir + "biome.frag";
    const std::string pClipCtrl        = shaderDir + "clipmap.tesc";
    const std::string pTerrainCull     = shaderDir + "terrain_cull.comp";
    const std::string pClipEval        = shaderDir + "clipmap.tese";
    const std::string pClipVert        = shaderDir + "clipmap.vert";
    const std::string pSimpleFrag      = shaderDir + "simple.frag";
    const std::string pSimpleVert      = shaderDir + "simple.vert";
    const std::string pTessCtrl        = shaderDir + "terrain.tesc";
    const std::string pTessEval        = shaderDir + "terrain.tese";
    const std::string pTessVert        = shaderDir + "terrain.vert";
    const std::string pTexFrag         = shaderDir + "textured.frag";
    const std::string pSimpleWireFrag  = shaderDir + "wire.frag";
    const std::string pSimpleWireVert  = shaderDir + "wire.vert";

    // Terrain pipeline (per-vertex color fallback)
    RHI::PipelineDesc pd;
    pd.vertexPath   = pSimpleVert.c_str();
    pd.fragmentPath = pSimpleFrag.c_str();
    pd.vertexLayout.strides = { (uint32_t)(sizeof(SimplePlanetVertex)) };
    const RHI::VertexAttribute climateAttrib = { 4, offsetof(SimplePlanetVertex, elev), RHI::Format::RGB32F, 0 };
    auto withClimate = [&](RHI::VertexLayout vl) -> RHI::VertexLayout {
        vl.attributes.push_back(climateAttrib);
        if (vl.strides.empty())
            vl.strides.push_back((uint32_t)sizeof(SimplePlanetVertex));
        else
            vl.strides[0] = (uint32_t)sizeof(SimplePlanetVertex);
        return vl;
    };

    pd.vertexLayout.attributes = {
        { 0, offsetof(SimplePlanetVertex, px), RHI::Format::RGB32F },
        { 1, offsetof(SimplePlanetVertex, nx), RHI::Format::RGB32F },
        { 2, offsetof(SimplePlanetVertex, u),  RHI::Format::RG32F  },
        { 3, offsetof(SimplePlanetVertex, cr), RHI::Format::RGB32F },
    };
    pd.vertexLayout.strides[0] = (uint32_t)sizeof(SimplePlanetVertex);
    pd.depth.test   = true;  pd.depth.write = true;
    pd.depth.compare = RHI::CompareOp::Greater;
    pd.cull = RHI::CullMode::Back;
    s_pipeline = dev->createPipeline(pd);

    // Textured terrain pipeline — same vertex layout, triplanar fragment shader
    RHI::PipelineDesc tpd;
    tpd.vertexPath   = pSimpleVert.c_str();
    tpd.fragmentPath = pTexFrag.c_str();
    tpd.vertexLayout   = pd.vertexLayout;
    tpd.depth          = pd.depth;
    tpd.cull           = pd.cull;
    s_texPipeline = dev->createPipeline(tpd);

    // Biome-blended pipeline — 4-layer triplanar texturing keyed by biome map
    RHI::PipelineDesc bpd;
    bpd.vertexPath   = pSimpleVert.c_str();
    bpd.fragmentPath = pBiomeFrag.c_str();
    bpd.vertexLayout   = withClimate(pd.vertexLayout);
    bpd.depth          = pd.depth;
    bpd.cull           = pd.cull;
    s_biomePipeline = dev->createPipeline(bpd);

    // Pipeline TESELADO: mismo fragment que el de bioma (las salidas del evaluation coinciden con
    // sus entradas), pero con control + evaluación y entrada de PARCHES.
    {
        RHI::PipelineDesc tpd;
        tpd.vertexPath      = pTessVert.c_str();
        tpd.tessControlPath = pTessCtrl.c_str();
        tpd.tessEvalPath    = pTessEval.c_str();
        tpd.fragmentPath    = pBiomeFrag.c_str();
        tpd.vertexLayout      = withClimate(pd.vertexLayout);
        tpd.topology          = RHI::PrimitiveTopology::Patches;
        tpd.patchVertices     = 4;
        tpd.depth             = pd.depth;
        tpd.cull              = pd.cull;
        s_tessPipeline = dev->createPipeline(tpd);

        RHI::PipelineDesc cpd;
        cpd.vertexPath      = pClipVert.c_str();
        cpd.tessControlPath = pClipCtrl.c_str();
        cpd.tessEvalPath    = pClipEval.c_str();
        cpd.fragmentPath    = pBiomeFrag.c_str();
        cpd.vertexLayout.strides    = { (uint32_t)(2 * sizeof(float)) };
        cpd.vertexLayout.attributes = { { 0, 0, RHI::Format::RG32F } };
        cpd.topology      = RHI::PrimitiveTopology::Patches;
        cpd.patchVertices = 4;
        cpd.depth         = pd.depth;
        // Coplanar con la malla del planeta A PROPÓSITO: las dos son el mismo suelo. El sesgo es lo
        // que decide cuál gana, y tiene que ganar la rejilla fina. Positivo porque en reversed-Z
        // "más cerca" es profundidad mayor. Sin esto no hay z-fighting bonito: gana la que se
        // dibuje después, entera.
        cpd.depth.biasConstant = 64.0f;
        cpd.depth.biasSlope    = 2.0f;
        cpd.cull          = pd.cull;
        s_clipPipeline = dev->createPipeline(cpd);

        // ── SUELO CERCANO DESDE LA COLISIÓN ─────────────────────────────────────────────────────
        //
        // MISMO fragment que el clipmap (`biome.frag`) y mismos bindings: solo cambia de dónde sale
        // la geometría. Ese es el objetivo — que el material, el bioma y las sombras sean los de
        // siempre y lo único distinto sea que los vértices ya no los inventa el teselador, sino que
        // llegan de la física. Sin teselación: triángulos y un índice explícito con la diagonal de
        // Jolt, que es lo que el teselador no puede garantizar.
        RHI::PipelineDesc npd;
        const std::string pNearVert = shaderDir + "nearground.vert";
        npd.vertexPath   = pNearVert.c_str();
        npd.fragmentPath = pBiomeFrag.c_str();
        npd.vertexLayout.strides    = { (uint32_t)(3 * sizeof(float)) };
        npd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 } };
        npd.topology = RHI::PrimitiveTopology::Triangles;
        npd.depth    = pd.depth;
        // Más sesgo que el clipmap: este parche y el clipmap son coplanares dentro de ±256 m y
        // mientras no haya hueco (paso 5 del plan) hay que decidir cuál gana. Tiene que ganar este,
        // que es el que describe lo que se pisa.
        npd.depth.biasConstant = 96.0f;
        npd.depth.biasSlope    = 3.0f;
        npd.cull = pd.cull;
        s_nearRingPipeline = dev->createPipeline(npd);
        HARUKA_LOGI("SimplePlanet", "pipeline suelo cercano: %s",
                    RHI::valid(s_nearRingPipeline) ? "ok" : "FALLO");

        HARUKA_LOGI("SimplePlanet", "pipeline clipmap: %s",
                    RHI::valid(s_clipPipeline) ? "ok" : "FALLO");
        HARUKA_LOGI("SimplePlanet", "pipeline teselado: %s",
                    RHI::valid(s_tessPipeline) ? "ok" : "FALLO (se usa la malla sin teselar)");

        // COMPUTE del culling de parches. Se crea siempre (es barato) pero solo se USA con
        // el ajuste: así el pipeline se valida en cada arranque y el fallo sale en el log
        // aunque el camino esté apagado, en vez de descubrirse el día que alguien lo enciende.
        RHI::PipelineDesc ccd;
        ccd.computePath = pTerrainCull.c_str();
        s_cullPipeline  = dev->createPipeline(ccd);
        HARUKA_LOGI("SimplePlanet", "pipeline culling de parches (compute): %s",
                    RHI::valid(s_cullPipeline) ? "ok" : "FALLO (se dibujan todos los parches)");
    }

    // ── EL MAR ──────────────────────────────────────────────────────────────────────────────────
    //
    // DOS geometrías de UNA superficie, por el mismo motivo que el terreno tiene malla base y
    // clipmap: la esfera del planeta tiene parches de 39 km y el teselador topa en 64, o sea quads
    // de 611 m. Una ola de 60 m NO CABE ahí. Las olas necesitan quads de metros, y eso solo lo da
    // una rejilla anclada bajo la cámara — la del clipmap, que ya existe.
    //
    //   · MAR CERCANO  (`ocean.*`)     — los anillos del clipmap, teselados, con Gerstner. Sin
    //     buffers propios: reusa `m_clipVB`/`m_clipIB` y los `ClipParams` por anillo del terreno.
    //   · MAR LEJANO   (`ocean_far.*`) — la esfera a nivel del mar, lisa. Más allá del clipmap una
    //     ola de 60 m es subpíxel: darle geometría sería pagar vértices por un aliasing.
    //
    // Las dos comparten `ocean.frag`, y la ola se desvanece a 0 antes del borde del clipmap
    // (`ocean.tese`), así que donde se relevan describen la MISMA superficie: la esfera a cota 0.
    //
    // ⚠️ `cull = None` en las dos, y no es descuido: con `Back`, en cuanto la cámara baja del nivel
    // del mar mira la superficie desde DENTRO, donde no hay un solo triángulo front-facing, y el mar
    // entero desaparece. Medido en su día sobre una captura: 0 de 405 triángulos visibles.
    {
        const std::string pOceanVert  = shaderDir + "ocean.vert";
        const std::string pOceanTesc  = shaderDir + "ocean.tesc";
        const std::string pOceanTese  = shaderDir + "ocean.tese";
        const std::string pOceanFrag  = shaderDir + "ocean.frag";
        const std::string pOceanFarV  = shaderDir + "ocean_far.vert";

        RHI::PipelineDesc od;
        od.vertexPath   = pOceanVert.c_str();
        od.tessControlPath = pOceanTesc.c_str();
        od.tessEvalPath = pOceanTese.c_str();
        od.fragmentPath = pOceanFrag.c_str();
        od.vertexLayout.strides    = { (uint32_t)(2 * sizeof(float)) };   // la rejilla del clipmap
        od.vertexLayout.attributes = { { 0, 0, RHI::Format::RG32F } };
        od.topology      = RHI::PrimitiveTopology::Patches;
        od.patchVertices = 4;
        od.depth.test    = true;  od.depth.write = false;
        od.depth.compare = RHI::CompareOp::Greater;
        od.cull          = RHI::CullMode::None;
        od.blend.enable  = true;  od.blend.mode = RHI::BlendMode::Alpha;
        s_oceanPipeline = dev->createPipeline(od);

        RHI::PipelineDesc ofd;
        ofd.vertexPath   = pOceanFarV.c_str();
        ofd.fragmentPath = pOceanFrag.c_str();
        ofd.vertexLayout.strides    = { (uint32_t)(6 * sizeof(float)) };  // pos(3) + normal(3)
        ofd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F },
                                        { 1, (uint32_t)(3 * sizeof(float)), RHI::Format::RGB32F } };
        ofd.topology     = RHI::PrimitiveTopology::Triangles;
        ofd.depth        = od.depth;
        ofd.cull         = RHI::CullMode::None;
        ofd.blend        = od.blend;
        s_oceanFarPipeline = dev->createPipeline(ofd);

        HARUKA_LOGI("SimplePlanet", "pipelines del mar: cercano(olas)=%s · lejano(esfera)=%s",
                    RHI::valid(s_oceanPipeline)    ? "ok" : "FALLO",
                    RHI::valid(s_oceanFarPipeline) ? "ok" : "FALLO");
    }

    // Wireframe pipeline
    RHI::PipelineDesc wire;
    wire.vertexPath   = pSimpleWireVert.c_str();
    wire.fragmentPath = pSimpleWireFrag.c_str();
    wire.vertexLayout   = withClimate({}); // ensure stride matches vertex size
    wire.vertexLayout.attributes = {
        { 0, offsetof(SimplePlanetVertex, px), RHI::Format::RGB32F },
    };
    wire.depth.test   = true;  wire.depth.write = false;
    wire.depth.compare = RHI::CompareOp::Greater;
    wire.cull = RHI::CullMode::None;
    wire.blend.enable = true;
    wire.blend.mode   = RHI::BlendMode::Alpha;
    s_wirePipeline = dev->createPipeline(wire);

    struct SimplePlanetUBO {
        glm::mat4 uMVP;
        glm::vec4 uCenter;
        glm::vec4 uLightDir;
        glm::vec4 uLightColor;
        glm::vec4 uAmbient;
        glm::vec4 uExtra;
        glm::vec4 uDebug;   // x = vista de depuración (ver TerrestrialPlanet::debugView)
        glm::vec4 uTexAnchor;  // xyz = ancla planetaria de las UV de terreno (ver el bloque de render)
    };
    SimplePlanetUBO init{};
    init.uLightDir   = glm::vec4(0.3f, 0.8f, 0.5f, 0.0f);
    init.uLightColor = glm::vec4(1.0f, 0.95f, 0.9f, 1.0f);
    init.uAmbient    = glm::vec4(0.05f, 0.08f, 0.12f, 0.0f);
    init.uExtra      = glm::vec4(0.0f);
    s_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(SimplePlanetUBO), &init, RHI::BufferMemory::Dynamic);
}

void TerrestrialPlanet::cleanupStatics() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(s_pipeline))     { dev->destroy(s_pipeline);     s_pipeline = {}; }
    if (RHI::valid(s_texPipeline))  { dev->destroy(s_texPipeline);  s_texPipeline = {}; }
    if (RHI::valid(s_biomePipeline)){ dev->destroy(s_biomePipeline); s_biomePipeline = {}; }
    if (RHI::valid(s_wirePipeline)) { dev->destroy(s_wirePipeline);  s_wirePipeline = {}; }
    // ⚠️ Estos tres faltaban: los pipelines de teselación y clipmap se creaban y nunca se
    // destruían. Fuga preexistente, no del culling — pero se arregla aquí porque es el mismo sitio.
    if (RHI::valid(s_tessPipeline)) { dev->destroy(s_tessPipeline);  s_tessPipeline = {}; }
    if (RHI::valid(s_clipPipeline)) { dev->destroy(s_clipPipeline);  s_clipPipeline = {}; }
    if (RHI::valid(s_oceanPipeline)) { dev->destroy(s_oceanPipeline); s_oceanPipeline = {}; }
    if (RHI::valid(s_oceanFarPipeline)) { dev->destroy(s_oceanFarPipeline); s_oceanFarPipeline = {}; }
    if (RHI::valid(s_nearRingPipeline)) { dev->destroy(s_nearRingPipeline); s_nearRingPipeline = {}; }
    if (RHI::valid(s_cullPipeline)) { dev->destroy(s_cullPipeline);  s_cullPipeline = {}; }
    if (RHI::valid(s_ubo))          { dev->destroy(s_ubo);           s_ubo = {}; }
    s_shadersReady = false;
}

// ── AGUA INTERIOR: la sim de ríos/lagos publica su SUPERFICIE, no una malla ─────────────────────
//
// La dibuja el MAR, con su misma geometría y su mismo `ocean.frag`. Aquí solo viaja el CAMPO: la
// cota del agua por celda y el marco del parche para localizarla desde una dirección del planeta.
// Es lo que convierte "tres aguas con tres shaders" en una superficie con tres orígenes.
//
// ⚠️ Va en un SSBO y no en una textura porque el RHI no expone actualización de textura: habría que
// destruir y recrear cada frame. El buffer se actualiza con `updateBuffer`, y de paso la bilineal
// se hace a mano en el shader — igual que el resto de campos del terreno, y por el mismo motivo
// (el filtrado del hardware usa pesos de 8 bits en varias GPU).
void TerrestrialPlanet::setInlandWater(const std::vector<float>* surfaceM, int n,
                                       const glm::vec3& anchorRelEye, const glm::vec3& tan,
                                       const glm::vec3& bit, const glm::vec3& up, float spanM) {
    RHI::Device* dev = RHI::device();
    if (!dev) { m_inlandWaterValid = false; return; }
    if (!surfaceM || n <= 1 || (int)surfaceM->size() < n * n) { m_inlandWaterValid = false; return; }

    const size_t bytes = surfaceM->size() * sizeof(float);
    if (!RHI::valid(m_inlandWaterSSBO))
        m_inlandWaterSSBO = dev->createBuffer(RHI::BufferUsage::Storage, bytes, surfaceM->data(),
                                              RHI::BufferMemory::Dynamic);
    else
        dev->updateBuffer(m_inlandWaterSSBO, 0, bytes, surfaceM->data());

    struct InlandParams { glm::vec4 anchor; glm::vec4 tanU; glm::vec4 tanV; glm::vec4 misc; } ip{};
    ip.anchor = glm::vec4(anchorRelEye, 0.0f);   // ancla del parche, relativa al OJO
    ip.tanU   = glm::vec4(tan, 0.0f);
    ip.tanV   = glm::vec4(bit, 0.0f);
    ip.misc   = glm::vec4(spanM, (float)n, 0.0f, 1.0f);   // w = 1 → hay campo
    if (!RHI::valid(m_inlandWaterUBO))
        m_inlandWaterUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ip), &ip,
                                             RHI::BufferMemory::Dynamic);
    else
        dev->updateBuffer(m_inlandWaterUBO, 0, sizeof(ip), &ip);
    m_inlandWaterValid = RHI::valid(m_inlandWaterSSBO) && RHI::valid(m_inlandWaterUBO);
    (void)up;   // el marco lo fijan tan/bit; `up` queda por si el parche deja de ser tangente
}

// EL SUELO CERCANO QUE VIENE DE LA FÍSICA. Solo se re-suben los buffers cuando llega geometría
// nueva (al saltar el anclaje del clipmap); el UBO del ancla sí va por frame, porque la traslación
// ancla→ojo cambia con la cámara aunque la geometría no.
void TerrestrialPlanet::setNearGroundRing(const std::vector<glm::vec3>* verts,
                                          const std::vector<uint32_t>* tris,
                                          const glm::vec3& anchorRelEye, const glm::vec3& anchorUp,
                                          const glm::dvec3& anchorWorld) {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (verts && tris && verts->size() >= 3 && tris->size() >= 3) {
        if (RHI::valid(m_nearRingVB)) { dev->destroy(m_nearRingVB); m_nearRingVB = {}; }
        if (RHI::valid(m_nearRingIB)) { dev->destroy(m_nearRingIB); m_nearRingIB = {}; }
        m_nearRingVB = dev->createBuffer(RHI::BufferUsage::Vertex,
                                         verts->size() * sizeof(glm::vec3), verts->data());
        m_nearRingIB = dev->createBuffer(RHI::BufferUsage::Index,
                                         tris->size() * sizeof(uint32_t), tris->data());
        m_nearRingIndices = (uint32_t)tris->size();
    }
    if (m_nearRingIndices < 3) return;
    // El ancla del anillo pasa a ser también la del clipmap (ver el bloque de ClipParams en `draw`):
    // es la única forma de que las dos retículas compartan vértices en la frontera del hueco.
    m_nearRingAnchor   = anchorWorld;
    m_nearRingAnchored = true;
    struct { glm::vec4 anchorRelEye; glm::vec4 anchorUp; } u{
        glm::vec4(anchorRelEye, 0.0f), glm::vec4(anchorUp, 0.0f) };
    if (!RHI::valid(m_nearRingUBO))
        m_nearRingUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(u), &u,
                                          RHI::BufferMemory::Dynamic);
    else
        dev->updateBuffer(m_nearRingUBO, 0, sizeof(u), &u);
}

void TerrestrialPlanet::buildMesh(
    const std::function<float(const glm::dvec3&)>& heightFn, double radius)
{
    clearGPU();
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    const int res = m_faceSubdiv;
    const int vertsPerFace = (res + 1) * (res + 1);
    const int totalVerts = vertsPerFace * 6;
    const int totalIndices = 6 * res * res * 6;  // 6 faces × res² quads × 2 tris × 3 indices

    std::vector<SimplePlanetVertex> verts(totalVerts);
    std::vector<uint32_t> indices(totalIndices);
    std::vector<uint32_t> patchIdx((size_t)6 * res * res * 4);
    // 4 esquinas por parche (vec4 cada una) para el culling en GPU. 393 216 parches × 64 B = 25 MB
    // en un planeta de faceRes 256; estático, se sube una vez y no se vuelve a tocar.
    std::vector<glm::vec4> patchBounds((size_t)6 * res * res * 4);

    // LAS 6 CARAS EN PARALELO: cada cara solo lee su retícula local (`faceDirs`/`heights`) y escribe
    // en un rango DISJUNTO de verts/indices/patchIdx; `heightFn`/`biomeColor`/`m_climate` son puras
    // `const` (geología, clima, elevación/zona: solo lectura). Es el reparto de carga ESTABLE: sin
    // mutex, sin estado compartido, sin carreras. Los buffers se pre-dimensionan y cada hilo rellena
    // solo su tramo, así que no hay push_back concurrente ni realloc.
    auto buildFace = [&](int face) {
        const PlanetFace pf = (PlanetFace)face;
        const uint32_t vBase = (uint32_t)(face * vertsPerFace);
        std::vector<glm::dvec3> faceDirs(vertsPerFace);
        for (int j = 0; j <= res; ++j)
            for (int i = 0; i <= res; ++i) {
                const double lx = (double)i / res * 2.0 - 1.0;
                const double ly = (double)j / res * 2.0 - 1.0;
                faceDirs[j * (res + 1) + i] = cubeFaceToDir(pf, lx, ly);
            }

        // Heights and colors
        std::vector<float> heights(vertsPerFace);
        for (int idx = 0; idx < vertsPerFace; ++idx) {
            heights[idx] = heightFn(faceDirs[idx]);
        }

        // NORMAL DEL TERRENO, no de la esfera. Antes era `normalize(dir)` —la normal de una esfera
        // perfecta— y eso tenía dos consecuencias que se veían: (1) el relieve NO EXISTÍA en la
        // iluminación, una cordillera se sombreaba igual que una llanura; (2) `slope` en el shader
        // es `1 - dot(n, up)`, y con n == up daba **0 en todo el planeta**, así que ningún material
        // de pendiente (la roca) podía seleccionarse jamás.
        //
        // Se calcula por DIFERENCIAS CENTRALES sobre la retícula de la cara: el producto vectorial
        // de las dos tangentes construidas con los vecinos. En el borde de la cara se cae a
        // diferencia lateral (clamp del índice).
        auto posAt = [&](int i, int j) {
            i = glm::clamp(i, 0, res); j = glm::clamp(j, 0, res);
            const int k = j * (res + 1) + i;
            return faceDirs[k] * (radius + (double)heights[k]);
        };

        // Vertex data
        for (int idx = 0; idx < vertsPerFace; ++idx) {
            const glm::dvec3 dir = faceDirs[idx];
            const float h = heights[idx];
            const glm::dvec3 pos = dir * (radius + h);
            const glm::vec3 col = biomeColor(dir, h / 1000.0f);
            const int vi = idx % (res + 1);
            const int vj = idx / (res + 1);

            SimplePlanetVertex v{};
            v.px = (float)pos.x; v.py = (float)pos.y; v.pz = (float)pos.z;
            glm::dvec3 n = glm::normalize(dir);
            {
                const glm::dvec3 du = posAt(vi + 1, vj) - posAt(vi - 1, vj);
                const glm::dvec3 dv = posAt(vi, vj + 1) - posAt(vi, vj - 1);
                glm::dvec3 g = glm::cross(du, dv);
                const double len = glm::length(g);
                // Degenerado (esquina del cubo, o dos vecinos coincidentes) → la radial es el
                // respaldo correcto: sin relieve medible, la normal de la esfera es la buena.
                if (len > 1e-12) {
                    g /= len;
                    if (glm::dot(g, dir) < 0.0) g = -g;   // orientada hacia AFUERA
                    n = g;
                }
            }
            v.nx = (float)n.x; v.ny = (float)n.y; v.nz = (float)n.z;
            v.u = (float)idx / vertsPerFace; // placeholder UV
            v.v = 0.0f;
            v.cr = col.r; v.cg = col.g; v.cb = col.b;
            // Climate data for biome shader
            float elevKm = h / 1000.0f;
            bool ocean = elevKm < 0;
            v.elev = elevKm;
            v.temp = (float)m_climate.temperature(dir, elevKm, ocean);
            v.humid = (float)m_climate.humidity(dir, elevKm, ocean);
            verts[vBase + (uint32_t)idx] = v;
        }

        // Indices
        size_t iout = (size_t)face * res * res * 6;
        size_t pout = (size_t)face * res * res * 4;
        for (int j = 0; j < res; ++j)
            for (int i = 0; i < res; ++i) {
                const uint32_t a = vBase + (uint32_t)(j * (res + 1) + i);
                const uint32_t b = vBase + (uint32_t)(j * (res + 1) + i + 1);
                const uint32_t c = vBase + (uint32_t)((j + 1) * (res + 1) + i);
                const uint32_t d = vBase + (uint32_t)((j + 1) * (res + 1) + i + 1);
                indices[iout++] = a; indices[iout++] = b; indices[iout++] = c;
                indices[iout++] = b; indices[iout++] = d; indices[iout++] = c;
                // Orden del PARCHE: recorrido del quad (a,b,d,c), no el de los triángulos. El tess
                // eval interpola con `mix(mix(p0,p1,u), mix(p3,p2,u), v)`, así que los cuatro tienen
                // que ir en anillo; con el orden de triángulos el parche saldría cruzado.
                patchIdx[pout++] = a; patchIdx[pout++] = b;
                patchIdx[pout++] = d; patchIdx[pout++] = c;
                // ENVOLVENTE DEL PARCHE para el culling en GPU (terrain_cull.comp): las 4 esquinas
                // en coords del planeta. Se calcula UNA vez aquí —la malla base es estática— para
                // que el compute no tenga que replicar el layout del vertex buffer en GLSL.
                const size_t pb = (size_t)(pout / 4 - 1) * 4;
                auto vpos = [&](uint32_t vi) {
                    return glm::vec4(verts[vi].px, verts[vi].py, verts[vi].pz, 0.0f);
                };
                patchBounds[pb + 0] = vpos(a);
                patchBounds[pb + 1] = vpos(b);
                patchBounds[pb + 2] = vpos(d);
                patchBounds[pb + 3] = vpos(c);
            }
    };

    const unsigned int hwc = std::thread::hardware_concurrency();
    const int nthreads = (hwc > 0 && hwc < 6) ? (int)hwc : 6;
    std::vector<std::thread> threads;
    for (int t = 0; t < nthreads; ++t)
        threads.emplace_back([&, t]() {
            for (int face = t; face < 6; face += nthreads) buildFace(face);
        });
    for (auto& th : threads) th.join();

    m_vertexBuffer = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(SimplePlanetVertex), verts.data(), RHI::BufferMemory::Static);
    m_indexBuffer  = dev->createBuffer(RHI::BufferUsage::Index, indices.size() * sizeof(uint32_t), indices.data(), RHI::BufferMemory::Static);
    m_indexCount   = (uint32_t)indices.size();
    m_vertexCount  = (uint32_t)verts.size();
    m_patchIB      = dev->createBuffer(RHI::BufferUsage::Index, patchIdx.size() * sizeof(uint32_t), patchIdx.data(), RHI::BufferMemory::Static);
    m_patchIndexCount = (uint32_t)patchIdx.size();
    // ── Culling de parches en GPU (ajuste GpuPatchCull; HARUKA_GPU_CULL=0/1 lo fuerza) ──────────
    // Buffers del compute: envolventes (SSBO estático), índice ORIGEN (el mismo patchIdx, como
    // storage), índice COMPACTADO (destino, tamaño máximo = el original) y el comando indirecto.
    // Se crean aquí porque su tamaño lo fija la malla; el dispatch va en el draw.
    m_patchCount   = (uint32_t)patchIdx.size() / 4u;
    m_patchBoundsSSBO = dev->createBuffer(RHI::BufferUsage::Storage,
                                          patchBounds.size() * sizeof(glm::vec4),
                                          patchBounds.data(), RHI::BufferMemory::Static);
    m_patchIdxSSBO = dev->createBuffer(RHI::BufferUsage::Storage,
                                       patchIdx.size() * sizeof(uint32_t),
                                       patchIdx.data(), RHI::BufferMemory::Static);
    // El compactado es a la vez ÍNDICE (para el draw) y STORAGE (destino del compute).
    m_patchIdxCulled = dev->createBuffer(RHI::BufferUsage::Index,
                                         patchIdx.size() * sizeof(uint32_t),
                                         nullptr, RHI::BufferMemory::Dynamic);
    // DrawElementsIndirectCommand: {indexCount, instanceCount, firstIndex, baseVertex, baseInstance}.
    // El compute solo escribe indexCount; el resto lo fija la CPU una vez.
    const uint32_t cmdInit[5] = { 0u, 1u, 0u, 0u, 0u };
    m_patchCullCmd = dev->createBuffer(RHI::BufferUsage::Indirect, sizeof(cmdInit), cmdInit,
                                       RHI::BufferMemory::Dynamic);

    // REJILLA DEL CLIPMAP: N x N parches de 128 m con la cámara en el centro; la rejilla no se
    // regenera nunca, solo se reorienta con el marco tangente del UBO. El TAMAÑO depende de la
    // calidad de terreno (TerrainQuality): el fino cubre más en calidad alta para empujar la malla
    // gruesa a mayor distancia. El semi-lado resultante (m_clipCoverM) se pasa a los shaders por
    // ClipParams (binding 13): el recorte de la malla base (terrain.tese) y el anillo de mezcla del
    // clipmap (clipmap.tese) leen ESE valor, nunca literales.
    {
        const float PATCH = (float)Haruka::Planet::TERRAIN_CLIP_PATCH_M;
        const int   quality = (int)SettingsManager::get().graphics().terrainQuality;
        const int   NCs[4]  = { 31, 47, 63, 95 };   // Low, Medium, High, Ultra (31 = ±1984 m, como antes)
        const int   NC = NCs[std::clamp(quality, 0, 3)];
        m_clipCoverM = NC * PATCH * 0.5f;
        std::vector<glm::vec2> cv;
        std::vector<uint32_t>  ci;
        cv.reserve((NC + 1) * (NC + 1));
        for (int j = 0; j <= NC; ++j)
            for (int i = 0; i <= NC; ++i)
                cv.push_back(glm::vec2((i - NC * 0.5f) * PATCH, (j - NC * 0.5f) * PATCH));
        for (int j = 0; j < NC; ++j)
            for (int i = 0; i < NC; ++i) {
                const uint32_t a = (uint32_t)(j * (NC + 1) + i);
                ci.push_back(a); ci.push_back(a + 1);
                ci.push_back(a + NC + 2); ci.push_back(a + NC + 1);
            }
        m_clipVB = dev->createBuffer(RHI::BufferUsage::Vertex, cv.size() * sizeof(glm::vec2), cv.data(), RHI::BufferMemory::Static);
        m_clipIB = dev->createBuffer(RHI::BufferUsage::Index,  ci.size() * sizeof(uint32_t),  ci.data(), RHI::BufferMemory::Static);
        m_clipIndexCount = (uint32_t)ci.size();
        m_clipVertexCount = (uint32_t)cv.size();
    }

    // ── MALLA DEL MAR LEJANO: esfera a nivel del mar ────────────────────────────────────────────
    //
    // Solo posición y normal (6 floats): esta superficie no lleva olas —las pone el mar cercano— así
    // que no necesita ni UV ni color ni clima. El campo ya viene desplazado para que el nivel del mar
    // sea la cota 0, así que "aquí hay mar" y "aquí se dibuja mar" son la misma cota por construcción.
    {
        std::vector<float>    ov;   ov.reserve((size_t)6 * vertsPerFace * 6);
        std::vector<uint32_t> oi;   oi.reserve((size_t)6 * res * res * 6);
        for (int face = 0; face < 6; ++face) {
            const PlanetFace pf = (PlanetFace)face;
            const uint32_t base = (uint32_t)(face * vertsPerFace);
            for (int j = 0; j <= res; ++j)
                for (int i = 0; i <= res; ++i) {
                    const glm::dvec3 dir = cubeFaceToDir(pf, (double)i / res * 2.0 - 1.0,
                                                             (double)j / res * 2.0 - 1.0);
                    const glm::dvec3 pos = dir * radius;
                    ov.push_back((float)pos.x); ov.push_back((float)pos.y); ov.push_back((float)pos.z);
                    ov.push_back((float)dir.x); ov.push_back((float)dir.y); ov.push_back((float)dir.z);
                }
            for (int j = 0; j < res; ++j)
                for (int i = 0; i < res; ++i) {
                    const uint32_t a = base + (uint32_t)(j * (res + 1) + i);
                    const uint32_t b = a + 1;
                    const uint32_t c = base + (uint32_t)((j + 1) * (res + 1) + i);
                    const uint32_t d = c + 1;
                    oi.push_back(a); oi.push_back(b); oi.push_back(c);
                    oi.push_back(b); oi.push_back(d); oi.push_back(c);
                }
        }
        m_oceanFarVB = dev->createBuffer(RHI::BufferUsage::Vertex, ov.size() * sizeof(float),
                                         ov.data(), RHI::BufferMemory::Static);
        m_oceanFarIB = dev->createBuffer(RHI::BufferUsage::Index, oi.size() * sizeof(uint32_t),
                                         oi.data(), RHI::BufferMemory::Static);
        m_oceanFarFaceStride = (uint32_t)(res * res * 6);
    }

    // ── SONDA: ¿QUÉ RANGO DE CLIMA PRODUCE DE VERDAD ESTE PLANETA? ─────────────────────────────
    //
    // ⚠️ No es decorativa. `PropScatter` descarta capas enteras por humedad y su aviso dice "la capa
    // pide [0.35, 0.95] · el terreno da [0.12, 0.13]" — pero ese rango lo mide sobre el PARCHE que
    // rodea al jugador, así que no distingue "el planeta es seco" de "estás en un sitio seco". Sin
    // esa distinción, ajustar las bandas de los props es adivinar: se puede estar tapando un campo
    // de clima roto con una banda más ancha.
    //
    // Esto recorre la retícula base ENTERA (la misma que alimenta al shader y a los props) y saca el
    // rango real. Sale una vez por planeta, en la construcción, y es una pasada sobre datos que ya
    // están en memoria.
    {
        float hMin = 1e9f, hMax = -1e9f, hSum = 0.0f;
        float tMin = 1e9f, tMax = -1e9f;
        int   nLand = 0;
        for (const auto& v : verts) {
            if (v.elev < 0.0f) continue;                 // el clima de props solo importa en tierra
            hMin = std::min(hMin, v.humid); hMax = std::max(hMax, v.humid); hSum += v.humid;
            tMin = std::min(tMin, v.temp);  tMax = std::max(tMax, v.temp);
            ++nLand;
        }
        if (nLand > 0)
            HARUKA_LOGI("Terrain", "clima del planeta (%d vértices de TIERRA): humedad [%.3f, %.3f] "
                        "media %.3f · temperatura [%.1f, %.1f] °C",
                        nLand, hMin, hMax, hSum / (float)nLand, tMin, tMax);
    }

    // Copia CPU de la retícula de alturas: es el suelo que consultará la física.
    m_baseRes    = res;
    m_baseRadius = (float)radius;
    m_baseHeights.resize((size_t)6 * vertsPerFace);
    for (size_t v = 0; v < verts.size(); ++v) m_baseHeights[v] = verts[v].elev * 1000.0f;

    // Y la misma retícula a la GPU. Las 6 caras van como capas de un array: el shader elige capa
    // con la misma descomposición `dirToCubeFace` que usa la CPU, así que los dos leen el mismo
    // téxel — que es la condición para que el clipmap y la malla del planeta describan un solo suelo.
    {
        // RGBA32F (NO RGB32F): Vulkan/NVIDIA no soporta muestrear R32G32B32 (RGB está prohibido para
        // imagenes). El shader lee .rgb, así que el 4º componente se rellena con 0 sin efecto.
        std::vector<float> field((size_t)6 * vertsPerFace * 4);
        for (size_t v = 0; v < verts.size(); ++v) {
            field[v * 4 + 0] = verts[v].elev * 1000.0f;
            field[v * 4 + 1] = verts[v].temp;
            field[v * 4 + 2] = verts[v].humid;
            field[v * 4 + 3] = 0.0f;
        }
        RHI::TextureDesc fd;
        fd.width = fd.height = (uint32_t)(res + 1);
        fd.layers = 6;
        fd.format = RHI::Format::RGBA32F;
        fd.filter = RHI::Filter::Nearest;   // se interpola A MANO, ver el shader del clipmap
        fd.wrap   = RHI::Wrap::ClampToEdge;
        fd.initialData = field.data();
        m_baseFieldTex = dev->createTexture(fd);
    }

}

void TerrestrialPlanet::clearGPU() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(m_vertexBuffer)) { dev->destroy(m_vertexBuffer); m_vertexBuffer = {}; }
    if (RHI::valid(m_indexBuffer))  { dev->destroy(m_indexBuffer);  m_indexBuffer  = {}; }
    if (RHI::valid(m_patchIB))      { dev->destroy(m_patchIB);      m_patchIB      = {}; }
    // Buffers del culling en GPU: por planeta, porque su tamaño lo fija su faceRes.
    if (RHI::valid(m_patchBoundsSSBO)) { dev->destroy(m_patchBoundsSSBO); m_patchBoundsSSBO = {}; }
    if (RHI::valid(m_patchIdxSSBO))    { dev->destroy(m_patchIdxSSBO);    m_patchIdxSSBO    = {}; }
    if (RHI::valid(m_patchIdxCulled))  { dev->destroy(m_patchIdxCulled);  m_patchIdxCulled  = {}; }
    if (RHI::valid(m_patchCullCmd))    { dev->destroy(m_patchCullCmd);    m_patchCullCmd    = {}; }
    if (RHI::valid(m_patchCullUBO))    { dev->destroy(m_patchCullUBO);    m_patchCullUBO    = {}; }
    m_patchCount = 0;
    if (RHI::valid(m_baseFieldTex)) { dev->destroy(m_baseFieldTex); m_baseFieldTex = {}; }
    if (RHI::valid(m_clipVB))       { dev->destroy(m_clipVB);       m_clipVB       = {}; }
    if (RHI::valid(m_oceanFarVB))   { dev->destroy(m_oceanFarVB);   m_oceanFarVB   = {}; }
    if (RHI::valid(m_oceanFarIB))   { dev->destroy(m_oceanFarIB);   m_oceanFarIB   = {}; }
    if (RHI::valid(m_clipIB))       { dev->destroy(m_clipIB);       m_clipIB       = {}; }
    if (RHI::valid(m_wetUBO)) { dev->destroy(m_wetUBO); m_wetUBO = {}; }
    for (auto& b : m_clipUBOs) if (RHI::valid(b)) { dev->destroy(b); b = {}; }
    m_clipUBOs.clear();
    if (RHI::valid(m_albedoTex))    { dev->destroy(m_albedoTex);    m_albedoTex    = {}; }
    if (RHI::valid(m_normalTex))    { dev->destroy(m_normalTex);    m_normalTex    = {}; }
    auto destroyTex = [&](RHI::TextureHandle& t) { if (RHI::valid(t)) { dev->destroy(t); t = {}; } };
    destroyTex(m_macroTex);
    destroyTex(m_biomeMapTex);
    destroyTex(m_heightTex);
    destroyTex(m_propWhiteTex);
    for (auto& kv : m_propDensityTex) destroyTex(kv.second);
    m_propDensityTex.clear();
    if (RHI::valid(m_propUBO)) { dev->destroy(m_propUBO); m_propUBO = {}; }
    m_heightCPU.clear(); m_heightW = 0; m_heightH = 0;
    m_indexCount = 0;
}

glm::vec3 TerrestrialPlanet::biomeColor(const glm::dvec3& dir, float elevKm) const {
    bool ocean = elevKm < 0;
    double temp = m_climate.temperature(dir, elevKm, ocean);
    double humid = m_climate.humidity(dir, elevKm, ocean);
    double precip = m_climate.precipitation(dir, elevKm, ocean);
    auto biome = m_biomes.classify(elevKm, temp, precip, ocean, std::asin(dir.y));
    return m_biomes.color(biome);
}

Haruka::RHI::TextureHandle TerrestrialPlanet::generateProceduralAlbedo(int width, int height) const {
    RHI::Device* dev = RHI::device();
    if (!dev) return {};
    std::vector<unsigned char> pixels(width * height * 4);
    int noiseSeed = (int)std::hash<std::string>{}(m_config.name);

    auto elevFn = [&](const glm::dvec3& dir) -> float {
        return (float)m_geology.elevationModifier(dir);
    };
    // Cache biome samples for boundary blending
    auto sampleBiomeAt = [&](double lat, double lon) -> std::pair<Haruka::Planet::Biome, glm::vec3> {
        double cy = std::sin(lat);
        double cx = std::cos(lon) * std::cos(lat);
        double cz = std::sin(lon) * std::cos(lat);
        glm::dvec3 dir(cx, cy, cz);
        float elevKm = elevFn(dir);
        bool ocean = elevKm < 0;
        double temp = m_climate.temperature(dir, elevKm, ocean);
        double humid = m_climate.humidity(dir, elevKm, ocean);
        double precip = m_climate.precipitation(dir, elevKm, ocean);
        auto biome = m_biomes.classify(elevKm, temp, precip, ocean, lat);
        return {biome, m_biomes.color(biome)};
    };

    for (int y = 0; y < height; ++y) {
        double lat = (double)y / (height - 1) * glm::pi<double>() - glm::pi<double>() * 0.5;
        double dLat = glm::pi<double>() / (height - 1) * 0.5;
        for (int x = 0; x < width; ++x) {
            double lon = (double)x / (width - 1) * glm::two_pi<double>();
            double dLon = glm::two_pi<double>() / (width - 1) * 0.5;

            auto [biome, col] = sampleBiomeAt(lat, lon);

            // Intra-biome detail noise (fBm)
            double cy = std::sin(lat);
            double cx = std::cos(lon) * std::cos(lat);
            double cz = std::sin(lon) * std::cos(lat);
            glm::dvec3 dir(cx, cy, cz);
            float elevKm = elevFn(dir);

            // fBm detail — position on unit sphere × frequency
            float detail = NoiseGenerator::fBm(dir, noiseSeed, 5, 0.45f, 2.3f, 3.0);
            float noiseStrength = 0.12f;
            if (elevKm < 0) noiseStrength = 0.04f;
            else if (biome == Haruka::Planet::Biome::Desert ||
                     biome == Haruka::Planet::Biome::Tundra ||
                     biome == Haruka::Planet::Biome::Snow)
                noiseStrength = 0.20f;
            else if (biome == Haruka::Planet::Biome::Grassland ||
                     biome == Haruka::Planet::Biome::Savanna)
                noiseStrength = 0.18f;
            col += glm::vec3(detail * noiseStrength);

            // Elevation gradient within biome
            if (elevKm > 0) {
                float ef = glm::clamp(elevKm / 6.0f, 0.0f, 1.0f);
                col = glm::mix(col, glm::vec3(0.55f, 0.50f, 0.40f), ef * 0.25f);
            } else if (elevKm > -0.2f) {
                // Shallow water — sandy tint
                float ef = glm::clamp((elevKm + 0.2f) / 0.2f, 0.0f, 1.0f);
                col = glm::mix(glm::vec3(0.8f, 0.7f, 0.4f), col, ef);
            }

            // Boundary blend: sample two offset positions and blend if biome differs
            for (int offset = 0; offset < 2; ++offset) {
                double olon = lon + (offset == 0 ? dLon : 0.0);
                double olat = lat + (offset == 1 ? dLat : 0.0);
                auto [ob, ocol] = sampleBiomeAt(olat, olon);
                if (ob != biome) {
                    col = glm::mix(col, ocol, 0.25f);
                }
            }

            size_t idx = (size_t)(y * width + x) * 4;
            pixels[idx + 0] = (unsigned char)(glm::clamp(col.r, 0.0f, 1.0f) * 255.0f);
            pixels[idx + 1] = (unsigned char)(glm::clamp(col.g, 0.0f, 1.0f) * 255.0f);
            pixels[idx + 2] = (unsigned char)(glm::clamp(col.b, 0.0f, 1.0f) * 255.0f);
            pixels[idx + 3] = 255;
        }
    }
    RHI::TextureDesc td;
    td.width = (uint32_t)width; td.height = (uint32_t)height;
    td.format = RHI::Format::RGBA8;
    td.mipmaps = true;
    td.initialData = pixels.data();
    return dev->createTexture(td);
}

Haruka::RHI::TextureHandle TerrestrialPlanet::generateProceduralNormal(int width, int height) const {
    RHI::Device* dev = RHI::device();
    if (!dev) return {};
    std::vector<unsigned char> pixels(width * height * 4);
    double R = m_config.radius;
    double dLat = glm::pi<double>() / (height - 1);
    double dLon = glm::two_pi<double>() / (width - 1);

    auto elevFn = [&](const glm::dvec3& dir) -> double {
        return (double)m_geology.elevationModifier(dir) * 1000.0; // km → m
    };

    for (int y = 0; y < height; ++y) {
        double lat = (double)y / (height - 1) * glm::pi<double>() - glm::pi<double>() * 0.5;
        double sLat = std::sin(lat);
        double cLat = std::cos(lat);
        for (int x = 0; x < width; ++x) {
            double lon = (double)x / (width - 1) * glm::two_pi<double>();
            double cLon = std::cos(lon);
            double sLon = std::sin(lon);

            glm::dvec3 dir(cLon * cLat, sLat, sLon * cLat);
            double h = elevFn(dir);

            // Neighbors for finite differences
            double lonE = lon + dLon;
            double latN = std::min(lat + dLat, glm::pi<double>() * 0.5);
            glm::dvec3 dirE(std::cos(lonE) * cLat, sLat, std::sin(lonE) * cLat);
            double cLatN = std::cos(latN);
            glm::dvec3 dirN(std::cos(lon) * cLatN, std::sin(latN), std::sin(lon) * cLatN);
            double hE = elevFn(dirE);
            double hN = elevFn(dirN);

            // Positions on sphere
            glm::dvec3 p0 = dir * (R + h);
            glm::dvec3 pE = dirE * (R + hE);
            glm::dvec3 pN = dirN * (R + hN);

            // Object-space normal via cross product of tangent vectors
            glm::vec3 normal = glm::normalize(glm::cross(glm::vec3(pE - p0), glm::vec3(pN - p0)));

            size_t idx = (size_t)(y * width + x) * 4;
            pixels[idx + 0] = (unsigned char)((normal.x * 0.5f + 0.5f) * 255.0f);
            pixels[idx + 1] = (unsigned char)((normal.y * 0.5f + 0.5f) * 255.0f);
            pixels[idx + 2] = (unsigned char)((normal.z * 0.5f + 0.5f) * 255.0f);
            pixels[idx + 3] = 255;
        }
    }
    RHI::TextureDesc td;
    td.width = (uint32_t)width; td.height = (uint32_t)height;
    td.format = RHI::Format::RGBA8;
    td.mipmaps = true;
    td.initialData = pixels.data();
    return dev->createTexture(td);
}

Haruka::RHI::TextureHandle TerrestrialPlanet::loadTexture(const std::string& path) {
    RHI::Device* dev = RHI::device();
    if (!dev || path.empty()) return {};

    // El MAESTRO es siempre la imagen de mayor área entre TODAS las carpetas (`src`/`16k`/`8k`/
    // `4k`/`hd`) del motor y del proyecto; la calidad de terreno decide después cuánto se reduce.
    static const char* kResLabels[] = { "src", "16k", "8k", "4k", "hd" };

    int w = 0, h = 0;
    unsigned char* data = nullptr;
    int bestArea = 0;
    std::string bestPath;
    auto takeBest = [&](const std::string& root) {
        for (int i = 0; i < 5; ++i) {
            std::string full = root + "terrain/" + kResLabels[i] + "/" + path;
            int cw = 0, ch = 0, cn = 0;
            unsigned char* d = stbi_load(full.c_str(), &cw, &ch, &cn, 4);
            if (!d) continue;
            const int area = cw * ch;
            if (area > bestArea) {
                if (data) stbi_image_free(data);
                data = d; w = cw; h = ch; bestArea = area; bestPath = full;
            } else {
                stbi_image_free(d);
            }
        }
    };
    takeBest(Haruka::AssetPaths::textures());
    // Raíz del PROYECTO: bajo el IDE el motor resuelve sus assets contra la carpeta del
    // editor, mientras que las biomas por defecto viven en las del proyecto abierto. Sin
    // esta segunda pasada, cada planeta bajo el editor caía al fallback procedural y
    // logueaba 8 WARN por nombre.
    takeBest(Haruka::AssetPaths::projectTextures());
    if (!data) {
        HARUKA_LOGW("SimplePlanet", "failed to load texture: %s", path.c_str());
        return {};
    }
    // ⚠️ QUÉ FICHERO HA GANADO, y por qué esto merece una línea de log.
    //
    // La regla de selección no es la calidad: es el MAYOR ÁREA entre 2 raíces × 5 subcarpetas
    // (motor y proyecto, cada uno con src/16k/8k/4k/hd). La calidad solo reduce DESPUÉS, en memoria.
    // O sea que un fichero suelto en `8k/` de cualquiera de los dos repos se convierte en el maestro
    // en silencio, y con cuatro copias del mismo nombre por ahí es imposible saber cuál se está
    // viendo sin ir a mirar tamaños a mano. Con esto es una línea.
    HARUKA_LOGI("SimplePlanet", "textura '%s': gana %s (%dx%d)", path.c_str(), bestPath.c_str(), w, h);

    RHI::TextureDesc td;
    td.format = RHI::Format::RGBA8;
    td.mipmaps = true;

    std::vector<unsigned char> resized;
    unsigned char* upload = data;
    const int f = qualityDownscaleFactor(std::max(w, h), terrainQualityTarget());
    if (f > 1) {
        boxDownsample(data, w, h, f, resized);
        stbi_image_free(data);
        w = w / f; h = h / f;
        upload = resized.data();
    }
    td.width = (uint32_t)w; td.height = (uint32_t)h;
    td.initialData = upload;
    RHI::TextureHandle handle = dev->createTexture(td);
    if (f <= 1) stbi_image_free(data);
    return handle;
}

// Apila N PNG de terreno en UNA textura array. Todas tienen que medir lo mismo: una capa de otra
// resolución no cabe en el storage inmutable del array, así que se RELLENA con gris neutro y se
// avisa — dejarla fuera correría los índices y cada material apuntaría a la textura del siguiente.
Haruka::RHI::TextureHandle TerrestrialPlanet::loadTextureArray(
    const std::vector<std::string>& paths, int& outLayers)
{
    outLayers = 0;
    RHI::Device* dev = RHI::device();
    if (!dev || paths.empty()) return {};

    // Igual que `loadTexture`: cada capa carga su MAESTRO (mayor área) y la calidad decide la
    // reducción. El array no mezcla tamaños: si una capa no llega a la resolución común, las que
    // sobran se REBAJAN por cajas hasta el tamaño de la menor (en vez de rellenar con gris).
    static const char* kResLabels[] = { "src", "16k", "8k", "4k", "hd" };
    // Ver la nota de `loadTexture`: el ganador es el de MAYOR ÁREA entre 2 raíces × 5 subcarpetas, no
    // el que dicte la calidad. Se registra cuál gana por capa para que sea auditable de un vistazo.
    auto takeBest = [&](const std::string& root, const std::string& path,
                        int& w, int& h, unsigned char*& data, int& bestArea, std::string& bestPath) {
        for (int i = 0; i < 5; ++i) {
            std::string full = root + "terrain/" + kResLabels[i] + "/" + path;
            int cw = 0, ch = 0, cn = 0;
            unsigned char* d = stbi_load(full.c_str(), &cw, &ch, &cn, 4);
            if (!d) continue;
            const int area = cw * ch;
            if (area > bestArea) {
                if (data) stbi_image_free(data);
                data = d; w = cw; h = ch; bestArea = area; bestPath = full;
            } else {
                stbi_image_free(d);
            }
        }
    };

    const int target = terrainQualityTarget();
    std::vector<std::vector<unsigned char>> layerData;
    std::vector<int> layerW, layerH;
    layerData.reserve(paths.size());
    layerW.reserve(paths.size());
    layerH.reserve(paths.size());
    int minLong = 0;

    for (const std::string& path : paths) {
        if (path.empty()) { layerData.emplace_back(); layerW.push_back(0); layerH.push_back(0); continue; }
        int w = 0, h = 0;
        unsigned char* data = nullptr;
        int bestArea = 0;   // persiste entre las dos raíces, se resetea por capa
        std::string bestPath;
        takeBest(Haruka::AssetPaths::textures(), path, w, h, data, bestArea, bestPath);
        takeBest(Haruka::AssetPaths::projectTextures(), path, w, h, data, bestArea, bestPath);
        if (data) HARUKA_LOGI("Terrain", "capa '%s': gana %s (%dx%d)",
                              path.c_str(), bestPath.c_str(), w, h);
        if (!data) {
            HARUKA_LOGW("Terrain", "capa '%s' no encontrada -> gris neutro", path.c_str());
            layerData.emplace_back(); layerW.push_back(0); layerH.push_back(0);
            continue;
        }
        std::vector<unsigned char> layer;
        const int f = qualityDownscaleFactor(std::max(w, h), target);
        if (f > 1) {
            boxDownsample(data, w, h, f, layer);
            w = w / f; h = h / f;
        } else {
            layer.assign(data, data + (size_t)w * h * 4);
        }
        stbi_image_free(data);
        layerData.push_back(std::move(layer));
        layerW.push_back(w); layerH.push_back(h);
        const int l = std::max(w, h);
        minLong = (minLong == 0) ? l : std::min(minLong, l);
    }
    if (minLong <= 0) return {};

    int W = 0, H = 0;
    for (size_t i = 0; i < layerData.size(); ++i) {
        if (layerW[i] <= 0) continue;
        const int f2 = qualityDownscaleFactor(std::max(layerW[i], layerH[i]), minLong);
        if (f2 > 1) {
            std::vector<unsigned char> tmp;
            boxDownsample(layerData[i].data(), layerW[i], layerH[i], f2, tmp);
            layerW[i] = layerW[i] / f2; layerH[i] = layerH[i] / f2;
            layerData[i].swap(tmp);
        }
        W = layerW[i]; H = layerH[i];
    }
    if (W <= 0 || H <= 0) return {};

    const size_t bytesPerLayer = (size_t)W * H * 4;
    std::vector<unsigned char> all;
    all.reserve(bytesPerLayer * layerData.size());
    for (auto& l : layerData) {
        if (l.size() != bytesPerLayer) l.assign(bytesPerLayer, 128);
        all.insert(all.end(), l.begin(), l.end());
    }

    RHI::TextureDesc td;
    td.width = (uint32_t)W; td.height = (uint32_t)H;
    td.layers = (uint32_t)layerData.size();
    td.format = RHI::Format::RGBA8;
    td.mipmaps = true;
    td.maxAnisotropy = 8.0f;
    td.initialData = all.data();
    outLayers = (int)layerData.size();
    // ⚠️ EL LOG IBA ANTES DE CREAR LA TEXTURA, así que decía "array de terreno: 9 capas" aunque la
    // creación fallara — y el sitio donde se enlaza tiene un `if (RHI::valid(...))` que se salta el
    // bind en silencio. Resultado: el shader muestrea un sampler2DArray SIN ENLAZAR, que es
    // comportamiento indefinido (en muchos drivers, valores que varían por téxel). Un array de 9 capas
    // de 2048² RGBA con mipmaps son ~200 MB de VRAM: es un fallo perfectamente posible, y era invisible.
    RHI::TextureHandle tex = dev->createTexture(td);
    if (!RHI::valid(tex)) {
        HARUKA_LOGE("Terrain", "NO se pudo crear el array de terreno (%d capas de %dx%d, ~%.0f MB): "
                    "el terreno se dibujara SIN textura de material",
                    outLayers, W, H, (double)outLayers * W * H * 4 * 1.34 / 1e6);
        outLayers = 0;
        return {};
    }
    HARUKA_LOGI("Terrain", "array de terreno: %d capas de %dx%d (%.0f MB)", outLayers, W, H,
                (double)outLayers * W * H * 4 * 1.34 / 1e6);
    return tex;
}

void TerrestrialPlanet::rebuildTerrainArrays() {
    m_materialTable.assignLayers();   // la capa es la POSICIÓN, no un número declarado a mano

    RHI::Device* dev = RHI::device();
    if (dev) {
        if (RHI::valid(m_terrainAlbedoArray)) { dev->destroy(m_terrainAlbedoArray); m_terrainAlbedoArray = {}; }
        if (RHI::valid(m_terrainNormalArray)) { dev->destroy(m_terrainNormalArray); m_terrainNormalArray = {}; }
    }
    m_terrainArrayLayers = 0;

    const auto albedos = m_materialTable.albedoPaths();
    if (albedos.empty()) return;    // tabla sin texturas: terreno de color de bioma, sin grano

    int layers = 0;
    m_terrainAlbedoArray = loadTextureArray(albedos, layers);
    m_terrainArrayLayers = layers;
    int nl = 0;
    m_terrainNormalArray = loadTextureArray(m_materialTable.normalPaths(), nl);
}

// Consulta el mapa de zonas en una dirección. Devuelve false si no hay mapa.
// Muestreo por VECINO MÁS CERCANO, no bilineal: el mapa es una PALETA, y promediar dos colores de
// paleta da un color que no es ninguno de los dos — una costa interpolada entre azul y verde daría
// un turquesa que no casa con ningún material.
static bool sampleZone(const std::vector<unsigned char>& img, int W, int H,
                       const glm::dvec3& dir, glm::vec3& outColor) {
    if (img.empty() || W <= 0 || H <= 0) return false;
    const glm::dvec3 d = glm::normalize(dir);
    const double u = 0.5 + std::atan2(d.z, d.x) * 0.1591549430918953;
    const double v = 0.5 - std::asin(glm::clamp(d.y, -1.0, 1.0)) * 0.3183098861837907;
    // BILINEAL, igual que la GPU: la línea de costa es un degradado de un texel y `zoneToMaterial`
    // (vecino más cercano en RGB) la cae a la zona correcta. Con vecino directo la línea de agua
    // seguía la rejilla del texel y la costa salía cuadriculada (~10 km por texel).
    const double fx = u * W - 0.5, fy = v * H - 0.5;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const double tx = fx - x0, ty = fy - y0;
    x0 = ((x0 % W) + W) % W;                 // longitud: envuelve
    const int x1 = (x0 + 1) % W;
    y0 = glm::clamp(y0, 0, H - 1);           // latitud: no
    const int y1 = glm::clamp(y0 + 1, 0, H - 1);
    auto at = [&](int x, int y) -> glm::vec3 {
        const size_t k = ((size_t)y * W + x) * 4;
        return glm::vec3(img[k], img[k + 1], img[k + 2]);
    };
    const glm::vec3 c00 = at(x0, y0), c10 = at(x1, y0);
    const glm::vec3 c01 = at(x0, y1), c11 = at(x1, y1);
    outColor = (c00 * (1.0f - (float)tx) + c10 * (float)tx) * (1.0f - (float)ty)
             + (c01 * (1.0f - (float)tx) + c11 * (float)tx) * (float)ty;
    return true;
}

// Índice del material cuyo `zoneColor` está más cerca del color leído. -1 si el píxel no está
// pintado o no corresponde a ninguna zona declarada.
//
// ⚠️ GEMELO del bloque de zonas de `harukaSelectMaterial` (lib/terrain_material.glsl): los dos
// umbrales tienen que ser los MISMOS o la altura (que decide esta función, vía `baseHeight`) y el
// material (que decide el shader) discreparían sobre dónde empieza una zona.
//
// El umbral es lo que hace el mapa PINTABLE. Sin él —como estaba— cualquier píxel caía al material
// más cercano, así que declarar una sola zona secuestraba el planeta entero y las reglas de clima,
// pendiente y altura quedaban muertas. Con umbral, el mapa es una capa aditiva: se pinta lo que se
// fija a mano y el resto lo deciden las reglas.
static int zoneToMaterial(const Haruka::Planet::TerrainMaterialTable& table, const glm::vec3& c) {
    constexpr float kZoneTol2   = 28.0f * 28.0f * 3.0f;   // distancia² máx. para considerarlo pintado
    constexpr float kUnpainted2 = 12.0f * 12.0f * 3.0f;   // por debajo: "negro" = sin pintar
    if (glm::dot(c, c) <= kUnpainted2) return -1;         // fondo sin pintar → mandan las reglas
    int best = -1; float bestD = 0.0f;
    for (size_t i = 0; i < table.materials.size(); ++i) {
        const auto& m = table.materials[i];
        if (!m.hasZone()) continue;
        const glm::vec3 d = m.zoneColor - c;
        const float dist = glm::dot(d, d);
        if (best < 0 || dist < bestD) { best = (int)i; bestD = dist; }
    }
    if (best >= 0 && bestD > kZoneTol2) return -1;        // pintado, pero de ninguna zona conocida
    return best;
}

float TerrestrialPlanet::rawHeight(const glm::dvec3& dir) const {
    return (float)m_geology.elevationModifier(dir) * 1000.0f; // km → m
}

float TerrestrialPlanet::sampleElevMap(const glm::dvec3& d) const {
    if (m_elevCPU.empty() || m_elevW <= 0 || m_elevH <= 0) return 0.0f;
    const glm::vec2 elevRange = m_config.surface.elevationRange;
    const double u = 0.5 + std::atan2(d.z, d.x) * 0.1591549430918953;
    const double v = 0.5 - std::asin(glm::clamp(d.y, -1.0, 1.0)) * 0.3183098861837907;
    const double fx = u * m_elevW - 0.5, fy = v * m_elevH - 0.5;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = (float)(fx - x0), ty = (float)(fy - y0);
    auto at = [this](int x, int y) -> float {
        x = ((x % m_elevW) + m_elevW) % m_elevW;              // longitud envuelve
        y = glm::clamp(y, 0, m_elevH - 1);                    // latitud no
        return m_elevCPU[((size_t)y * m_elevW + x) * 4] * (1.0f / 65535.0f);
    };
    const float a = at(x0, y0)     + (at(x0 + 1, y0)     - at(x0, y0))     * tx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    const float t = a + (b - a) * ty;
    return elevRange.x + (elevRange.y - elevRange.x) * t;
}

float TerrestrialPlanet::baseHeight(const glm::dvec3& dir) const {
    const bool hasElevMap = !m_elevCPU.empty();
    const bool hasZoneMap = !m_zoneCPU.empty();
    // Recorte SUAVE de ±8 m: solo garantiza el SIGNO (el mapa manda sobre mar/tierra) y no roza
    // la amplitud del detalle (±130 m). Con ±40 m la tierra de costa quedaba aplanada en un
    // estante y la línea de agua no podía seguirlo — la costa salía lisa. Con el mapa de
    // elevación actual (plataforma costera de ±60..100 m) el agua corta el relieve y la costa
    // sale dentada.
    constexpr float kZoneClearanceM = 8.0f;
    // Con mapa de elevación manda el AUTOR; sin él, geología menos el nivel del mar.
    float h = hasElevMap ? sampleElevMap(dir) : (rawHeight(dir) - m_seaLevelOffsetM);
    if (!hasZoneMap) return h;
    glm::vec3 zc;
    if (!sampleZone(m_zoneCPU, m_zoneW, m_zoneH, dir, zc)) return h;
    const int mi = zoneToMaterial(m_materialTable, zc);
    if (mi < 0) return h;
    // El mapa MANDA sobre el signo, el campo procedural sigue poniendo el relieve dentro de la
    // zona. Es un recorte, no una sustitución.
    return m_materialTable.materials[mi].submerged
         ? std::min(h, -kZoneClearanceM)
         : std::max(h,  kZoneClearanceM);
}

double TerrestrialPlanet::sampleHeight(const glm::dvec3& dirIn) const {
    // Campo cercano: el piso de `terrainTriM`, que es el lado real del quad del clipmap. Aquí había
    // un `2.0f` literal y es el camino por el que se anclan LOS PROPS — con el render evaluando a
    // 4 m y el ancla a 2 m, cada prop quedaba ~9 cm en el aire.
    return sampleHeight(dirIn, Haruka::Planet::terrainTriM(0.0));
}

double TerrestrialPlanet::sampleHeight(const glm::dvec3& dirIn, float minFeatureM) const {
    if (m_heightCPU.empty() || m_heightW <= 0 || m_heightH <= 0) {
        // ⚠️ Devolver 0 aquí es decir "el suelo está al nivel del mar", y el llamante no puede
        // distinguirlo de un suelo que de verdad está a esa cota. Mientras el bake no esté, la
        // física camina sobre una esfera lisa bajo un terreno con relieve: cientos de metros de
        // desajuste, no centímetros. Se avisa UNA vez para que deje de ser invisible.
        static bool warned = false;
        if (!warned) {
            warned = true;
            HARUKA_LOGW("Terrain", "sampleHeight('%s') sin bake de altura: la fisica cae al NIVEL DEL "
                        "MAR (esfera lisa) hasta que el bake termine", m_config.name.c_str());
        }
        return 0.0;
    }
    const glm::dvec3 dir = glm::normalize(dirIn);

    // El MISMO campo base que pintan la malla y el clipmap: la copia CPU del R32F horneado, muestreada
    // con la misma bilineal (mismo orden de operaciones, terrain_detail.h) que los shaders. Así la
    // física, la malla y el clipmap nacen del mismo dato y no pueden divergir (paridad por
    // construcción, fase 2b).
    const glm::vec2 uv    = Haruka::Planet::equirectUV(glm::vec3(dir));
    const float     baseH = Haruka::Planet::sampleHeightField(uv, m_heightW, m_heightH,
                                                              m_heightCPU.data());

    // Y el detalle, con la MISMA función que la GPU y el `minFeatureM` que el render usa EN ESTE
    // punto (§9 Fase 3): el clipmap evalúa la rejilla con `max(rad·0.002, 2.0)` y el per-pixel con
    // `max(dist·0.002, 0.5)`; la física de colisión debe usar exactamente el mismo, o el suelo que
    // se pisa y el que se dibuja se separan decímetros en el anillo fino (paridad medida, triM 0.5
    // es la octava de 4,5 m entera: 0.7 m de amplitud potencial).
    const float baseR = m_baseRadius + baseH;
    float det = Haruka::Planet::terrainDetail(glm::vec3(dir), baseR, minFeatureM)
              * Haruka::Planet::seaLevelAttenuation(baseH);
    // Paridad con los eval de teselado: en TIERRA (baseH > 0) se desliza la función del nivel del mar,
    // o el océano (esfera en R) lo taparía. La tierra queda ≥ R, el agua solo rellena los océanos.
    if (baseH > 0.0f) det = glm::max(det, -baseH);
    return (double)baseH + (double)det;
}

Haruka::FieldSample TerrestrialPlanet::fieldSampleAt(const glm::vec3& dirIn) const {
    const glm::dvec3 dir = glm::normalize(glm::dvec3(dirIn));
    Haruka::FieldSample s;
    const double hM = sampleHeight(glm::vec3(dir));
    s.elevKm = (float)(hM / 1000.0);
    const bool ocean = s.elevKm < 0.0f;
    s.tempC    = (float)m_climate.temperature(glm::vec3(dir), s.elevKm, ocean);
    s.humidity = (float)m_climate.humidity(glm::vec3(dir), s.elevKm, ocean);
    return s;
}

float TerrestrialPlanet::densityMapAt(const std::string& path, const glm::vec3& dirIn) const {    const glm::dvec3 dir = glm::normalize(glm::dvec3(dirIn));
    const auto found = m_densityMaps.find(path);
    if (found != m_densityMaps.end()) {
        const DensityMap& dm = found->second;
        if (dm.cpu.empty() || dm.w <= 0 || dm.h <= 0) return 1.0f;
        // Mismo muestreo bilineal equirectangular que sampleElevMap: R va en el canal rojo.
        const double u = 0.5 + std::atan2(dir.z, dir.x) * 0.1591549430918953;
        const double v = 0.5 - std::asin(glm::clamp(dir.y, -1.0, 1.0)) * 0.3183098861837907;
        const double fx = u * dm.w - 0.5, fy = v * dm.h - 0.5;
        int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const float tx = (float)(fx - x0), ty = (float)(fy - y0);
        auto at = [&](int x, int y) -> float {
            x = ((x % dm.w) + dm.w) % dm.w;             // longitud envuelve
            y = glm::clamp(y, 0, dm.h - 1);             // latitud no
            return dm.cpu[((size_t)y * dm.w + x) * 4] / 255.0f;
        };
        const float a = at(x0, y0)     + (at(x0 + 1, y0)     - at(x0, y0))     * tx;
        const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
        return glm::clamp(a + (b - a) * ty, 0.0f, 1.0f);
    }
    // Carga diferida por ruta (mismos candidatos que zone/elev; cacheada para el resto del pase).
    DensityMap dm;
    int zw = 0, zh = 0, zn = 0;
    unsigned char* zp = nullptr;
    const std::string cands[] = {
        path,
        Haruka::AssetPaths::projectRoot() + path,
        Haruka::AssetPaths::projectTextures() + path,
        Haruka::AssetPaths::textures() + path,
    };
    for (const auto& c : cands) { zp = stbi_load(c.c_str(), &zw, &zh, &zn, 4); if (zp) break; }
    if (!zp) {
        // Ruta inencontrable: cachea VACÍA para no reintentar stbi_load por celda (el placer
        // pregunta por cada spot) y el muestreo devolverá 1.0 (la capa no está limitada por mapa).
        dm.w = dm.h = 0;
        m_densityMaps.emplace(path, dm);
        m_densityMapOrder.push_back(path);
        HARUKA_LOGW("SimplePlanet", "'%s': mapa de distribución de props NO encontrado: %s",
                    m_config.name.c_str(), path.c_str());
        return 1.0f;
    }
    dm.cpu.assign(zp, zp + (size_t)zw * zh * 4);
    dm.w = zw; dm.h = zh;
    stbi_image_free(zp);
    m_densityMaps.emplace(path, dm);
    m_densityMapOrder.push_back(path);
    HARUKA_LOGI("SimplePlanet", "'%s': mapa de distribución de props %dx%d",
                m_config.name.c_str(), zw, zh);
    return densityMapAt(path, glm::vec3(dir));
}

std::string TerrestrialPlanet::zoneNameAt(const glm::vec3& dirIn) const {
    // Zonas GEOMÉTRICAS primero: el perímetro (círculo/polígono) es la delimitación más explícita;
    // una ciudad dentro de su polígono manda aunque el color pintado de ahí dijera otra cosa.
    if (!m_namedZones.empty()) {
        for (const auto& z : m_namedZones) {
            if (z.hasGeometry() && zoneShapeContains(z.shape, dirIn, m_config.radius))
                return z.name;
        }
    }

    // Sin zonas PINTADAS ni capas que consulten el material (`when: layer`), el zoneMap no puede
    // aportar NADA a la zona de los props: se decide solo por los perímetros geométricos (ya
    // comprobados). Se evita muestrear el mapa (bilineal sobre 4k ×2 + el match) en cada celda
    // del scatter global (~61k celdas) — véase `m_propNeedZoneMap` en build().
    if (!m_propNeedZoneMap) return {};

    // Zonas PINTADAS después: color del zoneMap más parecido a una zona declarada (vecino más
    // cercano, sin umbral: un color de borde/bilineal debe caer en la zona más próxima), y solo
    // entonces cae al material (el MISMO desempate que el terreno).
    if (m_zoneCPU.empty()) return {};
    glm::vec3 zc;
    if (!sampleZone(m_zoneCPU, m_zoneW, m_zoneH, dirIn, zc)) return {};
    int best = -1; float bestD = 0.0f;
    for (size_t i = 0; i < m_namedZones.size(); ++i) {
        const auto& z = m_namedZones[i];
        if (!z.hasColor()) continue;
        const glm::vec3 d = z.color - zc;
        const float dist = glm::dot(d, d);
        if (best < 0 || dist < bestD) { best = (int)i; bestD = dist; }
    }
    if (best >= 0) return m_namedZones[(size_t)best].name;

    return materialNameAt(dirIn);
}

std::string TerrestrialPlanet::materialNameAt(const glm::vec3& dirIn) const {
    // Solo el MATERIAL del terreno (zoneToMaterial), sin el recubrimiento de zonas nombradas.
    if (m_zoneCPU.empty()) return {};
    glm::vec3 zc;
    if (!sampleZone(m_zoneCPU, m_zoneW, m_zoneH, dirIn, zc)) return {};
    const int mi = zoneToMaterial(m_materialTable, zc);
    if (mi < 0 || mi >= (int)m_materialTable.materials.size()) return {};
    return m_materialTable.materials[(size_t)mi].name;
}

Haruka::RHI::TextureHandle TerrestrialPlanet::uploadPropDensityMap(const std::string& path) {
    // Misma búsqueda de candidatos que `densityMapAt`: relativa al cwd, al proyecto, a sus
    // texturas y a las texturas del motor. Solo sube a GPU (la vista de spawn); la copia CPU
    // sigue viviendo en `densityMapAt` para el placer.
    int zw = 0, zh = 0, zn = 0;
    unsigned char* zp = nullptr;
    const std::string cands[] = {
        path,
        Haruka::AssetPaths::projectRoot() + path,
        Haruka::AssetPaths::projectTextures() + path,
        Haruka::AssetPaths::textures() + path,
    };
    for (const auto& c : cands) { zp = stbi_load(c.c_str(), &zw, &zh, &zn, 4); if (zp) break; }
    if (!zp) {
        HARUKA_LOGW("SimplePlanet", "'%s': densityMap NO encontrado para la vista de spawn: %s",
                    m_config.name.c_str(), path.c_str());
        return m_propWhiteTex;
    }
    RHI::TextureDesc td;
    td.width = (uint32_t)zw; td.height = (uint32_t)zh;
    td.format = RHI::Format::RGBA8;
    td.filter = RHI::Filter::Linear;
    td.wrap   = RHI::Wrap::Repeat;
    td.mipmaps = true;
    td.initialData = zp;
    RHI::TextureHandle tex;
    if (RHI::Device* tdev = RHI::device()) tex = tdev->createTexture(td);
    stbi_image_free(zp);
    HARUKA_LOGI("SimplePlanet", "'%s': densityMap %dx%d subido para la vista de spawn",
                m_config.name.c_str(), zw, zh);
    return RHI::valid(tex) ? tex : m_propWhiteTex;
}

// Nivel del mar que deja EXACTAMENTE `landFraction` de la superficie por encima.
//
// Se muestrea el campo en una retícula cube-sphere (área aproximadamente uniforme, mejor que
// lat/lon que sobre-muestrea los polos), se ordena y se toma el cuantil. Calcularlo así en vez de
// fijar una cota tiene una propiedad que importa: la cantidad de tierra deja de depender de cómo
// haya caído el reparto de placas de esta semilla. Pides 29 % y sale 29 %, con las placas que sean.
template <typename HeightFn>
static float computeSeaLevel(const HeightFn& heightFn, double landFraction) {
    landFraction = glm::clamp(landFraction, 0.001, 0.999);
    const int R = 48;                                  // 6·48² = 13 824 muestras: sobra y es barato
    std::vector<float> hs;
    hs.reserve((size_t)6 * R * R);
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j < R; ++j)
            for (int i = 0; i < R; ++i) {
                const double lx = ((double)i + 0.5) / R * 2.0 - 1.0;
                const double ly = ((double)j + 0.5) / R * 2.0 - 1.0;
                hs.push_back(heightFn(cubeFaceToDir((PlanetFace)face, lx, ly)));
            }
    // El cuantil que deja `landFraction` ARRIBA es el (1 - landFraction) de la distribución.
    const size_t k = (size_t)((1.0 - landFraction) * (hs.size() - 1));
    std::nth_element(hs.begin(), hs.begin() + k, hs.end());
    const float q = hs[k];

    // MARGEN por debajo del cuantil. El campo de placas es casi BIMODAL —fondo oceánico a −3 km,
    // meseta continental a +0.5 km— así que el cuantil cae justo ENCIMA de la meseta y, al restarlo,
    // toda la tierra emergida queda exactamente a cota 0: coplanar con la esfera de agua, que es
    // z-fighting garantizado (se veía como moteado sobre el mar). Bajando el nivel un 0.5 % del
    // rango, la meseta queda ligeramente por encima y deja de pelearse con el agua.
    const auto mm = std::minmax_element(hs.begin(), hs.end());
    const float range = *mm.second - *mm.first;
    return q - 0.005f * range;
}

// Forward declaration for evaluateBiomeAlbedo (defined below in the ProcGraph section)
static RGBAImage evaluateBiomeAlbedo(int biomeIdx,
                                     float warmth, float humidity,
                                     int texSize);

// ⚠️ `loadBiomeTextures` YA NO EXISTE.
//
// Cargaba ocho texturas sueltas de bioma (sand/grass/land/rock × albedo/normal) y, si faltaban los
// PNG, las generaba procedurales con su normal map derivado. Ninguna la muestreaba ya ningún shader:
// el terreno elige su textura por CAPA del array de materiales desde hace tiempo, y estas eran el
// camino anterior que nadie retiró. Medido: 8 cargas de ~3 s y ~176 MB de VRAM por planeta.
//
// La última que sobrevivía era la arena de la orilla, y era el caso más claro del problema: cargaba
// OTRA VEZ el mismo `sand_albedo.png` que ya vive en el array. Ahora la orilla usa la capa del array
// que la tabla de materiales declara (`TerrainMaterialTable::shoreLayer`), así que hay un solo
// fichero, una sola copia en VRAM y una sola verdad sobre cuál es la arena.

// ── ProcGraph biome texture generation ──────────────────────────────────

// Describes the color palette for a single biome albedo
struct BiomePalette {
    float baseHue;       // 0-1 hue for the dominant color
    float hueVariation;  // how much the noise shifts hue
    float saturation;    // 0-1
    float lightness;     // 0-1
    float noiseScale;    // Perlin noise scale
    float detailStrength;// how much noise perturbs the output
};

static const BiomePalette kPalettes[4] = {
    /* Sand  */ {0.10f, 0.03f, 0.30f, 0.75f, 2.0f, 0.10f},
    /* Grass */ {0.30f, 0.06f, 0.55f, 0.50f, 3.0f, 0.15f},
    /* Land  */ {0.25f, 0.08f, 0.45f, 0.35f, 2.5f, 0.12f},
    /* Rock  */ {0.08f, 0.02f, 0.15f, 0.55f, 4.0f, 0.20f},
};

// Build a ProcGraph for a biome albedo texture and evaluate it.
// `warmth` (0=cold, 1=hot) and `humidity` (0=dry, 1=wet) shift the palette.
static RGBAImage evaluateBiomeAlbedo(int biomeIdx,
                                      float warmth, float humidity,
                                      int texSize)
{
    const BiomePalette& pal = kPalettes[biomeIdx % 4];
    float hueShift = (warmth - 0.5f) * 0.06f;
    float satBoost = (humidity - 0.5f) * 0.15f;

    // Build a multi-layer graph: domain-warped FBM + Voronoi detail
    Graph graph;

    // Layer 1: domain-warped FBM (macro structure)
    int warpNoise = graph.emplaceNode<FBMNode>();
    {
        FBMNode* f = static_cast<FBMNode*>(graph.node(warpNoise));
        f->setNoiseFn([](float x, float y, float z, int s) { return PerlinNode::perlin(x, y, z, s); });
    }
    int warpScale = graph.emplaceNode<ConstNode>(pal.noiseScale * 0.3f);
    graph.connect(warpScale, 0, warpNoise, 0);
    int warpSeed = graph.emplaceNode<ConstNode>((float)(biomeIdx + 50));
    graph.connect(warpSeed, 0, warpNoise, 4);

    // Domain warp: offset sampling coords by warpNoise output
    int mainNoise = graph.emplaceNode<FBMNode>();
    {
        FBMNode* f = static_cast<FBMNode*>(graph.node(mainNoise));
        f->setNoiseFn([](float x, float y, float z, int s) { return PerlinNode::perlin(x, y, z, s); });
    }
    int mainScale = graph.emplaceNode<ConstNode>(pal.noiseScale);
    graph.connect(mainScale, 0, mainNoise, 0);
    int mainSeed = graph.emplaceNode<ConstNode>((float)biomeIdx);
    graph.connect(mainSeed, 0, mainNoise, 4);

    // Layer 2: Voronoi cell noise for cracks/patches
    int voronoi = graph.emplaceNode<VoronoiNode>(VoronoiNode::F1, VoronoiNode::Euclidean);
    int voronoiScale = graph.emplaceNode<ConstNode>(pal.noiseScale * 2.0f);
    graph.connect(voronoiScale, 0, voronoi, 0);
    int voronoiSeed = graph.emplaceNode<ConstNode>((float)(biomeIdx + 200));
    graph.connect(voronoiSeed, 0, voronoi, 1);

    // Blend: 70% mainNoise + 30% voronoi distance
    int blendNode = graph.emplaceNode<BlendNode>(BlendMode::Mix);
    graph.connect(mainNoise, 0, blendNode, 0);
    graph.connect(voronoi, 0, blendNode, 1);
    int blendFactor = graph.emplaceNode<ConstNode>(0.7f);
    graph.connect(blendFactor, 0, blendNode, 2);

    // Clamp output to 0-1
    int clampNode = graph.emplaceNode<ClampNode>();
    graph.connect(blendNode, 0, clampNode, 0);

    graph.compile();

    RGBAImage img(texSize, texSize);
    int w = texSize, h = texSize;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float fx = (float)x;
            float fy = (float)y;

            // Domain warp: sample warp noise, use it to offset coords for main noise
            float warp = graph.evaluate(warpNoise, 0, fx, fy, 0).asFloat();
            warp = (warp - 0.5f) * 30.0f; // warp strength
            float wx = fx + warp;
            float wy = fy + warp * 0.7f;

            // Evaluate main noise at warped coords
            float mainN = graph.evaluateNodeAt(mainNoise, 0, wx, wy, 0).asFloat();
            float voronoiN = graph.evaluateNodeAt(voronoi, 0, fx, fy, 0).asFloat();
            float blended = mainN * 0.7f + voronoiN * 0.3f;
            blended = glm::clamp(blended, 0.0f, 1.0f);

            // Map noise to hue variation around base hue (modulated by climate)
            float hue = pal.baseHue + hueShift + (blended - 0.5f) * pal.hueVariation * 2.0f;
            float sat = glm::clamp(pal.saturation + satBoost + blended * pal.detailStrength, 0.0f, 1.0f);
            float lig = pal.lightness + (blended - 0.5f) * 0.2f;
            sat = glm::clamp(sat, 0.0f, 1.0f);
            lig = glm::clamp(lig, 0.0f, 1.0f);

            // HSL → RGB
            auto hueToRgb = [](float p, float q, float t) -> float {
                if (t < 0) t += 1;
                if (t > 1) t -= 1;
                if (t < 1.0f/6) return p + (q - p) * 6 * t;
                if (t < 1.0f/2) return q;
                if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
                return p;
            };
            float q = lig < 0.5f ? lig * (1 + sat) : lig + sat - lig * sat;
            float p = 2 * lig - q;
            float r = hueToRgb(p, q, hue + 1.0f/3);
            float gv = hueToRgb(p, q, hue);
            float b = hueToRgb(p, q, hue - 1.0f/3);

            img.setPixel(x, y,
                (uint8_t)(glm::clamp(r, 0.0f, 1.0f) * 255),
                (uint8_t)(glm::clamp(gv, 0.0f, 1.0f) * 255),
                (uint8_t)(glm::clamp(b, 0.0f, 1.0f) * 255),
                255);
        }
    }
    return img;
}

// ── Horneado PARALELO por filas ─────────────────────────────────────────
// `Graph::evaluate` muta buffers scratch INTERNOS del grafo (m_temp/m_inputs), así que un MISMO
// grafo no puede ejecutarse desde dos threads a la vez. Pero construir un grafo es determinista y
// barato, y TODOS los nodos del bake (PerlinNode/FBMNode/BiomeClassifyNode/SampleElevationNode/
// SampleClimateNode/… ) son puros: mismo (x,y,z) → mismo valor, sin estado compartido. Así que
// cada thread construye SU grafo idéntico y evalúa un rango DISJUNTO de filas: los píxeles salen
// EXACTAMENTE igual que single-thread, pero en N threads. Los nodos `BranchNode`/`KnotNode` (con
// estática mutable) NO se usan en estos horneados — si algún día se añaden, hay que excluirlos de
// la paralelización (o hacerlos puros).
//
// `buildGraph` recibe el grafo por REFERENCIA y lo rellena (nada de moves): los nodos guardan un
// raw pointer al grafo (`m_graph`, usado por DomainNode) que solo es válido si el grafo no se
// mueve. `rowFn(graph, y)` pinta la fila `y` — cada thread escribe filas disjuntas, sin races.
template<typename BuildGraphFn, typename RowFn>
static void parallelBake(int width, int height, BuildGraphFn&& buildGraph, RowFn&& rowFn) {
    unsigned hw = std::thread::hardware_concurrency();
    int nThreads = std::clamp<int>((int)hw, 1, 8);
    std::vector<std::thread> threads;
    threads.reserve(nThreads);
    for (int t = 0; t < nThreads; ++t) {
        const int y0 = t * height / nThreads;
        const int y1 = (t + 1) * height / nThreads;
        threads.emplace_back([&, t, y0, y1]() {
            Graph graph;                          // grafo PRIVADO de este thread (idéntico)
            buildGraph(graph);
            for (int y = y0; y < y1; ++y) rowFn(graph, y);
        });
    }
    for (auto& th : threads) th.join();
    (void)width;
}

// Generate a macro-variation texture (equirectangular) that breaks tile repetition.
// Sized mapRes², uses climate + large-scale noise → RGBA = (brightness, hueShift, detail, _)
static RGBAImage generateMacroVariation(const Haruka::Planet::ClimateOutput& climate,
                                         const Haruka::Planet::GeologyOutput& geology,
                                         int width, int height)
{
    auto buildGraph = [&](Graph& graph) -> int {
        int elevNode = graph.emplaceNode<SampleElevationNode>();
        static_cast<SampleElevationNode*>(graph.node(elevNode))->geology = &geology;

        int climateNode = graph.emplaceNode<SampleClimateNode>();
        auto* cn = static_cast<SampleClimateNode*>(graph.node(climateNode));
        cn->climate = &climate;
        cn->geology = &geology;

        int noiseNode = graph.emplaceNode<FBMNode>();
        {
            FBMNode* fbm = static_cast<FBMNode*>(graph.node(noiseNode));
            fbm->setNoiseFn([](float x, float y, float z, int seed) {
                return PerlinNode::perlin(x, y, z, seed);
            });
        }
        int noiseScale = graph.emplaceNode<ConstNode>(0.3f);
        graph.connect(noiseScale, 0, noiseNode, 0);
        int noiseSeed = graph.emplaceNode<ConstNode>(999.0f);
        graph.connect(noiseSeed, 0, noiseNode, 4);

        int tempMap = graph.emplaceNode<MapRangeNode>();
        graph.connect(climateNode, 0, tempMap, 0);
        int tMin = graph.emplaceNode<ConstNode>(-30.0f);
        graph.connect(tMin, 0, tempMap, 1);
        int tMax = graph.emplaceNode<ConstNode>(30.0f);
        graph.connect(tMax, 0, tempMap, 2);
        int tOutMin = graph.emplaceNode<ConstNode>(0.0f);
        graph.connect(tOutMin, 0, tempMap, 3);
        int tOutMax = graph.emplaceNode<ConstNode>(1.0f);
        graph.connect(tOutMax, 0, tempMap, 4);

        int mixNode = graph.emplaceNode<BlendNode>(BlendMode::Mix);
        graph.connect(noiseNode, 0, mixNode, 0);
        graph.connect(tempMap, 0, mixNode, 1);
        int mixFactor = graph.emplaceNode<ConstNode>(0.6f);
        graph.connect(mixFactor, 0, mixNode, 2);

        int clampNode = graph.emplaceNode<ClampNode>();
        graph.connect(mixNode, 0, clampNode, 0);

        graph.compile();
        return clampNode;
    };

    // Índice del nodo de salida: la construcción es IDÉNTICA en cada thread, así que se calcula
    // una vez en un grafo de plantilla y se comparte como int (lectura sola, sin race).
    Graph templateGraph;
    const int clampNode = buildGraph(templateGraph);

    RGBAImage img(width, height);
    parallelBake(width, height,
        [&](Graph& graph) { (void)buildGraph(graph); },
        [&](Graph& graph, int y) {
            // Convención del mapa de zonas (`tools/gen_planet_zones.py`): texel-center, longitudes
            // centradas (lon 0 en la columna central) y polo norte arriba — la inversa exacta de
            // `equirectUV`, para que muestrear en el shader con esa función dé el mismo valor.
            double vv = ((double)y + 0.5) / height;
            double lat = (0.5 - vv) * glm::pi<double>();
            double cLat = std::cos(lat);
            double sLat = std::sin(lat);
            for (int x = 0; x < width; ++x) {
                double u = ((double)x + 0.5) / width;
                double lon = (u - 0.5) * glm::two_pi<double>();
                double cLon = std::cos(lon);
                double sLon = std::sin(lon);
                glm::dvec3 dir(cLon * cLat, sLat, sLon * cLat);

                float v = graph.evaluate(clampNode, 0, (float)dir.x, (float)dir.y, (float)dir.z).asFloat();
                v = glm::clamp(v, 0.0f, 1.0f);

                float hueShift = 0.0f;
                {
                    Graph g2;
                    int n2 = g2.emplaceNode<FBMNode>();
                    {
                        FBMNode* f = static_cast<FBMNode*>(g2.node(n2));
                        f->setNoiseFn([](float x, float y, float z, int s) {
                            return PerlinNode::perlin(x, y, z, s);
                        });
                    }
                    int s2 = g2.emplaceNode<ConstNode>(0.15f);
                    g2.connect(s2, 0, n2, 0);
                    int sd2 = g2.emplaceNode<ConstNode>(888.0f);
                    g2.connect(sd2, 0, n2, 4);
                    g2.compile();
                    hueShift = g2.evaluate(n2, 0, (float)dir.x, (float)dir.y, (float)dir.z).asFloat();
                    hueShift = (hueShift - 0.5f) * 0.08f;
                }

                img.setPixel(x, y,
                    (uint8_t)(v * 255),
                    (uint8_t)((hueShift * 0.5f + 0.5f) * 255),
                    (uint8_t)((1.0f - v) * 128 + 64),
                    255);
            }
        });
    return img;
}

// Generate biome map equirectangular texture using ProcGraph.
// Replaces GLSL biomeColor() classification with a data-driven texture.
static RGBAImage generateBiomeMap(const Haruka::Planet::ClimateOutput& climate,
                                   const Haruka::Planet::GeologyOutput& geology,
                                   int width, int height,
                                   float noiseScale,
                                   const Haruka::Tools::ProcGraph::BiomeConfig& cfg)
{
    using namespace Haruka::Tools::ProcGraph;
    auto buildGraph = [&](Graph& graph) -> int {
        int noiseNode = -1;
        if (noiseScale > 0) {
            noiseNode = graph.emplaceNode<FBMNode>();
            FBMNode* fbm = static_cast<FBMNode*>(graph.node(noiseNode));
            fbm->setNoiseFn([](float x, float y, float z, int seed) {
                return PerlinNode::perlin(x, y, z, seed);
            });
            int ns = graph.emplaceNode<ConstNode>(noiseScale);
            graph.connect(ns, 0, noiseNode, 0);
            int nSeed = graph.emplaceNode<ConstNode>(42.0f);
            graph.connect(nSeed, 0, noiseNode, 4);
        }

        int bcNode = graph.emplaceNode<BiomeClassifyNode>();
        auto* bc = static_cast<BiomeClassifyNode*>(graph.node(bcNode));
        bc->climate = &climate;
        bc->geology = &geology;
        bc->config = cfg;

        if (noiseNode >= 0)
            graph.connect(noiseNode, 0, bcNode, 0);

        graph.compile();
        return bcNode;
    };

    // Mismo esquema que la macro: el índice del nodo de salida es determinista (construcción
    // idéntica en todos los threads), se calcula una vez y se comparte como int de solo lectura.
    Graph templateGraph;
    const int bcNode = buildGraph(templateGraph);

    RGBAImage img(width, height);
    parallelBake(width, height,
        [&](Graph& graph) { (void)buildGraph(graph); },
        [&](Graph& graph, int y) {
            // Misma convención que la macro (inversa de `equirectUV`): texel-center, lon 0 en el
            // centro, polo norte arriba — coincidiendo con `tools/gen_planet_zones.py`.
            double vv = ((double)y + 0.5) / height;
            double lat = (0.5 - vv) * glm::pi<double>();
            double cLat = std::cos(lat);
            double sLat = std::sin(lat);
            for (int x = 0; x < width; ++x) {
                double u = ((double)x + 0.5) / width;
                double lon = (u - 0.5) * glm::two_pi<double>();
                double cLon = std::cos(lon);
                double sLon = std::sin(lon);
                glm::dvec3 dir(cLon * cLat, sLat, sLon * cLat);

                Value v = graph.evaluate(bcNode, 0, (float)dir.x, (float)dir.y, (float)dir.z);
                float r = v.data[0];
                float g = v.data[1];
                float b = v.data[2];
                float idx = v.data[3];

                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(idx)) {
                    HARUKA_LOGE("BiomeMap", "NON-FINITE biome value at (%d,%d) dir=(%f,%f,%f) r=%f g=%f b=%f idx=%f",
                                x, y, dir.x, dir.y, dir.z, r, g, b, idx);
                    r = g = b = idx = 0.0f;
                }

                img.setPixel(x, y,
                    (uint8_t)(glm::clamp(r, 0.0f, 1.0f) * 255.0f),
                    (uint8_t)(glm::clamp(g, 0.0f, 1.0f) * 255.0f),
                    (uint8_t)(glm::clamp(b, 0.0f, 1.0f) * 255.0f),
                    (uint8_t)(glm::clamp(idx, 0.0f, 1.0f) * 255.0f));
            }
        });
    return img;
}

// ── Cache en disco de los horneados deterministas ────────────────────────
// Los dos horneados de arriba (macro-variación y mapa de biomas) son FUNCIONES PURAS de
// (geología, clima, resolución, biomeConfig): mismo planeta → mismos píxeles, siempre. Con
// `texRes` alto son cientos de MB; volver a evaluarlos en cada arranque es tirar minutos de CPU
// (o dejar de poder subirlos a la GPU). Este cache los persiste como PNG (writePNG, formato
// "stored" = determinista) y reutiliza el archivo si la clave no ha cambiado.
//
//   clave = FNV-1a sobre (versión, seed efectiva, texRes, biomeConfig completa)
//
// Cualquier cosa que cambie el resultado del horneado (semilla del planeta, resolución pedida, un
// color de la biomePalette, un umbral) invalida el archivo y fuerza a re-hornear; lo que no lo
// cambia (nombre, ruta, máquina) se reutiliza. La clave NO incluye rutas ni nombres de archivo.
namespace {

inline void bakeHashMix(uint64_t& h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;   // FNV-1a 64
    }
}

std::string bakeCacheKey(uint32_t seed, int w, int h,
                         const Haruka::Tools::ProcGraph::BiomeConfig& bc) {
    uint64_t hsh = 1469598103934665603ull;
    const char* version = "planet-bake-v2";   // v2: convención equirect = inversa de `equirectUV`
    bakeHashMix(hsh, version, std::strlen(version));
    bakeHashMix(hsh, &seed, sizeof(seed));
    bakeHashMix(hsh, &w, sizeof(w));
    bakeHashMix(hsh, &h, sizeof(h));
    auto mixVec = [&hsh](const glm::vec3& v) {
        bakeHashMix(hsh, &v.x, sizeof(v.x));
        bakeHashMix(hsh, &v.y, sizeof(v.y));
        bakeHashMix(hsh, &v.z, sizeof(v.z));
    };
    mixVec(bc.desert);  mixVec(bc.steppe);  mixVec(bc.grass);  mixVec(bc.forest);  mixVec(bc.jungle);
    mixVec(bc.ice);     mixVec(bc.tundra);  mixVec(bc.taiga);  mixVec(bc.savanna);
    float f;
    auto mixF = [&hsh](float x) { bakeHashMix(hsh, &x, sizeof(x)); };
    f = bc.steppeEdge0; mixF(f); f = bc.steppeEdge1; mixF(f);
    f = bc.grassEdge0;  mixF(f); f = bc.grassEdge1;  mixF(f);
    f = bc.forestEdge0; mixF(f); f = bc.forestEdge1; mixF(f);
    f = bc.jungleEdge0; mixF(f); f = bc.jungleEdge1; mixF(f);
    f = bc.iceEdge0;    mixF(f); f = bc.iceEdge1;    mixF(f);
    f = bc.coldEdge0;   mixF(f); f = bc.coldEdge1;   mixF(f);
    f = bc.warmEdge0;   mixF(f); f = bc.warmEdge1;   mixF(f);
    f = bc.taigaEdge0;  mixF(f); f = bc.taigaEdge1;  mixF(f);
    f = bc.savannaEdge0; mixF(f); f = bc.savannaEdge1; mixF(f);
    f = bc.hotJungleEdge0; mixF(f); f = bc.hotJungleEdge1; mixF(f);
    f = bc.edgeNoiseStrength; mixF(f);

    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hsh);
    return std::string(hex);
}

/** @brief Carpeta de los horneados: la del PROYECTO abierto si lo hay, si no relativa al binario. */
std::string bakeCacheDir() {
    const std::string& root = Haruka::AssetPaths::projectRoot();
    return root.empty() ? std::string("bakes/") : root + "bakes/";
}

std::string bakeCachePath(uint32_t seed, int w, int h,
                          const Haruka::Tools::ProcGraph::BiomeConfig& bc,
                          const char* tag) {
    return bakeCacheDir() + bakeCacheKey(seed, w, h, bc) + "_" + tag + ".png";
}

bool loadBakedPNG(const std::string& path, RGBAImage& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!p) {
        // ⚠️ POR QUÉ SE REGISTRA EL MOTIVO. Sin esto, un fallo de lectura era indistinguible de "no
        // había caché": `loadOrBake` se ponía a hornear 3 MINUTOS sin decir por qué. Pasó de verdad —
        // el fichero era válido (PIL y stb lo leían por separado) y el motor lo re-horneó igual. La
        // causa más probable a estos tamaños es la MEMORIA: un bake de 18750×9375 son 703 MB para stb
        // más otros 703 MB del vector de salida, 1,4 GB de pico, y si el malloc falla `stbi_load`
        // devuelve null. Con el motivo a la vista se distingue en un segundo.
        if (std::filesystem::exists(path))
            HARUKA_LOGW("BakeCache", "el fichero EXISTE pero no se pudo leer (%s): '%s' -> se re-hornea",
                        stbi_failure_reason() ? stbi_failure_reason() : "sin motivo", path.c_str());
        return false;
    }
    out.width = w; out.height = h;
    out.pixels.assign(p, p + (size_t)w * h * 4);
    stbi_image_free(p);
    return true;
}

bool saveBakedPNG(const std::string& path, const RGBAImage& img) {
    return Haruka::writePNG(path, img.width, img.height, 4, img.data());
}

/** @brief Carga del cache o, si no existe, hornea y persiste. El archivo es 1:1 con los
 *  píxeles que se hornean, así que cargar del disco y hornear dan EXACTAMENTE lo mismo. */
RGBAImage loadOrBake(const std::string& path, const std::function<RGBAImage()>& bake) {
    RGBAImage img;
    if (loadBakedPNG(path, img)) {
        HARUKA_LOGI("BakeCache", "cached  %s", path.c_str());
        return img;
    }
    img = bake();
    if (!saveBakedPNG(path, img))
        HARUKA_LOGW("BakeCache", "no pude persistir %s", path.c_str());
    else
        HARUKA_LOGI("BakeCache", "baked   %s (%dx%d)", path.c_str(), img.width, img.height);
    return img;
}

/**
 * @brief Sube un horneado a la GPU aplicando el DOWNSCALE por calidad de terreno.
 *
 * El MAESTRO del bake queda COMPLETO en disco (con `texRes` alto son cientos de MB: los píxeles
 * 1:1 que vería el raymarch/streaming de fases posteriores). Pero una textura RGBA8 de
 * 45000×22500 son ~4 GB en RAM de vídeo: subirla entera vuela la VRAM y es un crash — y la
 * estabilidad es OBLIGATORIA. Aquí se reduce a la resolución objetivo de la calidad activa
 * (Ultra=maestro, High=8192, Medium=4096, Low=2048) antes de `createRHIFromRGBA`. El filtrado es
 * de CAJAS (entero, sin alias), el mismo que usa el mapa de zonas.
 */
Haruka::RHI::TextureHandle uploadBake(const RGBAImage& img) {
    if (img.width <= 0 || img.height <= 0) return {};
    int target = terrainQualityTarget();
    // TOPE DURO de estabilidad: aunque la calidad pida "maestro completo" (Ultra=0), una textura de
    // 45000×22500 son ~4 GB y supera además el límite de textura GL (16384 en la mayoría de GPUs).
    // El MAESTRO queda completo en disco; a la GPU sube como mucho este lado. Es el puente hasta
    // el streaming/virtual-texture (fase 3).
    if (target <= 0 || target > 16384) target = 16384;
    // Factor CEL: garantiza que el lado mayor subido a GPU quede <= target. qualityDownsampleFactor
    // divide a la baja (18750/16384=1 → subiría el maestro entero); ceil(18750/16384)=2 → 9375.
    const int size = std::max(img.width, img.height);
    const int f = (size + target - 1) / target;
    if (f <= 1)
        return Haruka::Tools::ProcGraph::createRHIFromRGBA(img);

    std::vector<unsigned char> reduced;
    boxDownsample(img.data(), img.width, img.height, f, reduced);
    RGBAImage scaled(img.width / f, img.height / f);
    scaled.pixels = std::move(reduced);
    HARUKA_LOGI("BakeCache", "upload %dx%d -> %dx%d (calidad %d, master completo en disco)",
                img.width, img.height, scaled.width, scaled.height, target);
    return Haruka::Tools::ProcGraph::createRHIFromRGBA(scaled);
}

// ===========================================================================
// ALTURA HORNEADA (fase 2a): el campo BASE —geología + mapa de elevación + recorte de zonas— como
// textura 16-bit. El objetivo NO es subirla: es que CPU (física) y GPU (tess/clipmap) lean la MISMA
// textura en las fases 2b/2c y la paridad deje de depender de ficheros gemelos. Aquí solo se hornea,
// cachea y sube como R32F (aún no cableada a los shaders).
// ===========================================================================

// Altura base horneada: metros convertidos a uint16 (offset +32768). El rango útil (±32767 m)
// cubre la Tierra y cualquier planeta con relieve geológico razonable; fuera de él se clampea.
struct BakedHeightField {
    int w = 0, h = 0;
    std::vector<uint16_t> data;   // metros + 32768, filas de arriba abajo (como los demás PNG)
};

bool loadBakedHeightPNG16(const std::string& path, BakedHeightField& out) {
    int w = 0, h = 0, n = 0;
    stbi_us* p = stbi_load_16(path.c_str(), &w, &h, &n, 1);
    if (!p) return false;
    out.w = w; out.h = h;
    out.data.assign(p, p + (size_t)w * h);
    stbi_image_free(p);
    return true;
}

bool saveBakedHeightPNG16(const std::string& path, const BakedHeightField& field) {
    return Haruka::writePNG16(path, field.w, field.h, field.data.data());
}

/** @brief Clave del cache de altura. A diferencia de macro/bioma (que solo dependen de seed+res),
 *  la altura base depende TAMBIÉN del mapa de elevación (y su rango), del mapa de zonas y de la
 *  tabla de materiales (su submersion recorta el signo). Cualquiera de esos cambia → re-horneo. */
std::string heightBakeKey(uint32_t seed, int w, int h,
                          const std::vector<uint16_t>& elevCPU,
                          const glm::vec2& elevRange,
                          const std::vector<unsigned char>& zoneCPU,
                          const Haruka::Planet::TerrainMaterialTable& mats,
                          float seaLevelM) {
    uint64_t hsh = 1469598103934665603ull;
    const char* version = "planet-height-v2";   // v2: convención equirect = inversa de `equirectUV`
    bakeHashMix(hsh, version, std::strlen(version));
    bakeHashMix(hsh, &seed, sizeof(seed));
    bakeHashMix(hsh, &w, sizeof(w));
    bakeHashMix(hsh, &h, sizeof(h));
    bakeHashMix(hsh, &seaLevelM, sizeof(seaLevelM));
    bakeHashMix(hsh, &elevRange, sizeof(elevRange));
    if (!elevCPU.empty()) bakeHashMix(hsh, elevCPU.data(), elevCPU.size());
    if (!zoneCPU.empty())  bakeHashMix(hsh, zoneCPU.data(),  zoneCPU.size());
    // Solo lo que decide la altura: zona (color) y submersion. Lo demás (tint, texturas…) no afecta
    // a la geometría y no debe invalidar el horneado.
    for (const auto& m : mats.materials) {
        if (!m.hasZone()) continue;
        bakeHashMix(hsh, &m.zoneColor, sizeof(m.zoneColor));
        bakeHashMix(hsh, &m.submerged, sizeof(m.submerged));
    }
    char hex[20];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hsh);
    return std::string(hex);
}

struct HeightUpload {
    Haruka::RHI::TextureHandle tex;
    std::vector<float> cpuField;   // el MISMO campo R32F subido a GPU (paridad física/render)
    int w = 0, h = 0;
};

HeightUpload uploadHeight(const BakedHeightField& field) {
    HeightUpload up;
    if (field.w <= 0 || field.h <= 0) return up;
    // La ALTURA es GEOMETRÍA: merece más resolución que los mapas de color — el coste por muestra no
    // depende del tamaño de la textura, solo la memoria. Sin doblarla, un pico real a 21 km/texel
    // (calidad Low) se convierte en una aguja, y con menos texeles la montaña verdadera se ve como
    // un pincho. Mínimo 4096, el doble de la calidad de color, sin bajar nunca del máster.
    int target = terrainQualityTarget();
    if (target <= 0) target = 8192;                       // Ultra (0 = máster completo): capamos
    target = std::clamp(target * 2, 4096, 8192);
    const int size = std::max(field.w, field.h);
    const int f = (size + target - 1) / target;
    const int dw = field.w / f, dh = field.h / f;
    if (dw <= 0 || dh <= 0) return up;

    // 16-bit → R32F, ya downscaleado por cajas (media de cada bloque f×f).
    up.w = dw; up.h = dh;
    up.cpuField.resize((size_t)dw * dh);
    const int np = f * f;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            int acc = 0;
            for (int j = 0; j < f; ++j)
                for (int i = 0; i < f; ++i)
                    acc += (int)field.data[(size_t)(y * f + j) * field.w + (x * f + i)];
            up.cpuField[(size_t)y * dw + x] = (float)(acc / np) - 32768.0f;
        }
    }

    // POLOS + COSTURA ±180°: dos imperfecciones del mapa real que se ven como PINCOS desde órbita.
    //  - En el polo todas las longitudes son EL MISMO punto: la fila debe ser CONSTANTE. Las filas de
    //    los polos del mapa traen ~8 km de variación (basura de la proyección), y la bilineal —que
    //    abraza el polo repitiendo la fila— la dibuja como un ANILLO de picos alrededor del polo.
    //  - La longitud ±180° es EL MISMO meridiano: la columna 0 y la última deben COINCIDIR. El mapa
    //    las trae descuadradas hasta ~1,4 km y la envoltura de longitud lo dibuja como una grieta.
    {
        const int lastR = dh - 1, lastC = dw - 1;
        // La fila del polo se COPIA de su contigua (la primera real), no se rellena con un valor
        // inventado: así el polo es CONSTANTE (un punto) y además CONTINUO con el interior — la
        // bilineal interpola la fila 0 con la 1 en la primera franja, y si divergieran saldría un
        // escalón circular alrededor del polo. La mediana de la fila basura (~+455 m sobre un Ártico
        // real de ~−4 km) dejaba una meseta gigante: constante, pero falsa.
        for (int x = 0; x < dw; ++x) {
            up.cpuField[(size_t)0 * dw + x]     = up.cpuField[(size_t)1 * dw + x];
            up.cpuField[(size_t)lastR * dw + x] = up.cpuField[(size_t)(lastR - 1) * dw + x];
        }
        // ⚠️ LA COSTURA NO SE FUERZA EN UN SOLO TÉXEL: se REPARTE.
        //
        // Copiar la columna 0 en la última hace el campo periódico, sí, pero vuelca TODO el desajuste
        // del máster en la transición `lastC-1 → lastC`.
        //
        // ⚠️ MEDIDO sobre un máster real de 18750×9375, POR FILA: la primera y la última columna
        // difieren **362 m de media y hasta 5,1 km**. (No confundir con la media de columna entera,
        // ~100 m: la diluyen los océanos llanos.) El gradiente típico del mapa es 9,5 m/téxel, así que
        // la costura es un acantilado de cientos de metros —hasta kilómetros— sobre un solo téxel de
        // 10,7 km, perfectamente recto a lo largo del meridiano ±180°. Una cresta recta capta la luz
        // rasante: es la "raya recta" que se ve de noche, y las sombras de terreno la acentúan.
        //
        // ⚠️ ESTO ES UNA MITIGACIÓN, NO UN ARREGLO. Repartir 362 m sobre 16 téxeles baja el escalón de
        // 415 a 152 m de media (medido), un 2,7×: se nota menos, pero sigue muy por encima de los
        // 9,5 m del terreno normal. Llevarlo al gradiente típico exigiría repartirlo sobre ~510
        // téxeles = 5 400 km, un séptimo del planeta. **El arreglo real es que el mapa sea periódico
        // en longitud** (que su última columna case con la primera); ningún truco del motor puede
        // esconder una discontinuidad de kilómetros.
        //
        // Reparto el desajuste como una RAMPA sobre las últimas columnas: la corrección vale 0 al
        // entrar en la banda y llega al desajuste completo en la costura. El campo sigue siendo
        // periódico (la última columna acaba igual a la primera, que es lo que exige la envoltura de
        // longitud) pero la pendiente cae ~kSeamBlend veces y deja de ser una arista.
        //
        // El coste es honesto: se altera el terreno en una banda de ~kSeamBlend téxeles junto al
        // meridiano. Cambiar ≤100 m repartidos sobre ~170 km es infinitamente menos visible que un
        // escalón de 100 m, y la física lee el MISMO array corregido, así que la paridad no se toca.
        {
            const int kSeamBlend = std::min(16, std::max(1, dw / 8));
            for (int y = 0; y < dh; ++y) {
                const float delta = up.cpuField[(size_t)y * dw + 0]
                                  - up.cpuField[(size_t)y * dw + lastC];
                for (int k = 0; k < kSeamBlend; ++k) {
                    const int x = lastC - (kSeamBlend - 1) + k;
                    if (x < 0) continue;
                    const float w = (float)(k + 1) / (float)kSeamBlend;   // 0 → 1 hacia la costura
                    up.cpuField[(size_t)y * dw + x] += delta * w;
                }
                // Exactitud en la costura: la última columna DEBE ser la primera, o la envoltura de
                // longitud vuelve a dibujar una grieta (aunque ahora fuese de centímetros).
                up.cpuField[(size_t)y * dw + lastC] = up.cpuField[(size_t)y * dw + 0];
            }
        }
    }

    // PINCOS: un texel AISLADO que queda estrictamente por encima/debajo de sus 4 vecinos por más de
    // 1,2 km es una aguja, no una ladera — a 10–21 km por texel la cumbre o fosa real mide un solo
    // píxel, y el render lo dibuja como un pincho de kilómetros. Se sustituye por la media de sus
    // vecinos. Las laderas CONECTADAS (costas, sierras) no se tocan. Se filtra ANTES de subir el
    // campo, y la física y el render leen el MISMO campo filtrado (paridad intacta). Con el doble
    // de resolución de arriba quedan 22 agujas en 3750×1875 (las 218 de 1875×937); el filtro las
    // recorta todas. Análisis replicado en /tmp/opencode/check_spikes.py.
    // Las filas de los polos y la columna de la costura YA son constantes (arreglo de arriba): no
    // se tocan aquí, o el filtro las volvería a variar con sus vecinos.
    {
        std::vector<float> src = up.cpuField;
        const float kSpikeT = 1200.0f;                    // m por encima/debajo del vecino más extremo
        const int lastR = dh - 1, lastC = dw - 1;
        for (int y = 1; y < lastR; ++y) {
            const int y0 = y - 1;
            const int y1 = y + 1;
            for (int x = 1; x < lastC; ++x) {
                const float h = src[(size_t)y * dw + x];
                const int x0 = x - 1;
                const int x1 = x + 1;
                float mx = -1e30f, mn = 1e30f, acc = 0.0f;
                const int nxs[4] = {x0, x1, x0, x1};
                const int nys[4] = {y0, y0, y1, y1};
                for (int k = 0; k < 4; ++k) {
                    const float v = src[(size_t)nys[k] * dw + nxs[k]];
                    mx = std::max(mx, v); mn = std::min(mn, v); acc += v;
                }
                if (h > mx + kSpikeT || h < mn - kSpikeT)
                    up.cpuField[(size_t)y * dw + x] = acc * 0.25f;
            }
        }
    }

    RHI::TextureDesc td;
    td.width = (uint32_t)dw; td.height = (uint32_t)dh;
    td.format = RHI::Format::R32F;
    td.filter = RHI::Filter::Linear;
    td.wrap   = RHI::Wrap::ClampToEdge;
    td.mipmaps = false;
    td.initialData = up.cpuField.data();
    if (RHI::Device* dev = RHI::device()) {
        HARUKA_LOGI("BakeCache", "upload height %dx%d -> %dx%d R32F (calidad %d)",
                    field.w, field.h, dw, dh, target);
        up.tex = dev->createTexture(td);
    }
    return up;
}

} // namespace

// ── Construcción (el planeta se crea a sí mismo) ─────────────────────────

bool TerrestrialPlanet::build(const TerrestrialPlanetConfig& cfg) {
    try {
    m_config = cfg;

    // Re-build (el editor regenera el planeta al cambiar su surfaceConfig): se descartan los
    // recursos que dependen de la config anterior y que `clearGPU` (vía buildMesh) no toca — el
    // mapa de zonas y el UBO de materiales. Si la nueva config no trae zona, el handle viejo
    // haría que el shader siguiera viendo un mapa que ya no está declarado.
    if (RHI::Device* zdev = RHI::device()) {
        if (RHI::valid(m_zoneTex))     { zdev->destroy(m_zoneTex);     m_zoneTex     = {}; }
        if (RHI::valid(m_materialUBO)) { zdev->destroy(m_materialUBO); m_materialUBO = {}; }
        if (RHI::valid(m_propUBO))     { zdev->destroy(m_propUBO);     m_propUBO     = {}; }
    }
    if (!RHI::valid(m_propWhiteTex)) {
        const unsigned char white[4] = { 255, 255, 255, 255 };
        RHI::TextureDesc wd;
        wd.width = 1; wd.height = 1;
        wd.format = RHI::Format::RGBA8;
        wd.filter = RHI::Filter::Nearest;
        wd.wrap   = RHI::Wrap::Repeat;
        wd.initialData = white;
        if (RHI::Device* wdev = RHI::device()) m_propWhiteTex = wdev->createTexture(wd);
    }

    // GEOLOGÍA: la semilla del planeta MANDA en el reparto de placas; sin esto la geología usaba
    // random_device y el mismo mundo salía distinto en cada arranque.
    Haruka::Planet::GeologyConfig geo;
    geo.numPlates = cfg.seed ? (cfg.seed % 20 + 4) : 12;
    geo.seed = cfg.seed ? cfg.seed : (uint32_t)std::hash<std::string>{}(cfg.name);
    m_seed = geo.seed;   // semilla EFECTIVA: la que manda en geología y, por tanto, en los horneados
    m_geology = Haruka::Planet::generateGeology(geo);
    m_climate = Haruka::Planet::ClimateOutput{};
    m_biomes = Haruka::Planet::BiomesOutput{};
    m_weather.configure((uint32_t)std::hash<std::string>{}(cfg.name));
    m_faceSubdiv = cfg.faceRes > 0 ? cfg.faceRes : 512;
    m_mapRes = cfg.surface.texRes > 0 ? cfg.surface.texRes : 512;
    m_macroRes = cfg.surface.macroRes > 0 ? cfg.surface.macroRes : 2048;

    // biomePalette del SceneObject (opcional). Todas las entradas opcionales: una escena que no la
    // declare mantiene los valores por defecto y se comporta como antes.
    if (cfg.raw.is_object()) {
        if (cfg.raw.contains("biomePalette")) {
            auto& bp = cfg.raw["biomePalette"];
            Haruka::Tools::ProcGraph::BiomeConfig bc;
            if (bp.contains("desert") && bp["desert"].is_array())
                bc.desert  = glm::vec3(bp["desert"][0],  bp["desert"][1],  bp["desert"][2]);
            if (bp.contains("steppe") && bp["steppe"].is_array())
                bc.steppe  = glm::vec3(bp["steppe"][0],  bp["steppe"][1],  bp["steppe"][2]);
            if (bp.contains("grass") && bp["grass"].is_array())
                bc.grass   = glm::vec3(bp["grass"][0],   bp["grass"][1],   bp["grass"][2]);
            if (bp.contains("forest") && bp["forest"].is_array())
                bc.forest  = glm::vec3(bp["forest"][0],  bp["forest"][1],  bp["forest"][2]);
            if (bp.contains("jungle") && bp["jungle"].is_array())
                bc.jungle  = glm::vec3(bp["jungle"][0],  bp["jungle"][1],  bp["jungle"][2]);
            // Thresholds
            if (auto& t = bp["thresholds"]; t.is_object()) {
                if (t.contains("steppe") && t["steppe"].is_array()) {
                    bc.steppeEdge0 = t["steppe"][0]; bc.steppeEdge1 = t["steppe"][1];
                }
                if (t.contains("grass") && t["grass"].is_array()) {
                    bc.grassEdge0  = t["grass"][0];  bc.grassEdge1  = t["grass"][1];
                }
                if (t.contains("forest") && t["forest"].is_array()) {
                    bc.forestEdge0 = t["forest"][0]; bc.forestEdge1 = t["forest"][1];
                }
                if (t.contains("jungle") && t["jungle"].is_array()) {
                    bc.jungleEdge0 = t["jungle"][0]; bc.jungleEdge1 = t["jungle"][1];
                }
            }
            // Fila FRÍA y fila CÁLIDA (el eje de temperatura).
            auto readColor = [&bp](const char* key, glm::vec3& out) {
                if (bp.contains(key) && bp[key].is_array() && bp[key].size() >= 3)
                    out = glm::vec3(bp[key][0], bp[key][1], bp[key][2]);
            };
            readColor("ice",     bc.ice);
            readColor("tundra",  bc.tundra);
            readColor("taiga",   bc.taiga);
            readColor("savanna", bc.savanna);

            if (auto& t = bp["thresholds"]; t.is_object()) {
                auto readPair = [&t](const char* key, float& e0, float& e1) {
                    if (t.contains(key) && t[key].is_array() && t[key].size() >= 2) {
                        e0 = t[key][0]; e1 = t[key][1];
                    }
                };
                readPair("taiga",      bc.taigaEdge0,     bc.taigaEdge1);
                readPair("savanna",    bc.savannaEdge0,   bc.savannaEdge1);
                readPair("hotJungle",  bc.hotJungleEdge0, bc.hotJungleEdge1);
                // Bandas de temperatura, en °C (no normalizadas: son los grados del clima).
                readPair("iceTempC",   bc.iceEdge0,  bc.iceEdge1);
                readPair("coldTempC",  bc.coldEdge0, bc.coldEdge1);
                readPair("warmTempC",  bc.warmEdge0, bc.warmEdge1);
            }

            if (bp.contains("edgeNoiseStrength"))
                bc.edgeNoiseStrength = bp["edgeNoiseStrength"].get<float>();
            m_biomeConfig = bc;
            HARUKA_LOGI("BiomeConfig", "custom palette for '%s'", cfg.name.c_str());
        }

        // MATERIALES DEL TERRENO desde la escena. `surface.materials` es una LISTA: el planeta
        // declara los suyos y el motor no sabe cuáles son. Ausente = tabla por defecto. Presente =
        // sustituye a la tabla ENTERA, no la amplía: mezclar defaults con los del autor haría que
        // borrar un material del JSON no lo borrase de la vista.
        if (cfg.raw.contains("materials") && cfg.raw["materials"].is_array()) {
            Haruka::Planet::TerrainMaterialTable table;
            for (const auto& jm : cfg.raw["materials"]) {
                if (!jm.is_object()) continue;
                Haruka::Planet::TerrainMaterial m;
                m.name = jm.value("name", std::string("unnamed"));
                auto range = [&jm](const char* key, float& lo, float& hi) {
                    if (jm.contains(key) && jm[key].is_array() && jm[key].size() >= 2) {
                        lo = jm[key][0]; hi = jm[key][1];
                    }
                };
                range("humidity", m.humMin,   m.humMax);
                range("tempC",    m.tempMin,  m.tempMax);
                range("slope",    m.slopeMin, m.slopeMax);
                // `elevKm`: la banda de ALTURA sobre el nivel del mar, en kilómetros. Es el eje que
                // permite pintar pisos altitudinales — arena abajo, roca arriba, nieve en la cumbre.
                //   "elevKm": [1.5, 9.0]      → solo por encima de 1500 m
                //   "elevKm": [-0.03, 0.012]  → la franja de orilla (lo que hoy hace el parche fijo)
                range("elevKm",   m.elevMinKm, m.elevMaxKm);
                m.elevFeatherKm = jm.value("elevFeatherKm", m.elevFeatherKm);
                if (jm.contains("tint") && jm["tint"].is_array() && jm["tint"].size() >= 3)
                    m.tint = glm::vec3(jm["tint"][0], jm["tint"][1], jm["tint"][2]);
                m.grain    = jm.value("grain",    m.grain);
                m.detail   = jm.value("detail",   m.detail);
                m.feather  = jm.value("feather",  m.feather);
                m.priority = jm.value("priority", m.priority);
                // La TEXTURA la nombra el proyecto. Sin `albedo`, el material es liso (hielo, sal…).
                m.albedo = jm.value("albedo", std::string());
                m.normal = jm.value("normal", std::string());
                // ZONA: el color con el que este material está pintado en el mapa, en 0-255.
                if (jm.contains("zone") && jm["zone"].is_array() && jm["zone"].size() >= 3)
                    m.zoneColor = glm::vec3(jm["zone"][0], jm["zone"][1], jm["zone"][2]);
                m.submerged = jm.value("submerged", false);
                // PAPEL EN LA COLUMNA: "bedrock" = roca de debajo · "cover" (por defecto) = manto
                // suelto de encima. Ver `core/planet/terrain_strata.h`.
                {
                    const std::string role = jm.value("role", std::string("cover"));
                    m.role = (role == "bedrock" || role == "lecho" || role == "roca")
                           ? Haruka::Planet::TerrainMaterial::Role::Bedrock
                           : Haruka::Planet::TerrainMaterial::Role::Cover;
                }
                // COLOR propio: sustituye al del bioma donde manda este material, en 0-1.
                if (jm.contains("color") && jm["color"].is_array() && jm["color"].size() >= 3) {
                    m.baseColor = glm::vec3(jm["color"][0], jm["color"][1], jm["color"][2]);
                    m.colorWeight = jm.value("colorWeight", 1.0f);
                }
                table.materials.push_back(m);
            }
            if (!table.materials.empty()) {
                m_materialTable = table;
                rebuildTerrainArrays();   // sus texturas, no las que supone el motor
                HARUKA_LOGI("Terrain", "'%s': %zu materiales desde la escena",
                            cfg.name.c_str(), table.materials.size());
            }
        }

        // ZONAS del autor (escena `zones`): un nombre + cómo se delimita. GEOMÉTRICAS: un círculo
        // (`center` [lat,lon] grados + `radiusM`) o un polígono (`perimeter` lista de [lat,lon]).
        // PINTADA: `color` (0-255) en el zoneMap. Pueden ser geométricas Y pintadas a la vez.
        // Independientes de los materiales — "city" es una zona aunque ningún material se llame así.
        // Las capas de props las referencian por nombre en su campo `zones`.
        m_namedZones.clear();
        if (cfg.raw.contains("zones") && cfg.raw["zones"].is_array()) {
            for (const auto& jz : cfg.raw["zones"]) {
                if (!jz.is_object()) continue;
                Haruka::Planet::NamedZone z;
                z.name = jz.value("name", std::string());
                if (jz.contains("center") && jz["center"].is_array() && jz["center"].size() >= 2) {
                    z.shape.hasCircle = true;
                    z.shape.circleCenterDeg = glm::dvec2(jz["center"][0], jz["center"][1]); // (lat, lon)
                    z.shape.circleRadiusM   = jz.value("radiusM", 1000.0);
                } else if (jz.contains("perimeter") && jz["perimeter"].is_array()) {
                    for (const auto& jv : jz["perimeter"]) {
                        if (!jv.is_array() || jv.size() < 2) continue;
                        z.shape.polygonDeg.emplace_back(jv[0].get<double>(), jv[1].get<double>());
                        z.shape.hasPolygon = true;
                    }
                }
                if (jz.contains("color") && jz["color"].is_array() && jz["color"].size() >= 3)
                    z.color = glm::vec3(jz["color"][0], jz["color"][1], jz["color"][2]);
                if (z.name.empty() || (!z.hasGeometry() && !z.hasColor())) continue;   // basura
                m_namedZones.push_back(z);
            }
            if (!m_namedZones.empty())
                HARUKA_LOGI("Terrain", "'%s': %zu zonas desde la escena",
                            cfg.name.c_str(), m_namedZones.size());
        }

        // CAPAS DE PROPS desde la escena, para la VISTA DE SPAWN del editor (dónde instalaría
        // cada capa). El scatter global (`scatterPropsNear`) las parsea del surfaceConfig por su
        // lado; esta copia solo alimenta el UBO de depuración. Ausente = tabla vacía = vista apagada.
        if (cfg.raw.contains("propLayers") && cfg.raw["propLayers"].is_array()) {
            m_propLayers = Haruka::Planet::PropLayerTable::fromJSON(cfg.raw);
            HARUKA_LOGI("Terrain", "'%s': %zu capas de props desde la escena",
                        cfg.name.c_str(), m_propLayers.layers.size());
        } else {
            m_propLayers.layers.clear();
        }

        // ¿Se necesita el zoneMap en el muestreo de zona para PROPS? Solo si alguna capa consulta
        // el material del terreno (`when: layer`) o existe una zona PINTADA (colores en el zoneMap).
        // El caso habitual (solo `when: zone` + zonas GEOMÉTRICAS) se decide por perímetros y el
        // mapa no aporta: muestrearlo por celda son 2 bilineales 4k + el match de color (~61k
        // celdas) — el grueso del coste del scatter.
        m_propNeedZoneMap = false;
        for (const auto& L : m_propLayers.layers) {
            if (L.when.find("layer") != std::string::npos) { m_propNeedZoneMap = true; break; }
        }
        if (!m_propNeedZoneMap)
            for (const auto& z : m_namedZones)
                if (z.hasColor()) { m_propNeedZoneMap = true; break; }
    }

    // `rawHeight` es función miembro (see planet.h) y es la que el orquestador también usa.
    // Con mapa de elevación, el relieve lo pone el AUTOR y las placas dejan de decidir. Van
    // separados a propósito: mezclarlos haría que el mapa pintado saliera modulado por un campo que
    // el autor no ve, y "he pintado una cordillera y no está donde la puse" es indepurable.
    // MAPA DE ZONAS (si el planeta lo trae). Se carga ANTES del nivel del mar porque lo sustituye:
    // con mapa, el mar lo dibuja el autor y `landFraction` deja de tener sentido.
    if (!cfg.surface.zoneMap.empty()) {
        int zw = 0, zh = 0, zn = 0;
        unsigned char* zp = nullptr;
        const std::string cands[] = {
            cfg.surface.zoneMap,
            Haruka::AssetPaths::projectRoot() + cfg.surface.zoneMap,
            Haruka::AssetPaths::projectTextures() + cfg.surface.zoneMap,
            Haruka::AssetPaths::textures() + cfg.surface.zoneMap,
        };
        for (const auto& c : cands) { zp = stbi_load(c.c_str(), &zw, &zh, &zn, 4); if (zp) break; }
        if (zp) {
            m_zoneCPU.assign(zp, zp + (size_t)zw * zh * 4);
            m_zoneW = zw; m_zoneH = zh;
            stbi_image_free(zp);
            // Filtrado LINEAL + mipmaps (antes Nearest): el shader ya cae el color interpolado a
            // la zona más cercana (harukaSelectMaterial), así que interpolar dos zonas no inventa
            // un material. Con Nearest, los texeles de ~10 km se veían como cuadros duros en la
            // costa. La calidad de terreno decide la resolución que se sube a la GPU (m_zoneCPU
            // conserva el maestro completo para la CPU).
            int zt = terrainQualityTarget();
            int zf = qualityDownscaleFactor(std::max(zw, zh), zt);
            // ⚠️ ES UN MAPA DE ÍNDICES, NO UNA IMAGEN. Cada color IDENTIFICA un material; los valores
            // intermedios no significan nada. Interpolarlo produce colores que NO EXISTEN en la
            // paleta, y esos caen al material "más cercano", que en una frontera verde↔azul es agua.
            // Síntoma: continentes inundados y agua de varios colores.
            //
            // Por eso: PUNTO en el reescalado, NEAREST al muestrear y SIN mipmaps. Las tres cosas
            // hacen falta — con `boxDownsample` el téxel ya nace promediado, con `Linear` se mezcla
            // con sus vecinos, y con mipmaps se mezcla además entre niveles.
            //
            // Con 5 colores muy separados el daño quedaba disimulado; con 12, varios a poca distancia
            // entre sí, la frontera se vuelve ruido. El bug era el mismo antes.
            std::vector<unsigned char> zResized;
            unsigned char* zUpload = m_zoneCPU.data();
            if (zf > 1) {
                // Submuestreo por PUNTO: se toma un téxel, no la media de zf×zf.
                const int nw = zw / zf, nh = zh / zf;
                zResized.resize((size_t)nw * nh * 4);
                for (int y = 0; y < nh; ++y)
                    for (int x = 0; x < nw; ++x) {
                        const size_t src = ((size_t)(y * zf) * zw + (size_t)(x * zf)) * 4;
                        const size_t dst = ((size_t)y * nw + (size_t)x) * 4;
                        for (int c = 0; c < 4; ++c) zResized[dst + c] = m_zoneCPU[src + c];
                    }
                zUpload = zResized.data();
                zw = nw; zh = nh;
            }
            RHI::TextureDesc zd;
            zd.width = (uint32_t)zw; zd.height = (uint32_t)zh;
            zd.format = RHI::Format::RGBA8;
            zd.filter = RHI::Filter::Nearest;
            zd.wrap   = RHI::Wrap::Repeat;
            zd.mipmaps = false;
            zd.initialData = zUpload;
            if (RHI::Device* zdev = RHI::device()) m_zoneTex = zdev->createTexture(zd);
            HARUKA_LOGI("SimplePlanet", "'%s': mapa de zonas %dx%d",
                        cfg.name.c_str(), zw, zh);
        } else {
            HARUKA_LOGW("SimplePlanet", "'%s': mapa de zonas NO encontrado: %s",
                        cfg.name.c_str(), cfg.surface.zoneMap.c_str());
        }
    }

    // MAPA DE ELEVACIÓN. Se muestrea BILINEAL, al revés que el de zonas: aquí sí se quiere
    // interpolar — un valor intermedio entre dos alturas ES una altura válida. Vive como MIEMBRO
    // (m_elevCPU/m_elevW/m_elevH) porque la función de altura base que se guarda para
    // `rebuildWithEdits` tiene que seguir viva toda la vida del planeta.
    m_elevCPU.clear(); m_elevW = 0; m_elevH = 0;
    if (!cfg.surface.elevationMap.empty()) {
        int ew=0, eh=0, en=0;
        const std::string ec[] = {
            cfg.surface.elevationMap,
            Haruka::AssetPaths::projectRoot() + cfg.surface.elevationMap,
            Haruka::AssetPaths::projectTextures() + cfg.surface.elevationMap,
            Haruka::AssetPaths::textures() + cfg.surface.elevationMap,
        };
        // 16 BITS PRIMERO. `stbi_load_16` lee un PNG de 16 bits nativo; si el fichero es de 8, stb lo
        // promociona ×257 él mismo, así que el mapa antiguo da EXACTAMENTE los mismos valores y no hay
        // que distinguir formatos ni inventar un flag en la escena.
        uint16_t* ep16 = nullptr;
        for (const auto& c : ec) { ep16 = stbi_load_16(c.c_str(), &ew, &eh, &en, 4); if (ep16) break; }
        if (ep16) {
            m_elevCPU.assign(ep16, ep16 + (size_t)ew * eh * 4); m_elevW = ew; m_elevH = eh;
            stbi_image_free(ep16);
            HARUKA_LOGI("SimplePlanet", "'%s': mapa de elevación %dx%d, rango %.0f..%.0f m",
                        cfg.name.c_str(), ew, eh,
                        cfg.surface.elevationRange.x, cfg.surface.elevationRange.y);
        } else {
            HARUKA_LOGW("SimplePlanet", "'%s': mapa de elevación NO encontrado: %s",
                        cfg.name.c_str(), cfg.surface.elevationMap.c_str());
        }
    }
    const bool hasElevMap = !m_elevCPU.empty();

    // NIVEL DEL MAR.
    //  · CON mapa de elevación: 0 por construcción — el mapa ya dice a qué cota está cada punto.
    //  · CON mapa de zonas (sin elevación): 0.5 (el mapa decide mar/tierra vía material).
    //  · SIN mapas: cuantil que deja la fracción de tierra pedida.
    const bool hasZoneMap = !m_zoneCPU.empty();
    // `rawHeight` es ahora una función miembro; para `computeSeaLevel` (template que la invoca con
    // operator()) se envuelve en un lambda local, que solo vive durante el build.
    auto rawHeightFn = [this](const glm::dvec3& dir) -> float { return rawHeight(dir); };
    m_seaLevelOffsetM = (hasElevMap || hasZoneMap)
                       ? (hasElevMap ? 0.0f : computeSeaLevel(rawHeightFn, 0.5))
                       : computeSeaLevel(rawHeightFn, cfg.surface.landFraction);
    // La función de altura se guarda en `m_baseHeightFn`: `rebuildWithEdits` la reutiliza para
    // reconstruir la malla sin tocar geología ni mapas, y `rebuild` la recalcula igual al
    // regenerar la geología. `baseHeight` es función miembro (see planet.h), así que captura
    // SOLO `this`: cualquier referencia local moriría al salir de `build`.
    m_baseHeightFn = [this](const glm::dvec3& dir) -> float { return baseHeight(dir); };
    if (hasZoneMap)
        HARUKA_LOGI("SimplePlanet", "'%s': el MAPA decide mar y tierra (landFraction ignorado)",
                    cfg.name.c_str());
    else
        HARUKA_LOGI("SimplePlanet", "'%s': nivel del mar %.0f m para %.0f%% de tierra emergida",
                    cfg.name.c_str(), m_seaLevelOffsetM, cfg.surface.landFraction * 100.0);
    HARUKA_LOGI("SimplePlanet", "build '%s': radius=%.1f plates=%zu faceRes=%d tiling=%.1f",
                cfg.name.c_str(), cfg.radius, m_geology.plates.size(), m_faceSubdiv,
                cfg.surface.tiling);
    ensureShaders();
    HARUKA_LOGI("SimplePlanet", "build '%s': shaders ok", cfg.name.c_str());
    buildMesh(m_baseHeightFn, cfg.radius);
    HARUKA_LOGI("SimplePlanet", "build '%s': mesh ok", cfg.name.c_str());
    // Load biome textures (grass/rock/sand/land) for the 4-layer shader
    HARUKA_LOGI("SimplePlanet", "build '%s': orilla desde la capa %d del array (las 8 texturas sueltas "
                "de bioma se retiraron: ningun shader las muestreaba)",
                cfg.name.c_str(), m_materialTable.shoreLayer());
    // Generate macro-variation texture (always, even with PNGs, for tile-breaking).
    // Horneado DETERMINISTA con cache en disco: la macro y el mapa de biomas son funciones puras de
    // (geología, clima, resolución, biomeConfig). Si el archivo ya existe para esta clave, se carga
    // (píxeles 1:1 con lo que se hornearía); si no, se hornea una vez y se persiste.
    if (!RHI::valid(m_macroTex)) {
        auto macro = loadOrBake(bakeCachePath(m_seed, m_macroRes, m_macroRes / 2, m_biomeConfig, "macro"),
                                [&] { return generateMacroVariation(m_climate, m_geology, m_macroRes, m_macroRes / 2); });
        m_macroTex = uploadBake(macro);
    }
    HARUKA_LOGI("SimplePlanet", "build '%s': macro tex ok", cfg.name.c_str());
    // Generate biome map texture (always — replaces GLSL biomeColor() classification)
    if (!RHI::valid(m_biomeMapTex)) {
        auto biomeImg = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_mapRes / 2, m_biomeConfig, "biome"),
                                   [&] {
                                       return generateBiomeMap(m_climate, m_geology, m_mapRes, m_mapRes / 2,
                                                               0.004f, m_biomeConfig);
                                   });
        m_biomeMapTex = uploadBake(biomeImg);
    }
    HARUKA_LOGI("SimplePlanet", "build '%s': biome map ok", cfg.name.c_str());
    // ALTURA BASE horneada (fase 2a): el campo lento como R32F, aún no cableada a los shaders.
    bakeHeightMap();
    m_tiling = cfg.surface.tiling > 0.0f ? cfg.surface.tiling : 100.0f;
    return true;
    } catch (const std::exception& e) {
        HARUKA_LOGE("SimplePlanet", "build('%s') EXCEPTION: %s", cfg.name.c_str(), e.what());
    } catch (...) {
        HARUKA_LOGE("SimplePlanet", "build('%s') UNKNOWN EXCEPTION", cfg.name.c_str());
    }
    return false;
}

void TerrestrialPlanet::rebuild(const Haruka::Planet::GeologyConfig& geo, int faceRes) {
    if (faceRes > 0) m_faceSubdiv = faceRes;
    m_seed = geo.seed;
    m_geology = Haruka::Planet::generateGeology(geo);

    // El campo cambió con la geología nueva: se recalcula el nivel del mar con la MISMA regla que
    // `build` (mapa de elevación → 0; mapa de zonas sin elevación → cuantil 0.5; sin mapas → la
    // fracción de tierra pedida) y la MISMA función de altura base (mapa de elevación + recorte
    // por zonas). Antes `rebuild` construía una malla SOLO con la geología cruda: un planeta con
    // mapa de elevación o zonas se regeneraba sin su relieve y con todo el mar/tierra borrado.
    const bool hasElevMap = !m_elevCPU.empty();
    const bool hasZoneMap = !m_zoneCPU.empty();
    auto rawHeightFn = [this](const glm::dvec3& dir) -> float { return rawHeight(dir); };
    m_seaLevelOffsetM = (hasElevMap || hasZoneMap)
        ? (hasElevMap ? 0.0f : computeSeaLevel(rawHeightFn, 0.5))
        : computeSeaLevel(rawHeightFn, m_config.surface.landFraction);
    m_baseHeightFn = [this](const glm::dvec3& dir) -> float { return baseHeight(dir); };

    // `buildMesh` hace `clearGPU()`: destruye también las texturas de macro/bioma viejas.
    buildMesh(m_baseHeightFn, m_config.radius);
    // Con geología NUEVA los mapas viejos (horneados sobre el reparto de placas anterior)
    // quedarían desfasados: se regeneran SIEMPRE, no "si faltan". La clave del cache incluye la
    // semilla nueva, así que `loadOrBake` o bien carga el horneado de ESTA semilla (si ya se hizo
    // alguna vez) o bien hornea de nuevo — nunca reutiliza el de otra semilla.
    {
        auto macro = loadOrBake(bakeCachePath(m_seed, m_macroRes, m_macroRes / 2, m_biomeConfig, "macro"),
                                [&] { return generateMacroVariation(m_climate, m_geology, m_macroRes, m_macroRes / 2); });
        m_macroTex = uploadBake(macro);
        auto biomeImg = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_mapRes / 2, m_biomeConfig, "biome"),
                                   [&] {
                                       return generateBiomeMap(m_climate, m_geology, m_mapRes, m_mapRes / 2,
                                                               0.004f, m_biomeConfig);
                                   });
        m_biomeMapTex = uploadBake(biomeImg);
    }
    bakeHeightMap();
}

void TerrestrialPlanet::rebuildWithEdits(const std::function<float(const glm::dvec3&)>& editFn) {
    if (!m_baseHeightFn) return;
    auto combined = [&](const glm::dvec3& dir) -> float {
        return m_baseHeightFn(dir) + editFn(dir);
    };
    clearGPU();
    buildMesh(combined, m_config.radius);
}

// Hornea la ALTURA BASE (geología + mapa de elevación + recorte por zonas) a la misma resolución
// equirect 2:1 que la biome map. Es el MAESTRO del "campo lento" — lo que la fase 2b/2c cableará a
// tess/clipmap/CPU para que los tres lean la MISMA textura. Cache en disco como PNG 16-bit con la
// clave de `heightBakeKey` (incluye mapas y materiales: cambiarlos invalida).
void TerrestrialPlanet::bakeHeightMap() {
    if (m_mapRes <= 0) return;
    const int w = m_mapRes;
    const int h = m_mapRes / 2;
    const std::string key = heightBakeKey(m_seed, w, h, m_elevCPU,
                                          m_config.surface.elevationRange,
                                          m_zoneCPU, m_materialTable, m_seaLevelOffsetM);
    const std::string path = bakeCacheDir() + key + "_height.png";

    BakedHeightField field;
    if (loadBakedHeightPNG16(path, field)) {
        HARUKA_LOGI("BakeCache", "cached  %s", path.c_str());
    } else {
        field.w = w; field.h = h;
        field.data.resize((size_t)w * h);
        // Paralelo por filas: `baseHeight` solo LEE estado const (mapas, materiales, geología), así
        // que es seguro desde N threads sin grafo por hilo (no hay buffers scratch que proteger).
        unsigned hw = std::thread::hardware_concurrency();
        int nThreads = std::clamp<int>((int)hw, 1, 8);
        std::vector<std::thread> threads;
        for (int t = 0; t < nThreads; ++t) {
            const int y0 = t * h / nThreads;
            const int y1 = (t + 1) * h / nThreads;
            threads.emplace_back([this, &field, y0, y1, w, h]() {
                for (int y = y0; y < y1; ++y) {
                    // Misma convención que macro/bioma (inversa de `equirectUV`): texel-center,
                    // lon 0 en la columna central, polo norte arriba. Así `texture(heightTex,
                    // equirectUV(dir))` devuelve exactamente `baseHeight(dir)` (paridad CPU/GPU).
                    double v = ((double)y + 0.5) / h;
                    double lat = (0.5 - v) * glm::pi<double>();
                    double cLat = std::cos(lat), sLat = std::sin(lat);
                    for (int x = 0; x < w; ++x) {
                        double u = ((double)x + 0.5) / w;
                        double lon = (u - 0.5) * glm::two_pi<double>();
                        glm::dvec3 dir(std::cos(lon) * cLat, sLat, std::sin(lon) * cLat);
                        double meters = (double)baseHeight(dir);
                        long val = (long)glm::round(meters) + 32768L;
                        val = std::clamp<long>(val, 0L, 65535L);
                        field.data[(size_t)y * w + x] = (uint16_t)val;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        if (!saveBakedHeightPNG16(path, field))
            HARUKA_LOGW("BakeCache", "no pude persistir %s", path.c_str());
        else
            HARUKA_LOGI("BakeCache", "baked   %s (%dx%d)", path.c_str(), field.w, field.h);
    }

    HeightUpload up = uploadHeight(field);
    m_heightTex  = up.tex;
    m_heightCPU  = std::move(up.cpuField);   // copia CPU del R32F: la física usa el mismo campo
    m_heightW    = up.w;
    m_heightH    = up.h;
    HARUKA_LOGI("SimplePlanet", "'%s': height map ok (R32F=%d)", m_config.name.c_str(),
                RHI::valid(m_heightTex));
}

// ── Render ───────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────────────────────────────────────
// PREPARE: el trabajo de COMPUTE del frame, FUERA de cualquier render pass.
//
// ⚠️ Existe por una regla de Vulkan: `vkCmdDispatch` dentro de una instancia de render pass es
// ILEGAL. El culling de parches vivía dentro de `render()`, o sea entre el `beginRenderPass` de la
// escena y sus draws, porque en OpenGL eso es legal y corriente (`glDispatchCompute` con un FBO
// atado). En Vulkan cerraba el programa.
//
// Y no se podía arreglar en el RHI: reabrir el pase para colar el dispatch volvería a limpiar los
// attachments —todas las render passes del port son `loadOp CLEAR`— y borraría lo ya dibujado. La
// única solución correcta es que el compute ocurra antes de abrir el pase, que es lo que hace esto.
//
// En OpenGL el cambio es inocuo: agrupar el compute antes del pase no altera el resultado.
// ────────────────────────────────────────────────────────────────────────────────────────────────
void TerrestrialPlanet::prepare(const glm::dvec3& cameraPos) {
    m_cullReady = false;
    if (!RHI::valid(m_vertexBuffer)) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;

    // Mismas condiciones que antes. OPT-IN a propósito: un fallo aquí se ve como AGUJEROS en el
    // planeta, y `HARUKA_GPU_CULL=0` lo apaga sin recompilar. El TCS mantiene su propio test, así
    // que este camino solo puede quitar trabajo, nunca dibujar algo distinto.
    const bool gpuCull = settingWithEnvOverride(
        SettingsManager::get().graphics().gpuPatchCull, "HARUKA_GPU_CULL");
    if (!gpuCull || !RHI::valid(s_cullPipeline) || m_patchCount <= 0 ||
        !RHI::valid(m_patchBoundsSSBO) || !RHI::valid(m_patchIdxSSBO) ||
        !RHI::valid(m_patchIdxCulled) || !RHI::valid(m_patchCullCmd))
        return;

    // El comando se resetea por CPU cada frame: el compute solo ACUMULA con atomicAdd, así que si no
    // se pusiera indexCount a 0 crecería sin fin entre frames.
    const uint32_t cmdReset[5] = { 0u, 1u, 0u, 0u, 0u };
    dev->updateBuffer(m_patchCullCmd, 0, sizeof(cmdReset), &cmdReset);

    struct CullParams { glm::vec4 camRel, camDir; } cp{};
    const glm::dvec3 rel = cameraPos - m_config.position;
    const double relLen = glm::length(rel);
    cp.camRel = glm::vec4(glm::vec3(rel), (float)m_config.radius);
    cp.camDir = glm::vec4(glm::vec3(relLen > 1e-9 ? rel / relLen : glm::dvec3(0, 1, 0)),
                          (float)m_patchCount);
    if (!RHI::valid(m_patchCullUBO))
        m_patchCullUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(cp), &cp,
                                           RHI::BufferMemory::Dynamic);
    else
        dev->updateBuffer(m_patchCullUBO, 0, sizeof(cp), &cp);

    ctx->bindPipeline(s_cullPipeline);
    ctx->bindStorageBuffer(0, m_patchBoundsSSBO);
    ctx->bindStorageBuffer(1, m_patchIdxSSBO);
    ctx->bindStorageBuffer(2, m_patchIdxCulled);
    ctx->bindStorageBuffer(3, m_patchCullCmd);
    ctx->bindUniformBuffer(4, m_patchCullUBO);
    ctx->dispatch((m_patchCount + 63u) / 64u, 1, 1);
    // El draw leerá lo que el compute acaba de escribir: índice, comando y storage.
    ctx->memoryBarrier();

    m_cullReady = true;
}

void TerrestrialPlanet::render(const glm::dvec3& cameraPos,
                               const glm::mat4& proj,
                               const glm::mat4& view) {
    if (!RHI::valid(s_pipeline)) return;
    if (!RHI::valid(m_vertexBuffer)) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    RHI::Context* ctx = dev->beginFrame();

    // Debug: log planet render attempt once
    static std::unordered_set<std::string> s_logged;
    if (s_logged.find(m_config.name) == s_logged.end()) {
        s_logged.insert(m_config.name);
        HARUKA_LOGD("SimplePlanet", "rendering '%s': radius=%.0f hasPipe=%d hasVB=%d hasIB=%d idx=%u",
                m_config.name.c_str(), m_config.radius,
                RHI::valid(s_pipeline),
                RHI::valid(m_vertexBuffer),
                RHI::valid(m_indexBuffer),
                m_indexCount);
    }

    struct SimplePlanetUBO {
        glm::mat4 uMVP;
        glm::vec4 uCenter;
        glm::vec4 uLightDir;
        glm::vec4 uLightColor;
        glm::vec4 uAmbient;
        glm::vec4 uExtra;
        glm::vec4 uDebug;   // x = vista de depuración (ver TerrestrialPlanet::debugView)
        glm::vec4 uTexAnchor;
    };
    // Camera-relative rendering: the shader receives the planet center relative
    // to the camera (center - cam) and a rotation-only VP. This keeps the GPU
    // arithmetic at ~planet-radius scale (~6371 km) instead of astronomical
    // coordinates (~150e6 km), eliminating float-precision artifacts.
    const glm::dvec3 centerRelD = m_config.position - cameraPos;
    const glm::mat4  rotOnlyVP  = proj * glm::mat4(glm::mat3(view));
    SimplePlanetUBO ubo{};
    ubo.uMVP     = rotOnlyVP;
    ubo.uCenter  = glm::vec4(glm::vec3(centerRelD), 0.0f);
    ubo.uLightDir   = glm::vec4(m_sunDir, 0.0f);
    ubo.uLightColor = glm::vec4(m_sunColor, 1.0f);
    // Ambiente día/noche desde la elevación solar (el mismo que usa el cielo): de día luz
    // ambiental clara, de noche casi nada. Antes era fijo y diminuto → el lado noche del
    // terreno era una lámina negra que parecía "planeta sin dibujar".
    // uAmbient.w = SOMBRAS DE TERRENO por ray-march en biome.frag (ajuste TerrainShadows).
    //
    // El mapa de sombras del motor no contiene el terreno —solo lo que dibuja el hook del juego—, así
    // que una loma no proyecta sombra. El fragmento sí tiene la función de altura, así que puede
    // marchar hacia el sol y preguntarlo. Es coste de FRAGMENTO, y a pie el suelo es casi toda la
    // pantalla: es ajuste (TerrainShadows), y HARUKA_TERRAIN_SHADOW=0/1 lo fuerza para poder medir
    // el coste real en una escena concreta sin tocar el .ini.
    const bool terrainShadow = settingWithEnvOverride(
        SettingsManager::get().graphics().terrainShadows, "HARUKA_TERRAIN_SHADOW");
    ubo.uAmbient    = glm::vec4(m_ambientStrength * glm::vec3(0.55f, 0.65f, 0.85f),
                                terrainShadow ? 1.0f : 0.0f);
    // El camino de bioma depende del MAPA DE BIOMAS, no de que exista un PNG concreto: qué
    // texturas hay lo decide la tabla de materiales del proyecto, y una tabla legítima puede
    // no tener ninguna (terreno de color de bioma, liso).
    bool hasBiome = RHI::valid(m_biomeMapTex);
    ubo.uExtra = glm::vec4(hasBiome ? 1.0f : 0.0f, m_tiling,
                           RHI::valid(m_zoneTex) ? 1.0f : 0.0f,
                           (float)m_config.radius);   // w = radio, lo usa el tess eval
    // uDebug.y = segundos desde el arranque: lo usan los shaders animados (oleaje del agua).
    // Un reloj ESTÁTICO compartido entre planetas: todas las aguas del mundo en la misma fase.
    static const auto s_waterClockStart = std::chrono::high_resolution_clock::now();
    const float elapsedSec = std::chrono::duration<float>(
        std::chrono::high_resolution_clock::now() - s_waterClockStart).count();
    // ¿Dibuja el clipmap este frame? (misma condición que el bloque de draw de abajo). La malla
    // base se recorta donde el clipmap la cubre (terrain.tese), y el recorte SOLO debe actuar cuando
    // el clipmap va a pintar de verdad — si el pipeline no compiló, no hay bake o la cámara está en
    // órbita, recortar la base dejaría un agujero en el planeta.
    const glm::dvec3 camDir = cameraPos - m_config.position;
    const double camDist = glm::length(camDir);
    const double camAlt = camDist - m_config.radius;
    // ¿La cámara está DENTRO del terreno? (modo fly/ghost atravesando el suelo). La rejilla del
    // clipmap vive en el plano tangente de la cámara: si la cámara se hunde, la rejilla la persigue
    // como una lámina "pegada al personaje" mientras la malla base queda anclada al planeta → las
    // "dos capas de terreno". Soterrada, el clipmap no aporta nada (el suelo que la rodea ya no se
    // ve desde arriba) y solo crea el artefacto: se apaga junto con el recorte de la malla base.
    // Margen de 3 m: a la altura de los ojos (≈1,7 m) el clipmap sigue activo, y solo se apaga
    // cuando la cámara queda claramente bajo la superficie — sin eso, cruzar un bache con la cabeza
    // justa encima parpadearía entre rejilla fina y malla gorda.
    bool camUnderground = camAlt + 3.0 < sampleHeight(camDir);
    if (!camUnderground) {
        // El test del sub-punto falla al entrar en una LADERA en diagonal: la cámara está dentro
        // de la montaña pero su proyección radial cae en el valle de abajo (por debajo de su
        // altitud), y `camUnderground` queda falso — la rejilla sigue pegada al personaje. Se
        // sondean 4 puntos alrededor (a ±2, ±8 y ±25 m en el plano tangente): si el terreno a
        // cualquier lado queda >3 m por encima de la cámara, está enterrada y se apaga igual.
        const glm::dvec3 up = camDir / camDist;
        glm::dvec3 tu = glm::cross(glm::dvec3(0, 1, 0), up);
        if (glm::length(tu) < 1e-6) tu = glm::cross(glm::dvec3(1, 0, 0), up);
        tu = glm::normalize(tu);
        const glm::dvec3 tv = glm::cross(up, tu);
        const double R = m_config.radius;
        for (const double dM : { 2.0, 8.0, 25.0 }) {
            const double arc = dM / R;
            const double c = std::cos(arc), s = std::sin(arc);
            const glm::dvec3 probes[4] = {
                glm::normalize(up * c + tu * s), glm::normalize(up * c + tv * s),
                glm::normalize(up * c - tu * s), glm::normalize(up * c - tv * s),
            };
            for (const glm::dvec3& p : probes) {
                if (camAlt + 3.0 < sampleHeight(p)) { camUnderground = true; break; }
            }
            if (camUnderground) break;
        }
    }
    // ⚠️ ALTURA SOBRE EL SUELO, no sobre la esfera. (Este número ya no enciende ni apaga nada —el
    // clipmap vive a cualquier altura, ver abajo—, pero sigue decidiendo CUÁNTOS ANILLOS hay, así que
    // la trampa de medirla mal sigue viva y por eso se conserva la nota.) `camAlt` se mide desde el radio de referencia, así
    // que sobre terreno a 216 m valía 218 m y la condición `< 20 m` **nunca** se cumplía: el clipmap
    // solo se encendía donde el terreno está a menos de 20 m del nivel del mar, o sea casi en ningún
    // sitio. Tierra adentro el sistema de terreno fino estaba APAGADO y la malla base dibujaba con
    // quads de ~650 m bajo los pies: el suelo salía como una lámina plana decenas de metros por
    // debajo (banda lejana en el horizonte con hueco delante) y la física, que muestrea la función
    // exacta, se separaba metros de lo dibujado. Era la disparidad grande.
    //
    // `camUnderground` (arriba) ya lo hacía bien —compara `camAlt` contra `sampleHeight`—, lo que
    // delata que el error está solo aquí: dos usos del mismo número con referencias distintas.
    const double camAltGround = camAlt - sampleHeight(camDir);

    // ── COBERTURA DEL CLIPMAP ESCALADA CON LA ALTITUD ───────────────────────────────────────────
    //
    // El clipmap se apagaba por encima de 20 m y el suelo pasaba a pintarlo la malla base, cuyo quad
    // crece con la distancia: a 936 m medía **306 m por polígono**. Bastaba saltar o subir una loma
    // para que el terreno se volviera facetones. Y apagarlo era la única salida porque la rejilla
    // tiene tamaño fijo (±1984 m): desde el aire eso no es horizonte, es una alfombra bajo los pies.
    //
    // La rejilla no cambia — mismos NC×NC parches, mismo coste de teselación — solo se ESTIRA. Cubrir
    // más sale gratis; lo que se paga es resolución, que es exactamente el intercambio correcto
    // cuando te alejas.
    //
    // ⚠️ EL FACTOR ES POTENCIA DE DOS, y no es estética. Toda la paridad con la colisión cuelga de
    // que los vértices caigan en múltiplos del quad (§ `clipmap.tesc`): con un factor arbitrario
    // (1,7) los nodos caerían en 6,8 m y la retícula gruesa dejaría de ser un SUBCONJUNTO de la
    // fina. Con 2^k el quad pasa a 8, 16, 32 m… y la propiedad se conserva intacta.
    //
    // A ras de suelo (k=0) todo queda EXACTAMENTE como estaba: mismo quad de 4 m, misma cobertura.
    //
    // ── ANILLOS ANIDADOS (no una sola rejilla estirada) ─────────────────────────────────────────
    //
    // El estiramiento global resolvía la COBERTURA y estropeaba la RESOLUCIÓN CERCANA: `clipScale`
    // multiplica la rejilla ENTERA, así que a 640 m de altura el suelo bajo los pies también pasaba a
    // quads de 128 m. La resolución la decidía TU ALTURA, no la distancia a lo que mirabas — y eso es
    // la "capa nueva muy cerca" que se ve al subir.
    //
    // Ahora se dibujan VARIAS rejillas a la vez, cada una con el doble de escala que la anterior y con
    // el CENTRO HUECO justo donde vive la de dentro (de ahí "anillos"): son marcos encajados.
    //
    //     nivel 0: quad 4 m   · ±1,9 km  (lleno, salvo el hueco del anillo de colisión)
    //     nivel 1: quad 8 m   · ±4,0 km  (hueco central de ±1,9 km)
    //     nivel k: quad 4·2^k · ±1,9·2^k km
    //
    // `clipK` deja de ser un factor de estirado y pasa a ser el ÍNDICE DEL ANILLO EXTERIOR, con la
    // misma fórmula de antes: la cobertura total no cambia (±63 km con k=5), pero cerca se conserva
    // el quad de 4 m a cualquier altura.
    //
    // ⚠️ POR QUÉ NO HAY GRIETAS ENTRE ANILLOS, que es la duda obvia:
    //
    //  · En ALTURA: `clipmap.tese` evalúa el detalle con `clipM = max(rad*0.002, 4)`, que depende solo
    //    del RADIO en el plano, no del nivel. Dos anillos vecinos evalúan la MISMA función en el mismo
    //    punto → la misma altura. No hay escalón que coser entre ellos.
    //  · En TESELACIÓN: `edgeFactor` da nivel ∝ arista/distancia, o sea quad ≈ distancia·0,004
    //    INDEPENDIENTE del tamaño de parche. En la frontera el anillo de fuera tiene aristas 2× más
    //    largas y pide nivel 2× mayor, y como el nivel se redondea a potencia de dos, el quad sale
    //    IDÉNTICO por los dos lados. Sin T-junctions.
    //  · El hueco se descarta por PARCHE ENTERO (`clipmap.tesc`), así que el recorte es conservador:
    //    el anillo de fuera empieza un poco ANTES de donde acaba el de dentro (64·2^k m de solape).
    //    Se solapan dos superficies idénticas —misma altura, mismo quad— lo cual es feo pero inocuo;
    //    un hueco, en cambio, sería un agujero por el que se ve el espacio. Se elige el solape.
    // ── CUÁNTOS ANILLOS: LOS QUE TAPEN EL HORIZONTE ─────────────────────────────────────────────
    //
    // El número de anillos ya no sale de una heurística sobre la altura (`ceil(log2(alt/20))`), sino
    // de la distancia a la que de verdad se deja de ver suelo: el HORIZONTE, `sqrt(2·R·h)`.
    //
    // El objetivo es que la MALLA BASE no se vea NUNCA desde el suelo. Esa malla es la rejilla
    // antigua del planeta —cientos de metros por polígono— y es la "capa gorda" que aparece al subir:
    // el clipmap se acababa antes que la vista y a partir de ahí dibujaba ella. Si el anillo exterior
    // llega más lejos que el horizonte, la base queda recortada entera (`terrain.tese` la recorta con
    // `uClipCover.x`) y no hay distancia a la que se pueda ver.
    //
    //   a la altura de los ojos (1,7 m): horizonte 4,7 km  -> 3 anillos (±7,9 km)
    //   a 355 m:                         horizonte  67 km  -> 7 anillos (±127 km)
    //   a 2,5 km (donde el clipmap se apaga): horizonte 178 km -> 8 anillos (±254 km)
    //
    // ⚠️ ESTO CUESTA. A ras de suelo pasa de 1 anillo a 3: ~2 433 parches contra 961. La resolución
    // no cambia (cada anillo mantiene su quad), lo que se paga es cubrir con geometría fina lo que
    // antes tapaba una malla basta. Es el intercambio que se pidió, pero hay que MIRAR el profiler:
    // `planet.clipmap.draw`. Si no cabe, la salida no es volver a la malla basta sino cullear los
    // parches fuera de cámara en el TCS (hoy el clipmap no lo hace; la malla base sí).
    const double horizonM = std::sqrt(std::max(0.0, 2.0 * m_config.radius * std::max(camAltGround, 1.7)));
    int ringCount = Haruka::Planet::terrainClipRingIndex(horizonM, (double)m_clipCoverM) + 1;
    ringCount = std::max(1, std::min(ringCount, kMaxClipRings));
    const int clipK = ringCount - 1;                // índice del anillo exterior

    // ⚠️ A/B SIN RECOMPILAR: `HARUKA_CLIP_RINGS=N` topa el número de anillos.
    //
    //   =1  → UN solo anillo, estirado 2^k: EXACTAMENTE el comportamiento anterior a los anillos
    //         anidados. Es la única forma de saber si un defecto que se ve al subir lo trajo este
    //         cambio o ya estaba — y el síntoma "aparece una capa al subir" se reportó ANTES.
    //   =0 / sin definir → sin tope (por defecto).
    //
    // El anillo EXTERIOR conserva el estirado `2^k`, así que con N=1 la cobertura sigue siendo la de
    // siempre (±63 km) y no se abre un agujero entre el clipmap y la malla base.
    static const int s_ringCap = [] {
        const char* e = std::getenv("HARUKA_CLIP_RINGS");
        return e ? std::atoi(e) : 0;
    }();
    if (s_ringCap > 0 && ringCount > s_ringCap) ringCount = s_ringCap;

    const float clipScale = (float)std::exp2((double)clipK);   // escala del anillo EXTERIOR

    // ── SONDA `HARUKA_CLIP_PROBE=1` ─────────────────────────────────────────────────────────────
    //
    // Imprime DÓNDE está cada frontera. Sin esto, "aparece una capa al subir" no se puede separar de
    // "está oscuro" ni de un borde de costa: el número dice a qué distancia está el borde del
    // clipmap, y basta compararlo con lo que se ve. Solo cuando CAMBIA algo (número de anillos o
    // cobertura), que es justo el instante en que una capa aparece o desaparece.
    if (std::getenv("HARUKA_CLIP_PROBE")) {
        static int   s_lastRings = -1;
        static float s_lastCover = -1.0f;
        const float coverOuter = m_clipCoverM * clipScale;
        if (ringCount != s_lastRings || std::abs(coverOuter - s_lastCover) > 0.5f) {
            s_lastRings = ringCount;
            s_lastCover = coverOuter;
            std::string desglose;
            for (int r = 0; r < ringCount; ++r) {
                const bool   outer = (r == ringCount - 1);
                const double sc    = outer ? (double)clipScale : std::exp2((double)r);
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "      anillo %d: quad %.0f m · borde a %.0f m · hueco %.0f m\n",
                              r, 4.0 * sc, m_clipCoverM * sc,
                              r == 0 ? 0.0 : m_clipCoverM * std::exp2((double)(r - 1)));
                desglose += buf;
            }
            HARUKA_LOGI("ClipRings",
                        "alt=%.1f m sobre el suelo -> k=%d · %d anillo(s) · la malla base se recorta a %.0f m\n%s",
                        camAltGround, clipK, ringCount, (double)coverOuter, desglose.c_str());
        }
    }

    // ── EL CLIPMAP NO SE APAGA POR ALTURA ───────────────────────────────────────────────────────
    //
    // ⚠️ AQUÍ HABÍA UN UMBRAL (`20·2^5·4` ≈ 2,5 km, con histéresis 2,9 km), heredado del esquema de
    // la rejilla ESTIRADA. Al cruzarlo, el clipmap se apagaba ENTERO — y con él el descarte de la
    // malla base, que cuelga de `uDebug.w`—, así que la rejilla gruesa reaparecía DE GOLPE encima del
    // terreno fino. Eso era la "capa a +3 km": no una capa nueva, sino la malla base recuperando el
    // control en un escalón.
    //
    // Mover el umbral solo habría movido el escalón, que es exactamente el error que ya se cometió
    // dos veces en este terreno (el corte del relieve fino se ALEJÓ de 2 km a 20 km y la línea siguió
    // ahí). Un interruptor duro sobre lo que se dibuja SIEMPRE se ve. Así que no hay umbral: el
    // clipmap vive a cualquier altura.
    //
    // No hace falta apagarlo para que sea barato. `edgeFactor` reparte por TAMAÑO ANGULAR: desde
    // órbita los parches piden nivel 1 y la rejilla entera colapsa a su malla mínima. Y la cobertura
    // ya no es fija — el número de anillos sale del horizonte —, así que desde arriba el clipmap
    // ocupa lo que debe ocupar y la malla base dibuja el resto del planeta, cosidos por el anillo de
    // mezcla del exterior.
    //
    // Lo único que sigue apagándolo es estar BAJO TIERRA (`camUnderground`), que no es una cuestión
    // de escala sino de que la rejilla no describe nada útil desde dentro del terreno.
    const bool clipActive = RHI::valid(s_clipPipeline) && RHI::valid(m_clipVB) &&
                            RHI::valid(m_baseFieldTex) && hasBiome && !camUnderground;
    m_clipMapActive = clipActive;
    // ── QUÉ GEOMETRÍA HAY BAJO LOS PIES ─────────────────────────────────────────────────────────
    //
    // Es el número que falta para cerrar "piso a una altura distinta de la que veo". La sonda de
    // paridad compara la función con la malla de COLISIÓN y ambas usan quads de 4 m; pero lo que se
    // DIBUJA solo tiene 4 m si el clipmap está activo. Si no lo está, el suelo lo pinta la malla base
    // teselada, cuyo quad a los pies es de cientos de metros — y entonces se camina sobre una
    // superficie fina mirando un plano grueso, que es exactamente el síntoma.
    //
    // Se registra cada 2 s y solo si el estado o la altura cambian de forma apreciable, para no
    // inundar el log.
    {
        static double s_clipLogT = -1e9;
        static bool   s_lastActive = !clipActive;
        const double nowSec = (double)elapsedSec;
        if (s_lastActive != clipActive || nowSec - s_clipLogT > 2.0) {
            s_clipLogT = nowSec;
            s_lastActive = clipActive;
            HARUKA_LOGDIAG("TerrainDraw",
                "clipmap=%s · altura sobre el suelo=%.2f m · enterrado=%d · quad dibujado a los pies=%.1f m",
                clipActive ? "SI" : "NO", camAltGround, (int)camUnderground,
                clipActive ? Haruka::Planet::TERRAIN_CLIP_QUAD_M
                           : (double)Haruka::Planet::TERRAIN_CLIP_PATCH_M * 306.0 / 128.0);
        }
    }
    // VISTA DE DEPURACIÓN POR ENTORNO. `m_debugView` lo pone el EDITOR, que no existe en la build del
    // juego: sin esto las vistas de `biome.frag` (7 = costa, 8 = por qué está negro) son inalcanzables
    // justo donde hay que mirar, que es el juego corriendo. `HARUKA_PLANET_DEBUG=7` las abre sin
    // recompilar. Se lee UNA vez (getenv por frame es una búsqueda lineal en el entorno).
    static const int s_envDebugView = []() {
        const char* v = std::getenv("HARUKA_PLANET_DEBUG");
        return v ? std::atoi(v) : 0;
    }();
    const int debugView = s_envDebugView ? s_envDebugView : m_debugView;
    ubo.uDebug = glm::vec4((float)debugView, elapsedSec,
                           RHI::valid(m_heightTex) ? 1.0f : 0.0f,  // z = hay bake de altura (16)
                           clipActive ? 1.0f : 0.0f);              // w = el clipmap dibuja (recorte base)

    // ── ANCLA DE LAS UV DE TERRENO: por qué existe ──────────────────────────────────────────────
    //
    // `biome.frag` necesita la posición PLANETARIA del fragmento para que el patrón de textura esté
    // pegado al planeta y no se deslice con el jugador. La calculaba como `vFragPos - uCenter`, y ahí
    // está el problema: `uCenter` mide ~6,37e6, así que la resta se hace entre dos floats de esa
    // magnitud y el resultado queda CUANTIZADO a un ulp ≈ 0,76 m — POR PÍXEL.
    //
    // Eso no es un desplazamiento pequeño, es un desastre de filtrado: la GPU elige el nivel de mip
    // (y la anisotropía) con las DERIVADAS de la coordenada de textura entre píxeles vecinos, y esas
    // derivadas pasan a ser 0 o 0,76 m en vez del valor real (micras a milímetros). Cada píxel
    // aterriza en un mip distinto y arbitrario → sal y pimienta en TODO el suelo, a cualquier
    // distancia. Es el "ruido del suelo que no tiene sentido", y por eso no se arreglaba con más
    // resolución de PNG ni con más geometría: la textura estaba bien, el MUESTREO no.
    //
    // La solución es no reconstruir nunca la coordenada planetaria en el fragmento. `vFragPos` es
    // relativo a la CÁMARA (metros a kilómetros: precisión de milímetros) y sirve tal cual; lo único
    // que falta es el desplazamiento al marco del planeta, y ese se puede REDUCIR MÓDULO EL TILE en
    // la CPU con doubles. La textura es periódica con periodo `m_tiling`, así que sumar el resto en
    // vez del valor entero da EXACTAMENTE la misma imagen, sin saltos al cruzar un múltiplo — y las
    // coordenadas se quedan pequeñas, así que las derivadas vuelven a ser las de verdad.
    {
        const glm::dvec3 camRel = cameraPos - m_config.position;   // en DOUBLE: aquí está el valor exacto
        const double T = (m_tiling > 0.01f) ? (double)m_tiling : 1.0;
        // `w` = ALTITUD DE LA CÁMARA sobre la esfera de referencia. Va aquí porque en el shader es
        // incalculable: `length(cámara − centro) − radio` es 6,37e6 − 6,371e6, cancelación catastrófica
        // que deja la cota con ~0,8 m de basura. En double la resta es exacta, y con ella el fragmento
        // reconstruye la cota de cualquier punto sin restar nunca dos números grandes (ver `frameAlt`
        // en biome.frag). Es el número que hace que la sombra de terreno y el gradiente del detalle
        // dejen de tener ruido por píxel.
        const double camAlt = glm::length(camRel) - m_config.radius;
        ubo.uTexAnchor = glm::vec4((float)std::fmod(camRel.x, T),
                                   (float)std::fmod(camRel.y, T),
                                   (float)std::fmod(camRel.z, T),
                                   (float)camAlt);
    }
    dev->updateBuffer(s_ubo, 0, sizeof(ubo), &ubo);
    ctx->bindUniformBuffer(0, s_ubo);

    // TABLA DE MATERIALES → UBO binding 12. Se sube una vez por planeta (perezosa) y solo se
    // reescribe si la tabla cambió: es dato de autoría, no estado por frame.
    {
        // `f` es el vec4 NUEVO: la banda de altura. Se añade en vez de robar componentes libres a
        // los otros porque `d.w` ya lleva los flags y meter dos rangos más ahí sería empaquetado
        // ilegible — y este UBO se sube una vez por planeta, no por frame: 16 bytes más da igual.
        struct GpuMat { glm::vec4 a, b, c, d, e, f; };
        struct GpuTable {
            glm::vec4 count;
            GpuMat    mats[Haruka::Planet::TerrainMaterialTable::kMaxMaterials];
        } table{};
        const auto& src = m_materialTable.materials;
        const int n = std::min((int)src.size(),
                               Haruka::Planet::TerrainMaterialTable::kMaxMaterials);
        // `y` = ¿existe el array de terreno? Sin él, `harukaSelectMaterial` fuerza `tile = -1` y el
        // shader usa su valor neutro en vez de muestrear un sampler2DArray sin enlazar (indefinido).
        // z = capa de la ORILLA (la arena que se mezcla en la línea de agua). Viene de la tabla, que es
        // quien asigna las capas: así el shader no tiene que adivinar un índice que depende del orden de
        // los materiales de la escena.
        table.count = glm::vec4((float)n, RHI::valid(m_terrainAlbedoArray) ? 1.0f : 0.0f,
                                (float)m_materialTable.shoreLayer(), 0);
        for (int i = 0; i < n; ++i) {
            const auto& m = src[i];
            table.mats[i].a = glm::vec4(m.humMin, m.humMax, m.tempMin, m.tempMax);
            table.mats[i].b = glm::vec4(m.slopeMin, m.slopeMax, (float)m.layer, m.priority);
            table.mats[i].c = glm::vec4(m.tint, m.grain);
            // zonePacked: RGB888 en un float. Exacto (mantisa de 24 bits) y evita crecer el
            // UBO por tres floats. flags bit0 = tiene zona.
            const float zonePacked = m.hasZone()
                ? (std::round(m.zoneColor.r) * 65536.0f +
                   std::round(m.zoneColor.g) * 256.0f   +
                   std::round(m.zoneColor.b))
                : 0.0f;
            // flags: bit0 = tiene zona · bit1 = MATERIAL DE AGUA (`submerged`).
            //
            // ⚠️ El bit1 existe porque EL MAR SE HA RETIRADO. `main.scene` declara un material `water`
            // con color (0.09, 0.22, 0.45) que, al no ser agua sino un ALBEDO, pintaba el lecho de
            // azul oscuro: con la esfera del océano delante no se veía, y al quitarla queda a la vista
            // como una "malla negra" con el borde cuantizado del mapa. Marcándolo, el shader lo salta
            // en la selección y el lecho se sombrea con el material de tierra que le toque.
            //
            // Cuando el agua se rehaga, ESTE es el bit que la enciende: la capa `water` pasa a ser la
            // fuente de verdad de dónde hay agua, en vez de una segunda superficie que mantener de
            // acuerdo con el terreno. Por eso se marca ahora en vez de borrar el material.
            // flags: bit0 = zona · bit1 = agua · bit2 = LECHO (el resto es cobertura).
            const float matFlags = (m.hasZone() ? 1.0f : 0.0f) + (m.submerged ? 2.0f : 0.0f)
                                 + (m.role == Haruka::Planet::TerrainMaterial::Role::Bedrock ? 4.0f : 0.0f);
            table.mats[i].d = glm::vec4(m.detail, m.feather, zonePacked, matFlags);
            table.mats[i].e = m.hasColor()
                ? glm::vec4(m.baseColor, glm::clamp(m.colorWeight, 0.0f, 1.0f))
                : glm::vec4(0.0f);
            // Banda de altura (km) + su feather propio: el de clima/pendiente está en [0,1] y aquí
            // las unidades son kilómetros, así que compartirlo daría un degradado de 80 metros —
            // un corte duro para una transición que debe cubrir cientos de metros de ladera.
            table.mats[i].f = glm::vec4(m.elevMinKm, m.elevMaxKm, m.elevFeatherKm, 0.0f);
        }
        if (!RHI::valid(m_materialUBO)) {
            m_materialUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(table),
                                              &table, RHI::BufferMemory::Dynamic);
        } else {
            dev->updateBuffer(m_materialUBO, 0, sizeof(table), &table);
        }
        ctx->bindUniformBuffer(12, m_materialUBO);
    }

    // TABLA DE CAPAS DE PROPS → UBO binding 14 (VISTA DE SPAWN del editor). Como la de materiales:
    // se sube una vez por planeta y se reescribe solo si cambió; sin capas, `uPropCount.x = 0` y la
    // vista de props queda apagada. Se suben SOLO las capas que instalan (mesh no vacía), para que
    // el índice del shader (dbg-20) coincida con el del selector del editor, que también las filtra.
    // El densityMap de la capa SELECCIONADA se sube aparte (17).
    {
        struct GpuProp { glm::vec4 a, b, c, d; };
        struct GpuPropTable {
            glm::vec4 count;
            GpuProp    layers[Haruka::Planet::PropLayerTable::kMaxLayers];
        } ptable{};
        const auto& src = m_propLayers.layers;
        int n = 0;
        for (const auto& L : src) {
            if (L.mesh.empty()) continue;
            if (n >= Haruka::Planet::PropLayerTable::kMaxLayers) break;
            GpuProp& g = ptable.layers[n];
            g.a = glm::vec4(L.humMin, L.humMax, L.tempMin, L.tempMax);
            g.b = glm::vec4(L.slopeMin, L.slopeMax, 0.0f, L.feather);
            g.c = glm::vec4(L.densityMap.empty() ? 0.0f : 1.0f, 0, 0, 0);
            g.d = glm::vec4(L.density, 0, 0, 0);
            ++n;
        }
        ptable.count = glm::vec4((float)n, 0, 0, 0);
        if (!RHI::valid(m_propUBO)) {
            m_propUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(ptable),
                                          &ptable, RHI::BufferMemory::Dynamic);
        } else {
            dev->updateBuffer(m_propUBO, 0, sizeof(ptable), &ptable);
        }
        ctx->bindUniformBuffer(14, m_propUBO);

        // densityMap de la capa que el editor está viendo (dbg = 40+i): carga/subida perezosa por
        // ruta, cacheada. Sin mapa (o sin capa activa) se bindea una textura 1×1 BLANCA — el shader
        // multiplica por 1.0 y la vista muestra solo las bandas de clima/forma.
        const int dbgLayer = m_debugView - 40;
        RHI::TextureHandle densityTex = m_propWhiteTex;
        if (m_debugView >= 40 && dbgLayer >= 0 && dbgLayer < n) {
            // El densityMap vive en la capa ORIGINAL de la tabla (misma posición, ya que el filtro
            // por mesh no altera el orden relativo de las capas que instalan).
            int instCount = 0;
            for (const auto& L : src) {
                if (L.mesh.empty()) continue;
                if (instCount == dbgLayer) {
                    if (!L.densityMap.empty()) {
                        const auto& path = L.densityMap;
                        auto it = m_propDensityTex.find(path);
                        if (it != m_propDensityTex.end()) {
                            densityTex = it->second;
                        } else {
                            densityTex = uploadPropDensityMap(path);
                            m_propDensityTex[path] = densityTex;
                        }
                    }
                    break;
                }
                ++instCount;
            }
        }
        ctx->bindTexture(17, densityTex);
    }

    // Bind macro-variation texture (slot 10) if available — both paths
    if (RHI::valid(m_macroTex))
        ctx->bindTexture(10, m_macroTex);
    // Bind biome map texture (slot 11) — replaces GLSL biomeColor() classification
    if (RHI::valid(m_biomeMapTex))
        ctx->bindTexture(11, m_biomeMapTex);

    // Draw terrain — biome-blended pipeline preferred
    // Bindings must match biome.frag declarations:
    //   1 = sandAlbedo (la ÚNICA de bioma que sobrevive: la playa de la orilla).
    //   2..8 estaban ocupados por grass/land/rock y sus normales; ningún shader los muestreaba desde
    //   que el material elige su textura por CAPA del array, así que se retiraron — 21 s de arranque y
    //   ~156 MB de VRAM que se pagaban por nada.
    // Arrays de terreno: unidades 12 y 13. El UBO de materiales también es 12, pero en GL los
    // espacios de binding de UBO y de textura son distintos y no chocan (en Vulkan irían en
    // sets/bindings separados igualmente).
    if (RHI::valid(m_terrainAlbedoArray)) ctx->bindTexture(12, m_terrainAlbedoArray);
    if (RHI::valid(m_terrainNormalArray)) ctx->bindTexture(13, m_terrainNormalArray);
    // El estado INCOHERENTE que hay que delatar: la tabla declara materiales con textura pero el array
    // no existe. Antes esto era silencioso y el shader leía un sampler sin enlazar.
    if (!RHI::valid(m_terrainAlbedoArray) && !m_materialTable.albedoPaths().empty()) {
        static std::unordered_set<std::string> s_warned;
        if (s_warned.insert(m_config.name).second)
            HARUKA_LOGW("Terrain", "'%s': %zu materiales declaran textura pero el ARRAY no existe -> "
                        "el suelo va sin material (bindings 12/13 vacios)",
                        m_config.name.c_str(), m_materialTable.albedoPaths().size());
    }
    if (RHI::valid(m_zoneTex))              ctx->bindTexture(14, m_zoneTex);
    // El bake de altura (R32F, binding 16): lo leen terrain.tese, clipmap.tese y water.frag con la
    // bilineal manual compartida. Se enlaza SIEMPRE que exista, antes de cualquier draw, y aguanta
    // hasta el agua (ninguna bind intermedia toca la unidad 16).
    if (RHI::valid(m_heightTex))            ctx->bindTexture(16, m_heightTex);

    // TESELADO si está disponible: la malla base es la misma, cambia el índice (parches) y el
    // pipeline. Si el pipeline no compiló se cae a la malla sin teselar.
    // ── INTERRUPTOR PARA AISLAR LAS BANDAS ──────────────────────────────────────────────────────
    //
    // Todo el lado de DATOS está verificado limpio sobre la captura de RenderDoc (frame 919 de
    // testC2): posiciones de la malla base dentro de ±0,1 % de R, índice compactado con los 852
    // parches de dispersión exacta 258, comando indirecto 3408 = 852·4, instancias de props cuadrando
    // con su subida, hacha con matriz de rotación pura. Si aun así aparecen franjas estiradas, las
    // genera un shader de TESELACIÓN a partir de entradas sanas — y solo hay dos candidatos.
    //
    // `HARUKA_NO_TESS=1` dibuja la malla base SIN teselar (camino de respaldo que ya existía). Si con
    // eso desaparecen, el culpable es `terrain.tesc/tese`; si siguen, es el clipmap.
    static const bool s_noTess = [] {
        const char* e = std::getenv("HARUKA_NO_TESS");
        return e && e[0] == '1';
    }();
    const bool useTess = !s_noTess &&
                         RHI::valid(s_tessPipeline) && RHI::valid(m_patchIB) && hasBiome;

    // CLIPMAP: solo tiene sentido si la cámara está lo bastante cerca de la superficie. Con la
    // cámara en órbita la rejilla sería un punto, y el coste de decidirlo es despreciable. Misma
    // condición que `clipActive` del UBO (recorte de la malla base en terrain.tese).
    const bool useClip = clipActive;

    // ClipParams (binding 13) con el SEMI-LADO real del clipmap y su anillo de mezcla: lo usan
    // BOTH terrain.tese (recorte de la base) y clipmap.tese (blend ring). Se sube y se bindea ANTES
    // del draw de la base para que el recorte lea el mismo valor que dibuja el clipmap.
    if (clipActive) {
        // El marco lo da `terrainClipFrame` (terrain_lod.h), que es también el que usa la malla de
        // COLISIÓN: una sola definición para el suelo que se dibuja y el que se pisa.
        //
        // ⚠️ ANCLADO a la retícula del mundo (el radio activa el snap). Sin él la rejilla se desliza
        // con la cámara, la superficie dibujada se re-muestrea cada frame —el terreno "nada" bajo los
        // pies hasta la cuerda del quad— y la colisión no puede casar con un objetivo que se mueve.
        // ⚠️ SE ANCLA DONDE SE ANCLA EL ANILLO DE COLISIÓN, no en la cámara, cuando hay anillo.
        //
        // Los dos cuantizan en (lat, lon) con el mismo paso, pero desde puntos distintos: el clipmap
        // desde la CÁMARA y la física desde el JUGADOR. Cuando caen en celdas distintas —medido en
        // el juego: **hasta 4,00 m**, un escalón entero de anclaje— las dos retículas dejan de
        // compartir vértices, y eso rompe DOS cosas a la vez:
        //
        //   · el hueco de 192 m queda desplazado respecto al cuadrado que lo rellena → franja doble
        //     de 4 m por un lado y agujero de 4 m por el otro;
        //   · en la frontera cada lado muestrea el terreno en puntos DISTINTOS, así que el escalón no
        //     es la disparidad fina (2-4 cm de twist) sino `pendiente · 4 m` — decímetros en terreno
        //     quebrado. Es la costura abrupta que se ve.
        //
        // Anclar el clipmap en el jugador es además más correcto de por sí: la geometría fina debe
        // centrarse en quien camina, no en dónde mira. Sin anillo se conserva el comportamiento de
        // siempre (`cameraPos`), que es lo que necesitan el servidor y los tests.
        const glm::dvec3 clipFrom = m_nearRingAnchored ? m_nearRingAnchor : cameraPos;
        glm::dvec3 up, tu, tv;
        Haruka::Planet::terrainClipFrame(clipFrom, m_config.position, up, tu, tv,
                                         m_config.radius);

        struct ClipParams { glm::vec4 origin, tanU, tanV, cover; };

        // ── EL ANILLO DE COLISIÓN, que es el HUECO DEL NIVEL 0 ──────────────────────────────────
        //
        // Dentro del hueco el suelo lo dibuja el anillo cercano, con los mismos vértices que colisiona.
        // Solo cuando hay anillo que lo rellene: sin él, un hueco es un agujero por el que se ve el
        // espacio.
        //
        // ⚠️ SIGUE ATADO A ESTAR A RAS DE SUELO, y no por la razón de antes. Antes era obligatorio:
        // con la rejilla estirada los parches medían `128·2^k` y el borde de 192 m ya no caía en un
        // borde de parche. Ahora el NIVEL 0 nunca se estira, así que el hueco sería legal a cualquier
        // altura — se mantiene la condición porque 192 m de detalle fino bajo un jugador que está a
        // cientos de metros no aporta nada, no porque la geometría lo impida.
        const bool nearRingOK = (m_nearRingIndices >= 3 && RHI::valid(m_nearRingVB)
                                 && camAltGround <= 20.0);
        const float hole0 = nearRingOK ? (float)Haruka::Planet::TERRAIN_CLIP_HOLE_M : 0.0f;
        m_nearRingVisible = (hole0 > 0.0f);

        m_clipRingCount = ringCount;
        if ((int)m_clipUBOs.size() < ringCount) m_clipUBOs.resize((size_t)ringCount, RHI::BufferHandle{});

        for (int r = 0; r < ringCount; ++r) {
            // El anillo EXTERIOR lleva `clipScale` (2^k), no `2^r`. Sin tope los dos coinciden
            // —`ringCount-1 == clipK`— así que esto no cambia nada en el camino normal; es lo que
            // hace que `HARUKA_CLIP_RINGS=1` reproduzca de verdad la rejilla estirada de antes en vez
            // de dejar una rejilla sin estirar que cubriría 32× menos.
            const float scale = (r == ringCount - 1) ? clipScale : (float)std::exp2((double)r);
            const float half  = m_clipCoverM * scale;
            ClipParams cp{};
            cp.origin = glm::vec4(glm::vec3(up), (float)Haruka::Planet::TERRAIN_CLIP_PATCH_M);
            cp.tanU   = glm::vec4(glm::vec3(tu), scale);   // w = escala de ESTE anillo (2^r)
            cp.tanV   = glm::vec4(glm::vec3(tv), 0.0f);

            // ANILLO DE MEZCLA: SOLO EN EL EXTERIOR. Ese blend cose el clipmap con la malla base
            // (pasa de las octavas finas a las que usa la malla), y eso solo debe ocurrir donde de
            // verdad acaba el clipmap. Aplicarlo en un anillo interior desvanecería su relieve fino
            // justo donde empieza el siguiente —que sí lo tiene— y dejaría un escalón circular.
            // Fuera del exterior se empuja la banda más allá de la cobertura: el tese toma entonces
            // siempre la rama `rad <= blendStart`, o sea detalle fino puro.
            const bool outer = (r == ringCount - 1);
            const float blendS = outer ? half * 0.70f : 1e9f;
            const float blendE = outer ? half * 0.95f : 1e9f;

            // HUECO: el nivel 0 deja el del anillo de colisión; los demás dejan la cobertura ENTERA
            // del anillo de dentro. El descarte es por parche entero (`clipmap.tesc`), así que esto
            // recorta de menos —solape de 64·2^(r-1) m— y nunca de más. Ver la nota de arriba.
            cp.cover = glm::vec4(half, blendS, blendE,
                                 r == 0 ? hole0 : m_clipCoverM * (float)std::exp2((double)(r - 1)));

            if (!RHI::valid(m_clipUBOs[(size_t)r]))
                m_clipUBOs[(size_t)r] = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(cp), &cp,
                                                          RHI::BufferMemory::Dynamic);
            else
                dev->updateBuffer(m_clipUBOs[(size_t)r], 0, sizeof(cp), &cp);
        }
        // La malla base y el per-pixel de `biome.frag` se recortan contra el clipmap ENTERO, así que
        // les toca el ClipParams del anillo EXTERIOR: es el que dice hasta dónde llega la rejilla. Con
        // el de un anillo interior, la base volvería a dibujarse bajo los anillos de fuera.
        ctx->bindUniformBuffer(13, m_clipUBOs[(size_t)(ringCount - 1)]);
    }

    // ── SUELO MOJADO / NEVADO (UBO 23 + máscara cenital en la unidad 17) ────────────────────────
    //
    // UN SOLO buffer: su contenido es el mismo para la malla base, los anillos del clipmap y el
    // anillo cercano, así que no puede repetir el fallo del recurso reescrito entre draws. Se ata
    // SIEMPRE, aunque no llueva: `biome.frag` lo declara y leer un bloque sin atar es indefinido.
    {
        struct WetParams { glm::mat4 skySpace; glm::vec4 wet; } wp{};
        wp.skySpace = m_skySpace;
        wp.wet = glm::vec4(m_groundWet, RHI::valid(m_skyMaskTex) ? 1.0f : 0.0f, m_groundSnow, 0.0f);
        if (!RHI::valid(m_wetUBO))
            m_wetUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(wp), &wp, RHI::BufferMemory::Dynamic);
        else
            dev->updateBuffer(m_wetUBO, 0, sizeof(wp), &wp);
        ctx->bindUniformBuffer(23, m_wetUBO);
        if (RHI::valid(m_skyMaskTex)) ctx->bindTexture(17, m_skyMaskTex);
    }

    { HARUKA_PROFILE("planet.base.draw");
    if (useTess) {
        ctx->bindPipeline(s_tessPipeline);
        ctx->bindVertexBuffer(m_vertexBuffer);

        // ── CULLING DE PARCHES EN GPU (ajuste GpuPatchCull; HARUKA_GPU_CULL=0/1 lo fuerza) ──────
        //
        // Sin esto se envían los 393 216 parches y el TCS mata casi todos: a altura de ojo el
        // horizonte está a 4,65 km y un parche mide 39,1 km, así que se ve parte de UNO. Con el
        // compute, el TCS solo recibe los que pueden salir en pantalla.
        //
        // ⚠️ Es OPT-IN a propósito. Un fallo aquí se ve como AGUJEROS en el planeta, y con una
        // variable de entorno se apaga sin recompilar ni revertir. El TCS mantiene su propio test,
        // así que este camino solo puede quitar trabajo, nunca dibujar algo distinto.
        // ⚠️ EL DISPATCH YA NO ESTÁ AQUÍ: lo hace `prepare()`, ANTES de abrir el render pass.
        // `vkCmdDispatch` dentro de una instancia de render pass es ILEGAL en Vulkan (en OpenGL es
        // legal, y por eso vivía aquí). Cerraba el programa, y el síntoma no se parecía a la causa:
        // en una captura de RenderDoc los draws se veían bien uno a uno —el replay los ejecuta
        // aislados— mientras en ejecución el dispositivo se perdía a mitad de frame y la pantalla
        // quedaba negra. `m_cullReady` transporta aquí si el culling llegó a ejecutarse.
        if (m_cullReady) {
            ctx->bindPipeline(s_tessPipeline);
            ctx->bindVertexBuffer(m_vertexBuffer);
            ctx->bindIndexBuffer(m_patchIdxCulled);
            ctx->drawIndexedIndirect(m_patchCullCmd, 1, 0, 0);
        } else {
            ctx->bindIndexBuffer(m_patchIB);
            ctx->drawIndexed(m_patchIndexCount);
        }
    } else if (hasBiome) {
        ctx->bindPipeline(s_biomePipeline);
    } else {
        ctx->bindPipeline(s_pipeline);
    }
    if (!useTess) {
        ctx->bindVertexBuffer(m_vertexBuffer);
        ctx->bindIndexBuffer(m_indexBuffer);
        ctx->drawIndexed(m_indexCount);
    }
    }  // fin planet.base.draw

    // El clipmap va DESPUÉS de la malla del planeta, no antes. Las dos describen el MISMO
    // suelo —misma altura base, misma función de detalle—, así que son coplanares a propósito y
    // el orden no las separa: lo que decide es el sesgo de profundidad del pipeline.
    { HARUKA_PROFILE("planet.clipmap.draw");
    if (useClip) {
        // Los ClipParams de todos los anillos ya se rellenaron ANTES del draw de la base (que se
        // recorta contra el exterior); aquí se re-bindea el de cada anillo antes de su draw.
        ctx->bindPipeline(s_clipPipeline);
        ctx->bindUniformBuffer(0, s_ubo);
        ctx->bindUniformBuffer(23, m_wetUBO);
        if (RHI::valid(m_skyMaskTex)) ctx->bindTexture(17, m_skyMaskTex);
        ctx->bindTexture(15, m_baseFieldTex);
        ctx->bindVertexBuffer(m_clipVB);
        ctx->bindIndexBuffer(m_clipIB);
        // UN DRAW POR ANILLO, con la MISMA malla (misma rejilla NC×NC, mismos índices): lo único que
        // cambia entre ellos es el ClipParams —escala y hueco—, que es lo que los convierte en marcos
        // encajados. Por eso los anillos no cuestan memoria: comparten el buffer de vértices.
        // De DENTRO hacia FUERA, para que el z-buffer resuelva el solape de 64·2^k m a favor del
        // anillo fino, que es el que llega primero.
        for (int r = 0; r < m_clipRingCount; ++r) {
            if (!RHI::valid(m_clipUBOs[(size_t)r])) continue;
            ctx->bindUniformBuffer(13, m_clipUBOs[(size_t)r]);
            ctx->drawIndexed(m_clipIndexCount);
        }
    }
    }  // fin planet.clipmap.draw

    // ── SUELO CERCANO DESDE LA COLISIÓN ─────────────────────────────────────────────────────────
    //
    // Se dibuja DESPUÉS del clipmap y con más sesgo de profundidad: dentro de ±256 m los dos
    // describen el mismo suelo y tiene que ganar éste, que es el que de verdad se pisa. Cuando el
    // paso 5 abra el hueco en el clipmap dejarán de solaparse y el sesgo sobrará.
    { HARUKA_PROFILE("planet.nearring.draw");
    if (m_nearRingVisible && m_nearRingIndices >= 3 && RHI::valid(s_nearRingPipeline)
        && RHI::valid(m_nearRingVB) && RHI::valid(m_nearRingIB)) {
        ctx->bindPipeline(s_nearRingPipeline);
        ctx->bindUniformBuffer(0, s_ubo);
        // ClipParams del NIVEL 0: es el anillo cuyo hueco rellena este suelo. Con el del exterior
        // vería una cobertura 2^k veces mayor y un hueco que no es el suyo.
        ctx->bindUniformBuffer(13, m_clipUBOs[0]);
        ctx->bindUniformBuffer(22, m_nearRingUBO);
        ctx->bindUniformBuffer(23, m_wetUBO);
        if (RHI::valid(m_skyMaskTex)) ctx->bindTexture(17, m_skyMaskTex);
        ctx->bindTexture(15, m_baseFieldTex);
        ctx->bindVertexBuffer(m_nearRingVB);
        ctx->bindIndexBuffer(m_nearRingIB);
        ctx->drawIndexed(m_nearRingIndices);
    }
    }  // fin planet.nearring.draw

    // ── EL MAR ──────────────────────────────────────────────────────────────────────────────────
    //
    // Va DESPUÉS de todo el terreno: es transparente y se mezcla sobre el fondo marino ya dibujado,
    // que es lo que hace que el agua somera deje ver la arena y la profunda no. No escribe
    // profundidad (`depth.write = false`), así que dos capas de agua no pueden pelearse por el
    // z-buffer — y no las hay: los anillos se descartan por el hueco y la esfera lejana solo asoma
    // donde el clipmap no llega.
    //
    // ⚠️ El ORDEN importa: primero los anillos (de dentro afuera) y después la esfera. El agua
    // cercana lleva las olas y la lejana es lisa; si la esfera fuera primero, sus fragmentos
    // pasarían el depth test bajo las crestas y se verían a través de ellas.
    { HARUKA_PROFILE("planet.ocean.draw");
    static const bool s_noWater = std::getenv("HARUKA_NOWATER") != nullptr;
    static bool s_noWaterLogged = false;
    if (!s_noWaterLogged) {
        s_noWaterLogged = true;
        HARUKA_LOGI("SimplePlanet", "MAR: %s (cercano = anillos del clipmap con oleaje Gerstner · "
                    "lejano = esfera lisa). HARUKA_NOWATER lo apaga.",
                    s_noWater ? "APAGADO por HARUKA_NOWATER" : "activo");
    }
    if (!s_noWater && RHI::valid(m_heightTex)) {
        // ── LAS OLAS SOLO EXISTEN CERCA ─────────────────────────────────────────────────────────
        //
        // ⚠️ SIN ESTA COTA EL MAR CUESTA 180 ms EN ÓRBITA. `ringCount` sale del HORIZONTE, así que a
        // 1 000 km de altura son ~11 anillos × 3 844 parches = **42 000 parches de agua teselada**,
        // y el TCS los procesa todos aunque queden tras el planeta o fuera de cámara (esta rejilla
        // no cullea limbo ni frustum, solo el hueco del anillo interior).
        //
        // Y no aportan NADA: la ola más larga del tren mide 61 m, que a 30 km ya está por debajo del
        // píxel. Todo eso es geometría para un detalle que nadie puede ver. Por encima de la cota el
        // mar lo describe la esfera lisa, que es una superficie idéntica —la ola se desvanece a 0 en
        // el borde del clipmap (`ocean.tese`)— así que el relevo no tiene costura.
        //
        // 8 km: a esa altura una ola de 61 m mide ~0,7 píxeles a 1080p. Por debajo se ve; por
        // encima, no.
        const double camAltWave = glm::length(cameraPos - m_config.position) - m_config.radius;
        const bool   wavesVisible = camAltWave < 25000.0;   // a 25 km una ola de 61 m es ~0,2 px

        // MAR CERCANO: la MISMA rejilla y los MISMOS ClipParams que acaba de usar el terreno. Los
        // anillos ya están rellenados arriba; aquí solo se cambia el pipeline y se repasan.
        if (wavesVisible && useClip && RHI::valid(s_oceanPipeline) && RHI::valid(m_clipVB)) {
            ctx->bindPipeline(s_oceanPipeline);
            ctx->bindUniformBuffer(0, s_ubo);
            ctx->bindTexture(16, m_heightTex);
            // AGUA INTERIOR: el campo de ríos/lagos. Se ata SIEMPRE —aunque no haya sim— porque el
            // shader declara el bloque y leer uno sin atar es indefinido; con `misc.w = 0` devuelve
            // su centinela y el mar se queda con su nivel 0.
            if (m_inlandWaterValid) {
                ctx->bindUniformBuffer(24, m_inlandWaterUBO);
                ctx->bindStorageBuffer(25, m_inlandWaterSSBO);
            }
            ctx->bindVertexBuffer(m_clipVB);
            ctx->bindIndexBuffer(m_clipIB);
            // Y ni siquiera todos los anillos que haya: solo los que caen dentro del alcance en el
            // que la ola se ve. El anillo r cubre `clipCover·2^r`, así que con cobertura de 1 984 m
            // son 2 anillos hasta ~4 km — el resto ya lo dibuja la esfera lisa, que a esa distancia
            // es la misma superficie. Sin este tope, a ras de suelo se teselaban los 11 anillos.
            // 16 km: con el fragmento del agua ya barato (ver `ocean.frag`), lo que limita
            // el alcance es la teselación, y ésta ya se desvanece sola con `rad` en el TCS.
            const double kWaveReachM = 16000.0;
            for (int r = 0; r < m_clipRingCount; ++r) {
                if ((double)m_clipCoverM * std::exp2((double)r) > kWaveReachM && r > 0) break;
                if (!RHI::valid(m_clipUBOs[(size_t)r])) continue;
                ctx->bindUniformBuffer(13, m_clipUBOs[(size_t)r]);
                ctx->drawIndexed(m_clipIndexCount);
            }
        }
        // MAR LEJANO: la esfera, solo por las caras cúbicas orientadas hacia la cámara. Las otras
        // cinco están tras el planeta y su vertex shading sería trabajo tirado.
        if (RHI::valid(s_oceanFarPipeline) && RHI::valid(m_oceanFarVB)) {
            ctx->bindPipeline(s_oceanFarPipeline);
            ctx->bindUniformBuffer(0, s_ubo);
            ctx->bindTexture(16, m_heightTex);
            if (m_inlandWaterValid) {
                ctx->bindUniformBuffer(24, m_inlandWaterUBO);
                ctx->bindStorageBuffer(25, m_inlandWaterSSBO);
            }
            ctx->bindVertexBuffer(m_oceanFarVB);
            ctx->bindIndexBuffer(m_oceanFarIB);
            const glm::dvec3 camRelD = cameraPos - m_config.position;
            const glm::dvec3 camDirD = camRelD / glm::length(camRelD);
            static const glm::dvec3 kFaceAxis[6] = {
                { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
            };
            for (int face = 0; face < 6; ++face) {
                if (glm::dot(camDirD, kFaceAxis[face]) < 0.0) continue;
                ctx->drawIndexed(m_oceanFarFaceStride, (uint32_t)face * m_oceanFarFaceStride);
            }
        }
    }
    }  // fin planet.ocean.draw

    m_lastRenderStats = RenderStats{};   // lo rellena este frame (lo que realmente dibuja)

    // Geometría DEL ÚLTIMO FRAME para el panel: la malla base (teselada o lisa), el clipmap solo
    // cuando pinta,. Triángulos = índices/3.
    m_lastRenderStats.baseVertices  = m_vertexCount;
    m_lastRenderStats.baseTriangles = (useTess ? m_patchIndexCount : m_indexCount) / 3;
    // × anillos: el clipmap ya no es un draw sino `m_clipRingCount`. Sin multiplicar, el panel
    // seguiría enseñando el coste de UN anillo y el cambio saldría gratis en pantalla.
    // (Es una cota ALTA: los anillos exteriores descartan su centro en el TCS, ~736 de 961 parches.)
    m_lastRenderStats.clipVertices  = useClip ? m_clipVertexCount * (uint32_t)m_clipRingCount : 0;
    m_lastRenderStats.clipTriangles = useClip ? (m_clipIndexCount / 3) * (uint32_t)m_clipRingCount : 0;
    m_lastRenderStats.drawCalls = 1 + (useClip ? m_clipRingCount : 0);
}

}} // namespace Haruka::Planet
