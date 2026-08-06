#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#include <cstddef>

namespace Haruka { namespace Tools {

template<typename T>
class TileAtlas {
public:
    TileAtlas() = default;

    TileAtlas(int maxTiles, int tileWidth, int tileHeight)
        : m_maxTiles(maxTiles)
        , m_tileW(tileWidth), m_tileH(tileHeight)
        , m_atlas(maxTiles * tileWidth * tileHeight)
        , m_slots(maxTiles)
    {}

    int maxTiles() const { return m_maxTiles; }
    int tileWidth() const { return m_tileW; }
    int tileHeight() const { return m_tileH; }
    int tileSize() const { return m_tileW * m_tileH; }

    T* data() { return m_atlas.data(); }
    const T* data() const { return m_atlas.data(); }
    int dataSize() const { return static_cast<int>(m_atlas.size()); }

    T* tile(int slot) {
        return m_atlas.data() + slot * tileSize();
    }

    const T* tile(int slot) const {
        return m_atlas.data() + slot * tileSize();
    }

    T& at(int slot, int x, int y) {
        return m_atlas[slot * tileSize() + y * m_tileW + x];
    }

    const T& at(int slot, int x, int y) const {
        return m_atlas[slot * tileSize() + y * m_tileW + x];
    }

    int acquireSlot() {
        if (m_maxTiles == 0) return -1;

        int slot = findFreeSlot();
        if (slot >= 0) {
            m_slots[slot].timestamp = ++m_clock;
            return slot;
        }

        slot = findLRUSlot();
        if (m_slots[slot].key != 0) {
            m_hash.erase(m_slots[slot].key);
            m_slots[slot].key = 0;
        }
        m_slots[slot].timestamp = ++m_clock;
        return slot;
    }

    void markUsed(int slot) {
        if (slot >= 0 && slot < m_maxTiles) {
            m_slots[slot].timestamp = ++m_clock;
        }
    }

    // Optional hash table mapping
    void setMapping(uint64_t key, int slot) {
        if (slot < 0 || slot >= m_maxTiles) return;
        // Clear old mapping if slot had a different key
        if (m_slots[slot].key != 0 && m_slots[slot].key != key) {
            m_hash.erase(m_slots[slot].key);
        }
        // Clear old slot if key was mapped elsewhere
        auto it = m_hash.find(key);
        if (it != m_hash.end() && it->second != slot) {
            m_slots[it->second].key = 0;
        }
        m_hash[key] = slot;
        m_slots[slot].key = key;
        m_slots[slot].timestamp = ++m_clock;
    }

    int findSlot(uint64_t key) const {
        auto it = m_hash.find(key);
        return it != m_hash.end() ? it->second : -1;
    }

    bool hasMapping(uint64_t key) const {
        return m_hash.find(key) != m_hash.end();
    }

    void removeMapping(uint64_t key) {
        auto it = m_hash.find(key);
        if (it != m_hash.end()) {
            if (it->second >= 0 && it->second < m_maxTiles) {
                m_slots[it->second].key = 0;
            }
            m_hash.erase(it);
        }
    }

    void clear() {
        std::fill(m_slots.begin(), m_slots.end(), SlotInfo{});
        m_hash.clear();
        m_clock = 0;
    }

    int usedSlots() const {
        int count = 0;
        for (const auto& s : m_slots) {
            if (s.key != 0) ++count;
        }
        return count;
    }

    int freeSlots() const {
        return m_maxTiles - usedSlots();
    }

private:
    struct SlotInfo {
        uint64_t key = 0;
        uint64_t timestamp = 0;
    };

    int findFreeSlot() const {
        for (int i = 0; i < m_maxTiles; i++) {
            if (m_slots[i].key == 0) return i;
        }
        return -1;
    }

    int findLRUSlot() const {
        int oldest = 0;
        for (int i = 1; i < m_maxTiles; i++) {
            if (m_slots[i].timestamp < m_slots[oldest].timestamp)
                oldest = i;
        }
        return oldest;
    }

    int m_maxTiles = 0;
    int m_tileW = 0, m_tileH = 0;
    std::vector<T> m_atlas;
    std::vector<SlotInfo> m_slots;
    std::unordered_map<uint64_t, int> m_hash;
    uint64_t m_clock = 0;
};

}} // namespace Haruka::Tools