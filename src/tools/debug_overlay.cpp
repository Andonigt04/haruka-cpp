#include "tools/debug_overlay.h"

#include "rhi/rhi_device.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace Haruka { namespace Tools {

DebugOverlay& DebugOverlay::get() { static DebugOverlay o; return o; }

// ── lo estatico ───────────────────────────────────────────────────────────────────────────────

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string cpuName() {
#ifdef __linux__
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("model name", 0) == 0) { const size_t c = line.find(':'); if (c != std::string::npos) return trim(line.substr(c + 1)); }
#endif
    return "?";
}

/// MemTotal de /proc/meminfo (bytes), 0 si no se puede.
uint64_t memTotalBytes() {
#ifdef __linux__
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("MemTotal:", 0) == 0) return (uint64_t)std::atoll(line.c_str() + 9) * 1024ull;
#endif
    return 0;
}

/// VmRSS de /proc/self/status (bytes), 0 si no se puede.
uint64_t rssBytes() {
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return (uint64_t)std::atoll(line.c_str() + 6) * 1024ull;
#endif
    return 0;
}

const char* colorFor(float ms) { return ms > 33.4f ? "!!" : (ms > 16.8f ? "!" : ""); }

} // namespace

void DebugOverlay::readStatic() {
    m_staticRead = true;
    m_cpu = cpuName();
    if (RHI::Device* dev = RHI::device()) {
        m_gpu = dev->deviceName();
        m_api = (dev->backend() == RHI::Backend::Vulkan) ? "Vulkan" : "OpenGL";
    } else { m_gpu = "?"; m_api = "?"; }
    m_display = "?";
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        if (ids && n > 0) {
            const SDL_DisplayID id = SDL_GetPrimaryDisplay() ? SDL_GetPrimaryDisplay() : ids[0];
            const char* nm = SDL_GetDisplayName(id);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
            char buf[160];
            if (mode) std::snprintf(buf, sizeof buf, "%s %dx%d @%.0f Hz", nm ? nm : "?", mode->w, mode->h, mode->refresh_rate);
            else      std::snprintf(buf, sizeof buf, "%s", nm ? nm : "?");
            m_display = buf;
        }
        if (ids) SDL_free(ids);
    }
    m_totalBytes = memTotalBytes();
}

void DebugOverlay::readMemory() {
    m_rssBytes = rssBytes();
    if (m_totalBytes == 0) m_totalBytes = memTotalBytes();
}

// ── por frame ─────────────────────────────────────────────────────────────────────────────────

void DebugOverlay::pushFrame(const DebugFrameInfo& f) {
    m_last = f;
    const float dt = (f.dtSeconds > 0.0f) ? f.dtSeconds : 0.0f;
    if (dt > 0.0f) {
        m_dtMs.push_back(dt * 1000.0f);
        while (m_dtMs.size() > kWindow) m_dtMs.pop_front();
    }
    // Relojes propios: el dt del frame, que es lo unico que se tiene aqui.
    m_memClock += dt;
    m_netClock += dt;
    if (m_memLast < 0.0 || m_memClock - m_memLast >= 0.5) { m_memLast = m_memClock; readMemory(); }
    // tx/rx: KiB/s = delta de acumulados / delta de tiempo, cada segundo.
    if (m_netLast < 0.0) { m_netLast = m_netClock; m_txLast = f.net.txBytes; m_rxLast = f.net.rxBytes; }
    else if (m_netClock - m_netLast >= 1.0) {
        const double span = m_netClock - m_netLast;
        m_txKiBs = (float)((double)(f.net.txBytes - m_txLast) / 1024.0 / span);
        m_rxKiBs = (float)((double)(f.net.rxBytes - m_rxLast) / 1024.0 / span);
        m_netLast = m_netClock; m_txLast = f.net.txBytes; m_rxLast = f.net.rxBytes;
    }
}

// Percentiles del tiempo de frame sobre la ventana: 240 valores, ordenar una copia es nada. El
// rango es el indice mas cercano (p·(n−1) redondeado), sin interpolar: con 240 muestras p99.5 es la
// segunda peor, que es lo que se quiere ver (un tiron aislado sale ahi, no diluido).
void DebugOverlay::computeStats() {
    if (m_dtMs.empty()) return;
    std::vector<float> v(m_dtMs.begin(), m_dtMs.end());
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))]; };
    m_p50 = pct(0.50); m_p98 = pct(0.98); m_p995 = pct(0.995);
    double sum = 0.0; for (float x : v) sum += x;
    m_fps = (sum > 0.0) ? (float)((double)v.size() * 1000.0 / sum) : 0.0f;
}

void DebugOverlay::render() {
    if (!m_visible) return;
    if (!m_staticRead) readStatic();

    computeStats();

    // Anclado arriba a la derecha, sin fondo, sin bordes, sin raton, sin guardarse en el ini.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize
                                 | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("##debug_overlay", nullptr, flags)) { ImGui::End(); return; }

    // Sombra bajo el texto: sin fondo, sobre cielo claro no se lee. Se dibuja el texto dos veces.
    auto line = [&](const char* fmt, ...) {
        char buf[256];
        va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, 200), buf);
        ImGui::TextUnformatted(buf);
    };

    line("%.0f fps  %s", m_fps, colorFor(m_p50));
    line("p50 %.2f  p98 %.2f  p99.5 %.2f ms", m_p50, m_p98, m_p995);
    const double rssMiB = (double)m_rssBytes / 1048576.0, totMiB = (double)m_totalBytes / 1048576.0;
    line("mem %.0f %%  %.0f/%.0f MiB", totMiB > 0.0 ? 100.0 * rssMiB / totMiB : 0.0, rssMiB, totMiB);

    const DebugNetInfo& n = m_last.net;
    if (n.connected) {
        if (n.headMs >= 0.0f) line("head %.1f ms  min %.1f  avg %.1f  max %.1f", n.headMs, n.headMinMs, n.headAvgMs, n.headMaxMs);
        else                  line("head -- ms (sin muestra)");
        if (n.zoneMs >= 0.0f) line("zona %.1f ms  min %.1f  avg %.1f  max %.1f%s", n.zoneMs, n.zoneMinMs, n.zoneAvgMs, n.zoneMaxMs,
                                   n.pingsLost > 0 ? ("  perdidos " + std::to_string(n.pingsLost)).c_str() : "");
        else if (!n.zone.empty()) line("zona -- ms (sin pong%s)", n.pingsLost > 0 ? (": " + std::to_string(n.pingsLost) + " perdidos").c_str() : "");
        line("tx %.1f  rx %.1f KiB/s", m_txKiBs, m_rxKiBs);
    } else {
        line("integrado (sin DGS)");
    }
    {
        const double t = m_last.worldTimeS;
        const int h = (int)(t / 3600.0) % 24, m = (int)(t / 60.0) % 60, s = (int)t % 60;
        line("mundo %02d:%02d:%02d  (%.0f s)", h, m, s, t);
    }
    line("servidor %s", n.server.empty() ? "-" : n.server.c_str());
    if (!n.zone.empty()) line("zona %s", n.zone.c_str());
    line("objs %d  props %d%s", m_last.sceneObjects, m_last.scatterProps,
         n.ghosts > 0 ? ("  ghosts " + std::to_string(n.ghosts)).c_str() : "");
    line("xyz %.1f  %.1f  %.1f", m_last.worldPos.x, m_last.worldPos.y, m_last.worldPos.z);
    line("cpu %s", m_cpu.c_str());
    line("gpu %s", m_gpu.c_str());
    line("pantalla %s", m_display.c_str());
    line("api %s", m_api.c_str());
    ImGui::End();
}

}} // namespace Haruka::Tools
