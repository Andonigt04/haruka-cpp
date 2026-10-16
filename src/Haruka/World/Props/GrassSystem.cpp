#include "Haruka/World/Props/GrassSystem.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/Physics/PhysicsEngine.h"
#include "Haruka/World/Planet/Planet.h"
#include "Haruka/World/Props/PropCollider.h"   // propCullRadiusM
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Haruka::World::Props {

using RHI::valid;

namespace {
// Reloj de las pisadas: UN epoch para que lo sellado en `flattenNear` y lo que compara el relevo sean
// comparables. De PARED: la pisada no es estado de juego, solo vida efímera.
double grassClockNow() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

GrassSystem::~GrassSystem() { GrassSystem::shutdown(); }

void GrassSystem::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        for (RHI::BufferHandle* b : { &m_burstSSBO, &m_stateSSBO, &m_cmdBuf })
            if (valid(*b)) dev->destroy(*b);
        if (valid(m_pso)) dev->destroy(m_pso);
    }
    m_burstSSBO = {}; m_stateSSBO = {}; m_cmdBuf = {}; m_pso = {};
    m_burstCap = m_stateCap = m_cmdCap = 0;
    m_dirty = true;
    PropSystemBase::shutdown();   // worker + mallas de prototipo + UBOs de parámetros
}

// ── re-siembra (los huecos de PropSystemBase) ─────────────────────────────────────────────────

void GrassSystem::configureScatter(PropScatterParams& params) {
    // Un BROTE por celda: la CPU guarda uno y la GPU expande las matas, así que el halo cercano ya no
    // cuesta memoria por mata y puede ser MUCHO más denso y ancho que cuando cada mata era un prop
    // (24 m ×150). Mandos para medir sin recompilar: `HARUKA_GRASS_NEAR=<m>`,
    // `HARUKA_GRASS_MUL=<x>` (tope de matas por brote: kMassMaxCount).
    static const float s_near = [] { const char* e = std::getenv("HARUKA_GRASS_NEAR"); return e ? (float)std::atof(e) : 48.0f; }();
    static const float s_mul  = [] { const char* e = std::getenv("HARUKA_GRASS_MUL");  return e ? (float)std::atof(e) : 320.0f; }();
    params.massBursts     = true;
    params.grassNearM     = s_near;
    params.grassNearMul   = s_mul;
    params.grassNearFadeM = 90.0f;
    params.maxProps       = 400000;   // BROTES, no matas
    params.fadeInM        = 300.0f;
    // Las bandas históricas: la hierba suelta muere en su `maxDistM` (~1 km); más allá solo quedan
    // los CÚMULOS de pradera de las celdas gruesas.
    params.lods = { { 1000.0f, 12.0f, 0.30f }, { 3000.0f, 40.0f, 0.30f }, { 6000.0f, 120.0f, 0.30f } };
}

GrassSystem::ScatterJob GrassSystem::makeJob(const ScatterContext& ctx) {
    const std::unordered_map<std::string, int> protoByName = ctx.protoByName;
    const glm::dvec3 camPos = ctx.camPos;
    return [protoByName, camPos](std::vector<ScatteredProp>&& placed) -> std::shared_ptr<ScatterOutput> {
        auto out = std::make_shared<GrassOutput>();
        out->origin = camPos;
        out->bursts.reserve(placed.size());
        for (const ScatteredProp& sp : placed) {
            const auto itP = protoByName.find(sp.mesh);
            if (itP == protoByName.end()) continue;
            MassBurst b;
            b.proto = itP->second; b.dir = sp.dir; b.heightM = sp.heightM; b.seed = sp.cellSeed;
            if (sp.burst > 0) {
                b.cellM = sp.burstCellM; b.count = sp.burst;
                b.scaleMin = sp.scale; b.scaleMax = sp.burstScaleMax;
            } else {
                // CÚMULO de pradera (celda gruesa): UNA masa en el centro, a su escala exacta. Un
                // brote de una mata con celda 0 es eso mismo, y se destruye igual que la hierba.
                b.cellM = 0.0f; b.count = 1;
                b.scaleMin = b.scaleMax = sp.scale;
            }
            out->bursts.push_back(b);
        }
        return out;
    };
}

void GrassSystem::applyScatter(std::shared_ptr<ScatterOutput> outBase, const ApplyContext& ctx) {
    GrassOutput& out = static_cast<GrassOutput&>(*outBase);
    HARUKA_PROFILE("hierba.aplica");
    // Mismos descartes que los props: boca de cueva y cuenco de cráter, contra el vox VIVO.
    const SurfaceFilter filter = makeSurfaceFilter(ctx.planet);
    std::vector<MassBurst> kept;
    kept.reserve(out.bursts.size());
    for (const MassBurst& b : out.bursts)
        if (!filter.removes(b.dir)) kept.push_back(b);
    m_bursts = std::move(kept);
    m_origin = out.origin;
    rebuildGpu();
}

void GrassSystem::onRefresh(Physics::PhysicsEngine* physics) {
    const double now = grassClockNow();
    sweepFlat(now);                                          // pisadas vencidas: solo los brotes pisados
    if (now - m_lastWaterSweep < kWaterSweepS) return;       // el agua no necesita resolución por frame
    m_lastWaterSweep = now;
    const Haruka::Physics::IWorldProvider* water = physics ? physics->worldProvider() : nullptr;
    if (!water) return;
    sweepWater([water](const glm::dvec3& wp) { return water->waterSurfaceAt(wp); }, now);
}

int GrassSystem::flattenNear(const glm::dvec3& center, double radius) {
    return flattenAt(center, radius, grassClockNow() + kFlatDwellS);
}

// ── datos ─────────────────────────────────────────────────────────────────────────────────────

void GrassSystem::setBursts(std::vector<MassBurst> bursts, const glm::dvec3& planetC, double planetR,
                            const glm::dvec3& origin) {
    m_bursts = std::move(bursts);
    m_planetC = planetC; m_planetR = planetR; m_origin = origin;
    rebuildGpu();
}

uint64_t GrassSystem::elementCount() const {
    uint64_t n = 0;
    for (const MassBurst& b : m_bursts) n += b.count;
    return n;
}

glm::dvec3 GrassSystem::elementWorld(const MassBurst& b, uint32_t k) const {
    glm::vec3 u, v;
    massTangents(b.dir, u, v);
    const MassElement e = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
    return centerWorld(b) + glm::dvec3(u) * (double)e.offset.x + glm::dvec3(v) * (double)e.offset.y;
}

void GrassSystem::rebuildGpu() {
    m_gpu.resize(m_bursts.size());
    const glm::dvec3 originToCenter = m_planetC - m_origin;
    for (size_t i = 0; i < m_bursts.size(); ++i) {
        const MassBurst& b = m_bursts[i];
        MassBurstGpu& g = m_gpu[i];
        const glm::vec3 c = glm::vec3(originToCenter + glm::dvec3(b.dir) * (m_planetR + (double)b.heightM));
        g.a = glm::vec4(c, b.scaleMin);
        g.b = glm::vec4(b.dir, b.scaleMax);
        uint32_t cellBits; std::memcpy(&cellBits, &b.cellM, sizeof(cellBits));
        g.c = glm::uvec4(b.seed, std::min(b.count, kMassMaxCount), kMassNoState, cellBits);
    }
    m_dirty = true;   // los offsets de estado (`c.z`) los pone `upload`
}

void GrassSystem::upload(RHI::Device* dev) {
    if (!dev || m_bursts.empty()) return;
    if (!m_dirty && valid(m_burstSSBO) && valid(m_stateSSBO)) return;
    // Estado COMPACTO: solo los brotes tocados aportan palabras; el resto apunta a "sin estado".
    std::vector<uint32_t> words;
    for (size_t i = 0; i < m_bursts.size(); ++i) {
        const auto it = m_state.find(m_bursts[i].seed);
        if (it == m_state.end()) { m_gpu[i].c.z = kMassNoState; continue; }
        const uint32_t need = massStateWords(m_bursts[i].count);
        std::vector<uint32_t>& w = it->second.words;
        if (w.size() < need) w.resize(need, 0u);   // el brote creció (más matas cerca de la cámara)
        m_gpu[i].c.z = (uint32_t)words.size();
        words.insert(words.end(), w.begin(), w.begin() + need);
    }
    if (words.empty()) words.push_back(0u);   // un SSBO vacío no se puede atar
    auto put = [&](RHI::BufferHandle& buf, size_t& cap, const void* data, size_t bytes) {
        if (cap < bytes) {
            // El frame anterior ya terminó en la GPU (el device espera en endFrame).
            if (valid(buf)) dev->destroy(buf);
            cap = bytes + bytes / 2 + 1024;
            buf = dev->createBuffer(RHI::BufferUsage::Storage, cap, nullptr, RHI::BufferMemory::Dynamic);
        }
        if (valid(buf)) dev->updateBuffer(buf, 0, bytes, data);
    };
    put(m_burstSSBO, m_burstCap, m_gpu.data(), m_gpu.size() * sizeof(MassBurstGpu));
    put(m_stateSSBO, m_stateCap, words.data(), words.size() * sizeof(uint32_t));
    m_dirty = false;
}

// ── dibujo ────────────────────────────────────────────────────────────────────────────────────
//
// Un comando INDIRECTO por brote visible. El cull y el LOD son los de la base, por brote (centro +
// medio lado de celda + la mata más grande). Lejos se RALEA: un brote a nivel 1 dibuja la mitad de
// sus matas y a nivel 2 un cuarto — las primeras `k`, así que el subconjunto es estable (no
// parpadea al cambiar de nivel). `firstInstance` = índice del brote en el SSBO.
void GrassSystem::draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) {
    if (propsDisabled() || !m_enabled || !ctx || !f.camera || m_bursts.empty()) return;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount <= 0) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    HARUKA_PROFILE("scene.grass"); HARUKA_GPU_SCOPE("scene.grass");
    if (m_gpu.size() != m_bursts.size()) rebuildGpu();

    if (!valid(m_pso)) {
        const std::string vs = Renderer::Shader::baseDir() + "shaders/prop_burst.vert";
        const std::string fs = Renderer::Shader::baseDir() + "shaders/prop_inst.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        appendPropVertexLayout(pd.vertexLayout, /*withPart*/false);   // SIN stream de instancia
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_pso = dev->createPipeline(pd);
        if (!valid(m_pso)) { HARUKA_LOGE("GrassSystem", "no se pudo crear el pipeline de prop_burst"); return; }
    }
    upload(dev);

    // Umbrales históricos (90/25 px). La hierba no tiene aún controlador de carga propio: su coste
    // lo acota el raleo por nivel y el tope de matas por brote.
    const Cull cull = makeCull(f, 90.0f, 25.0f);
    const glm::vec3 originRel = glm::vec3(m_origin - glm::dvec3(f.camera->position));
    std::vector<float> cullRadius((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi) cullRadius[(size_t)pi] = propCullRadiusM(m_registry.prototype(pi).name);

    constexpr int kLods = PrototypeGpu::kLods;
    struct IndirectCmd { uint32_t indexCount, instanceCount, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
    static_assert(sizeof(IndirectCmd) == 20, "VkDrawIndexedIndirectCommand / DrawElementsIndirectCommand");
    std::vector<std::vector<IndirectCmd>> groups((size_t)(protoCount * kLods));
    uint64_t drawn = 0;
    for (uint32_t i = 0; i < (uint32_t)m_bursts.size(); ++i) {
        const MassBurst& b = m_bursts[i];
        if (b.proto < 0 || b.proto >= protoCount || b.count == 0) continue;
        const glm::vec3 posF = glm::vec3(m_gpu[i].a) + originRel;
        const float radius = 0.65f * b.cellM + cullRadius[(size_t)b.proto] * b.scaleMax;
        int lod = 0;
        if (cull(posF, radius, lod) != 0) continue;
        lod = std::clamp(lod, 0, kLods - 1);
        uint32_t n = std::min(b.count, kMassMaxCount);
        if (lod == 1) n = std::max(1u, (n + 1u) / 2u);
        else if (lod == 2) n = std::max(1u, (n + 3u) / 4u);
        groups[(size_t)(b.proto * kLods + lod)].push_back({ 0u, n, 0u, 0, i });
        drawn += n;
    }
    size_t total = 0;
    for (const auto& g : groups) total += g.size();

    if (total > 0) {
        const size_t bytes = total * sizeof(IndirectCmd);
        if (m_cmdCap < bytes) {
            // El frame anterior ya terminó en la GPU (el device espera en endFrame): se suelta el
            // viejo. Con holgura, para no reasignar cada frame.
            if (valid(m_cmdBuf)) dev->destroy(m_cmdBuf);
            m_cmdCap = bytes + bytes / 2 + 4096;
            m_cmdBuf = dev->createBuffer(RHI::BufferUsage::Indirect, m_cmdCap, nullptr, RHI::BufferMemory::Dynamic);
        }
        std::vector<IndirectCmd> all;
        all.reserve(total);
        std::vector<size_t> groupOff(groups.size(), 0);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            groupOff[gi] = all.size() * sizeof(IndirectCmd);
            if (groups[gi].empty()) continue;
            const PrototypeGpu* pg = prototypeGpu(m_registry.prototype((int)(gi / (size_t)kLods)));
            const uint32_t ic = pg ? pg->indexCount[gi % (size_t)kLods] : 0u;
            for (IndirectCmd c : groups[gi]) { c.indexCount = ic; all.push_back(c); }
        }
        // UNA subida por frame, ANTES de grabar: cada draw lee SU tramo (en Vulkan se ejecutan al
        // final del frame, y un buffer reescrito entre draws daría a todos el último).
        dev->updateBuffer(m_cmdBuf, 0, bytes, all.data());

        const glm::vec3 wind = frameWind(f);
        const float     time = frameTime();
        ctx->bindPipeline(m_pso);
        ctx->bindUniformBuffer(0, f.perFrameUBO);
        ctx->bindStorageBuffer(2, m_burstSSBO);
        ctx->bindStorageBuffer(3, m_stateSSBO);
        for (int pi = 0; pi < protoCount; ++pi) {
            bool any = false;
            for (int lod = 0; lod < kLods; ++lod) any = any || !groups[(size_t)(pi * kLods + lod)].empty();
            if (!any) continue;
            const PrototypeGpu* pgp = prototypeGpu(m_registry.prototype(pi));
            if (!pgp) continue;
            const PrototypeGpu& pg = *pgp;
            const PropParams pp = makeParams(pg, wind, time, originRel, f);
            const RHI::BufferHandle ubo = paramsUBO(dev, pi);
            if (valid(ubo)) { dev->updateBuffer(ubo, 0, sizeof(pp), &pp); ctx->bindUniformBuffer(6, ubo); }
            bindPrototypeMaterial(ctx, pg);
            for (int lod = 0; lod < kLods; ++lod) {
                const auto& g = groups[(size_t)(pi * kLods + lod)];
                if (g.empty() || !valid(pg.vbo[lod]) || pg.indexCount[lod] == 0) continue;
                ctx->bindVertexBuffer(pg.vbo[lod], 0);
                ctx->bindIndexBuffer(pg.ebo[lod]);
                ctx->drawIndexedIndirect(m_cmdBuf, (uint32_t)g.size(), (uint32_t)sizeof(IndirectCmd),
                                         groupOff[(size_t)(pi * kLods + lod)]);
                uint64_t inst = 0;
                for (const auto& c : g) inst += c.instanceCount;
                stats.renderedVertices  += (int)(pg.vertexCount[lod] * inst);
                stats.renderedTriangles += (int)(pg.indexCount[lod] / 3 * inst);
                ++stats.renderedDrawCalls;
            }
        }
    }
    if (valid(f.scenePSO)) ctx->bindPipeline(f.scenePSO);   // restaura el PSO de escena (el cierre lo asume)

    static double s_lastLog = -1e9;
    if (f.diagClock - s_lastLog > 2.0) {
        s_lastLog = f.diagClock;
        HARUKA_LOGDIAG("GrassSystem", "brotes=%zu matas=%llu -> visibles: %zu brotes, %llu matas · tocados=%zu",
                       m_bursts.size(), (unsigned long long)elementCount(), total,
                       (unsigned long long)drawn, m_state.size());
    }
}

// ── estado ────────────────────────────────────────────────────────────────────────────────────

int GrassSystem::setStateIn(const glm::dvec3& center, double radius, uint32_t fromMask, MassState to,
                          double flatUntil) {
    if (m_bursts.empty()) return 0;
    const double r2 = radius * radius;
    int changed = 0;
    for (const MassBurst& b : m_bursts) {
        // Descarte por brote: ninguna mata cae a más de ~0,64·celda del centro (90 % del lado, en diagonal).
        const glm::dvec3 cw = centerWorld(b);
        const double reach = radius + 0.65 * (double)b.cellM;
        const glm::dvec3 dc = cw - center;
        if (glm::dot(dc, dc) > reach * reach) continue;
        glm::vec3 u, v;
        massTangents(b.dir, u, v);
        StateEntry* entry = nullptr;
        if (auto it = m_state.find(b.seed); it != m_state.end()) entry = &it->second;
        const uint32_t need = massStateWords(b.count);
        for (uint32_t k = 0; k < b.count; ++k) {
            const MassElement e = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
            const glm::dvec3 p = cw + glm::dvec3(u) * (double)e.offset.x + glm::dvec3(v) * (double)e.offset.y;
            const glm::dvec3 d = p - center;
            if (glm::dot(d, d) > r2) continue;
            if (entry && entry->words.size() < need) entry->words.resize(need, 0u);
            const MassState cur = entry ? massGetState(entry->words, k) : MassState::Alive;
            if ((fromMask & (1u << (uint32_t)cur)) == 0u) continue;
            if (!entry) { entry = &m_state[b.seed]; entry->words.assign(need, 0u); }
            massSetState(entry->words, k, to);
            if (to == MassState::Flat) entry->flatUntil = flatUntil;
            ++changed;
        }
    }
    if (changed > 0) m_dirty = true;
    return changed;
}

int GrassSystem::destroyAt(const glm::dvec3& center, double radius) {
    return setStateIn(center, radius,
                      (1u << (uint32_t)MassState::Alive) | (1u << (uint32_t)MassState::Flat),
                      MassState::Destroyed, -1.0);
}

int GrassSystem::flattenAt(const glm::dvec3& center, double radius, double until) {
    return setStateIn(center, radius, 1u << (uint32_t)MassState::Alive, MassState::Flat, until);
}

GrassSystem::Hit GrassSystem::nearest(const glm::dvec3& center, double radius) const {
    Hit best;
    double bestD2 = radius * radius;
    for (const MassBurst& b : m_bursts) {
        const glm::dvec3 cw = centerWorld(b);
        const double reach = radius + 0.65 * (double)b.cellM;
        const glm::dvec3 dc = cw - center;
        if (glm::dot(dc, dc) > reach * reach) continue;
        const auto it = m_state.find(b.seed);
        glm::vec3 u, v;
        massTangents(b.dir, u, v);
        for (uint32_t k = 0; k < b.count; ++k) {
            if (it != m_state.end() && ((k * 2u) >> 5) < it->second.words.size()
                && massGetState(it->second.words, k) == MassState::Destroyed) continue;
            const MassElement e = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
            const glm::dvec3 p = cw + glm::dvec3(u) * (double)e.offset.x + glm::dvec3(v) * (double)e.offset.y;
            const glm::dvec3 d = p - center;
            const double d2 = glm::dot(d, d);
            if (d2 > bestD2) continue;
            bestD2 = d2;
            best.hit = true; best.seed = b.seed; best.element = k; best.proto = b.proto; best.pos = p;
        }
    }
    return best;
}

bool GrassSystem::destroyElement(uint32_t seed, uint32_t element) {
    for (const MassBurst& b : m_bursts) {
        if (b.seed != seed) continue;
        if (element >= b.count) return false;
        StateEntry& entry = m_state[seed];
        const uint32_t need = massStateWords(b.count);
        if (entry.words.size() < need) entry.words.resize(need, 0u);
        if (massGetState(entry.words, element) == MassState::Destroyed) return false;
        massSetState(entry.words, element, MassState::Destroyed);
        m_dirty = true;
        return true;
    }
    return false;
}

void GrassSystem::sweepFlat(double now) {
    bool changed = false;
    for (auto it = m_state.begin(); it != m_state.end(); ) {
        StateEntry& e = it->second;
        if (e.flatUntil >= 0.0 && now >= e.flatUntil) {
            // Flat (01) → Alive (00): se borra el bit bajo de los pares cuyo alto no está puesto.
            bool any = false;
            for (uint32_t& w : e.words) {
                const uint32_t flatBits = w & 0x55555555u & ~((w & 0xAAAAAAAAu) >> 1);
                w &= ~flatBits;
                any = any || w != 0u;
            }
            e.flatUntil = -1.0;
            changed = true;
            if (!any) { it = m_state.erase(it); continue; }   // intacto otra vez: no ocupa
        }
        ++it;
    }
    if (changed) m_dirty = true;
}

void GrassSystem::sweepWater(const std::function<double(const glm::dvec3&)>& waterSurfaceAt, double now) {
    if (!waterSurfaceAt || m_bursts.empty()) return;
    if (m_waterCursor >= m_bursts.size()) m_waterCursor = 0;   // la re-siembra cambió los brotes
    const auto t0 = std::chrono::steady_clock::now();
    size_t i = m_waterCursor;
    for (; i < m_bursts.size(); ++i) {
        if ((i & 63u) == 0 && i != m_waterCursor &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > kWaterSweepBudgetMs)
            break;
        const MassBurst& b = m_bursts[i];
        // ⚠️ COTA CONTRA COTA: `waterSurfaceAt` da metros sobre el radio, igual que `heightM`. La
        // versión de props comparaba con la Y del mundo (~3e6 m) y el agua nunca tumbó nada.
        if (waterSurfaceAt(centerWorld(b)) <= (double)b.heightM) continue;
        StateEntry& entry = m_state[b.seed];
        const uint32_t need = massStateWords(b.count);
        if (entry.words.size() < need) entry.words.resize(need, 0u);
        for (uint32_t k = 0; k < b.count; ++k)
            if (massGetState(entry.words, k) == MassState::Alive) massSetState(entry.words, k, MassState::Flat);
        entry.flatUntil = now + kFlatDwellS;   // sigue cubierta: el reloj se renueva cada vuelta
        m_dirty = true;
    }
    m_waterCursor = (i >= m_bursts.size()) ? 0 : i;
}

std::vector<GrassSystem::Delta> GrassSystem::serialize() const {
    std::vector<Delta> out;
    out.reserve(m_state.size());
    for (const auto& [seed, st] : m_state) {
        // Solo lo DESTRUIDO: la pisada es efímera. Se quita el bit bajo de cada par (Flat = 01) y
        // queda el alto (Destroyed = 10).
        Delta d; d.seed = seed; d.words = st.words;
        bool any = false;
        for (uint32_t& w : d.words) { w &= 0xAAAAAAAAu; any = any || w != 0u; }
        if (any) out.push_back(std::move(d));
    }
    return out;
}

void GrassSystem::restore(const std::vector<Delta>& deltas) {
    m_state.clear();
    for (const auto& d : deltas) m_state[d.seed].words = d.words;
    m_dirty = true;
}

} // namespace Haruka::World::Props
