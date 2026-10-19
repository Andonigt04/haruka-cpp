#include "RHI/Common/RHITestCommon.h"

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

void testTextureFormats()
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

void testBuffers()
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

void testRenderTargets()
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

void testArrayAndCube()
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

void testFrameCycle()
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
void testBindingPersistence()
{
    BEGIN("bindings: persisten entre draws y entre pipelines");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/tests/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/tests/rhitest_solid.frag";
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
void testDispatchInsideRenderPass()
{
    BEGIN("compute: dispatch dentro de un render pass no tumba el device");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string cs = base + "shaders/tests/rhitest_noop.comp";
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
        const Haruka::Core::LogLevel prevLevel = Haruka::Core::getLogLevel();
        Haruka::Core::setLogLevel(Haruka::Core::LogLevel::None);
        c->dispatch(4, 1, 1);       // ILEGAL en Vulkan: el backend debe rechazarlo, no grabarlo
        Haruka::Core::setLogLevel(prevLevel);
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
// BLIT CON EL PASE ABIERTO: nubes y fluido copian la escena (`blitColor`/`blitDepth`) justo tras
// `beginFrame`, con el pase de la escena SIN CERRAR. En GL es legal (un blit no depende del FBO atado);
// en Vulkan `vkCmdCopyImage` y sus barreras dentro de un pase son ilegales (VUID-vkCmdCopyImage-
// renderpass, -PipelineBarrier-None-07889), y lo ilegal es lo que pierde el dispositivo. Lo destapo
// la capa de validacion en el juego (17-10), tras un cuelgue de GPU que tumbo el escritorio.
//
// Ademas la profundidad va con el formato POR DEFECTO (D24S8), que AMD no tiene en Vulkan: ejerce la
// caida a D32F_S8 y la copia de profundidad entre dos targets de ese formato.
//
// Se mide que la copia LLEGUE: A se limpia a naranja con el pase abierto, se copia a B (que estaba
// verde) y se muestrea B en pantalla. CONTRAPRUEBA: lo mismo sin la copia tiene que seguir verde,
// o el test no estaria leyendo B.
// ────────────────────────────────────────────────────────────────────────────────────────────────
void testBlitInsideRenderPass()
{
    BEGIN("blit: copiar con el pase ABIERTO llega y no tumba el device");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/tests/rhitest_tex.frag";
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

    const int n = 64;
    RenderTargetDesc d{};
    d.width = n; d.height = n;
    d.colorFormats = { Format::RGBA8 };
    d.colorFilter = Filter::Nearest;
    d.hasDepth = true;                    // D24S8 por defecto: AMD no lo tiene en Vulkan
    RenderPassHandle a = g_dev->createRenderTarget(d);
    RenderPassHandle b = g_dev->createRenderTarget(d);
    CHECK(valid(a) && valid(b), "dos targets RGBA8 + profundidad por defecto");
    const float uv[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, 16, uv, BufferMemory::Dynamic);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    auto clearTo = [](Context* c, RenderPassHandle t, float r, float g, float bl) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = r; cv.color[1] = g; cv.color[2] = bl; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass(t, cv);
    };
    // Un frame: B a verde (cerrado), A a naranja (ABIERTO), copia A -> B si `blit`, y B a pantalla.
    auto run = [&](bool blit, unsigned char out[4]) {
        std::memset(out, 0, 4);
        Context* c = g_dev->beginFrame();
        if (!c) return false;
        clearTo(c, b, 0.1f, 0.8f, 0.2f);
        c->endRenderPass();
        clearTo(c, a, 0.9f, 0.5f, 0.1f);  // se queda ABIERTO a proposito
        if (blit) {
            c->blitColor(a, b, n, n);
            c->blitDepth(a, b, n, n);
        }
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ubo);
        c->bindTexture(0, g_dev->getColorTexture(b, 0));
        c->draw(3);
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
        return true;
    };

    unsigned char got[4] = {}, ctl[4] = {};
    if (valid(a) && valid(b) && run(true, got) && run(false, ctl)) {
        std::printf("    B tras copiar A con el pase abierto = (%d,%d,%d) · sin copiar = (%d,%d,%d)\n",
                    got[0], got[1], got[2], ctl[0], ctl[1], ctl[2]);
        auto near = [](const unsigned char* p, int r, int g, int bl) {
            return std::abs(p[0] - r) <= 8 && std::abs(p[1] - g) <= 8 && std::abs(p[2] - bl) <= 8;
        };
        CHECK(near(got, 230, 128, 26), "la copia hecha con el pase de A abierto llega a B (naranja)");
        CHECK(near(ctl, 26, 204, 51), "CONTRAPRUEBA: sin copiar, B sigue verde (se lee B de verdad)");
    }

    g_dev->destroy(ubo);
    if (valid(a)) g_dev->destroy(a);
    if (valid(b)) g_dev->destroy(b);
    g_dev->destroy(pipe);
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
void testTextureContent()
{
    BEGIN("textura: el contenido subido llega al shader");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/tests/rhitest_tex.frag";
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

void testVertexInterpolation()
{
    BEGIN("vertice: los atributos se INTERPOLAN dentro del triangulo");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_vcolor.vert";
    const std::string fs = base + "shaders/tests/rhitest_vcolor.frag";

    struct V { float px, py, pz; float r, g, b; };
    // Triangulo grande con un vertice ROJO, otro VERDE y otro AZUL, colocados para que cada uno
    // domine una esquina del cuadro de 256x256.
    const V verts[3] = {
        { -1.0f, -1.0f, 0.0f,  1.0f, 0.0f, 0.0f },
        {  3.0f, -1.0f, 0.0f,  0.0f, 1.0f, 0.0f },
        { -1.0f,  3.0f, 0.0f,  0.0f, 0.0f, 1.0f },
    };

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0, (uint32_t)offsetof(V, px), Format::RGB32F, 0 },
        { 2, (uint32_t)offsetof(V, r),  Format::RGB32F, 0 },
    };
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull         = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    BufferHandle   vb   = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts);
    CHECK(valid(pipe) && valid(vb), "pipeline y buffer creados");
    if (!valid(pipe) || !valid(vb)) return;

    const int W = 256, H = 256;
    std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
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
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
    }
    auto at = [&](int x, int y) {
        const size_t k = ((size_t)y * W + x) * 4;
        return std::array<int,3>{ px[k], px[k+1], px[k+2] };
    };
    const auto a = at(24, 24), b = at(W - 24, 24), c2 = at(24, H - 24), mid = at(W/2, H/2);
    std::printf("    tres vertices R/G/B en un solo triangulo:\n");
    std::printf("      esquina A (%3d,%3d,%3d) · esquina B (%3d,%3d,%3d) · esquina C (%3d,%3d,%3d)\n",
                a[0],a[1],a[2], b[0],b[1],b[2], c2[0],c2[1],c2[2]);
    std::printf("      centro    (%3d,%3d,%3d)  <- si INTERPOLA, es una mezcla de los tres\n",
                mid[0],mid[1],mid[2]);

    // Cuanto se separan las tres esquinas entre si. Con interpolacion son casi colores puros y
    // distintos; SIN ella los tres puntos salen del MISMO color (el del vertice provocador).
    int spread = 0;
    for (int ch = 0; ch < 3; ++ch) {
        spread = std::max(spread, std::abs(a[ch] - b[ch]));
        spread = std::max(spread, std::abs(a[ch] - c2[ch]));
        spread = std::max(spread, std::abs(b[ch] - c2[ch]));
    }
    std::printf("      separacion maxima entre esquinas: %d  (plano daria ~0)\n", spread);

    g_dev->destroy(pipe); g_dev->destroy(vb);

    CHECK(spread > 100, "los atributos SE INTERPOLAN: las tres esquinas tiran hacia su vertice");
    CHECK(mid[0] > 20 && mid[1] > 20 && mid[2] > 20,
          "y el centro es MEZCLA de los tres (no el color de un solo vertice)");
}

void testVertexColor()
{
    BEGIN("vertice: un atributo que no es la posicion llega al fragment");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_vcolor.vert";
    const std::string fs = base + "shaders/tests/rhitest_vcolor.frag";

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
void testCullWindingWithProjection()
{
    BEGIN("giro de caras: la proyeccion del motor no puede invertir el descarte");

    // Antihorario mirando desde la cámara (que mira hacia -Z), a 2 m de distancia. Grande para que
    // cubra el centro de la pantalla con cualquier aspecto razonable.
    const float triCCW[9] = { -0.8f, -0.8f, -2.0f,
                               0.8f, -0.8f, -2.0f,
                               0.0f,  0.8f, -2.0f };

    auto drawWith = [&](const float verts[9], uint8_t out[4]) -> bool {
        Haruka::Core::Camera cam(Haruka::Tools::WorldPos(0.0, 0.0, 0.0));
        const glm::mat4 proj = cam.getProjectionMatrix(1.0f);

        BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(glm::mat4),
                                               &proj, BufferMemory::Dynamic);
        BufferHandle vb  = g_dev->createBuffer(BufferUsage::Vertex, 9 * sizeof(float),
                                               verts, BufferMemory::Static);

        PipelineDesc pd;
        const std::string sd = Haruka::Renderer::Shader::baseDir() + "shaders/";
        const std::string vsp = sd + "tests/rhitest_cull.vert", fsp = sd + "tests/rhitest_cull.frag";
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
// ¿`double` ES DOUBLE EN ESTA GPU? El test que le faltaba al banco, y que explica una sesion entera.
//
// La sonda `terrain_node_drawn_vs_field` da, con el MISMO shader y el MISMO dato:
//
//     OpenGL sobre NVIDIA   stride 1 -> media 0,0015 m · peor 0,0107 m
//     OpenGL sobre AMD      stride 1 -> media 0,0405 m · peor 0,0889 m     27x peor
//
// No es el backend —Vulkan sobre NVIDIA da las mismas cifras que GL sobre NVIDIA— es la GPU. Y el
// sospechoso es la aritmetica en double del vertex shader, que es justo lo que se puso para que el
// ulp del float (0,38-0,76 m a radio terrestre) dejara de verse.
//
// Aqui no se mide terreno: se mide si `double` es double, con casos que en float colapsan a CERO.
// ================================================================================================
void testGpuFp64()
{
    BEGIN("¿la GPU hace la aritmetica en DOUBLE, o la degrada a float?");

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/fp64_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de fp64_probe.comp creado");
    if (!valid(cp)) return;

    const size_t N = 12;
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Readback);

    // ⚠️ LAS ENTRADAS VAN POR SSBO Y ESO ES EL TEST, NO FONTANERIA. Escritas como literales en el
    // shader, el driver plegaba los nueve casos en tiempo de compilacion y pasaban en cualquier GPU:
    // se medi­a el plegador de constantes del compilador, no la ALU. Ver la cabecera del .comp.
    // Todos son exactos en float, asi que `double(uIn[k])` vale exactamente lo que se pretende.
    const float inputs[10] = { 6371000.0f, 1.0e-10f, 0.001f, 2.0f,
                               0.31f, 0.62f, 0.72f, -0.4f, 0.25f, 0.0f };
    BufferHandle in = g_dev->createBuffer(BufferUsage::Storage, sizeof(inputs), inputs,
                                          BufferMemory::Static);

    // El dispatch va FUERA de un render pass (en Vulkan dentro es ilegal, ver
    // `testDispatchInsideRenderPass`), y el `memoryBarrier` ordena la escritura antes de la copia.
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, N * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (v) {
        // ── LOS ORACULOS, CALCULADOS EN LA CPU ──────────────────────────────────────────────────
        //
        // ⚠️ NINGUNO ES UN UMBRAL INVENTADO: los tres son el MISMO calculo hecho en `double` de x86,
        // donde `sqrt` es correctamente redondeada por el hardware. Lo que se comprueba es que la GPU
        // llegue al mismo sitio, no que caiga dentro de un margen que yo haya elegido.
        // ⚠️ LOS ORACULOS PARTEN DE LOS MISMOS FLOAT QUE EL SHADER, no de los decimales bonitos: el
        // shader recibe `0.31f` y hace `double(0.31f)`, que NO es 0.31. Escribir 0.31 aqui metería un
        // desajuste de 1e-9 que se confundiría con el bug que se busca.
        const glm::dvec3 seedRef((double)inputs[4], (double)inputs[5], (double)inputs[6]);
        const glm::dvec3 pRef  = glm::normalize(seedRef) * (double)inputs[0];
        const double residRef  = pRef.x - (double)(float)pRef.x;          // caso (3)
        const double sqrtRef   = (std::sqrt(2.0) * std::sqrt(2.0) - 2.0) * 1.0e12;   // caso (5)
        const double rsqrtRef  = (1.0 / std::sqrt(2.0) * (1.0 / std::sqrt(2.0)) * 2.0 - 1.0) * 1.0e12;
        const glm::dvec3 rRef  = Haruka::World::Terrain::cubeFaceToDir(Haruka::Tools::PlanetFace::FRONT,
                                                       (double)inputs[7], (double)inputs[8]);
        const double unitRef   = (glm::dot(rRef, rRef) - 1.0) * 1.0e12;   // caso (7)
        const double rxResid   = (rRef.x - (double)(float)rRef.x) * 1.0e9; // caso (10)

        std::printf("    (1) (1+1e-10 - 1)*1e10        = %.6f        (double: 1,0 · float: 0)\n", v[0]);
        std::printf("    (2) (R+1mm - R) en mm         = %.6f        (double: 1,0 · float: 0)\n", v[1]);
        std::printf("    (3) residuo sub-float de p.x  = %.9f m  (CPU: %.9f · float: 0 exacto)\n",
                    v[2], residRef);
        std::printf("    (4) el (2) en FLOAT           = %.6f        (contraprueba: tiene que dar -1)\n", v[3]);
        std::printf("    (5) (sqrt(2)^2 - 2)*1e12      = %.6f        (CPU: %.6f)\n", v[4], sqrtRef);
        std::printf("    (6) inversesqrt, idem         = %.6f        (CPU: %.6f)\n", v[5], rsqrtRef);
        std::printf("    (7) (|cubeFaceToDir|^2-1)*1e12= %.6f        (CPU: %.6f)\n", v[6], unitRef);
        std::printf("    (8) ...en METROS de superficie= %.9f m  (comparable con el bake)\n", v[7]);
        std::printf("    (9) el (5) en FLOAT           = %.1f      (contraprueba: ~2,4e5)\n", v[8]);
        std::printf("   (10) residuo sub-float de r.x  = %.6f      (CPU: %.6f · float: 0 exacto)\n",
                    v[9], rxResid);
        const double mulRef = (double)inputs[0] * (double)inputs[4];
        const double divRef = (double)inputs[0] / 3.0;
        std::printf("   (11) residuo del PRODUCTO      = %.9f  (CPU: %.9f · float: 0 exacto)\n",
                    v[10], mulRef - (double)(float)mulRef);
        std::printf("   (12) residuo de la DIVISION    = %.9f  (CPU: %.9f · float: 0 exacto)\n",
                    v[11], divRef - (double)(float)divRef);
        CHECK(std::fabs((double)v[10] - (mulRef - (double)(float)mulRef)) < 1e-4,
              "(11) el PRODUCTO en doble conserva los bits que un float no puede guardar");
        CHECK(std::fabs((double)v[11] - (divRef - (double)(float)divRef)) < 1e-4,
              "(12) la DIVISION en doble conserva los bits que un float no puede guardar");

        // ⚠️ SIN TOLERANCIA BLANDA: en float estos dos casos dan CERO EXACTO, asi que basta con pedir
        // que se parezcan a lo esperado. Un 0,5 aqui seria tan diagnostico como un 0.
        CHECK(std::fabs(v[0] - 1.0f) < 0.01f,
              "(1) la GPU distingue 1+1e-10 de 1 -> hay fp64 de verdad");
        CHECK(std::fabs(v[1] - 1.0f) < 0.01f,
              "(2) la GPU suma 1 mm a un radio terrestre sin perderlo");
        // El residuo es un numero CONCRETO, no "distinto de cero": si la GPU calculo en double llega
        // al mismo que la CPU salvo el ulp del double (1e-9 m a esta escala). Un cero exacto aqui es
        // la firma de que `normalize(dvec3)*R` se hizo en float.
        CHECK(std::fabs((double)v[2] - residRef) < 1.0e-6,
              "(3) `normalize(dvec3)*R` conserva los bits que un float no puede guardar");
        // La contraprueba: el MISMO caso en float tiene que perder el milimetro. Si no lo perdiera,
        // (2) no estaria demostrando nada sobre el double.
        CHECK(std::fabs(v[3] + 1.0f) < 0.01f,
              "(4) en FLOAT el milimetro SE PIERDE (si no, el caso (2) no demuestra nada)");

        // ── LA RAIZ EN DOBLE, QUE ES LO QUE NO SE MIRABA ────────────────────────────────────────
        //
        // Margen 1,0 en unidades de 1e-12, o sea 1e-12 relativo: dos mil veces el ulp del double y
        // aun asi SEIS ORDENES por debajo de lo que daria la aproximacion `V_RSQ_F64` sin pulir
        // (~1e3). No hay zona gris entre "correctamente redondeada" y "aproximada".
        CHECK(std::fabs((double)v[4] - sqrtRef) < 1.0,
              "(5) `sqrt(double)` esta correctamente redondeada (no es la aproximacion del hardware)");
        CHECK(std::fabs((double)v[5] - rsqrtRef) < 1.0,
              "(6) `inversesqrt(double)` esta correctamente redondeada");
        // El camino REAL: tres `sqrt(double)` dentro de `harukaCubeFaceToDir`, medidos sin `sqrt`.
        CHECK(std::fabs((double)v[6] - unitRef) < 1.0,
              "(7) la direccion del bake sale unitaria con precision de double");
        // Y la traduccion a lo que se ve: por debajo del milimetro no puede explicar los 2 cm del bake.
        CHECK(v[7] < 0.001f,
              "(8) el error de la proyeccion cara->esfera esta por debajo del milimetro");
        // Contraprueba del (5): sin ella, un (5) que casa no distingue "la raiz es exacta" de "la
        // sonda no esta midiendo la raiz".
        // ⚠️ EN VALOR ABSOLUTO. `sqrt(2)` en float redondea HACIA ABAJO, asi que `s*s-2` es NEGATIVO
        // (-68457) y un `> 1e4` lo daba por fallo. La contraprueba mide MAGNITUD de error, no signo.
        CHECK(std::fabs(v[8]) > 1.0e4f,
              "(9) en FLOAT la raiz SI pierde precision (si no, el caso (5) no demuestra nada)");
        // La misma pregunta que contesta el modo 6 del bake, aqui aislada: si la direccion sale con
        // bits por debajo del float, el bake no puede estar perdiendolos en esta funcion.
        CHECK(std::fabs((double)v[9] - rxResid) < 0.01,
              "(10) `harukaCubeFaceToDir` devuelve una direccion con precision de double");
    }
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}

void testComputeArraySampler()
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

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/rhitest_arraysample.comp";
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
