/**
#include <cstdlib>   // getenv: HARUKA_GL_TEXPROBE
 * @file gl_context.cpp
 * @brief Implementación OpenGL de RHI::Context: traduce cada comando a llamadas GL inmediatas.
 */
#include "rhi/opengl/gl_context.h"
#include "rhi/opengl/gl_device.h"
#include <cstdio>
#include <algorithm>

namespace Haruka::RHI::opengl
{
    GLContext::~GLContext()
    {
        if (m_cubeFbo) glDeleteFramebuffers(1, &m_cubeFbo);
    }

    void GLContext::beginRenderPass(RenderPassHandle target, const ClearValues& c)
    {
        if (target.id == 0)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);          // backbuffer (pantalla)
        }
        else if (const GLRenderTarget* rt = m_device->renderTarget(target))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
            glViewport(0, 0, (GLsizei)rt->width, (GLsizei)rt->height);
        }

        GLbitfield mask = 0;
        if (c.clearColor)
        {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(c.color[0], c.color[1], c.color[2], c.color[3]);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (c.clearDepth)
        {
            glDepthMask(GL_TRUE);                            // clear de depth requiere write habilitado
            glClearDepth(c.depth);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (mask) glClear(mask);
    }

    void GLContext::beginRenderPassCubemapFace(TextureHandle th, int face, int mipLevel, const ClearValues& c)
    {
        const GLTexture* t = m_device->texture(th);
        if (!t) return;
        if (!m_cubeFbo) glGenFramebuffers(1, &m_cubeFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_cubeFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, t->id, mipLevel);
        // size from the texture dimensions (top-level, mip-scaled)
        GLint w = 0, h = 0;
        glGetTextureLevelParameteriv(t->id, 0, GL_TEXTURE_WIDTH, &w);
        glGetTextureLevelParameteriv(t->id, 0, GL_TEXTURE_HEIGHT, &h);
        int fw = std::max(1, w >> mipLevel);
        int fh = std::max(1, h >> mipLevel);
        glViewport(0, 0, (GLsizei)fw, (GLsizei)fh);

        GLbitfield mask = 0;
        if (c.clearColor)
        {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(c.color[0], c.color[1], c.color[2], c.color[3]);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (c.clearDepth)
        {
            glDepthMask(GL_TRUE);
            glClearDepth(c.depth);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (mask) glClear(mask);
    }

    void GLContext::endRenderPass()
    {
        // En GL no hay "fin de pass" explícito; el siguiente beginRenderPass rebindea el FBO.
    }

    void GLContext::setViewport(int x, int y, int w, int h)
    {
        glViewport(x, y, (GLsizei)w, (GLsizei)h);
    }

    void GLContext::clear(float r, float g, float b, float a)
    {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void GLContext::bindPipeline(PipelineHandle h)
    {
        const GLPipeline* p = m_device->pipeline(h);
        if (!p) return;
        // Copiamos POR VALOR lo que los draws necesitan: el pool del device puede realocar y un
        // puntero guardado quedaría colgando (ver gl_context.h).
        m_vao          = p->vao;
        m_topology     = (unsigned int)p->topology;
        m_bindingCount = p->bindingCount;
        for (unsigned int i = 0; i < 8; ++i) m_strides[i] = (int)p->strides[i];
        m_hasPipeline  = true;

        glUseProgram(p->program);
        if (p->compute) return;   // los pipelines de compute no tienen estado de rasterizado

        // Tamaño del parche de TESELACIÓN. Va aquí y no en el draw porque glPatchParameteri es
        // estado GLOBAL de GL: puesto en el draw, dos pipelines de teselación con distinto tamaño
        // de parche se pisarían, y el síntoma sería geometría rota en el segundo — lejos de la causa.
        if (p->patchVertices > 0) glPatchParameteri(GL_PATCH_VERTICES, p->patchVertices);

        if (p->depth.test)
        {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(p->depth.write ? GL_TRUE : GL_FALSE);
            glDepthFunc(toCompare(p->depth.compare));
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }

        // Sesgo de profundidad para superficies coplanares a propósito. Se resetea SIEMPRE (rama
        // else) porque glPolygonOffset es estado global: dejarlo puesto contaminaría cualquier
        // pipeline que viniera detrás, y el síntoma —una malla que se hunde en otra tres draws más
        // allá— no apunta a esta línea.
        if (p->depth.biasConstant != 0.0f || p->depth.biasSlope != 0.0f)
        {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glEnable(GL_POLYGON_OFFSET_LINE);   // para que el wireframe de depuración coincida
            glPolygonOffset(p->depth.biasSlope, p->depth.biasConstant);
        }
        else
        {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDisable(GL_POLYGON_OFFSET_LINE);
        }

        if (p->blend.enable)
        {
            glEnable(GL_BLEND);
            // Additive (src·a + dst) = emisivo/radiante (partículas). Alpha = transparencia normal.
            if (p->blend.mode == BlendMode::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            else                                      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glDisable(GL_BLEND);
        }

        if (p->cull == CullMode::None)
        {
            glDisable(GL_CULL_FACE);      // a dos caras (agua, vegetación)
        }
        else
        {
            glEnable(GL_CULL_FACE);
            glCullFace(p->cull == CullMode::Back ? GL_BACK : GL_FRONT);
        }

        // Puntos: el tamaño lo escribe el shader (gl_PointSize) y GL exige habilitarlo
        // explícitamente. En Vulkan es implícito → esto es un detalle del backend GL, no de la API.
        if (p->topology == GL_POINTS) glEnable(GL_PROGRAM_POINT_SIZE);
        else                          glDisable(GL_PROGRAM_POINT_SIZE);

        glBindVertexArray(p->vao);
    }

    // Ata el buffer a UN binding del layout. Una malla con streams separados (el terreno) llama
    // una vez por stream; el caso común (todo interleaved) usa solo el binding 0.
    void GLContext::bindVertexBuffer(BufferHandle h, uint32_t binding)
    {
        if (!m_hasPipeline || !m_vao || binding >= 8) return;
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glVertexArrayVertexBuffer(m_vao, binding, b->id, 0, (GLsizei)m_strides[binding]);
    }

    void GLContext::bindIndexBuffer(BufferHandle h)
    {
        if (!m_hasPipeline || !m_vao) return;
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glVertexArrayElementBuffer(m_vao, b->id);
    }

    void GLContext::bindUniformBuffer(uint32_t slot, BufferHandle h)
    {
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glBindBufferBase(GL_UNIFORM_BUFFER, slot, b->id);
    }

    void GLContext::bindStorageBuffer(uint32_t slot, BufferHandle h)
    {
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, b->id);
    }

    void GLContext::bindTexture(uint32_t slot, TextureHandle th, SamplerHandle sh)
    {
        const GLTexture* t = m_device->texture(th);
        if (!t) {
            // ⚠️ UNA VEZ POR RANURA, NO POR FRAME. Este aviso salio gritando 60 veces por segundo en
            // el juego y lo dejo inservible para probar. Un diagnostico que impide diagnosticar no
            // sirve — pero callarlo del todo tampoco: un slot sin atar es BASURA en Vulkan, y estos
            // respaldos existen justamente para que eso no pase.
            static bool warned[64] = {};
            if (slot < 64 && !warned[slot]) {
                warned[slot] = true;
                HARUKA_LOGW("RHI/GL", "bindTexture(%u): handle sin textura — el respaldo de esa ranura "
                                      "no existe (solo se avisa la primera vez)", slot);
            }
            return;
        }
        if (std::getenv("HARUKA_GL_TEXPROBE"))
            HARUKA_LOGI("RHI/GL", "bindTexture(slot=%u) -> id=%u", slot, (unsigned)t->id);
        glBindTextureUnit(slot, t->id);

        if (const GLSampler* s = m_device->sampler(sh))
            glBindSampler(slot, s->id);
        else
            glBindSampler(slot, 0);   // usa los parámetros propios de la textura
    }

    // Siempre la variante instanciada: con instanceCount=1 es EXACTAMENTE glDrawArrays, y así hay
    // un solo camino (igual que Vulkan, donde instanceCount es un campo del comando de dibujo).
    void GLContext::draw(uint32_t vertexCount, uint32_t first, uint32_t instanceCount)
    {
        if (!m_hasPipeline || !instanceCount) return;
        glDrawArraysInstanced((GLenum)m_topology, (GLint)first, (GLsizei)vertexCount,
                              (GLsizei)instanceCount);
    }

    void GLContext::drawIndexed(uint32_t indexCount, uint32_t first, uint32_t instanceCount)
    {
        if (!m_hasPipeline || !instanceCount) return;
        const void* offset = (const void*)(uintptr_t)(first * sizeof(uint32_t));
        glDrawElementsInstanced((GLenum)m_topology, (GLsizei)indexCount, GL_UNSIGNED_INT, offset,
                                (GLsizei)instanceCount);
    }

    // Multi-draw indirecto: la GPU lee los comandos del buffer. `stride` 0 = comandos contiguos
    // (GL lo admite); en Vulkan el stride es obligatorio, así que los llamadores deben pasarlo.
    void GLContext::drawIndexedIndirect(BufferHandle cmds, uint32_t drawCount,
                                        uint32_t stride, size_t offset)
    {
        if (!m_hasPipeline || !drawCount) return;
        const GLBuffer* b = m_device->buffer(cmds);
        if (!b) return;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b->id);
        glMultiDrawElementsIndirect((GLenum)m_topology, GL_UNSIGNED_INT,
                                    (const void*)(uintptr_t)offset,
                                    (GLsizei)drawCount, (GLsizei)stride);
    }

    void GLContext::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        if (std::getenv("HARUKA_GL_TEXPROBE")) {
            // ¿Que ve GL en las unidades justo ANTES del dispatch? Es la pregunta que las sondas
            // anteriores no hacian: todas miraban NUESTRO camino, no el estado del driver.
            GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            for (GLuint u = 3; u <= 4; ++u) {
                GLint arr = 0, t2d = 0;
                glGetIntegeri_v(GL_TEXTURE_BINDING_2D_ARRAY, u, &arr);
                glGetIntegeri_v(GL_TEXTURE_BINDING_2D,       u, &t2d);
                HARUKA_LOGI("RHI/GL", "  pre-dispatch prog=%d unidad %u: 2D_ARRAY=%d 2D=%d",
                            prog, u, arr, t2d);
            }
        }
        glDispatchCompute(x, y, z);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);   // conservador: asegura visibilidad tras el compute
    }

    void GLContext::blitDepth(RenderPassHandle src, RenderPassHandle dst, int w, int h)
    {
        const GLuint srcFBO = (src.id == 0) ? 0 : (m_device->renderTarget(src) ? m_device->renderTarget(src)->fbo : 0);
        const GLuint dstFBO = (dst.id == 0) ? 0 : (m_device->renderTarget(dst) ? m_device->renderTarget(dst)->fbo : 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }

    void GLContext::blitColor(RenderPassHandle src, RenderPassHandle dst, int w, int h)
    {
        const GLuint srcFBO = (src.id == 0) ? 0 : (m_device->renderTarget(src) ? m_device->renderTarget(src)->fbo : 0);
        const GLuint dstFBO = (dst.id == 0) ? 0 : (m_device->renderTarget(dst) ? m_device->renderTarget(dst)->fbo : 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    void GLContext::memoryBarrier(uint32_t /*barriers*/)
    {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }

    // ------------------------------------------------------------------ fences
    FenceHandle GLContext::signalFence()
    {
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sync) return {};
        GLFence f;
        f.sync = sync;
        return FenceHandle{ GLDevice::alloc(m_device->m_fences, m_device->m_freeFences, f) };
    }

    bool GLContext::waitFence(FenceHandle h, uint64_t timeoutNs)
    {
        const GLFence* f = m_device->fence(h);
        if (!f || !f->sync) return true;
        GLbitfield flags = (timeoutNs == 0) ? 0 : GL_SYNC_FLUSH_COMMANDS_BIT;
        GLuint64 t = (timeoutNs == UINT64_MAX) ? GL_TIMEOUT_IGNORED : (GLuint64)timeoutNs;
        GLenum r = glClientWaitSync(f->sync, flags, t);
        return (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
    }

    void GLContext::deleteFence(FenceHandle h)
    {
        const GLFence* f = m_device->fence(h);
        if (f && f->sync) glDeleteSync(f->sync);
        GLDevice::release(m_device->m_fences, m_device->m_freeFences, h.id);
    }
}
