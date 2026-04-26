#include "debug_overlay.h"

#include <imgui.h>
#include <iostream>

#include "io/asset_streamer.h"

void DebugOverlay::init() {
    lastFrameTime = std::chrono::high_resolution_clock::now();
    fpsHistory.resize(120);  // Últimos 120 frames
    frameTimeHistory.resize(120);
    std::cout << "✓ Debug Overlay initialized\n";
}

void DebugOverlay::updateMetrics(const FrameMetrics& metrics) {
    lastMetrics = metrics;
    frameCount++;

    // Calcular FPS history
    updateFPSHistory();
}

void DebugOverlay::updateFPSHistory() {
    auto now = std::chrono::high_resolution_clock::now();
    auto deltaMs = std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
    lastFrameTime = now;

    float currentFPS = 1000.0f / deltaMs;
    
    fpsHistory.pop_front();
    fpsHistory.push_back(currentFPS);
    
    frameTimeHistory.pop_front();
    frameTimeHistory.push_back(deltaMs);

    // Calcular FPS promedio
    float sumFPS = 0.0f;
    for (float f : fpsHistory) {
        sumFPS += f;
    }
    averageFPS = sumFPS / fpsHistory.size();
}

FrameMetrics DebugOverlay::getAverageMetrics() const {
    FrameMetrics avg = lastMetrics;
    
    float sumFrameTime = 0.0f;
    for (float t : frameTimeHistory) {
        sumFrameTime += t;
    }
    avg.frameTimeMs = sumFrameTime / frameTimeHistory.size();
    avg.fps = averageFPS;

    return avg;
}

void DebugOverlay::render() {
    if (!visible) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.7f);

    if (!ImGui::Begin("Debug Overlay", &visible, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    // Selector de modo
    ImGui::Text("Overlay Mode:");
    ImGui::RadioButton("Minimal", (int*)&overlayMode, MINIMAL);
    ImGui::SameLine();
    ImGui::RadioButton("Standard", (int*)&overlayMode, STANDARD);
    ImGui::SameLine();
    ImGui::RadioButton("Detailed", (int*)&overlayMode, DETAILED);
    ImGui::SameLine();
    ImGui::RadioButton("Graphs", (int*)&overlayMode, GRAPH);

    ImGui::Separator();

    switch (overlayMode) {
        case MINIMAL:
            renderMinimal();
            break;
        case STANDARD:
            renderStandard();
            break;
        case DETAILED:
            renderDetailed();
            break;
        case GRAPH:
            renderGraphs();
            break;
    }

    ImGui::End();
}

void DebugOverlay::renderMinimal() {
    auto metrics = getAverageMetrics();
    
    ImGui::TextColored(
        metrics.fps >= 60.0f ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) :
        metrics.fps >= 30.0f ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) :
        ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
        "FPS: %.1f", metrics.fps
    );
    
    ImGui::Text("Frame: %.2f ms", metrics.frameTimeMs);
}

void DebugOverlay::renderStandard() {
    auto metrics = getAverageMetrics();
    auto streamStats = AssetStreamer::getInstance().getStats();

    // FPS
    ImGui::TextColored(
        metrics.fps >= 60.0f ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) :
        metrics.fps >= 30.0f ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) :
        ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
        "FPS: %.1f (%.2f ms)", metrics.fps, metrics.frameTimeMs
    );

    ImGui::Separator();

    // Rendering
    ImGui::TextUnformatted("=== RENDERING ===");
    ImGui::Text("Draw Calls: %d", metrics.drawCalls);
    ImGui::Text("Triangles: %d", metrics.totalTriangles);
    ImGui::Text("Vertices: %d", metrics.totalVertices);

    ImGui::Separator();

    // Memory
    ImGui::TextUnformatted("=== MEMORY ===");
    ImGui::Text("RAM: %.1f MB", metrics.ramUsage / (1024.0f * 1024.0f));
    ImGui::Text("VRAM: %.1f MB", metrics.vramUsage / (1024.0f * 1024.0f));

    ImGui::Separator();

    // Lighting
    ImGui::TextUnformatted("=== LIGHTING ===");
    ImGui::Text("Total Lights: %d", metrics.totalLights);
    ImGui::Text("Culled: %d", metrics.culledLights);

    ImGui::Separator();
    
    // Shadow Cascades
    ImGui::TextUnformatted("=== SHADOWS ===");
    ImGui::Text("Cascades: %d", metrics.numCascades);
    ImGui::Text("Active: %d", metrics.activeCascade);

    ImGui::Separator();

    // Asset Streaming
    ImGui::TextUnformatted("=== ASSET STREAMING ===");
    ImGui::Text("Loaded: %d", streamStats.loadedAssets);
    ImGui::Text("Pending: %d", streamStats.pendingAssets);
    ImGui::Text("Cache: %.1f%%", streamStats.cacheUtilization * 100.0f);
}

void DebugOverlay::renderDetailed() {
    auto metrics = getAverageMetrics();
    auto streamStats = AssetStreamer::getInstance().getStats();

    renderStandard();

    ImGui::Separator();
    ImGui::TextUnformatted("=== DETAILED STATS ===");

    ImGui::Text("RT Binds: %d", metrics.renderTargetBinds);
    ImGui::Text("Shader Switches: %d", metrics.shaderSwitches);

    if (streamStats.failedAssets > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Failed Assets: %d", streamStats.failedAssets);
    }

    ImGui::Text("Cache Memory: %.1f / %.1f MB",
        streamStats.totalCacheMemory / (1024.0f * 1024.0f),
        streamStats.maxCacheMemory / (1024.0f * 1024.0f));

    // Custom metrics
    if (!customMetrics.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("=== CUSTOM METRICS ===");
        for (const auto& [name, value] : customMetrics) {
            ImGui::Text("%s: %.2f", name.c_str(), value);
        }
    }

    if (!customMetricsStr.empty()) {
        for (const auto& [name, value] : customMetricsStr) {
            ImGui::Text("%s: %s", name.c_str(), value.c_str());
        }
    }
}

void DebugOverlay::renderGraphs() {
    auto metrics = getAverageMetrics();

    ImGui::TextColored(
        metrics.fps >= 60.0f ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) :
        metrics.fps >= 30.0f ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) :
        ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
        "FPS: %.1f", metrics.fps
    );

    // FPS Graph
    ImGui::PlotLines("FPS History", 
        [](void* data, int idx) -> float {
            auto* history = (std::deque<float>*)data;
            return (*history)[idx];
        },
        (void*)&fpsHistory,
        fpsHistory.size(),
        0,
        nullptr,
        0.0f,
        120.0f,
        ImVec2(0, 80));

    // Frame Time Graph
    ImGui::PlotLines("Frame Time (ms)",
        [](void* data, int idx) -> float {
            auto* history = (std::deque<float>*)data;
            return (*history)[idx];
        },
        (void*)&frameTimeHistory,
        frameTimeHistory.size(),
        0,
        nullptr,
        0.0f,
        33.0f,  // ~30 FPS target
        ImVec2(0, 80));

    ImGui::Separator();
    renderStandard();
}
