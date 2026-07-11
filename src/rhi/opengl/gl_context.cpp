/**
 * @file gl_context.cpp
 * @brief Implementación OpenGL de RHI::Context: traduce cada comando a llamadas GL inmediatas.
 */
#include "rhi/opengl/gl_context.h"
#include "rhi/opengl/gl_device.h"

namespace Haruka::RHI::opengl
{
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
        m_pipeline = p;

        glUseProgram(p->program);
        if (p->compute) return;   // los pipelines de compute no tienen estado de rasterizado

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

        if (p->blend.enable)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glDisable(GL_BLEND);
        }

        glBindVertexArray(p->vao);
    }

    void GLContext::bindVertexBuffer(BufferHandle h)
    {
        if (!m_pipeline || !m_pipeline->vao) return;
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glVertexArrayVertexBuffer(m_pipeline->vao, 0, b->id, 0, m_pipeline->stride);
    }

    void GLContext::bindIndexBuffer(BufferHandle h)
    {
        if (!m_pipeline || !m_pipeline->vao) return;
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glVertexArrayElementBuffer(m_pipeline->vao, b->id);
    }

    void GLContext::bindUniformBuffer(uint32_t slot, BufferHandle h)
    {
        const GLBuffer* b = m_device->buffer(h);
        if (!b) return;
        glBindBufferBase(GL_UNIFORM_BUFFER, slot, b->id);
    }

    void GLContext::bindTexture(uint32_t slot, TextureHandle th, SamplerHandle sh)
    {
        const GLTexture* t = m_device->texture(th);
        if (!t) return;
        glBindTextureUnit(slot, t->id);

        if (const GLSampler* s = m_device->sampler(sh))
            glBindSampler(slot, s->id);
        else
            glBindSampler(slot, 0);   // usa los parámetros propios de la textura
    }

    void GLContext::draw(uint32_t vertexCount, uint32_t first)
    {
        if (!m_pipeline) return;
        glDrawArrays(m_pipeline->topology, (GLint)first, (GLsizei)vertexCount);
    }

    void GLContext::drawIndexed(uint32_t indexCount, uint32_t first)
    {
        if (!m_pipeline) return;
        const void* offset = (const void*)(uintptr_t)(first * sizeof(uint32_t));
        glDrawElements(m_pipeline->topology, (GLsizei)indexCount, GL_UNSIGNED_INT, offset);
    }

    void GLContext::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        glDispatchCompute(x, y, z);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);   // conservador: asegura visibilidad tras el compute
    }
}
