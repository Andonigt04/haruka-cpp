#pragma once

#include "proc_graph.h"
#include <random>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Perlin noise node
// ===========================================================================
class PerlinNode : public Node {
public:
    PerlinNode(int seed = 0, float scale = 2.0f)
        : m_seed(seed), m_scale(scale) {}

    std::string name() const override { return "Perlin Noise"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(m_scale)},
                {"Seed",  DataType::Int,   Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Value", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        float s = inputs[0].asFloat();
        float sx = s == 0 ? 1.0f : s;
        int se = m_seed + inputs[1].asInt();
        outputs[0] = Value::Float(perlin(x / sx, y / sx, z / sx, se));
    }

    static float perlin(float x, float y, float z, int seed = 0);

private:
    int m_seed;
    float m_scale;
};

// ===========================================================================
// Voronoi node
// ===========================================================================
class VoronoiNode : public Node {
public:
    enum Metric { Euclidean, Manhattan, Chebyshev };
    enum Output { F1, F2, F1F2, Cell, Edge };

    VoronoiNode(int seed = 0, float scale = 2.0f,
                Metric metric = Euclidean, Output output = F1)
        : m_seed(seed), m_scale(scale), m_metric(metric), m_output(output) {}

    std::string name() const override { return "Voronoi"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(m_scale)},
                {"Seed",  DataType::Int,   Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Distance", DataType::Float}, {"Cell", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override;

private:
    int m_seed, m_permSeed = 0;
    float m_scale;
    Metric m_metric;
    Output m_output;
};

// ===========================================================================
// FBM — wraps any noise type into fractal Brownian motion
// ===========================================================================
class FBMNode : public Node {
public:
    enum Mode { Sum, Max, Min, AbsSum };

    FBMNode(int octaves = 4, float lacunarity = 2.0f, float gain = 0.5f,
            Mode mode = Sum, int seed = 0)
        : m_octaves(octaves), m_lacunarity(lacunarity), m_gain(gain)
        , m_mode(mode), m_seed(seed), m_permSeed(seed) {}

    void setNoiseFn(std::function<float(float, float, float, int)> fn) {
        m_noiseFn = std::move(fn);
    }

    std::string name() const override { return "FBM"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(1.0f)},
                {"Octaves", DataType::Int, Value::Int((float)m_octaves)},
                {"Lacunarity", DataType::Float, Value::Float(m_lacunarity)},
                {"Gain", DataType::Float, Value::Float(m_gain)},
                {"Seed", DataType::Int, Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Value", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        if (!m_noiseFn) { outputs[0] = Value::Float(0); return; }
        float s = inputs[0].asFloat();
        int oct = std::max(1, inputs[1].asInt());
        float lac = inputs[2].asFloat();
        float gain = inputs[3].asFloat();
        int se = m_seed + inputs[4].asInt();
        float amp = 1.0f, freq = s, result = 0.0f, maxAmp = 0.0f;
        for (int i = 0; i < oct; i++) {
            float n = m_noiseFn(x * freq, y * freq, z * freq, se + i * 137);
            switch (m_mode) {
                case Sum:    result += amp * n; maxAmp += amp; break;
                case Max:    if (amp * n > result) result = amp * n; break;
                case Min:    if (amp * n < result) result = amp * n; break;
                case AbsSum: result += amp * std::abs(n); maxAmp += amp; break;
            }
            freq *= lac;
            amp  *= gain;
        }
        outputs[0] = Value::Float((m_mode == Sum || m_mode == AbsSum) && maxAmp > 0
                                   ? result / maxAmp : result);
    }

private:
    int m_octaves;
    float m_lacunarity, m_gain;
    Mode m_mode;
    int m_seed, m_permSeed;
    std::function<float(float, float, float, int)> m_noiseFn;
};

// ===========================================================================
// White noise node
// ===========================================================================
class WhiteNode : public Node {
public:
    WhiteNode(int seed = 0, float scale = 2.0f)
        : m_seed(seed), m_scale(scale) {}

    std::string name() const override { return "White Noise"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(m_scale)},
                {"Seed",  DataType::Int,   Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Value", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        float s = inputs[0].asFloat();
        if (s == 0) s = 1;
        int se = m_seed + inputs[1].asInt();
        outputs[0] = Value::Float(hashFloat((int)std::floor(x / s),
                                            (int)std::floor(y / s),
                                            (int)std::floor(z / s), se));
    }

    static float hashFloat(int ix, int iy, int iz, int seed);

private:
    int m_seed;
    float m_scale;
};

// ===========================================================================
// Inline implementations
// ===========================================================================

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15;
    x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

inline float WhiteNode::hashFloat(int ix, int iy, int iz, int seed) {
    uint32_t h = hash32((uint32_t)(ix + 1024 * iy + 1024 * 1024 * iz + 1024 * 1024 * 8 * seed));
    return (float)((double)h / (double)UINT32_MAX);
}

inline float grad3d(int h, float x, float y, float z) {
    int h3 = h & 15;
    float u = h3 < 8 ? x : y;
    float v = h3 < 4 ? y : (h3 == 12 || h3 == 14 ? x : z);
    return ((h3 & 1) == 0 ? u : -u) + ((h3 & 2) == 0 ? v : -v);
}

inline float PerlinNode::perlin(float x, float y, float z, int seed) {
    int ix = (int)std::floor(x);
    int iy = (int)std::floor(y);
    int iz = (int)std::floor(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    fz = fz * fz * (3 - 2 * fz);
    float n000 = grad3d((int)(WhiteNode::hashFloat(ix, iy, iz, seed) * 255), fx, fy, fz);
    float n001 = grad3d((int)(WhiteNode::hashFloat(ix, iy, iz+1, seed) * 255), fx, fy, fz-1);
    float n010 = grad3d((int)(WhiteNode::hashFloat(ix, iy+1, iz, seed) * 255), fx, fy-1, fz);
    float n011 = grad3d((int)(WhiteNode::hashFloat(ix, iy+1, iz+1, seed) * 255), fx, fy-1, fz-1);
    float n100 = grad3d((int)(WhiteNode::hashFloat(ix+1, iy, iz, seed) * 255), fx-1, fy, fz);
    float n101 = grad3d((int)(WhiteNode::hashFloat(ix+1, iy, iz+1, seed) * 255), fx-1, fy, fz-1);
    float n110 = grad3d((int)(WhiteNode::hashFloat(ix+1, iy+1, iz, seed) * 255), fx-1, fy-1, fz);
    float n111 = grad3d((int)(WhiteNode::hashFloat(ix+1, iy+1, iz+1, seed) * 255), fx-1, fy-1, fz-1);
    float nx00 = n000 + (n100 - n000) * fx;
    float nx01 = n001 + (n101 - n001) * fx;
    float nx10 = n010 + (n110 - n010) * fx;
    float nx11 = n011 + (n111 - n011) * fx;
    float nxy0 = nx00 + (nx10 - nx00) * fy;
    float nxy1 = nx01 + (nx11 - nx01) * fy;
    return nxy0 + (nxy1 - nxy0) * fz;
}

inline void VoronoiNode::evaluate(const Value* inputs, int, Value* outputs, int,
                                   float x, float y, float z) {
    float s = inputs[0].asFloat();
    if (s == 0) s = 1;
    int se = m_seed + inputs[1].asInt();
    x /= s; y /= s; z /= s;
    int ix = (int)std::floor(x);
    int iy = (int)std::floor(y);
    int iz = (int)std::floor(z);
    float f1 = 1e10f, f2 = 1e10f;
    float cellVal = 0;
    for (int di = -1; di <= 1; di++) {
        for (int dj = -1; dj <= 1; dj++) {
            for (int dk = -1; dk <= 1; dk++) {
                int ci = ix + di, cj = iy + dj, ck = iz + dk;
                float cx = ci + WhiteNode::hashFloat(ci, cj, ck, se);
                float cy = cj + WhiteNode::hashFloat(ci, cj, ck, se + 1);
                float cz = ck + WhiteNode::hashFloat(ci, cj, ck, se + 2);
                float dx = x - cx, dy = y - cy, dz = z - cz;
                float d;
                switch (m_metric) {
                    case Manhattan:  d = std::abs(dx) + std::abs(dy) + std::abs(dz); break;
                    case Chebyshev:  d = std::max({std::abs(dx), std::abs(dy), std::abs(dz)}); break;
                    default:         d = dx*dx + dy*dy + dz*dz; break;
                }
                if (d < f1) { f2 = f1; f1 = d; cellVal = WhiteNode::hashFloat(ci, cj, ck, se + 3); }
                else if (d < f2) f2 = d;
            }
        }
    }
    switch (m_output) {
        case F1:   outputs[0] = Value::Float(f1); break;
        case F2:   outputs[0] = Value::Float(f2); break;
        case F1F2: outputs[0] = Value::Float(f1 - f2); break;
        case Cell: outputs[0] = Value::Float(cellVal); break;
        case Edge: { float e = f1 - f2; outputs[0] = Value::Float(e < 0.1f ? e / 0.1f : 1.0f); } break;
    }
    outputs[1] = Value::Float(cellVal);
}

// ===========================================================================
// Voronoi Edge node — distance to nearest edge (crackle)
// ===========================================================================
class VoronoiEdgeNode : public Node {
public:
    VoronoiEdgeNode(int seed = 0, float scale = 2.0f, VoronoiNode::Metric metric = VoronoiNode::Metric::Euclidean)
        : m_seed(seed), m_permSeed(seed), m_scale(scale), m_metric(metric) {}

    std::string name() const override { return "Voronoi Edge"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(m_scale)},
                {"Seed",  DataType::Int,   Value::Int(m_seed)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Edge", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        float s = inputs[0].asFloat();
        if (s == 0) s = 1;
        int se = m_seed + inputs[1].asInt();
        if (se != 0) m_permSeed = se;

        float sx = x / s, sy = y / s, sz = z / s;
        int ix = (int)std::floor(sx);
        int iy = (int)std::floor(sy);
        int iz = (int)std::floor(sz);

        float nearest = 999, second = 999;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                    float fx = cx + WhiteNode::hashFloat(cx, cy, cz, m_permSeed);
                    float fy = cy + WhiteNode::hashFloat(cx, cy, cz, m_permSeed + 1);
                    float fz = cz + WhiteNode::hashFloat(cx, cy, cz, m_permSeed + 2);
                    float ox = sx - fx, oy = sy - fy, oz = sz - fz;
                    float d;
                    switch (m_metric) {
                        case VoronoiNode::Metric::Euclidean: d = ox*ox + oy*oy + oz*oz; break;
                        case VoronoiNode::Metric::Manhattan: d = std::abs(ox) + std::abs(oy) + std::abs(oz); break;
                        case VoronoiNode::Metric::Chebyshev: d = std::max({std::abs(ox), std::abs(oy), std::abs(oz)}); break;
                    }
                    if (d < nearest) { second = nearest; nearest = d; }
                    else if (d < second) { second = d; }
                }
            }
        }
        outputs[0] = Value::Float(std::sqrt(second) - std::sqrt(nearest));
    }

private:
    int m_seed, m_permSeed;
    float m_scale;
    VoronoiNode::Metric m_metric;
};

}}} // namespace Haruka::Tools::ProcGraph