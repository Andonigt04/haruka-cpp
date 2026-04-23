#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <map>
#include <glm/glm.hpp>

/**
 * @brief Real-time performance and diagnostics overlay.
 *
 * Displays FPS, frame time, draw calls, memory usage, lighting counts,
 * asset streaming stats, and GPU metrics with minimal overhead.
 */

/** @brief Snapshot of runtime performance counters. */
struct FrameMetrics {
    float fps = 0.0f;
    float frameTimeMs = 0.0f;
    float gpuTimeMs = 0.0f;
    int drawCalls = 0;
    int renderTargetBinds = 0;
    int shaderSwitches = 0;
    
    // Memory
    size_t ramUsage = 0;
    size_t vramUsage = 0;
    
    // Lighting
    int totalLights = 0;
    int culledLights = 0;
    
    // Assets
    int loadedAssets = 0;
    int pendingAssets = 0;
    float cacheUtilization = 0.0f;
    
    // GPU
    int totalTriangles = 0;
    int totalVertices = 0;
    
    // Shadows
    int activeCascade = 0;  // Cascada activa actual
    int numCascades = 4;    // Total de cascadas
};

class DebugOverlay {
public:
    static DebugOverlay& getInstance() {
        static DebugOverlay instance;
        return instance;
    }

    /** @brief Initializes overlay state and timing. */
    void init();

    /** @brief Renders overlay UI. Call after rendering the frame. */
    void render();

    /** @brief Updates current metrics snapshot. */
    void updateMetrics(const FrameMetrics& metrics);

    /** @brief Toggles overlay visibility. */
    void toggle() { visible = !visible; }
    /** @brief Shows the overlay. */
    void show() { visible = true; }
    /** @brief Hides the overlay. */
    void hide() { visible = false; }
    /** @brief Returns current visibility state. */
    bool isVisible() const { return visible; }

    /** @brief Adds/updates a custom floating-point metric. */
    void addMetric(const std::string& name, float value) {
        customMetrics[name] = value;
    }

    void addMetric(const std::string& name, int value) {
        customMetrics[name] = static_cast<float>(value);
    }

    void addMetric(const std::string& name, const std::string& value) {
        customMetricsStr[name] = value;
    }

    /** @brief Available overlay presentation modes. */
    enum OverlayMode {
        MINIMAL,      // Solo FPS + frame time
        STANDARD,     // FPS, memoria, luces, assets
        DETAILED,     // Todo + historial de FPS
        GRAPH         // Gráficas de performance
    };

    void setMode(OverlayMode mode) { overlayMode = mode; }
    OverlayMode getMode() const { return overlayMode; }

    /** @brief Returns the most recent metrics snapshot. */
    FrameMetrics getLastMetrics() const { return lastMetrics; }
    
    /** @brief Returns averaged metrics over history buffers. */
    FrameMetrics getAverageMetrics() const;

    ~DebugOverlay() = default;

private:
    DebugOverlay() = default;

    void renderMinimal();
    void renderStandard();
    void renderDetailed();
    void renderGraphs();

    void updateFPSHistory();

    bool visible = true;
    OverlayMode overlayMode = OverlayMode::STANDARD;

    FrameMetrics lastMetrics;
    std::deque<float> fpsHistory;
    std::deque<float> frameTimeHistory;
    
    std::map<std::string, float> customMetrics;
    std::map<std::string, std::string> customMetricsStr;

    // Timing
    std::chrono::high_resolution_clock::time_point lastFrameTime;
    int frameCount = 0;
    float averageFPS = 0.0f;

    // Colores para overlay
    glm::vec4 colorNormal = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);   // Verde
    glm::vec4 colorWarning = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);  // Amarillo
    glm::vec4 colorCritical = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // Rojo
};
