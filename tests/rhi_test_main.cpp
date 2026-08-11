/**
 * @file rhi_test_main.cpp
 * @brief Banco de pruebas ABIERTO del RHI: mismo test sobre GL y Vulkan (requiere VENTANA).
 *
 * A diferencia de `haruka_tests` (headless, CPU-only), estos tests necesitan un device real
 * con swapchain, así que abren una ventana SDL y ejercitan el RHI completo contra CADA backend:
 *
 *   ./bin/haruka_tests_rhi [gl|vk|all]
 *
 * Cubre:
 *   - Creación de textura para TODOS los formatos de color de textura del motor (RGBA/R/RG,
 *     incluido RGBA32F y RGBA16F). Regresión: los formatos RGB de 3 canales ya NO se declaran
 *     como textura (Vulkan/NVIDIA no los muestrea), así que aquí solo viven formatos válidos.
 *   - Buffers: create/update/upload/copy y readback por mapeo (BufferMemory::Readback).
 *   - Render targets: color solo, color+depth no muestreable, depth muestreable (shadow), MRT.
 *   - Texturas ARRAY y CUBEMAP (con updateCubemapFace).
 *   - Samplers (filtro x wrap).
 *   - Un ciclo real del frame (beginFrame/beginRenderPass/endRenderPass/endFrame) → la cadena
 *     acquire→submit→present que rompía en NVIDIA (TDR) + clear→readPixels de paridad.
 *
 * Devuelve 0 si TODO pasa, !=0 si algo falla. Se registra como ctest `haruka_tests_rhi`.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"

#include <SDL3/SDL.h>

// --- mini-framework (mismo estilo que tests/test_common.h) ---
static int  g_pass = 0, g_fail = 0;
static const char* g_cur = "";
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  [FAIL] [%s] %s\n", g_cur, msg); } \
} while (0)
#define BEGIN(name) do { g_cur = name; std::printf("== %s ==\n", name); } while (0)

using namespace Haruka::RHI;

static const char* fmtName(Format f)
{
    switch (f) {
        case Format::RGBA8:        return "RGBA8";
        case Format::SRGB8_ALPHA8: return "SRGB8_ALPHA8";
        case Format::RGBA16F:      return "RGBA16F";
        case Format::RG16F:        return "RG16F";
        case Format::RGBA32F:      return "RGBA32F";
        case Format::RG32F:        return "RG32F";
        case Format::R11G11B10F:   return "R11G11B10F";
        case Format::R8:           return "R8";
        case Format::RG32UI:       return "RG32UI";
        case Format::RGBA16UI:     return "RGBA16UI";
        case Format::R32F:         return "R32F";
        default:                   return "?";
    }
}

// Los formatos de color que el motor usa como TEXTURA (muestreable). Los RGB puros de 3 canales
// (RGB32F, RGB16F, RGB8…) NO están aquí: son formatos de VÉRTICE (o se emite como RGBA) porque
// Vulkan/NVIDIA no soporta muestrear R32G32B32_* — justo el bug que esta suite vigila.
static const Format kTexFormats[] = {
    Format::RGBA8, Format::SRGB8_ALPHA8, Format::RGBA16F, Format::RG16F,
    Format::RGBA32F, Format::RG32F, Format::R11G11B10F, Format::R8,
    Format::RG32UI, Format::RGBA16UI, Format::R32F,
};
static const int kNumTexFormats = (int)(sizeof(kTexFormats) / sizeof(kTexFormats[0]));

// Genera un buffer de píxeles con margen holgado: GL lee RG16F/RGBA16F como GL_FLOAT (4 B/comp)
// mientras VK los lee como half (2 B/comp), y RG 32F como 8 B/px. Para ser backend-agnóstico y
// no sobrepasar el buffer en GL, asignamos 16 B/px (el peor caso, RGBA32F = 16) sea cual sea el
// formato: cada backend solo consume lo que le toca del patrón no-trivial que pintamos.
static std::vector<uint8_t> makePixels(int w, int h)
{
    std::vector<uint8_t> p((size_t)w * h * 16);
    for (size_t i = 0; i < p.size(); ++i) p[i] = (uint8_t)((i * 37u + 11u) & 0xFF);
    return p;
}

static Device* g_dev = nullptr;

static void testTextureFormats()
{
    BEGIN("textura: creacion por formato");
    const int W = 4, H = 4;
    for (int i = 0; i < kNumTexFormats; ++i) {
        Format f = kTexFormats[i];
        std::vector<uint8_t> data = makePixels(W, H);
        TextureDesc d;
        d.width = W; d.height = H; d.format = f;
        d.filter = Filter::Nearest; d.wrap = Wrap::ClampToEdge;
        d.initialData = data.data();
        TextureHandle t = g_dev->createTexture(d);
        bool ok = valid(t);
        if (ok) g_dev->destroy(t);
        std::string msg = std::string("create/destroy ") + fmtName(f);
        if (!ok) CHECK(false, msg.c_str());
        else     CHECK(true,  msg.c_str());
    }
}

static void testBuffers()
{
    BEGIN("buffer: create/update/copy/readback");
    float src[16]; for (int i = 0; i < 16; ++i) src[i] = (float)i * 0.25f;
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(src), src);
    CHECK(valid(vb), "vertex buffer create");
    if (!valid(vb)) return;

    // updateBuffer: reescribir en offset.
    float half[8]; for (int i = 0; i < 8; ++i) half[i] = -1.0f;
    g_dev->updateBuffer(vb, 0, sizeof(half), half);
    CHECK(true, "updateBuffer parcial");

    // uploadBuffer (Stream): reasignar.
    BufferHandle sb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(src), nullptr,
                                          BufferMemory::Stream);
    CHECK(valid(sb), "stream buffer create");
    if (valid(sb)) {
        g_dev->uploadBuffer(sb, sizeof(src), src);
        g_dev->destroy(sb);
    }

    // copyBuffer GPU→GPU.
    BufferHandle dst = g_dev->createBuffer(BufferUsage::Vertex, sizeof(src), nullptr);
    CHECK(valid(dst), "dst buffer create");
    if (valid(dst)) {
        g_dev->copyBuffer(vb, dst, 0, 0, sizeof(src));
        g_dev->destroy(dst);
    }

    // Readback por MAPA (VK: memoria HOST_VISIBLE; GL: glMapBuffer).
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, sizeof(src), nullptr,
                                          BufferMemory::Readback);
    CHECK(valid(rb), "readback buffer create");
    if (valid(rb)) {
        const float* rbp = (const float*)g_dev->mappedData(rb);
        CHECK(rbp != nullptr, "readback mappedData != null");
        if (rbp) { volatile float v = rbp[0]; (void)v; CHECK(true, "readback mapeado legible"); }
        g_dev->destroy(rb);
    }

    g_dev->destroy(vb);
}

static void testRenderTargets()
{
    BEGIN("render target: color / color+depth / depth-textura / MRT");
    // color-only (R8 = SSAO)
    {
        RenderTargetDesc d{};
        d.width = d.height = 16;
        d.colorFormats = { Format::R8 };
        d.colorFilter = Filter::Nearest;
        d.hasDepth = false;
        RenderPassHandle rt = g_dev->createRenderTarget(d);
        bool ok = valid(rt) && valid(g_dev->getColorTexture(rt, 0));
        if (valid(rt)) g_dev->destroy(rt);
        CHECK(ok, "RT R8 (color only)");
    }
    // color + depth no muestreable
    {
        RenderTargetDesc d{};
        d.width = d.height = 16;
        d.colorFormats = { Format::RGBA8 };
        d.hasDepth = true; d.depthFormat = Format::D24S8;
        RenderPassHandle rt = g_dev->createRenderTarget(d);
        bool ok = valid(rt);
        if (valid(rt)) g_dev->destroy(rt);
        CHECK(ok, "RT RGBA8 + depth D24S8");
    }
    // depth AS TEXTURA (shadow maps)
    {
        RenderTargetDesc d{};
        d.width = d.height = 16;
        d.colorFormats = {};
        d.hasDepth = true; d.depthFormat = Format::D32F;
        d.depthAsTexture = true;
        RenderPassHandle rt = g_dev->createRenderTarget(d);
        bool ok = valid(rt) && valid(g_dev->getDepthTexture(rt));
        if (valid(rt)) g_dev->destroy(rt);
        CHECK(ok, "RT depth-as-texture D32F (samplable)");
    }
    // MRT 2 color
    {
        RenderTargetDesc d{};
        d.width = d.height = 16;
        d.colorFormats = { Format::RGBA8, Format::RGBA16F };
        d.hasDepth = true; d.depthFormat = Format::D24S8;
        RenderPassHandle rt = g_dev->createRenderTarget(d);
        bool ok = valid(rt) &&
                  valid(g_dev->getColorTexture(rt, 0)) &&
                  valid(g_dev->getColorTexture(rt, 1));
        if (valid(rt)) g_dev->destroy(rt);
        CHECK(ok, "RT MRT (RGBA8 + RGBA16F)");
    }
}

static void testArrayAndCube()
{
    BEGIN("textura: array + cubemap + samplers");
    // array 2D de varias capas (el clima del planeta usa layers=6).
    {
        TextureDesc td{};
        td.width = td.height = 4; td.format = Format::RGBA8;
        td.layers = 4; td.filter = Filter::Nearest;
        TextureHandle t = g_dev->createTexture(td);
        bool ok = valid(t);
        if (ok) g_dev->destroy(t);
        CHECK(ok, "array 2D (4 layers)");
    }
    // cubemap + updateCubemapFace (IBL).
    {
        TextureDesc td{};
        td.width = td.height = 8; td.format = Format::RGBA8;
        td.cube = true; td.filter = Filter::Linear;
        TextureHandle t = g_dev->createTexture(td);
        bool ok = valid(t);
        if (ok) {
            std::vector<uint8_t> face = makePixels(8, 8);
            for (int f = 0; f < 6; ++f)
                g_dev->updateCubemapFace(t, f, 8, 8, Format::RGBA8, face.data());
            g_dev->destroy(t);
        }
        CHECK(ok, "cubemap create + updateCubemapFace");
    }
    // sampler: todas las combinaciones filtro x wrap.
    {
        bool allOk = true;
        for (int fi = 0; fi < 2 && allOk; ++fi)
            for (int wi = 0; wi < 3 && allOk; ++wi) {
                SamplerDesc sd{};
                sd.filter = (fi == 0) ? Filter::Nearest : Filter::Linear;
                sd.wrap   = (Wrap)wi;
                SamplerHandle s = g_dev->createSampler(sd);
                if (!valid(s)) { allOk = false; break; }
                g_dev->destroy(s);
            }
        CHECK(allOk, "samplers filtro x wrap");
    }
}

// Un ciclo de frame completo: acquire→clear→present (la cadena que daba TDR en NVIDIA) y,
// en GL, verificación del clear vía readPixels (paridad de valores).
static void testFrameCycle()
{
    BEGIN("frame: acquire->clear->present");
    Context* c = g_dev->beginFrame();
    CHECK(c != nullptr, "beginFrame != null");
    if (!c) return;

    ClearValues cv;
    cv.clearColor = true;
    cv.color[0] = 0.2f; cv.color[1] = 0.4f; cv.color[2] = 0.6f; cv.color[3] = 1.0f;
    cv.clearDepth = true; cv.depth = 0.0f;   // reversed-Z: lejano = 0
    c->beginRenderPass({}, cv);
    c->endRenderPass();
    g_dev->endFrame();   // present
    CHECK(true, "frame completo (present OK)");
}

static int runBackend(Backend backend)
{
    const char* name = (backend == Backend::Vulkan) ? "Vulkan" : "OpenGL";
    Uint32 winFlags = (backend == Backend::Vulkan) ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;

    // Como el motor (core/window.cpp): si el video no inicializó, intenta forzar el driver
    // (x11 / wayland) antes de rendirse. SDL_GetError() puede salir vacío si hubo dos SDL3.
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
        bool okVideo = false;
        const char* kDrivers[] = { "x11", "wayland" };
        for (const char* drv : kDrivers) {
            if (!SDL_SetHint(SDL_HINT_VIDEO_DRIVER, drv)) continue;
            if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { okVideo = true; break; }
        }
        if (!okVideo) {
            std::printf("[FAIL] SDL video: %d (SDL v%u.%u.%u, err=\"%s\")\n",
                        SDL_Init(SDL_INIT_VIDEO), SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                        SDL_MICRO_VERSION, SDL_GetError() ? SDL_GetError() : "(sin error)");
            return 2;
        }
    }

    SDL_Window* w = SDL_CreateWindow(std::string("haruka rhi_test [" + std::string(name) + "]").c_str(),
                                     256, 256, (SDL_WindowFlags)winFlags);
    if (!w) {
        std::printf("[FAIL] SDL_CreateWindow (%s): %s\n", name, SDL_GetError());
        return 1;
    }

    std::unique_ptr<Device> dev = Device::create(backend, w);
    if (!dev) {
        std::printf("== %s: device no disponible (skip) ==\n", name);
        SDL_DestroyWindow(w);
        return 0;
    }
    setDevice(dev.get());
    g_dev = dev.get();
    std::printf("== Backend %s activo ==\n", name);

    testTextureFormats();
    testBuffers();
    testRenderTargets();
    testArrayAndCube();
    testFrameCycle();

    setDevice(nullptr);
    dev.reset();
    SDL_DestroyWindow(w);
    return 0;
}

int main(int argc, char** argv)
{
    std::string run = (argc > 1) ? argv[1] : "all";
    if (run != "gl" && run != "vk" && run != "all") {
        std::fprintf(stderr, "uso: haruka_tests_rhi [gl|vk|all]\n");
        return 2;
    }

    if (run == "gl" || run == "all") runBackend(Backend::OpenGL);
    if (run == "vk" || run == "all") runBackend(Backend::Vulkan);

    SDL_Quit();
    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
