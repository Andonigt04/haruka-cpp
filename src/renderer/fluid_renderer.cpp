#include "fluid_renderer.h"
#include "shader.h"
#include "physics/fluid/pbf_solver.h"
#include "rhi/rhi_device.h"

namespace Haruka {

FluidRenderer::~FluidRenderer() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_particleBuf)) dev->destroy(m_particleBuf);
        if (RHI::valid(m_quadBuf))     dev->destroy(m_quadBuf);
        if (RHI::valid(m_depthPass))     dev->destroy(m_depthPass);
        if (RHI::valid(m_smoothPass[0])) dev->destroy(m_smoothPass[0]);
        if (RHI::valid(m_smoothPass[1])) dev->destroy(m_smoothPass[1]);
        if (RHI::valid(m_sceneCopyPass)) dev->destroy(m_sceneCopyPass);
    } else {
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_quadVBO) glDeleteBuffers(1, &m_quadVBO);
        if (m_depthFBO) glDeleteFramebuffers(1, &m_depthFBO);
        if (m_depthTex) glDeleteTextures(1, &m_depthTex);
        if (m_depthRB)  glDeleteRenderbuffers(1, &m_depthRB);
        if (m_smoothFBO[0]) glDeleteFramebuffers(2, m_smoothFBO);
        if (m_smoothTex[0]) glDeleteTextures(2, m_smoothTex);
        if (m_sceneCopyFBO) glDeleteFramebuffers(1, &m_sceneCopyFBO);
        if (m_sceneCopyTex) glDeleteTextures(1, &m_sceneCopyTex);
    }
    if (m_vao) glDeleteVertexArrays(1, &m_vao);          // VAOs siempre GL (transitorio)
    if (m_quadVAO) glDeleteVertexArrays(1, &m_quadVAO);
}

void FluidRenderer::ensureGL() {
    if (m_init) return;
    m_sphereShader  = std::make_unique<Shader>("shaders/fluid_particle.vert", "shaders/fluid_particle.frag");
    m_depthShader   = std::make_unique<Shader>("shaders/fluid_particle.vert", "shaders/fluid_depth.frag");
    m_blurShader    = std::make_unique<Shader>("shaders/screenquad.vert",     "shaders/fluid_blur.frag");
    m_surfaceShader = std::make_unique<Shader>("shaders/screenquad.vert",     "shaders/fluid_surface.frag");

    RHI::Device* dev = RHI::device();

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    if (dev) { m_particleBuf = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);
               m_vbo = dev->nativeBuffer(m_particleBuf); glBindBuffer(GL_ARRAY_BUFFER, m_vbo); }
    else     { glGenBuffers(1, &m_vbo); glBindBuffer(GL_ARRAY_BUFFER, m_vbo); }
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Fullscreen quad (triangle strip) for the smooth/composite passes.
    constexpr float quad[] = { -1,1, 0,1,  -1,-1, 0,0,  1,1, 1,1,  1,-1, 1,0 };
    glGenVertexArrays(1, &m_quadVAO);
    glBindVertexArray(m_quadVAO);
    if (dev) { m_quadBuf = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quad), quad);
               m_quadVBO = dev->nativeBuffer(m_quadBuf); glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO); }
    else     { glGenBuffers(1, &m_quadVBO); glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
               glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW); }
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    m_init = true;
}

void FluidRenderer::ensureTargets(int w, int h) {
    if (m_depthFBO && m_fbW == w && m_fbH == h) return;
    m_fbW = w; m_fbH = h;

    // Ruta RHI: 4 render targets. Al redimensionar se destruyen y recrean (storage inmutable).
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_depthPass))     dev->destroy(m_depthPass);
        if (RHI::valid(m_smoothPass[0])) dev->destroy(m_smoothPass[0]);
        if (RHI::valid(m_smoothPass[1])) dev->destroy(m_smoothPass[1]);
        if (RHI::valid(m_sceneCopyPass)) dev->destroy(m_sceneCopyPass);

        RHI::RenderTargetDesc dep;
        dep.width = w; dep.height = h; dep.colorFormats = { RHI::Format::R32F };
        dep.colorFilter = RHI::Filter::Nearest; dep.hasDepth = true; dep.depthFormat = RHI::Format::D24;
        m_depthPass = dev->createRenderTarget(dep);
        m_depthFBO = dev->nativeFramebuffer(m_depthPass);
        m_depthTex = dev->nativeTexture(dev->getColorTexture(m_depthPass, 0));

        for (int i = 0; i < 2; ++i) {
            RHI::RenderTargetDesc sm;
            sm.width = w; sm.height = h; sm.colorFormats = { RHI::Format::R32F };
            sm.colorFilter = RHI::Filter::Nearest; sm.hasDepth = false;
            m_smoothPass[i] = dev->createRenderTarget(sm);
            m_smoothFBO[i] = dev->nativeFramebuffer(m_smoothPass[i]);
            m_smoothTex[i] = dev->nativeTexture(dev->getColorTexture(m_smoothPass[i], 0));
        }

        RHI::RenderTargetDesc sc;
        sc.width = w; sc.height = h; sc.colorFormats = { RHI::Format::RGBA16F };
        sc.colorFilter = RHI::Filter::Linear; sc.hasDepth = false;
        m_sceneCopyPass = dev->createRenderTarget(sc);
        m_sceneCopyFBO = dev->nativeFramebuffer(m_sceneCopyPass);
        m_sceneCopyTex = dev->nativeTexture(dev->getColorTexture(m_sceneCopyPass, 0));
        return;
    }

    auto makeR32F = [&](GLuint& tex) {
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    // Depth pass target: R32F color + a depth renderbuffer (nearest particle wins).
    if (!m_depthFBO) glGenFramebuffers(1, &m_depthFBO);
    makeR32F(m_depthTex);
    if (!m_depthRB) glGenRenderbuffers(1, &m_depthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRB);
    // Match the scene target's depth format (HDR uses generic GL_DEPTH_COMPONENT)
    // so the depth blit for occlusion is format-compatible.
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_depthTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRB);

    // Smooth ping-pong targets (R32F, color only).
    for (int i = 0; i < 2; ++i) {
        if (!m_smoothFBO[i]) glGenFramebuffers(1, &m_smoothFBO[i]);
        makeR32F(m_smoothTex[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, m_smoothFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_smoothTex[i], 0);
    }

    // Scene-colour copy (RGBA16F covers both HDR and LDR scene targets) for refraction.
    if (!m_sceneCopyTex) glGenTextures(1, &m_sceneCopyTex);
    glBindTexture(GL_TEXTURE_2D, m_sceneCopyTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!m_sceneCopyFBO) glGenFramebuffers(1, &m_sceneCopyFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_sceneCopyFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneCopyTex, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FluidRenderer::uploadParticles(const Haruka::WorldPos& cameraPos, int n) {
    m_verts.resize(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        glm::vec3 cp = glm::vec3(m_solver->worldPos(i) - glm::dvec3(cameraPos));
        m_verts[i*3+0] = cp.x; m_verts[i*3+1] = cp.y; m_verts[i*3+2] = cp.z;
    }
    if (RHI::Device* dev = RHI::device()) {
        dev->uploadBuffer(m_particleBuf, m_verts.size()*sizeof(float), m_verts.data());
    } else {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_verts.size()*sizeof(float), m_verts.data(), GL_DYNAMIC_DRAW);
        glBindVertexArray(0);
    }
}

void FluidRenderer::render(const Haruka::WorldPos& cameraPos, int vpW, int vpH) {
    if (!m_solver || m_solver->count() == 0) return;
    ensureGL();

    const int n = m_solver->count();
    uploadParticles(cameraPos, n);

    // Use the ACTUAL bound viewport (handles Render Scale: the scene target may be
    // smaller than the window). Fall back to the passed size if unavailable.
    GLint vp[4] = {0, 0, vpW, vpH};
    glGetIntegerv(GL_VIEWPORT, vp);
    int w = vp[2] > 0 ? vp[2] : vpW;
    int h = vp[3] > 0 ? vp[3] : vpH;

    if (s_surfaceMode) renderSurface(n, w, h);
    else               renderSpheres(n, (float)h);
}

void FluidRenderer::renderSpheres(int n, float vpH) {
    m_sphereShader->use();
    glUniform1f(12, m_radius);
    glUniform1f(13, vpH);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS, 0, n);
    glBindVertexArray(0);
}

void FluidRenderer::renderSurface(int n, int w, int h) {
    GLint prevFBO = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
    ensureTargets(w, h);

    // 0) Copy the scene colour (for refraction) and depth (for occlusion) out of the
    //    scene target via blits — works even though the scene depth is a renderbuffer.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sceneCopyFBO);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depthFBO);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    // 1) Depth pass: particle spheres → nearest eye-depth into m_depthTex, depth-
    //    tested against the SCENE depth just blitted in (so terrain in front occludes
    //    the fluid). Clear colour only — keep the blitted scene depth.
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE);
    m_depthShader->use();
    glUniform1f(12, m_radius);
    glUniform1f(13, (float)h);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS, 0, n);

    // 2) Separable bilateral smooth: depthTex -> smooth[0] (H) -> smooth[1] (V).
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    m_blurShader->use();
    glUniform1f(1, 0.5f); // u_depthFalloff (m) — edge sharpness
    glBindVertexArray(m_quadVAO);

    glBindFramebuffer(GL_FRAMEBUFFER, m_smoothFBO[0]);
    glUniform2f(0, 1.0f / (float)w, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindFramebuffer(GL_FRAMEBUFFER, m_smoothFBO[1]);
    glUniform2f(0, 0.0f, 1.0f / (float)h);
    glBindTexture(GL_TEXTURE_2D, m_smoothTex[0]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 3) Composite the water surface into the scene target (prevFBO). Opaque: the
    //    shader samples the scene COPY for refraction, so no feedback loop and no
    //    blend needed; empty pixels are discarded.
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    m_surfaceShader->use();
    glUniform2f(0, 1.0f / (float)w, 1.0f / (float)h); // u_texel
    glUniform1f(1, 0.03f);                            // u_refractScale (uv units)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_smoothTex[1]); // smoothed depth (binding 0)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_sceneCopyTex); // scene behind (binding 1)
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glActiveTexture(GL_TEXTURE0);  // restore default active unit
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

} // namespace Haruka
