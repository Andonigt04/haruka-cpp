/**
 * @file shader.h
 * @brief OpenGL shader program loaded from pre-compiled SPIR-V binaries (OpenGL 4.6).
 */
#ifndef SHADER_H
#define SHADER_H

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <string>
#include <fstream>
#include <vector>
#include <iostream>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace Haruka { namespace Renderer {

/**
 * @brief OpenGL shader program loaded from pre-compiled SPIR-V binaries.
 *
 * Paths passed to constructors are the original GLSL paths; the class appends
 * ".spv" and resolves them relative to the base directory set via setBaseDir().
 * Requires OpenGL 4.6 (GL_ARB_gl_spirv — core since 4.6).
 *
 * Call Shader::setBaseDir(SDL_GetBasePath()) once at application init so the
 * .spv files are found regardless of the process working directory.
 */
class Shader {
public:
    unsigned int ID;

    /**
     * @brief Sets the root directory prepended to every shader path.
     * Must be called before constructing any Shader (typically in Application::init).
     * Trailing separator is added automatically.
     */
    static void setBaseDir(const char* dir) {
        if (!dir) { s_baseDir.clear(); return; }
        s_baseDir = dir;
        if (!s_baseDir.empty() && s_baseDir.back() != '/' && s_baseDir.back() != '\\')
            s_baseDir += '/';
    }

    /** @brief Builds a program from vertex + fragment (+ optional geometry) SPIR-V. */
    Shader(const char* vertexPath, const char* fragmentPath, const char* geometryPath = nullptr) {
        // Ruta RHI: el device crea el pipeline (GLSL-first en GL) y exponemos su programa nativo.
        if (Haruka::RHI::Device* dev = Haruka::RHI::device()) {
            std::string vp = s_baseDir + vertexPath, fp = s_baseDir + fragmentPath, gp;
            Haruka::RHI::PipelineDesc d;
            d.vertexPath = vp.c_str();
            d.fragmentPath = fp.c_str();
            if (geometryPath) { gp = s_baseDir + geometryPath; d.geometryPath = gp.c_str(); }
            m_pipe = dev->createPipeline(d);
            ID = dev->nativeProgram(m_pipe);
            return;
        }
        // Fallback (editor/headless sin device): GL directo, comportamiento idéntico al previo.
        GLuint vert = loadSPV(GL_VERTEX_SHADER,   vertexPath);
        GLuint frag = loadSPV(GL_FRAGMENT_SHADER, fragmentPath);
        GLuint geom = geometryPath ? loadSPV(GL_GEOMETRY_SHADER, geometryPath) : 0;

        ID = glCreateProgram();
        glAttachShader(ID, vert);
        glAttachShader(ID, frag);
        if (geom) glAttachShader(ID, geom);
        glLinkProgram(ID);
        checkErrors(ID, "PROGRAM");

        glDeleteShader(vert);
        glDeleteShader(frag);
        if (geom) glDeleteShader(geom);
    }

    /** @brief Builds a program from a single compute SPIR-V. */
    explicit Shader(const char* computePath) {
        if (Haruka::RHI::Device* dev = Haruka::RHI::device()) {
            std::string cp = s_baseDir + computePath;
            Haruka::RHI::PipelineDesc d;
            d.computePath = cp.c_str();
            m_pipe = dev->createPipeline(d);
            ID = dev->nativeProgram(m_pipe);
            return;
        }
        GLuint comp = loadSPV(GL_COMPUTE_SHADER, computePath);

        ID = glCreateProgram();
        glAttachShader(ID, comp);
        glLinkProgram(ID);
        checkErrors(ID, "PROGRAM");

        glDeleteShader(comp);
    }

    /** @brief Binds the program for subsequent draw calls. */
    void use() { glUseProgram(ID); }

    /** @brief True solo si el programa existe Y linkó OK. getID()!=0 NO basta (el handle
     *  se crea aunque el link falle) → usar esto antes de fijar uniforms/dibujar para no
     *  corromper el estado del programa anterior con un glUseProgram a uno inválido. */
    bool linked() const {
        if (ID == 0) return false;
        GLint ok = GL_FALSE; glGetProgramiv(ID, GL_LINK_STATUS, &ok);
        return ok == GL_TRUE;
    }

    /** @name Uniform helpers — name-based (GLSL) and location-based (SPIR-V) */
    ///@{
    void setBool (const std::string& name, bool value)          const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform1i (l, (int)value); }
    void setInt  (const std::string& name, int value)           const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform1i (l, value); }
    void setFloat(const std::string& name, float value)         const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform1f (l, value); }
    void setVec2 (const std::string& name, const glm::vec2& v)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform2fv(l, 1, &v[0]); }
    void setVec2 (const std::string& name, float x, float y)    const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform2f (l, x, y); }
    void setVec3 (const std::string& name, const glm::vec3& v)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform3fv(l, 1, &v[0]); }
    void setVec3 (const std::string& name, float x, float y, float z) const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform3f(l, x, y, z); }
    void setVec4 (const std::string& name, const glm::vec4& v)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform4fv(l, 1, &v[0]); }
    void setVec4 (const std::string& name, float x, float y, float z, float w) const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniform4f(l, x, y, z, w); }
    void setMat2 (const std::string& name, const glm::mat2& m)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniformMatrix2fv(l, 1, GL_FALSE, &m[0][0]); }
    void setMat3 (const std::string& name, const glm::mat3& m)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniformMatrix3fv(l, 1, GL_FALSE, &m[0][0]); }
    void setMat4 (const std::string& name, const glm::mat4& m)  const { GLint l = glGetUniformLocation(ID, name.c_str()); if (l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, &m[0][0]); }

    // Location-based overloads for SPIR-V shaders where name lookup returns -1.
    void setBool (GLint loc, bool value)           const { glUniform1i (loc, (int)value); }
    void setInt  (GLint loc, int value)            const { glUniform1i (loc, value); }
    void setFloat(GLint loc, float value)          const { glUniform1f (loc, value); }
    void setVec2 (GLint loc, const glm::vec2& v)   const { glUniform2fv(loc, 1, &v[0]); }
    void setVec3 (GLint loc, const glm::vec3& v)   const { glUniform3fv(loc, 1, &v[0]); }
    void setVec4 (GLint loc, const glm::vec4& v)   const { glUniform4fv(loc, 1, &v[0]); }
    void setMat3 (GLint loc, const glm::mat3& m)   const { glUniformMatrix3fv(loc, 1, GL_FALSE, &m[0][0]); }
    void setMat4 (GLint loc, const glm::mat4& m)   const { glUniformMatrix4fv(loc, 1, GL_FALSE, &m[0][0]); }
    ///@}

private:
    // Handle del pipeline RHI cuando el shader se crea vía device (ID = programa GL nativo).
    // Vacío si se usó la ruta de compatibilidad GL directa. El device libera el pipeline al cerrar.
    Haruka::RHI::PipelineHandle m_pipe;

    // Layout UNIFICADO dev==release: los paths ("shaders/x.vert") se enraízan SIEMPRE
    // bajo assets/ → en ambos casos resuelven a "assets/shaders/x.vert" relativo al exe.
    // Así dev y release usan la MISMA estructura (sin el clásico "va en dev, rota en
    // release"). El build debe dejar los shaders en <exe>/assets/shaders/ en ambos.
    inline static std::string s_baseDir = "assets/";

    // Loads a shader, preferring GLSL source over SPIR-V.
    //
    // Priority:
    //   1. GLSL source (baseDir+path) — present in dev builds, reliable on all drivers.
    //   2. SPIR-V binary (baseDir+path+".spv") — production builds that ship only .spv.
    //
    // Background: GL_ARB_gl_spirv / glSpecializeShader on AMD RadeonSI (mesa) has
    // incomplete support and can produce silently-invalid programs or hang. A draw
    // call with an invalid program causes a GPU fault which blocks all subsequent
    // GL calls. Compiling from GLSL source avoids this entirely.
    static GLuint loadSPV(GLenum type, const char* glslPath) {
        // 1. GLSL source — try first; always present during development.
        std::string glslFullPath = s_baseDir + glslPath;
        {
            std::ifstream g(glslFullPath);
            if (g.is_open()) {
                std::string src((std::istreambuf_iterator<char>(g)),
                                std::istreambuf_iterator<char>());
                const char* srcPtr = src.c_str();
                GLuint shader = glCreateShader(type);
                glShaderSource(shader, 1, &srcPtr, nullptr);
                glCompileShader(shader);
                checkErrors(shader, glslFullPath.c_str());
                return shader;
            }
        }

        // 2. SPIR-V binary — used only when GLSL source is absent (production).
        std::string spvPath = s_baseDir + glslPath + ".spv";
        {
            std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
            if (f.is_open()) {
                auto byteSize = static_cast<std::streamsize>(f.tellg());
                f.seekg(0, std::ios::beg);
                std::vector<char> buf(byteSize);
                f.read(buf.data(), byteSize);

                GLuint shader = glCreateShader(type);
                glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V,
                               buf.data(), static_cast<GLsizei>(byteSize));
                glSpecializeShader(shader, "main", 0, nullptr, nullptr);
                checkErrors(shader, spvPath.c_str());
                return shader;
            }
        }

        HARUKA_RENDERER_ERROR(ErrorCode::SHADER_COMPILATION_FAILED,
            "Shader not found: " + glslFullPath + " (or " + spvPath + ")");
        return 0;
    }

    static void checkErrors(GLuint object, const char* label) {
        GLint ok = GL_TRUE;
        char log[1024];
        if (glIsProgram(object)) {
            glGetProgramiv(object, GL_LINK_STATUS, &ok);
            if (!ok) {
                glGetProgramInfoLog(object, sizeof(log), nullptr, log);
                HARUKA_RENDERER_ERROR(ErrorCode::SHADER_COMPILATION_FAILED,
                    std::string("link error [") + label + "]: " + log);
            }
        } else {
            glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                glGetShaderInfoLog(object, sizeof(log), nullptr, log);
                HARUKA_RENDERER_ERROR(ErrorCode::SHADER_COMPILATION_FAILED,
                    std::string("specialize error [") + label + "]: " + log);
            }
        }
    }
};

}} // namespace Haruka::Renderer

// Back-compat aliases during the namespace migration.
using Haruka::Renderer::Shader;
namespace Haruka { using Renderer::Shader; }
#endif
