#pragma once

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <functional>

namespace Haruka { namespace Tools {

template<typename T, typename Key = int64_t>
class SpatialHash {
public:
    using CellList = std::vector<T>;

    void insert(Key cellKey, const T& value) {
        m_cells[cellKey].push_back(value);
    }

    void insert(Key cellKey, T&& value) {
        m_cells[cellKey].push_back(std::move(value));
    }

    template<typename Pred>
    void remove(Key cellKey, Pred&& pred) {
        auto it = m_cells.find(cellKey);
        if (it == m_cells.end()) return;
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), std::forward<Pred>(pred)), list.end());
        if (list.empty()) m_cells.erase(it);
    }

    void removeAll(Key cellKey) {
        m_cells.erase(cellKey);
    }

    CellList* find(Key cellKey) {
        auto it = m_cells.find(cellKey);
        return it != m_cells.end() ? &it->second : nullptr;
    }

    const CellList* find(Key cellKey) const {
        auto it = m_cells.find(cellKey);
        return it != m_cells.end() ? &it->second : nullptr;
    }

    bool contains(Key cellKey) const {
        return m_cells.find(cellKey) != m_cells.end();
    }

    template<typename F>
    void queryAABB(Key minKey, Key maxKey, F&& callback) const {
        for (const auto& [key, list] : m_cells) {
            if (key >= minKey && key <= maxKey) {
                for (const auto& value : list) {
                    callback(key, value);
                }
            }
        }
    }

    template<typename F>
    void forEach(F&& callback) const {
        for (const auto& [key, list] : m_cells) {
            for (const auto& value : list) {
                callback(key, value);
            }
        }
    }

    void clear() { m_cells.clear(); }

    size_t cellCount() const { return m_cells.size(); }
    bool empty() const { return m_cells.empty(); }

    static int64_t pack(int x, int y, int z) {
        return (static_cast<int64_t>(x) & 0x1FFFFF) |
               ((static_cast<int64_t>(y) & 0x1FFFFF) << 21) |
               ((static_cast<int64_t>(z) & 0x1FFFFF) << 42);
    }

private:
    std::unordered_map<Key, CellList> m_cells;
};

}} // namespace Haruka::Tools