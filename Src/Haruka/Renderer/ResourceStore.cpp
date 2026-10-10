#include "Haruka/Renderer/ResourceStore.h"
#include "Haruka/RHI/RHIDevice.h"

#include <algorithm>
#include <chrono>

namespace Haruka::Renderer {

ResourceStore::ResourceStore(Destroyer destroyer, uint32_t framesInFlight)
    : m_destroyer(std::move(destroyer)), m_framesInFlight(framesInFlight) {}

ResourceStore::~ResourceStore() {
    for (auto& [id, e] : m_entries) if (!e.gpu.empty()) m_destroyer(e.gpu);
    for (auto& g : m_graveyard) m_destroyer(g.set);
}

ResourceId ResourceStore::create(std::string name, bool keepCpu) {
    std::lock_guard lk(m_mutex);
    const ResourceId id = m_nextId++;
    Entry& e = m_entries[id];
    e.name = std::move(name);
    e.keepCpu = keepCpu;
    return id;
}

uint64_t ResourceStore::publishErased(ResourceId id, std::shared_ptr<const void> data,
                                      std::type_index type, Uploader up) {
    std::lock_guard lk(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return 0;
    Entry& e = it->second;
    // Si ya había una subida pendiente, esta la SUSTITUYE: la GPU solo necesita la última versión.
    const bool wasPending = e.cpuRevision != e.gpuRevision;
    e.cpu = std::move(data);
    e.type = type;
    e.uploader = std::move(up);
    ++e.cpuRevision;
    if (!wasPending) m_pending.push_back(id);
    return e.cpuRevision;
}

std::shared_ptr<const void> ResourceStore::cpuErased(ResourceId id, std::type_index type,
                                                     uint64_t* rev) const {
    std::lock_guard lk(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end() || it->second.type != type) return nullptr;
    if (rev) *rev = it->second.cpuRevision;
    return it->second.cpu;
}

const GpuSet* ResourceStore::gpu(ResourceId id, uint64_t* revision) const {
    std::lock_guard lk(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return nullptr;
    if (revision) *revision = it->second.gpuRevision;
    return &it->second.gpu;
}

void ResourceStore::bury(GpuSet set) {
    if (set.empty()) return;
    m_graveyard.push_back({ std::move(set), m_frame });
}

size_t ResourceStore::processUploads(double budgetMs) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    size_t done = 0;
    for (;;) {
        if (done > 0 && std::chrono::duration<double, std::milli>(clock::now() - t0).count() >= budgetMs)
            break;
        // Se toma el trabajo BAJO el candado y se sube SIN él: subir puede tardar milisegundos y un
        // `publish` de otro hilo no tiene por qué esperarlo.
        ResourceId id = 0;
        std::shared_ptr<const void> data;
        Uploader up;
        uint64_t rev = 0;
        {
            std::lock_guard lk(m_mutex);
            while (!m_pending.empty()) {
                id = m_pending.front();
                m_pending.erase(m_pending.begin());
                auto it = m_entries.find(id);
                if (it != m_entries.end() && it->second.cpuRevision != it->second.gpuRevision) {
                    data = it->second.cpu;
                    up = it->second.uploader;
                    rev = it->second.cpuRevision;
                    break;
                }
                id = 0;   // soltado o ya al día: siguiente
            }
        }
        if (!id) break;
        GpuSet fresh = up ? up(data.get()) : GpuSet{};
        ++m_uploads;
        ++done;
        std::lock_guard lk(m_mutex);
        auto it = m_entries.find(id);
        if (it == m_entries.end()) { bury(std::move(fresh)); continue; }   // lo soltaron mientras
        Entry& e = it->second;
        bury(std::move(e.gpu));            // la versión vieja puede estar en un frame en vuelo
        e.gpu = std::move(fresh);
        e.gpuRevision = rev;
        // Si mientras se subía llegó otra revisión, vuelve a la cola: la subida de ahora ya es vieja.
        if (e.cpuRevision != e.gpuRevision) m_pending.push_back(id);
        else if (!e.keepCpu) e.cpu.reset();
    }
    return done;
}

void ResourceStore::beginFrame() {
    ++m_frame;
    auto safe = [this](const Grave& g) { return m_frame >= g.frame + m_framesInFlight; };
    for (const Grave& g : m_graveyard) if (safe(g)) { m_destroyer(g.set); ++m_destroyed; }
    std::erase_if(m_graveyard, safe);
}

void ResourceStore::release(ResourceId id) {
    std::lock_guard lk(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return;
    bury(std::move(it->second.gpu));
    m_entries.erase(it);
}

void ResourceStore::clear() {
    std::lock_guard lk(m_mutex);
    for (auto& [id, e] : m_entries) if (!e.gpu.empty()) { m_destroyer(e.gpu); ++m_destroyed; }
    for (auto& g : m_graveyard) { m_destroyer(g.set); ++m_destroyed; }
    m_entries.clear();
    m_graveyard.clear();
    m_pending.clear();
}

ResourceStats ResourceStore::stats() const {
    std::lock_guard lk(m_mutex);
    ResourceStats s;
    s.resources = m_entries.size();
    for (const auto& [id, e] : m_entries) if (e.cpuRevision != e.gpuRevision) ++s.pendingUploads;
    s.graveyard = m_graveyard.size();
    s.uploads = m_uploads;
    s.destroyed = m_destroyed;
    return s;
}

ResourceStore& resources() {
    static ResourceStore s([](const GpuSet& g) {
        RHI::Device* dev = RHI::device();
        if (!dev) return;   // ya cerrado: el barrido de pools del device liberó lo suyo
        for (RHI::BufferHandle b : g.buffers)   if (RHI::valid(b)) dev->destroy(b);
        for (RHI::TextureHandle t : g.textures) if (RHI::valid(t)) dev->destroy(t);
    }, kFramesInFlight);
    return s;
}

} // namespace Haruka::Renderer
