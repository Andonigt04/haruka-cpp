#ifndef COMPUTE_SHADER_H
#define COMPUTE_SHADER_H

#include <string>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"
#include "rhi/rhi_context.h"

namespace Haruka { namespace Renderer {

class ComputeShader
{
public:
    ComputeShader(const std::string& computePath);
    ComputeShader(const char* computeSource);
    ~ComputeShader();

    void use() const;
    void dispatch(unsigned int x, unsigned int y, unsigned int z, RHI::Context* ctx = nullptr) const;

    Haruka::RHI::PipelineHandle getPipe() const { return m_pipe; }
    bool linked() const;

private:
    Haruka::RHI::PipelineHandle m_pipe;
    std::string readFile(const std::string& filePath);
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::ComputeShader;
namespace Haruka { using Renderer::ComputeShader; }
#endif