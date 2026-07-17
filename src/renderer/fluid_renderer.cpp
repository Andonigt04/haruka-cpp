#include "fluid_renderer.h"
#include "shader.h"                 // Shader::baseDir (createPipeline NO resuelve rutas)
#include "physics/fluid/pbf_solver.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"

namespace Haruka {

FluidRenderer::~FluidRenderer() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_particleBuf)) dev->destroy(m_particleBuf);
        if (RHI::valid(m_quadBuf))     dev->destroy(m_quadBuf);
        if (RHI::valid(m_uboParams))   dev->destroy(m_uboParams);
        if (RHI::valid(m_depthPass))     dev->destroy(m_depthPass);
        if (RHI::valid(m_smoothPass[0])) dev->destroy(m_smoothPass[0]);
        if (RHI::valid(m_smoothPass[1])) dev->destroy(m_smoothPass[1]);
        if (RHI::valid(m_sceneCopyPass)) dev->destroy(m_sceneCopyPass);
        if (RHI::valid(m_psoSpheres)) dev->destroy(m_psoSpheres);
        if (RHI::valid(m_psoDepth))   dev->destroy(m_psoDepth);
        if (RHI::valid(m_psoBlur))    dev->destroy(m_psoBlur);
        if (RHI::valid(m_psoSurface)) dev->destroy(m_psoSurface);
    }
}

void FluidRenderer::ensureGL() {
    if (m_init) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    const std::string base = Shader::baseDir();
    const std::string vPart = base + "shaders/fluid_particle.vert";
    const std::string fPart = base + "shaders/fluid_particle.frag";
    const std::string fDep  = base + "shaders/fluid_depth.frag";
    const std::string vQuad = base + "shaders/screenquad.vert";
    const std::string fBlur = base + "shaders/fluid_blur.frag";
    const std::string fSurf = base + "shaders/fluid_surface.frag";

    // --- PSOs de PARTÍCULAS (point sprites). El tamaño del punto lo escribe el shader
    //     (gl_PointSize) → el backend GL habilita GL_PROGRAM_POINT_SIZE por la topología Points.
    RHI::PipelineDesc pp;
    pp.vertexPath              = vPart.c_str();
    pp.fragmentPath            = fPart.c_str();
    pp.vertexLayout.strides    = { (uint32_t)(3 * sizeof(float)) };
    pp.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 } };  // aPos (cámara-relativa)
    pp.topology     = RHI::PrimitiveTopology::Points;
    pp.depth.test   = true;
    pp.depth.write  = true;
    pp.blend.enable = false;
    pp.cull         = RHI::CullMode::None;
    m_psoSpheres = dev->createPipeline(pp);

    pp.fragmentPath = fDep.c_str();          // mismo vertex, distinto fragment (→ profundidad de ojo)
    m_psoDepth = dev->createPipeline(pp);

    // --- PSOs de QUAD a pantalla completa (triangle strip; sin depth).
    RHI::PipelineDesc pq;
    pq.vertexPath              = vQuad.c_str();
    pq.fragmentPath            = fBlur.c_str();
    pq.vertexLayout.strides    = { (uint32_t)(4 * sizeof(float)) };
    pq.vertexLayout.attributes = {
        { 0, 0,                        RHI::Format::RG32F, 0 },   // aPos
        { 1, (uint32_t)(2*sizeof(float)), RHI::Format::RG32F, 0 },// aTexCoords
    };
    pq.topology     = RHI::PrimitiveTopology::TriangleStrip;
    pq.depth.test   = false;
    pq.depth.write  = false;
    pq.blend.enable = false;
    pq.cull         = RHI::CullMode::None;
    m_psoBlur = dev->createPipeline(pq);

    pq.fragmentPath = fSurf.c_str();
    m_psoSurface = dev->createPipeline(pq);

    m_particleBuf = dev->createBuffer(RHI::BufferUsage::Vertex, 0, nullptr, RHI::BufferMemory::Stream);

    // Fullscreen quad (triangle strip) for the smooth/composite passes.
    constexpr float quad[] = { -1,1, 0,1,  -1,-1, 0,0,  1,1, 1,1,  1,-1, 1,0 };
    m_quadBuf = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quad), quad);

    m_uboParams = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(FluidParamsUBO), nullptr,
                                    RHI::BufferMemory::Dynamic);
    m_init = true;
}

void FluidRenderer::bindParams(RHI::Context* ctx) {
    RHI::Device* dev = RHI::device();
    dev->updateBuffer(m_uboParams, 0, sizeof(FluidParamsUBO), &m_params);
    ctx->bindUniformBuffer(8, m_uboParams);
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
        dep.colorFilter = RHI::Filter::Nearest; dep.hasDepth = true;
        // D32F: debe COINCIDIR con el depth del target de escena — el pase de fluido lo copia con
        // glBlitFramebuffer, y un blit entre formatos de depth distintos falla en silencio.
        dep.depthFormat = RHI::Format::D32F;
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
    }
}

void FluidRenderer::uploadParticles(const Haruka::WorldPos& cameraPos, int n) {
    m_verts.resize(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        glm::vec3 cp = glm::vec3(m_solver->worldPos(i) - glm::dvec3(cameraPos));
        m_verts[i*3+0] = cp.x; m_verts[i*3+1] = cp.y; m_verts[i*3+2] = cp.z;
    }
    if (RHI::Device* dev = RHI::device())
        dev->uploadBuffer(m_particleBuf, m_verts.size()*sizeof(float), m_verts.data());
}

void FluidRenderer::render(const Haruka::WorldPos& cameraPos, int vpW, int vpH) {
    if (!m_solver || m_solver->count() == 0) return;
    if (getenv("HARUKA_NO_FLUID")) return;   // DEBUG: aislar el pase de fluido
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
    RHI::Device* dev = RHI::device();
    m_params.radius    = m_radius;
    m_params.viewportH = vpH;

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_psoSpheres);   // puntos + depth on (absorbe glEnable(GL_PROGRAM_POINT_SIZE))
    bindParams(ctx);
    ctx->bindVertexBuffer(m_particleBuf, 0);
    ctx->draw((uint32_t)n);
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

    RHI::Device*  dev = RHI::device();
    RHI::Context* ctx = dev->beginFrame();

    // 1) Depth pass: particle spheres → nearest eye-depth into m_depthTex, depth-
    //    tested against the SCENE depth just blitted in (so terrain in front occludes
    //    the fluid). Clear colour only — keep the blitted scene depth.
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthFBO);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);   // SOLO color: la profundidad de la escena recién blitteada se conserva
    m_params.radius    = m_radius;
    m_params.viewportH = (float)h;
    ctx->bindPipeline(m_psoDepth);  // puntos + depth on/write on + blend off (antes: glEnable sueltos)
    bindParams(ctx);
    ctx->bindVertexBuffer(m_particleBuf, 0);
    ctx->draw((uint32_t)n);

    // 2) Separable bilateral smooth: depthTex -> smooth[0] (H) -> smooth[1] (V).
    m_params.depthFalloff = 0.5f;   // (m) nitidez del borde
    ctx->bindPipeline(m_psoBlur);   // quad + depth OFF
    ctx->bindVertexBuffer(m_quadBuf, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, m_smoothFBO[0]);
    m_params.blurDir = glm::vec2(1.0f / (float)w, 0.0f);   // horizontal
    bindParams(ctx);
    ctx->bindTexture(0, dev->getColorTexture(m_depthPass, 0));
    ctx->draw(4);

    glBindFramebuffer(GL_FRAMEBUFFER, m_smoothFBO[1]);
    m_params.blurDir = glm::vec2(0.0f, 1.0f / (float)h);   // vertical
    bindParams(ctx);
    ctx->bindTexture(0, dev->getColorTexture(m_smoothPass[0], 0));
    ctx->draw(4);

    // 3) Composite the water surface into the scene target (prevFBO). Opaque: the
    //    shader samples the scene COPY for refraction, so no feedback loop and no
    //    blend needed; empty pixels are discarded.
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(0, 0, w, h);
    m_params.texel        = glm::vec2(1.0f / (float)w, 1.0f / (float)h);
    m_params.refractScale = 0.03f;                          // unidades uv
    ctx->bindPipeline(m_psoSurface);
    bindParams(ctx);
    ctx->bindVertexBuffer(m_quadBuf, 0);
    ctx->bindTexture(0, dev->getColorTexture(m_smoothPass[1], 0));  // profundidad suavizada
    ctx->bindTexture(1, dev->getColorTexture(m_sceneCopyPass, 0));  // escena de detrás (refracción)
    ctx->draw(4);

    // Los pases que vienen después siguen siendo GL crudo y dan por hecho el estado normal.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

} // namespace Haruka
