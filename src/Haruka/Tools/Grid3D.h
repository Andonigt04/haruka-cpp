#pragma once

#include <vector>
#include <cstddef>
#include <algorithm>

namespace Haruka { namespace Tools {

template<typename T>
class Grid3D {
public:
    Grid3D() = default;

    Grid3D(int width, int height, int depth, const T& fill = T{})
        : m_width(width), m_height(height), m_depth(depth)
        , m_data(width * height * depth, fill)
    {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    int depth() const { return m_depth; }
    int size() const { return static_cast<int>(m_data.size()); }

    T& at(int x, int y, int z) {
        return m_data[index(x, y, z)];
    }

    const T& at(int x, int y, int z) const {
        return m_data[index(x, y, z)];
    }

    T& operator()(int x, int y, int z) {
        return m_data[z * m_width * m_height + y * m_width + x];
    }

    const T& operator()(int x, int y, int z) const {
        return m_data[z * m_width * m_height + y * m_width + x];
    }

    T* data() { return m_data.data(); }
    const T* data() const { return m_data.data(); }

    T* slice(int z) { return m_data.data() + z * m_width * m_height; }
    const T* slice(int z) const { return m_data.data() + z * m_width * m_height; }

    void resize(int width, int height, int depth, const T& fill = T{}) {
        m_width = width;
        m_height = height;
        m_depth = depth;
        m_data.assign(width * height * depth, fill);
    }

    void fill(const T& value) {
        std::fill(m_data.begin(), m_data.end(), value);
    }

    bool empty() const { return m_data.empty(); }

    auto begin() { return m_data.begin(); }
    auto end() { return m_data.end(); }
    auto begin() const { return m_data.begin(); }
    auto end() const { return m_data.end(); }

private:
    int index(int x, int y, int z) const {
        return z * m_width * m_height + y * m_width + x;
    }

    int m_width = 0;
    int m_height = 0;
    int m_depth = 0;
    std::vector<T> m_data;
};

}} // namespace Haruka::Tools