#include "compute_postprocess.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

ComputePostProcess::ComputePostProcess() {}

ComputePostProcess::~ComputePostProcess() {}

void ComputePostProcess::init(int width, int height) {
    screenWidth  = width;
    screenHeight = height;
    initialized = true;
}

}} // namespace Haruka::Renderer