#include "compute_shader.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include <iostream>
#include <fstream>
#include "core/logger.h"
#include "tools/error_reporter.h"

namespace Haruka { namespace Renderer {

ComputeShader::ComputeShader(const std::string& computePath)
    : ComputeShader(readFile(computePath).c_str()) {}

ComputeShader::ComputeShader(const char* computeSource) {
    if (!computeSource || !computeSource[0]) return;
    RHI::Device* dev = RHI::device();
    if (!dev) { HARUKA_LOGE("ComputeShader", "no RHI device"); return; }
    RHI::PipelineDesc pd;
    pd.computeSource = computeSource;
    m_pipe = dev->createPipeline(pd);
    if (!RHI::valid(m_pipe))
        HARUKA_LOGE("ComputeShader", "pipeline creation failed");
}

ComputeShader::~ComputeShader() {
    if (RHI::valid(m_pipe)) {
        if (RHI::Device* dev = RHI::device())
            dev->destroy(m_pipe);
    }
}

void ComputeShader::use() const {
    // No-op: the pipeline is bound via RHI Context::bindPipeline in dispatch
}

void ComputeShader::dispatch(unsigned int x, unsigned int y, unsigned int z, RHI::Context* ctx) const {
    if (!RHI::valid(m_pipe)) return;
    RHI::Context* c = ctx;
    if (!c) {
        RHI::Device* dev = RHI::device();
        if (!dev) return;
        c = dev->beginFrame();
    }
    c->bindPipeline(m_pipe);
    c->dispatch(x, y, z);
}

bool ComputeShader::linked() const {
    return RHI::valid(m_pipe);
}

std::string ComputeShader::readFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        HARUKA_LOGE("ComputeShader", "cannot open %s", filePath.c_str());
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}} // namespace Haruka::Renderer