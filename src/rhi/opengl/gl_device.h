/**
 * @file gl_device.h
 * @brief Implementación OpenGL de RHI::Device. Posee las tablas de recursos GL y el contexto GL.
 */
#pragma once

#include "rhi/rhi_device.h"
#include "rhi/opengl/gl_common.h"
#include "rhi/opengl/gl_context.h"

#include <vector>
#include <memory>

namespace Haruka::RHI::opengl
{
    // --- Recursos GL internos. Un handle.id == índice+1 en la tabla correspondiente. ---
    struct GLBuffer  { GLuint id = 0; GLenum target = GL_ARRAY_BUFFER; };
    struct GLTexture { GLuint id = 0; };
    struct GLSampler { GLuint id = 0; };

    struct GLPipeline
    {
        GLuint     program  = 0;
        GLuint     vao      = 0;              // 0 en pipelines de compute
        GLenum     topology = GL_TRIANGLES;
        GLsizei    stride   = 0;
        DepthState depth;
        BlendState blend;
        bool       compute  = false;
    };

    struct GLRenderTarget
    {
        GLuint                     fbo      = 0;
        std::vector<TextureHandle> colors;    // 0..N attachments de color (owned)
        GLuint                     depthRbo = 0;   // profundidad como renderbuffer (o 0)
        TextureHandle              depthTex;       // profundidad como textura/cubemap (o vacío)
        uint32_t                   width = 0, height = 0;
    };

    class GLDevice : public Device
    {
        public:
            explicit GLDevice(SDL_Window* window);
            ~GLDevice() override;

            BufferHandle     createBuffer(BufferUsage, size_t bytes, const void* data, BufferMemory) override;
            void             updateBuffer(BufferHandle, size_t offset, size_t bytes, const void* data) override;
            void             uploadBuffer(BufferHandle, size_t bytes, const void* data) override;
            void             copyBuffer(BufferHandle src, BufferHandle dst, size_t srcOffset, size_t dstOffset, size_t bytes) override;
            TextureHandle    createTexture(const TextureDesc&) override;
            SamplerHandle    createSampler(const SamplerDesc&) override;
            PipelineHandle   createPipeline(const PipelineDesc&) override;
            RenderPassHandle createRenderTarget(const RenderTargetDesc&) override;
            TextureHandle    getColorTexture(RenderPassHandle, uint32_t index) override;
            TextureHandle    getDepthTexture(RenderPassHandle) override;
            uint32_t         nativeTexture(TextureHandle) override;
            uint32_t         nativeFramebuffer(RenderPassHandle) override;
            uint32_t         nativeProgram(PipelineHandle) override;
            uint32_t         nativeBuffer(BufferHandle) override;

            void destroy(BufferHandle) override;
            void destroy(TextureHandle) override;
            void destroy(SamplerHandle) override;
            void destroy(PipelineHandle) override;
            void destroy(RenderPassHandle) override;

            Context* beginFrame() override;
            void     endFrame() override;
            Backend  backend() const override { return Backend::OpenGL; }

            bool ready() const { return m_glContext != nullptr; }

            // --- Resolución handle -> recurso GL (la usa GLContext). null si inválido. ---
            const GLBuffer*       buffer(BufferHandle h) const           { return get(m_buffers, h.id); }
            const GLTexture*      texture(TextureHandle h) const         { return get(m_textures, h.id); }
            const GLSampler*      sampler(SamplerHandle h) const         { return get(m_samplers, h.id); }
            const GLPipeline*     pipeline(PipelineHandle h) const        { return get(m_pipelines, h.id); }
            const GLRenderTarget* renderTarget(RenderPassHandle h) const  { return get(m_targets, h.id); }

        private:
            template <class T>
            static const T* get(const std::vector<T>& pool, uint32_t id)
            {
                if (id == 0 || id > pool.size()) return nullptr;
                return &pool[id - 1];
            }

            SDL_Window*                m_window      = nullptr;
            SDL_GLContext              m_glContext   = nullptr;
            bool                       m_ownsContext = false;   // false = adoptado de la Window
            std::unique_ptr<GLContext> m_context;

            std::vector<GLBuffer>       m_buffers;
            std::vector<GLTexture>      m_textures;
            std::vector<GLSampler>      m_samplers;
            std::vector<GLPipeline>     m_pipelines;
            std::vector<GLRenderTarget> m_targets;
    };
}
