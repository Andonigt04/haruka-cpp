#include "compute_shader.h"

#include <iostream>
#include <fstream>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <sstream>
#include <glm/gtc/type_ptr.hpp>

namespace Haruka { namespace Renderer {

ComputeShader::ComputeShader(const std::string& computePath)
{
    // Ruta RHI: el device crea el pipeline de compute (carga GLSL-first la ruta dada).
    if (RHI::Device* dev = RHI::device()) {
        RHI::PipelineDesc d;
        d.computePath = computePath.c_str();
        m_pipe = dev->createPipeline(d);
        ID = dev->nativeProgram(m_pipe);
    }
}

ComputeShader::~ComputeShader()
{
    if (RHI::valid(m_pipe)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pipe);
        return;
    }
    glDeleteProgram(ID);
}

void ComputeShader::use() const
{
    glUseProgram(ID);
}

void ComputeShader::dispatch(GLuint x, GLuint y, GLuint z) const
{
    glDispatchCompute(x, y, z);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

void ComputeShader::setInt(const std::string& name, int value) const
{
    GLint loc = getUniformLocation(name);
    glUniform1i(loc, value);
}

void ComputeShader::setFloat(const std::string& name, float value) const
{
    GLint loc = getUniformLocation(name);
    glUniform1f(loc, value);
}

void ComputeShader::setVec3(const std::string& name, const glm::vec3& value) const
{
    GLint loc = getUniformLocation(name);
    glUniform3fv(loc, 1, glm::value_ptr(value));
}

void ComputeShader::setMat4(const std::string& name, const glm::mat4& value) const
{
    GLint loc = getUniformLocation(name);
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(value));
}

std::string ComputeShader::readFile(const std::string& filePath)
{
    std::ifstream file(filePath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

GLint ComputeShader::getUniformLocation(const std::string& name) const
{
    return glGetUniformLocation(ID, name.c_str());
}

}} // namespace Haruka::Renderer
