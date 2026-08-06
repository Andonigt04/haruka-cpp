#pragma once

#include <vector>
#include <cstddef>
#include <cassert>
#include <stdexcept>

namespace Haruka { namespace Tools {

template<typename T>
class Grid2D {
public:
    Grid2D() = default;

    Grid2D(int width, int height, const T& fill = T{})
        : m_width(width), m_height(height), m_data(width * height, fill)
    {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    int size() const { return static_cast<int>(m_data.size()); }

    T& at(int x, int y) {
        return m_data[index(x, y)];
    }

    const T& at(int x, int y) const {
        return m_data[index(x, y)];
    }

    T& operator()(int x, int y) {
        return m_data[y * m_width + x];
    }

    const T& operator()(int x, int y) const {
        return m_data[y * m_width + x];
    }

    T* data() { return m_data.data(); }
    const T* data() const { return m_data.data(); }

    T* row(int y) { return m_data.data() + y * m_width; }
    const T* row(int y) const { return m_data.data() + y * m_width; }

    void resize(int width, int height, const T& fill = T{}) {
        m_width = width;
        m_height = height;
        m_data.assign(width * height, fill);
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
    int index(int x, int y) const {
        return y * m_width + x;
    }

    int m_width = 0;
    int m_height = 0;
    std::vector<T> m_data;
};

}} // namespace Haruka::Tools