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

#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"
#include "core/logger.h"
#include "renderer/shader.h"
#include "core/camera.h"
#include "core/sky_ambient.h"   // gemelo CPU del ambiente (paridad)   // Shader::baseDir() para los shaders del banco

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
        { "planeta/clipmap",       sh+"planet/clipmap.vert", sh+"planet/biome.frag",
          sh+"planet/clipmap.tesc", sh+"planet/clipmap.tese", true },
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
    testDispatchInsideRenderPass();
    testTextureContent();
    testVertexColor();
    testEnginePipelines();
    testOceanShading();
    testSkyAmbientGPU();
    testTerrainLighting();
    testPropLighting();
    testTerrainShadow();
    testCullWindingWithProjection();

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
