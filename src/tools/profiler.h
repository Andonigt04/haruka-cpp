/**
 * @file profiler.h
 * @brief Lightweight scoped CPU profiler — measure before optimizing.
 *
 * Zero-config: drop HARUKA_PROFILE("name") at the top of a scope and its wall
 * time is accumulated per frame under that name. Call Profiler::get().newFrame()
 * once per frame; read the last frame's section times via sections().
 *
 * Single-threaded by design (the render/update hot path). Negligible overhead:
 * one high_resolution_clock read on enter/exit and a hashmap add. Compile out by
 * defining HARUKA_NO_PROFILER if ever needed.
 */
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace Haruka {

class Profiler {
public:
    static Profiler& get() { static Profiler p; return p; }

    /** @brief Adds elapsed milliseconds to a named section (this frame). */
    void add(const std::string& name, double ms) {
        m_accum[name] += ms;
    }

    /** @brief Snapshots accumulated times into the readable list, resets accum. */
    void newFrame() {
        m_last.clear();
        m_last.reserve(m_accum.size());
        for (auto& [k, v] : m_accum) m_last.push_back({k, v});
        std::sort(m_last.begin(), m_last.end(),
                  [](const Section& a, const Section& b){ return a.ms > b.ms; });
        m_accum.clear();
    }

    struct Section { std::string name; double ms; };
    /** @brief Last completed frame's section times, sorted descending. */
    const std::vector<Section>& sections() const { return m_last; }

    void setEnabled(bool e) { m_enabled = e; }
    bool enabled() const { return m_enabled; }

private:
    std::unordered_map<std::string, double> m_accum;
    std::vector<Section> m_last;
    bool m_enabled = true;
};

/** @brief RAII scope timer; accumulates its lifetime under `name`. */
class ScopedTimer {
public:
    explicit ScopedTimer(const char* name)
        : m_name(name), m_start(std::chrono::high_resolution_clock::now()) {}
    ~ScopedTimer() {
        if (!Profiler::get().enabled()) return;
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> ms = end - m_start;
        Profiler::get().add(m_name, ms.count());
    }
private:
    const char* m_name;
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
