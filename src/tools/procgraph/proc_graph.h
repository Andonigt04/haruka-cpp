/**
 * @file proc_graph.h
 * @brief Grafo computacional para generación procedural (ProcGraph).
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
 */
#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

namespace Haruka { namespace Tools { namespace ProcGraph {

/** @brief Tipo de dato que fluye por el grafo. */
enum class DataType { Float, Int, Vec2, Vec3, Vec4, Bool };

/**
 * @brief Tagged union para datos que fluyen por el grafo.
 * Almacena hasta 4 floats con un discriminante de tipo.
 */
struct Value {
    DataType type = DataType::Float;
    float data[4] = {0, 0, 0, 0};

    static Value Float(float v)       { Value r; r.type = DataType::Float; r.data[0] = v; return r; }
    static Value Int(int v)           { Value r; r.type = DataType::Int; r.data[0] = (float)v; return r; }
    static Value Vec2(float x, float y)   { Value r; r.type = DataType::Vec2; r.data[0]=x; r.data[1]=y; return r; }
    static Value Vec3(float x, float y, float z) { Value r; r.type = DataType::Vec3; r.data[0]=x; r.data[1]=y; r.data[2]=z; return r; }
    static Value Vec4(float x, float y, float z, float w) { Value r; r.type = DataType::Vec4; r.data[0]=x; r.data[1]=y; r.data[2]=z; r.data[3]=w; return r; }
    static Value Bool(bool b)         { Value r; r.type = DataType::Bool; r.data[0] = b ? 1 : 0; return r; }

    float asFloat() const { return data[0]; }
    int   asInt()    const { return (int)data[0]; }
    bool  asBool()   const { return data[0] != 0; }
    void  asVec2(float& x, float& y) const { x = data[0]; y = data[1]; }
    void  asVec3(float& x, float& y, float& z) const { x = data[0]; y = data[1]; z = data[2]; }
    void  asVec4(float& x, float& y, float& z, float& w) const { x = data[0]; y = data[1]; z = data[2]; w = data[3]; }
};

/** @brief Descriptor de un socket (entrada/salida): nombre + tipo + valor por defecto. */
struct SocketDesc {
    std::string name;
    DataType    type = DataType::Float;
    Value       defaultValue;
};

class Graph;

/**
 * @brief Nodo virtual base del grafo.
 * Cada nodo declara sus entradas/salidas y se evalúa en (x,y,z).
 */
class Node {
    friend class Graph;
public:
    virtual ~Node() = default;
    /** @brief Nombre del nodo (para debugging). */
    virtual std::string     name() const = 0;
    /** @brief Descripción de los sockets de entrada. */
    virtual std::vector<SocketDesc> inputs() const = 0;
    /** @brief Descripción de los sockets de salida. */
    virtual std::vector<SocketDesc> outputs() const = 0;

    /**
     * @brief Evalúa el nodo en la coordenada (x,y,z).
     * @param inputs  Valores de los nodos upstream conectados.
     * @param outputs Buffer donde escribir los valores de salida.
     */
    virtual void evaluate(const Value* inputs, int numInputs,
                          Value* outputs, int numOutputs,
                          float x, float y, float z) = 0;

protected:
    Graph* m_graph = nullptr;
    int m_nodeIndex = -1;
};

/** @brief Conexión entre la salida de un nodo y la entrada de otro. */
struct Connection {
    int fromNode   = -1;
    int fromOutput = 0;
    int toNode     = -1;
    int toInput    = 0;
};

/**
 * @brief Contenedor de nodos + conexiones con evaluador topológico.
 *
 * Uso: emplaceNode(...) para añadir nodos, connect(...) para cablear,
 * compile() para ordenar, evaluate(...) para ejecutar.
 */
class Graph {
public:
    /** @brief Añade un nodo al grafo. Retorna su índice. */
    int addNode(std::unique_ptr<Node> node) {
        int idx = (int)m_nodes.size();
        node->m_graph = this;
        node->m_nodeIndex = idx;
        m_nodes.push_back(std::move(node));
        m_dirty = true;
        return idx;
    }

    /** @brief Construye un nodo in-place y lo añade. Retorna su índice. */
    template<typename T, typename... Args>
    int emplaceNode(Args&&... args) {
        return addNode(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /** @brief Conecta la salida fromOutput del nodo fromNode a la entrada toInput de toNode. */
    void connect(int fromNode, int fromOutput, int toNode, int toInput) {
        m_connections.push_back({fromNode, fromOutput, toNode, toInput});
        m_dirty = true;
    }

    /** @brief Lista de conexiones (const). */
    const std::vector<Connection>& connections() const { return m_connections; }

    /** @brief Nodo por índice (mutable). */
    Node* node(int index) {
        return index >= 0 && index < (int)m_nodes.size() ? m_nodes[index].get() : nullptr;
    }

    /** @brief Nodo por índice (const). */
    const Node* node(int index) const {
        return index >= 0 && index < (int)m_nodes.size() ? m_nodes[index].get() : nullptr;
    }

    /** @brief Cantidad de nodos. */
    int nodeCount() const { return (int)m_nodes.size(); }

    /**
     * @brief Compila el grafo: orden topológico + caché.
     * Debe llamarse después de cablear y antes de evaluate().
     * @return false si hay ciclos.
     */
    bool compile() {
        m_order.clear();
        int n = (int)m_nodes.size();
        if (n == 0) { m_dirty = false; return true; }

        std::vector<int> inDegree(n, 0);
        std::vector<std::vector<int>> outEdges(n);
        for (const auto& conn : m_connections) {
            if (conn.toNode >= 0 && conn.toNode < n) {
                inDegree[conn.toNode]++;
            }
            if (conn.fromNode >= 0 && conn.fromNode < n) {
                outEdges[conn.fromNode].push_back(conn.toNode);
            }
        }

        std::vector<int> queue;
        for (int i = 0; i < n; i++) {
            if (inDegree[i] == 0) queue.push_back(i);
        }

        while (!queue.empty()) {
            int idx = queue.back(); queue.pop_back();
            m_order.push_back(idx);
            for (int neighbor : outEdges[idx]) {
                if (--inDegree[neighbor] == 0) queue.push_back(neighbor);
            }
        }

        bool valid = (int)m_order.size() == n;
        m_dirty = false;
        return valid;
    }

    /**
     * @brief Evalúa el grafo completo en (x,y,z).
     * @return Valor de la salida outputIndex del nodo nodeIndex.
     */
    Value evaluate(int nodeIndex, int outputIndex, float x, float y, float z = 0) {
        if (m_dirty) compile();
        int n = (int)m_nodes.size();
        if (n == 0) return Value::Float(0);

        // Ensure temp storage is large enough
        if ((int)m_temp.size() < n) m_temp.resize(n);
        if ((int)m_inputs.size() < n) m_inputs.resize(n);

        // For each node in topological order:
        for (int idx : m_order) {
            Node* nd = m_nodes[idx].get();
            if (!nd) continue;

            auto ins = nd->inputs();
            auto outs = nd->outputs();
            int numIn = (int)ins.size();
            int numOut = (int)outs.size();

            // Gather input values from connections or defaults
            if ((int)m_inputs[idx].size() < numIn) m_inputs[idx].resize(numIn);
            for (int i = 0; i < numIn; i++) {
                m_inputs[idx][i] = ins[i].defaultValue; // default
            }
            for (const auto& conn : m_connections) {
                if (conn.toNode == idx && conn.toInput < numIn) {
                    m_inputs[idx][conn.toInput] = m_temp[conn.fromNode][conn.fromOutput];
                }
            }

            // Evaluate
            if ((int)m_temp[idx].size() < numOut) m_temp[idx].resize(numOut);
            nd->evaluate(m_inputs[idx].data(), numIn,
                         m_temp[idx].data(), numOut,
                         x, y, z);
        }

        if (nodeIndex >= 0 && nodeIndex < n &&
            outputIndex >= 0 && outputIndex < (int)m_temp[nodeIndex].size()) {
            return m_temp[nodeIndex][outputIndex];
        }
        return Value::Float(0);
    }

    /**
     * @brief Re-evalúa solo el subgrafo upstream de un nodo en (x,y,z).
     * Útil para domain warping: permite muestrear el source en coordenadas transformadas.
     */
    Value evaluateNodeAt(int nodeIndex, int outputIndex, float x, float y, float z) {
        int n = (int)m_nodes.size();
        if (nodeIndex < 0 || nodeIndex >= n) return Value::Float(0);

        // Mark upstream nodes
        std::vector<bool> needed(n, false);
        std::vector<int> stack = {nodeIndex};
        needed[nodeIndex] = true;
        while (!stack.empty()) {
            int idx = stack.back(); stack.pop_back();
            for (auto& conn : m_connections) {
                if (conn.toNode == idx && !needed[conn.fromNode]) {
                    needed[conn.fromNode] = true;
                    stack.push_back(conn.fromNode);
                }
            }
        }

        // Topo-sort needed nodes preserving global order
        std::vector<int> order;
        for (int idx : m_order) {
            if (needed[idx]) order.push_back(idx);
        }

        // Temp storage for this re-evaluation
        std::vector<std::vector<Value>> temp(n);
        std::vector<std::vector<Value>> inps(n);

        for (int idx : order) {
            Node* nd = m_nodes[idx].get();
            auto ins = nd->inputs();
            auto outs = nd->outputs();
            int numIn = (int)ins.size();
            int numOut = (int)outs.size();

            if ((int)inps[idx].size() < numIn) inps[idx].resize(numIn);
            for (int i = 0; i < numIn; i++) {
                inps[idx][i] = ins[i].defaultValue;
            }
            for (auto& conn : m_connections) {
                if (conn.toNode == idx && conn.toInput < numIn && conn.fromNode >= 0) {
                    if ((int)temp[conn.fromNode].size() > conn.fromOutput) {
                        inps[idx][conn.toInput] = temp[conn.fromNode][conn.fromOutput];
                    }
                }
            }

            if ((int)temp[idx].size() < numOut) temp[idx].resize(numOut);
            nd->evaluate(inps[idx].data(), numIn, temp[idx].data(), numOut, x, y, z);
        }

        if (outputIndex >= 0 && outputIndex < (int)temp[nodeIndex].size()) {
            return temp[nodeIndex][outputIndex];
        }
        return Value::Float(0);
    }

    /**
     * @brief Evalúa el nodo upstream conectado a (nodeIndex, inputSlot) en (x,y,z).
     * @return Valor por defecto si no hay conexión.
     */
    Value evaluateInputAt(int nodeIndex, int inputSlot, float x, float y, float z) {
        for (auto& conn : m_connections) {
            if (conn.toNode == nodeIndex && conn.toInput == inputSlot) {
                return evaluateNodeAt(conn.fromNode, conn.fromOutput, x, y, z);
            }
        }
        return Value::Float(0);
    }

    /** @brief Limpia todos los nodos, conexiones y cachés. */
    void clear() {
        m_nodes.clear();
        m_connections.clear();
        m_order.clear();
        m_temp.clear();
        m_inputs.clear();
        m_dirty = true;
    }

private:
    std::vector<std::unique_ptr<Node>> m_nodes;
    std::vector<Connection> m_connections;
    std::vector<int> m_order;                     // topological order indices
    std::vector<std::vector<Value>> m_temp;       // per-node output cache
    std::vector<std::vector<Value>> m_inputs;     // per-node input cache
    bool m_dirty = true;
};

/**
 * @brief Evalúa el grafo en una malla 2D equiespaciada.
 * @tparam Grid2D Debe tener width(), height() y operator()(int x, int y) -> float&.
 */
template<typename Grid2D>
void evaluateToGrid(const Graph& graph, int nodeIndex, int outputIndex,
                    Grid2D& grid, float ox = 0, float oy = 0, float scale = 1) {
    // graph.evaluate is not const (it modifies cache), but we work around
    Graph& g = const_cast<Graph&>(graph);
    for (int y = 0; y < grid.height(); y++) {
        for (int x = 0; x < grid.width(); x++) {
            float fx = (x + ox) * scale;
            float fy = (y + oy) * scale;
            grid(x, y) = g.evaluate(nodeIndex, outputIndex, fx, fy, 0).asFloat();
        }
    }
}

}}} // namespace Haruka::Tools::ProcGraph