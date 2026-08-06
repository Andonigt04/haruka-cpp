#ifndef SHADER_H
#define SHADER_H

#include <glm/glm.hpp>
#include <string>
#include <fstream>
#include <vector>
#include <iostream>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "core/asset_paths.h"

namespace Haruka { namespace Renderer {

class Shader {
public:
    unsigned int ID;

    static void setBaseDir(const char* dir) {
        if (!dir) { s_baseDir.clear(); Haruka::AssetPaths::setAssetsDir({}); return; }
        s_baseDir = dir;
        if (!s_baseDir.empty() && s_baseDir.back() != '/' && s_baseDir.back() != '\\')
            s_baseDir += '/';
        Haruka::AssetPaths::setAssetsDir(s_baseDir);
        // Raíz de los `#include` de GLSL. Va aquí, con las otras dos, por la misma razón que dice
        // el comentario de AssetPaths: tenerlas separadas es cómo se coló que una ruta siguiera
        // yendo por el cwd mientras las demás ya iban por la raíz.
        Haruka::RHI::setShaderIncludeDir(Haruka::AssetPaths::shaders());
    }

    static const std::string& baseDir() { return s_baseDir; }

    /** @brief Builds a program from vertex + fragment (+ optional geometry) files. */
    Shader(const char* vertexPath, const char* fragmentPath, const char* geometryPath = nullptr) {
        Haruka::RHI::Device* dev = Haruka::RHI::device();
        std::string vp = s_baseDir + vertexPath, fp = s_baseDir + fragmentPath, gp;
        Haruka::RHI::PipelineDesc d;
        d.vertexPath = vp.c_str();
        d.fragmentPath = fp.c_str();
        if (geometryPath) { gp = s_baseDir + geometryPath; d.geometryPath = gp.c_str(); }
        m_pipe = dev->createPipeline(d);
        ID = dev->nativeProgram(m_pipe);
    }

    /** @brief Builds a compute program. */
    explicit Shader(const char* computePath) {
        Haruka::RHI::Device* dev = Haruka::RHI::device();
        std::string cp = s_baseDir + computePath;
        Haruka::RHI::PipelineDesc d;
        d.computePath = cp.c_str();
        m_pipe = dev->createPipeline(d);
        ID = dev->nativeProgram(m_pipe);
    }

    bool linked() const {
        return Haruka::RHI::valid(m_pipe);
    }

    void use() const;

    Haruka::RHI::PipelineHandle handle() const { return m_pipe; }

private:
    Haruka::RHI::PipelineHandle m_pipe;
    inline static std::string s_baseDir = "assets/";
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Shader;
namespace Haruka { using Renderer::Shader; }
#endif
