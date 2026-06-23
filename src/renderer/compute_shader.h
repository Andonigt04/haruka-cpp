/**
 * @file compute_shader.h
 * @brief OpenGL compute shader wrapper with uniform setters and dispatch helper.
 */
#ifndef COMPUTE_SHADER_H
#define COMPUTE_SHADER_H

#include <glad/glad.h>
#include <string>
#include <glm/glm.hpp>

namespace Haruka { namespace Renderer {

class ComputeShader
{
public:
    /** @brief Builds compute shader program from file path. */
    ComputeShader(const std::string& computePath);
    ~ComputeShader();

    /** @brief Binds compute program for subsequent uniform/dispatch calls. */
    void use() const;
    /** @brief Dispatches compute workload dimensions. */
    void dispatch(GLuint x, GLuint y, GLuint z) const;

    /** @name Uniform setters */
    ///@{
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec3(const std::string& name, const glm::vec3& value) const;
    void setMat4(const std::string& name, const glm::mat4& value) const;
    ///@}
    
    GLuint getID() const { return ID; }
    /** @brief True solo si el programa linkó OK (getID()!=0 NO basta: el handle existe
     *  aunque el link falle, p.ej. si no se encontró el fichero del shader). */
    bool linked() const {
        if (ID == 0) return false;
        GLint ok = GL_FALSE; glGetProgramiv(ID, GL_LINK_STATUS, &ok);
        return ok == GL_TRUE;
    }
private:
    GLuint ID;
    std::string readFile(const std::string& filePath);
    GLint getUniformLocation(const std::string& name) const;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::ComputeShader;
namespace Haruka { using Renderer::ComputeShader; }
#endif