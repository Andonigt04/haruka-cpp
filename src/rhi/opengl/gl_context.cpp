/**
 * @file gl_context.cpp
 * @brief Implementación OpenGL de RHI::Context: traduce cada comando a llamadas GL inmediatas.
 */
#include "rhi/opengl/gl_context.h"
#include "rhi/opengl/gl_device.h"
#include <cstdio>

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
        // Copiamos POR VALOR lo que los draws necesitan: el pool del device puede realocar y un
        // puntero guardado quedaría colgando (ver gl_context.h).
        m_vao          = p->vao;
        m_topology     = (unsigned int)p->topology;
        m_bindingCount = p->bindingCount;
        for (unsigned int i = 0; i < 8; ++i) m_strides[i] = (int)p->strides[i];
        m_hasPipeline  = true;

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
        if (!t) return;
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
        glDispatchCompute(x, y, z);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);   // conservador: asegura visibilidad tras el compute
    }
}
