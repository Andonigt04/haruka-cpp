#include "RHI/Common/RHITestCommon.h"

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
