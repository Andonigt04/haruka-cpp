#include "gpu_heightfield.h"
#include "core/terrain/planet_geology.h"   // F2: tabla de placas → GPU
#include "core/terrain/planet_fields.h"    // F3: campo erosionado → GPU
#include "core/terrain/planet_meso.h"      // F3-MESO: teselas erosionadas → GPU
#include "renderer/compute_shader.h"
#include "core/asset_paths.h" // AssetPaths::shaders() (ruta canónica de shaders)
#include "rhi/rhi_device.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace Haruka {


// El CAMPO EROSIONADO en GPU. Se genera una vez por seed (erosión iterativa: NO es una fórmula) y
// lo comparten el compute de terreno y el shader de agua. Una sola fuente de verdad, un solo buffer.
// El MESO en GPU: mismo generador determinista que la CPU → las teselas son idénticas aunque cada
// lado las construya por su cuenta. Se PIDEN las teselas de las direcciones del chunk ANTES de
// despachar (si no, el compute cae al macro y la malla no casaría con la física).
static PlanetFields& fieldsInstance(uint32_t seed);

// MESO **ENCENDIDO POR DEFECTO** (`HARUKA_MESO=0` lo apaga).
//
// El compute BAJA las colinas a ±120 m repartidos en ~21 km (una rampa) *a propósito*, porque el
// detalle a escala humana lo pone el MESO (teselas de ~5 km a 40 m/téxel, erosionadas). Con el meso
// apagado, ese detalle no lo pone NADIE y el mundo queda LISO: "no tiene desnivel". O sea que
// tenerlo opt-in no era una precaución: era dejar el terreno a medias.
bool mesoEnabled() {
    static const bool on = [] {
        const char* v = getenv("HARUKA_MESO");
        return !(v && v[0] == '0');
    }();
    return on;
}

PlanetFields& planetFieldsFor(uint32_t seed) { return fieldsInstance(seed); }

// ⚠️ CACHÉ POR SEED, no de un solo hueco. Antes había UNA instancia y un `s_seed`: con dos planetas
// de seeds distintas (Tierra + Luna) cada uno pedía su campo POR FRAME → se regeneraba el campo
// ENTERO (tectónica + erosión: segundos) una y otra vez. El juego se arrastraba y la suite se colgaba.
static PlanetFields& fieldsInstance(uint32_t seed) {
    static std::mutex mx;
    static std::unordered_map<uint32_t, std::unique_ptr<PlanetFields>> cache;
    std::lock_guard<std::mutex> lk(mx);
    auto it = cache.find(seed);
    if (it != cache.end()) return *it->second;
    auto f = std::make_unique<PlanetFields>();
    PlanetGeology g; g.generate(seed);
    f->generate(seed, g, 256, 40);
    auto& ref = *f;
    cache.emplace(seed, std::move(f));
    return ref;
}

unsigned int planetFieldsSSBO(uint32_t seed, int& outRes) {
    // UN SSBO POR SEED (ver fieldsInstance): con un solo hueco, dos planetas se pisaban el buffer y
    // lo resubían cada frame (393k celdas) — y, peor, cada uno veía el campo del otro.
    struct Entry { GLuint ssbo = 0; int res = 0; };
    static std::unordered_map<uint32_t, Entry> s_cache;
    Entry& e = s_cache[seed];
    if (e.ssbo == 0) {
        glGenBuffers(1, &e.ssbo);
        PlanetFields& fl = fieldsInstance(seed);
        const int R = fl.faceRes();
        std::vector<glm::vec4> cells((size_t)6 * R * R);
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const float u = ((i + 0.5f) / R) * 2.0f - 1.0f;
                    const float v = ((j + 0.5f) / R) * 2.0f - 1.0f;
                    glm::vec3 d;   // misma proyección de cara que PlanetFields
                    switch (f) {
                        case 0: d = glm::vec3( 1, -v, -u); break;
                        case 1: d = glm::vec3(-1, -v,  u); break;
                        case 2: d = glm::vec3( u,  1,  v); break;
                        case 3: d = glm::vec3( u, -1, -v); break;
                        case 4: d = glm::vec3( u, -v,  1); break;
                        default:d = glm::vec3(-u, -v, -1); break;
                    }
                    const FieldSample fs = fl.sample(glm::normalize(d));
                    cells[(size_t)f * R * R + (size_t)j * R + i] =
                        glm::vec4(fs.elevKm, fs.flow, fs.waterKm, fs.orogeny);
                }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, e.ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, cells.size() * sizeof(glm::vec4), cells.data(), GL_STATIC_DRAW);
        e.res = R;
        // DIAG (una vez por seed): rango de elevación del campo que alimenta la MALLA. Si min≈max≈0 el
        // campo llega PLANO → la malla sale esfera base ("terreno default"). Con relieve real el rango
        // va de la fosa (~-8 km) al pico (~+9 km). HARUKA_FIELD_DBG=1 para verlo.
        if (getenv("HARUKA_FIELD_DBG")) {
            float mn = 1e9f, mx = -1e9f;
            for (const auto& c : cells) { mn = std::min(mn, c.x); mx = std::max(mx, c.x); }
            std::fprintf(stderr, "[field-dbg] seed=%u res=%d elevKm[min=%.3f max=%.3f]  (min~max~0 = campo PLANO)\n",
                         seed, R, mn, mx);
        }
    }
    outRes = e.res;
    return e.ssbo;
}


unsigned int planetClimateSSBO(uint32_t seed, int& outRes) {
    struct Entry { GLuint ssbo = 0; int res = 0; };
    static std::unordered_map<uint32_t, Entry> s_cache;
    Entry& e = s_cache[seed];
    if (e.ssbo == 0) {
        glGenBuffers(1, &e.ssbo);
        PlanetFields& fl = fieldsInstance(seed);
        const int R = fl.faceRes();
        std::vector<glm::vec4> cells((size_t)6 * R * R);
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const float u = ((i + 0.5f) / R) * 2.0f - 1.0f;
                    const float v = ((j + 0.5f) / R) * 2.0f - 1.0f;
                    glm::vec3 d;   // MISMA proyección de cara que el campo (o el bioma se desplaza)
                    switch (f) {
                        case 0: d = glm::vec3( 1, -v, -u); break;
                        case 1: d = glm::vec3(-1, -v,  u); break;
                        case 2: d = glm::vec3( u,  1,  v); break;
                        case 3: d = glm::vec3( u, -1, -v); break;
                        case 4: d = glm::vec3( u, -v,  1); break;
                        default:d = glm::vec3(-u, -v, -1); break;
                    }
                    const FieldSample fs = fl.sample(glm::normalize(d));
                    cells[(size_t)f * R * R + (size_t)j * R + i] =
                        glm::vec4(fs.tempC, fs.humidity, 0.0f, 0.0f);
                }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, e.ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, cells.size() * sizeof(glm::vec4), cells.data(), GL_STATIC_DRAW);
        e.res = R;
    }
    outRes = e.res;
    return e.ssbo;
}

GpuHeightfield::GpuHeightfield() = default;
GpuHeightfield::~GpuHeightfield() {
    auto del = [](Buffers& b) {
        if (RHI::valid(b.hDirs))
            if (RHI::Device* dev = RHI::device()) {
                dev->destroy(b.hDirs); dev->destroy(b.hElev); dev->destroy(b.hNorm); dev->destroy(b.hWater);
            }
    };
    del(m_sync);
    for (auto& s : m_slot) { del(s.buf); if (s.fence) glDeleteSync(s.fence); }
}

bool GpuHeightfield::ensureShader() {
    if (m_failed) return false;
    if (!m_shader) {
        // AssetPaths::shaders() = "assets/shaders/" → ComputeShader (que NO aplica el
        // base de Shader) encuentra el fichero tras mover los shaders a assets/. Sin esto
        // buscaba "shaders/terrain_gen.comp" (inexistente) → compute vacío → sin terreno.
        m_shader = std::make_unique<Haruka::Renderer::ComputeShader>(
            Haruka::AssetPaths::shaders() + "terrain_gen.comp");
        // OJO: getID()!=0 NO basta — el handle se crea aunque el LINK falle. Usar
        // linked() (GL_LINK_STATUS) o el compute se "usa" sin linkar → glDispatchCompute
        // "no active compute shader" cada frame + sin terreno. Si falló, marcamos failed.
        if (!m_shader->linked()) {
            fprintf(stderr, "[GpuHeightfield] terrain_gen.comp NO linkó → terreno GPU deshabilitado\n");
            m_failed = true; return false;
        }
    }
    return true;
}

void GpuHeightfield::ensureBuffers(Buffers& b, std::size_t n) {
    if (n <= b.capacity && b.dirs) return;

    // Ruta RHI: 4 SSBOs dinámicos. Al crecer, se destruyen y recrean (storage inmutable).
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(b.hDirs)) { dev->destroy(b.hDirs); dev->destroy(b.hElev); dev->destroy(b.hNorm); dev->destroy(b.hWater); }
        // dirs: la CPU escribe → Dynamic. elev/norm/water: la GPU escribe y la CPU LEE → Readback
        // (mapeado persistente). Cosechar deja de ser 3 glGetBufferSubData por chunk (sincronizan
        // con el driver) y pasa a ser 3 memcpy: es lo que hacía que `pump.harvest` costase ~6 ms.
        b.hDirs  = dev->createBuffer(RHI::BufferUsage::Storage, n * sizeof(glm::vec4), nullptr, RHI::BufferMemory::Dynamic);
        b.hElev  = dev->createBuffer(RHI::BufferUsage::Storage, n * sizeof(float),     nullptr, RHI::BufferMemory::Readback);
        b.hNorm  = dev->createBuffer(RHI::BufferUsage::Storage, n * sizeof(glm::vec4), nullptr, RHI::BufferMemory::Readback);
        b.hWater = dev->createBuffer(RHI::BufferUsage::Storage, n * sizeof(float),     nullptr, RHI::BufferMemory::Readback);
        b.dirs = dev->nativeBuffer(b.hDirs); b.elev = dev->nativeBuffer(b.hElev);
        b.norm = dev->nativeBuffer(b.hNorm); b.water = dev->nativeBuffer(b.hWater);
        b.capacity = n;
    }
}

void GpuHeightfield::uploadAndDispatch(Buffers& b, const std::vector<glm::vec3>& dirs, const Params& p,
                                       std::size_t count) {
    const std::size_t n = p.deriveDirs ? count : dirs.size();
    ensureBuffers(b, n);
    if (!p.deriveDirs) { // la GPU no las deriva → hay que subir la rejilla
        std::vector<glm::vec4> dirs4(n);
        for (std::size_t i = 0; i < n; ++i) dirs4[i] = glm::vec4(dirs[i], 0.0f);
        if (RHI::valid(b.hDirs))
            if (RHI::Device* dev = RHI::device()) dev->updateBuffer(b.hDirs, 0, n * sizeof(glm::vec4), dirs4.data());
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, b.dirs);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, b.elev);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, b.norm);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, b.water);

    m_shader->use();
    glUniform1i(0, (int)n);
    glUniform1i(1, p.seed);
    glUniform1f(2, p.continentFreqA);
    glUniform1f(3, p.reliefStrength);
    glUniform1f(4, (float)(p.radius > 1e-9 ? 1.0 / p.radius : 0.0));
    glUniform1f(5, p.seaThreshold);
    glUniform1f(6, p.voronoiDensity);
    glUniform1f(7, p.lakeDensity);
    glUniform1f(8, p.lakeMaxProb);
    glUniform1f(9, p.coastWidth);
    glUniform1f(10, p.vertexSpacingM);

    // Derivación de direcciones en GPU (ver Params::deriveDirs).
    glUniform1i(40, p.deriveDirs ? 1 : 0);
    glUniform1i(41, p.gridRes);
    glUniform1i(42, p.face);
    glUniform1i(43, p.lod);
    glUniform1i(44, p.chunkX);
    glUniform1i(45, p.chunkY);

    // --- F2: TABLA DE PLACAS -------------------------------------------------------------
    // Las placas las genera C++ (PlanetGeology) y se SUBEN: los datos tienen UNA sola fuente de
    // verdad, así que GPU y CPU no pueden discrepar por generarlas cada uno por su cuenta. Lo
    // único duplicado es la fórmula de `sample`, cubierta por el test de paridad.
    {
        static uint32_t s_geoSeed = 0xFFFFFFFFu;
        static GLuint   s_geoSSBO = 0;
        static int      s_geoCount = 0;
        if (s_geoSSBO == 0) glGenBuffers(1, &s_geoSSBO);
        if (s_geoSeed != (uint32_t)p.seed) {
            PlanetGeology geo;
            geo.generate((uint32_t)p.seed);
            struct GpuPlate { glm::vec4 siteCont; glm::vec4 motionSpd; };
            std::vector<GpuPlate> gp;
            gp.reserve(geo.plates().size());
            for (const auto& pl : geo.plates())
                gp.push_back({ glm::vec4(pl.site, pl.continental ? 1.0f : 0.0f),
                               glm::vec4(pl.motion, pl.speed) });
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_geoSSBO);
            glBufferData(GL_SHADER_STORAGE_BUFFER, gp.size() * sizeof(GpuPlate), gp.data(), GL_STATIC_DRAW);
            s_geoCount = (int)gp.size();
            s_geoSeed  = (uint32_t)p.seed;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_geoSSBO);
        glUniform1i(20, s_geoCount);
        {
            int fldRes = 0;
            const unsigned int fldSSBO = planetFieldsSSBO((uint32_t)p.seed, fldRes);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, fldSSBO);
            glUniform1i(30, fldRes);

            // --- MESO: asegurar las teselas de ESTE chunk y subir el atlas -------------------
            // ⚠️ UN ATLAS POR SEED. Eran dos GLuint ESTÁTICOS compartidos por todos los planetas,
            // pero cada planeta tiene su PROPIO PlanetMeso (otra escala de tesela, otras teselas).
            // Con el meso apagado no se notaba; al encenderlo, los chunks de la Tierra muestreaban el
            // atlas que acababa de subir la LUNA → alturas basura → la malla se iba fuera de sitio y
            // **el terreno desaparecía** (quedaba solo la esfera base, que no usa meso).
            struct MesoBufs { GLuint atlas = 0, table = 0; size_t atlasBytes = 0; };
            static std::unordered_map<uint32_t, MesoBufs> s_meso;
            MesoBufs& mb = s_meso[(uint32_t)p.seed];
            // La MISMA instancia que usa el sampler de física (una sola caché de teselas → paridad).
            PlanetMeso& meso = planetMeso((uint32_t)p.seed, fieldsInstance((uint32_t)p.seed), p.radius);
            if (mb.atlas == 0) { glGenBuffers(1, &mb.atlas); glGenBuffers(1, &mb.table); }

            // ⚠️ SOLO EN CHUNKS FINOS. Un chunk de LOD bajo abarca media cara del cubo: pedirle una
            // tesela por vértice serían MILES de teselas (~10 ms cada una) → el generador se cuelga
            // (pasó). Y no haría falta: a esa distancia el detalle de 40 m ni se ve. Lejos → macro.
            // ⚠️ DESACTIVADO POR DEFECTO (HARUKA_MESO=1 para probarlo). El coste de generar teselas
            // en el hilo de generación se dispara y cuelga el arranque: hay que moverlo a un WORKER
            // con presupuesto (N teselas por frame) antes de encenderlo. La lógica y el test de
            // costuras están hechos y verdes; lo que falta es el PRESUPUESTO, no el algoritmo.
            const bool useMeso = mesoEnabled() && (p.vertexSpacingM > 0.0f && p.vertexSpacingM < 200.0f);
            // Las teselas YA se pidieron (y se esperaron) en TerrainGenerator::gpuDispatch: si
            // llegamos aquí con useMeso, están listas. Nada de generar en este hilo.

            // Snapshot COHERENTE (bajo lock): el worker escribe el atlas en otro hilo.
            std::vector<glm::ivec4> table;
            std::vector<int>        dirtySlots;
            std::vector<float>      dirtyData;
            meso.gpuSnapshot(table, dirtySlots, dirtyData);

            const size_t atlasBytes = PlanetMeso::atlasFloats() * sizeof(float);
            if (mb.atlasBytes != atlasBytes) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, mb.atlas);
                glBufferData(GL_SHADER_STORAGE_BUFFER, atlasBytes, nullptr, GL_DYNAMIC_DRAW);
                mb.atlasBytes = atlasBytes;
            }
            if (!dirtySlots.empty()) {
                const size_t tileFloats = (size_t)PlanetMeso::kTileRes * PlanetMeso::kTileRes;
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, mb.atlas);
                for (size_t i = 0; i < dirtySlots.size(); ++i)
                    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                    (GLintptr)(dirtySlots[i] * tileFloats * sizeof(float)),
                                    (GLsizeiptr)(tileFloats * sizeof(float)),
                                    dirtyData.data() + i * tileFloats);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, mb.table);
            glBufferData(GL_SHADER_STORAGE_BUFFER, table.size() * sizeof(glm::ivec4),
                         table.data(), GL_DYNAMIC_DRAW);

            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, mb.atlas);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, mb.table);
            glUniform1i(31, useMeso ? PlanetMeso::kTileRes : 0);   // 0 = sin meso → el shader usa macro
            glUniform1i(32, meso.tileLevel());
            glUniform1i(33, PlanetMeso::kHashSize);   // tabla HASH (pot. de 2), no el cupo
        }
        glUniform1f(21, PlanetGeology::kContinentalBaseKm);
        glUniform1f(22, PlanetGeology::kOceanicBaseKm);
        glUniform1f(23, PlanetGeology::kBoundaryWidthRad);
        glUniform1f(24, PlanetGeology::kMaxOrogenyKm);
        glUniform1f(25, PlanetGeology::kMarginRad);
    }

    glDispatchCompute((GLuint)((n + 63) / 64), 1, 1);
}

void GpuHeightfield::readback(Buffers& b, std::size_t n, std::vector<float>& elev,
                              std::vector<glm::vec4>& normal4, std::vector<float>& water) {
    elev.resize(n); water.resize(n); normal4.resize(n);

    // Camino rápido: los buffers están mapeados persistentes → copiar es memcpy, sin driver.
    // El llamador YA esperó la fence, así que los datos están completos.
    RHI::Device* dev = RHI::device();
    const void* mElev  = dev ? dev->mappedData(b.hElev)  : nullptr;
    const void* mWater = dev ? dev->mappedData(b.hWater) : nullptr;
    const void* mNorm  = dev ? dev->mappedData(b.hNorm)  : nullptr;
    if (mElev && mWater && mNorm) {
        std::memcpy(elev.data(),    mElev,  n * sizeof(float));
        std::memcpy(water.data(),   mWater, n * sizeof(float));
        std::memcpy(normal4.data(), mNorm,  n * sizeof(glm::vec4));
        return;
    }

    // Fallback (sin RHI / sin mapeo): lectura clásica.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.elev);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(float), elev.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.water);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(float), water.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b.norm);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, n * sizeof(glm::vec4), normal4.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// Espera (bloqueando) a que la GPU termine, y libera la fence.
static void waitFence(GLsync fence) {
    if (!fence) return;
    GLenum r = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); // 1 s
    while (r == GL_TIMEOUT_EXPIRED) r = glClientWaitSync(fence, 0, 1000000000ull);
    glDeleteSync(fence);
}

bool GpuHeightfield::generate(const std::vector<glm::vec3>& dirs, const Params& p,
                              std::vector<float>& outElevKm, std::vector<glm::vec3>& outNormal,
                              std::vector<float>& outWaterKm) {
    if (!ensureShader()) return false;
    if (dirs.empty()) { outElevKm.clear(); outNormal.clear(); outWaterKm.clear(); return true; }
    uploadAndDispatch(m_sync, dirs, p);
    // CLIENT_MAPPED: elev/norm/water están mapeados persistentes; sin este bit el driver no
    // garantiza que lo escrito por el compute sea visible por el puntero mapeado.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT
                    | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    // Los buffers están MAPEADOS: leerlos es un memcpy y NO sincroniza (glGetBufferSubData sí lo
    // hacía). Este camino es síncrono por contrato → hay que esperar al compute explícitamente,
    // o se leería basura (síntoma: el test de paridad CPU↔GPU se dispara).
    waitFence(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    std::vector<glm::vec4> norm4;
    readback(m_sync, dirs.size(), outElevKm, norm4, outWaterKm);
    outNormal.resize(norm4.size());
    for (std::size_t i = 0; i < norm4.size(); ++i) outNormal[i] = glm::vec3(norm4[i]);
    return true;
}

// --------------------------- camino ASÍNCRONO ---------------------------

bool GpuHeightfield::hasFreeSlot() const {
    for (const auto& s : m_slot) if (!s.inUse) return true;
    return false;
}

int GpuHeightfield::dispatchAsync(const std::vector<glm::vec3>& dirs, const Params& p, std::size_t count) {
    const std::size_t n = p.deriveDirs ? count : dirs.size();
    if (!ensureShader() || n == 0) return -1;
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) if (!m_slot[i].inUse) { slot = i; break; }
    if (slot < 0) return -1;
    Slot& s = m_slot[slot];
    uploadAndDispatch(s.buf, dirs, p, n);
    // CLIENT_MAPPED: elev/norm/water están mapeados persistentes; sin este bit el driver no
    // garantiza que lo escrito por el compute sea visible por el puntero mapeado.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT
                    | GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s.count = n;
    s.inUse = true;
    return slot;
}

bool GpuHeightfield::tryHarvest(int slot, std::vector<float>& outElevKm, std::vector<glm::vec4>& outNormal4,
                                std::vector<float>& outWaterKm) {
    if (slot < 0 || slot >= kSlots) return false;
    Slot& s = m_slot[slot];
    if (!s.inUse || !s.fence) return false;
    GLenum r = glClientWaitSync(s.fence, 0, 0); // 0 timeout → no bloquea
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return false; // aún computa
    readback(s.buf, s.count, outElevKm, outNormal4, outWaterKm);
    glDeleteSync(s.fence); s.fence = nullptr; s.inUse = false;
    return true;
}

bool GpuHeightfield::tryMapHarvest(int slot, MappedView& out) {
    if (slot < 0 || slot >= kSlots) return false;
    Slot& s = m_slot[slot];
    if (!s.inUse || !s.fence) return false;
    GLenum r = glClientWaitSync(s.fence, 0, 0); // 0 timeout → no bloquea
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return false; // aún computa

    RHI::Device* dev = RHI::device();
    if (!dev) return false;
    const void* e = dev->mappedData(s.buf.hElev);
    const void* nn = dev->mappedData(s.buf.hNorm);
    const void* w = dev->mappedData(s.buf.hWater);
    if (!e || !nn || !w) return false; // sin mapeo persistente → el llamador usará tryHarvest

    // La fence ya se cumplió: los datos están completos y el slot queda RESERVADO (inUse) hasta
    // que el worker llame a releaseSlot → nadie los sobreescribe mientras los copia.
    glDeleteSync(s.fence); s.fence = nullptr;
    out = MappedView{ (const float*)e, (const glm::vec4*)nn, (const float*)w, s.count };
    return true;
}

void GpuHeightfield::releaseSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    m_slot[slot].inUse.store(false, std::memory_order_release);
}

} // namespace Haruka
