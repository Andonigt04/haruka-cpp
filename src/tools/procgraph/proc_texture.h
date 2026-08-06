#pragma once

#include "proc_graph.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_types.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// RGBAImage — 2D grid of RGBA8 pixels
// ===========================================================================
struct RGBAImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA interleaved, row-major

    RGBAImage() = default;

    RGBAImage(int w, int h)
        : width(w), height(h), pixels((size_t)w * h * 4, 255) {}

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign((size_t)w * h * 4, 255);
    }

    uint8_t* data() { return pixels.data(); }
    const uint8_t* data() const { return pixels.data(); }
    size_t sizeBytes() const { return pixels.size(); }

    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        size_t idx = ((size_t)y * width + x) * 4;
        pixels[idx + 0] = r;
        pixels[idx + 1] = g;
        pixels[idx + 2] = b;
        pixels[idx + 3] = a;
    }
};

// ===========================================================================
// evaluateToRGBA — evaluate graph output as Vec4 across a 2D grid
//
// Evaluates `graph` at `(x, y)` for each pixel, reading the Vec4 output of
// `(nodeIndex, outputIndex)`. The graph's float outputs are treated as the
// RGBA channels and multiplied by 255.
//
// - ox, oy, scale: coordinate transform (same as evaluateToGrid)
// - channelMapping: which graph output channels map to R, G, B, A
//   Default is {0,1,2,3} meaning output[0]=R, output[1]=G, etc.
//   If the graph outputs fewer Vec4 components, use channelMapping to
//   specify which ones to read (e.g. {0} for a single-float graph means
//   all RGBA = same value).
// ===========================================================================
inline RGBAImage evaluateToRGBA(const Graph& graph,
                                 int nodeIndex, int outputIndex,
                                 int width, int height,
                                 float ox = 0, float oy = 0, float scale = 1.0f,
                                 const int channelMapping[4] = nullptr)
{
    RGBAImage img(width, height);
    Graph& g = const_cast<Graph&>(graph);
    int cmap[4] = {0, 1, 2, 3};
    if (channelMapping) {
        cmap[0] = channelMapping[0];
        cmap[1] = channelMapping[1];
        cmap[2] = channelMapping[2];
        cmap[3] = channelMapping[3];
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = (float(x) + ox) * scale;
            float fy = (float(y) + oy) * scale;
            Value v = g.evaluate(nodeIndex, outputIndex, fx, fy, 0);
            // Read the mapped channels from the Value's data array
            float r = (cmap[0] >= 0 && cmap[0] < 4) ? v.data[cmap[0]] : 0;
            float gv = (cmap[1] >= 0 && cmap[1] < 4) ? v.data[cmap[1]] : 0;
            float b = (cmap[2] >= 0 && cmap[2] < 4) ? v.data[cmap[2]] : 0;
            float a = (cmap[3] >= 0 && cmap[3] < 4) ? v.data[cmap[3]] : 1.0f;
            img.setPixel(x, y,
                (uint8_t)(glm::clamp(r, 0.0f, 1.0f) * 255.0f),
                (uint8_t)(glm::clamp(gv, 0.0f, 1.0f) * 255.0f),
                (uint8_t)(glm::clamp(b, 0.0f, 1.0f) * 255.0f),
                (uint8_t)(glm::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
    return img;
}

// ===========================================================================
// evaluateToNormalMap — evaluate elevation graph and derive normals via
//   central differences, producing a tangent-space normal map.
//
// Evaluates `(nodeIndex, outputIndex)` as height values across the grid,
// then computes tangent-space normals using the screen-space derivatives.
// The result is an RGBA normal map (xy = tangent-space normal, z = 1,
// blue channel computed from red/green).
//
// Parameters:
//   heightScale: how much to amplify height differences (default 1.0)
// ===========================================================================
inline RGBAImage evaluateToNormalMap(const Graph& graph,
                                      int nodeIndex, int outputIndex,
                                      int width, int height,
                                      float ox = 0, float oy = 0, float scale = 1.0f,
                                      float heightScale = 1.0f)
{
    RGBAImage img(width, height);
    Graph& g = const_cast<Graph&>(graph);

    // Evaluate height field
    std::vector<float> heights((size_t)width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float fx = (float(x) + ox) * scale;
            float fy = (float(y) + oy) * scale;
            heights[(size_t)y * width + x] = g.evaluate(nodeIndex, outputIndex, fx, fy, 0).asFloat();
        }
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int xm = std::max(0, x - 1);
            int xp = std::min(width - 1, x + 1);
            int ym = std::max(0, y - 1);
            int yp = std::min(height - 1, y + 1);

            float hL = heights[(size_t)y * width + xm];
            float hR = heights[(size_t)y * width + xp];
            float hD = heights[(size_t)ym * width + x];
            float hU = heights[(size_t)yp * width + x];

            float dx = (hR - hL) * heightScale;
            float dy = (hU - hD) * heightScale;

            // Tangent-space normal: N = normalize(-dx, -dy, 1)
            float len = std::sqrt(dx * dx + dy * dy + 1.0f);
            float nx = -dx / len;
            float ny = -dy / len;
            float nz = 1.0f / len;

            img.setPixel(x, y,
                (uint8_t)((nx * 0.5f + 0.5f) * 255.0f),
                (uint8_t)((ny * 0.5f + 0.5f) * 255.0f),
                (uint8_t)((nz * 0.5f + 0.5f) * 255.0f),
                255);
        }
    }
    return img;
}

// ===========================================================================
// createRHIFromRGBA — upload an RGBAImage as an RHI texture
// ===========================================================================
inline Haruka::RHI::TextureHandle createRHIFromRGBA(const RGBAImage& img) {
    Haruka::RHI::Device* dev = Haruka::RHI::device();
    if (!dev || img.width <= 0 || img.height <= 0) return {};

    Haruka::RHI::TextureDesc td;
    td.width = (uint32_t)img.width;
    td.height = (uint32_t)img.height;
    td.format = Haruka::RHI::Format::RGBA8;
    td.mipmaps = true;
    td.initialData = img.data();
    return dev->createTexture(td);
}

}}} // namespace Haruka::Tools::ProcGraph
