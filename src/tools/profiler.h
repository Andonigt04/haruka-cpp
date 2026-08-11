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
 * THREAD-LOCAL: cada hilo tiene su propio árbol (Profiler::get() es thread_local). No es
 * thread-safe dentro de un hilo por diseño; el aislamiento por hilo evita la corrupción cuando
 * varios hilos perfilan (p.ej. el worker del LOD, ver lodasync). El HUD lee el árbol del hilo de
 * render (newFrame/nodes en el main). Negligible overhead: one clock read on enter/exit + a small
 * child lookup. Compile out with HARUKA_NO_PROFILER.
 */
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>    // snprintf — volcado del arbol para la auditoria de rendimiento

namespace Haruka {

class Profiler {
public:
    // THREAD-LOCAL: cada hilo acumula su PROPIO árbol. El profiler no es thread-safe (una sola
    // pila m_cur/m_stack), así que con el recompute del LOD en un hilo worker (ver lodasync) un
    // singleton global se corrompía (enter/leave concurrentes → índices fuera de rango). Con
    // thread_local el hilo de render lee su árbol (el HUD, vía newFrame/nodes en el main) y el
    // worker el suyo (nunca leído, inofensivo). Es lo correcto además: el trabajo movido al worker
    // NO debe aparecer en el árbol del frame del hilo de render.
    static Profiler& get() { thread_local Profiler p; return p; }

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

    /**
     * @brief Vuelca el árbol COMPLETO al log, sin el umbral de 0,5 ms del HUD.
     *
     * El panel colapsa todo lo que baja de 0,5 ms en un "+8 < 0.5 ms", y para una auditoría eso es
     * justo lo que hay que ver: un scope de 0,3 ms que corre 200 veces por frame no aparece, y sí es
     * medio frame. Aquí sale todo, con su tiempo INCLUSIVO, su cuenta de entradas y —lo que de verdad
     * señala al culpable— su tiempo PROPIO (inclusivo menos el de sus hijos).
     *
     * Se promedia sobre `frames` para que un pico aislado no dicte el diagnóstico.
     */
    std::string dumpTree(int frames = 1) const {
        std::string out;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%-46s %9s %9s %7s\n", "scope", "incl(ms)", "self(ms)", "veces");
        out += buf;
        dumpNode(0, frames, out);
        return out;
    }

    Profiler() { resetCur(); }

private:
    void dumpNode(int idx, int frames, std::string& out) const {
        if (idx < 0 || idx >= (int)m_last.size()) return;
        const Node& n = m_last[(size_t)idx];
        if (idx != 0) {
            double self = n.ms;
            for (int c : n.children) self -= m_last[(size_t)c].ms;
            const double f = frames > 0 ? (double)frames : 1.0;
            char buf[256];
            std::string indent((size_t)(n.depth - 1) * 2, ' ');
            std::snprintf(buf, sizeof buf, "%-46s %9.3f %9.3f %7.1f\n",
                          (indent + n.name).substr(0, 46).c_str(), n.ms / f, self / f, n.count / f);
            out += buf;
        }
        // Hijos ordenados por tiempo inclusivo: lo caro arriba, que es como se lee una auditoría.
        std::vector<int> kids = n.children;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) {
            return m_last[(size_t)a].ms > m_last[(size_t)b].ms; });
        for (int c : kids) dumpNode(c, frames, out);
    }

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
