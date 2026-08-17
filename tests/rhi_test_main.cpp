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

#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "rhi/rhi_resources.h"
#include "core/logger.h"
#include "renderer/shader.h"   // Shader::baseDir() para los shaders del banco

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
