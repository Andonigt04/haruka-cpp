#pragma once
#include <cmath>
#include <glm/glm.hpp>

namespace Haruka { namespace Planet {

// ===========================================================================
// Erosion layer — optional
// Stream-power + thermal erosion model evaluated per-vertex.
// Uses analytical slope from height function central differences.
// ===========================================================================

struct ErosionConfig {
    double streamPowerK = 0.02;    // erosion rate
    double thermalK     = 0.01;    // thermal diffusion rate
    double talusAngle   = 1.1;     // radians — angle of repose
    double hillslopeK   = 0.005;   // hillslope diffusion
    int    iterations   = 3;       // how many passes (more = more eroded)
};

struct ErosionOutput {
    // Apply erosion iteratively to a height field sampled on a grid.
    // Modifies `heights` in place. For per-vertex use, see `sample()` below.
    template<typename F>
    static void erodeGrid(std::vector<double>& heights, int width, int height,
                          double cellSize, const ErosionConfig& cfg,
                          F&& heightFn) {
        // Simple iterative stream-power + thermal erosion
        int n = width * height;
        std::vector<double> flow(n, 1.0); // uniform rainfall

        for (int iter = 0; iter < cfg.iterations; iter++) {
            // 1. Flow accumulation (D8)
            std::fill(flow.begin(), flow.end(), 1.0);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int idx = y * width + x;
                    if (x == 0 || x == width-1 || y == 0 || y == height-1) continue;

                    // Find steepest downslope neighbor
                    double hc = heights[idx];
                    double bestSlope = 0;
                    int bestNx = x, bestNy = y;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx, ny = y + dy;
                            double hn = heights[ny * width + nx];
                            double dist = std::sqrt((double)(dx*dx + dy*dy));
                            double slope = (hc - hn) / (cellSize * dist);
                            if (slope > bestSlope) {
                                bestSlope = slope;
                                bestNx = nx; bestNy = ny;
                            }
                        }
                    }

                    if (bestNx != x || bestNy != y) {
                        flow[bestNy * width + bestNx] += flow[idx];
                    }
                }
            }

            // 2. Stream-power erosion
            for (int y = 1; y < height-1; y++) {
                for (int x = 1; x < width-1; x++) {
                    int idx = y * width + x;
                    double hc = heights[idx];

                    // Slope (central differences)
                    double sx = (heights[y * width + x + 1] - heights[y * width + x - 1]) / (2 * cellSize);
                    double sy = (heights[(y+1) * width + x] - heights[(y-1) * width + x]) / (2 * cellSize);
                    double slope = std::sqrt(sx*sx + sy*sy);

                    // Stream power: E = K * A^0.5 * S^2
                    double area = flow[idx];
                    double erosion = cfg.streamPowerK * std::sqrt(area) * slope * slope * cellSize * 0.1;
                    heights[idx] -= erosion;

                    // Thermal erosion: relax steep slopes
                    if (slope * cellSize > cfg.talusAngle * cellSize) {
                        heights[idx] -= cfg.thermalK * (slope - cfg.talusAngle) * cellSize;
                    }

                    // Hillslope diffusion
                    double lap = (heights[y * width + x + 1] + heights[y * width + x - 1] +
                                  heights[(y+1) * width + x] + heights[(y-1) * width + x] -
                                  4 * hc) / (cellSize * cellSize);
                    heights[idx] += cfg.hillslopeK * lap;
                }
            }
        }
    }

    // Per-vertex erosion modifier (cheaper, single evaluation)
    // Returns elevation change in km
    template<typename F>
    static double sample(const glm::dvec3& dir, double baseElev, double cellSize,
                         const ErosionConfig& cfg, F&& heightFn) {
        // Compute local slope from central differences on height function
        const double eps = cellSize * 0.1;
        double h_center = heightFn(dir);
        glm::dvec3 dx, dy;
        // Use face-relative directions for slope computation
        // For slope, we need tangent directions on the sphere
        glm::dvec3 tangentU = glm::normalize(glm::cross(dir, glm::dvec3(0, 1, 0)));
        if (glm::length(tangentU) < 0.01)
            tangentU = glm::normalize(glm::cross(dir, glm::dvec3(1, 0, 0)));
        glm::dvec3 tangentV = glm::normalize(glm::cross(dir, tangentU));

        double h_u1 = heightFn(glm::normalize(dir + tangentU * eps));
        double h_u2 = heightFn(glm::normalize(dir - tangentU * eps));
        double h_v1 = heightFn(glm::normalize(dir + tangentV * eps));
        double h_v2 = heightFn(glm::normalize(dir - tangentV * eps));

        double slopeU = (h_u1 - h_u2) / (2 * eps);
        double slopeV = (h_v1 - h_v2) / (2 * eps);
        double slope = std::sqrt(slopeU*slopeU + slopeV*slopeV);

        // Curvature (second derivative)
        double curvU = (h_u1 + h_u2 - 2 * h_center) / (eps * eps);
        double curvV = (h_v1 + h_v2 - 2 * h_center) / (eps * eps);
        double curvature = curvU + curvV;

        // Stream power erosion
        double erosion = cfg.streamPowerK * slope * slope * std::max(0.0, curvature * 100.0);

        // Thermal: limit slope
        double thermal = 0;
        if (slope > cfg.talusAngle) {
            thermal = cfg.thermalK * (slope - cfg.talusAngle) * cellSize;
        }

        return -(erosion + thermal);
    }
};

}} // namespace Haruka::Planet
