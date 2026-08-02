#pragma once

// ===========================================================================
// Nodos orgánicos estilo anime para el ProcGraph.
//
// KnotNode   -> nudos del tronco (óvalos concéntricos alrededor de sitios
//               dispersos con hash determinista). Los sitios viven en una
//               grilla para poder evaluar el vecindario local por píxel.
// BranchNode -> ramas / rutas de corteza: árbol fractal determinista a partir
//               de una semilla. Devuelve una máscara de líneas con taper.
// ===========================================================================

#include "proc_graph.h"
#include "proc_noise.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace Haruka {
namespace Tools {
namespace ProcGraph {

namespace organic_internal {
    inline float smoothstep(float a, float b, float x) {
        float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    inline float distToSegment(float px, float py,
                               float ax, float ay, float bx, float by) {
        float abx = bx - ax, aby = by - ay;
        float t = (px - ax) * abx + (py - ay) * aby;
        float len2 = abx * abx + aby * aby;
        if (len2 > 1e-8f) t /= len2;
        t = std::clamp(t, 0.0f, 1.0f);
        float cx = ax + abx * t, cy = ay + aby * t;
        float dx = px - cx, dy = py - cy;
        return std::sqrt(dx * dx + dy * dy);
    }
}

// ===========================================================================
// Nudos del tronco (estilo anime: óvalos con anillo de corteza)
// ===========================================================================
class KnotNode : public Node {
public:
    KnotNode(int seed = 0, int count = 8, float size = 1.0f)
        : m_seed(seed), m_count(count), m_size(size) {}

    std::string name() const override { return "Knots (nudos)"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Count", DataType::Int, Value::Int(m_count)},
                {"Size",  DataType::Float, Value::Float(m_size)},
                {"Seed",  DataType::Int, Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Mask", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float) override {
        int n = std::max(1, std::min(64, inputs[0].asInt()));
        float size = std::max(inputs[1].asFloat(), 1e-3f);
        int se = m_seed + inputs[2].asInt();
        outputs[0] = Value::Float(eval(x, y, n, size, se));
    }

    static float eval(float x, float y, int count, float size, int seed) {
        int g = std::max(2, (int)std::ceil(std::sqrt((float)count * 2.0f)));
        float cell = 1.0f / (float)g;
        int ix = (int)std::floor(x / cell);
        int iy = (int)std::floor(y / cell);
        if (x < 0) --ix;
        if (y < 0) --iy;

        float best = 0.0f;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                int ci = ix + di, cj = iy + dj;
                if (WhiteNode::hashFloat(ci, cj, 0, seed) < 0.5f) continue;

                float ox = (WhiteNode::hashFloat(ci, cj, 1, seed) - 0.5f) * 0.8f;
                float oy = (WhiteNode::hashFloat(ci, cj, 2, seed) - 0.5f) * 0.8f;
                float sx = (ci + 0.5f + ox) * cell;
                float sy = (cj + 0.5f + oy) * cell;

                float aspect = 0.55f + WhiteNode::hashFloat(ci, cj, 3, seed) * 1.1f;
                float rot = WhiteNode::hashFloat(ci, cj, 4, seed) * 1.5707963f;
                float cr = std::cos(rot), sr = std::sin(rot);
                float dx = x - sx, dy = y - sy;
                float lx = (dx * cr + dy * sr) / aspect;
                float ly = -dx * sr + dy * cr;

                float r = std::sqrt(lx * lx + ly * ly) / (size * cell);
                float center = 1.0f - organic_internal::smoothstep(0.0f, 0.30f, r);
                float ring = organic_internal::smoothstep(0.42f, 0.52f, r) *
                             (1.0f - organic_internal::smoothstep(0.60f, 0.72f, r));
                best = std::max(best, std::max(center, ring));
            }
        }
        return best;
    }

private:
    int m_seed, m_count;
    float m_size;
};

// ===========================================================================
// Ramas / rutas de corteza (árbol fractal determinista)
// ===========================================================================
class BranchNode : public Node {
public:
    BranchNode(int seed = 0, float width = 0.02f, int depth = 4)
        : m_seed(seed), m_width(width), m_depth(depth) {}

    std::string name() const override { return "Branches (ramas)"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Width", DataType::Float, Value::Float(m_width)},
                {"Depth", DataType::Int, Value::Int(m_depth)},
                {"Seed",  DataType::Int, Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Mask", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float) override {
        float w = std::max(inputs[0].asFloat(), 1e-4f);
        int d = std::max(1, std::min(6, inputs[1].asInt()));
        int se = m_seed + inputs[2].asInt();
        outputs[0] = Value::Float(eval(x, y, w, d, se));
    }

    static float eval(float x, float y, float width, int depth, int seed) {
        m_xy.clear();
        m_wi.clear();
        addBranch(0.5f, 0.0f, 0.0f, 1.0f, 0.42f, depth, seed);

        float best = 0.0f;
        for (size_t i = 0; i < m_xy.size(); i += 4) {
            float d = organic_internal::distToSegment(x, y, m_xy[i], m_xy[i + 1],
                                                      m_xy[i + 2], m_xy[i + 3]);
            float w = width * m_wi[i >> 2];
            best = std::max(best, 1.0f - organic_internal::smoothstep(0.0f, w, d));
            if (best > 0.99f) break;
        }
        return best;
    }

private:
    int m_seed;
    float m_width;
    int m_depth;

    // Recursión fractal: (px,py) punto de inicio, (dx,dy) dirección unitaria,
    // length longitud, taper factor de anchura por nivel.
    static void addBranch(float px, float py, float dx, float dy, float length,
                          int depth, int seed) {
        float ex = px + dx * length;
        float ey = py + dy * length;
        float taper = 1.0f - 0.18f * (float)(7 - depth);   // tronco más ancho
        m_xy.push_back(px); m_xy.push_back(py);
        m_xy.push_back(ex); m_xy.push_back(ey);
        m_wi.push_back(std::max(taper, 0.15f));

        if (depth <= 0) return;
        int idx = (int)m_xy.size() >> 2;
        float spread = 0.55f + WhiteNode::hashFloat(idx, depth, 1, seed) * 0.35f;
        float lenFac = 0.55f + WhiteNode::hashFloat(idx, depth, 2, seed) * 0.35f;

        float nx = -dy, ny = dx;
        float a = spread * WhiteNode::hashFloat(idx, depth, 3, seed);
        float c = std::cos(a), s = std::sin(a);
        float cw = WhiteNode::hashFloat(idx, depth, 4, seed) < 0.5f ? 1.0f : -1.0f;
        float d1x = dx * c + nx * s * cw, d1y = dy * c + ny * s * cw;
        addBranch(ex, ey, d1x, d1y, length * lenFac, depth - 1, seed);

        // segunda rama con ángulo opuesto
        a = spread * (1.0f - WhiteNode::hashFloat(idx, depth, 5, seed));
        c = std::cos(a); s = std::sin(a);
        float d2x = dx * c - nx * s * cw, d2y = dy * c - ny * s * cw;
        addBranch(ex, ey, d2x, d2y, length * lenFac * (0.7f + 0.3f * WhiteNode::hashFloat(idx, depth, 6, seed)),
                  depth - 1, seed);
    }

    static std::vector<float> m_xy;
    static std::vector<float> m_wi;
};

inline std::vector<float> BranchNode::m_xy;
inline std::vector<float> BranchNode::m_wi;

} // namespace ProcGraph
} // namespace Tools
} // namespace Haruka
