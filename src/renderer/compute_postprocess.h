#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class ComputePostProcess {
public:
    enum ToneMapMode {
        TONE_LINEAR,
        TONE_REINHARD,
        TONE_ACES,
        TONE_FILMIC
    };

    ComputePostProcess();
    ~ComputePostProcess();

    void init(int width, int height);

    struct ComputeStats {
        int dispatchWidth;
        int dispatchHeight;
        int localGroupSize;
        float estimatedSpeedup;
    };

    ComputeStats getStats() const { return stats; }

private:
    std::vector<Haruka::RHI::PipelineHandle> m_pipes;
    int screenWidth = 0;
    int screenHeight = 0;
    bool initialized = false;
    int localGroupSize = 8;
    ComputeStats stats;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::ComputePostProcess;
namespace Haruka { using Renderer::ComputePostProcess; }