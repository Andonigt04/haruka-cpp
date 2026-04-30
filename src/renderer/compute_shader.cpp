#include "compute_shader.h"

#include <iostream>
#include <fstream>
#include "core/error_reporter.h"
#include <sstream>
#include <glm/gtc/type_ptr.hpp>

ComputeShader::ComputeShader(const std::string& computePath)
{
    std::string computeCode = readFile(computePath);
    const char* cCode = computeCode.c_str();

    GLuint compute = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(compute, 1, &cCode, nullptr);
    glCompileShader(compute);

    // Error checking
    int success;
    char infoLog[512];
    glGetShaderiv(compute, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(compute, 512, nullptr, infoLog);
        HARUKA_RENDERER_ERROR(ErrorCode::SHADER_COMPILATION_FAILED,
            std::string("compute shader compile error: ") + infoLog);
    }

    ID = glCreateProgram();
    glAttachShader(ID, compute);
    glLinkProgram(ID);

    glGetProgramiv(ID, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(ID, 512, nullptr, infoLog);
        HARUKA_RENDERER_ERROR(ErrorCode::SHADER_COMPILATION_FAILED,
            std::string("compute shader link error: ") + infoLog);
    }

    glDeleteShader(compute);
}

ComputeShader::~ComputeShader()
{
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