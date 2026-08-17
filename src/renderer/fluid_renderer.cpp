#include "fluid_renderer.h"
#include "shader.h"
#include "physics/fluid/pbf_solver.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "core/logger.h"

namespace Haruka {

FluidRenderer::~FluidRenderer() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    auto del = [&](auto& h) { if (RHI::valid(h)) { dev->destroy(h); h = {}; } };
    del(m_particleBuf); del(m_quadBuf); del(m_uboParams);
    del(m_depthPass); del(m_smoothPass[0]); del(m_smoothPass[1]); del(m_sceneCopyPass);
    del(m_psoSpheres); del(m_psoDepth); del(m_psoBlur); del(m_psoSurface);
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

    RHI::PipelineDesc pp;
    pp.vertexPath              = vPart.c_str();
    pp.fragmentPath            = fPart.c_str();
    pp.vertexLayout.strides    = { (uint32_t)(3 * sizeof(float)) };
    pp.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 } };
    pp.topology     = RHI::PrimitiveTopology::Points;
    pp.depth.test   = true;
    pp.depth.write  = true;
    pp.blend.enable = false;
    pp.cull         = RHI::CullMode::None;
    m_psoSpheres = dev->createPipeline(pp);

    pp.fragmentPath = fDep.c_str();
    m_psoDepth = dev->createPipeline(pp);

    RHI::PipelineDesc pq;
    pq.vertexPath              = vQuad.c_str();
    pq.fragmentPath            = fBlur.c_str();
    pq.vertexLayout.strides    = { (uint32_t)(4 * sizeof(float)) };
    pq.vertexLayout.attributes = {
        { 0, 0,                        RHI::Format::RG32F, 0 },
        { 1, (uint32_t)(2*sizeof(float)), RHI::Format::RG32F, 0 },
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

void FluidRenderer::ensureTargets(int w, int h, RHI::Format srcDepthFmt) {
    if (RHI::valid(m_depthPass) && m_fbW == w && m_fbH == h && m_depthFmt == srcDepthFmt) return;
    m_fbW = w; m_fbH = h; m_depthFmt = srcDepthFmt;

    RHI::Device* dev = RHI::device();
    if (!dev) return;
    auto del = [&](auto& h) { if (RHI::valid(h)) { dev->destroy(h); h = {}; } };
    del(m_depthPass); del(m_smoothPass[0]); del(m_smoothPass[1]); del(m_sceneCopyPass);

    RHI::RenderTargetDesc dep;
    dep.width = w; dep.height = h; dep.colorFormats = { RHI::Format::R32F };
    dep.colorFilter = RHI::Filter::Nearest; dep.hasDepth = true;
    // ⚠️ EL FORMATO LO DICTA LA FUENTE DEL BLIT, no este bloque: `blitDepth` exige formatos
    // IDÉNTICOS, y cuando la escena va al BACKBUFFER su profundidad la elige SDL, no el motor.
    // Con D32F fijo aquí, GL rechazaba el blit (`Depth formats do not match`) y las gotas ocluían
    // contra una profundidad sin escribir. Misma causa que tenía el pase de nubes.
    dep.depthFormat = srcDepthFmt;
    m_depthPass = dev->createRenderTarget(dep);

    for (int i = 0; i < 2; ++i) {
        RHI::RenderTargetDesc sm;
        sm.width = w; sm.height = h; sm.colorFormats = { RHI::Format::R32F };
        sm.colorFilter = RHI::Filter::Nearest; sm.hasDepth = false;
        m_smoothPass[i] = dev->createRenderTarget(sm);
    }

    RHI::RenderTargetDesc sc;
    sc.width = w; sc.height = h; sc.colorFormats = { RHI::Format::RGBA16F };
    sc.colorFilter = RHI::Filter::Linear; sc.hasDepth = false;
    m_sceneCopyPass = dev->createRenderTarget(sc);
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

void FluidRenderer::render(const Haruka::WorldPos& cameraPos, int vpW, int vpH,
                           RHI::RenderPassHandle scenePass) {
    if (!m_solver || m_solver->count() == 0) return;
    if (getenv("HARUKA_NO_FLUID")) return;
    ensureGL();

    const int n = m_solver->count();
    // ⚠️ EL SISTEMA DE FLUIDOS TIENE DOS RENDERERS, y confundirlos ya costó una vuelta entera:
    // `HARUKA_NOLAMINA` apaga la lámina de ríos/lagos (ShallowWaterRenderer) y NO toca éste. Éste
    // compone una SUPERFICIE LÍQUIDA en espacio de pantalla a partir de las partículas PBF, así que
    // también se ve como "agua" — y como las partículas viven en el parche que sigue al jugador, se
    // ve en un cuadrado. Se avisa la primera vez que dibuja, con cuántas partículas, para que
    // "quién pinta el agua" no haya que deducirlo. Lo apaga `HARUKA_NO_FLUID=1`.
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        HARUKA_LOGI("Fluid", "superficie PBF DIBUJANDO: %d partículas (modo %s) — es el OTRO renderer "
                             "del fluido; lo apaga HARUKA_NO_FLUID, no HARUKA_NOLAMINA",
                    n, m_surfaceMode ? "superficie en pantalla" : "esferas");
    }
    uploadParticles(cameraPos, n);

    if (m_surfaceMode) renderSurface(n, vpW, vpH, scenePass);
    else               renderSpheres(n, (float)vpH);
}

void FluidRenderer::renderSpheres(int n, float vpH) {
    RHI::Device* dev = RHI::device();
    m_params.radius    = m_radius;
    m_params.viewportH = vpH;

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_psoSpheres);
    bindParams(ctx);
    ctx->bindVertexBuffer(m_particleBuf, 0);
    ctx->draw((uint32_t)n);
}

void FluidRenderer::renderSurface(int n, int w, int h, RHI::RenderPassHandle scenePass) {
    // La escena puede estar en el backbuffer (`scenePass` inválido) o en el target de post: el
    // formato de profundidad de la copia tiene que seguir a ESA fuente.
    ensureTargets(w, h, RHI::valid(scenePass) ? RHI::Format::D32F
                                              : RHI::device()->backbufferDepthFormat());
    if (!RHI::valid(m_depthPass)) return;
    if (n <= 0) return;

    // scenePass es el target de escena: `{0}` == backbuffer (válido). Los blits y el compuesto
    // leen/escriben ese target para sembrar refracción+oclusión y componer la superficie final.

    RHI::Device*  dev = RHI::device();
    RHI::Context* ctx = dev->beginFrame();
    const float texelX = 1.0f / (float)w;
    const float texelY = 1.0f / (float)h;

    // 0) SIEMBRA del modo superficie (blits GL: el RHI no tiene comando de blit).
    //    - Copia el COLOR de la escena a m_sceneCopyPass → lo lee el composite para refracción.
    //    - Copia el DEPTH de la escena al depth del pase de partículas → las gotas OCLUYEN a lo
    //      que tienen por detrás (paredes, acantilados, la propia costa). Sin esto el splash
    //      pintaría por delante de geometría que le tapa.
    ctx->blitColor(scenePass, m_sceneCopyPass, w, h);
    ctx->blitDepth(scenePass, m_depthPass, w, h);

    // 1) Particle depth pass: clear colour (R32F=0), preserve blitted scene depth.
    RHI::ClearValues cv;
    cv.clearColor = true;  cv.color[0] = cv.color[1] = cv.color[2] = cv.color[3] = 0.0f;
    cv.clearDepth = false; // keep scene depth from blit

    ctx->beginRenderPass(m_depthPass, cv);
    m_params.radius    = m_radius;
    m_params.viewportH = (float)h;
    m_params.texel     = glm::vec2(texelX, texelY);
    ctx->bindPipeline(m_psoDepth);
    bindParams(ctx);
    ctx->bindVertexBuffer(m_particleBuf, 0);
    ctx->draw((uint32_t)n);
    ctx->endRenderPass();

    // 2) Separable bilateral smooth: depthTex -> smooth[0] (H) -> smooth[1] (V).
    m_params.depthFalloff = 0.5f;

    RHI::ClearValues ccv;
    ccv.clearColor = true; ccv.color[0] = ccv.color[1] = ccv.color[2] = ccv.color[3] = 0.0f;
    ccv.clearDepth = false;

    ctx->beginRenderPass(m_smoothPass[0], ccv);
    ctx->bindPipeline(m_psoBlur);
    ctx->bindVertexBuffer(m_quadBuf, 0);
    m_params.blurDir = glm::vec2(texelX, 0.0f);
    bindParams(ctx);
    ctx->bindTexture(0, dev->getColorTexture(m_depthPass, 0));
    ctx->draw(4);
    ctx->endRenderPass();

    ctx->beginRenderPass(m_smoothPass[1], ccv);
    m_params.blurDir = glm::vec2(0.0f, texelY);
    bindParams(ctx);
    ctx->bindTexture(0, dev->getColorTexture(m_smoothPass[0], 0));
    ctx->draw(4);
    ctx->endRenderPass();

    // 3) Compuesto de SUPERFICIE OPAQUE sobre el target de escena. El quad cubre toda la
    //    pantalla; fluid_surface.frag descarta donde no hay fluido (u_depth ≤ 0) y refracta lo
    //    que hay detrás (u_scene) donde sí lo hay → el resto de la escena queda intacto.
    RHI::ClearValues keep;
    keep.clearColor = false; keep.clearDepth = false;
    ctx->beginRenderPass(scenePass, keep);
    ctx->setViewport(0, 0, w, h);
    ctx->bindPipeline(m_psoSurface);
    m_params.texel       = glm::vec2(texelX, texelY);
    m_params.refractScale = 0.03f;
    bindParams(ctx);
    ctx->bindVertexBuffer(m_quadBuf, 0);
    ctx->bindTexture(0, dev->getColorTexture(m_smoothPass[1], 0));   // smoothed depth
    ctx->bindTexture(1, dev->getColorTexture(m_sceneCopyPass, 0));   // scene colour behind
    ctx->draw(4);
    ctx->endRenderPass();
}

} // namespace Haruka