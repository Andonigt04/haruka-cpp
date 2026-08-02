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
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_clipPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_waterPipeline;
Haruka::RHI::PipelineHandle TerrestrialPlanet::s_wirePipeline;
Haruka::RHI::BufferHandle TerrestrialPlanet::s_ubo;
bool TerrestrialPlanet::s_shadersReady = false;

TerrestrialPlanet::TerrestrialPlanet() = default;
TerrestrialPlanet::~TerrestrialPlanet() { clearGPU(); }

void TerrestrialPlanet::setOrbit(int parent, double a, double ecc, double period, double phase,
                                 const glm::dvec3& u, const glm::dvec3& v) {
    m_config.orbitParent  = parent;
    m_config.orbitA       = a;
    m_config.orbitEcc     = ecc;
    m_config.orbitPeriod  = period;
    m_config.orbitPhase   = phase;
    m_config.orbitU       = u;
    m_config.orbitV       = v;
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
    const std::string pClipEval        = shaderDir + "clipmap.tese";
    const std::string pClipVert        = shaderDir + "clipmap.vert";
    const std::string pSimpleFrag      = shaderDir + "simple.frag";
    const std::string pSimpleVert      = shaderDir + "simple.vert";
    const std::string pTessCtrl        = shaderDir + "terrain.tesc";
    const std::string pTessEval        = shaderDir + "terrain.tese";
    const std::string pTessVert        = shaderDir + "terrain.vert";
    const std::string pTexFrag         = shaderDir + "textured.frag";
    const std::string pWatFrag         = shaderDir + "water.frag";
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
        HARUKA_LOGI("SimplePlanet", "pipeline clipmap: %s",
                    RHI::valid(s_clipPipeline) ? "ok" : "FALLO");
        HARUKA_LOGI("SimplePlanet", "pipeline teselado: %s",
                    RHI::valid(s_tessPipeline) ? "ok" : "FALLO (se usa la malla sin teselar)");
    }

    // Water pipeline — same vertex format, blue shader, alpha blend
    RHI::PipelineDesc wpd;
    wpd.vertexPath   = pSimpleVert.c_str();
    wpd.fragmentPath = pWatFrag.c_str();
    wpd.vertexLayout   = withClimate(pd.vertexLayout);
    wpd.depth.test     = true;  wpd.depth.write = false;
    wpd.depth.compare  = RHI::CompareOp::Greater;
    wpd.cull = RHI::CullMode::Back;
    wpd.blend.enable = true;
    wpd.blend.mode   = RHI::BlendMode::Alpha;
    s_waterPipeline = dev->createPipeline(wpd);

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
    if (RHI::valid(s_waterPipeline)){ dev->destroy(s_waterPipeline); s_waterPipeline = {}; }
    if (RHI::valid(s_wirePipeline)) { dev->destroy(s_wirePipeline);  s_wirePipeline = {}; }
    if (RHI::valid(s_ubo))          { dev->destroy(s_ubo);           s_ubo = {}; }
    s_shadersReady = false;
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
    m_patchIB      = dev->createBuffer(RHI::BufferUsage::Index, patchIdx.size() * sizeof(uint32_t), patchIdx.data(), RHI::BufferMemory::Static);
    m_patchIndexCount = (uint32_t)patchIdx.size();

    // REJILLA DEL CLIPMAP: 31x31 parches de 128 m = ~4 km a la redonda con 961 parches. En metros
    // LOCALES y centrada en 0: la cámara va en el centro y la rejilla no se regenera nunca, solo se
    // reorienta con el marco tangente que se le pasa por UBO.
    {
        const int   NC = 31;
        const float PATCH = 128.0f;
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
        std::vector<float> field((size_t)6 * vertsPerFace * 3);
        for (size_t v = 0; v < verts.size(); ++v) {
            field[v * 3 + 0] = verts[v].elev * 1000.0f;
            field[v * 3 + 1] = verts[v].temp;
            field[v * 3 + 2] = verts[v].humid;
        }
        RHI::TextureDesc fd;
        fd.width = fd.height = (uint32_t)(res + 1);
        fd.layers = 6;
        fd.format = RHI::Format::RGB32F;
        fd.filter = RHI::Filter::Nearest;   // se interpola A MANO, ver el shader del clipmap
        fd.wrap   = RHI::Wrap::ClampToEdge;
        fd.initialData = field.data();
        m_baseFieldTex = dev->createTexture(fd);
    }

    // Water mesh — EN el nivel del mar. El campo ya viene desplazado para que el nivel del mar sea
    // 0, así que "aquí hay mar" y "aquí se dibuja mar" son la misma cota.
    const double oceanRadius = radius;
    std::vector<SimplePlanetVertex> wVerts;
    std::vector<uint32_t> wIndices;
    wVerts.reserve(totalVerts);
    wIndices.reserve(totalIndices);

    for (int face = 0; face < 6; ++face) {
        const PlanetFace pf = (PlanetFace)face;
        uint32_t base = (uint32_t)(face * vertsPerFace);
        for (int j = 0; j <= res; ++j)
            for (int i = 0; i <= res; ++i) {
                const double lx = (double)i / res * 2.0 - 1.0;
                const double ly = (double)j / res * 2.0 - 1.0;
                const glm::dvec3 dir = cubeFaceToDir(pf, lx, ly);
                const glm::dvec3 pos = dir * oceanRadius;
                SimplePlanetVertex v{};
                v.px = (float)pos.x; v.py = (float)pos.y; v.pz = (float)pos.z;
                v.nx = (float)dir.x; v.ny = (float)dir.y; v.nz = (float)dir.z;
                v.u = (float)i / res; v.v = (float)j / res;
                v.cr = 0.1f; v.cg = 0.25f; v.cb = 0.55f;
                wVerts.push_back(v);
            }

        for (int j = 0; j < res; ++j)
            for (int i = 0; i < res; ++i) {
                const uint32_t a = base + (uint32_t)(j * (res + 1) + i);
                const uint32_t b = base + (uint32_t)(j * (res + 1) + i + 1);
                const uint32_t c = base + (uint32_t)((j + 1) * (res + 1) + i);
                const uint32_t d = base + (uint32_t)((j + 1) * (res + 1) + i + 1);
                wIndices.push_back(a); wIndices.push_back(b); wIndices.push_back(c);
                wIndices.push_back(b); wIndices.push_back(d); wIndices.push_back(c);
            }
    }

    m_waterVB = dev->createBuffer(RHI::BufferUsage::Vertex, wVerts.size() * sizeof(SimplePlanetVertex), wVerts.data(), RHI::BufferMemory::Static);
    m_waterIB = dev->createBuffer(RHI::BufferUsage::Index, wIndices.size() * sizeof(uint32_t), wIndices.data(), RHI::BufferMemory::Static);
    m_waterIndexCount = (uint32_t)wIndices.size();
}

void TerrestrialPlanet::clearGPU() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(m_vertexBuffer)) { dev->destroy(m_vertexBuffer); m_vertexBuffer = {}; }
    if (RHI::valid(m_indexBuffer))  { dev->destroy(m_indexBuffer);  m_indexBuffer  = {}; }
    if (RHI::valid(m_patchIB))      { dev->destroy(m_patchIB);      m_patchIB      = {}; }
    if (RHI::valid(m_baseFieldTex)) { dev->destroy(m_baseFieldTex); m_baseFieldTex = {}; }
    if (RHI::valid(m_clipVB))       { dev->destroy(m_clipVB);       m_clipVB       = {}; }
    if (RHI::valid(m_clipIB))       { dev->destroy(m_clipIB);       m_clipIB       = {}; }
    if (RHI::valid(m_clipUBO))      { dev->destroy(m_clipUBO);      m_clipUBO      = {}; }
    if (RHI::valid(m_waterVB))      { dev->destroy(m_waterVB);      m_waterVB      = {}; }
    if (RHI::valid(m_waterIB))      { dev->destroy(m_waterIB);      m_waterIB      = {}; }
    if (RHI::valid(m_albedoTex))    { dev->destroy(m_albedoTex);    m_albedoTex    = {}; }
    if (RHI::valid(m_normalTex))    { dev->destroy(m_normalTex);    m_normalTex    = {}; }
    auto destroyTex = [&](RHI::TextureHandle& t) { if (RHI::valid(t)) { dev->destroy(t); t = {}; } };
    destroyTex(m_biomeTex.sandAlbedo);  destroyTex(m_biomeTex.sandNormal);
    destroyTex(m_biomeTex.grassAlbedo); destroyTex(m_biomeTex.grassNormal);
    destroyTex(m_biomeTex.landAlbedo);  destroyTex(m_biomeTex.landNormal);
    destroyTex(m_biomeTex.rockAlbedo);  destroyTex(m_biomeTex.rockNormal);
    destroyTex(m_macroTex);
    destroyTex(m_biomeMapTex);
    m_indexCount = 0;
    m_waterIndexCount = 0;
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
    auto takeBest = [&](const std::string& root) {
        for (int i = 0; i < 5; ++i) {
            std::string full = root + "terrain/" + kResLabels[i] + "/" + path;
            int cw = 0, ch = 0, cn = 0;
            unsigned char* d = stbi_load(full.c_str(), &cw, &ch, &cn, 4);
            if (!d) continue;
            const int area = cw * ch;
            if (area > bestArea) {
                if (data) stbi_image_free(data);
                data = d; w = cw; h = ch; bestArea = area;
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
    auto takeBest = [&](const std::string& root, const std::string& path,
                        int& w, int& h, unsigned char*& data, int& bestArea) {
        for (int i = 0; i < 5; ++i) {
            std::string full = root + "terrain/" + kResLabels[i] + "/" + path;
            int cw = 0, ch = 0, cn = 0;
            unsigned char* d = stbi_load(full.c_str(), &cw, &ch, &cn, 4);
            if (!d) continue;
            const int area = cw * ch;
            if (area > bestArea) {
                if (data) stbi_image_free(data);
                data = d; w = cw; h = ch; bestArea = area;
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
        takeBest(Haruka::AssetPaths::textures(), path, w, h, data, bestArea);
        takeBest(Haruka::AssetPaths::projectTextures(), path, w, h, data, bestArea);
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
    HARUKA_LOGI("Terrain", "array de terreno: %d capas de %dx%d", outLayers, W, H);
    return dev->createTexture(td);
}

void TerrestrialPlanet::rebuildTerrainArrays() {
    m_materialTable.assignLayers();   // la capa es la POSICIÓN, no un número declarado a mano

    RHI::Device* dev = RHI::device();
    if (dev) {
        if (RHI::valid(m_biomeTex.albedoArray)) { dev->destroy(m_biomeTex.albedoArray); m_biomeTex.albedoArray = {}; }
        if (RHI::valid(m_biomeTex.normalArray)) { dev->destroy(m_biomeTex.normalArray); m_biomeTex.normalArray = {}; }
    }
    m_biomeTex.arrayLayers = 0;

    const auto albedos = m_materialTable.albedoPaths();
    if (albedos.empty()) return;    // tabla sin texturas: terreno de color de bioma, sin grano

    int layers = 0;
    m_biomeTex.albedoArray = loadTextureArray(albedos, layers);
    m_biomeTex.arrayLayers = layers;
    int nl = 0;
    m_biomeTex.normalArray = loadTextureArray(m_materialTable.normalPaths(), nl);
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

// Índice del material cuyo `zoneColor` está más cerca del color leído. -1 si no hay ninguno con
// zona declarada. Sin umbral de distancia a propósito: un color del PNG que no case exactamente
// con ninguna entrada (compresión, antialias del pincel) debe caer en el material MÁS PARECIDO,
// no quedarse sin material y salir como un agujero.
static int zoneToMaterial(const Haruka::Planet::TerrainMaterialTable& table, const glm::vec3& c) {
    int best = -1; float bestD = 0.0f;
    for (size_t i = 0; i < table.materials.size(); ++i) {
        const auto& m = table.materials[i];
        if (!m.hasZone()) continue;
        const glm::vec3 d = m.zoneColor - c;
        const float dist = glm::dot(d, d);
        if (best < 0 || dist < bestD) { best = (int)i; bestD = dist; }
    }
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
        return m_elevCPU[((size_t)y * m_elevW + x) * 4] / 255.0f;
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
    if (m_baseHeights.empty() || m_baseRes <= 0) return 0.0;
    const glm::dvec3 dir = glm::normalize(dirIn);

    // Misma descomposición cara+UV que la malla: `cubeFaceToDir` es la inversa de esto, y las dos
    // vienen del mismo `cube_sphere.h` compartido y GL-free.
    PlanetFace f; double lx, ly;
    dirToCubeFace(dir, f, lx, ly);

    // La malla coloca el vértice (i,j) en lx = i/res*2-1, así que el índice es (lx+1)/2*res.
    const int    res = m_baseRes;
    const double fu  = (lx + 1.0) * 0.5 * res;
    const double fv  = (ly + 1.0) * 0.5 * res;
    int i0 = (int)std::floor(fu), j0 = (int)std::floor(fv);
    i0 = glm::clamp(i0, 0, res - 1);
    j0 = glm::clamp(j0, 0, res - 1);
    const float tu = (float)(fu - i0), tv = (float)(fv - j0);

    const size_t base = (size_t)(int)f * (size_t)(res + 1) * (res + 1);
    auto at = [&](int i, int j) -> float {
        i = glm::clamp(i, 0, res); j = glm::clamp(j, 0, res);
        return m_baseHeights[base + (size_t)j * (res + 1) + i];
    };
    // Bilineal en el MISMO orden que el shader: primero en u, luego en v.
    const float h00 = at(i0, j0),     h10 = at(i0 + 1, j0);
    const float h01 = at(i0, j0 + 1), h11 = at(i0 + 1, j0 + 1);
    const float e01  = h00 + (h10 - h00) * tu;
    const float e32  = h01 + (h11 - h01) * tu;
    const float baseH = e01 + (e32 - e01) * tv;

    // Y el detalle, con la MISMA función que la GPU (paridad medida: 34 µm en el peor caso).
    const float baseR = m_baseRadius + baseH;
    float det = Haruka::Planet::terrainDetail(glm::vec3(dir), baseR)
              * Haruka::Planet::seaLevelAttenuation(baseH);
    // Paridad con los eval de teselado: en TIERRA (baseH > 0) el detalle nunca baja del nivel del mar,
    // o el océano (esfera en R) lo taparía. La tierra queda ≥ R, el agua solo rellena los océanos.
    if (baseH > 0.0f) det = glm::max(det, -baseH);
    return (double)baseH + (double)det;
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

BiomeTextures TerrestrialPlanet::loadBiomeTextures(
    const Haruka::Planet::ClimateOutput& climate,
    const Haruka::Planet::GeologyOutput& geology)
{
    BiomeTextures bt;

    // Try loading from PNG files first
    bt.sandAlbedo  = loadTexture("sand_albedo.png");
    bt.sandNormal  = loadTexture("sand_normal.png");
    bt.grassAlbedo = loadTexture("grass_albedo.png");
    bt.grassNormal = loadTexture("grass_normal.png");
    bt.landAlbedo  = loadTexture("land_albedo.png");
    bt.landNormal  = loadTexture("land_normal.png");
    bt.rockAlbedo  = loadTexture("rock_albedo.png");
    bt.rockNormal  = loadTexture("rock_normal.png");

    // If PNGs are missing, generate all textures procedurally
    if (!RHI::valid(bt.sandAlbedo)) {
        int texSize = 512;
        float warmth = 0.5f, humidity = 0.5f;
        // Sample climate at a representative mid-latitude point
        {
            glm::dvec3 refDir = glm::normalize(glm::dvec3(0.5, 0.3, 0.2));
            float elevKm = (float)geology.elevationModifier(refDir);
            bool ocean = elevKm < 0;
            double t   = climate.temperature(refDir, elevKm, ocean);
            double h   = climate.humidity(refDir, elevKm, ocean);
            warmth   = glm::clamp((float)(t * 0.02f + 0.5f), 0.0f, 1.0f);
            humidity = glm::clamp((float)(h * 0.01f + 0.5f), 0.0f, 1.0f);
        }

        for (int i = 0; i < 4; ++i) {
            auto albedo = evaluateBiomeAlbedo(i, warmth, humidity, texSize);

            // Derive normal map from albedo grayscale (bump mapping)
            RGBAImage normal(texSize, texSize);
            std::vector<float> gray((size_t)texSize * texSize);
            for (int y = 0; y < texSize; ++y)
                for (int x = 0; x < texSize; ++x) {
                    size_t idx = ((size_t)y * texSize + x) * 4;
                    gray[(size_t)y * texSize + x] =
                        (float)(albedo.pixels[idx] + albedo.pixels[idx + 1] + albedo.pixels[idx + 2]) / (3.0f * 255.0f);
                }

            float strength = 2.0f;
            for (int y = 0; y < texSize; ++y)
                for (int x = 0; x < texSize; ++x) {
                    int xm = (x - 1 + texSize) % texSize;
                    int xp = (x + 1) % texSize;
                    int ym = (y - 1 + texSize) % texSize;
                    int yp = (y + 1) % texSize;
                    float dx = (gray[(size_t)yp * texSize + xp] - gray[(size_t)ym * texSize + xm]) * strength;
                    float dy = (gray[(size_t)yp * texSize + xm] - gray[(size_t)ym * texSize + xp]) * strength;
                    float len = std::sqrt(dx * dx + dy * dy + 1.0f);
                    normal.setPixel(x, y,
                        (uint8_t)((-dx / len * 0.5f + 0.5f) * 255.0f),
                        (uint8_t)((-dy / len * 0.5f + 0.5f) * 255.0f),
                        (uint8_t)((1.0f / len * 0.5f + 0.5f) * 255.0f), 255);
                }

            RHI::TextureHandle albedoTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(albedo);
            RHI::TextureHandle normalTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(normal);

            switch (i) {
                case 0: bt.sandAlbedo = albedoTex; bt.sandNormal = normalTex; break;
                case 1: bt.grassAlbedo = albedoTex; bt.grassNormal = normalTex; break;
                case 2: bt.landAlbedo = albedoTex; bt.landNormal = normalTex; break;
                case 3: bt.rockAlbedo = albedoTex; bt.rockNormal = normalTex; break;
            }
        }
    }
    return bt;
}

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

// Generate a macro-variation texture (equirectangular) that breaks tile repetition.
// Sized mapRes², uses climate + large-scale noise → RGBA = (brightness, hueShift, detail, _)
static RGBAImage generateMacroVariation(const Haruka::Planet::ClimateOutput& climate,
                                         const Haruka::Planet::GeologyOutput& geology,
                                         int width, int height)
{
    Graph graph;
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

    RGBAImage img(width, height);
    for (int y = 0; y < height; ++y) {
        double lat = (double)y / (height - 1) * glm::pi<double>() - glm::pi<double>() * 0.5;
        double cLat = std::cos(lat);
        double sLat = std::sin(lat);
        for (int x = 0; x < width; ++x) {
            double lon = (double)x / (width - 1) * glm::two_pi<double>();
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
    }
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
    Graph graph;

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

    RGBAImage img(width, height);
    for (int y = 0; y < height; ++y) {
        double lat = (double)y / (height - 1) * glm::pi<double>() - glm::pi<double>() * 0.5;
        double cLat = std::cos(lat);
        double sLat = std::sin(lat);
        for (int x = 0; x < width; ++x) {
            double lon = (double)x / (width - 1) * glm::two_pi<double>();
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
    }
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

std::string bakeCacheKey(uint32_t seed, int texRes,
                         const Haruka::Tools::ProcGraph::BiomeConfig& bc) {
    uint64_t h = 1469598103934665603ull;
    const char* version = "planet-bake-v1";
    bakeHashMix(h, version, std::strlen(version));
    bakeHashMix(h, &seed, sizeof(seed));
    bakeHashMix(h, &texRes, sizeof(texRes));
    auto mixVec = [&h](const glm::vec3& v) {
        bakeHashMix(h, &v.x, sizeof(v.x));
        bakeHashMix(h, &v.y, sizeof(v.y));
        bakeHashMix(h, &v.z, sizeof(v.z));
    };
    mixVec(bc.desert);  mixVec(bc.steppe);  mixVec(bc.grass);  mixVec(bc.forest);  mixVec(bc.jungle);
    mixVec(bc.ice);     mixVec(bc.tundra);  mixVec(bc.taiga);  mixVec(bc.savanna);
    float f;
    auto mixF = [&h](float x) { bakeHashMix(h, &x, sizeof(x)); };
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
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    return std::string(hex);
}

/** @brief Carpeta de los horneados: la del PROYECTO abierto si lo hay, si no relativa al binario. */
std::string bakeCacheDir() {
    const std::string& root = Haruka::AssetPaths::projectRoot();
    return root.empty() ? std::string("bakes/") : root + "bakes/";
}

std::string bakeCachePath(uint32_t seed, int texRes,
                          const Haruka::Tools::ProcGraph::BiomeConfig& bc,
                          const char* tag) {
    return bakeCacheDir() + bakeCacheKey(seed, texRes, bc) + "_" + tag + ".png";
}

bool loadBakedPNG(const std::string& path, RGBAImage& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!p) return false;
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
            std::vector<unsigned char> zResized;
            unsigned char* zUpload = m_zoneCPU.data();
            if (zf > 1) {
                boxDownsample(m_zoneCPU.data(), zw, zh, zf, zResized);
                zUpload = zResized.data();
                zw = zw / zf; zh = zh / zf;
            }
            RHI::TextureDesc zd;
            zd.width = (uint32_t)zw; zd.height = (uint32_t)zh;
            zd.format = RHI::Format::RGBA8;
            zd.filter = RHI::Filter::Linear;
            zd.wrap   = RHI::Wrap::Repeat;
            zd.mipmaps = true;
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
        int ew=0, eh=0, en=0; unsigned char* ep = nullptr;
        const std::string ec[] = {
            cfg.surface.elevationMap,
            Haruka::AssetPaths::projectRoot() + cfg.surface.elevationMap,
            Haruka::AssetPaths::projectTextures() + cfg.surface.elevationMap,
            Haruka::AssetPaths::textures() + cfg.surface.elevationMap,
        };
        for (const auto& c : ec) { ep = stbi_load(c.c_str(), &ew, &eh, &en, 4); if (ep) break; }
        if (ep) {
            m_elevCPU.assign(ep, ep + (size_t)ew * eh * 4); m_elevW = ew; m_elevH = eh;
            stbi_image_free(ep);
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
    m_biomeTex = loadBiomeTextures(m_climate, m_geology);
    HARUKA_LOGI("SimplePlanet", "build '%s': biome textures ok (sand=%d grass=%d land=%d rock=%d)",
                cfg.name.c_str(), RHI::valid(m_biomeTex.sandAlbedo), RHI::valid(m_biomeTex.grassAlbedo),
                RHI::valid(m_biomeTex.landAlbedo), RHI::valid(m_biomeTex.rockAlbedo));
    // Generate macro-variation texture (always, even with PNGs, for tile-breaking).
    // Horneado DETERMINISTA con cache en disco: la macro y el mapa de biomas son funciones puras de
    // (geología, clima, resolución, biomeConfig). Si el archivo ya existe para esta clave, se carga
    // (píxeles 1:1 con lo que se hornearía); si no, se hornea una vez y se persiste.
    if (!RHI::valid(m_macroTex)) {
        auto macro = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_biomeConfig, "macro"),
                                [&] { return generateMacroVariation(m_climate, m_geology, m_mapRes, m_mapRes); });
        m_macroTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(macro);
    }
    HARUKA_LOGI("SimplePlanet", "build '%s': macro tex ok", cfg.name.c_str());
    // Generate biome map texture (always — replaces GLSL biomeColor() classification)
    if (!RHI::valid(m_biomeMapTex)) {
        auto biomeImg = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_biomeConfig, "biome"),
                                   [&] {
                                       return generateBiomeMap(m_climate, m_geology, m_mapRes, m_mapRes,
                                                               0.004f, m_biomeConfig);
                                   });
        m_biomeMapTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(biomeImg);
    }
    HARUKA_LOGI("SimplePlanet", "build '%s': biome map ok", cfg.name.c_str());
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
    m_biomeTex = loadBiomeTextures(m_climate, m_geology);
    // Con geología NUEVA los mapas viejos (horneados sobre el reparto de placas anterior)
    // quedarían desfasados: se regeneran SIEMPRE, no "si faltan". La clave del cache incluye la
    // semilla nueva, así que `loadOrBake` o bien carga el horneado de ESTA semilla (si ya se hizo
    // alguna vez) o bien hornea de nuevo — nunca reutiliza el de otra semilla.
    {
        auto macro = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_biomeConfig, "macro"),
                                [&] { return generateMacroVariation(m_climate, m_geology, m_mapRes, m_mapRes); });
        m_macroTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(macro);
        auto biomeImg = loadOrBake(bakeCachePath(m_seed, m_mapRes, m_biomeConfig, "biome"),
                                   [&] {
                                       return generateBiomeMap(m_climate, m_geology, m_mapRes, m_mapRes,
                                                               0.004f, m_biomeConfig);
                                   });
        m_biomeMapTex = Haruka::Tools::ProcGraph::createRHIFromRGBA(biomeImg);
    }
}

void TerrestrialPlanet::rebuildWithEdits(const std::function<float(const glm::dvec3&)>& editFn) {
    if (!m_baseHeightFn) return;
    auto combined = [&](const glm::dvec3& dir) -> float {
        return m_baseHeightFn(dir) + editFn(dir);
    };
    clearGPU();
    buildMesh(combined, m_config.radius);
}

// ── Render ───────────────────────────────────────────────────────────────

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
    ubo.uAmbient    = glm::vec4(m_ambientStrength * glm::vec3(0.55f, 0.65f, 0.85f), 0.0f);
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
    ubo.uDebug = glm::vec4((float)m_debugView, elapsedSec, 0.0f, 0.0f);
    dev->updateBuffer(s_ubo, 0, sizeof(ubo), &ubo);
    ctx->bindUniformBuffer(0, s_ubo);

    // TABLA DE MATERIALES → UBO binding 12. Se sube una vez por planeta (perezosa) y solo se
    // reescribe si la tabla cambió: es dato de autoría, no estado por frame.
    {
        struct GpuMat { glm::vec4 a, b, c, d, e; };
        struct GpuTable {
            glm::vec4 count;
            GpuMat    mats[Haruka::Planet::TerrainMaterialTable::kMaxMaterials];
        } table{};
        const auto& src = m_materialTable.materials;
        const int n = std::min((int)src.size(),
                               Haruka::Planet::TerrainMaterialTable::kMaxMaterials);
        table.count = glm::vec4((float)n, 0, 0, 0);
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
            table.mats[i].d = glm::vec4(m.detail, m.feather, zonePacked,
                                        m.hasZone() ? 1.0f : 0.0f);
            table.mats[i].e = m.hasColor()
                ? glm::vec4(m.baseColor, glm::clamp(m.colorWeight, 0.0f, 1.0f))
                : glm::vec4(0.0f);
        }
        if (!RHI::valid(m_materialUBO)) {
            m_materialUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(table),
                                              &table, RHI::BufferMemory::Dynamic);
        } else {
            dev->updateBuffer(m_materialUBO, 0, sizeof(table), &table);
        }
        ctx->bindUniformBuffer(12, m_materialUBO);
    }

    // Bind macro-variation texture (slot 10) if available — both paths
    if (RHI::valid(m_macroTex))
        ctx->bindTexture(10, m_macroTex);
    // Bind biome map texture (slot 11) — replaces GLSL biomeColor() classification
    if (RHI::valid(m_biomeMapTex))
        ctx->bindTexture(11, m_biomeMapTex);

    // Draw terrain — biome-blended pipeline preferred
    // Bindings must match biome.frag declarations:
    //   1=sandAlbedo, 2=sandNormal, 3=grassAlbedo, 4=grassNormal,
    //   5=landAlbedo, 6=landNormal, 7=rockAlbedo, 8=rockNormal.
    // Arrays de terreno: unidades 12 y 13. El UBO de materiales también es 12, pero en GL los
    // espacios de binding de UBO y de textura son distintos y no chocan (en Vulkan irían en
    // sets/bindings separados igualmente).
    if (RHI::valid(m_biomeTex.albedoArray)) ctx->bindTexture(12, m_biomeTex.albedoArray);
    if (RHI::valid(m_biomeTex.normalArray)) ctx->bindTexture(13, m_biomeTex.normalArray);
    if (RHI::valid(m_zoneTex))              ctx->bindTexture(14, m_zoneTex);

    // TESELADO si está disponible: la malla base es la misma, cambia el índice (parches) y el
    // pipeline. Si el pipeline no compiló se cae a la malla sin teselar.
    const bool useTess = RHI::valid(s_tessPipeline) && RHI::valid(m_patchIB) && hasBiome;

    // CLIPMAP: solo tiene sentido si la cámara está lo bastante cerca de la superficie. Con la
    // cámara en órbita la rejilla sería un punto, y el coste de decidirlo es despreciable.
    const double camAlt = glm::length(cameraPos - m_config.position) - m_config.radius;
    const bool useClip = RHI::valid(s_clipPipeline) && RHI::valid(m_clipVB) &&
                         RHI::valid(m_baseFieldTex) && camAlt < 20000.0 && hasBiome;

    if (useTess) {
        ctx->bindPipeline(s_tessPipeline);
        ctx->bindTexture(1, m_biomeTex.sandAlbedo);
        ctx->bindTexture(2, m_biomeTex.sandNormal);
        ctx->bindTexture(3, m_biomeTex.grassAlbedo);
        ctx->bindTexture(5, m_biomeTex.landAlbedo);
        ctx->bindTexture(6, m_biomeTex.landNormal);
        ctx->bindTexture(7, m_biomeTex.rockAlbedo);
        ctx->bindTexture(8, m_biomeTex.rockNormal);
        ctx->bindVertexBuffer(m_vertexBuffer);
        ctx->bindIndexBuffer(m_patchIB);
        ctx->drawIndexed(m_patchIndexCount);
    } else if (hasBiome) {
        ctx->bindPipeline(s_biomePipeline);
        ctx->bindTexture(1, m_biomeTex.sandAlbedo);
        ctx->bindTexture(2, m_biomeTex.sandNormal);
        ctx->bindTexture(3, m_biomeTex.grassAlbedo);
        ctx->bindTexture(5, m_biomeTex.landAlbedo);
        ctx->bindTexture(6, m_biomeTex.landNormal);
        ctx->bindTexture(7, m_biomeTex.rockAlbedo);
        ctx->bindTexture(8, m_biomeTex.rockNormal);
    } else {
        ctx->bindPipeline(s_pipeline);
    }
    if (!useTess) {
        ctx->bindVertexBuffer(m_vertexBuffer);
        ctx->bindIndexBuffer(m_indexBuffer);
        ctx->drawIndexed(m_indexCount);
    }

    // El clipmap va DESPUÉS de la malla del planeta, no antes. Las dos describen el MISMO
    // suelo —misma altura base, misma función de detalle—, así que son coplanares a propósito y
    // el orden no las separa: lo que decide es el sesgo de profundidad del pipeline.
    if (useClip) {
        // Marco tangente en la vertical de la cámara. El clipmap vive en ESE plano, así que se
        // reorienta solo al moverse: no hay que regenerar la rejilla nunca.
        const glm::dvec3 up = glm::normalize(cameraPos - m_config.position);
        glm::dvec3 tu = glm::cross(glm::dvec3(0, 1, 0), up);
        if (glm::length(tu) < 1e-6) tu = glm::cross(glm::dvec3(1, 0, 0), up);
        tu = glm::normalize(tu);
        const glm::dvec3 tv = glm::cross(up, tu);

        struct ClipParams { glm::vec4 origin, tanU, tanV; } cp{};
        cp.origin = glm::vec4(glm::vec3(up), 128.0f);
        cp.tanU   = glm::vec4(glm::vec3(tu), 0.0f);
        cp.tanV   = glm::vec4(glm::vec3(tv), 0.0f);
        if (!RHI::valid(m_clipUBO))
            m_clipUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(cp), &cp, RHI::BufferMemory::Dynamic);
        else
            dev->updateBuffer(m_clipUBO, 0, sizeof(cp), &cp);

        ctx->bindPipeline(s_clipPipeline);
        ctx->bindUniformBuffer(0, s_ubo);
        ctx->bindUniformBuffer(13, m_clipUBO);
        ctx->bindTexture(15, m_baseFieldTex);
        ctx->bindTexture(1, m_biomeTex.sandAlbedo);
        ctx->bindTexture(2, m_biomeTex.sandNormal);
        ctx->bindTexture(3, m_biomeTex.grassAlbedo);
        ctx->bindTexture(5, m_biomeTex.landAlbedo);
        ctx->bindTexture(6, m_biomeTex.landNormal);
        ctx->bindTexture(7, m_biomeTex.rockAlbedo);
        ctx->bindTexture(8, m_biomeTex.rockNormal);
        ctx->bindVertexBuffer(m_clipVB);
        ctx->bindIndexBuffer(m_clipIB);
        ctx->drawIndexed(m_clipIndexCount);
    }

    // Draw water (transparent ocean sphere). El océano es una esfera a nivel del mar: la tierra está
    // garantizada por encima (el detalle se recorta para no hundir el suelo bajo el nivel del mar, ver
    // los eval de teselado), así que la esfera solo rellena los océanos y no tapa la costa. Quitarla:
    // HARUKA_NOWATER=1 (deja a la vista el suelo sumergido, con su relieve de ruido).
    if (RHI::valid(m_waterVB) && !std::getenv("HARUKA_NOWATER")) {
        ctx->bindPipeline(s_waterPipeline);
        ctx->bindVertexBuffer(m_waterVB);
        ctx->bindIndexBuffer(m_waterIB);
        // La costa per-pixel de water.frag lee el campo base (binding 15): se enlaza aquí
        // SIEMPRE, no solo cuando está el clipmap, o el agua muestrearía una textura sin bindear.
        if (RHI::valid(m_baseFieldTex)) ctx->bindTexture(15, m_baseFieldTex);
        ctx->drawIndexed(m_waterIndexCount);
    }
}

}} // namespace Haruka::Planet
