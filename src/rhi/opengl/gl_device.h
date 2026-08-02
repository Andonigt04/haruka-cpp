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
    struct GLBuffer  { GLuint id = 0; GLenum target = GL_ARRAY_BUFFER; void* mapped = nullptr; };
    struct GLTexture { GLuint id = 0; GLenum target = GL_TEXTURE_2D; };
    struct GLSampler { GLuint id = 0; };

    struct GLPipeline
    {
        GLuint     program  = 0;
        GLuint     vao      = 0;              // 0 en pipelines de compute
        GLenum     topology = GL_TRIANGLES;
        GLint      patchVertices = 0;   // >0 solo con topology == GL_PATCHES
        GLsizei    strides[8] = {0};   // stride por BINDING (máx 8 vertex buffers)
        uint32_t   bindingCount = 0;
        DepthState depth;
        BlendState blend;
        CullMode   cull = CullMode::None;
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

    struct GLFence { GLsync sync = nullptr; };

    class GLDevice : public Device
    {
        public:
            explicit GLDevice(SDL_Window* window);
            ~GLDevice() override;

            friend class GLContext;

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
            const void*      mappedData(BufferHandle) override;
            void             updateCubemapFace(TextureHandle tex, int face, int width, int height,
                                                Format format, const void* data) override;

            void destroy(BufferHandle) override;
            void destroy(TextureHandle) override;
            void destroy(SamplerHandle) override;
            void destroy(PipelineHandle) override;
            void destroy(RenderPassHandle) override;

            Context* beginFrame() override;
            void     endFrame() override;
            Backend  backend() const override { return Backend::OpenGL; }
            void     readPixels(int x, int y, int w, int h, Format format, void* data) override;

            bool ready() const { return m_glContext != nullptr; }

            // --- Resolución handle -> recurso GL (la usa GLContext). null si inválido. ---
            const GLBuffer*       buffer(BufferHandle h) const           { return get(m_buffers, h.id); }
            const GLTexture*      texture(TextureHandle h) const         { return get(m_textures, h.id); }
            const GLSampler*      sampler(SamplerHandle h) const         { return get(m_samplers, h.id); }
            const GLPipeline*     pipeline(PipelineHandle h) const        { return get(m_pipelines, h.id); }
            const GLRenderTarget* renderTarget(RenderPassHandle h) const  { return get(m_targets, h.id); }
            const GLFence*        fence(FenceHandle h) const              { return get(m_fences, h.id); }

        private:
            template <class T>
            static const T* get(const std::vector<T>& pool, uint32_t id)
            {
                if (id == 0 || id > pool.size()) return nullptr;
                return &pool[id - 1];
            }

            // Asigna un slot: reusa uno liberado (free-list) o crece el pool. handle.id = slot+1.
            // Evita el crecimiento sin límite de los pools bajo churn de chunks (terreno).
            template <class T>
            static uint32_t alloc(std::vector<T>& pool, std::vector<uint32_t>& freeList, const T& v)
            {
                if (!freeList.empty()) { uint32_t s = freeList.back(); freeList.pop_back(); pool[s] = v; return s + 1; }
                pool.push_back(v); return (uint32_t)pool.size();
            }
            template <class T>
            static void release(std::vector<T>& pool, std::vector<uint32_t>& freeList, uint32_t id)
            {
                if (id == 0 || id > pool.size()) return;
                pool[id - 1] = T{};              // limpia el slot
                freeList.push_back(id - 1);      // disponible para reutilizar
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
            std::vector<GLFence>        m_fences;
            std::vector<uint32_t>       m_freeBuffers, m_freeTextures;   // slots reciclables (churn de chunks)
            std::vector<uint32_t>       m_freeSamplers, m_freePipelines, m_freeTargets, m_freeFences;
    };
}
