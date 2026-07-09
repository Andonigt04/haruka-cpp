/**
 * @file profiler.h
 * @brief Lightweight scoped CPU profiler — measure before optimizing.
 *
 * Zero-config: drop HARUKA_PROFILE("name") at the top of a scope and its wall
 * time is accumulated per frame under that name. Scopes NEST: a HARUKA_PROFILE
 * inside another becomes its CHILD in a call tree (the runtime scope stack builds
 * the hierarchy), so the profiler now reports a TREE (renderFrameContent →
 * planetary.update → lod.recompute …) instead of a flat list. Call
 * Profiler::get().newFrame() once per frame; read last frame's tree via nodes().
 *
 * Single-threaded by design (the render/update hot path). Negligible overhead:
 * one clock read on enter/exit + a small child lookup. Compile out with
 * HARUKA_NO_PROFILER.
 */
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

namespace Haruka {

class Profiler {
public:
    static Profiler& get() { static Profiler p; return p; }

    /** @brief Un nodo del árbol de perfilado (tiempo INCLUSIVO = self + hijos). */
    struct Node {
        std::string name;
        double ms = 0.0;          // tiempo acumulado este frame (inclusivo)
        int    count = 0;         // nº de veces que entró el scope este frame
        int    parent = -1;       // índice del padre (-1 = raíz virtual [0])
        int    depth = 0;         // 0 = raíz virtual; los tope-nivel son depth 1
        std::vector<int> children;
    };

    /** @brief Entra en un scope: crea/encuentra el hijo del scope actual y lo apila. */
    int enter(const char* name) {
        if (!m_enabled) return -1;
        int parent = m_stack.empty() ? 0 : m_stack.back();
        int idx = findOrAddChild(parent, name);
        m_stack.push_back(idx);
        return idx;
    }
    /** @brief Sale del scope `idx`, sumándole `ms`. */
    void leave(int idx, double ms) {
        if (idx < 0) return;
        m_cur[idx].ms += ms;
        m_cur[idx].count += 1;
        if (!m_stack.empty()) m_stack.pop_back();
    }

    /** @brief Añade ms a una sección con nombre BAJO el scope actual (o raíz). Para
     *  valores medidos fuera de un ScopedTimer (p.ej. "GPU (frame)"). */
    void add(const std::string& name, double ms) {
        if (!m_enabled) return;
        int parent = m_stack.empty() ? 0 : m_stack.back();
        int idx = findOrAddChild(parent, name.c_str());
        m_cur[idx].ms += ms;
        m_cur[idx].count += 1;
    }

    /** @brief Cierra el frame: snapshot del árbol acumulado → m_last, y reinicia. */
    void newFrame() {
        m_last = m_cur;
        resetCur();
    }

    /** @brief Árbol del último frame. nodes()[0] = raíz virtual; recorre sus
     *  children para los scopes de tope-nivel. Vacío si aún no hubo frame. */
    const std::vector<Node>& nodes() const { return m_last; }

    void setEnabled(bool e) { m_enabled = e; }
    bool enabled() const { return m_enabled; }

    Profiler() { resetCur(); }

private:
    void resetCur() {
        m_cur.clear();
        m_cur.push_back(Node{});   // [0] = raíz virtual (name vacío, depth 0)
        m_stack.clear();
    }
    int findOrAddChild(int parent, const char* name) {
        for (int c : m_cur[parent].children) if (m_cur[c].name == name) return c;
        const int depth = m_cur[parent].depth + 1;
        const int idx = (int)m_cur.size();
        Node n; n.name = name; n.parent = parent; n.depth = depth;
        m_cur.push_back(std::move(n));       // puede realocar → tras esto reindexar por [parent]
        m_cur[parent].children.push_back(idx);
        return idx;
    }

    std::vector<Node> m_cur;   // árbol en construcción (frame actual)
    std::vector<Node> m_last;  // snapshot del frame anterior (para el HUD)
    std::vector<int>  m_stack; // pila de scopes activos (índices en m_cur)
    bool m_enabled = true;
};

/** @brief RAII scope timer; su vida se acumula como HIJO del scope activo. */
class ScopedTimer {
public:
    explicit ScopedTimer(const char* name)
        : m_idx(Profiler::get().enter(name)),
          m_start(std::chrono::high_resolution_clock::now()) {}
    ~ScopedTimer() {
        if (m_idx < 0) return;
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> ms = end - m_start;
        Profiler::get().leave(m_idx, ms.count());
    }
private:
    int m_idx;
    std::chrono::high_resolution_clock::time_point m_start;
};

} // namespace Haruka

#ifndef HARUKA_NO_PROFILER
  #define HARUKA_PROFILE_CONCAT2(a,b) a##b
  #define HARUKA_PROFILE_CONCAT(a,b) HARUKA_PROFILE_CONCAT2(a,b)
  #define HARUKA_PROFILE(name) ::Haruka::ScopedTimer HARUKA_PROFILE_CONCAT(_haruka_st_, __LINE__)(name)
#else
  #define HARUKA_PROFILE(name) ((void)0)
#endif
