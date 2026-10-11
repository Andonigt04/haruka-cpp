// La PIEL DE AUTOR se dibuja: el modo malla de `SoftBodyRenderer` (posiciones de la tela, uv, color por
// vértice y textura) en GL y Vulkan. Ver Survival/scripts/Creatures/MeshSkin.h.
#include "RHI/Common/RHITestCommon.h"
#include "Haruka/Renderer/SoftbodyRenderer.h"
#include "Haruka/Physics/XPBD/XPBDSolver.h"
#include "Haruka/IO/ImageWriter.h"

#include <filesystem>

namespace {
struct PerFrame {   // gemelo de `PerFrameData` (binding 0) de softbody_mesh.vert/frag, std140
    float view[16]; float proj[16];
    float camPos[3]; float pad0; float sunDir[3]; float pad1; float sunColor[3]; float ambient;
    int32_t enableHDR; int32_t pad2[3];
};

// Un triángulo de tela delante de la cámara (que mira a −Z desde el origen), cubriendo el centro.
// Devuelve el píxel central dibujado con `md`.
bool drawCenter(const Haruka::Renderer::SoftBodyRenderer::MeshDraw& md, unsigned char out[4]) {
    Haruka::Physics::XPBD::XPBDSolver S;
    S.particles.origin = glm::dvec3(0.0);
    S.particles.add(glm::vec3(-3.0f, -3.0f, -2.0f), 1.0f);
    S.particles.add(glm::vec3( 3.0f, -3.0f, -2.0f), 1.0f);
    S.particles.add(glm::vec3( 0.0f,  3.0f, -2.0f), 1.0f);
    Haruka::Renderer::SoftBodyRenderer r;
    if (!r.addMesh(md, &S)) return false;

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = uw ? (int)uw : 256, h = uh ? (int)uh : 256;
    PerFrame pf{};
    const glm::mat4 I(1.0f);
    std::memcpy(pf.view, &I[0][0], sizeof(pf.view));
    glm::mat4 P(0.0f);                                     // reversed-Z, lejano infinito (como el motor)
    const float f = 1.0f / std::tan(glm::radians(30.0f));
    P[0][0] = f * (float)h / (float)w; P[1][1] = f; P[2][3] = -1.0f; P[3][2] = 0.1f;
    std::memcpy(pf.proj, &P[0][0], sizeof(pf.proj));
    pf.sunDir[2] = 1.0f; pf.sunColor[0] = pf.sunColor[1] = pf.sunColor[2] = 1.0f; pf.ambient = 0.3f;
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pf), &pf, BufferMemory::Dynamic);

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindUniformBuffer(0, ubo);
        r.render(glm::dvec3(0.0));
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(w / 2, h / 2, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk) g_dev->readPixels(w / 2, h / 2, 1, 1, Format::RGBA8, out);
    }
    r.clear();
    g_dev->destroy(ubo);
    return true;
}
} // namespace

void testSoftBodyMesh() {
    BEGIN("piel de autor: la malla de la tela se dibuja con su color y su textura");
    Haruka::Renderer::SoftBodyRenderer::MeshDraw md;
    md.vertexParticle = { 0, 1, 2 };
    md.indices = { 0, 1, 2 };
    md.uv = { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } };
    md.color = { { 0.2f, 0.4f, 0.9f }, { 0.2f, 0.4f, 0.9f }, { 0.2f, 0.4f, 0.9f } };

    unsigned char a[4] = {};
    CHECK(drawCenter(md, a), "la malla de la tela se registra");
    std::printf("    color por vertice (0.2,0.4,0.9) -> (%d,%d,%d)\n", a[0], a[1], a[2]);
    CHECK(a[2] > a[1] && a[1] > a[0] && a[2] > 60, "el color por vertice llega (azul: b > g > r)");

    // Con TEXTURA roja y color blanco: el píxel tiene que salir ROJO. Si no se muestreara, saldría blanco
    // (el color) — la contraprueba de que la textura llega.
    const std::filesystem::path png = std::filesystem::temp_directory_path() / "haruka_skin_rojo.png";
    const uint8_t rojo[4 * 4] = { 220, 30, 30, 255, 220, 30, 30, 255, 220, 30, 30, 255, 220, 30, 30, 255 };
    Haruka::IO::writePNG(png.string(), 2, 2, 4, rojo);
    md.color = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } };
    md.texture = png.string();
    unsigned char b[4] = {};
    drawCenter(md, b);
    std::filesystem::remove(png);
    std::printf("    textura roja sobre blanco -> (%d,%d,%d)\n", b[0], b[1], b[2]);
    CHECK(b[0] > 2 * b[1] && b[0] > 2 * b[2] && b[0] > 60, "la TEXTURA se muestrea (rojo, no el blanco del color)");
}
