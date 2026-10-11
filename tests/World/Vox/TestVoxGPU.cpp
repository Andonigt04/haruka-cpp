#include "RHI/Common/RHITestCommon.h"
#include "Haruka/Renderer/VoxRenderer.h"
#include "Haruka/Renderer/ResourceStore.h"
#include "Haruka/World/Vox/VoxWorld.h"
#include <filesystem>
#include <thread>
#include <chrono>

// ── EL RECORTE DEL CAMPO VOLUMÉTRICO EN EL PASE REAL ───────────────────────────────────────────
//
// La boca de una cueva es un `discard` en `terrain_node.frag` gobernado por una ventana R8/RGBA8
// (cada texel = `VoxWorld::surfaceCut`) y una matriz ojo→uv. Aqui se dibuja el pase de nodos con una
// ventana SINTETICA —un disco en el centro— mirando al nadir, y se cuenta el suelo que desaparece:
// dentro del disco, todo; fuera, nada. Con contraprueba (ventana vacia: no desaparece nada). Es el
// gemelo de `test_caves_coupling` (CPU), sobre el shader de verdad y los dos backends.
void testNodeVoxCut()
{
    BEGIN("cuevas: el pase de nodos RECORTA el suelo donde la ventana dice boca");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 256)) { CHECK(false, "init del pase"); return; }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // La ventana: 256², un disco de radio 0,18 (en uv) en el centro. G (lecho) = 0.
    const int W = 256;
    auto makeCut = [&](bool disco) {
        std::vector<uint8_t> px((size_t)W * W * 4, 0);
        for (int y = 0; y < W; ++y)
            for (int x = 0; x < W; ++x) {
                const double u = (x + 0.5) / W - 0.5, v = (y + 0.5) / W - 0.5;
                const bool in = disco && (u * u + v * v) < 0.18 * 0.18;
                const size_t o = ((size_t)y * W + x) * 4;
                px[o] = in ? 255 : 0; px[o + 3] = 255;
            }
        TextureDesc td; td.width = W; td.height = W; td.format = Format::RGBA8;
        td.filter = Filter::Linear; td.wrap = Wrap::ClampToEdge; td.mipmaps = false; td.initialData = px.data();
        return g_dev->createTexture(td);
    };

    // Camara a 400 m sobre el suelo mirando al nadir; la ventana cubre ±320 m alrededor del pie, en
    // el marco (e1, e2) tangente. Con 60° de FOV a 400 m se ven ±230 m de alto: el disco (0,18 de
    // uv = ±115 m) cae en el centro del cuadro y se ve entero.
    const double alt = 400.0;
    const glm::dvec3 cam = pc + up0 * (R + alt);
    const glm::dvec3 fwd = -up0;
    const glm::dvec3 e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), up0)), e2 = glm::cross(up0, e1);
    const glm::dvec3 foot = up0 * R;          // el pie: el terreno sintetico esta a cota 0 (sin bake)
    const glm::dvec3 footRelCam = foot - cam;
    glm::dmat4 toFrame(1.0);
    toFrame[0] = glm::dvec4(e1.x, e2.x, up0.x, 0.0);
    toFrame[1] = glm::dvec4(e1.y, e2.y, up0.y, 0.0);
    toFrame[2] = glm::dvec4(e1.z, e2.z, up0.z, 0.0);
    toFrame[3] = glm::dvec4(-glm::dot(footRelCam, e1), -glm::dot(footRelCam, e2), -glm::dot(footRelCam, up0), 1.0);
    glm::dmat4 toUV(1.0);
    const double H = 320.0;
    toUV[0][0] = 0.5 / H; toUV[1][1] = 0.5 / H; toUV[3] = glm::dvec4(0.5, 0.5, 0.0, 1.0);
    const glm::mat4 cutSpace = glm::mat4(toUV * toFrame);

    const float aspect = (float)w / (float)h;
    const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

    // Se cuenta el suelo en dos zonas del cuadro: el CENTRO (dentro del disco: ±58 m son ±h/8 px a
    // 400 m con 60°) y un ANILLO exterior (fuera del disco pero dentro de la ventana).
    auto cuenta = [&](TextureHandle cut, size_t& centro, size_t& anillo, size_t& sentinel) {
        Haruka::World::Terrain::TerrainNodeRenderer::Shade sh;
        sh.on = false;                          // luz plana: solo importa si hay fragmento
        if (valid(cut)) { sh.cutTex = cut; sh.cutSpace = cutSpace; }
        r.setShade(sh);
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        centro = anillo = sentinel = 0;
        // Radio del disco en px: 0,18 de uv sobre una ventana de 2H metros → 0,18·2H m; a `alt` m
        // con FOV vertical `fovY`, el medio alto son alt·tan(fovY/2) m.
        const double rIn = (0.18 * 2.0 * H) / (alt * std::tan(fovY * 0.5)) * (h * 0.5);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) { ++sentinel; continue; }
                const bool lit = px[k] || px[k+1] || px[k+2];
                const double dx = x - w * 0.5, dy = y - h * 0.5, d = std::sqrt(dx * dx + dy * dy);
                if (d < rIn * 0.8) { if (lit) ++centro; }
                else if (d > rIn * 1.3 && d < rIn * 2.5) { if (lit) ++anillo; }
            }
    };

    size_t c0, a0, s0, c1, a1, s1;
    TextureHandle sin = makeCut(false), con = makeCut(true);
    cuenta(sin, c0, a0, s0);
    cuenta(con, c1, a1, s1);
    std::printf("    sin boca: centro %zu px de suelo · anillo %zu   |   con boca: centro %zu · anillo %zu   (centinela %zu/%zu)\n",
                c0, a0, c1, a1, s0, s1);
    CHECK(s0 < (size_t)w * h / 2 && s1 < (size_t)w * h / 2, "el readback escribio");
    CHECK(c0 > 100, "CONTRAPRUEBA: sin boca el centro tiene suelo");
    CHECK(c1 * 50 < c0, "con boca el suelo del centro DESAPARECE (>98 %)");
    CHECK(a1 * 10 > a0 * 9, "y fuera del disco se queda (el recorte no se sale de la ventana)");
    g_dev->destroy(sin); g_dev->destroy(con);
}

// ── THE FINE CUT WINDOW IN THE REAL PASS ────────────────────────────────────────────────────────
//
// A decimetre hole is cut with the fine window of the ATLAS (lib/vox_cut.glsl): the coarse window
// (left, 160²) says nothing, the fine one (right, 320² over ±16 m = 10 cm texels) has a 50 cm disc.
// Camera 3 m above the ground looking down. The disc must disappear and the ground around it stay.
// Controls: with the fine radius at 0 the same atlas cuts nothing (the hand-over is real), and the
// legacy layout (no atlas) is `testNodeVoxCut` above.
void testNodeVoxCutFine()
{
    BEGIN("caves: the fine cut window (atlas) cuts a 50 cm hole in the real pass, only within the fine radius");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 256)) { CHECK(false, "pass init"); return; }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // The atlas: coarse 160² (all zero) + fine 320² with a disc of 0.5 m radius in the middle.
    const int CW = 160, FW = 320, AW = CW + FW, AH = FW;
    const double FH = 16.0, discM = 0.5;
    std::vector<uint8_t> px((size_t)AW * AH * 4, 0);
    for (size_t o = 3; o < px.size(); o += 4) px[o] = 255;
    for (int y = 0; y < FW; ++y)
        for (int x = 0; x < FW; ++x) {
            const double u = ((x + 0.5) / FW - 0.5) * 2.0 * FH, v = ((y + 0.5) / FW - 0.5) * 2.0 * FH;
            if (u * u + v * v < discM * discM) px[((size_t)y * AW + CW + x) * 4] = 255;
        }
    TextureDesc td; td.width = AW; td.height = AH; td.format = Format::RGBA8;
    td.filter = Filter::Linear; td.wrap = Wrap::ClampToEdge; td.mipmaps = false; td.initialData = px.data();
    TextureHandle atlasTex = g_dev->createTexture(td);
    const glm::vec4 atlas((float)CW / AW, (float)CW / AH, (float)CW / AW, (float)FW / AW);

    // Without a bake the node is pure DETAIL over R (`TestTerrainDrawnGPU`): the ground under the eye
    // is its CPU twin, not cota 0 (measured: ~85 m below R here). The fine radius is a real distance,
    // so the camera has to be really 3 m above it.
    const double ground = (double)terrainDetail(up0, R, 0.5f);   // no base field: not attenuated (TerrainNode.h)
    const double alt = 3.0;
    const glm::dvec3 cam = pc + up0 * (R + ground + alt);
    const glm::dvec3 fwd = -up0;
    const glm::dvec3 e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), up0)), e2 = glm::cross(up0, e1);
    const glm::dvec3 footRelCam = up0 * (R + ground) - cam;
    auto space = [&](double H) {
        glm::dmat4 toFrame(1.0);
        toFrame[0] = glm::dvec4(e1.x, e2.x, up0.x, 0.0);
        toFrame[1] = glm::dvec4(e1.y, e2.y, up0.y, 0.0);
        toFrame[2] = glm::dvec4(e1.z, e2.z, up0.z, 0.0);
        toFrame[3] = glm::dvec4(-glm::dot(footRelCam, e1), -glm::dot(footRelCam, e2), -glm::dot(footRelCam, up0), 1.0);
        glm::dmat4 toUV(1.0);
        toUV[0][0] = 0.5 / H; toUV[1][1] = 0.5 / H; toUV[3] = glm::dvec4(0.5, 0.5, 0.0, 1.0);
        return glm::mat4(toUV * toFrame);
    };
    const float aspect = (float)w / (float)h, fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));

    // "Ground" = the NEAR ground's flat colour (no light: `sh.on = false`), taken from the control's
    // ring. Through a hole without walls the pass shows another surface (the far side of the
    // sphere, medido: (25,25,20) where the ground is (53,58,44)), not black: "anything lit" would not
    // tell the hole from the ground.
    int gr = -1, gg = -1, gb = -1;
    auto isGround = [&](const uint8_t* q) { return std::abs(q[0] - gr) <= 3 && std::abs(q[1] - gg) <= 3 && std::abs(q[2] - gb) <= 3; };
    auto count = [&](float fineR, size_t& centre, size_t& ring, size_t& sentinel) {
        TerrainNodeRenderer::Shade sh;
        sh.on = false;
        sh.cutTex = atlasTex; sh.cutSpace = space(320.0); sh.cutFineSpace = space(FH); sh.cutFineR = fineR; sh.cutAtlas = atlas;
        r.setShade(sh);
        std::vector<uint8_t> img((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, img.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, img.data());
        }
        centre = ring = sentinel = 0;
        const double rIn = discM / (alt * std::tan(fovY * 0.5)) * (h * 0.5);   // disc radius in px
        if (gr < 0) {   // the first call (the control) fixes the ground colour from a ring pixel
            const size_t k = ((size_t)(h / 2) * w + (size_t)(w / 2 + (int)(rIn * 2.0))) * 4;
            gr = img[k]; gg = img[k + 1]; gb = img[k + 2];
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (img[k] == 0xAA && img[k + 1] == 0xAA && img[k + 2] == 0xAA) { ++sentinel; continue; }
                const bool ground = isGround(&img[k]);
                const double d = std::sqrt((x - w * 0.5) * (x - w * 0.5) + (y - h * 0.5) * (y - h * 0.5));
                if (d < rIn * 0.8) { if (ground) ++centre; }
                else if (d > rIn * 1.3 && d < rIn * 2.5) { if (ground) ++ring; }
            }
    };
    size_t c0, a0, s0, c1, a1, s1;
    count(0.0f, c0, a0, s0);    // fine radius 0: the fine window is not read
    count(12.0f, c1, a1, s1);
    std::printf("    ground under the eye %.1f m (detail over R)\n", ground);
    std::printf("    fine radius 0: centre %zu px of ground · ring %zu   |   fine radius 12 m: centre %zu · ring %zu   (sentinel %zu/%zu)\n",
                c0, a0, c1, a1, s0, s1);
    CHECK(s0 < (size_t)w * h / 2 && s1 < (size_t)w * h / 2, "the readback wrote");
    CHECK(c0 > 100, "CONTROL: with the fine radius at 0 the centre keeps its ground (coarse window is empty)");
    CHECK(c1 * 50 < c0, "within the fine radius the 50 cm disc DISAPPEARS (>98 %)");
    CHECK(a1 * 10 > a0 * 9, "and around it the ground stays");
    g_dev->destroy(atlasTex);
}

// ── FINE BRICKS IN THE REAL WALL PASS ───────────────────────────────────────────────────────────
//
// `VoxRenderer` near the eye: a 30 cm pick right under a camera 2 m above flat ground. The 4 m chunk
// meshes have nothing there (the grid cannot open it); the fine bricks do. Measured: the bricks are
// chosen, the fine radius only opens once they are UPLOADED (before that, nothing fine is drawn — the
// control), and then the hole's rock is drawn in the middle of the image.
void testVoxFineBricks()
{
    BEGIN("caves: VoxRenderer draws a 30 cm pick with fine bricks, only once they are uploaded");
    namespace V = Haruka::World::Vox;
    const double R = 6371000.0;
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_fine_gpu").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    V::VoxWorld w;
    V::caveSetAutoProbability(0.0f);
    w.configure(5u, R, tmp + "/bakes", "", [](const glm::dvec3&) { return 10.0f; });
    w.setSynchronousBake(true);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(0.3, 0.8, 0.5));
    const glm::dvec3 ground = up0 * (R + 10.0);
    w.stroke(ground - up0 * 0.15, 0.3f, true);

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int W = (uw > 0) ? (int)uw : 256, H = (uh > 0) ? (int)uh : 256;
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    const double alt = 2.0, fovY = 60.0 * 3.14159265358979 / 180.0;
    const glm::dvec3 cam = ground + up0 * alt;
    const glm::dvec3 fwd = -up0;
    const float aspect = (float)W / (float)H, fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
    const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
    const glm::mat4 rotVP = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));

    Haruka::Renderer::VoxRenderer vr;
    auto rockPixels = [&]() {
        std::vector<uint8_t> img((size_t)W * H * 4, 0xAA);
        for (int f = 0; f < 3; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            vr.draw(w, rotVP, cam, glm::vec3(up0));
            ctx->endRenderPass();
            if (!isVk && f == 2) g_dev->readPixels(0, 0, W, H, Format::RGBA8, img.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 2) g_dev->readPixels(0, 0, W, H, Format::RGBA8, img.data());
        }
        size_t rock = 0;
        const double rPx = 0.26 / (alt * std::tan(fovY * 0.5)) * (H * 0.5);   // the opening, in px
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t k = ((size_t)y * W + x) * 4;
                const double d = std::sqrt((x - W * 0.5) * (x - W * 0.5) + (y - H * 0.5) * (y - H * 0.5));
                if (d < rPx && (img[k] || img[k + 1] || img[k + 2]) && !(img[k] == 0xAA && img[k + 1] == 0xAA)) ++rock;
            }
        return rock;
    };
    // (1) chosen and published, NOT uploaded yet: no fine radius
    vr.update(w, cam, 10.0);
    const size_t loaded = vr.fineBricksLoaded();
    const float r0 = vr.fineRadius();
    const size_t rock0 = rockPixels();
    // (2) uploaded
    int frames = 0;
    // The bricks are built in the job system: a few ms each, so each update waits a little.
    for (; frames < 200 && vr.fineRadius() < 10.0f; ++frames) {
        Haruka::Renderer::resources().processUploads(1000.0); vr.update(w, cam, 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const float r1 = vr.fineRadius();
    const size_t rock1 = rockPixels();
    // THE FINE WINDOW the job computed (same eye as the draw): open over the hole, closed 1.5 m away (what the terrain reads).
    {
        const glm::dvec3 side = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
        const float inHole = vr.fineCutAt(glm::vec3(ground - cam)), away = vr.fineCutAt(glm::vec3(ground + side * 1.5 - cam));
        std::printf("    fine window (computed in a job): over the hole %.2f · 1.5 m away %.2f\n", inHole, away);
        CHECK(inHole > 0.5f && away >= 0.0f && away < 0.5f, "the fine window opens the ground over the hole and only there");
    }
    // Steady state on the render thread: choosing the bricks (nothing to build) and the fine window
    // rebuilt once (as when the eye moves 2 m).
    double steadyMs = 0.0;
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 20; ++i) vr.update(w, cam, 10.0);
        steadyMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count() / 20.0;
    }
    double moveMs = 0.0;
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        vr.update(w, cam + glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0))) * 2.5, 10.0);   // > 2 m: the fine window is rebuilt
        moveMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    }
    std::printf("    render-thread cost: steady update %.2f ms · update that rebuilds the fine window %.2f ms\n", steadyMs, moveMs);
    std::printf("    bricks chosen %zu · before upload: fine radius %.1f m, %zu rock px in the hole · after %d updates: %.1f m, %zu rock px · %zu bricks drawn\n",
                loaded, r0, rock0, frames, r1, rock1, vr.fineBricksDrawn());
    CHECK(loaded > 0, "fine bricks are chosen around the pick");
    CHECK(r0 == 0.0f && rock0 == 0, "CONTROL: before the upload there is no fine radius and the 4 m walls draw nothing for a 30 cm hole");
    CHECK(r1 > 10.0f, "once uploaded the fine radius opens (~12 m)");
    CHECK(rock1 > 200 && vr.fineBricksDrawn() > 0, "and the hole's rock is drawn by the fine bricks");
    std::filesystem::remove_all(tmp, ec);
}
