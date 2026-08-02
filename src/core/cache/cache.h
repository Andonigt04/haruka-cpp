#pragma once

#include "memory_budget.h"
#include <unordered_map>
#include <list>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>

namespace Haruka { namespace Cache {

template<typename K, typename V>
class Cache {
public:
    using HashFunc = std::function<size_t(const K&)>;
    using SizeFunc = std::function<size_t(const V&)>;
    using PinFunc  = std::function<bool(const K&)>;
    using DiskSaveFunc = std::function<bool(const K&, const V&)>;
    using DiskLoadFunc = std::function<bool(const K&, V&)>;

    explicit Cache(HashFunc hashFn, SizeFunc sizeFn, size_t maxMemoryMB = 128)
        : m_hash(std::move(hashFn))
        , m_sizeOf(std::move(sizeFn))
        , m_budget(std::max<size_t>(16 * 1024 * 1024, maxMemoryMB * 1024 * 1024))
    {}

    ~Cache() {
        m_diskStop = true;
        m_diskCv.notify_all();
        if (m_diskThread.joinable()) m_diskThread.join();
    }

    const V* get(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t h = m_hash(key);
        auto it = m_map.find(h);
        if (it != m_map.end()) {
            m_stats.hits++;
            touch(h, key);
            return &it->second.value;
        }
        m_stats.misses++;
        return nullptr;
    }

    bool getCopy(const K& key, V& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t h = m_hash(key);
        auto it = m_map.find(h);
        if (it == m_map.end()) { m_stats.misses++; return false; }
        m_stats.hits++;
        touch(h, key);
        out = it->second.value;
        return true;
    }

    void put(const K& key, const V& value) {
        if (m_diskSave && !m_diskDir.empty()) {
            enqueueDiskWrite(key, value);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        size_t h = m_hash(key);
        size_t sz = m_sizeOf(value);
        bool pinned = m_pinFunc && m_pinFunc(key);

        auto existing = m_map.find(h);
        if (existing != m_map.end()) {
            m_budget.deallocate(existing->second.sizeBytes);
            if (pinned) m_pinnedBytes -= existing->second.sizeBytes;
            existing->second.value = value;
            existing->second.sizeBytes = sz;
            if (pinned) m_pinnedBytes += sz;
            m_budget.allocate(sz);
            touch(h, key);
        } else {
            Entry entry;
            entry.key = key;
            entry.value = value;
            entry.sizeBytes = sz;
            m_map[h] = std::move(entry);
            if (!pinned) {
                m_lruOrder.push_back(key);
                m_keyToIter[h] = std::prev(m_lruOrder.end());
            } else {
                m_pinnedBytes += sz;
            }
            m_budget.allocate(sz);
        }
        evictToFitMemory();
    }

    void remove(const K& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t h = m_hash(key);
        auto it = m_map.find(h);
        if (it == m_map.end()) return;

        m_budget.deallocate(it->second.sizeBytes);
        if (m_pinFunc && m_pinFunc(key)) m_pinnedBytes -= it->second.sizeBytes;

        auto iterIt = m_keyToIter.find(h);
        if (iterIt != m_keyToIter.end()) {
            m_lruOrder.erase(iterIt->second);
            m_keyToIter.erase(iterIt);
        }
        m_map.erase(it);
    }

    bool has(const K& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_map.find(m_hash(key)) != m_map.end();
    }

    void touchMany(const std::vector<K>& keys) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& k : keys) {
            size_t h = m_hash(k);
            if (m_map.find(h) == m_map.end()) continue;
            m_stats.hits++;
            touch(h, k);
        }
    }

    void setPinFunc(PinFunc func) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pinFunc = std::move(func);
    }

    void setDiskCache(const std::string& dir,
                      DiskSaveFunc saveFn, DiskLoadFunc loadFn) {
        m_diskDir = dir;
        m_diskSave = std::move(saveFn);
        m_diskLoad = std::move(loadFn);
        if (!m_diskDir.empty() && !m_diskThread.joinable()) {
            m_diskThread = std::thread(&Cache::diskWriterLoop, this);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_map.clear();
        m_lruOrder.clear();
        m_keyToIter.clear();
        m_budget.resetUsage();
        m_pinnedBytes = 0;
        m_stats = {};
    }

    const std::string& diskDir() const { return m_diskDir; }

    template<typename F>
    void forEach(F&& f) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [h, entry] : m_map) {
            f(entry.key, entry.value);
        }
    }

    void recalculatePins() {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t totalPinned = 0;
        for (auto& [h, entry] : m_map) {
            bool pinned = m_pinFunc && m_pinFunc(entry.key);
            auto iterIt = m_keyToIter.find(h);
            bool inLRU = (iterIt != m_keyToIter.end());
            if (pinned && inLRU) {
                m_lruOrder.erase(iterIt->second);
                m_keyToIter.erase(iterIt);
            } else if (!pinned && !inLRU) {
                m_lruOrder.push_back(entry.key);
                m_keyToIter[h] = std::prev(m_lruOrder.end());
            }
            if (pinned) totalPinned += entry.sizeBytes;
        }
        m_pinnedBytes = totalPinned;
    }

    size_t size() const { std::lock_guard<std::mutex> l(m_mutex); return m_map.size(); }
    size_t memoryBytes() const { return m_budget.used(); }
    size_t pinnedBytes() const { std::lock_guard<std::mutex> l(m_mutex); return m_pinnedBytes; }
    size_t maxBytes() const { return m_budget.budget(); }
    size_t maxMB() const { return m_budget.budget() / (1024 * 1024); }

    void setMaxMemory(size_t mb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_budget.setBudget(std::max<size_t>(16 * 1024 * 1024, mb * 1024 * 1024));
        evictToFitMemory();
    }

    struct Stats {
        size_t hits = 0, misses = 0, evictions = 0;
        float hitRate() const {
            size_t total = hits + misses;
            return total == 0 ? 0.0f : float(hits) / float(total);
        }
    };

    Stats getStats() const { std::lock_guard<std::mutex> l(m_mutex); return m_stats; }
    void resetStats() { std::lock_guard<std::mutex> l(m_mutex); m_stats = {}; }

private:
    struct Entry {
        K key;
        V value;
        size_t sizeBytes = 0;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<size_t, Entry> m_map;
    std::list<K> m_lruOrder;
    std::unordered_map<size_t, typename std::list<K>::iterator> m_keyToIter;
    size_t m_pinnedBytes = 0;
    Stats m_stats;

    HashFunc m_hash;
    SizeFunc m_sizeOf;
    PinFunc  m_pinFunc;
    MemoryBudget m_budget;

    void touch(size_t h, const K& key) {
        auto it = m_keyToIter.find(h);
        if (it == m_keyToIter.end()) return;
        m_lruOrder.erase(it->second);
        m_lruOrder.push_back(key);
        m_keyToIter[h] = std::prev(m_lruOrder.end());
    }

    bool evictOne() {
        if (m_lruOrder.empty()) return false;
        K lruKey = std::move(m_lruOrder.front());
        m_lruOrder.pop_front();
        size_t h = m_hash(lruKey);
        auto it = m_map.find(h);
        if (it != m_map.end()) {
            if (m_diskSave && !m_diskDir.empty()) {
                enqueueDiskWrite(lruKey, it->second.value);
            }
            m_budget.deallocate(it->second.sizeBytes);
            m_map.erase(it);
            m_stats.evictions++;
        }
        m_keyToIter.erase(h);
        return true;
    }

    void evictToFitMemory() {
        while (m_budget.isOverBudget() && !m_lruOrder.empty()) {
            evictOne();
        }
    }

    struct DiskJob { K key; V value; };
    std::string m_diskDir;
    DiskSaveFunc m_diskSave;
    DiskLoadFunc m_diskLoad;
    std::deque<DiskJob> m_diskQueue;
    mutable std::mutex m_diskMx;
    std::condition_variable m_diskCv;
    std::thread m_diskThread;
    std::atomic<bool> m_diskStop{false};

    void enqueueDiskWrite(const K& key, const V& value) {
        if (m_diskDir.empty() || !m_diskSave) return;
        {
            std::lock_guard<std::mutex> lk(m_diskMx);
            if (m_diskQueue.size() >= 48) return;
            m_diskQueue.push_back({key, value});
        }
        m_diskCv.notify_one();
    }

    void diskWriterLoop() {
        for (;;) {
            DiskJob job;
            {
                std::unique_lock<std::mutex> lk(m_diskMx);
                m_diskCv.wait(lk, [&] { return m_diskStop || !m_diskQueue.empty(); });
                if (m_diskStop) return;
                job = std::move(m_diskQueue.front());
                m_diskQueue.pop_front();
            }
            if (m_diskSave) m_diskSave(job.key, job.value);
        }
    }
};

}} // namespace Haruka::Cache