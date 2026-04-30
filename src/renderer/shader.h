#ifndef SHADER_H
#define SHADER_H

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <string>
#include <fstream>
#include <vector>
#include <iostream>

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
        GLuint comp = loadSPV(GL_COMPUTE_SHADER, computePath);

        ID = glCreateProgram();
        glAttachShader(ID, comp);
        glLinkProgram(ID);
        checkErrors(ID, "PROGRAM");

        glDeleteShader(comp);
    }

    /** @brief Binds the program for subsequent draw calls. */
    void use() { glUseProgram(ID); }

    /** @name Uniform helpers */
    ///@{
    void setBool(const std::string& name, bool value) const {
        glUniform1i(glGetUniformLocation(ID, name.c_str()), (int)value);
    }
    void setInt(const std::string& name, int value) const {
        glUniform1i(glGetUniformLocation(ID, name.c_str()), value);
    }
    void setFloat(const std::string& name, float value) const {
        glUniform1f(glGetUniformLocation(ID, name.c_str()), value);
    }
    void setVec2(const std::string& name, const glm::vec2& v) const {
        glUniform2fv(glGetUniformLocation(ID, name.c_str()), 1, &v[0]);
    }
    void setVec2(const std::string& name, float x, float y) const {
        glUniform2f(glGetUniformLocation(ID, name.c_str()), x, y);
    }
    void setVec3(const std::string& name, const glm::vec3& v) const {
        glUniform3fv(glGetUniformLocation(ID, name.c_str()), 1, &v[0]);
    }
    void setVec3(const std::string& name, float x, float y, float z) const {
        glUniform3f(glGetUniformLocation(ID, name.c_str()), x, y, z);
    }
    void setVec4(const std::string& name, const glm::vec4& v) const {
        glUniform4fv(glGetUniformLocation(ID, name.c_str()), 1, &v[0]);
    }
    void setVec4(const std::string& name, float x, float y, float z, float w) const {
        glUniform4f(glGetUniformLocation(ID, name.c_str()), x, y, z, w);
    }
    void setMat2(const std::string& name, const glm::mat2& m) const {
        glUniformMatrix2fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, &m[0][0]);
    }
    void setMat3(const std::string& name, const glm::mat3& m) const {
        glUniformMatrix3fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, &m[0][0]);
    }
    void setMat4(const std::string& name, const glm::mat4& m) const {
        glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 1, GL_FALSE, &m[0][0]);
    }
    ///@}

private:
    inline static std::string s_baseDir;

    // Loads baseDir+path+".spv", uploads as SPIR-V, specialises at "main", returns shader object.
    static GLuint loadSPV(GLenum type, const char* glslPath) {
        std::string spvPath = s_baseDir + glslPath + ".spv";

        std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            std::cerr << "ERROR::SHADER::SPV_NOT_FOUND: " << spvPath << "\n";
            return 0;
        }
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

    static void checkErrors(GLuint object, const char* label) {
        GLint ok = GL_TRUE;
        char log[1024];
        if (glIsProgram(object)) {
            glGetProgramiv(object, GL_LINK_STATUS, &ok);
            if (!ok) {
                glGetProgramInfoLog(object, sizeof(log), nullptr, log);
                std::cerr << "ERROR::SHADER::LINK [" << label << "]\n" << log << "\n";
            }
        } else {
            glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                glGetShaderInfoLog(object, sizeof(log), nullptr, log);
                std::cerr << "ERROR::SHADER::SPECIALIZE [" << label << "]\n" << log << "\n";
            }
        }
    }
};
#endif
