/**
 * @file chunk_cache.cpp
 * @brief Implementation of LRU chunk cache.
 */

#include "chunk_cache.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <atomic>
#include "core/terrain/shared_index_table.h"

namespace Haruka {

ChunkCache::ChunkCache(size_t maxMemoryMB)
    : maxMemoryBytes(std::max<size_t>(16 * 1024 * 1024, maxMemoryMB * 1024 * 1024)),
      currentMemoryBytes(0) {}

const ChunkData* ChunkCache::getChunk(const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);
    auto it = cache.find(hash);

    if (it != cache.end()) {
        stats.hits++;
        updateLRUOrder(key);
        return &it->second.data;
    }

    stats.misses++;
    return nullptr;
}

bool ChunkCache::getChunkCopy(const PlanetChunkKey& key, ChunkData& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = cache.find(keyToHash(key));
    if (it == cache.end()) { stats.misses++; return false; }
    stats.hits++;
    updateLRUOrder(key);
    out = it->second.data;   // COPY under the lock → safe from concurrent rehash/evict
    return true;
}

void ChunkCache::addChunk(const PlanetChunkKey& key, const ChunkData& data) {
    // PERSISTIR AL GENERAR (no solo al evictar): el trabajo caro (compute + erosión + readback) se
    // hace UNA vez en la vida del mundo; luego recuperarlo es LEER.
    // ⚠️ EN SEGUNDO PLANO: hacerlo aquí, síncrono, metía ~500 KB de I/O por chunk en el hilo de
    // cosecha (pump.harvest) y hundía el frame.
    enqueueDiskWrite(key, data);

    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);

    // Tamaño REAL (todos los arrays: terreno + agua + uvs/morph). Antes contaba solo
    // vertices+normals+colors+indices → subestimaba ~2.6× → la caché retenía mucho más
    // que el cap y se comía toda la RAM. Fuente única: ChunkData::getSizeBytes().
    size_t chunkSizeBytes = data.getSizeBytes();

    auto existing = cache.find(hash);
    if (existing != cache.end()) {
        currentMemoryBytes -= existing->second.sizeBytes;
        existing->second.data = data;
        existing->second.sizeBytes = chunkSizeBytes;
        currentMemoryBytes += chunkSizeBytes;
        updateLRUOrder(key);
    } else {
        CacheEntry entry{data, chunkSizeBytes};
        cache[hash] = entry;
        // La PIRÁMIDE GRUESA (lod <= kPinnedLOD) NO entra en la lista LRU: nunca se evicta, así que
        // tenerla ahí solo servía para que evictLRU la RECORRIERA entera saltándosela — con ~6000
        // chunks pinneados al frente y la memoria al tope, cada evicción era O(n) y evictToFitMemory
        // O(n²) → PICOS DE 150 ms al girar (evicción en masa). Fuera de la lista: evictLRU vuelve a
        // ser O(1) y solo ve candidatos reales.
        if (key.lod > kPinnedLOD) {
            lruOrder.push_back(key);
            keyToIterator[hash] = std::prev(lruOrder.end());
        }
        currentMemoryBytes += chunkSizeBytes;
    }

    evictToFitMemory();
}

void ChunkCache::removeChunk(const PlanetChunkKey& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);

    auto it = cache.find(hash);
    if (it == cache.end()) return;

    currentMemoryBytes -= it->second.sizeBytes;

    auto iterIt = keyToIterator.find(hash);
    if (iterIt != keyToIterator.end()) {
        lruOrder.erase(iterIt->second);
        keyToIterator.erase(iterIt);
    }

    cache.erase(it);
}

bool ChunkCache::hasChunk(const PlanetChunkKey& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t hash = keyToHash(key);
    return cache.find(hash) != cache.end();
}

void ChunkCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    cache.clear();
    lruOrder.clear();
    keyToIterator.clear();
    currentMemoryBytes = 0;
}

void ChunkCache::setMaxMemory(size_t newMaxMemoryMB) {
    std::lock_guard<std::mutex> lock(m_mutex);
    maxMemoryBytes = std::max<size_t>(16 * 1024 * 1024, newMaxMemoryMB * 1024 * 1024);
    evictToFitMemory();
}

uint64_t ChunkCache::keyToHash(const PlanetChunkKey& key) {
    // Empaquetado EXACTO (es identidad, no un hash con colisiones): face(3) | lod(5) |
    // x(23) | y(23) | body(10) = 64 bits. x/y de 23 bits cubren hasta LOD 23 (antes 12
    // bits → colisión a LOD>12). body en los 10 bits altos → cuerpos nunca colisionan.
    uint64_t h = 0;
    h |=  (static_cast<uint64_t>(key.face) & 0x7);
    h |= ((static_cast<uint64_t>(key.lod)  & 0x1F)     << 3);
    h |= ((static_cast<uint64_t>(key.x)    & 0x7FFFFF) << 8);
    h |= ((static_cast<uint64_t>(key.y)    & 0x7FFFFF) << 31);
    h |= ((static_cast<uint64_t>(key.body) & 0x3FF)    << 54);
    return h;
}


// ===== CACHÉ EN DISCO ======================================================================
// Generar un chunk cuesta ~5-7 ms (compute + erosión + readback). Con la caché de RAM al tope, cada
// evicción condena a REGENERAR → thrashing (el terreno desaparece y vuelve). Aquí evictar = escribir
// y fallar = leer: la regeneración pasa de recomputar a cargar (~0.5 ms).
//
// Formato: crudo, sin comprimir. El chunk YA está empaquetado (normales en 4 B, uv en half) y el
// cuello es CPU, no disco — comprimir añadiría coste de CPU justo donde duele.
namespace {
    struct DiskHeader {
        uint32_t magic = 0x484B4331;   // 'HKC1'
        uint32_t vertexCount = 0, indexCount = 0, waterVerts = 0, waterIdx = 0;
        double   planetRadius = 0.0;
        double   cx = 0.0, cy = 0.0, cz = 0.0;   // chunkCenter
        uint8_t  hasOcean = 0, pad[7] = {0};
    };
    template <typename T> void wr(std::ofstream& o, const std::vector<T>& v) {
        o.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(v.size() * sizeof(T)));
    }
    template <typename T> void rd(std::ifstream& i, std::vector<T>& v, size_t n) {
        v.resize(n);
        if (n) i.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(n * sizeof(T)));
    }
}

// TOPE de la caché de disco. Sin esto crece SIN LÍMITE conforme exploras (medido: 3.7 GB tras dos
// sesiones cortas) y acaba llenando el disco. Al pasarse, borra los ficheros más ANTIGUOS (LRU por
// mtime): el chunk que no visitas hace tiempo es justo el que menos duele recomputar.
static constexpr uint64_t kDiskCapBytes = 4ull * 1024 * 1024 * 1024;   // 4 GB

static void pruneDiskCache(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec || !e.is_regular_file(ec)) continue;
        const auto sz = e.file_size(ec);
        if (ec) continue;
        total += sz;
        files.emplace_back(e.last_write_time(ec), e.path());
    }
    if (total <= kDiskCapBytes) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [t, p] : files) {
        if (total <= kDiskCapBytes) break;
        const auto sz = fs::file_size(p, ec);
        if (!ec && fs::remove(p, ec)) total -= sz;
    }
}

ChunkCache::~ChunkCache() {
    m_diskStop = true;
    m_diskCv.notify_all();
    if (m_diskThread.joinable()) m_diskThread.join();
}

void ChunkCache::enqueueDiskWrite(const PlanetChunkKey& key, const ChunkData& d) {
    if (m_diskDir.empty() || d.vertices.empty()) return;
    {
        std::lock_guard<std::mutex> lk(m_diskMx);
        if (m_diskQueue.size() >= 48) return;   // cola llena → se descarta (best-effort, ver header)
        m_diskQueue.push_back({ key, d });
    }
    m_diskCv.notify_one();
}

void ChunkCache::diskWriterLoop() {
    for (;;) {
        DiskJob job;
        {
            std::unique_lock<std::mutex> lk(m_diskMx);
            m_diskCv.wait(lk, [&] { return m_diskStop || !m_diskQueue.empty(); });
            if (m_diskStop) return;
            job = std::move(m_diskQueue.front());
            m_diskQueue.pop_front();
        }
        saveToDisk(job.key, job.data);
    }
}

void ChunkCache::setDiskCache(const std::string& dir, uint32_t seed) {
    if (dir.empty()) { m_diskDir.clear(); return; }
    // Separado POR SEED: un mundo nuevo no puede leer los chunks del anterior (la generación cambió
    // → serían de otro planeta). Es la trampa obvia de una caché de disco.
    // Directorio = seed + VERSIÓN DE GENERACIÓN. Sin la versión, al cambiar la fórmula del terreno
    // se seguían leyendo chunks del mundo anterior (ver kGenVersion).
    m_diskDir = dir + "/chunks_" + std::to_string(seed) + "_v" + std::to_string(kGenVersion);
    std::error_code ec;
    std::filesystem::create_directories(m_diskDir, ec);
    if (ec) { m_diskDir.clear(); return; }
    pruneDiskCache(m_diskDir);   // al arrancar: si la sesión anterior se pasó del tope, poda.
    if (!m_diskThread.joinable()) m_diskThread = std::thread(&ChunkCache::diskWriterLoop, this);
}

static std::string chunkPath(const std::string& dir, const PlanetChunkKey& k) {
    return dir + "/" + std::to_string(ChunkCache::keyToHash(k)) + ".chk";
}

// Los ÍNDICES no van en el chunk: son topología COMPARTIDA por resolución (SharedIndexTable), y la
// rellena el GENERADOR. Si un chunk se carga de disco, el generador NO corre → la tabla se queda
// vacía → el chunk se sube con 0 índices y **no se dibuja nada** (pasó: pantalla vacía). Así que la
// tabla también se persiste, una vez por topología.
static std::string idxPath(const std::string& dir, uint32_t indexCount) {
    return dir + "/idx_" + std::to_string(indexCount) + ".bin";
}
static void saveIndexTable(const std::string& dir, uint32_t indexCount) {
    if (indexCount == 0) return;
    const std::string p = idxPath(dir, indexCount);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return;
    const std::vector<unsigned int>* idx = SharedIndexTable::get().find(indexCount);
    if (!idx || idx->empty()) return;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (o) o.write(reinterpret_cast<const char*>(idx->data()),
                   (std::streamsize)(idx->size() * sizeof(unsigned int)));
}
static bool ensureIndexTable(const std::string& dir, uint32_t indexCount) {
    if (indexCount == 0) return false;
    if (SharedIndexTable::get().find(indexCount)) return true;      // ya registrada
    std::ifstream i(idxPath(dir, indexCount), std::ios::binary);
    if (!i) return false;                                            // no está → regenerar el chunk
    std::vector<unsigned int> idx(indexCount);
    i.read(reinterpret_cast<char*>(idx.data()), (std::streamsize)(indexCount * sizeof(unsigned int)));
    if (!i && !i.eof()) return false;
    SharedIndexTable::get().registerOnce(indexCount, idx);
    return true;
}

void ChunkCache::saveToDisk(const PlanetChunkKey& key, const ChunkData& d) const {
    if (m_diskDir.empty() || d.vertices.empty()) return;
    saveIndexTable(m_diskDir, d.indexCount);   // topología compartida (una vez por resolución)
    const std::string path = chunkPath(m_diskDir, key);
    // Ya está en disco → no reescribir (el contenido es determinista por seed).
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return;
    DiskHeader h;
    h.vertexCount  = (uint32_t)d.vertices.size();
    h.indexCount   = d.indexCount;
    h.waterVerts   = (uint32_t)d.waterVertices.size();
    h.waterIdx     = (uint32_t)d.waterIndices.size();
    h.planetRadius = d.planetRadius;
    h.cx = d.chunkCenter.x; h.cy = d.chunkCenter.y; h.cz = d.chunkCenter.z;
    h.hasOcean = d.hasOcean ? 1 : 0;
    o.write(reinterpret_cast<const char*>(&h), sizeof(h));
    // Poda amortizada: cada 1024 escrituras comprobamos el tope (recorrer el directorio no es gratis).
    static std::atomic<int> s_writes{0};
    if ((s_writes.fetch_add(1) % 1024) == 1023) pruneDiskCache(m_diskDir);
    wr(o, d.vertices); wr(o, d.morphTargets);
    wr(o, d.normalsPacked); wr(o, d.morphNormalsPacked); wr(o, d.uvsPacked);
    wr(o, d.waterVertices); wr(o, d.waterMorphTargets); wr(o, d.waterNormals);
    wr(o, d.waterParams);   wr(o, d.waterIndices);
}

bool ChunkCache::loadFromDisk(const PlanetChunkKey& key, ChunkData& d) const {
    if (m_diskDir.empty()) return false;
    std::ifstream i(chunkPath(m_diskDir, key), std::ios::binary);
    if (!i) return false;
    DiskHeader h;
    i.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!i || h.magic != 0x484B4331u || h.vertexCount == 0) return false;

    const size_t v = h.vertexCount;
    rd(i, d.vertices, v); rd(i, d.morphTargets, v);
    rd(i, d.normalsPacked, v); rd(i, d.morphNormalsPacked, v); rd(i, d.uvsPacked, v);
    rd(i, d.waterVertices, h.waterVerts); rd(i, d.waterMorphTargets, h.waterVerts);
    rd(i, d.waterNormals, h.waterVerts);  rd(i, d.waterParams, h.waterVerts);
    rd(i, d.waterIndices, h.waterIdx);
    if (!i && !i.eof()) return false;

    // Sin la topología no se puede mallar → mejor REGENERAR el chunk que subir uno invisible.
    if (!ensureIndexTable(m_diskDir, h.indexCount)) return false;

    d.indexCount   = h.indexCount;
    d.planetRadius = h.planetRadius;
    d.chunkCenter  = glm::dvec3(h.cx, h.cy, h.cz);
    d.hasOcean     = h.hasOcean != 0;
    d.key          = key;
    return true;
}

bool ChunkCache::evictLRU() {
    if (lruOrder.empty()) return false;

    // La PIRÁMIDE GRUESA (lod <= kPinnedLOD) ni siquiera está en esta lista (ver addChunk): es el
    // ÚNICO fallback del render —sin ancestro residente, un chunk fino que no llega deja un AGUJERO—
    // y por eso nunca se evicta. Aquí, por tanto, todo lo que hay es evictable → O(1), sin escanear.
    PlanetChunkKey lruKey = lruOrder.front();
    lruOrder.pop_front();

    uint64_t hash = keyToHash(lruKey);
    auto it = cache.find(hash);
    if (it != cache.end()) {
        enqueueDiskWrite(lruKey, it->second.data);   // evictar = escribir (en segundo plano)
        currentMemoryBytes -= it->second.sizeBytes;
        cache.erase(it);
        stats.evictions++;
    }

    keyToIterator.erase(hash);
    return true;
}

void ChunkCache::evictToFitMemory() {
    while (currentMemoryBytes > maxMemoryBytes && !lruOrder.empty()) {
        evictLRU();
    }
}

void ChunkCache::touchMany(const std::vector<PlanetChunkKey>& keys) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& k : keys) {
        if (cache.find(keyToHash(k)) == cache.end()) continue;
        stats.hits++;
        updateLRUOrder(k);
    }
}

void ChunkCache::updateLRUOrder(const PlanetChunkKey& key) {
    uint64_t hash = keyToHash(key);

    auto iterIt = keyToIterator.find(hash);
    if (iterIt == keyToIterator.end()) return;

    auto iter = iterIt->second;
    lruOrder.erase(iter);
    lruOrder.push_back(key);
    keyToIterator[hash] = std::prev(lruOrder.end());
}

} // namespace Haruka
