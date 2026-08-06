#pragma once

// ===========================================================================
// Malla de árbol generada por el node material graph.
//
// `TreeMeshNode` es el primer nodo de GEOMETRÍA del ProcGraph: a partir de una
// seed determinista emite el árbol final (tronco con taper + ramas + copa de
// blobs achatados), sin tocar GPU. `bakeTreeMesh` extrae el resultado como
// vectores CPU (posiciones/normales/colores/indices) listos para subir a un
// MeshRendererComponent o un Renderer::Mesh.
//
// Determinista: misma seed + parámetros ⇒ misma malla exacta (paridad con el
// resto del grafo). Las normales apuntan hacia fuera (flip por hint radial en
// el build), la base del tronco queda en y=0 en el origen.
// ===========================================================================

#include "proc_graph.h"
#include "proc_noise.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Datos de la malla generada (CPU, sin GPU).
// ===========================================================================
struct TreeMeshData {
    std::vector<glm::vec3>       positions;
    std::vector<glm::vec3>       normals;
    std::vector<glm::vec3>       colors;
    std::vector<glm::vec2>       uvs;
    std::vector<unsigned int>    indices;
    std::vector<unsigned char>   materialId;   // 0 = corteza, 1 = follaje

    void clear() {
        positions.clear(); normals.clear(); colors.clear();
        uvs.clear(); indices.clear(); materialId.clear();
    }
    size_t vertexCount()   const { return positions.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
};

// ===========================================================================
// Nodo de geometría "Tree Mesh".
//
// Entradas: Seed, Height (m), Trunk Radius (m), Canopy Size (escala), Segments.
// Salidas:  VertexCount (int), MeshReady (bool).
// evaluate() construye la malla una sola vez por parámetros y la cachea.
// ===========================================================================
class TreeMeshNode : public Node {
public:
    TreeMeshNode(int seed = 0, float height = 6.5f, float trunkRadius = 0.35f,
                 float canopySize = 1.0f, int segments = 8)
        : m_seed(seed), m_height(height), m_trunkRadius(trunkRadius),
          m_canopySize(canopySize), m_segments(segments) {}

    std::string name() const override { return "Tree Mesh"; }

    std::vector<SocketDesc> inputs() const override {
        return {
            {"Seed",         DataType::Int,   Value::Int(m_seed)},
            {"Height",       DataType::Float, Value::Float(m_height)},
            {"Trunk Radius", DataType::Float, Value::Float(m_trunkRadius)},
            {"Canopy Size",  DataType::Float, Value::Float(m_canopySize)},
            {"Segments",     DataType::Int,   Value::Int(m_segments)},
        };
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"VertexCount", DataType::Int}, {"MeshReady", DataType::Bool}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        int    seed   = m_seed + inputs[0].asInt();
        float  height = inputs[1].asFloat();
        float  trunkR = inputs[2].asFloat();
        float  canopy = inputs[3].asFloat();
        int    segs   = std::max(3, inputs[4].asInt());

        if (!m_built || seed   != m_lastSeed   || height != m_lastHeight ||
            trunkR   != m_lastTrunkR || canopy != m_lastCanopy || segs != m_lastSegs) {
            build(seed, height, trunkR, canopy, segs);
            m_lastSeed = seed; m_lastHeight = height; m_lastTrunkR = trunkR;
            m_lastCanopy = canopy; m_lastSegs = segs;
            m_built = true;
        }

        outputs[0] = Value::Int((int)m_mesh.vertexCount());
        outputs[1] = Value::Bool(true);
    }

    const TreeMeshData& mesh() const { return m_mesh; }

private:
    void build(int seed, float height, float trunkR, float canopy, int segs);

    TreeMeshData m_mesh;
    bool m_built = false;

    int   m_seed;
    float m_height, m_trunkRadius, m_canopySize;
    int   m_segments;

    int   m_lastSeed   = 0x7fffffff;
    float m_lastHeight = -1, m_lastTrunkR = -1, m_lastCanopy = -1;
    int   m_lastSegs   = -1;
};

// ===========================================================================
// Implementación del build: geometría determinista.
// ===========================================================================
inline void TreeMeshNode::build(int seed, float height, float trunkR,
                                float canopy, int segs) {
    TreeMeshData& m = m_mesh;
    m.clear();

    struct Ctx {
        TreeMeshData& m;
        std::vector<glm::vec3> hints; // referencia radial para orientar normales

        size_t addV(const glm::vec3& p, const glm::vec3& hint,
                    const glm::vec2& uv, unsigned char mat, const glm::vec3& color) {
            m.positions.push_back(p);
            m.normals.push_back(glm::vec3(0.0f));
            m.uvs.push_back(uv);
            m.materialId.push_back(mat);
            m.colors.push_back(color);
            hints.push_back(hint);
            return m.positions.size() - 1;
        }

        void addTri(size_t a, size_t b, size_t c) {
            const glm::vec3& va = m.positions[a];
            const glm::vec3& vb = m.positions[b];
            const glm::vec3& vc = m.positions[c];
            glm::vec3 n = glm::cross(vb - va, vc - va);
            glm::vec3 hintSum = hints[a] + hints[b] + hints[c];
            glm::vec3 hint = glm::length(hintSum) > 1e-6f ? glm::normalize(hintSum)
                                                         : glm::vec3(0, 1, 0);
            if (glm::dot(n, hint) < 0.0f) {
                std::swap(b, c);
                n = -n;
            }
            m.indices.push_back((unsigned)a);
            m.indices.push_back((unsigned)b);
            m.indices.push_back((unsigned)c);
            m.normals[a] += n;
            m.normals[b] += n;
            m.normals[c] += n;
        }

        void addQuad(size_t a, size_t b, size_t c, size_t d) {
            addTri(a, b, c);
            addTri(a, c, d);
        }
    } ctx{m};

    const float kPi = 3.14159265358979323846f;

    // ---- colores anime por material ---------------------------------------
    const glm::vec3 barkCol   (0.42f, 0.30f, 0.20f);
    const glm::vec3 foliageCol(0.24f, 0.42f, 0.17f);

    auto appendCone = [&](const glm::vec3& base, const glm::vec3& top,
                          float baseR, float topR, int rings,
                          unsigned char mat, const glm::vec3& color) {
        glm::vec3 d = top - base;
        float len = glm::length(d);
        if (len < 1e-6f) return;
        glm::vec3 axis = d / len;
        glm::vec3 ref = (std::fabs(axis.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 u = glm::normalize(glm::cross(ref, axis));
        glm::vec3 v = glm::cross(axis, u);

        std::vector<std::vector<size_t>> ring(rings + 1);
        for (int i = 0; i <= rings; ++i) {
            float t = (float)i / (float)rings;
            glm::vec3 c = base + d * t;
            float r = baseR + (topR - baseR) * t;
            for (int j = 0; j < segs; ++j) {
                float ang = 2.0f * kPi * (float)j / (float)segs;
                float co = std::cos(ang), sn = std::sin(ang);
                glm::vec3 p = c + (u * co + v * sn) * r;
                glm::vec3 hint = glm::normalize(p - c);
                glm::vec2 uv((float)j / (float)segs, t);
                ring[i].push_back(ctx.addV(p, hint, uv, mat, color));
            }
        }
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segs; ++j) {
                int j2 = (j + 1) % segs;
                ctx.addQuad(ring[i][j], ring[i][j2], ring[i + 1][j2], ring[i + 1][j]);
            }
        }
    };

    // ---- tronco con taper y ligera inclinación determinista ----------------
    float bendX = (WhiteNode::hashFloat(0, 0, 0, seed) - 0.5f) * 0.12f;
    float bendZ = (WhiteNode::hashFloat(0, 1, 0, seed) - 0.5f) * 0.12f;
    float trunkH = height * 0.62f;
    glm::vec3 base(0, 0, 0);
    glm::vec3 top(bendX * trunkH, trunkH, bendZ * trunkH);
    appendCone(base, top, trunkR, trunkR * 0.25f, 5, 0, barkCol);

    // ---- ramas: 2-3 desde la parte superior del tronco ---------------------
    int nBranches = 2 + (int)(WhiteNode::hashFloat(0, 2, 0, seed) * 2.0f); // 2..3
    float branchBaseY = trunkH * 0.55f;
    for (int b = 0; b < nBranches; ++b) {
        float ang  = 2.0f * kPi * WhiteNode::hashFloat(b, 3, 0, seed);
        float lean = 0.35f + WhiteNode::hashFloat(b, 4, 0, seed) * 0.35f;
        float up   = 0.30f + WhiteNode::hashFloat(b, 5, 0, seed) * 0.30f;
        float lenB = height * (0.22f + WhiteNode::hashFloat(b, 6, 0, seed) * 0.12f);
        glm::vec3 dir(std::cos(ang) * lean, up, std::sin(ang) * lean);
        dir = glm::normalize(dir);
        glm::vec3 bBase(base.x + bendX * branchBaseY, branchBaseY,
                        base.z + bendZ * branchBaseY);
        appendCone(bBase, bBase + dir * lenB,
                   trunkR * 0.30f, trunkR * 0.05f, 2, 0, barkCol);
    }

    // ---- copa: cluster de blobs achatados (estilo anime) -------------------
    auto appendSphere = [&](const glm::vec3& center, float radius,
                            float squashY, unsigned char mat,
                            const glm::vec3& color) {
        int lat = 4, lon = std::max(4, segs);
        size_t topV = ctx.addV(center + glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, 1, 0), glm::vec2(0.5f, 1.0f), mat, color);
        std::vector<std::vector<size_t>> rings(lat - 1);
        for (int i = 1; i < lat; ++i) {
            float theta = kPi * (float)i / (float)lat;
            float r = radius * std::sin(theta);
            float y = radius * std::cos(theta) * squashY;
            for (int j = 0; j < lon; ++j) {
                float phi = 2.0f * kPi * (float)j / (float)lon;
                glm::vec3 p(center.x + std::cos(phi) * r,
                            center.y + y,
                            center.z + std::sin(phi) * r);
                glm::vec3 hint = glm::normalize(p - center);
                glm::vec2 uv((float)j / (float)lon, (float)i / (float)lat);
                rings[i - 1].push_back(ctx.addV(p, hint, uv, mat, color));
            }
        }
        size_t botV = ctx.addV(center - glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, -1, 0), glm::vec2(0.5f, 0.0f), mat, color);
        for (int j = 0; j < lon; ++j) {
            int j2 = (j + 1) % lon;
            ctx.addTri(topV, rings[0][j2], rings[0][j]);
            for (int i = 1; i < lat - 1; ++i)
                ctx.addQuad(rings[i - 1][j], rings[i - 1][j2],
                            rings[i][j2], rings[i][j]);
            ctx.addTri(botV, rings[lat - 2][j], rings[lat - 2][j2]);
        }
    };

    int nBlobs = 5 + (int)(WhiteNode::hashFloat(0, 7, 0, seed) * 3.0f); // 5..7
    float canopyY = trunkH * 0.95f;
    float canopyR = trunkR * (3.2f + 1.6f * canopy);
    for (int b = 0; b < nBlobs; ++b) {
        float ang = 2.0f * kPi * WhiteNode::hashFloat(b, 8, 0, seed);
        float rad = canopyR * (0.35f + WhiteNode::hashFloat(b, 9, 0, seed) * 0.45f);
        float y   = canopyY + (WhiteNode::hashFloat(b, 10, 0, seed) - 0.5f) * canopyR * 0.5f;
        float br  = trunkR * (1.4f + WhiteNode::hashFloat(b, 11, 0, seed) * 0.9f) * canopy;
        glm::vec3 c(std::cos(ang) * rad, y, std::sin(ang) * rad);
        appendSphere(c, br, 0.82f, 1, foliageCol);
    }

    // ---- normalización final de normales ------------------------------------
    for (size_t i = 0; i < m.normals.size(); ++i) {
        float l = glm::length(m.normals[i]);
        if (l > 1e-6f) m.normals[i] /= l;
        else m.normals[i] = glm::normalize(ctx.hints[i]);
    }
}

// ===========================================================================
// Extrae la malla del nodo tras forzar su evaluación.
// ===========================================================================
inline bool bakeTreeMesh(Graph& g, int nodeIndex, TreeMeshData& out) {
    g.evaluate(nodeIndex, 0, 0.0f, 0.0f, 0.0f);
    TreeMeshNode* n = dynamic_cast<TreeMeshNode*>(g.node(nodeIndex));
    if (!n) return false;
    out = n->mesh();
    return !out.positions.empty();
}

}}} // namespace Haruka::Tools::ProcGraph
