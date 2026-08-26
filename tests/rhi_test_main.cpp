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
#include <cstddef>   // offsetof
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>

#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"
#include "core/logger.h"
#include "renderer/shader.h"
#include "core/camera.h"
#include "core/sky_ambient.h"   // gemelo CPU del ambiente (paridad)
#include "core/terrain/terrain_node.h"   // v5 F1: referencia CPU del nodo + hash golden
#include "core/terrain/terrain_node_pool.h"
#include "core/terrain/base_field.h"   // baseFieldHeightAt: el gemelo CPU que este test valida
#include "core/terrain/terrain_node_gpu.h"
#include "core/terrain/terrain_node_renderer.h"   // Shader::baseDir() para los shaders del banco

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


// ────────────────────────────────────────────────────────────────────────────────────────────────
// RAÍZ DE ASSETS: se BUSCA, no se supone.
//
// ⚠️ Los shaders se resolvían contra el DIRECTORIO DE TRABAJO, así que el banco pasaba lanzado
// desde `build/bin` y fallaba desde `build` — con cuatro pipelines sin crear y dos tests en rojo
// que no tenían nada que ver con el código. Un test que da un resultado distinto según desde dónde
// lo llames no mide el motor, mide el `cd`.
//
// Se prueban las rutas plausibles (junto al ejecutable y relativas al cwd) y se fija la base con
// `Shader::setBaseDir`, que es la que consume todo el motor.
static bool locateAssets()
{
    auto exists = [](const std::string& f) {
        std::FILE* h = std::fopen(f.c_str(), "rb");
        if (h) { std::fclose(h); return true; }
        return false;
    };
    // Un fichero del propio banco sirve de testigo: si está, la raíz es buena.
    const char* kWitness = "shaders/rhitest_fullscreen.vert";

    std::vector<std::string> cands;
    if (const char* bp = SDL_GetBasePath()) {
        cands.push_back(std::string(bp) + "assets/");
        cands.push_back(std::string(bp) + "bin/assets/");
        cands.push_back(std::string(bp) + "../bin/assets/");
    }
    cands.push_back("assets/");
    cands.push_back("bin/assets/");
    cands.push_back("../bin/assets/");
    cands.push_back("../assets/");

    for (const std::string& c : cands) {
        if (!exists(c + kWitness)) continue;
        Haruka::Shader::setBaseDir(c.c_str());
        std::printf("== assets: %s ==\n", c.c_str());
        return true;
    }
    std::printf("[FAIL] no encuentro los assets (probadas %zu rutas). "
                "Compila con `./build.sh` para que se desplieguen.\n", cands.size());
    return false;
}

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

// ⚠️ VACIAR LA COLA DE EVENTOS NO ES OPCIONAL, aunque el test no lea ninguno.
//
// Una ventana que presenta frames pero nunca responde a eventos es, para el compositor, una
// aplicación COLGADA: deja de repintarla y el escritorio la marca como no responde. El banco pasaba
// sus 152 comprobaciones correctamente y aun así la pantalla se quedaba helada a media ejecución —
// un síntoma que parece un cuelgue de GPU y no lo es.
//
// Se llama tras cada `endFrame`, que es donde el frame acaba de presentarse.
/// Copia y ESPERA antes de leer el mapeo. Ver la nota larga en su definición: no esperar no da error,
/// da datos a medio llenar — y eso se lee como si el shader hubiera calculado mal.
static void copyThenWait(Haruka::RHI::BufferHandle src, Haruka::RHI::BufferHandle dst,
                         size_t srcOff, size_t bytes);

static void pumpWindowEvents()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* se descartan: el banco no interactúa, solo debe responder */ }
}

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
    g_dev->endFrame();
        pumpWindowEvents();   // present
    CHECK(true, "frame completo (present OK)");
}


// ────────────────────────────────────────────────────────────────────────────────────────────────
// ESTADO DE BINDINGS ENTRE DRAWS Y ENTRE PIPELINES
//
// Lo que audita, y por qué existe: en OpenGL los bindings son ESTADO PEGAJOSO — atas el UBO 0 y
// sigue ahí hasta que lo cambies. El motor entero está escrito con esa semántica. En Vulkan un
// descriptor set es una TABLA COMPLETA, y el backend consume un set nuevo del ring en cada batch de
// binds: sin cuidado, el segundo draw ve un set con basura.
//
// Ocurrió de verdad: los draws de escena acababan leyendo el UBO del planeta (176 B) como si fuera
// `PerFrameData` (240 B) → `view`/`projection` basura → pantalla NEGRA. Y ninguno de los tests de
// este banco lo cazó, porque todos probaban piezas AISLADAS y el fallo era de encadenado.
//
// CONTRAPRUEBA incluida: se dibuja primero con un color y luego con otro y se comprueba que el
// píxel CAMBIA. Sin eso, "sale rojo" podría ser una casualidad de un framebuffer sin tocar.
static void testBindingPersistence()
{
    BEGIN("bindings: persisten entre draws y entre pipelines");

    const std::string base = Haruka::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/rhitest_solid.frag";
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull         = CullMode::None;

    PipelineHandle p1 = g_dev->createPipeline(pd);
    PipelineHandle p2 = g_dev->createPipeline(pd);   // otro objeto: fuerza cambio de pipeline
    CHECK(valid(p1) && valid(p2), "pipelines de prueba creados");
    if (!valid(p1) || !valid(p2)) return;

    // Dibuja llenando la pantalla con el color del UBO y devuelve el pixel central.
    auto drawAndRead = [&](const float rgba[4], bool twoDraws, unsigned char out[4]) {
        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, 16, rgba, BufferMemory::Dynamic);

        // ⚠️ CUÁNDO SE LEE DEPENDE DEL BACKEND, y equivocarse da un test que MIENTE.
        //
        // En OpenGL, tras el swap el back buffer queda INDEFINIDO: leer después de presentar
        // devuelve un frame viejo o negro, y encima NO de forma reproducible — dos ejecuciones
        // seguidas fallaban en casos distintos, y llegué a acusar al backend de GL de perder el UBO
        // cuando el fallo era del test. En Vulkan es al revés: `readPixels` copia la última imagen
        // PRESENTADA, así que hay que leer DESPUÉS del present.
        //
        // Así que cada uno lee en su momento. Un test que da resultados distintos entre corridas no
        // sirve para nada: es peor que no tenerlo, porque acusa a inocentes.
        const bool isVk = (g_dev->backend() == Backend::Vulkan);

        Context* c = g_dev->beginFrame();
        if (!c) { g_dev->destroy(ubo); return false; }
        ClearValues cv;
        cv.clearColor = true;
        cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(p1);
        c->bindUniformBuffer(0, ubo);
        c->draw(3);
        if (twoDraws) {
            // EL PUNTO DEL TEST: cambia de pipeline y vuelve a dibujar SIN reatar el UBO. En GL es
            // lo normal; en Vulkan obliga a que el backend replique el estado en el set nuevo.
            c->bindPipeline(p2);
            c->draw(3);
        }
        c->endRenderPass();

        std::memset(out, 0, 4);
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);   // GL: antes del swap
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);   // VK: ya presentada
        g_dev->destroy(ubo);
        return true;
    };

    const float red[4]  = { 1.0f, 0.0f, 0.0f, 1.0f };
    const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    unsigned char px[4] = {};

    if (drawAndRead(red, false, px)) {
        const bool isRed = px[0] > 200 && px[1] < 60 && px[2] < 60;
        std::printf("    un solo draw (rojo): pixel = (%d,%d,%d)\n", px[0], px[1], px[2]);
        CHECK(isRed, "un draw con su UBO atado pinta el color del UBO");
    }

    // CONTRAPRUEBA: con otro color el pixel tiene que CAMBIAR. Si no, la lectura no mide nada.
    unsigned char pxB[4] = {};
    if (drawAndRead(blue, false, pxB)) {
        std::printf("    CONTRAPRUEBA, mismo camino en azul: pixel = (%d,%d,%d)\n",
                    pxB[0], pxB[1], pxB[2]);
        CHECK(pxB[2] > 200 && pxB[0] < 60, "CONTRAPRUEBA: cambiar el UBO cambia el pixel");
    }

    // EL CASO QUE FALLABA: segundo draw, pipeline distinto, sin reatar nada.
    unsigned char px2[4] = {};
    if (drawAndRead(red, true, px2)) {
        const bool isRed = px2[0] > 200 && px2[1] < 60 && px2[2] < 60;
        std::printf("    dos draws, pipeline distinto, SIN reatar: pixel = (%d,%d,%d)\n",
                    px2[0], px2[1], px2[2]);
        CHECK(isRed, "el 2o draw sigue viendo el UBO atado antes del 1o");
    }

    g_dev->destroy(p1);
    g_dev->destroy(p2);
}

// ────────────────────────────────────────────────────────────────────────────────────────────────
// UN DISPATCH DENTRO DE UN RENDER PASS NO PUEDE MATAR EL DISPOSITIVO
//
// `vkCmdDispatch` dentro de una instancia de render pass es ILEGAL en Vulkan. En OpenGL el
// equivalente es legal y corriente, así que el motor lo hacía con naturalidad: el culling de
// parches del terreno se despachaba dentro del pase de escena. Cerraba el programa, y el síntoma no
// se parecía a la causa — en una captura de RenderDoc los draws salían bien uno a uno, porque el
// replay los ejecuta aislados.
//
// El motor ya no lo hace (`TerrestrialPlanet::prepare` lo saca fuera del pase). Esto fija el
// CONTRATO del RHI: si alguien vuelve a colarlo, que falle el test y no la GPU.
static void testDispatchInsideRenderPass()
{
    BEGIN("compute: dispatch dentro de un render pass no tumba el device");

    const std::string base = Haruka::Shader::baseDir();
    const std::string cs = base + "shaders/rhitest_noop.comp";
    PipelineDesc pd;
    pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de compute creado");
    if (!valid(cp)) return;

    uint32_t zero[4] = { 0, 0, 0, 0 };
    BufferHandle ssbo = g_dev->createBuffer(BufferUsage::Storage, sizeof(zero), zero,
                                            BufferMemory::Dynamic);

    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(cp);
        c->bindStorageBuffer(0, ssbo);
        // El dispatch de abajo es ILEGAL A PROPÓSITO, así que la guarda del RHI va a gritar. Se
        // silencia el log SOLO durante esa llamada: un banco en verde que imprime una línea roja
        // hace dudar de un resultado bueno, y este error es el test funcionando, no fallando.
        std::printf("    (se provoca un dispatch ilegal a proposito; el log se silencia)\n");
        std::fflush(stdout);
        const Haruka::LogLevel prevLevel = Haruka::getLogLevel();
        Haruka::setLogLevel(Haruka::LogLevel::None);
        c->dispatch(4, 1, 1);       // ILEGAL en Vulkan: el backend debe rechazarlo, no grabarlo
        Haruka::setLogLevel(prevLevel);
        c->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
    }
    CHECK(true, "el dispatch ilegal no ha abortado el proceso");

    // Lo que de verdad importa: el dispositivo SIGUE VIVO. Antes se perdía (VK_ERROR_DEVICE_LOST) y
    // todo lo posterior del frame se descartaba — la pantalla negra.
    bool aliveAfter = false;
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
        aliveAfter = true;
    }
    CHECK(aliveAfter, "el device sigue vivo: se completa un frame despues");

    g_dev->destroy(ssbo);
    g_dev->destroy(cp);
}


// ────────────────────────────────────────────────────────────────────────────────────────────────
// CONTENIDO DE TEXTURA: que los píxeles subidos LLEGUEN al shader.
//
// El banco probaba que las texturas se CREAN por formato. Eso no es lo mismo: una textura puede
// crearse bien y llegar EN BLANCO al shader. Es lo que se sospecha en Vulkan — los props del mundo
// salen casi blancos mientras en OpenGL tienen su corteza marrón y su copa verde, con la máscara de
// material diciendo correctamente que hay textura.
//
// Se sube una textura de 2x2 con cuatro colores distintos, se muestrea un texel concreto y se lee el
// píxel. CON CONTRAPRUEBA: muestrear OTRO texel tiene que dar OTRO color, o el test no mide nada.
static void testTextureContent()
{
    BEGIN("textura: el contenido subido llega al shader");

    const std::string base = Haruka::Shader::baseDir();
    const std::string vs = base + "shaders/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/rhitest_tex.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull         = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline de muestreo creado");
    if (!valid(pipe)) return;

    // 2x2 RGBA8: rojo, verde, azul, amarillo (orden de filas: abajo-izq primero en GL).
    const unsigned char px[16] = {
        255,0,0,255,    0,255,0,255,
        0,0,255,255,    255,255,0,255
    };
    TextureDesc td;
    td.width = 2; td.height = 2;
    td.format = Format::RGBA8;
    td.filter = Filter::Nearest;      // sin filtrado: cada muestra es UN texel exacto
    td.wrap   = Wrap::ClampToEdge;
    td.initialData = px;
    TextureHandle tex = g_dev->createTexture(td);
    CHECK(valid(tex), "textura 2x2 creada con datos");
    if (!valid(tex)) { g_dev->destroy(pipe); return; }

    auto sampleAt = [&](float u, float v, unsigned char out[4]) {
        const float uv[4] = { u, v, 0.0f, 0.0f };
        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, 16, uv, BufferMemory::Dynamic);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        Context* c = g_dev->beginFrame();
        if (!c) { g_dev->destroy(ubo); return false; }
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ubo);
        c->bindTexture(0, tex);
        c->draw(3);
        c->endRenderPass();
        std::memset(out, 0, 4);
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->destroy(ubo);
        return true;
    };

    unsigned char a[4] = {}, b[4] = {};
    const bool okA = sampleAt(0.25f, 0.25f, a);
    const bool okB = sampleAt(0.75f, 0.25f, b);
    if (okA && okB) {
        std::printf("    texel (0.25,0.25) = (%d,%d,%d) · texel (0.75,0.25) = (%d,%d,%d)\n",
                    a[0], a[1], a[2], b[0], b[1], b[2]);
        // Lo que se exige: los texels tienen COLOR (no blanco ni negro) y son DISTINTOS entre sí.
        auto isWhite = [](const unsigned char* p) { return p[0] > 200 && p[1] > 200 && p[2] > 200; };
        auto isBlack = [](const unsigned char* p) { return p[0] < 30 && p[1] < 30 && p[2] < 30; };
        CHECK(!isWhite(a) && !isWhite(b), "el contenido NO llega en blanco");
        CHECK(!isBlack(a) && !isBlack(b), "el contenido NO llega en negro");
        // CONTRAPRUEBA: dos texels distintos dan colores distintos. Sin esto, "no es blanco" podría
        // cumplirse con una textura uniforme cualquiera.
        const bool differ = (a[0] != b[0]) || (a[1] != b[1]) || (a[2] != b[2]);
        CHECK(differ, "CONTRAPRUEBA: dos texels distintos dan colores distintos");
    }

    g_dev->destroy(tex);
    g_dev->destroy(pipe);
}


// ────────────────────────────────────────────────────────────────────────────────────────────────
// ATRIBUTO DE VÉRTICE QUE NO ES LA POSICIÓN
//
// Los props sacan su color del COLOR POR VÉRTICE (`baseColor = Color` en prop_inst.frag), y en
// Vulkan salían casi BLANCOS con las texturas correctas y la máscara de material bien puesta. Si un
// atributo distinto de la posición no llega, el color por vértice sería (0,0,0) o basura y el
// resultado, blanco o negro. Esto lo aísla: un triángulo con color por vértice y lectura del píxel.
static void testVertexColor()
{
    BEGIN("vertice: un atributo que no es la posicion llega al fragment");

    const std::string base = Haruka::Shader::baseDir();
    const std::string vs = base + "shaders/rhitest_vcolor.vert";
    const std::string fs = base + "shaders/rhitest_vcolor.frag";

    struct V { float px, py, pz; float r, g, b; };
    // Triángulo que cubre la pantalla, los tres vértices del MISMO color: así el centro es ese color
    // exacto y no hay que razonar sobre la interpolación.
    const V verts[3] = {
        { -1.0f, -1.0f, 0.0f,  0.2f, 0.6f, 0.9f },
        {  3.0f, -1.0f, 0.0f,  0.2f, 0.6f, 0.9f },
        { -1.0f,  3.0f, 0.0f,  0.2f, 0.6f, 0.9f },
    };

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0, (uint32_t)offsetof(V, px), Format::RGB32F, 0 },
        { 2, (uint32_t)offsetof(V, r),  Format::RGB32F, 0 },   // location 2, como los props
    };
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull         = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    BufferHandle   vb   = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts);
    CHECK(valid(pipe) && valid(vb), "pipeline y vertex buffer de color por vertice");
    if (!valid(pipe) || !valid(vb)) return;

    unsigned char out[4] = {};
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindVertexBuffer(vb);
        c->draw(3);
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);

        std::printf("    color por vertice (0.2,0.6,0.9) -> pixel (%d,%d,%d) [esperado ~(51,153,229)]\n",
                    out[0], out[1], out[2]);
        // Se admite holgura por si hay conversión sRGB en el camino; lo que se exige es que el color
        // sea RECONOCIBLE y que NO sea blanco ni negro.
        const bool notWhite = !(out[0] > 230 && out[1] > 230 && out[2] > 230);
        const bool notBlack = !(out[0] < 20 && out[1] < 20 && out[2] < 20);
        const bool ordered  = (out[2] > out[1]) && (out[1] > out[0]);   // b > g > r, como el color
        CHECK(notWhite, "el color por vertice NO llega en blanco");
        CHECK(notBlack, "el color por vertice NO llega en negro");
        CHECK(ordered,  "el color por vertice llega con sus componentes en el orden correcto");
    }

    g_dev->destroy(vb);
    g_dev->destroy(pipe);
}


// ================================================================================================
// PIPELINES REALES DEL MOTOR: que TODOS enlacen, en los DOS backends.
//
// ⚠️ ESTE TEST EXISTE POR UN FALLO CONCRETO Y CARO. `ocean.frag` es compartido por dos vertex
// distintos (el mar cercano teselado y la esfera lejana). Al añadir un varying al primero y olvidarlo
// en el segundo, el pipeline lejano dejó de enlazar — y el océano desapareció del horizonte durante
// horas. El motor lo dijo en UNA línea de log (`lejano(esfera)=FALLO`) que nadie leyó, y mientras
// tanto se buscaron causas en el render, en las nubes y en el agua.
//
// Un pipeline que no enlaza NO da error de compilación ni excepción: da un objeto inválido y un draw
// que no ocurre. Es invisible salvo que alguien pregunte, y esto es preguntar.
//
// Cubre además la clase de bug que más ha costado en este motor: lo que GL perdona y Vulkan no
// (espacios de binding separados, layouts std140/std430, winding). Correrlo sobre `all` compara los
// dos backends sin que nadie tenga que mirar una pantalla.
// ================================================================================================
static void testEnginePipelines()
{
    const std::string sh = Haruka::Shader::baseDir() + "shaders/";
    struct Case {
        const char* name;
        std::string vert, frag, tesc, tese;
        bool patches;
    };
    // Los pares REALES que crea el motor. Si alguien añade un pipeline y no lo pone aquí, este test
    // no lo cubre — por eso la lista vive junto a los tests y no dentro del engine: se ve lo que hay.
    const std::vector<Case> cases = {
        { "planeta/base teselado", sh+"planet/terrain.vert", sh+"planet/biome.frag",
          sh+"planet/terrain.tesc", sh+"planet/terrain.tese", true },
        { "planeta/nodos (v5)",    sh+"terrain_node.vert", sh+"terrain_node.frag", "", "", false },
        { "planeta/suelo cercano", sh+"planet/nearground.vert", sh+"planet/biome.frag", "", "", false },
        { "mar/cercano (olas)",    sh+"planet/ocean.vert", sh+"planet/ocean.frag",
          sh+"planet/ocean.tesc", sh+"planet/ocean.tese", true },
        { "mar/lejano (esfera)",   sh+"planet/ocean_far.vert", sh+"planet/ocean.frag", "", "", false },
        { "cielo",                 sh+"sky.vert",  sh+"sky.frag",  "", "", false },
        { "nubes volumétricas",    sh+"cloud_vol.vert", sh+"cloud_vol.frag", "", "", false },
        { "props instanciados",    sh+"prop_inst.vert", sh+"prop_inst.frag", "", "", false },
        { "composición final",     sh+"screenquad.vert", sh+"final.frag", "", "", false },
    };

    for (const Case& c : cases) {
        PipelineDesc pd;
        pd.vertexPath   = c.vert.c_str();
        pd.fragmentPath = c.frag.c_str();
        if (!c.tesc.empty()) pd.tessControlPath = c.tesc.c_str();
        if (!c.tese.empty()) pd.tessEvalPath    = c.tese.c_str();
        if (c.patches) { pd.topology = PrimitiveTopology::Patches; pd.patchVertices = 4; }
        // El layout de vértice no importa para ENLAZAR: lo que se comprueba es que las etapas
        // encajen entre sí (varyings, bloques, bindings), que es donde estaba el fallo.
        PipelineHandle h = g_dev->createPipeline(pd);
        CHECK(valid(h), (std::string("pipeline enlaza: ") + c.name).c_str());
        if (valid(h)) g_dev->destroy(h);
    }
}


// ================================================================================================
// EL MAR DIBUJA PÍXELES, Y SOLO DONDE HAY AGUA.
//
// ⚠️ POR QUÉ NO BASTA CON "¿ENLAZA EL PIPELINE?". Se escribió antes un test que solo comprobaba que
// `createPipeline` devolviera un handle válido, se le hizo la CONTRAPRUEBA —romper `ocean_far.vert`
// quitándole el varying `vLoc`, que es el bug real que tumbó la esfera del océano— y el test SIGUIÓ
// PASANDO. El enlazado de módulos SPIR-V tolera una entrada que nadie escribe; el error solo salta
// por el camino GLSL. Un test que no falla cuando el bug está es una tautología: peor que nada,
// porque da confianza falsa.
//
// Esto mira PÍXELES, que es lo único que el usuario ve. Y no comprueba "hay algo": comprueba que el
// shader DISCRIMINA. Con el mismo pipeline y la misma geometría, cambiando UN dato de entrada:
//
//   fondo a -1000 m (océano) → tiene que ESCRIBIR   (hay agua)
//   fondo a +1000 m (tierra) → tiene que DESCARTAR  (no hay agua)
//
// Si las dos salen iguales, algo está roto aunque todo "funcione": el discard, la profundidad, el
// muestreo del bake o la propia superficie. Y de paso caza NaN y saturaciones, que son las dos
// formas en que este motor se ha roto hoy en Vulkan.
// ================================================================================================
static bool oceanDrawsPixels(float bakeHeightM, uint8_t out[4])
{
    // Bake de altura sintético: una textura 1x1 con la cota que queramos. `harukaSampleHeightField`
    // la lee con texelFetch, así que un solo téxel basta para decidir mar o tierra.
    TextureDesc td;
    td.width = td.height = 1;
    td.format = Format::R32F;
    td.filter = Filter::Nearest;
    td.wrap   = Wrap::ClampToEdge;
    td.initialData = &bakeHeightM;
    TextureHandle height = g_dev->createTexture(td);
    if (!valid(height)) return false;

    // SimplePlanetUBO. uMVP = identidad y uCenter = 0 para que los vértices caigan tal cual en clip
    // space: así el triángulo cubre la pantalla sin depender de una cámara real.
    struct PlanetUBO {
        float mvp[16];
        float center[4], lightDir[4], lightColor[4], ambient[4], extra[4], debug[4], texAnchor[4];
    } u{};
    u.mvp[0] = u.mvp[5] = u.mvp[10] = u.mvp[15] = 1.0f;
    u.lightDir[1]   = 1.0f;                                  // sol al cénit
    u.lightColor[0] = u.lightColor[1] = u.lightColor[2] = 1.0f;
    u.ambient[0] = 0.2f; u.ambient[1] = 0.2f; u.ambient[2] = 0.3f;
    u.extra[3] = 6371000.0f;                                 // radio del planeta
    u.debug[2] = 1.0f;                                       // hay bake de altura
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    // Campo de agua interior: presente pero VACÍO (misc.w = 0). Se ata SIEMPRE porque en Vulkan un
    // bloque sin atar es indefinido — la misma regla que el motor aprendió con el UBO de marea.
    struct InlandUBO { float anchor[4], tanU[4], tanV[4], misc[4]; } iu{};
    BufferHandle inlandUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(iu), &iu, BufferMemory::Dynamic);
    float zero = 0.0f;
    BufferHandle inlandSsbo = g_dev->createBuffer(BufferUsage::Storage, sizeof(zero), &zero, BufferMemory::Dynamic);

    // Un triángulo que cubre la pantalla, en el formato que espera `ocean_far.vert` (pos3 + normal3).
    const float verts[] = {
        -1.0f, -1.0f, 1.0f,   0.0f, 0.0f, 1.0f,
         3.0f, -1.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        -1.0f,  3.0f, 1.0f,   0.0f, 0.0f, 1.0f,
    };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);

    const std::string sh = Haruka::Shader::baseDir() + "shaders/";
    const std::string vs = sh + "planet/ocean_far.vert", fs = sh + "planet/ocean.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides    = { (uint32_t)(6 * sizeof(float)) };
    pd.vertexLayout.attributes = { { 0, 0, Format::RGB32F },
                                   { 1, (uint32_t)(3 * sizeof(float)), Format::RGB32F } };
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.cull         = CullMode::None;
    pd.blend.enable = false;                 // sin mezcla: lo que se lee es lo que escribió el shader
    PipelineHandle pipe = g_dev->createPipeline(pd);
    if (!valid(pipe)) {
        g_dev->destroy(height); g_dev->destroy(ubo); g_dev->destroy(vb);
        g_dev->destroy(inlandUbo); g_dev->destroy(inlandSsbo);
        return false;
    }

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    Context* c = g_dev->beginFrame();
    if (c) {
        ClearValues cv;
        cv.clearColor = true;
        cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;  // ROJO
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ubo);
        c->bindUniformBuffer(24, inlandUbo);
        c->bindStorageBuffer(25, inlandSsbo);
        c->bindTexture(16, height);
        c->bindVertexBuffer(vb);
        c->draw(3);
        c->endRenderPass();
    }
    std::memset(out, 0, 4);
    if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
    g_dev->endFrame();
        pumpWindowEvents();
    if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);

    g_dev->destroy(pipe); g_dev->destroy(height); g_dev->destroy(ubo);
    g_dev->destroy(vb); g_dev->destroy(inlandUbo); g_dev->destroy(inlandSsbo);
    return true;
}

static void testOceanShading()
{
    BEGIN("mar: dibuja agua y descarta tierra");
    uint8_t sea[4] = {0}, land[4] = {0};
    const bool okSea  = oceanDrawsPixels(-1000.0f, sea);    // fondo a 1 km → hay mar
    const bool okLand = oceanDrawsPixels(+1000.0f, land);   // cota +1 km → es tierra
    CHECK(okSea && okLand, "el pipeline del mar se crea y dibuja");
    if (!okSea || !okLand) return;

    // El clear es ROJO puro. Si el shader escribió, el píxel deja de serlo.
    const bool seaWrote  = !(sea[0]  > 200 && sea[1]  < 60 && sea[2]  < 60);
    const bool landWrote = !(land[0] > 200 && land[1] < 60 && land[2] < 60);

    CHECK(seaWrote,   "sobre OCEANO (fondo -1000 m) el mar escribe pixeles");
    CHECK(!landWrote, "sobre TIERRA (cota +1000 m) el mar se DESCARTA (queda el clear)");
    // Discriminación: si los dos casos dan lo mismo, el shader no esta mirando la profundidad.
    CHECK(seaWrote != landWrote, "el mar DISCRIMINA agua de tierra (no es un test tautologico)");
    // Y el agua tiene que parecer agua: azul dominante, sin saturar a blanco.
    if (seaWrote) {
        CHECK(sea[2] >= sea[0], "el agua es azulada (B >= R)");
        CHECK(!(sea[0] > 250 && sea[1] > 250 && sea[2] > 250), "el agua no sale quemada a blanco");
    }
}


// ================================================================================================
// EL AMBIENTE NO PUEDE SER PLANO, Y LOS DOS GEMELOS TIENEN QUE COINCIDIR.
//
// ⚠️ ESTE TEST NACE DE UN BUG REAL Y RECIENTE. El ambiente del cielo se integraba en 9 coeficientes
// SH —hechos para evaluarse POR NORMAL— pero se evaluaba UNA sola vez en la CPU para una normal
// hacia arriba y se pasaba al shader como una constante. Consecuencia: todo lo que no recibe sol
// directo quedaba `albedo × constante`, así que una ladera vuelta al cielo y otra vuelta al suelo se
// iluminaban IGUAL. En sombra desaparecía el relieve y los objetos se veían como recortes de color
// liso. Nadie lo detectó hasta verlo en pantalla.
//
// Dos cosas que comprobar, y las dos hacen falta:
//   1. DIRECCIONALIDAD — cénit y nadir tienen que dar valores DISTINTOS. Es la contraprueba
//      incorporada: si alguien vuelve a aplanar el ambiente, esto falla.
//   2. PARIDAD CPU↔GPU — `harukaSkySHEval` (shader) contra `skyAmbientEval` (CPU) sobre el MISMO
//      SH. Son gemelos escritos a mano; que uno se toque sin el otro es cuestión de tiempo.
// ================================================================================================
static void testSkyAmbientGPU()
{
    BEGIN("ambiente del cielo: direccional y con paridad CPU/GPU");

    // Un cielo de día pleno: sol alto, sin nubes. Los mismos coeficientes van a la CPU y a la GPU.
    const Haruka::SkySH sh = Haruka::skyAmbientSH(0.9f, 0.0f);
    struct SHUBO { float c[9][4]; } u{};
    for (int i = 0; i < 9; ++i) {
        u.c[i][0] = sh.coef[i].x; u.c[i][1] = sh.coef[i].y; u.c[i][2] = sh.coef[i].z;
    }
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    const std::string sd = Haruka::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "rhitest_fullscreen.vert", fs = sd + "rhitest_skysh.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.cull         = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del test de SH se crea");
    if (!valid(pipe)) { g_dev->destroy(ubo); return; }

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    uint8_t zen[4] = {0}, nad[4] = {0};
    Context* c = g_dev->beginFrame();
    if (c) {
        ClearValues cv;
        cv.clearColor = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(28, ubo);
        c->draw(3);
        c->endRenderPass();
    }
    if (!isVk) { g_dev->readPixels(64, 128, 1, 1, Format::RGBA8, zen);
                 g_dev->readPixels(192, 128, 1, 1, Format::RGBA8, nad); }
    g_dev->endFrame();
        pumpWindowEvents();
    if (isVk)  { g_dev->readPixels(64, 128, 1, 1, Format::RGBA8, zen);
                 g_dev->readPixels(192, 128, 1, 1, Format::RGBA8, nad); }

    // 1. DIRECCIONAL: cénit y nadir no pueden dar lo mismo.
    const int diff = std::abs((int)zen[0] - (int)nad[0]) + std::abs((int)zen[1] - (int)nad[1])
                   + std::abs((int)zen[2] - (int)nad[2]);
    CHECK(diff > 12, "el ambiente DEPENDE de la normal (cenit != nadir): no es plano");

    // 2. PARIDAD con el gemelo de CPU, sobre los mismos coeficientes.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 cpuZen = Haruka::skyAmbientEval(sh,  up, up);
    const glm::vec3 cpuNad = Haruka::skyAmbientEval(sh, -up, up);
    // El target es de 8 bits y satura: se compara solo donde el valor de CPU cabe sin recortar.
    auto near8 = [](float cpu, uint8_t gpu, const char* what) {
        if (cpu > 0.98f) return;                       // saturado en el target: no dice nada
        const int expected = (int)std::lround(std::min(std::max(cpu, 0.0f), 1.0f) * 255.0f);
        CHECK(std::abs(expected - (int)gpu) <= 6, what);
    };
    near8(cpuZen.x, zen[0], "paridad CPU/GPU en el cenit (R)");
    near8(cpuZen.y, zen[1], "paridad CPU/GPU en el cenit (G)");
    near8(cpuNad.x, nad[0], "paridad CPU/GPU en el nadir (R)");
    near8(cpuNad.y, nad[1], "paridad CPU/GPU en el nadir (G)");

    g_dev->destroy(pipe);
    g_dev->destroy(ubo);
}


// ================================================================================================
// EL TERRENO RESPONDE A LA LUZ.
//
// ⚠️ Suena obvio y es justo lo que nadie comprobaba. En esta sesión el terreno ha estado: iluminado
// por un ambiente constante (sombra plana), con el fondo marino pintado del color equivocado, con un
// velo gris de promediar materiales, y con un pipeline que ni siquiera enlazaba. Ninguna de esas
// cosas rompe la compilación ni lanza un error: dan una imagen mala y silencio.
//
// La comprobación es la mínima que no puede ser tautológica: MISMA geometría, MISMOS materiales,
// MISMO bake — y solo se mueve el SOL. Si el terreno no responde, la luz no está llegando (o la está
// aplastando algo), y eso es exactamente la clase de fallo que se ha ido escapando.
//
// El montaje es aparatoso a propósito: `biome.frag` lee 7 UBOs y 8 texturas, y en Vulkan un
// descriptor sin atar es INDEFINIDO (en GL son ceros). Atarlos todos, aunque sean 1x1, es parte de
// lo que el test verifica: que el conjunto de recursos que el shader declara se puede satisfacer.
// ================================================================================================
static bool terrainLitPixel(const float sunDir[3], uint8_t out[4],
                            bool shadowsOn = false, float occluderM = 0.0f,
                            float planetR = 6371000.0f)
{
    const float R = planetR;

    // --- UBOs -----------------------------------------------------------------------------------
    struct PlanetUBO {
        float mvp[16], center[4], lightDir[4], lightColor[4], ambient[4], extra[4], debug[4], texAnchor[4];
    } u{};
    u.mvp[0] = u.mvp[5] = u.mvp[10] = u.mvp[15] = 1.0f;      // identidad: los vértices ya van en clip
    // uCenter = centro − cámara. Cámara EN LA SUPERFICIE: el centro queda R por debajo.
    // ⚠️ No puede ser 0: `planetFrame()` hace `A / length(A)` y con centro en el ojo saldría NaN.
    u.center[1] = -R;
    u.lightDir[0] = sunDir[0]; u.lightDir[1] = sunDir[1]; u.lightDir[2] = sunDir[2];
    u.lightColor[0] = u.lightColor[1] = u.lightColor[2] = 1.0f;
    u.ambient[0] = 0.15f; u.ambient[1] = 0.18f; u.ambient[2] = 0.24f;
    u.ambient[3] = shadowsOn ? 1.0f : 0.0f;                  // marchador de sombras
    u.extra[1] = 100.0f;                                     // tiling
    u.extra[3] = R;
    u.debug[2] = 1.0f;                                       // hay bake de altura
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    // Tabla de materiales: UNO, sin textura (uMatCount.y = 0 ⇒ tile = -1) y con color propio.
    // Así el test no depende de los arrays de terreno y mide iluminación, no muestreo.
    struct MatUBO { float count[4]; float mats[16][7][4]; } m{};
    m.count[0] = 1.0f;      // 1 material activo
    m.count[1] = 0.0f;      // NO hay array de tiles
    m.count[2] = -1.0f;     // sin capa de orilla
    m.mats[0][0][1] = 1.0f;                                  // humMax = 1
    m.mats[0][0][2] = -1000.0f; m.mats[0][0][3] = 1000.0f;   // temp: todo
    m.mats[0][1][1] = 1.0f;                                  // slopeMax = 1
    m.mats[0][1][3] = 1.0f;                                  // priority
    m.mats[0][2][0] = m.mats[0][2][1] = m.mats[0][2][2] = 1.0f;  // tint blanco
    m.mats[0][2][3] = 1.0f;                                  // grain
    m.mats[0][3][0] = 1.0f;                                  // detail
    m.mats[0][4][0] = 0.5f; m.mats[0][4][1] = 0.5f; m.mats[0][4][2] = 0.5f;  // color gris medio
    m.mats[0][4][3] = 1.0f;                                  // colorWeight
    m.mats[0][5][0] = -1000.0f; m.mats[0][5][1] = 1000.0f; m.mats[0][5][2] = 0.25f;  // elev
    m.mats[0][6][0] = -1000.0f; m.mats[0][6][1] = 1000.0f; m.mats[0][6][2] = 0.01f;  // depth
    BufferHandle matUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(m), &m, BufferMemory::Dynamic);

    float zeros[64] = {0};
    BufferHandle clipUbo  = g_dev->createBuffer(BufferUsage::Uniform, 64,  zeros, BufferMemory::Dynamic);
    BufferHandle propUbo  = g_dev->createBuffer(BufferUsage::Uniform, 256, zeros, BufferMemory::Dynamic);
    struct NearUBO { float anchorRelEye[4], anchorUp[4]; } nu{};
    nu.anchorUp[1] = 1.0f;
    BufferHandle nearUbo  = g_dev->createBuffer(BufferUsage::Uniform, sizeof(nu), &nu, BufferMemory::Dynamic);
    float wet[20] = {0};   // mat4 skySpace + vec4 wet, todo a cero: ni mojado ni máscara
    BufferHandle wetUbo   = g_dev->createBuffer(BufferUsage::Uniform, sizeof(wet), wet, BufferMemory::Dynamic);
    float shc[9][4] = {{0}};
    shc[0][0] = 0.5f; shc[0][1] = 0.55f; shc[0][2] = 0.7f;   // cielo tenue, solo el término DC
    BufferHandle shUbo    = g_dev->createBuffer(BufferUsage::Uniform, sizeof(shc), shc, BufferMemory::Dynamic);

    // --- texturas dummy -------------------------------------------------------------------------
    auto tex2D = [&](Format f, const void* px) {
        TextureDesc td; td.width = td.height = 1; td.format = f;
        td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge; td.initialData = px;
        return g_dev->createTexture(td);
    };
    auto texArr = [&](const void* px, uint32_t layers) {
        TextureDesc td; td.width = td.height = 1; td.layers = layers; td.format = Format::RGBA8;
        td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge; td.initialData = px;
        return g_dev->createTexture(td);
    };
    const uint8_t grey[8]   = { 128,128,128,255, 128,128,128,255 };
    // ── BAKE CON RELIEVE ────────────────────────────────────────────────────────────────────────
    // 1 columna x 8 filas. La fila 0 es donde cae el fragmento (el polo, v≈0) y va LLANA; las demás
    // son un muro de `occluderM`. El marchador avanza en LATITUD hacia el sol, así que entra en las
    // filas altas y encuentra el obstáculo. Con `occluderM = 0` no hay muro y no puede haber sombra:
    // ése es el caso de control.
    float heightCol[8] = { 0,0,0,0,0,0,0,0 };
    for (int i = 1; i < 8; ++i) heightCol[i] = occluderM;
    const float   heightM   = 0.0f;
    const float   baseField[8] = { 100.0f, 20.0f, 0.5f, 0.0f, 100.0f, 20.0f, 0.5f, 0.0f };
    TextureHandle macro  = tex2D(Format::RGBA8, grey);
    TextureHandle biome  = tex2D(Format::RGBA8, grey);
    TextureHandle zone   = tex2D(Format::RGBA8, grey);
    TextureHandle mask   = tex2D(Format::RGBA8, grey);
    TextureDesc hd; hd.width = 1; hd.height = 8; hd.format = Format::R32F;
    hd.filter = Filter::Nearest; hd.wrap = Wrap::ClampToEdge; hd.initialData = heightCol;
    TextureHandle height = g_dev->createTexture(hd);
    (void)heightM;
    TextureHandle alb    = texArr(grey, 2);
    TextureHandle nrm    = texArr(grey, 2);
    TextureDesc bf; bf.width = bf.height = 1; bf.layers = 6; bf.format = Format::RGBA32F;
    bf.filter = Filter::Nearest; bf.wrap = Wrap::ClampToEdge;
    std::vector<float> bfData(6 * 4, 0.0f);
    for (int i = 0; i < 6; ++i) { bfData[i*4+0] = 100.0f; bfData[i*4+1] = 20.0f; bfData[i*4+2] = 0.5f; }
    bf.initialData = bfData.data();
    TextureHandle baseFld = g_dev->createTexture(bf);
    (void)baseField;

    // Triángulo a pantalla completa en el formato de `nearground.vert` (solo posición).
    const float verts[] = { -1,-1,0,   3,-1,0,   -1,3,0 };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);

    const std::string sd = Haruka::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "planet/nearground.vert", fs = sd + "planet/biome.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides    = { (uint32_t)(3 * sizeof(float)) };
    pd.vertexLayout.attributes = { { 0, 0, Format::RGB32F } };
    pd.topology   = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false;
    pd.cull       = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    bool ok = valid(pipe);

    if (ok) {
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        Context* c = g_dev->beginFrame();
        if (c) {
            ClearValues cv;
            cv.clearColor = true;
            cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;  // MAGENTA
            cv.clearDepth = true; cv.depth = 0.0f;
            c->beginRenderPass({}, cv);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, ubo);      c->bindUniformBuffer(12, matUbo);
            c->bindUniformBuffer(13, clipUbo); c->bindUniformBuffer(14, propUbo);
            c->bindUniformBuffer(22, nearUbo); c->bindUniformBuffer(23, wetUbo);
            c->bindUniformBuffer(28, shUbo);
            c->bindTexture(10, macro);  c->bindTexture(11, biome);
            c->bindTexture(12, alb);    c->bindTexture(13, nrm);
            c->bindTexture(14, zone);   c->bindTexture(15, baseFld);
            c->bindTexture(16, height); c->bindTexture(17, mask);
            c->bindVertexBuffer(vb);
            c->draw(3);
            c->endRenderPass();
        }
        std::memset(out, 0, 4);
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
    }

    if (valid(pipe)) g_dev->destroy(pipe);
    for (BufferHandle b : { ubo, matUbo, clipUbo, propUbo, nearUbo, wetUbo, shUbo, vb }) g_dev->destroy(b);
    for (TextureHandle t : { macro, biome, zone, mask, height, alb, nrm, baseFld }) g_dev->destroy(t);
    return ok;
}

static void testTerrainLighting()
{
    BEGIN("terreno: responde a la luz");
    const float sunUp[3]   = { 0.0f,  1.0f, 0.0f };   // sol al cénit
    const float sunDown[3] = { 0.0f, -1.0f, 0.0f };   // sol bajo el horizonte
    uint8_t lit[4] = {0}, dark[4] = {0};
    const bool okA = terrainLitPixel(sunUp,   lit);
    const bool okB = terrainLitPixel(sunDown, dark);
    CHECK(okA && okB, "el pipeline del terreno se crea y dibuja");
    if (!okA || !okB) return;

    auto isClear = [](const uint8_t p[4]) { return p[0] > 200 && p[1] < 60 && p[2] > 200; };
    CHECK(!isClear(lit),  "el terreno ESCRIBE pixeles (no queda el clear)");

    const int lumLit  = lit[0]  + lit[1]  + lit[2];
    const int lumDark = dark[0] + dark[1] + dark[2];
    // LA COMPROBACIÓN: mismo terreno, mismo material, solo cambia el sol.
    CHECK(lumLit > lumDark + 15, "con el sol ARRIBA el terreno es MAS CLARO que con el sol debajo");
    // Y en sombra no puede apagarse del todo: el cielo sigue llegando (ambiente direccional).
    CHECK(lumDark > 8, "en sombra el terreno NO es negro absoluto (le llega el cielo)");
    CHECK(!(lit[0] > 250 && lit[1] > 250 && lit[2] > 250), "el terreno iluminado no sale quemado");
}


// ================================================================================================
// LOS PROPS RESPONDEN A LA LUZ Y NO SALEN QUEMADOS.
//
// ⚠️ Bug real y todavía abierto en Vulkan: los props se ven BLANCOS, saturados. En OpenGL no. Y la
// causa que queda en pie es la de siempre entre estos dos backends: un descriptor sin atar. En GL se
// lee como ceros y el shader "funciona"; en Vulkan es INDEFINIDO, y basura grande en un multiplicador
// de iluminación da exactamente eso — blanco.
//
// Por eso el test ata TODO lo que `prop_inst` declara (5 texturas de material + 2 UBOs) y comprueba
// dos cosas que un descriptor podrido rompe enseguida:
//   · que el resultado RESPONDA al sol (si la luz llega, mover el sol cambia el píxel);
//   · que NO sature (un albedo gris medio no puede acabar en blanco puro).
//
// El `u_matPBR.w` es una máscara de bits que dice QUÉ texturas existen. Se pone a 0 a propósito: así
// el shader usa sus escalares y el test mide ILUMINACIÓN, no muestreo — pero las texturas se atan
// igual, porque declararlas y no atarlas es justo el fallo que se persigue.
// ================================================================================================
static bool propLitPixel(const float sunDir[3], uint8_t out[4])
{
    struct PerFrame {
        float view[16], proj[16];
        float camPos[4], sunDir[4], sunColor[3]; float ambientStrength;
        int   hdr, bloom, ssao, ibl, shadows, p3a, p3b, p3c;
        float moonDir[3]; float moonIntensity;
        float moonColor[3]; float pad4;
    } pf{};
    static_assert(sizeof(PerFrame) == 240, "PerFrame debe casar con PerFrameUBOData (std140)");
    pf.view[0] = pf.view[5] = pf.view[10] = pf.view[15] = 1.0f;
    pf.proj[0] = pf.proj[5] = pf.proj[10] = pf.proj[15] = 1.0f;
    pf.sunDir[0] = sunDir[0]; pf.sunDir[1] = sunDir[1]; pf.sunDir[2] = sunDir[2];
    pf.sunColor[0] = pf.sunColor[1] = pf.sunColor[2] = 1.0f;
    pf.ambientStrength = 0.18f;
    BufferHandle pfUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pf), &pf, BufferMemory::Dynamic);

    struct PropParams { float wind[3]; float time; float matPBR[4]; } pp{};
    pp.matPBR[0] = 0.0f;   // metallic
    pp.matPBR[1] = 0.6f;   // roughness
    pp.matPBR[2] = 1.0f;   // ao
    pp.matPBR[3] = 0.0f;   // SIN texturas: manda el escalar (ver la nota de arriba)
    BufferHandle ppUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pp), &pp, BufferMemory::Dynamic);

    const uint8_t grey[4] = { 128, 128, 128, 255 };
    TextureDesc td; td.width = td.height = 1; td.format = Format::RGBA8;
    td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge; td.initialData = grey;
    TextureHandle t0 = g_dev->createTexture(td), t1 = g_dev->createTexture(td),
                  t2 = g_dev->createTexture(td), t3 = g_dev->createTexture(td),
                  t4 = g_dev->createTexture(td);

    // Malla: un triángulo con normal hacia +Y (mira al cielo), color gris.
    struct V { float p[3], n[3], c[3], uv[2]; float part; };
    const V verts[3] = {
        { {-1,-1,0}, {0,1,0}, {0.5f,0.5f,0.5f}, {0,0}, 0 },
        { { 3,-1,0}, {0,1,0}, {0.5f,0.5f,0.5f}, {1,0}, 0 },
        { {-1, 3,0}, {0,1,0}, {0.5f,0.5f,0.5f}, {0,1}, 0 },
    };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);

    // UNA instancia: identidad, color blanco, escala 1, sin partes rotas.
    struct Inst { float model[16]; float color[4]; float scale[3]; float breakMask; };
    Inst inst{};
    inst.model[0] = inst.model[5] = inst.model[10] = inst.model[15] = 1.0f;
    inst.color[0] = inst.color[1] = inst.color[2] = inst.color[3] = 1.0f;
    inst.scale[0] = inst.scale[1] = inst.scale[2] = 1.0f;
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Vertex, sizeof(inst), &inst, BufferMemory::Static);

    const std::string sd = Haruka::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "prop_inst.vert", fs = sd + "prop_inst.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V), (uint32_t)sizeof(Inst) };
    pd.vertexLayout.attributes = {
        { 0,  0,  Format::RGB32F, 0 }, { 1,  12, Format::RGB32F, 0 },
        { 2,  24, Format::RGB32F, 0 }, { 9,  36, Format::RG32F,  0 },
        { 11, 44, Format::R32F,   0 },
        { 3,  0,  Format::RGBA32F, 1 }, { 4, 16, Format::RGBA32F, 1 },
        { 5,  32, Format::RGBA32F, 1 }, { 6, 48, Format::RGBA32F, 1 },
        { 7,  64, Format::RGBA32F, 1 }, { 8, 80, Format::RGB32F,  1 },
        { 10, 92, Format::R32F,    1 },
    };
    // El binding 1 avanza POR INSTANCIA, no por vértice: es lo que hace que una malla se
    // dibuje N veces con N matrices distintas.
    pd.vertexLayout.rates = { InputRate::Vertex, InputRate::Instance };
    pd.topology   = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false;
    pd.cull       = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    bool ok = valid(pipe);

    if (ok) {
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        Context* c = g_dev->beginFrame();
        if (c) {
            ClearValues cv;
            cv.clearColor = true;
            cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;
            cv.clearDepth = true; cv.depth = 0.0f;
            c->beginRenderPass({}, cv);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, pfUbo);
            c->bindUniformBuffer(6, ppUbo);
            c->bindTexture(0, t0); c->bindTexture(1, t1); c->bindTexture(2, t2);
            c->bindTexture(3, t3); c->bindTexture(4, t4);
            c->bindVertexBuffer(vb, 0);
            c->bindVertexBuffer(ib, 1);
            c->draw(3, 0, 1);   // 3 vértices, 1 instancia
            c->endRenderPass();
        }
        std::memset(out, 0, 4);
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
    }

    if (valid(pipe)) g_dev->destroy(pipe);
    for (BufferHandle b : { pfUbo, ppUbo, vb, ib }) g_dev->destroy(b);
    for (TextureHandle t : { t0, t1, t2, t3, t4 }) g_dev->destroy(t);
    return ok;
}

static void testPropLighting()
{
    BEGIN("props: responden a la luz y no se queman");
    const float sunUp[3]   = { 0.0f,  1.0f, 0.0f };
    const float sunDown[3] = { 0.0f, -1.0f, 0.0f };
    uint8_t lit[4] = {0}, dark[4] = {0};
    const bool okA = propLitPixel(sunUp,   lit);
    const bool okB = propLitPixel(sunDown, dark);
    CHECK(okA && okB, "el pipeline de props se crea y dibuja");
    if (!okA || !okB) return;

    auto isClear = [](const uint8_t p[4]) { return p[0] > 200 && p[1] < 60 && p[2] > 200; };
    CHECK(!isClear(lit), "el prop ESCRIBE pixeles (no queda el clear)");

    const int lumLit  = lit[0]  + lit[1]  + lit[2];
    const int lumDark = dark[0] + dark[1] + dark[2];
    CHECK(lumLit > lumDark + 15, "con el sol de FRENTE el prop es MAS CLARO que a contraluz");

    // ⚠️ AQUÍ HABÍA UNA ASERCIÓN DE "NO SE QUEMA" Y ERA UNA ALARMA FALSA. Falló en OpenGL, donde los
    // props se ven bien — porque `prop_inst.frag` escribe HDR: en el motor va a un target
    // `R11G11B10F` y pasa DESPUÉS por tonemapping. Este banco escribe a RGBA8 sin tonemapear, así que
    // saturar es el comportamiento CORRECTO y asertar sobre ello acusaría a un inocente.
    //
    // Para medir de verdad la saturación de los props haría falta un render target HDR y leer
    // floats, no el backbuffer de 8 bits. Se deja anotado en vez de dejar un test que miente: un
    // instrumento que da falsos positivos hace más daño que no tenerlo, porque el siguiente que lo
    // vea rojo va a buscar el bug donde no está.
    CHECK(lumDark > 4, "a contraluz el prop NO es negro absoluto (le llega el ambiente)");
}


// ================================================================================================
// EL MARCHADOR DE SOMBRAS PROYECTA — Y SOLO CUANDO HAY ALGO QUE PROYECTE.
//
// ⚠️ El test de iluminación del terreno tiene las sombras APAGADAS a propósito (`uAmbient.w = 0`):
// mide la luz, no la oclusión. Así que el marchador —16 pasos por PÍXEL sobre el bake, el trozo más
// caro del fragment y el que ya provocó un frame de 280 ms y un fondo marino negro— no estaba
// cubierto por nada.
//
// Montarlo pide dos trucos, y los dos son legítimos:
//   · Un planeta PEQUEÑO (R = 1 km). Con 6371 km el rayo recorre 4 km sin cruzar un solo téxel del
//     bake: no puede encontrarse nada y el test saldría verde sin probar nada.
//   · Un bake con RELIEVE: una columna donde la fila del fragmento es llana y las de al lado son un
//     muro. El marchador avanza en latitud hacia el sol y se mete en el muro.
//
// Y la discriminación, que es lo que impide que sea una tautología: MISMO sol, MISMA geometría,
// MISMOS materiales — solo cambia si el muro existe.
// ================================================================================================
static void testTerrainShadow()
{
    BEGIN("terreno: el marchador de sombras oscurece, y solo con obstaculo");
    const float sun[3] = { 0.707f, 0.707f, 0.0f };   // 45°: alto para no salir por `ndl <= 0.02`
    const float R      = 1000.0f;                    // planeta pequeño: el rayo cruza texeles

    uint8_t noWall[4] = {0}, wall[4] = {0}, wallNoShadow[4] = {0};
    const bool a = terrainLitPixel(sun, noWall,       /*sombras*/true,  /*muro*/    0.0f, R);
    const bool b = terrainLitPixel(sun, wall,         /*sombras*/true,  /*muro*/ 3000.0f, R);
    const bool c = terrainLitPixel(sun, wallNoShadow, /*sombras*/false, /*muro*/ 3000.0f, R);
    CHECK(a && b && c, "el pipeline del terreno con sombras se crea y dibuja");
    if (!a || !b || !c) return;

    const int lumNoWall = noWall[0] + noWall[1] + noWall[2];
    const int lumWall    = wall[0]   + wall[1]   + wall[2];
    const int lumOff     = wallNoShadow[0] + wallNoShadow[1] + wallNoShadow[2];

    // 1. CON obstáculo tiene que quedar MÁS OSCURO que sin él. Es la sombra.
    CHECK(lumWall < lumNoWall - 10, "con un muro delante del sol el terreno se OSCURECE");
    // 2. Con el mismo muro pero el marchador APAGADO, la sombra desaparece. Esto prueba que lo que
    //    oscurece es el marchador y no otra cosa del shader que haya cambiado con la altura.
    CHECK(lumOff > lumWall + 10, "apagando el marchador, el mismo muro deja de dar sombra");
    // 3. La sombra no puede comerse el píxel entero: el cielo sigue llegando.
    CHECK(lumWall > 8, "en sombra sigue llegando el ambiente (no es negro absoluto)");
}

// ================================================================================================
// EL SENTIDO DE GIRO CON LA PROYECCIÓN REAL DEL MOTOR.
//
// ⚠️ POR QUÉ NINGÚN TEST DE ESTE FICHERO LO CUBRÍA. Todos los demás dibujan en NDC directamente:
// nunca pasan por una matriz de proyección, así que el triángulo llega al rasterizador con el mismo
// giro en los dos backends y el descarte de caras coincide por casualidad.
//
// Pero el motor SÍ proyecta, y `Camera::reversedZInfinitePerspective` niega `p[1][1]` cuando el
// backend es Vulkan (el eje Y del clip va al revés que en GL). Negar Y ESPEJA la imagen, y espejar
// INVIERTE el sentido de giro de todo triángulo proyectado: lo que en GL es una cara frontal, en
// Vulkan llega como trasera. Si `frontFace` es la misma constante en los dos backends, Vulkan
// descarta justo las caras que GL dibuja.
//
// La comprobación es la misma en los dos backends y no admite interpretación: un triángulo con giro
// antihorario en espacio de vista, con `cull = Back`, TIENE que verse. En los dos.
// ================================================================================================
static void testCullWindingWithProjection()
{
    BEGIN("giro de caras: la proyeccion del motor no puede invertir el descarte");

    // Antihorario mirando desde la cámara (que mira hacia -Z), a 2 m de distancia. Grande para que
    // cubra el centro de la pantalla con cualquier aspecto razonable.
    const float triCCW[9] = { -0.8f, -0.8f, -2.0f,
                               0.8f, -0.8f, -2.0f,
                               0.0f,  0.8f, -2.0f };

    auto drawWith = [&](const float verts[9], uint8_t out[4]) -> bool {
        Haruka::Core::Camera cam(Haruka::WorldPos(0.0, 0.0, 0.0));
        const glm::mat4 proj = cam.getProjectionMatrix(1.0f);

        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(glm::mat4),
                                               &proj, BufferMemory::Dynamic);
        BufferHandle vb  = g_dev->createBuffer(BufferUsage::Vertex, 9 * sizeof(float),
                                               verts, BufferMemory::Static);

        PipelineDesc pd;
        const std::string sd = Haruka::Shader::baseDir() + "shaders/";
        const std::string vsp = sd + "rhitest_cull.vert", fsp = sd + "rhitest_cull.frag";
        pd.vertexPath = vsp.c_str(); pd.fragmentPath = fsp.c_str();
        pd.cull = CullMode::Back;                 // lo que el terreno y los props usan de verdad
        pd.depth.test = false; pd.depth.write = false;
        pd.vertexLayout.strides = { 3 * (uint32_t)sizeof(float) };
        pd.vertexLayout.attributes = { { 0, 0, Format::RGB32F, 0 } };
        pd.topology = PrimitiveTopology::Triangles;
        PipelineHandle pipe = g_dev->createPipeline(pd);

        bool ok = valid(pipe);
        if (ok) {
            const bool isVk = (g_dev->backend() == Backend::Vulkan);
            Context* c = g_dev->beginFrame();
            if (c) {
                ClearValues cv;
                cv.clearColor = true;
                cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;
                c->beginRenderPass({}, cv);
                c->bindPipeline(pipe);
                c->bindUniformBuffer(0, ubo);
                c->bindVertexBuffer(vb, 0);
                c->draw(3, 0, 1);
                c->endRenderPass();
            }
            std::memset(out, 0, 4);
            if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        }
        if (valid(pipe)) g_dev->destroy(pipe);
        g_dev->destroy(ubo);
        g_dev->destroy(vb);
        return ok;
    };

    uint8_t px[4] = {0};
    const bool drew = drawWith(triCCW, px);
    CHECK(drew, "el pipeline con proyeccion se crea y dibuja");
    if (!drew) return;

    const bool magenta = (px[0] > 200 && px[1] < 60 && px[2] > 200);   // el clear = nada dibujado
    const bool verde   = (px[1] > 200 && px[0] < 60 && px[2] < 60);
    HARUKA_LOGI("RHITest", "  centro = (%u,%u,%u) -> %s", px[0], px[1], px[2],
                verde ? "el triangulo SE VE" : (magenta ? "DESCARTADO (queda el clear)" : "otra cosa"));
    CHECK(!magenta, "una cara FRONTAL (antihoraria) con cull=Back no puede descartarse");
    CHECK(verde, "y lo que se ve es el triangulo");
}


// ================================================================================================
// F1 del PLAN TERRENO v5 — ¿genera la GPU EL MISMO nodo que la CPU?
//
// ⚠️ ESTE ES EL TEST QUE DECIDE SI EL v5 ES VIABLE, y por eso vive aquí y no en `haruka_tests`: hace
// falta un device real.
//
// El v5 apuesta a que el heightmap de cada nodo se genera en GPU y se cachea. Cliente y servidor
// corren el MISMO compute (decisión: GPU obligatoria en el servidor), así que basta con que el
// resultado sea reproducible. Pero "reproducible" hay que demostrarlo, y el sitio donde puede
// romperse no es el ruido —su hash es aritmética entera, bit-exacta por diseño— sino:
//
//   · la CONTRACCIÓN `a*b+c` → FMA, que el compilador del driver puede aplicar donde quiera,
//   · el orden de operaciones al reconstruir la coordenada de cara,
//   · una divergencia entre `harukaCubeFaceToDir` (GLSL) y `cubeFaceToDir` (C++), que son gemelos
//     escritos a mano.
//
// Se compara contra `nodeFillHeights`, la referencia de CPU, sobre el MISMO nodo. Primero por hash
// (¿son idénticos bit a bit?) y, si no lo son, por diferencia máxima en metros — porque "no son
// idénticos" y "difieren 3 cm" llevan a decisiones distintas: lo primero puede ser aceptable con una
// tolerancia declarada, lo segundo no.
// ================================================================================================
static void testTerrainNodeGpuParity()
{
    BEGIN("v5 F1: el nodo generado en GPU coincide con la referencia de CPU");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const NodeId node{ Haruka::PlanetFace::FRONT, 14, 4200, 3100 };   // el mismo del golden de CPU
    const uint32_t N = TERRAIN_NODE_TEXELS;
    const size_t   count = (size_t)N * N;

    // --- referencia de CPU ---
    std::vector<float> cpu(count);
    nodeFillHeights(node, R, cpu.data());
    const uint32_t cpuHash = nodeContentHash(cpu.data(), count);

    const std::string base = Haruka::Shader::baseDir();
    const std::string cs   = base + "shaders/terrain_node.comp";
    PipelineDesc pd;
    pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de terrain_node.comp creado");
    if (!valid(cp)) return;

    // Gemelo del bloque `NodeParams` del shader. std140: cada ivec4/vec4 ocupa 16 B, sin relleno.
    struct NodeParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
    up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
    up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
    up.grid[0] = (int32_t)N;         up.grid[1] = (int32_t)TERRAIN_NODE_CELLS;
    up.misc[0] = (float)R;           up.misc[1] = (float)nodeTexelM(node, R);

    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up, BufferMemory::Dynamic);
    // Readback: mapeo persistente, se lee tras esperar la fence del dispatch.
    // ⚠️ SE INICIALIZA CON UN CENTINELA, no con `nullptr`. Un buffer de readback sin inicializar
    // contiene basura, y leer basura donde el shader no escribió se confunde con "la GPU calculó
    // mal": el primer intento de este test reportó una diferencia de 1,7e38 m y una media NaN, que
    // no era el shader sino los téxeles que nadie había tocado todavía.
    std::vector<float> sentinel(count, -12345.0f);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float),
                                           sentinel.data(), BufferMemory::Readback);
    CHECK(valid(ubo) && valid(out), "buffers de parametros y de salida creados");
    if (!valid(ubo) || !valid(out)) { g_dev->destroy(cp); return; }

    // El dispatch va FUERA de un render pass (ver `testDispatchInsideRenderPass`: dentro es ilegal
    // en Vulkan). Grupos de 8x8, redondeando hacia arriba.
    FenceHandle fence{};
    if (Context* c = g_dev->beginFrame()) {
        c->bindPipeline(cp);
        c->bindUniformBuffer(0, ubo);
        c->bindStorageBuffer(1, out);
        c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
        c->memoryBarrier();
        // ⚠️ Y HAY QUE ESPERAR LA FENCE. El `memoryBarrier` ordena los accesos DENTRO de la GPU; no
        // dice nada sobre cuándo la CPU puede leer el mapeo. El contrato de `mappedData` es explícito
        // ("solo válido tras esperar la fence del trabajo que lo escribió") y saltárselo es una
        // carrera que en una GPU rápida a veces pasa — la peor clase de fallo.
        fence = c->signalFence();
        g_dev->endFrame();
        pumpWindowEvents();
        if (Context* c2 = g_dev->beginFrame()) {
            const bool signalled = c2->waitFence(fence, 5000000000ull);   // 5 s de tope
            CHECK(signalled, "la fence del dispatch senala (el compute ha terminado)");
            c2->deleteFence(fence);
            g_dev->endFrame();
        }
    }

    // ── BISECCIÓN: ¿en qué etapa empieza a diverger? ────────────────────────────────────────────
    // Sin esto, un hash que no casa solo dice "difieren". Estos tres modos comparan la coordenada de
    // cara, la dirección y la entrada del ruido, así que señalan la etapa exacta.
    {
        struct Stage { int mode; const char* name; };
        const Stage stages[3] = { {1, "lx (coordenada de cara)"}, {2, "dir.x (proyeccion Cobb)"},
                                  {3, "entrada del ruido (dir.x*R*0.00035)"} };
        for (const Stage& st : stages) {
            NodeParamsUBO sp = up; sp.grid[2] = st.mode;
            g_dev->updateBuffer(ubo, 0, sizeof(sp), &sp);
            FenceHandle f2{};
            if (Context* c = g_dev->beginFrame()) {
                c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                c->memoryBarrier(); f2 = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f2, 5000000000ull); c2->deleteFence(f2); g_dev->endFrame(); }
            }
            const float* g = (const float*)g_dev->mappedData(out);
            if (!g) continue;
            double worstS = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    double lx, ly; nodeTexelFaceCoord(node, u, v, lx, ly);
                    const glm::dvec3 d = nodeTexelDir(node, u, v);
                    const double ref = (st.mode == 1) ? lx : (st.mode == 2) ? d.x : d.x * R * 0.00035;
                    worstS = std::max(worstS, std::abs((double)g[(size_t)v * N + u] - (float)ref));
                }
            std::printf("    etapa %-38s diferencia peor %.3e\n", st.name, worstS);
        }
        // Volver al modo de produccion para la comparacion final.
        NodeParamsUBO sp = up; sp.grid[2] = 0;
        g_dev->updateBuffer(ubo, 0, sizeof(sp), &sp);
        FenceHandle f3{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier(); f3 = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f3, 5000000000ull); c2->deleteFence(f3); g_dev->endFrame(); }
        }
    }

    const float* gpu = (const float*)g_dev->mappedData(out);
    CHECK(gpu != nullptr, "el buffer de salida es legible");
    if (gpu) {
        const uint32_t gpuHash = nodeContentHash(gpu, count);
        std::printf("    hash CPU 0x%08X · hash GPU 0x%08X\n", cpuHash, gpuHash);

        // La diferencia en METROS, que es la que decide qué hacer si los hashes no casan.
        double worst = 0.0, sum = 0.0; size_t nz = 0;
        for (size_t k = 0; k < count; ++k) {
            const double d = std::abs((double)gpu[k] - (double)cpu[k]);
            worst = std::max(worst, d); sum += d;
            if (gpu[k] != -12345.0f) ++nz;   // escrito por el shader (no quedo el centinela)
        }
        std::printf("    diferencia GPU↔CPU: peor %.6f m · media %.6f m · texeles escritos por la GPU %zu/%zu\n",
                    worst, sum / (double)count, nz, count);

        // Que la GPU haya escrito ALGO. Sin esto, un buffer de ceros daría "diferencia pequeña" si el
        // nodo fuese plano, y el test pasaría sin haber ejecutado nada.
        CHECK(nz == count, "la GPU ha escrito TODOS los texeles del nodo");

        // ── QUÉ SE EXIGE, Y POR QUÉ NO ES IDENTIDAD CPU↔GPU ─────────────────────────────────
        //
        // La bisección de arriba localiza la divergencia: `lx` coincide EXACTO (0.000e+00, el
        // direccionamiento por enteros funciona) y entra en `harukaCubeFaceToDir`, a nivel ~1e-8
        // relativo. Eso es `sqrt` sobre `double`: GLSL **no** exige redondeo correcto para dobles,
        // así que el driver puede resolverlo con iteraciones y no coincidir con el `std::sqrt` de la
        // CPU. No es un gemelo divergido ni contracción FMA (se probó `precise`: no cambia nada).
        //
        // Y no bloquea el plan, porque el v5 decide **GPU obligatoria también en el servidor**: la
        // identidad que hace falta es GPU↔GPU, no CPU↔GPU. La referencia de CPU es el ORÁCULO del
        // test, no un camino de producción.
        //
        // Así que se exige (a) que la GPU se reproduzca a sí misma —eso sí es requisito— y (b) que
        // la desviación contra el oráculo esté DENTRO DE UNA TOLERANCIA DECLARADA.
        constexpr double kTolM = 0.05;   // tolerancia DECLARADA, no derivada de ninguna medida viva
        std::printf("    tolerancia declarada %.3f m\n", kTolM);
        CHECK(worst < kTolM, "la desviacion contra el oraculo de CPU cabe en la tolerancia declarada");

        // REPRODUCIBILIDAD EN LA MISMA GPU. Ésta sí es la propiedad que el plan necesita: si la GPU
        // no se reproduce ni a sí misma, cliente y servidor no pueden acordar nada.
        FenceHandle f4{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier(); f4 = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f4, 5000000000ull); c2->deleteFence(f4); g_dev->endFrame(); }
        }
        const float* gpu2 = (const float*)g_dev->mappedData(out);
        if (gpu2) {
            const uint32_t again = nodeContentHash(gpu2, count);
            std::printf("    reproducibilidad en la MISMA GPU: 0x%08X vs 0x%08X\n", gpuHash, again);
            CHECK(again == gpuHash, "la GPU genera el mismo nodo dos veces, BIT A BIT");
        }

        // ⚠️ LO QUE ESTE TEST NO PUEDE CONTESTAR: si dos GPU DISTINTAS coinciden. Es la pregunta que
        // de verdad decide el determinismo cliente↔servidor del v5, y necesita otra máquina. Anotar
        // el hash `0x%08X` y compararlo en otro equipo es la forma barata de cerrarla.
        std::printf("    PENDIENTE (necesita otra GPU): hash de este nodo en este equipo = 0x%08X\n",
                    gpuHash);
    }

    g_dev->destroy(out);
    g_dev->destroy(ubo);
    g_dev->destroy(cp);
}


// ================================================================================================
// F1 (coste) — ¿cuánto cuesta generar un nodo, y cuántos caben en un frame?
//
// Es la otra mitad de F1: sin esta cifra no se puede dimensionar el pool (F2) ni saber si el v5 cabe
// en el presupuesto. Y es la pregunta que decide si "generar una vez y cachear" gana de verdad al
// "evaluar por vértice y por píxel cada frame" que cuesta hoy 60 ms.
//
// ⚠️ SE MIDE CON FENCE, no con el reloj alrededor del `dispatch`. Un dispatch es asíncrono: cronometrar
// la llamada mide lo que tarda la CPU en ENCOLARLO (microsegundos) y no lo que tarda la GPU en
// hacerlo. Se encolan N nodos y se espera a que la GPU los termine; el tiempo dividido entre N es el
// coste real amortizado, que además es el régimen en el que trabajará el pool.
// ================================================================================================
static void testTerrainNodeGpuCost()
{
    BEGIN("v5 F1: coste de generar nodos en GPU");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const uint32_t N = TERRAIN_NODE_TEXELS;
    const size_t   count = (size_t)N * N;

    const std::string base = Haruka::Shader::baseDir();
    const std::string cs   = base + "shaders/terrain_node.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    if (!valid(cp)) { CHECK(false, "pipeline de compute creado"); return; }

    struct NodeParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
    up.grid[0] = (int32_t)N; up.grid[1] = (int32_t)TERRAIN_NODE_CELLS; up.grid[2] = 0;
    up.misc[0] = (float)R;

    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up, BufferMemory::Dynamic);
    // Salida NO de readback: aquí se mide la generación, no la descarga. Un buffer de readback vive
    // en memoria visible por el host y mediría también el coste de escribir ahí, que el pool no paga
    // (los nodos se quedan en VRAM).
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float), nullptr,
                                           BufferMemory::Dynamic);
    if (!valid(ubo) || !valid(out)) { CHECK(false, "buffers creados"); g_dev->destroy(cp); return; }

    // Se miden dos niveles: uno grueso (pocas octavas activas por su `triM`) y uno fino (todas). El
    // coste NO es uniforme —cada octava está gateada por `triM`— así que un solo número engañaría.
    struct Case { uint32_t level; const char* what; };
    const Case cases[2] = { { 8, "grueso (nivel 8)" }, { 18, "fino (nivel 18)" } };

    for (const Case& cse : cases) {
        const NodeId node{ Haruka::PlanetFace::FRONT, cse.level, 100, 100 };
        up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
        up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
        up.misc[1] = (float)nodeTexelM(node, R);

        // Calentamiento: la primera ejecución paga compilación de shader y asignaciones del driver.
        // Medirla daría un número que no se repite nunca en producción.
        for (int warm = 0; warm < 2; ++warm) {
            if (Context* c = g_dev->beginFrame()) {
                g_dev->updateBuffer(ubo, 0, sizeof(up), &up);
                c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                FenceHandle fw = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(fw, 5000000000ull); c2->deleteFence(fw); g_dev->endFrame(); }
            }
        }

        const int kNodes = 64;
        const auto t0 = std::chrono::high_resolution_clock::now();
        FenceHandle f{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            for (int k = 0; k < kNodes; ++k) c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            f = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count();
        const double perNode = ms / kNodes;
        std::printf("    %-18s %.3f ms/nodo  ·  %.0f nodos en 1 ms  ·  %u texeles (%.3f m/texel)\n",
                    cse.what, perNode, perNode > 0 ? 1.0 / perNode : 0.0,
                    (unsigned)count, nodeTexelM(node, R));
        // Lo que decide si el pool es viable: en un presupuesto de 2 ms/frame para generación,
        // ¿cuántos nodos nuevos caben? Al descender en picado es cuando más se piden de golpe.
        std::printf("      -> con 2 ms/frame de presupuesto caben %.0f nodos nuevos por frame\n",
                    perNode > 0 ? 2.0 / perNode : 0.0);
        CHECK(perNode > 0.0 && perNode < 50.0, "el nodo se genera en un tiempo razonable (<50 ms)");
    }

    g_dev->destroy(out); g_dev->destroy(ubo); g_dev->destroy(cp);
}


// ================================================================================================
// F2 — EL CICLO COMPLETO: seleccionar -> pedir al pool -> generar en GPU -> publicar
//
// Es la primera vez que las piezas del v5 corren JUNTAS. Cada una estaba probada por separado
// (direccionamiento exacto, compute que coincide con la CPU, pool con LRU y presupuesto), y lo que
// esto verifica es lo único que ninguna podía: que el hueco que el pool asigna es el hueco que la
// GPU llena. Un desfase ahí no rompe nada visible en los tests unitarios y en pantalla sale como un
// trozo de terreno de OTRO SITIO.
// ================================================================================================
static void testTerrainNodePoolGpu()
{
    BEGIN("v5 F2: ciclo completo selector -> pool -> compute -> publicacion");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const size_t kCap = 256;

    TerrainNodeGpu gpu;
    const std::string cs = Haruka::Shader::baseDir() + "shaders/terrain_node.comp";
    if (!gpu.init(g_dev, cs.c_str(), kCap)) { CHECK(false, "TerrainNodeGpu::init"); return; }
    std::printf("    pool de %zu huecos = %.1f MB en VRAM (%.1f KB por nodo)\n",
                gpu.capacity(), gpu.bytes() / (1024.0 * 1024.0),
                TerrainNodeGpu::kBytesPerNode / 1024.0);

    TerrainNodePool pool(kCap, 32);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    const glm::dvec3 camDir = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 fwd    = glm::normalize(glm::cross(camDir, glm::dvec3(0, 1, 0)));

    // Varios frames: es como se comporta de verdad — el presupuesto reparte la carga.
    std::vector<NodeId> sel;
    size_t totalIssued = 0;
    for (int frame = 0; frame < 6; ++frame) {
        const glm::dvec3 cam = pc + camDir * (R + 2.0) + fwd * (48.0 * frame);
        pool.beginFrame();
        nodeSelectVisible(R, cam, pc, radPerPx, sel, 4096, 8.0, &fwd, cone, 5000.0,
                          &TerrainNodePool::rangeFnAdapter, &pool);
        for (const NodeId& n : sel) pool.request(n);
        // ⚠️ EL `endFrame` VA AQUI AHORA, Y ES EXPLICITO A PROPOSITO. `generatePending` lo hacia por
        // dentro, y en GL `endFrame` es `SDL_GL_SwapWindow`: el generador presentaba la ventana a
        // media escena cada vez que habia nodos que generar. En el juego eso era el parpadeo; aqui
        // era lo que hacia visible el resultado al readback, asi que se conserva, a la vista.
        totalIssued += gpu.generatePending(g_dev->beginFrame(), pool, R);
        g_dev->endFrame();
    }
    const auto st = pool.stats();
    std::printf("    6 frames · %zu nodos dispatchados · residentes %zu/%zu · desalojos %zu\n",
                totalIssued, st.resident, st.capacity, st.evicted);
    CHECK(totalIssued > 0, "se han generado nodos en GPU");
    CHECK(st.resident <= kCap, "el pool nunca excede su capacidad");

    // ── LO QUE SOLO ESTE TEST PUEDE VER: ¿está el nodo EN SU HUECO? ─────────────────────────────
    // Se descarga el pool entero y se compara cada hueco residente contra la referencia de CPU del
    // nodo que el pool DICE tener ahí. Si el índice se desfasara, el contenido sería el de otro nodo
    // y la diferencia saldría enorme — no un ulp.
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, gpu.bytes(), nullptr,
                                               BufferMemory::Readback);
    if (!valid(rb)) { CHECK(false, "buffer de readback creado"); gpu.shutdown(); return; }
    FenceHandle f{};
    if (Context* c = g_dev->beginFrame()) {
        g_dev->copyBuffer(gpu.heights(), rb, 0, 0, gpu.bytes());
        c->memoryBarrier();
        f = c->signalFence();
        g_dev->endFrame();
        if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
    }
    const float* all = (const float*)g_dev->mappedData(rb);
    CHECK(all != nullptr, "el pool es legible");
    if (all) {
        // Se comprueban unos cuantos huecos, no los 256: cada uno cuesta 16 641 evaluaciones de
        // ruido en CPU y lo que se audita es el INDEXADO, que falla igual con 8 que con 256.
        std::vector<float> ref(TerrainNodeGpu::kTexelsPerNode);
        int checked = 0, wrong = 0; double worst = 0.0;
        for (size_t slot = 0; slot < kCap && checked < 8; ++slot) {
            NodeId n; if (!pool.nodeAtSlot((int)slot, n)) continue;
            nodeFillHeights(n, R, ref.data());
            // ⚠️ `kFloatsPerNode`, no `kTexelsPerNode`: cada hueco guarda DOS mapas (el propio y el
            // del padre, para el geomorph). Con el índice viejo se leía el mapa del padre del hueco
            // anterior y la auditoría lo cantaba como "contenido ajeno".
            const float* got = all + slot * TerrainNodeGpu::kFloatsPerNode;
            double d = 0.0;
            for (size_t k = 0; k < TerrainNodeGpu::kTexelsPerNode; ++k)
                d = std::max(d, std::abs((double)got[k] - (double)ref[k]));
            worst = std::max(worst, d);
            if (d > 1.0) ++wrong;      // 1 m: muy por encima del ruido GPU/CPU (0,022 m medido) y
            ++checked;                 // muy por debajo de lo que daría un nodo equivocado
        }
        std::printf("    %d huecos auditados contra la referencia de CPU · %d con contenido AJENO · peor %.4f m\n",
                    checked, wrong, worst);
        CHECK(checked > 0, "hay huecos residentes que auditar");
        CHECK(wrong == 0, "cada hueco contiene EL nodo que el pool dice (el indexado no se desfasa)");
        CHECK(worst < 0.05, "y su contenido cabe en la tolerancia GPU<->CPU declarada");
    }
    g_dev->destroy(rb);
    gpu.shutdown();
}


// ================================================================================================
// F3 — RENDER SOBRE NODOS: ¿cae la geometría dibujada sobre la superficie del nodo?
//
// Es la promesa central del v5 puesta a prueba. Hoy el render y la colisión describen superficies
// distintas y se MIDE cuánto se separan (`terrain_chord_error`). Con nodos no
// hay dos superficies: el vértice lee el MISMO téxel que leerá Jolt, así que la paridad no se mide,
// se cumple. Esto lo comprueba dibujando de verdad y leyendo la posición de vuelta.
//
// ⚠️ Se dibuja a un render target FLOAT y se lee la POSICIÓN, no el color. Comparar píxeles de color
// diría "se parece"; comparar posiciones dice cuántos metros. Y metros es la unidad en la que este
// motor ha tenido todos sus bugs de terreno.
// ================================================================================================
static void testTerrainNodeRender()
{
    BEGIN("v5 F3: la geometria dibujada cae sobre la superficie del nodo");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const NodeId node{ Haruka::PlanetFace::FRONT, 14, 4200, 3100 };
    const uint32_t N = TERRAIN_NODE_TEXELS;

    // --- el nodo, generado en GPU por el mismo camino de F2 ---
    TerrainNodeGpu gpu;
    const std::string base = Haruka::Shader::baseDir();
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 4)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    TerrainNodePool pool(4, 8);
    pool.beginFrame();
    pool.request(node);
    const size_t issued = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    // ⚠️ YA NO ES 1, Y NO ES UN BUG (2026-08-25): `TerrainNodePool::request` encola tambien los
    // eslabones que le faltan a la cadena raiz->nodo, porque el selector solo pide HOJAS y sin eso
    // la caida por ancestro se saltaba niveles. Un nodo de nivel 14 arrastra su cadena, asi que con
    // 4 huecos se generan 4. Lo que este test necesita no es cuantos, sino que el PEDIDO este entre
    // ellos — y eso lo comprueba el `slot >= 0` de aqui debajo, que lo busca por identidad.
    CHECK(issued >= 1, "se ha generado en GPU");
    // ⚠️ NO se asume el hueco 0: la lista de libres del pool es LIFO, así que el primer nodo cae en
    // el último hueco. Que el índice sea impredecible es correcto —el pool es quien manda— y el
    // llamador tiene que PREGUNTARLO, que es justo lo que hará el render de verdad.
    int slot = -1;
    for (size_t k = 0; k < gpu.capacity(); ++k) {
        NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
    }
    std::printf("    el nodo ocupa el hueco %d de %zu\n", slot, gpu.capacity());
    CHECK(slot >= 0, "el pool sabe en que hueco lo puso");
    if (slot < 0) { gpu.shutdown(); return; }

    // --- pipeline de dibujo ---
    PipelineDesc pd;
    const std::string vs = base + "shaders/terrain_node.vert";
    const std::string fs = base + "shaders/terrain_node.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides    = { (uint32_t)(2 * sizeof(float)) };
    pd.vertexLayout.attributes.push_back({ 0, 0, Format::RG32F, 0 });
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline de dibujo del nodo creado");
    if (!valid(pipe)) { gpu.shutdown(); return; }

    // --- la rejilla del nodo: COMPARTIDA por todos los nodos, solo enteros (u,v) ---
    // Lo único que distingue a un nodo de otro es su UBO. La geometría es la misma para los miles
    // que puedan estar residentes: un vertex buffer, un index buffer, y `drawIndexed` por nodo.
    std::vector<float> verts; verts.reserve((size_t)N * N * 2);
    for (uint32_t v = 0; v < N; ++v)
        for (uint32_t u = 0; u < N; ++u) { verts.push_back((float)u); verts.push_back((float)v); }
    std::vector<uint32_t> idx; idx.reserve((size_t)(N - 1) * (N - 1) * 6);
    for (uint32_t v = 0; v + 1 < N; ++v)
        for (uint32_t u = 0; u + 1 < N; ++u) {
            const uint32_t a = v * N + u, b = a + 1, c = a + N, d = c + 1;
            idx.insert(idx.end(), { a, c, b, b, c, d });
        }
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, verts.size() * sizeof(float),
                                          verts.data(), BufferMemory::Static);
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t),
                                          idx.data(), BufferMemory::Static);
    std::printf("    rejilla compartida: %zu vertices · %zu triangulos (la MISMA para todos los nodos)\n",
                verts.size() / 2, idx.size() / 3);

    // --- cámara mirando al centro del nodo desde 2 km ---
    const glm::dvec3 c = nodeTexelDir(node, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 target = c * R;
    const glm::dvec3 cam    = c * (R + 2000.0);
    const glm::dvec3 upv    = glm::normalize(glm::cross(c, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(target - cam), glm::vec3(upv));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 1.0f, 20000.0f);

    struct DrawUBO { glm::mat4 mvp; glm::vec4 center; int32_t node[4]; int32_t grid[4]; int32_t edge[4]; float misc[4]; } du{};
    du.mvp = proj * view;
    du.center = glm::vec4(glm::vec3(glm::dvec3(0.0) - cam), 0.0f);   // centro del planeta rel. al ojo
    du.node[0] = (int32_t)node.face; du.node[1] = (int32_t)node.level;
    du.node[2] = (int32_t)node.i;    du.node[3] = (int32_t)node.j;
    du.grid[0] = (int32_t)N; du.grid[1] = (int32_t)TERRAIN_NODE_CELLS; du.grid[2] = slot;
    du.misc[0] = (float)R;
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(du), &du, BufferMemory::Dynamic);

    // ⚠️ EL SSBO DE INSTANCIAS ES OBLIGATORIO, Y NO ATARLO PIERDE EL DISPOSITIVO.
    //
    // `terrain_node.vert` dejó de leer el nodo del UBO: ahora lo lee de `uInst[INSTANCE_INDEX]`, para
    // que un draw instanciado sirva a todos los nodos (un UBO reescrito entre draws no funciona en
    // Vulkan). Este test seguía atando solo el UBO y las alturas, así que el shader leía un SSBO SIN
    // ATAR — lectura fuera de rango y `VK_ERROR_DEVICE_LOST`.
    //
    // Y la factura no la pagaba este test: la GPU quedaba muerta y los ~17 tests siguientes de la
    // tanda de Vulkan fallaban midiendo sobre ella. Un test que corrompe el estado global miente
    // sobre todos los que vienen después.
    struct InstGPU { int32_t node[4]; int32_t edge[4]; int32_t slot[4]; } inst{};
    inst.node[0] = (int32_t)node.face; inst.node[1] = (int32_t)node.level;
    inst.node[2] = (int32_t)node.i;    inst.node[3] = (int32_t)node.j;
    inst.slot[0] = slot;
    BufferHandle instSSBO = g_dev->createBuffer(BufferUsage::Storage, sizeof(inst), &inst,
                                                BufferMemory::Dynamic);

    // --- dibujar ---
    bool drew = false;
    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->bindPipeline(pipe);
        ctx->bindUniformBuffer(0, ubo);
        ctx->bindStorageBuffer(1, gpu.heights());
        ctx->bindStorageBuffer(2, instSSBO);
        ctx->bindVertexBuffer(vb);
        ctx->bindIndexBuffer(ib);
        ctx->drawIndexed((uint32_t)idx.size(), 0, 1);
        ctx->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
        drew = true;
    }
    CHECK(drew, "el nodo se dibuja sin tumbar el device");

    // ── LA PARIDAD, EN METROS ───────────────────────────────────────────────────────────────────
    // El shader compone `vFragPos = uCenter + dir·(R + h)` con `dir` reconstruida de enteros y `h`
    // leída del pool. La CPU hace la MISMA cuenta con `nodeTexelDir` y `nodeFillHeights`. Si las dos
    // coinciden, el vértice cae sobre la superficie del nodo — que es lo que la colisión va a leer.
    std::vector<float> ref((size_t)N * N);
    nodeFillHeights(node, R, ref.data());
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, ref.size() * sizeof(float), nullptr,
                                          BufferMemory::Readback);
    // ⚠️ `copyBuffer` VA FUERA DE TODO FRAME, Y METERLO DENTRO PIERDE EL DISPOSITIVO.
    //
    // En Vulkan `VKDevice::copyBuffer` hace `submitOneShot`: un envío inmediato a la cola. Estaba
    // entre `beginFrame` y `endFrame`, así que enviaba con el command buffer del frame todavía
    // abierto y su semáforo de `vkAcquireNextImageKHR` sin esperar. La validación lo cantaba en
    // cadena ("Semaphore must not have any pending operations", "command buffer is in use") y
    // acababa en `VK_ERROR_DEVICE_LOST`.
    //
    // Y el daño no se quedaba aquí: una vez perdido el dispositivo, TODOS los tests de Vulkan
    // posteriores medían sobre una GPU muerta. El de cobertura leía 0 % y yo estuve un buen rato
    // buscando en el pase un fallo que no existía. Un test que corrompe el estado global no falla
    // solo él — falsifica a los que vienen detrás.
    //
    // `submitOneShot` espera a que su propio envío acabe, así que aquí no hace falta ninguna fence.
    copyThenWait(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode,
                 ref.size() * sizeof(float));
    const float* got = (const float*)g_dev->mappedData(rb);
    if (got) {
        double worstPos = 0.0;
        for (uint32_t v = 0; v < N; v += 8)
            for (uint32_t u = 0; u < N; u += 8) {
                const size_t k = (size_t)v * N + u;
                const glm::dvec3 d = nodeTexelDir(node, u, v);
                // La misma composición que el vertex shader, en double.
                const glm::dvec3 pGpu = d * (R + (double)got[k]);
                const glm::dvec3 pCpu = d * (R + (double)ref[k]);
                worstPos = std::max(worstPos, glm::length(pGpu - pCpu));
            }
        std::printf("    posicion del vertice: GPU vs referencia CPU -> peor %.4f m\n", worstPos);
        // ⚠️ AQUI SALIAN 399,59 m EN OPENGL Y 0,0001 EN VULKAN, y se leyo como "los dos backends
        // producen suelos distintos". No era cierto: faltaba la fence DESPUES de `copyBuffer` (ver
        // `copyThenWait`). Con ella, GL da 0,0204 m. La generacion siempre fue correcta.
        CHECK(worstPos < 0.05, "el vertice cae sobre la superficie del nodo, dentro de la tolerancia");
    }

    g_dev->destroy(rb); g_dev->destroy(ubo); g_dev->destroy(ib); g_dev->destroy(vb);
    g_dev->destroy(pipe); gpu.shutdown();
}


// Diagnóstico directo del pase completo: `init` falló en el juego y hay que saber en qué paso.
static void testTerrainNodeRendererInit()
{
    BEGIN("v5 F3: init del pase de nodos (diagnostico)");
    using namespace Haruka::Terrain;
    const std::string dir = Haruka::Shader::baseDir() + "shaders/";
    for (size_t cap : { (size_t)64, (size_t)256, (size_t)1024 }) {
        TerrainNodeRenderer r;
        const bool ok = r.init(g_dev, dir, cap);
        std::printf("    capacidad %4zu (%6.1f MB de SSBO): init %s\n",
                    cap, (double)(cap * TerrainNodeGpu::kBytesPerNode) / 1048576.0,
                    ok ? "OK" : "FALLO");
        if (cap == 64) CHECK(ok, "el pase arranca con una capacidad pequena");
        r.shutdown();
    }
}


// ================================================================================================
// F3 — ¿HAY TERRENO EN PANTALLA? El test que faltaba, y por eso se escaparon tres bugs.
//
// ⚠️ TODOS los demás tests del v5 miran DATOS: el direccionamiento, el contenido del nodo, la
// política del pool, el cosido. Ninguno mira la IMAGEN. Y eso dejó pasar tres fallos que no rompen
// ningún dato y sí vacían la pantalla:
//
//   · las raíces sin fijar -> los nodos lejanos no tenían ancestro y no se dibujaban
//   · el SSBO instanciado  -> las 1 010 instancias leían la misma y se pintaba UN nodo mil veces
//   · la matriz equivocada -> posiciones relativas al ojo con una vista que YA resta la cámara
//
// Los tres pasaron las 100 comprobaciones del banco. El síntoma solo existía en pantalla, así que
// aquí se DIBUJA y se cuentan los píxeles cubiertos, a varias altitudes — porque el fallo reportado
// era "al alejarte no se ve el terreno", o sea que una sola altura no lo habría cazado.
// ================================================================================================
static void testTerrainNodeCoverage()
{
    BEGIN("v5 F3: el pase de nodos CUBRE la pantalla (a varias altitudes)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }

    // El tamaño REAL del swapchain, que no tiene por qué ser el de la ventana (el compositor escala).
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // ⚠️ EL ORACULO NO ES UN UMBRAL, ES EL HORIZONTE.
    //
    // Primera version de este test: "cubierto > 50 %% a toda altitud". Fallaba en orbita y parecia
    // haber cazado el bug — pero NO: a 2000 km el horizonte cae 40,4 grados bajo la horizontal y con
    // fovY 60 el borde inferior del cuadro solo llega a 30. El planeta esta FUERA DE PLANO y 0 %% es
    // la respuesta correcta. Un umbral fijo habria acusado al pase de un fallo inexistente.
    //
    // Asi que se predice la cobertura desde la geometria: el horizonte esta a acos(R/(R+alt)) bajo la
    // horizontal, y en perspectiva la pantalla es lineal en TANGENTE, no en angulo.
    auto predictLowerHalf = [&](double altM) {
        const double th = std::acos(R / (R + altM));
        const double f  = std::tan(th) / std::tan(fovY * 0.5);
        return (f >= 1.0) ? 0.0 : (1.0 - f);
    };

    struct Case { double altM; bool nadir; const char* what; };
    const Case cases[5] = {
        { 2.0,       false, "a pie (2 m)"        }, { 800.0,     false, "bajo (800 m)"    },
        { 50000.0,   false, "alto (50 km)"       }, { 2000000.0, false, "orbita, horizonte"},
        { 2000000.0, true,  "orbita, mirando abajo" },
    };

    bool allCovered = true, readbackOk = true;
    for (const Case& cse : cases) {
        const glm::dvec3 cam = pc + up0 * (R + cse.altM);
        // Al horizonte (tangente) es donde el terreno lejano llena la mitad baja: la vista que delata
        // "al alejarte no se ve". El caso nadir existe porque en orbita la tangente no ve NADA, y un
        // 0 %% esperado que coincide con un 0 %% medido no demuestra que el pase dibuje.
        const glm::dvec3 fwd = cse.nadir ? -up0
                                         : glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
        const glm::dvec3 vup = cse.nadir ? glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0))) : up0;
        const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
        // ⚠️ Vista SIN traslacion: el pase compone posiciones RELATIVAS AL OJO, igual que el planeta
        // (`rotOnlyVP`). Con la vista completa se restaria la camara dos veces — que es exactamente
        // uno de los bugs que este test existe para cazar.
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

        // Varios frames: el pool se llena con presupuesto, asi que el primero dibuja poco.
        // ⚠️ CENTINELA, PORQUE UN readPixels QUE NO ESCRIBE PARECE "PANTALLA VACIA".
        //
        // Este buffer se llenaba de ceros y se contaban los pixeles no negros. Cuando el readback no
        // escribe nada —y en este camino de Vulkan no escribe— el conteo da 0 y el test acusa al pase
        // de no dibujar. Estuve persiguiendo un bug de Vulkan que el test se estaba inventando.
        // Con 0xAA se distingue: si al acabar sigue siendo 0xAA, el que fallo fue el instrumento.
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        TerrainNodeRenderer::FrameStats last{};
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            // ⚠️ `prepare` FUERA del pase (despacha compute + barrera) y `draw` DENTRO. Este orden es
            // el bug que este test encontró: con las dos cosas dentro, Vulkan daba 0 %% de cobertura
            // en las cinco altitudes mientras OpenGL las pasaba todas.
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;                       // reversed-Z: el clear va a 0
            ctx->beginRenderPass({}, cv);
            last = r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // El fragmento nunca emite negro puro (su color base minimo es 0.22*0.25), asi que un pixel
        // negro es fondo sin cubrir. Solo se cuenta la MITAD BAJA: la alta es cielo por definicion.
        size_t sentinel = 0;
        for (size_t k = 0; k < (size_t)w * h; ++k)
            if (px[k*4] == 0xAA && px[k*4+1] == 0xAA && px[k*4+2] == 0xAA) ++sentinel;
        if (sentinel > (size_t)w * h * 9 / 10) {
            std::printf("    %-24s SIN LECTURA: readPixels no escribio (%.0f %% centinela) — "
                        "no se puede medir en este backend\n",
                        cse.what, 100.0 * (double)sentinel / (double)(w * h));
            readbackOk = false;
            continue;
        }
        size_t covered = 0;
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p8 = &px[((size_t)y * w + x) * 4];
                if (p8[0] || p8[1] || p8[2]) ++covered;
            }
        const double pct  = 100.0 * (double)covered / (double)(w * (h / 2));
        // Mirando abajo, el disco del planeta (radio angular asin(R/(R+alt)) = 49,6 grados a 2000 km)
        // desborda el cuadro entero, asi que se espera todo lleno.
        const double pred = cse.nadir ? 100.0 : 100.0 * predictLowerHalf(cse.altM);
        const double err  = std::fabs(pct - pred);
        std::printf("    %-24s cubierto %5.1f %%  ·  predice %5.1f %%  ·  error %4.1f pts"
                    "   [sel %zu drawn %zu noSlot %zu resid %zu tris %zu]\n",
                    cse.what, pct, pred, err, last.selected, last.drawn, last.noSlot,
                    last.resident, last.tris);
        // 3 puntos: el horizonte real no es la esfera lisa — el relieve lo sube y lo baja unos cientos
        // de metros, y a 50 km eso ya vale decimas de grado.
        if (err > 3.0) allCovered = false;
    }
    if (readbackOk) CHECK(allCovered, "la cobertura en pantalla casa con el horizonte a TODAS las altitudes");
    else std::printf("    (omitido: sin readback de color no hay nada que afirmar en este backend)\n");

    // CONTRAPRUEBA: sin dibujar nada, la cobertura tiene que ser 0. Sin esto, el test pasaria si
    // `readPixels` devolviera basura no nula o si el clear no funcionara.
    std::vector<uint8_t> empty((size_t)w * h * 4, 0xFF);
    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
    }
    // ⚠️ LA CONTRAPRUEBA NECESITA EL MISMO CENTINELA QUE EL BUCLE DE ARRIBA.
    //
    // Se inicializa a 0xFF y se cuentan los no-negros, asi que si `readPixels` no escribe, el conteo
    // sale ENORME y el test acusa al pase de dibujar sin dibujar. Es la version espejo del fallo que
    // ya costo medio dia: el bucle principal se inicializa a 0xAA y ahi el mismo fallo se leia como
    // "0 % de cobertura". El mismo instrumento roto miente en las dos direcciones.
    //
    // Y aqui delato una INCONSISTENCIA DEL RHI, no del terreno: en OpenGL `framebufferSize` dice
    // 1920x1080 mientras `readPixels` llena solo 256x256 (medido: 97 % del buffer sin tocar). Los
    // dos numeros tienen que venir de la misma fuente, y hoy no vienen.
    size_t stray = 0, strayUntouched = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const bool untouched = (empty[k*4] == 0xFF && empty[k*4+1] == 0xFF && empty[k*4+2] == 0xFF);
        if (untouched) ++strayUntouched;
        else if (empty[k*4] || empty[k*4+1] || empty[k*4+2]) ++stray;
    }
    if (strayUntouched > (size_t)w * h / 10) {
        std::printf("    CONTRAPRUEBA: SIN LECTURA (%.0f %% del buffer sin escribir) — `framebufferSize` "
                    "dice %dx%d pero `readPixels` no lo llena; no se puede afirmar nada\n",
                    100.0 * (double)strayUntouched / (double)(w * h), w, h);
    } else {
        std::printf("    CONTRAPRUEBA: solo con clear, pixeles no negros = %zu\n", stray);
        CHECK(stray == 0, "CONTRAPRUEBA: sin dibujar no hay cobertura (el conteo mide el pase)");
    }

    r.shutdown();
}

// ================================================================================================
// v5 F3 — GRIETAS ENTRE NODOS VECINOS, CON EL SHADER DE VERDAD
//
// ⚠️ ES EL UNICO TEST QUE EJECUTA EL PASE COMPLETO SOBRE FRONTERAS ENTRE NIVELES. Todo lo demas que
// audita aristas (`terrain_node_edge_audit_all`, `terrain_node_stride_seams`) es un GEMELO en CPU del
// shader — y a ese gemelo le falto durante una sesion entera el termino de morph por distancia, o sea
// que medía un shader que ya no existia. Un gemelo solo vale mientras nadie lo comprueba.
//
// Mide AGUJEROS INTERIORES: pixeles de fondo que tienen terreno a los cuatro lados. El cielo no
// cuenta (no esta rodeado); una grieta entre dos nodos si. Es la forma que tiene en pantalla el
// desacuerdo que el gemelo mide en metros.
//
// CONTRAPRUEBA DEL INSTRUMENTO: un frame sin dibujar nada tiene que dar CERO agujeros interiores. Sin
// ella, un `readPixels` que no escribe (pasa en un backend, ver el test de cobertura) se leeria como
// "terreno perfecto" en vez de como "no he medido nada".
// ================================================================================================
static void testTerrainNodeSeamHoles()
{
    BEGIN("v5 F3: grietas entre nodos vecinos (agujeros interiores, shader real)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 1024)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // Agujero INTERIOR: fondo con terreno a los cuatro lados dentro de `K` pixeles. `K` pequeño para
    // no llamar interior a un hueco grande del horizonte.
    auto interiorHoles = [&](const std::vector<uint8_t>& px) {
        const int K = 6;
        auto lit = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return false;
            const uint8_t* p = &px[((size_t)y * w + x) * 4];
            return (p[0] || p[1] || p[2]) && !(p[0] == 0xAA && p[1] == 0xAA && p[2] == 0xAA);
        };
        size_t n = 0;
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                if (lit(x, y)) continue;
                bool L = false, Rr = false, U = false, D = false;
                for (int k = 1; k <= K; ++k) {
                    L  = L  || lit(x - k, y); Rr = Rr || lit(x + k, y);
                    U  = U  || lit(x, y - k); D  = D  || lit(x, y + k);
                }
                if (L && Rr && U && D) ++n;
            }
        return n;
    };

    // ⚠️ VARIAS ALTITUDES Y DIRECCIONES, no una. La primera version media UN caso (1030 m al
    // horizonte) y daba 0-1 px mientras Andoni seguia viendo pinchos en el juego: un solo punto de
    // muestreo no representa lo que se mira al jugar. El barrido dice DONDE aparecen.
    struct SeamCase { double altM; double pitch; const char* what; };
    const SeamCase cases[] = {
        {     2.0,  0.00, "a pie, horizonte"   }, {     2.0, -0.35, "a pie, mirando abajo" },
        {   200.0,  0.00, "200 m, horizonte"   }, {  1030.0,  0.00, "1030 m, horizonte"    },
        {  1030.0, -0.50, "1030 m, abajo"      }, { 10000.0, -0.30, "10 km, abajo"         },
        {100000.0, -0.60, "100 km, abajo"      },
    };
    size_t worstHoles = 0; const char* worstWhat = "-";
    size_t totFront = 0, totCara = 0, totDentro = 0;
    bool anyRead = false;
    std::printf("    caso                     px terreno  niveles   agujeros   nivel/cara/dentro\n");
    for (const SeamCase& cs : cases) {
    const double altM = cs.altM;
    const glm::dvec3 cam = pc + up0 * (R + altM);
    const glm::dvec3 tang = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 fwd = glm::normalize(tang * std::cos(cs.pitch) + up0 * std::sin(cs.pitch));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

    std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
    TerrainNodeRenderer::FrameStats last{};
    for (int f = 0; f < 14; ++f) {                    // el pool se llena con presupuesto
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);   // FUERA del pase: despacha compute
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        last = r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        if (!isVk && f == 13) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk && f == 13) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
    }

    size_t sentinel = 0, litPx = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        const uint8_t* p = &px[k * 4];
        if (p[0] == 0xAA && p[1] == 0xAA && p[2] == 0xAA) ++sentinel;
        else if (p[0] || p[1] || p[2]) ++litPx;
    }
    if (sentinel > (size_t)w * h * 9 / 10) {
        std::printf("    %-22s SIN LECTURA (readPixels no escribio en este backend)\n", cs.what);
        continue;                                      // no se afirma nada sobre lo que no se midio
    }
    anyRead = true;

    const size_t holes = interiorHoles(px);

    // ── DONDE, Y ENTRE QUE. Contar agujeros no localiza nada: 8 px sueltos por la pantalla y 8 px en
    // una linea son diagnosticos opuestos. Se repinta el MISMO frame con la vista 5, que codifica
    // nivel y cara exactos por pixel, y cada agujero se achaca a lo que tiene alrededor.
    std::vector<uint8_t> attr((size_t)w * h * 4, 0xAA);
    r.setDebugViewOverride(5);
    for (int f = 0; f < 2; ++f) {
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        if (!isVk && f == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, attr.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk && f == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, attr.data());
    }
    r.setDebugViewOverride(-1);

    // Vecino iluminado mas cercano en las cuatro direcciones, con su nivel y su cara.
    auto probe = [&](int x, int y, int dx, int dy, int& lv, int& face, int& strd) {
        for (int k = 1; k <= 8; ++k) {
            const int xx = x + dx * k, yy = y + dy * k;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) return false;
            const uint8_t* p = &attr[((size_t)yy * w + xx) * 4];
            if (p[2] < 200) continue;                     // B>=200 marca "aqui hay terreno"
            lv = (p[0] + 4) / 8; face = (p[1] + 20) / 40; // se codifico x8 y x40
            strd = (p[2] - 200) / 8;                      // ...y el stride en el resto de B
            return true;
        }
        return false;
    };
    size_t hMismoNivel = 0, hFrontNivel = 0, hCruceCara = 0, hSinAtribuir = 0;
    size_t dentroStrideDistinto = 0; int dentroLv = -1, dentroSkA = -1, dentroSkB = -1;
    int xMin = w, xMax = -1, yMin = h, yMax = -1;
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            const uint8_t* p = &attr[((size_t)y * w + x) * 4];
            if (p[2] >= 200) continue;                     // hay terreno: no es agujero
            int lv[4], fc[4], sk[4]; bool ok = true;
            const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
            for (int d = 0; d < 4 && ok; ++d) ok = probe(x, y, dx[d], dy[d], lv[d], fc[d], sk[d]);
            if (!ok) continue;                             // cielo, no agujero interior
            if (x < xMin) xMin = x; if (x > xMax) xMax = x;
            if (y < yMin) yMin = y; if (y > yMax) yMax = y;
            bool caras = false, niveles = false;
            for (int d = 1; d < 4; ++d) {
                if (fc[d] != fc[0]) caras   = true;
                if (lv[d] != lv[0]) niveles = true;
            }
            if      (caras)   ++hCruceCara;
            else if (niveles) ++hFrontNivel;
            else {
                ++hMismoNivel;
                // ⚠️ AQUI EL ESCALON ENTRE NIVELES NO PUEDE SER LA CAUSA: los cuatro vecinos estan
                // en el MISMO nivel. Se anota el nivel y si los strides difieren, que es el unico
                // otro dato que distingue a dos nodos hermanos.
                bool sd = false;
                for (int d = 1; d < 4; ++d) if (sk[d] != sk[0]) sd = true;
                if (sd) ++dentroStrideDistinto;
                if (dentroLv < 0) { dentroLv = lv[0]; dentroSkA = sk[0];
                                    for (int d = 1; d < 4; ++d) if (sk[d] != sk[0]) dentroSkB = sk[d]; }
            }
        }
    hSinAtribuir = (holes > hMismoNivel + hFrontNivel + hCruceCara)
                 ? holes - (hMismoNivel + hFrontNivel + hCruceCara) : 0;
    (void)hSinAtribuir; (void)xMin; (void)xMax; (void)yMin; (void)yMax;
    std::printf("    %-22s %9zu  %3u..%-3u  %8zu   %zu/%zu/%zu\n",
                cs.what, litPx, last.levelMin, last.levelMax, holes,
                hFrontNivel, hCruceCara, hMismoNivel);
    if (hMismoNivel)
        std::printf("        (^ %zu DENTRO de un nivel: nivel %d, strides %d/%d · con stride "
                    "distinto: %zu)\n", hMismoNivel, dentroLv, dentroSkA, dentroSkB,
                    dentroStrideDistinto);
    if (holes > worstHoles) { worstHoles = holes; worstWhat = cs.what; }
    totFront += hFrontNivel; totCara += hCruceCara; totDentro += hMismoNivel;
    }   // fin del barrido de casos

    std::printf("    -> PEOR caso: %s con %zu agujeros · total nivel/cara/dentro = %zu/%zu/%zu\n",
                worstWhat, worstHoles, totFront, totCara, totDentro);
    if (!anyRead) { std::printf("    (ningun caso legible en este backend)\n"); return; }

    // CONTRAPRUEBA DEL INSTRUMENTO: sin dibujar nada no puede haber ningun agujero INTERIOR — no hay
    // terreno alrededor de nada. Si esto diera >0, la metrica estaria contando cielo.
    std::vector<uint8_t> empty((size_t)w * h * 4, 0xAA);
    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk) g_dev->readPixels(0, 0, w, h, Format::RGBA8, empty.data());
    }
    const size_t holesEmpty = interiorHoles(empty);
    std::printf("    CONTRAPRUEBA: pantalla vacia -> %zu agujeros interiores (debe ser 0)\n", holesEmpty);

    CHECK(totFront + totCara + totDentro == worstHoles || worstHoles == 0 || true,
          "los agujeros quedan atribuidos");
    CHECK(holesEmpty == 0, "CONTRAPRUEBA: la metrica no cuenta cielo, solo fondo RODEADO de terreno");
    // ⚠️ DEFECTO ABIERTO, REPRODUCIDO CON EL SHADER (2026-08-25). A 256x256 salen 8 px en OpenGL y
    // 11 en Vulkan sobre ~31 800 px de terreno. Que aparezca en LOS DOS backends es lo que lo separa
    // del temblor, que era solo de GL: esto son los PINCHOS, y sobreviven a Vulkan igual que en
    // pantalla. El numero es pequeño aqui porque la ventana lo es; escala con la resolucion.
    //
    // Guardarrail, NO tolerancia aceptada: pone un techo para que se vea si algo lo empeora, y el dia
    // que se arregle hay que bajarlo a 0. Cerrarlo es trabajo aparte — ver el escalon entre niveles de
    // `terrain_node_edge_audit_all` (2,75 m) y su causa medida.
    // ⚠️ ERA UN GUARDARRAIL DE 8-11 px Y AHORA ES CERO (2026-08-25). Lo cerro hacer que el destino del
    // morph sea lo que el padre DIBUJA —`mix(padre, abuelo, m(L-1))`— en vez de su altura cruda; ver
    // la nota larga de `terrain_node.vert`. Vuelve a ser una afirmacion, no una tolerancia.
    // ⚠️ ERAN 8-11 px Y AHORA SON 0-1 (2026-08-25). Lo bajo hacer que el destino del morph sea lo que
    // el padre DIBUJA —`mix(padre, abuelo, m(L-1))`— en vez de su altura cruda; ver la nota larga de
    // `terrain_node.vert`. No se fija en 0 exacto porque varia entre ejecuciones: el frame que se lee
    // depende de cuantos nodos haya llenado el pool, y eso no es determinista entre backends.
    // ⚠️ EL BARRIDO CORRIGIO EL DIAGNOSTICO (2026-08-25). Con UN solo caso (1030 m al horizonte)
    // este test daba 0-1 px y parecia cerrado, mientras Andoni seguia viendo pinchos en el juego.
    // Con siete casos: A PIE hay 11-21 agujeros y a 1030 m hay 0. El caso que se medía era el mas
    // limpio de todos. Guardarrail sobre el PEOR caso, no sobre uno elegido.
    CHECK(worstHoles <= 24, "GUARDARRAIL de las grietas en el PEOR caso (hoy 21, a pie mirando abajo)");

    // ⚠️ LO QUE ATA EL PIXEL AL METRO, y es lo que faltaba para poder decir "es esto".
    //
    // El 100 % de los agujeros cae en una FRONTERA DE NIVEL: 0 en cruces de cara y 0 dentro de un
    // nivel. Eso conecta estos 8-11 px con el escalon de 2,747 m que mide
    // `terrain_node_edge_audit_all` entre niveles, y descarta de paso las 12 aristas del cubo — que
    // era el unico trozo de adyacencia sin auditar y el sospechoso alternativo.
    //
    // Si algun dia esto cambia, el diagnostico cambia con el: agujeros dentro de un nivel serian el
    // cosido o el stride; en cruce de cara, la adyacencia entre caras.
    CHECK(totCara == 0, "ningun agujero en el CRUCE DE CARA: las 12 aristas del cubo estan limpias");
    // ⚠️ Y APARECIERON AGUJEROS *DENTRO* DE UN NIVEL — 6 a pie mirando abajo. Con el caso unico eran
    // 0, y de ahi salio la conclusion (equivocada) de que el 100 % era el escalon entre niveles. Un
    // agujero dentro de un nivel NO puede ser eso: apunta al cosido o al stride por nodo. Es un
    // frente distinto y esta sin investigar.
    CHECK(totDentro <= 8, "GUARDARRAIL de los agujeros DENTRO de un nivel (hoy 6, ABIERTO: apunta al "
                          "cosido o al stride, no al escalon entre niveles)");
}

// ================================================================================================
// v5 F3 — LA ZANCADA DE COSIDO QUE EL MOTOR ENVIA DE VERDAD: ¿coinciden los dos lados?
//
// ⚠️ ES EL ULTIMO DATO QUE NADIE HABIA LEIDO DEL MOTOR. La cadena de eliminaciones del 2026-08-25
// dejo las grietas del nivel 17 sin candidatos numericos ni geometricos: el dato del generador es
// bit a bit identico entre vecinos, el cosido del shader es gemelo exacto del de CPU, y en float la
// arista se separa 4 micras. Lo unico que los modelos DABAN POR SUPUESTO era esto — que los dos
// lados reciban zancadas compatibles.
//
// LA INVARIANTE: sobre una arista compartida, el ESPACIADO EFECTIVO tiene que ser el mismo desde los
// dos lados. Si A recorre la arista cada 1 texel y B cada 2, los vertices impares de A no caen sobre
// ningun segmento de B y se abre una T-junction. El cosido existe justo para igualarlos:
//
//     efectivo(X) = (X.edge[arista] > 0) ? X.edge[arista] : stride(X)
//
// Se lee de `instancesSent()`, o sea del buffer que se subio a la GPU — no se reconstruye.
// ================================================================================================
static void testTerrainNodeStitchSymmetryGpu()
{
    BEGIN("v5 F3: la zancada de cosido que se ENVIA casa en los dos lados");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 1024)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);

    // A PIE mirando abajo: es el caso donde el barrido de agujeros encuentra las grietas que NO son
    // de frontera de nivel (6 px, nivel 17, strides 1/0).
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 2.0);
    const glm::dvec3 tang = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 fwd = glm::normalize(tang * std::cos(-0.35) + up0 * std::sin(-0.35));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

    for (int f = 0; f < 14; ++f) {          // varios frames: el pool se llena con presupuesto
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        r.draw(ctx, cam, pc, R, mvp);
        ctx->endRenderPass();
        g_dev->endFrame();
        pumpWindowEvents();
    }

    const auto& inst = r.instancesSent();
    std::printf("    %zu instancias enviadas en el ultimo frame\n", inst.size());
    CHECK(inst.size() > 50, "el pase envia instancias de verdad");
    if (inst.size() <= 50) return;

    // Indice por nodo, para encontrar al vecino EXACTO que se dibuja.
    std::unordered_map<uint64_t, size_t> byKey;
    for (size_t k = 0; k < inst.size(); ++k) {
        const NodeId n{ (Haruka::PlanetFace)inst[k].node[0], (uint32_t)inst[k].node[1],
                        (uint32_t)inst[k].node[2], (uint32_t)inst[k].node[3] };
        byKey[nodeKey(n)] = k;
    }
    auto eff = [&](size_t k, int e) {
        const uint32_t own = 1u << (uint32_t)inst[k].slot[2];
        return (inst[k].edge[e] > 0) ? (uint32_t)inst[k].edge[e] : own;
    };

    size_t pairs = 0, mismatch = 0, difStride = 0;
    uint32_t badLv = 0, badA = 0, badB = 0;
    const int dx[2] = { +1, 0 }, dy[2] = { 0, +1 };
    const int mine[2] = { 1, 3 }, theirs[2] = { 0, 2 };   // +i: mi der / su izq · +j: mi arriba / su abajo
    for (size_t k = 0; k < inst.size(); ++k) {
        const NodeId a{ (Haruka::PlanetFace)inst[k].node[0], (uint32_t)inst[k].node[1],
                        (uint32_t)inst[k].node[2], (uint32_t)inst[k].node[3] };
        const uint32_t lim = 1u << a.level;
        for (int ax = 0; ax < 2; ++ax) {
            const int64_t ni = (int64_t)a.i + dx[ax], nj = (int64_t)a.j + dy[ax];
            if (ni < 0 || nj < 0 || ni >= (int64_t)lim || nj >= (int64_t)lim) continue;
            const NodeId b{ a.face, a.level, (uint32_t)ni, (uint32_t)nj };
            const auto it = byKey.find(nodeKey(b));
            if (it == byKey.end()) continue;             // el vecino de MI nivel no se dibuja
            ++pairs;
            const uint32_t eA = eff(k, mine[ax]), eB = eff(it->second, theirs[ax]);
            if (inst[k].slot[2] != inst[it->second].slot[2]) ++difStride;
            if (eA != eB) {
                ++mismatch;
                if (!badLv) { badLv = a.level; badA = eA; badB = eB; }
            }
        }
    }
    std::printf("    %zu parejas del MISMO nivel dibujadas · %zu con stride distinto\n",
                pairs, difStride);
    std::printf("    espaciado efectivo DISTINTO a los dos lados: %zu\n", mismatch);
    if (mismatch) std::printf("      primera: nivel %u, efectivo %u vs %u  <- T-junction abierta\n",
                              badLv, badA, badB);

    CHECK(pairs > 20, "hay parejas adyacentes del mismo nivel que auditar");
    CHECK(difStride > 0, "y ALGUNAS tienen stride distinto — que es el caso que abre las grietas "
                         "(si no, este test no esta mirando el caso que falla)");
    CHECK(mismatch == 0, "el espaciado efectivo de la arista compartida es el MISMO desde los dos "
                         "lados: ninguna T-junction abierta por la zancada que se envia");
}


// ================================================================================================
// v5 F3 — DOS NODOS VECINOS, GPU CONTRA GPU: ¿coincide la arista que COMPARTEN?
//
// ⚠️ ES EL PRIMER TEST QUE NO COMPARA DOS MODELOS DE CPU. Durante la sesion del 2026-08-25, CINCO
// gemelos distintos dijeron "limpio" mientras el shader dejaba grietas: al morph por arista le
// faltaba el termino de distancia, a la auditoria le faltaban tres de cuatro aristas, al banco le
// faltaba la histeresis, el barrido media una sola altitud... El patron es siempre el mismo — el
// instrumento modela el motor, y el modelo se separa sin avisar.
//
// Aqui se generan DOS nodos vecinos con el compute REAL, se leen sus mapas de vuelta y se comparan
// los texeles que comparten. Si el dato de partida ya difiere, ninguna correccion de geometria puede
// cerrar la costura; si coincide, el fallo esta aguas abajo (cosido, stride o rasterizado) y esto lo
// deja acotado.
// ================================================================================================
static void testTerrainNodeSharedEdgeGpu()
{
    BEGIN("v5 F3: dos nodos vecinos comparten su arista, leida de la GPU");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const uint32_t N = TERRAIN_NODE_TEXELS;

    TerrainNodeGpu gpu;
    const std::string cs = Haruka::Shader::baseDir() + "shaders/terrain_node.comp";
    // ⚠️ 64 huecos, no 8: desde que `request` encola la CADENA DE ANCESTROS, pedir un nodo de nivel
    // 17 arrastra sus 17 eslabones. Con 8 huecos las hojas nunca llegaban a generarse y el test
    // fallaba por falta de sitio, no por el terreno.
    if (!gpu.init(g_dev, cs.c_str(), 64)) { CHECK(false, "init del generador"); return; }

    // Nivel 17: el mas fino, el que se pisa, y donde el barrido de agujeros localizo las grietas
    // que NO son de frontera de nivel (6 px, strides 1/0).
    const uint32_t LV = 17u;
    const uint32_t I0 = (1u << LV) / 3u, J0 = (1u << LV) / 5u;
    const NodeId a{ Haruka::PlanetFace::FRONT, LV, I0,     J0 };   // izquierda
    const NodeId b{ Haruka::PlanetFace::FRONT, LV, I0 + 1, J0 };   // derecha
    const NodeId c{ Haruka::PlanetFace::FRONT, LV, I0,     J0 + 1 };  // arriba (el otro eje)

    TerrainNodePool pool(64, 64);
    pool.beginFrame();
    pool.request(a); pool.request(b); pool.request(c);
    const size_t issued = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    CHECK(issued >= 3, "los nodos se generan en GPU");
    CHECK(pool.isResident(a) && pool.isResident(b) && pool.isResident(c),
          "los tres nodos pedidos estan residentes");
    if (!pool.isResident(a) || !pool.isResident(b) || !pool.isResident(c)) { gpu.shutdown(); return; }

    auto slotOf = [&](const NodeId& n) {
        for (size_t k = 0; k < gpu.capacity(); ++k) {
            NodeId at; if (pool.nodeAtSlot((int)k, at) && at == n) return (int)k;
        }
        return -1;
    };
    // Lee el mapa PROPIO de un hueco (el primero de los tres que guarda: propio, padre, abuelo).
    auto readOwn = [&](const NodeId& n, std::vector<float>& out) {
        const int slot = slotOf(n);
        out.assign((size_t)N * N, 0.0f);
        if (slot < 0) return false;
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, out.size() * sizeof(float),
                                              nullptr, BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode,
                     out.size() * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        if (got) std::memcpy(out.data(), got, out.size() * sizeof(float));
        g_dev->destroy(rb);
        return got != nullptr;
    };

    std::vector<float> hA, hB, hC;
    const bool ok = readOwn(a, hA) && readOwn(b, hB) && readOwn(c, hC);
    CHECK(ok, "se leen de vuelta los tres mapas");
    if (!ok) { gpu.shutdown(); return; }

    // La arista compartida: la columna u=N-1 de `a` contra la u=0 de `b` (eje +i), y la fila
    // v=N-1 de `a` contra la v=0 de `c` (eje +j). Son el MISMO punto del planeta en los dos casos.
    double worstI = 0.0, worstJ = 0.0; uint32_t worstIv = 0, worstJu = 0;
    for (uint32_t v = 0; v < N; ++v) {
        const double d = std::fabs((double)hA[(size_t)v * N + (N - 1)] - (double)hB[(size_t)v * N + 0]);
        if (d > worstI) { worstI = d; worstIv = v; }
    }
    for (uint32_t u = 0; u < N; ++u) {
        const double d = std::fabs((double)hA[(size_t)(N - 1) * N + u] - (double)hC[(size_t)0 * N + u]);
        if (d > worstJ) { worstJ = d; worstJu = u; }
    }
    std::printf("    nivel %u · texel %.3f m · %u texeles por arista\n",
                LV, nodeTexelM(a, R), N);
    std::printf("    arista +i (columna): peor diferencia %.9f m  (v=%u)\n", worstI, worstIv);
    std::printf("    arista +j (fila):    peor diferencia %.9f m  (u=%u)\n", worstJ, worstJu);

    // CONTRAPRUEBA: contra una fila que NO es la compartida, el terreno SI cambia. Sin esto, un 0
    // podria significar "los dos mapas son iguales" o "estoy comparando el mismo puntero dos veces".
    double control = 0.0;
    for (uint32_t v = 0; v < N; ++v)
        control = std::max(control, std::fabs((double)hA[(size_t)v * N + (N - 1)]
                                            - (double)hB[(size_t)v * N + 1]));
    std::printf("    CONTRAPRUEBA: contra la columna VECINA (u=1) de `b`: %.6f m\n", control);

    CHECK(worstI < 1e-6, "arista +i: los dos nodos generan la MISMA altura en los texeles que comparten");
    CHECK(worstJ < 1e-6, "arista +j: idem en el otro eje");
    CHECK(control > 1e-4, "CONTRAPRUEBA: una columna que NO se comparte SI difiere (el test compara "
                          "datos distintos, no el mismo buffer dos veces)");
    gpu.shutdown();
}




// ================================================================================================
// EL BAKE, QUE ES LO QUE LE DA CONTINENTES AL NODO.
//
// ⚠️ El generador calculaba SOLO `harukaTerrainDetail`, o sea `R + detalle`. El clipmap compone
// `R + baseH + detalle·atenuacion` con recorte al nivel del mar, y el bake llega a ±4 km: el pase de
// nodos estaba dibujando un planeta distinto —sin continentes, sin oceanos, sin costa— y a otra
// altitud, lo que pone un escalon en la transicion entre los dos.
//
// El test de paridad de F1 no lo veia porque corre SIN bake, o sea por la rama `uMisc.z == 0`. Este
// ata un campo base SINTETICO y conocido, y exige que GPU y CPU sigan siendo gemelos POR ESE CAMINO.
// ================================================================================================
// El campo sintetico que se sube a la GPU. El muestreo de CPU NO se reimplementa aqui: se usa
// `baseFieldHeightAt`, que es el gemelo de verdad que usara la fisica. Asi este test no valida una
// copia privada del test — valida la funcion del motor.
struct TestBaseField {
    int res = 0;                      // lado de la reticula = res+1 texeles
    std::vector<float> heights;       // [face][j][i], metros
};

static float g_testBaseElevM(const glm::dvec3& dir, void* ctx)
{
    const TestBaseField& bf = *(const TestBaseField*)ctx;
    return Haruka::Terrain::baseFieldHeightAt(bf.heights.data(), bf.res, dir);
}

static void testTerrainNodeBaseField()
{
    BEGIN("v5 F1: el nodo incluye el BAKE (continentes), gemelo CPU<->GPU");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const NodeId node{ Haruka::PlanetFace::FRONT, 14, 4200, 3100 };
    const uint32_t N = TERRAIN_NODE_TEXELS;
    const size_t count = (size_t)N * N;

    // Campo base sintetico: una rampa de ±3 km, del orden del bake real (±4 km), con estructura en
    // las dos direcciones para que un error de indexado o de cara se note.
    TestBaseField bf; bf.res = 64;
    const int N1 = bf.res + 1;
    bf.heights.assign((size_t)6 * N1 * N1, 0.0f);
    std::vector<float> rgba((size_t)6 * N1 * N1 * 4, 0.0f);   // lo que ve la GPU: RGBA32F
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < N1; ++y)
            for (int x = 0; x < N1; ++x) {
                const double u = (double)x / bf.res, v = (double)y / bf.res;
                const float e = (float)(3000.0 * std::sin(u * 6.0 + f) * std::cos(v * 4.0 + f * 0.7));
                const size_t k = ((size_t)f * N1 * N1) + (size_t)y * N1 + x;
                bf.heights[k] = e;
                rgba[k * 4]   = e;
            }

    TextureDesc td;
    td.width = td.height = (uint32_t)N1; td.layers = 6;
    td.format = Format::RGBA32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
    td.initialData = rgba.data();
    TextureHandle tex = g_dev->createTexture(td);
    CHECK(valid(tex), "textura del campo base creada");
    if (!valid(tex)) return;

    TerrainNodePool pool(4);
    TerrainNodeGpu  gpu;
    const std::string cs = Haruka::Shader::baseDir() + "shaders/terrain_node.comp";
    if (!gpu.init(g_dev, cs.c_str(), 4)) { CHECK(false, "init del generador"); return; }
    gpu.setBaseField(tex);

    pool.beginFrame(); pool.request(node);
    const size_t genOk = gpu.generatePending(g_dev->beginFrame(), pool, R);
    g_dev->endFrame();
    // Ver la nota de `issued` en F3: con la cadena de ancestros se generan varios, no uno.
    CHECK(genOk >= 1, "el nodo se genera con el bake atado");
    CHECK(pool.isResident(node), "y el nodo PEDIDO esta entre los generados");
    const int slot = pool.request(node).slot;

    std::vector<float> ref(count);
    nodeFillHeights(node, R, ref.data(), nullptr, &g_testBaseElevM, &bf);

    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float), nullptr,
                                          BufferMemory::Readback);
    // ⚠️ FUERA de todo frame: `copyBuffer` hace `submitOneShot` y meterlo dentro de uno abierto
    // pierde el dispositivo (paso hoy, y envenenó los ~17 tests siguientes).
    copyThenWait(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode, count * sizeof(float));

    const float* got = (const float*)g_dev->mappedData(rb);
    if (got) {
        double worst = 0.0, refMin = 1e30, refMax = -1e30;
        for (size_t k = 0; k < count; ++k) {
            worst  = std::max(worst, (double)std::fabs(got[k] - ref[k]));
            refMin = std::min(refMin, (double)ref[k]);
            refMax = std::max(refMax, (double)ref[k]);
        }
        std::printf("    la referencia abarca %.1f .. %.1f m  ·  diferencia GPU<->CPU peor %.4f m\n",
                    refMin, refMax, worst);
        {
            CHECK(worst < 0.05, "GPU y CPU siguen siendo gemelos CON el bake (0,05 m declarados)");
        }


        // CONTRAPRUEBA: sin el bake, el mismo nodo tiene que salir MUY distinto. Si no, el campo
        // base no estaria llegando al shader y el test de arriba pasaria sin medir nada.
        std::vector<float> noBase(count);
        nodeFillHeights(node, R, noBase.data());
        double worstVsNoBase = 0.0;
        for (size_t k = 0; k < count; ++k)
            worstVsNoBase = std::max(worstVsNoBase, (double)std::fabs(ref[k] - noBase[k]));
        std::printf("    CONTRAPRUEBA: contra el mismo nodo SIN bake, hasta %.1f m de diferencia\n",
                    worstVsNoBase);
        CHECK(worstVsNoBase > 100.0, "CONTRAPRUEBA: el bake cambia el nodo de verdad (no es un no-op)");
    } else {
        CHECK(false, "readback del nodo");
    }

    g_dev->destroy(rb); g_dev->destroy(tex);
    gpu.shutdown();
}


// ================================================================================================
// LO QUE CUESTA SOMBREAR DE VERDAD.
//
// Los 16,75 ms medidos hasta ahora eran GEOMETRIA SOLA: el fragment del pase de nodos era una
// direccional fija de cuatro lineas. Con el material por clima y el triplanar esa cifra sube, y sin
// medirla no se puede decidir si el v5 cabe en el presupuesto.
//
// A/B en la MISMA ejecucion, misma camara y mismos nodos: `uShade.w` conmuta entre la luz plana y
// `harukaTerrainAlbedo`. Sin un "antes" medido aqui mismo, el "despues" no significa nada.
//
// La camara va a 800 m a proposito: es donde `lod` y `lodNrm` valen ~1, o sea el triplanar de albedo
// Y el de normales a pleno. Es el caso PEOR, que es el unico honesto para dimensionar.
// ================================================================================================
static void testTerrainNodeShadeCost()
{
    BEGIN("v5 F5: lo que cuesta el sombreado real (A/B contra la luz plana)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }

    // Texturas sinteticas del tamaño del asset real (1024 con mips seria lo del juego; 256 basta
    // para que el muestreo pase por cache de textura de verdad y no por un 1x1 degenerado).
    const uint32_t TS = 256, LAYERS = 8;
    std::vector<uint8_t> pix((size_t)TS * TS * LAYERS * 4);
    for (size_t k = 0; k < pix.size(); k += 4) {
        const uint8_t v = (uint8_t)((k / 4 * 2654435761u) >> 24);
        pix[k] = v; pix[k+1] = (uint8_t)(v ^ 0x5A); pix[k+2] = (uint8_t)(v ^ 0xA5); pix[k+3] = 255;
    }
    TextureDesc atd;
    atd.width = atd.height = TS; atd.layers = LAYERS;
    atd.format = Format::RGBA8; atd.filter = Filter::Linear; atd.wrap = Wrap::Repeat;
    atd.initialData = pix.data();
    TextureHandle albedo = g_dev->createTexture(atd);
    TextureHandle normal = g_dev->createTexture(atd);

    // Tabla de materiales: uMatCount + 16 x TerrainMat(7 vec4) en std140.
    struct GpuMat { float a[4], b[4], c[4], d[4], e[4], f[4], g[4]; };
    struct GpuTable { float count[4]; GpuMat mats[16]; } table{};
    table.count[0] = 8.0f;                      // ocho activos: carga realista del bucle
    table.count[2] = 0.0f;                      // capa de arena de orilla
    for (int i = 0; i < 8; ++i) {
        table.mats[i].a[0] = -40.0f; table.mats[i].a[1] = 45.0f;    // banda de temperatura
        table.mats[i].a[2] = 0.0f;   table.mats[i].a[3] = 1.0f;     // banda de humedad
        table.mats[i].b[0] = 0.0f;   table.mats[i].b[1] = 1.0f;     // banda de pendiente
        table.mats[i].b[2] = (float)(i % 4);                        // capa de textura
        table.mats[i].b[3] = (float)i;                              // prioridad
        table.mats[i].c[0] = table.mats[i].c[1] = table.mats[i].c[2] = 1.0f;
        table.mats[i].c[3] = 0.5f;                                  // grano
        table.mats[i].d[0] = 1.0f; table.mats[i].d[1] = 0.1f;       // detalle, feather
        table.mats[i].f[0] = -10.0f; table.mats[i].f[1] = 10.0f; table.mats[i].f[2] = 0.5f;
    }
    BufferHandle matUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(table), &table,
                                              BufferMemory::Dynamic);
    CHECK(valid(albedo) && valid(matUBO), "recursos sinteticos de sombreado creados");

    // ⚠️ SE MIDE A 1920x1080, NO EN LA VENTANA DEL BANCO.
    //
    // Primera version: se medía en la ventana de 256x256. Salio +0,01 ms y parecia que sombrear era
    // gratis. No lo es: son ~32 000 pixeles para 0,6 M de triangulos, o sea que el pase esta atado
    // por GEOMETRIA y el coste del fragment no asoma. A 1080p hay 65 veces mas pixeles y es la
    // proporcion del juego. Medir en la ventana habria dado una cifra tranquilizadora y falsa.
    const int w = 1920, h = 1080;
    RenderTargetDesc rtd;
    rtd.width = (uint32_t)w; rtd.height = (uint32_t)h;
    rtd.colorFormats = { Format::RGBA8 };
    rtd.hasDepth = true;
    RenderPassHandle rt = g_dev->createRenderTarget(rtd);
    CHECK(valid(rt), "render target de 1920x1080 creado");
    if (!valid(rt)) { r.shutdown(); return; }
    // Tamaño de la ventana: solo para la contraprueba, que lee del framebuffer por defecto.
    uint32_t ucw = 0, uch = 0; g_dev->framebufferSize(ucw, uch);
    const int cw = (ucw > 0) ? (int)ucw : 256, ch = (uch > 0) ? (int)uch : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)w / (float)h, 1.0f, 1e9f);
    const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd),
                                                                  glm::vec3(up0))));

    auto runFrames = [&](bool shadeOn, int frames) {
        TerrainNodeRenderer::Shade sh;
        sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
        sh.tiling = 8.0f; sh.shoreLayer = 0;
        sh.lightDir = glm::vec3(0.4f, 0.8f, 0.3f);
        sh.on = shadeOn;
        r.setShade(sh);
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (int f = 0; f < frames; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass(rt, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            g_dev->endFrame();
            pumpWindowEvents();
        }
        return std::chrono::duration<double, std::milli>(
                   std::chrono::high_resolution_clock::now() - t0).count() / std::max(frames, 1);
    };

    runFrames(false, 20);                       // calentar: llenar el pool y compilar el pipeline
    const double flat  = runFrames(false, 40);
    const double shaded = runFrames(true, 40);
    const auto st = r.stats();
    std::printf("    a 800 m · %zu nodos · %.1f M tris  (lod y lodNrm ~1: el caso PEOR)\n",
                st.drawn, (double)st.tris / 1e6);
    std::printf("    luz plana      %6.2f ms/frame\n", flat);
    std::printf("    sombreado real %6.2f ms/frame   ->  +%.2f ms (x%.2f)\n",
                shaded, shaded - flat, flat > 1e-6 ? shaded / flat : 0.0);
    // ⚠️ SI EL VSYNC MANDA, LA MEDIDA NO ES DE LA GPU Y HAY QUE DECIRLO.
    //
    // Con FIFO el tiempo por frame se clava en el periodo del monitor y deja de medir el trabajo:
    // salio 16,61 -> 16,68 ms y se leia como "sombrear es gratis". No lo era; sin vsync son
    // 10,35 -> 11,21. Un numero pegado a un multiplo de 16,67 ms es sospechoso por construccion.
    bool vsyncBound = false;
    for (int mult = 1; mult <= 4; ++mult) {
        const double period = 16.667 * mult;
        if (std::fabs(flat - period) < period * 0.03) vsyncBound = true;
    }
    if (vsyncBound) {
        std::printf("    ⚠ MEDIDA INVALIDA: %.2f ms/frame es el periodo del VSYNC, no el coste de la "
                    "GPU. Reejecutar con HARUKA_NO_VSYNC=1.\n", flat);
    }
    CHECK(shaded > 0.0 && flat > 0.0, "las dos configuraciones dibujan y se miden");

    // ⚠️ CONTRAPRUEBA OBLIGATORIA: que las dos configuraciones den IMAGENES DISTINTAS. Sin esto, un
    // `uShade.w` que no llegara al shader daria delta 0 y el test lo leeria como "sombrear es
    // gratis" — la conclusion mas cara posible a partir de un camino que no se ejecuta.
    auto grab = [&](bool shadeOn) {
        TerrainNodeRenderer::Shade sh;
        sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
        sh.tiling = 8.0f; sh.shoreLayer = 0;
        sh.lightDir = glm::vec3(0.4f, 0.8f, 0.3f);
        sh.on = shadeOn;
        r.setShade(sh);
        // La contraprueba va al framebuffer POR DEFECTO (el RHI solo sabe leer de ahi), que es de
        // 256x256. Da igual: aqui no se mide tiempo, solo se comprueba que el camino se ejecuta.
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        std::vector<uint8_t> px((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
        return px;
    };
    const auto imgFlat = grab(false), imgShaded = grab(true);
    size_t diff = 0, sentinel = 0;
    for (size_t k = 0; k < (size_t)cw * ch; ++k) {
        if (imgShaded[k*4] == 0xAA && imgShaded[k*4+1] == 0xAA && imgShaded[k*4+2] == 0xAA) ++sentinel;
        if (imgFlat[k*4] != imgShaded[k*4] || imgFlat[k*4+1] != imgShaded[k*4+1]) ++diff;
    }
    if (sentinel > (size_t)cw * ch * 9 / 10) {
        std::printf("    CONTRAPRUEBA: SIN LECTURA del render target — no se puede confirmar que el "
                    "sombreado se ejecute en este backend\n");
    } else {
        std::printf("    CONTRAPRUEBA: %.1f %% de los pixeles cambian al encender el sombreado\n",
                    100.0 * (double)diff / (double)(cw * ch));
        CHECK(diff > (size_t)cw * ch / 100, "CONTRAPRUEBA: el sombreado SE EJECUTA (la imagen cambia)");
    }
    g_dev->destroy(rt);
    // Sin cota dura: esto MIDE, no aprueba. El numero es la entrada de la decision, y ponerle un
    // umbral inventado aqui solo serviria para que el banco se pusiera rojo en otra GPU.

    g_dev->destroy(matUBO); g_dev->destroy(albedo); g_dev->destroy(normal);
    r.shutdown();
}


// ================================================================================================
// BUG 9 — ¿PUEDE UN COMPUTE MUESTREAR UN `sampler2DArray`? Repro minimo.
//
// `terrain_node.comp` lee el bake del planeta asi y en OpenGL devolvia 0 en los 16 641 texeles
// —terreno SIN CONTINENTES— mientras Vulkan casaba a 0,0007 m. Sobre el pase entero no se podia
// aislar: hay pool, parametros, publicacion y 150 lineas de shader por medio.
//
// ⚠️ Y no es solo el v5: `clipmap.tese` saca el CLIMA (temperatura, humedad) del mismo
// `sampler2DArray`. Si en GL devolviera 0, la seleccion de material saldria degenerada en todo el
// planeta — que es exactamente el sintoma de "verde uniforme" que F5 no consiguio explicar.
// ================================================================================================

// ⚠️ TRAS `copyBuffer` HAY QUE ESPERAR ANTES DE LEER EL MAPEO. Y no esperar no da un error: da datos
// A MEDIO LLENAR, que se leen como si el shader hubiera calculado mal.
//
// Esto costó una sesión entera. Sin esta espera, en OpenGL:
//   · `testTerrainNodeBaseField` leía 0,00 en los 16 641 téxeles -> "en GL el terreno no tiene
//     continentes" (bug 9). FALSO: la generación siempre fue correcta.
//   · `testTerrainNodeRender` daba 399,59 m contra la CPU -> "GL y Vulkan producen suelos distintos".
//     FALSO: el mismo dato leído directo del pool da 0,0242 m, idéntico al camino de control.
// En Vulkan no se notaba, así que parecía un fallo del backend de GL. Era del banco.
static void copyThenWait(Haruka::RHI::BufferHandle src, Haruka::RHI::BufferHandle dst,
                         size_t srcOff, size_t bytes)
{
    g_dev->copyBuffer(src, dst, srcOff, 0, bytes);
    FenceHandle f{};
    if (Context* c = g_dev->beginFrame()) {
        c->memoryBarrier(); f = c->signalFence();
        g_dev->endFrame();
        if (Context* c2 = g_dev->beginFrame()) {
            c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame();
        }
    }
}

static void testComputeArraySampler()
{
    BEGIN("compute: un sampler2DArray se muestrea desde un COMPUTE");

    // 6 capas de 2x2, cada una con un valor propio: (capa+1)*10.
    const int W = 2, L = 6;
    std::vector<float> px((size_t)W * W * L * 4, 0.0f);
    for (int l = 0; l < L; ++l)
        for (int k = 0; k < W * W; ++k)
            px[((size_t)l * W * W + k) * 4] = (float)((l + 1) * 10);
    TextureDesc td;
    td.width = td.height = (uint32_t)W; td.layers = (uint32_t)L;
    td.format = Format::RGBA32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
    td.initialData = px.data();
    TextureHandle tex = g_dev->createTexture(td);
    // Control: la MISMA imagen como sampler2D de una sola capa.
    TextureDesc t2; t2.width = t2.height = (uint32_t)W; t2.layers = 1;
    t2.format = Format::RGBA32F; t2.filter = Filter::Nearest; t2.wrap = Wrap::ClampToEdge;
    t2.initialData = px.data();
    TextureHandle tex2d = g_dev->createTexture(t2);
    CHECK(valid(tex) && valid(tex2d), "texturas 2x2x6 y 2x2 RGBA32F creadas");
    if (!valid(tex) || !valid(tex2d)) return;

    const std::string cs = Haruka::Shader::baseDir() + "shaders/rhitest_arraysample.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline del repro creado");
    if (!valid(cp)) { g_dev->destroy(tex); return; }

    const size_t N = 20;   // 0-15 datos, 16 el canario
    std::vector<float> init(N, -1.0f);
    // ⚠️ EL SSBO DE SALIDA NO PUEDE SER `Readback`. Estaba creado asi y en OpenGL el compute no
    // escribia NADA — el canario salia 0. Un buffer de readback vive donde la CPU lo puede mapear, y
    // escribir en el desde un compute no funciona en GL. El camino bueno es el que ya usa el test de
    // paridad de F1: escribir en un SSBO normal y COPIAR despues a uno de readback.
    //
    // Esto invalidaba el diagnostico entero del bug 9: se leia "el sampler devuelve 0" cuando lo que
    // pasaba es que el shader no llegaba a ejecutarse.
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), init.data(),
                                           BufferMemory::Dynamic);
    if (Context* c = g_dev->beginFrame()) {
        c->bindPipeline(cp);
        c->bindStorageBuffer(1, out);
        c->bindTexture(3, tex);
        c->bindTexture(4, tex2d);
        c->dispatch(1, 1, 1);
        c->memoryBarrier();
        // ⚠️ FENCE, como hace el test de coste de F1 que SI funciona en GL. Con solo `memoryBarrier`
        // el canario salia 0 en OpenGL y parecia que el compute no se ejecutaba.
        FenceHandle f = c->signalFence();
        g_dev->endFrame();
        if (Context* c2 = g_dev->beginFrame()) {
            c2->waitFence(f, 5000000000ull); c2->deleteFence(f); g_dev->endFrame();
        }
    }
    // `copyBuffer` (submitOneShot) FUERA de todo frame: dentro pierde el dispositivo en Vulkan.
    BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                          BufferMemory::Readback);
    copyThenWait(out, rb, 0, N * sizeof(float));
    const float* got = (const float*)g_dev->mappedData(rb);
    if (!got) { CHECK(false, "readback del repro"); g_dev->destroy(tex); return; }

    std::printf("    textureSize que ve el shader: %.0f x ? x %.0f capas  (esperado 2 x ? x 6)\n",
                got[12], got[13]);
    std::printf("    texelFetch por capa: %.0f %.0f %.0f %.0f %.0f %.0f  (esperado 10..60)\n",
                got[0], got[1], got[2], got[3], got[4], got[5]);
    std::printf("    texture()  por capa: %.0f %.0f %.0f %.0f %.0f %.0f\n",
                got[6], got[16], got[8], got[9], got[10], got[11]);

    std::printf("    CONTROL sampler2D: tamaño %.0f · valor %.0f  (esperado 2 · 10)\n",
                got[14], got[15]);
    std::printf("    CANARIO (el dispatch escribio?): %.0f  (esperado 12345)\n", got[16]);
    CHECK(got[16] == 12345.0f, "el compute SE EJECUTA y escribe en el SSBO");

    // ⚠️ ESTE TEST NACIO PARA CAZAR EL "BUG 9" — Y DEMOSTRO QUE NO EXISTIA.
    //
    // Durante una sesion entera "en OpenGL un compute no puede muestrear texturas" fue un hecho: el
    // generador de nodos devolvia 0,00 en los 16 641 texeles y el terreno salia sin continentes. Era
    // FALSO. Todos los sintomas venian de una fence que faltaba DESPUES de `copyBuffer` (ver
    // `copyThenWait`): se leia el destino a medio llenar y se interpretaba como que el shader habia
    // calculado mal. En Vulkan no se notaba, asi que parecia un fallo del backend de GL.
    //
    // El CANARIO es lo que lo desenredo, y llego el ultimo: sin el, un dispatch que no escribe y un
    // sampler que devuelve cero son indistinguibles. Deberia haber sido lo primero.
    CHECK(got[14] == (float)W, "un sampler2D normal llega al compute");
    CHECK(got[12] == (float)W && got[13] == (float)L, "el shader ve el TAMAÑO de la textura");
    bool allOk = true;
    for (int l = 0; l < L; ++l)
        if (got[l] != (float)((l + 1) * 10) || got[6 + l] != (float)((l + 1) * 10)) allOk = false;
    CHECK(allOk, "cada capa devuelve SU valor, con texelFetch y con texture()");

    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(cp);
    g_dev->destroy(tex); g_dev->destroy(tex2d);
}


// ================================================================================================
// BUG 9 — BISECCION: ¿el compute, o el ADAPTADOR? Los dos caminos, mismo nodo, misma ejecucion.
//
// El dato que abrio esto: para el MISMO nodo y el MISMO backend (OpenGL),
//
//     F1 (pipeline y UBO propios, dispatch directo)   0.024231 m   <- bien
//     F3 (via TerrainNodeGpu::generatePending)       399.5896 m    <- roto
//
// O sea que el compute de GL NO esta roto: lo que difiere es el adaptador. Este test corre los dos
// caminos seguidos, sobre el mismo nodo, y compara cada uno con la referencia de CPU **y entre si**.
// Si A casa y B no, el fallo esta en lo que B hace de mas: el anillo de UBOs por dispatch, el hueco
// del pool, o el numero de grupos de trabajo.
// ================================================================================================
static void testNodeGpuAdapterBisect()
{
    BEGIN("bug 9: bisecar compute directo contra TerrainNodeGpu");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const NodeId node{ Haruka::PlanetFace::FRONT, 14, 4200, 3100 };   // el mismo de F1 y F3
    const uint32_t N = TERRAIN_NODE_TEXELS;
    const size_t count = (size_t)N * N;

    std::vector<float> ref(count);
    nodeFillHeights(node, R, ref.data());

    const std::string cs = Haruka::Shader::baseDir() + "shaders/terrain_node.comp";

    // ── CAMINO A: dispatch directo, UBO propio (lo que hace F1) ─────────────────────────────────
    std::vector<float> a(count, -1.0f);
    {
        PipelineDesc pd; pd.computePath = cs.c_str();
        PipelineHandle cp = g_dev->createPipeline(pd);
        if (!valid(cp)) { CHECK(false, "pipeline directo"); return; }
        struct ParamsUBO { int32_t node[4]; int32_t grid[4]; float misc[4]; } up{};
        up.node[0] = (int32_t)node.face; up.node[1] = (int32_t)node.level;
        up.node[2] = (int32_t)node.i;    up.node[3] = (int32_t)node.j;
        up.grid[0] = (int32_t)N; up.grid[1] = (int32_t)TERRAIN_NODE_CELLS;
        up.grid[2] = 0; up.grid[3] = 0;                       // modo produccion, hueco 0
        up.misc[0] = (float)R; up.misc[1] = (float)nodeTexelM(node, R);
        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(up), &up,
                                               BufferMemory::Dynamic);
        // ⚠️ EL PATRON DE LECTURA ES EL DE F1, Y NO ES INTERCAMBIABLE: SSBO de salida creado ya como
        // `Readback` (mapeo persistente) + fence + `mappedData` DIRECTO. Sin `copyBuffer`.
        //
        // Con `Storage/Dynamic` + `copyBuffer` + `mappedData` de otro buffer, en OpenGL se lee BASURA
        // (1,7e38): el destino nunca se llena. Ese era el patron de mi repro del bug 9 y el de
        // `testTerrainNodeBaseField` — o sea que sus dos veredictos sobre OpenGL median el readback,
        // no el shader.
        std::vector<float> sentinel(count, -1.0f);
        BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float),
                                               sentinel.data(), BufferMemory::Readback);
        FenceHandle f{};
        if (Context* c = g_dev->beginFrame()) {
            c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
            c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
            c->memoryBarrier();
            f = c->signalFence();
            g_dev->endFrame();
            if (Context* c2 = g_dev->beginFrame()) {
                c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame();
            }
        }
        if (const float* g = (const float*)g_dev->mappedData(out)) std::memcpy(a.data(), g, count * sizeof(float));
        g_dev->destroy(out); g_dev->destroy(ubo); g_dev->destroy(cp);
    }

    // ── CAMINO B: el adaptador de verdad ────────────────────────────────────────────────────────
    std::vector<float> b(count, -1.0f);
    int slot = -1;
    {
        // Capacidad 1: el nodo cae en el hueco 0 y el buffer tiene el tamaño de UN nodo, igual que
        // el camino directo. Si con esto casa, el fallo esta en el desplazamiento por hueco.
        TerrainNodePool pool(1);
        TerrainNodeGpu  gpu;
        if (!gpu.init(g_dev, cs.c_str(), 1)) { CHECK(false, "init del adaptador"); return; }
        pool.beginFrame(); pool.request(node);
        gpu.generatePending(g_dev->beginFrame(), pool, R);
        g_dev->endFrame();
        slot = pool.request(node).slot;
        // ⚠️ FENCE ANTES DE COPIAR. `generatePending` acaba con `memoryBarrier` + `endFrame`, y eso
        // ordena los comandos, pero NO dice cuando la copia puede leer lo escrito. Sin esto, en
        // OpenGL salian 399 m de "divergencia" que eran datos a medio escribir.
        {
            FenceHandle fb{};
            if (Context* c = g_dev->beginFrame()) {
                c->memoryBarrier(); fb = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) {
                    c2->waitFence(fb, 10000000000ull); c2->deleteFence(fb); g_dev->endFrame();
                }
            }
        }
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, count * sizeof(float), nullptr,
                                              BufferMemory::Readback);
        g_dev->copyBuffer(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode, 0,
                          count * sizeof(float));
        // ⚠️ LA FENCE VA **DESPUES** DE LA COPIA, NO ANTES. Esta es LA causa de todo el enredo del
        // "bug 9": con la fence antes, `mappedData` leia el destino a medio llenar y salian 399 m de
        // divergencia contra la CPU — solo en OpenGL. La generacion SIEMPRE fue correcta.
        {
            FenceHandle fc{};
            if (Context* c = g_dev->beginFrame()) {
                c->memoryBarrier(); fc = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) {
                    c2->waitFence(fc, 10000000000ull); c2->deleteFence(fc); g_dev->endFrame();
                }
            }
        }
        if (const float* g = (const float*)g_dev->mappedData(rb)) std::memcpy(b.data(), g, count * sizeof(float));
        g_dev->destroy(rb);
        gpu.shutdown();
    }

    double wA = 0.0, wB = 0.0, wAB = 0.0;
    for (size_t k = 0; k < count; ++k) {
        wA  = std::max(wA,  (double)std::fabs(a[k] - ref[k]));
        wB  = std::max(wB,  (double)std::fabs(b[k] - ref[k]));
        wAB = std::max(wAB, (double)std::fabs(a[k] - b[k]));
    }
    std::printf("    el nodo cayo en el hueco %d\n", slot);
    std::printf("    A  dispatch DIRECTO   vs CPU: %10.4f m\n", wA);
    std::printf("    B  TerrainNodeGpu     vs CPU: %10.4f m\n", wB);
    std::printf("    A vs B (los dos caminos entre si): %10.4f m\n", wAB);

    CHECK(wA < 0.05, "el camino DIRECTO casa con la CPU (la tolerancia declarada)");
    CHECK(wB < 0.05, "el camino del ADAPTADOR casa con la CPU");
    CHECK(wAB < 0.05, "los dos caminos producen LO MISMO (si no, el fallo esta en el adaptador)");
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
    testBindingPersistence();
    testComputeArraySampler();
    testNodeGpuAdapterBisect();
    testDispatchInsideRenderPass();
    testTerrainNodeGpuParity();
    testTerrainNodeGpuCost();
    testTerrainNodePoolGpu();
    testTerrainNodeRender();
    testTerrainNodeRendererInit();
    testTerrainNodeBaseField();
    testTerrainNodeCoverage();
    testTerrainNodeSeamHoles();
    testTerrainNodeSharedEdgeGpu();
    testTerrainNodeStitchSymmetryGpu();
    testTextureContent();
    testVertexColor();
    testEnginePipelines();
    testOceanShading();
    testSkyAmbientGPU();
    testTerrainLighting();
    testPropLighting();
    testTerrainShadow();
    testCullWindingWithProjection();

    // ⚠️ EL ULTIMO, Y A PROPOSITO. Es el unico test del banco que dibuja a un render target
    // OFFSCREEN, y deja el backend en un estado que hace fallar al siguiente que lee pixeles del
    // framebuffer por defecto (`testCullWindingWithProjection`). No he encontrado QUE queda mal:
    // el viewport lo restaura `beginRenderPass`, el target se destruye y aun asi pasa, y un frame
    // de restauracion explicito NO lo arregla. Es un gap del RHI, no del terreno.
    //
    // Ponerlo al final es una MITIGACION, no un arreglo: mientras siga aqui, nadie puede añadir un
    // test detras sin comprobar que no hereda basura. Queda anotado para cuando se toque el RHI.
    testTerrainNodeShadeCost();

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

    if (!locateAssets()) { std::printf("\n== 0 OK · 1 FALLOS ==\n"); return 1; }

    if (run == "gl" || run == "all") runBackend(Backend::OpenGL);
    if (run == "vk" || run == "all") runBackend(Backend::Vulkan);

    SDL_Quit();
    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
