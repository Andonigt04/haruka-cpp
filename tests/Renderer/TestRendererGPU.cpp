#include "RHI/Common/RHITestCommon.h"
#include <glm/gtc/packing.hpp>
#include "Haruka/World/Props/MassBurst.h"   // MassBurstGpu: el brote del test de hierba
#include "Haruka/World/Props/PropGpu.h"     // PropVertex / PropParams
#include <cstring>   // el vértice empaquetado del prop (ver propLitPixel)
#include "Haruka/Renderer/ScenePass.h"     // testScenePassObjects: el pase de escena REAL
#include "Haruka/Renderer/SceneUBO.h"
#include "Haruka/Core/Camera.h"
#include "Haruka/Core/ApplicationInternal.h"   // cleanupGLStatics: mallas primitivas por backend

void testItemPreviewShader()
{
    BEGIN("icono de inventario: los shaders pintan y coinciden GL/VK");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/item_preview.vert";
    const std::string fs = base + "shaders/item_preview.frag";

    struct V { float px, py, pz, nx, ny, nz, r, g, b; };
    std::vector<V> verts;
    std::vector<uint32_t> idx;
    {   // Caja de 0.2 de semilado subida a y=+0.30: deliberadamente ARRIBA del encuadre.
        const glm::vec3 half(0.20f), off(0.0f, 0.30f, 0.0f), col(0.80f, 0.55f, 0.30f);
        const glm::vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; ++f) {
            const glm::vec3 n = N[f];
            const glm::vec3 u = std::abs(n.y) > 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
            const glm::vec3 t = glm::normalize(glm::cross(u, n)), b = glm::cross(n, t);
            const uint32_t v0 = (uint32_t)verts.size();
            const glm::vec3 c4[4] = { (n-t-b)*half + off, (n+t-b)*half + off,
                                      (n+t+b)*half + off, (n-t+b)*half + off };
            for (const glm::vec3& p : c4)
                verts.push_back({ p.x,p.y,p.z, n.x,n.y,n.z, col.r,col.g,col.b });
            for (uint32_t e : { 0u,1u,2u, 0u,2u,3u }) idx.push_back(v0 + e);
        }
    }

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0, (uint32_t)offsetof(V, px), Format::RGB32F, 0 },
        { 1, (uint32_t)offsetof(V, nx), Format::RGB32F, 0 },
        { 2, (uint32_t)offsetof(V, r),  Format::RGB32F, 0 },
    };
    pd.depth.test = true; pd.depth.write = true;
    pd.depth.compare = CompareOp::Less;       // el icono NO usa reversed-Z (ver cabecera)
    pd.blend.enable  = false;
    pd.cull          = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    BufferHandle   vb   = g_dev->createBuffer(BufferUsage::Vertex, verts.size()*sizeof(V), verts.data());
    BufferHandle   ib   = g_dev->createBuffer(BufferUsage::Index,  idx.size()*sizeof(uint32_t), idx.data());
    struct UBO { glm::mat4 mvp, model; } ubo;
    BufferHandle   ub   = g_dev->createBuffer(BufferUsage::Uniform, sizeof(UBO), nullptr, BufferMemory::Dynamic);
    CHECK(valid(pipe) && valid(vb) && valid(ib) && valid(ub),
          "pipeline del icono creado (si falla aqui, falta el .spv: el glob de shaders)");
    if (!valid(pipe)) return;

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    // SIN invertir la Y en Vulkan, igual que item_preview.cpp — y por la razon que este mismo test
    // midio: el destino es una TEXTURA, no la pantalla, y su convenio de origen ya absorbe la
    // diferencia. Con el flip puesto, las dos mitades salian intercambiadas entre backends.
    const glm::mat4 proj = glm::ortho(-0.62f, 0.62f, -0.62f, 0.62f, -4.0f, 4.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.9f, 0.75f, 1.0f), glm::vec3(0.0f), glm::vec3(0,1,0));
    ubo.model = glm::mat4(1.0f);
    ubo.mvp   = proj * view * ubo.model;
    g_dev->updateBuffer(ub, 0, sizeof(UBO), &ubo);

    const int W = 256, H = 256;
    std::vector<uint8_t> px((size_t)W * H * 4, 0x00);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 1.0f;        // ver cabecera: 1, no el 0 de reversed-Z
        c->beginRenderPass({}, cv);
        c->setViewport(0, 0, W, H);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ub);
        c->bindVertexBuffer(vb);
        c->bindIndexBuffer(ib);
        c->drawIndexed((uint32_t)idx.size());
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
    }

    // Tinta = pixel que no es el fondo negro del clear. Se cuenta por MITADES del array de texeles.
    long lit = 0, litLow = 0, litHigh = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t k = ((size_t)y * W + x) * 4;
            if (px[k] + px[k+1] + px[k+2] > 24) { ++lit; (y < H/2 ? litLow : litHigh) += 1; }
        }
    const double frac    = (double)lit / (double)(W*H) * 100.0;
    const double highPct = lit ? (double)litHigh / (double)lit * 100.0 : 0.0;
    std::printf("    %s: %ld px con tinta (%.2f%% del cuadro)\n",
                isVk ? "VULKAN" : "OpenGL", lit, frac);
    std::printf("      reparto por mitades del array de texeles: filas 0..127 %.1f%% · filas 128..255 %.1f%%\n",
                100.0 - highPct, highPct);
    std::printf("      (ImGui muestrea el texel (0,0) en los dos backends: mismo reparto = misma imagen)\n");

    // 1. Que pinte ALGO. Es la guardia contra el fallo mudo: una caja de este tamano cubre bastante
    //    mas del 1% del cuadro, y 0% es exactamente lo que daba el bug de la profundidad heredada.
    CHECK(frac > 1.0, "el icono pinta (no es la textura vacia del fallo mudo)");

    // 2. Que este DESCOMPENSADO, o el test no podria detectar un volteo: si la tinta quedara repartida
    //    50/50 la caja seria simetrica en vertical y dar la vuelta a la imagen no cambiaria nada.
    CHECK(highPct < 35.0 || highPct > 65.0, "la caja cae claramente en una mitad (el test puede ver un volteo)");

    // 3. Y que los DOS backends la pongan en la MISMA mitad. Es el careo que de verdad importa: un
    //    icono espejado no da ningun error, se dibuja perfecto, y solo se nota si alguien mira una
    //    pieza asimetrica. Los dos backends corren en el mismo proceso, GL primero, asi que en la
    //    pasada de Vulkan ya hay con que comparar.
    static double s_highPct[2] = { -1.0, -1.0 };
    s_highPct[isVk ? 1 : 0] = highPct;
    if (s_highPct[0] >= 0.0 && s_highPct[1] >= 0.0) {
        const double d = std::abs(s_highPct[0] - s_highPct[1]);
        std::printf("      careo con OpenGL: %.1f%% vs %.1f%% (diferencia %.1f pts)\n",
                    s_highPct[0], s_highPct[1], d);
        CHECK(d < 5.0, "GL y Vulkan ponen el icono en la misma mitad (no esta espejado)");
    }

    g_dev->destroy(pipe); g_dev->destroy(vb); g_dev->destroy(ib); g_dev->destroy(ub);
}


// ================================================================================================
// EL PASE DE ESCENA DIBUJA CADA OBJETO CON SUS DATOS, en los DOS backends.
//
// ⚠️ FALLO MEDIDO (17-10): `ScenePass` reescribia UN solo UBO por objeto antes de cada draw. En GL
// `glBufferSubData` queda ordenado entre draws; en Vulkan el buffer es memoria mapeada y la escritura
// es un `memcpy` inmediato, asi que todos los draws del frame leian los datos del ULTIMO objeto. En el
// juego el Sol y la Luna salian con la matriz de otro y no se veian (y en GL si).
//
// Tres objetos emisivos con el pase REAL (`ScenePass::begin` + `drawObjects`): uno a cada lado y una
// "Luna" a 384 400 km en el centro, a su escala real. Tienen que salir los tres. Con el UBO compartido
// solo sale el ultimo, asi que el test lo ve en una sola pasada.
// ================================================================================================
void testScenePassObjects()
{
    BEGIN("pase de escena: cada objeto con su UBO (Sol y Luna en Vulkan)");
    using namespace Haruka::Core::Scene;

    auto body = [](const char* name, glm::dvec3 pos, double scale, glm::vec3 col) {
        auto o = std::make_shared<SceneObject>();
        // LUZ emisiva (no STAR: una estrella es un cuerpo del cielo y su color sale de la radiometria,
        // no del objeto). Color plano: lo que se mide es que cada uno salga con SUS datos.
        o->name = name; o->objectType = Haruka::Tools::ObjectType::LIGHT;
        o->flags.castLight = true;
        o->position = Haruka::Tools::WorldPos(pos); o->scale = glm::dvec3(scale);
        o->color = glm::vec4(col, 1.0f);
        return o;
    };
    std::vector<std::shared_ptr<SceneObject>> objs = {
        body("izq",  { -3.0, 0.0, -10.0 }, 1.0, { 1.0f, 0.2f, 0.2f }),
        body("Luna", {  0.0, 0.0, -384400000.0 }, 1737400.0 * 8.0, { 0.2f, 1.0f, 0.2f }),
        body("der",  {  3.0, 0.0, -10.0 }, 1.0, { 0.2f, 0.2f, 1.0f }),
    };
    std::vector<RenderCommand> queue;
    RenderQueueState qs;
    syncSceneRenderQueue(objs, queue, qs);
    CHECK(queue.size() == 3, "los tres van a la cola");

    const int W = 256, H = 256;
    Haruka::Core::Camera cam(Haruka::Tools::WorldPos(glm::dvec3(0.0)));   // mira a -Z
    Haruka::Renderer::PerFrameUBOData fd{};
    fd.view = glm::mat4(glm::mat3(cam.getViewMatrix()));
    fd.projection = cam.getProjectionMatrix((float)W / (float)H);
    fd.sunDirection = glm::vec3(0, 1, 0); fd.sunLightColor = glm::vec3(1.0f);
    BufferHandle frameUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(fd), &fd, BufferMemory::Dynamic);

    // EL TARGET DE ESCENA DEL MOTOR: color + D32F con reversed-Z. Con un depth de punto fijo (el
    // backbuffer de GL es D24) la profundidad de la Luna (~1e-10) redondea a 0 y no pasa el GREATER.
    RenderTargetDesc rd{};
    rd.width = W; rd.height = H;
    rd.colorFormats = { Format::RGBA8 };
    rd.hasDepth = true; rd.depthFormat = Format::D32F;
    RenderPassHandle rt = g_dev->createRenderTarget(rd);
    CHECK(valid(rt), "target de escena D32F creado");
    if (!valid(rt)) { g_dev->destroy(frameUbo); return; }

    Haruka::Renderer::ScenePass pass;
    Haruka::Renderer::ScenePass::Frame sf;
    sf.camera = &cam; sf.perFrameUBO = frameUbo; sf.width = W; sf.height = H;
    sf.sceneTargetPass = rt;
    Haruka::Renderer::ScenePass::DrawStats st;

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    std::vector<uint8_t> px((size_t)W * H * 4, 0x00);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;        // reversed-Z, como el motor
        c->beginRenderPass(rt, cv);
        c->setViewport(0, 0, W, H);
        c->endRenderPass();
        pass.begin(c, sf, true);
        c->setViewport(0, 0, W, H);
        pass.drawObjects(c, sf, queue, st);
        c->endRenderPass();
        ClearValues bb; bb.clearColor = true; bb.clearDepth = true; bb.depth = 0.0f;
        c->beginRenderPass({}, bb);
        c->endRenderPass();
        c->blitColor(rt, {}, W, H);                    // el color de la escena, a donde se lee
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
    }

    // Por TERCIOS en horizontal (el volteo vertical de un backend no los mezcla): rojo a la izquierda,
    // verde en el centro, azul a la derecha. Rojo y azul se cuentan sin distinguirlos entre si: la
    // copia al backbuffer de Vulkan es CRUDA (target RGBA -> swapchain BGRA) y los intercambia; lo
    // que se mide es que el objeto ESTE, no su canal.
    long red = 0, green = 0, blue = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t* p = &px[((size_t)y * W + x) * 4];
            const int third = x * 3 / W;
            const bool rb = std::max(p[0], p[2]) > 100 && std::abs((int)p[0] - (int)p[2]) > 40 && p[1] < 120;
            if (third == 0 && rb) ++red;
            if (third == 1 && p[1] > 100 && p[1] > p[0] + 40) ++green;
            if (third == 2 && rb) ++blue;
        }
    std::printf("    %s: draws %d · px rojo (izq) %ld · verde (Luna, centro) %ld · azul (der) %ld\n",
                isVk ? "VULKAN" : "OpenGL", st.renderedDrawCalls, red, green, blue);
    CHECK(st.renderedDrawCalls == 3, "el pase dibuja los tres");
    CHECK(red > 50,   "sale el objeto de la izquierda (con un UBO compartido, no)");
    CHECK(green > 50, "sale la Luna a 384 400 km (con un UBO compartido, no)");
    CHECK(blue > 50,  "sale el de la derecha (el ultimo: este salia siempre)");

    pass.shutdown();
    // Las primitivas del motor (la esfera) se cachean en globales con buffers del device que las
    // creo: sin soltarlas aqui, la pasada de Vulkan dibujaba con los buffers de GL y no salia nada.
    Haruka::Core::AppInternal::cleanupGLStatics();
    g_dev->destroy(rt);
    g_dev->destroy(frameUbo);
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
void testEnginePipelines()
{
    const std::string sh = Haruka::Renderer::Shader::baseDir() + "shaders/";
    struct Case {
        const char* name;
        std::string vert, frag, tesc, tese;
        bool patches;
    };
    // Los pares REALES que crea el motor. Si alguien añade un pipeline y no lo pone aquí, este test
    // no lo cubre — por eso la lista vive junto a los tests y no dentro del engine: se ve lo que hay.
    const std::vector<Case> cases = {
        { "planeta/respaldo",      sh+"planet/simple.vert", sh+"planet/simple.frag", "", "", false },
        { "planeta/nodos (v5)",    sh+"terrain_node.vert", sh+"terrain_node.frag", "", "", false },
        // ⚠️ AQUI HABIA DOS ENTRADAS DE MAR (`ocean.*` teselado y `ocean_far.*`) Y SUS SHADERS YA NO
        // EXISTEN. El agua se migro al pase de nodos el 2026-09-01: dejo de tener geometria propia,
        // asi que dejo de tener pipelines propios. La entrada nueva es la que hay que vigilar.
        { "agua/nodos (v5)",       sh+"terrain_node_water.vert", sh+"terrain_node_water.frag",
          "", "", false },
        { "cielo",                 sh+"sky.vert",  sh+"sky.frag",  "", "", false },
        { "nubes volumétricas",    sh+"cloud_vol.vert", sh+"cloud_vol.frag", "", "", false },
        { "props instanciados",    sh+"prop_inst.vert", sh+"prop_inst.frag", "", "", false },
        // Los pares GENERICOS: un vertex para muchos fragment. Si uno deja de casar locations o UBOs
        // con su vertex compartido, el pase entero desaparece sin error de compilacion.
        { "obra instanciada",      sh+"mesh_inst.vert", sh+"prop_inst.frag", "", "", false },
        { "post: bloom extract",   sh+"fullscreen_tri.vert", sh+"bloom_extract.frag", "", "", false },
        { "post: bloom blur",      sh+"fullscreen_tri.vert", sh+"bloom_blur.frag", "", "", false },
        { "post: present",         sh+"fullscreen_tri.vert", sh+"post_present.frag", "", "", false },
        { "fluido: blur",          sh+"fullscreen_tri.vert", sh+"fluid_blur.frag", "", "", false },
        { "fluido: superficie",    sh+"fullscreen_tri.vert", sh+"fluid_surface.frag", "", "", false },
        { "nubes: composicion",    sh+"fullscreen_tri.vert", sh+"cloud_upsample.frag", "", "", false },
        { "IBL: BRDF LUT",         sh+"fullscreen_tri.vert", sh+"brdf_lut.frag", "", "", false },
        // El campo de suelo (Renderer/GroundField). Con los `BIND_*` de lib/backend.glsl su sampler caía
        // en el binding 133 en Vulkan y RADV se caía DENTRO de vkCreateGraphicsPipelines.
        { "suelo: campo",          sh+"fullscreen_tri.vert", sh+"ground_field.frag", "", "", false },
        { "IBL: equirect→cubo",    sh+"cube_capture.vert", sh+"equirect_to_cubemap.frag", "", "", false },
        { "IBL: irradiancia",      sh+"cube_capture.vert", sh+"irradiance_convolution.frag", "", "", false },
        { "IBL: prefiltrado",      sh+"cube_capture.vert", sh+"prefilter_env.frag", "", "", false },
        // ⚠️ `simple.vert`, NO un vertex de pantalla completa: es el par que construye el motor
        // (`ApplicationRender.cpp`, `_mainShader`/`m_scenePSO`). Aquel (hoy `fullscreen_tri.vert`) solo saca
        // `TexCoords`, y `final.frag` lee `Normal`/`FragPos`/`TexCoord` — o sea que este caso
        // comprobaba un par que el motor NUNCA arma. Colaba porque el driver de AMD enlaza
        // varyings que faltan; Mesa aplica la especificacion y da
        // `error: "FragPos" not declared as input from previous stage`.
        { "composición final",     sh+"simple.vert", sh+"final.frag", "", "", false },
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

// (Aqui vivia `testOceanShading`, que dibujaba un triangulo a pantalla completa con
// `ocean_far.vert` + `ocean.frag` para comprobar que el mar dibuja agua y DESCARTA la tierra. Los dos
// shaders se borraron al migrar el agua al pase de nodos el 2026-09-01, y la propiedad que probaba la
// cubre ahora `testNodeWaterDraws`: la misma pregunta, pero sobre el pase REAL, con contraprueba de
// tierra y con centinela del readback.)


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
void testSkyAmbientGPU()
{
    BEGIN("ambiente del cielo: direccional y con paridad CPU/GPU");

    // Un cielo azul arriba y un suelo oscuro abajo (lo que da `SkyLight` de día). Los mismos
    // coeficientes van a la CPU y a la GPU; por debajo de la saturación del target de 8 bits.
    const Haruka::Core::SkySH sh = Haruka::Core::skyHemisphereSH(glm::vec3(0.45f, 0.62f, 0.90f),
                                                                  glm::vec3(0.06f, 0.07f, 0.05f));
    struct SHUBO { float c[9][4]; } u{};
    for (int i = 0; i < 9; ++i) {
        u.c[i][0] = sh.coef[i].x; u.c[i][1] = sh.coef[i].y; u.c[i][2] = sh.coef[i].z;
    }
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    const std::string sd = Haruka::Renderer::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "tests/rhitest_fullscreen.vert", fs = sd + "tests/rhitest_skysh.frag";
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
    const glm::vec3 cpuZen = Haruka::Core::skyAmbientEval(sh,  up, up);
    const glm::vec3 cpuNad = Haruka::Core::skyAmbientEval(sh, -up, up);
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
/// Recursos del prop sintetico. Se devuelven cuando se dibuja dentro de un pase ajeno: en Vulkan los
/// comandos aun no se han ejecutado al volver, asi que destruirlos ahi seria un uso-despues-de-liberar.
/// El llamador los destruye DESPUES de `endFrame`.
struct PropRes { PipelineHandle pipe; BufferHandle pf, pp, vb, ib; TextureHandle t[5]; };

/// @param into  si no es nulo, graba el draw en ESE contexto (sin abrir frame ni leer pixeles) y
///              devuelve los recursos en `keep` para que el llamador los destruya tras `endFrame`.
static bool propLitPixel(const float sunDir[3], uint8_t out[4],
                         Context* into = nullptr, PropRes* keep = nullptr)
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

    // ⚠️ 96 B, como el bloque `PropParams` del shader (viento, PBR, ORIGEN RELATIVO y perspectiva
    // aerea). Con 32 B el shader leia `u_originRel`/`u_aerial` FUERA del buffer: Mesa devuelve 0 y
    // el test pasaba; NVIDIA/OpenGL devuelve basura y el prop no escribia pixeles o salia negro
    // (10 de 12 angulos a 0). Misma clase de fallo que el `CloudUBO` de 304 B contra 320.
    struct PropParams { float wind[3]; float time; float matPBR[4]; float originRel[4]; float material[4];
                        float weather[4]; float flags[4]; } pp{};
    static_assert(sizeof(PropParams) == 96, "PropParams del banco desincronizado del shader (lib/prop_params.glsl)");
    pp.matPBR[0] = 0.0f;   // metallic
    pp.matPBR[1] = 0.6f;   // roughness
    pp.matPBR[2] = 1.0f;   // ao
    pp.matPBR[3] = 0.0f;   // SIN texturas: manda el escalar (ver la nota de arriba)
    pp.flags[1]  = 1.0f;   // rim     (prop_inst.frag: la ganancia viaja en el UBO; la obra usa 0,35)
    pp.flags[2]  = 1.0f;   // especular (la obra usa 0)
    pp.material[0] = 1.0f; // facetado (low-poly), como la vegetación del motor
    BufferHandle ppUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pp), &pp, BufferMemory::Dynamic);

    const uint8_t grey[4] = { 128, 128, 128, 255 };
    TextureDesc td; td.width = td.height = 1; td.format = Format::RGBA8;
    td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge; td.initialData = grey;
    TextureHandle t0 = g_dev->createTexture(td), t1 = g_dev->createTexture(td),
                  t2 = g_dev->createTexture(td), t3 = g_dev->createTexture(td),
                  t4 = g_dev->createTexture(td);

    // Malla: un triángulo con normal hacia +Y (mira al cielo), color gris. MISMO layout empaquetado
    // que `PropVertex` (PropSystem.cpp, 32 B): normal en 10/10/10/2 y color RGBA8 con la escala en el
    // alfa. Con el layout viejo de 48 B el shader lee la normal y el color de bytes que no son suyos.
    struct V { float p[3]; uint32_t n, c; float uv[2]; float part; };
    static_assert(sizeof(V) == 32, "V debe casar con PropVertex");
    const uint32_t nUp   = glm::packSnorm3x10_1x2(glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const uint32_t grey5 = glm::packUnorm4x8(glm::vec4(0.5f, 0.5f, 0.5f, 0.25f));   // escala 1
    const V verts[3] = {
        { {-1,-1,0}, nUp, grey5, {0,0}, 0 },
        { { 3,-1,0}, nUp, grey5, {1,0}, 0 },
        { {-1, 3,0}, nUp, grey5, {0,1}, 0 },
    };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);

    // UNA instancia: identidad, color blanco, escala 1, sin partes rotas. El MISMO struct y el MISMO
    // layout que el motor (`InstanceDataFloat` + `appendInstanceLayout`): un banco con su propia copia
    // del layout fue lo que dejó de casar con el shader al compactar la instancia.
    using Haruka::Renderer::InstanceDataFloat;
    const InstanceDataFloat inst = InstanceDataFloat::make(glm::vec3(0.0f), glm::quat(1, 0, 0, 0),
                                                           glm::vec3(1.0f), glm::vec3(1.0f));
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Vertex, sizeof(inst), &inst, BufferMemory::Static);

    const std::string sd = Haruka::Renderer::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "prop_inst.vert", fs = sd + "prop_inst.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0,  0,  Format::RGB32F, 0 }, { 1,  12, Format::RGB10A2_SNORM, 0 },
        { 2,  16, Format::RGBA8,  0 }, { 9,  20, Format::RG32F,  0 },
        { 11, 28, Format::R32F,   0 },
    };
    pd.vertexLayout.rates = { InputRate::Vertex };
    // El binding 1 avanza POR INSTANCIA, no por vértice: es lo que hace que una malla se
    // dibuje N veces con N transformaciones distintas.
    Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);
    pd.topology   = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false;
    pd.cull       = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    bool ok = valid(pipe);

    if (ok && into) {
        into->bindPipeline(pipe);
        into->bindUniformBuffer(0, pfUbo);
        into->bindUniformBuffer(6, ppUbo);
        into->bindTexture(0, t0); into->bindTexture(1, t1); into->bindTexture(2, t2);
        into->bindTexture(3, t3); into->bindTexture(4, t4);
        into->bindVertexBuffer(vb, 0);
        into->bindVertexBuffer(ib, 1);
        into->draw(3, 0, 1);
        if (keep) *keep = PropRes{ pipe, pfUbo, ppUbo, vb, ib, { t0, t1, t2, t3, t4 } };
        return true;                      // NO se destruye: lo hace el llamador tras endFrame
    }
    if (ok) {
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        for (int frame = 0; frame < 1; ++frame) {
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
    }

    if (valid(pipe)) g_dev->destroy(pipe);
    for (BufferHandle b : { pfUbo, ppUbo, vb, ib }) g_dev->destroy(b);
    for (TextureHandle t : { t0, t1, t2, t3, t4 }) g_dev->destroy(t);
    return ok;
}

// ================================================================================================
// EL PROP CONTRA LA LUZ, BARRIDO COMPLETO — y el mismo en los dos backends
//
// `testPropLighting` mira DOS puntos: sol de frente y sol detras. Eso dice que la luz "llega", pero no
// COMO responde: una respuesta plana, una invertida y una correcta pasan las tres si los extremos
// salen bien. Y "los props se ven quemados o sin luz" es justamente una queja sobre la FORMA de esa
// respuesta, no sobre sus extremos.
//
// Aqui se barre el sol por 12 angulos alrededor del objeto y se saca la curva de luminancia. Lo que
// se comprueba es lo que una curva permite y dos puntos no:
//   · que sea MONOTONA de la luz de frente a la de espalda (sin escalones raros),
//   · que el maximo caiga donde apunta la luz y no en otro sitio,
//   · que no sature (quemado) ni se quede pegada a un valor (sin luz),
//   · y que las DOS curvas, GL y Vulkan, sean la MISMA.
//
// El shader de props no tiene luces puntuales —solo sol direccional y luna, ver `PerFrameData`—, asi
// que el barrido va sobre la direccion del sol, que es la luz que ese pase entiende.
void testPropLightSweep()
{
    BEGIN("props: la respuesta a la LUZ, barrida entera (no solo los extremos)");

    const int W = 256, H = 256;
    const int N = 12;
    std::vector<double> curva; curva.reserve(N);
    std::vector<uint8_t> primera;

    for (int i = 0; i < N; ++i) {
        const double a = 2.0 * 3.14159265358979 * (double)i / (double)N;
        const float sun[3] = { (float)std::sin(a), (float)std::cos(a), 0.35f };

        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
        PropRes keep{}; bool ok = false;
        if (Context* c = g_dev->beginFrame()) {
            ClearValues cv;
            cv.clearColor = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.clearDepth = true; cv.depth = 0.0f;
            c->beginRenderPass({}, cv);
            uint8_t dummy[4];
            ok = propLitPixel(sun, dummy, c, &keep);
            c->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        }
        if (ok) {   // los comandos ya se ejecutaron: ahora si se puede liberar
            if (valid(keep.pipe)) g_dev->destroy(keep.pipe);
            for (BufferHandle b : { keep.pf, keep.pp, keep.vb, keep.ib }) if (valid(b)) g_dev->destroy(b);
            for (TextureHandle t : keep.t) if (valid(t)) g_dev->destroy(t);
        }
        if (!ok) { CHECK(false, "el prop se dibuja en cada angulo"); return; }

        // Solo los pixeles del PROP: el fondo es negro puro, asi que cualquier cosa con luz cuenta.
        double acc = 0.0; size_t n = 0;
        for (size_t k = 0; k + 3 < px.size(); k += 4) {
            const double L = px[k] * 0.299 + px[k+1] * 0.587 + px[k+2] * 0.114;
            if (L > 2.0) { acc += L; ++n; }
        }
        curva.push_back(n ? acc / (double)n : 0.0);
        if (i == 0) primera = px;
    }

    std::printf("    luminancia del prop segun de donde viene el sol (12 angulos):\n      ");
    for (double v : curva) std::printf("%6.1f", v);
    std::printf("\n      ");
    const double mx = *std::max_element(curva.begin(), curva.end());
    for (double v : curva) std::printf("%6s", mx > 0 ? (v > mx * 0.85 ? "###" : (v > mx * 0.5 ? "##" : (v > mx * 0.2 ? "#" : "."))) : ".");
    std::printf("\n");

    const double mn = *std::min_element(curva.begin(), curva.end());
    const size_t iMax = (size_t)(std::max_element(curva.begin(), curva.end()) - curva.begin());
    std::printf("      max %.1f (angulo %zu de %d) · min %.1f · recorrido %.2fx\n",
                mx, iMax, N, mn, mn > 0.01 ? mx / mn : 0.0);

    recordShot("prop.sol frontal", W, H, primera);

    CHECK(mx > 20.0, "el prop se ILUMINA con el sol de frente (si no, no hay nada que medir)");
    CHECK(mx < 250.0, "y NO se quema: el maximo no satura a blanco");
    CHECK(mn < mx * 0.7, "la respuesta a la luz tiene RECORRIDO (no es plana: eso seria 'sin luz')");
    CHECK(mn > 1.0, "y a contraluz no se apaga del todo: le llega la ambiente");
}

void testPropLighting()
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
// LA ESCENA COMPLETA: terreno Y un prop, en el MISMO pase y con la MISMA luz
//
// El banco tenia `testTerrainLighting` y `testPropLighting`, pero cada uno por su lado. El sintoma que
// pidio este test es justamente el que no se ve por separado: *"el sombreado y color de props y
// terreno se ve como quemado o sin luz"* — o sea, los dos dibujados a la vez y uno de ellos sin
// responder al sol.
//
// Lo que se mide es la RESPUESTA A LA LUZ de cada uno por separado dentro de la misma imagen: se
// dibuja con el sol de frente y con el sol al otro lado, y se compara la luminancia del terreno con la
// del prop. Un prop que sale igual de claro con el sol delante que detras es "blanco de noche", y eso
// aqui es un numero, no una impresion.
//
// ⚠️ CONTRAPRUEBAS: (1) los dos tienen que DIBUJAR algo —si el prop no escribe pixeles, "no responde a
// la luz" seria trivialmente cierto—; (2) el terreno TIENE que oscurecerse al girar el sol, porque si
// no se oscurece nada la escena entera esta pegada y el test no distingue un fallo del prop.
void testSceneTerrainAndProp()
{
    BEGIN("escena: terreno + prop en el mismo pase, los dos con luz");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase de terreno"); return;
    }

    // Material sintetico para que el terreno pase por su camino de sombreado real, no por el plano.
    const uint32_t TS = 64, LAYERS = 4;
    std::vector<uint8_t> pix((size_t)TS * TS * LAYERS * 4, 200);
    TextureDesc atd; atd.width = atd.height = TS; atd.layers = LAYERS;
    atd.format = Format::RGBA8; atd.filter = Filter::Linear; atd.wrap = Wrap::Repeat;
    atd.initialData = pix.data();
    TextureHandle albedo = g_dev->createTexture(atd);
    TextureHandle normal = g_dev->createTexture(atd);
    struct MatUBO { float v[64]; } mu{};
    for (float& f : mu.v) f = 0.5f;
    BufferHandle matUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(mu), &mu, BufferMemory::Dynamic);
    CHECK(valid(albedo) && valid(normal) && valid(matUBO), "material sintetico del terreno creado");

    const int cw = 256, ch = 256;
    const double fovY = glm::radians(60.0);
    const double radPerPx = fovY / (double)ch;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, 1.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::dvec3 fwd = -up0;                       // al nadir: el terreno llena el cuadro
    const glm::dvec3 rgh = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::Tools::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)cw / (float)ch);
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(
                              glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(glm::cross(rgh, fwd)))));

    // El prop va con proyeccion identidad (asi lo monta `propLitPixel`), asi que cae en el CENTRO del
    // cuadro. El terreno ocupa el resto. Se muestrean dos zonas disjuntas: centro = prop, esquina =
    // terreno. No es una escena "bonita"; es una escena donde los dos caminos de sombreado corren a la
    // vez y se pueden medir por separado, que es lo que hace falta.
    auto frame = [&](const float sun[3], std::vector<uint8_t>& px) {
        Haruka::World::Terrain::TerrainNodeRenderer::Shade sh;
        sh.albedo = albedo; sh.normal = normal; sh.materialUBO = matUBO;
        sh.tiling = 8.0f; sh.shoreLayer = 0;
        sh.lightDir = glm::vec3(sun[0], sun[1], sun[2]);
        sh.on = true;
        r.setShade(sh);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        px.assign((size_t)cw * ch * 4, 0xAA);
        PropRes keep{}; bool drewProp = false;
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            uint8_t dummy[4];
            drewProp = propLitPixel(sun, dummy, ctx, &keep);   // el prop, en el MISMO pase
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
        if (drewProp) {                                  // ya ejecutado: ahora si se puede destruir
            if (valid(keep.pipe)) g_dev->destroy(keep.pipe);
            for (BufferHandle b : { keep.pf, keep.pp, keep.vb, keep.ib }) if (valid(b)) g_dev->destroy(b);
            for (TextureHandle t : keep.t) if (valid(t)) g_dev->destroy(t);
        }
        return drewProp;
    };
    auto lum = [&](const std::vector<uint8_t>& px, int x0, int y0, int x1, int y1) {
        double acc = 0.0; int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const size_t k = ((size_t)y * cw + x) * 4;
                acc += (px[k] * 0.299 + px[k+1] * 0.587 + px[k+2] * 0.114); ++n;
            }
        return n ? acc / n : 0.0;
    };

    const float sunA[3] = {  0.4f,  0.8f,  0.3f };     // de frente
    const float sunB[3] = { -0.4f, -0.8f, -0.3f };     // al otro lado
    std::vector<uint8_t> imgA, imgB;
    const bool okA = frame(sunA, imgA);
    const bool okB = frame(sunB, imgB);
    CHECK(okA && okB, "los dos frames se dibujan (terreno + prop)");
    if (!okA || !okB) { g_dev->destroy(albedo); g_dev->destroy(normal); g_dev->destroy(matUBO); return; }

    const double propA = lum(imgA, cw/2 - 12, ch/2 - 12, cw/2 + 12, ch/2 + 12);
    const double propB = lum(imgB, cw/2 - 12, ch/2 - 12, cw/2 + 12, ch/2 + 12);
    const double terA  = lum(imgA, 4, 4, 44, 44);
    const double terB  = lum(imgB, 4, 4, 44, 44);

    std::printf("    luminancia con el sol de FRENTE / al OTRO LADO\n");
    std::printf("      terreno (esquina) %6.1f -> %6.1f   (cae %5.1f %%)\n",
                terA, terB, terA > 0 ? 100.0 * (terA - terB) / terA : 0.0);
    std::printf("      prop    (centro)  %6.1f -> %6.1f   (cae %5.1f %%)\n",
                propA, propB, propA > 0 ? 100.0 * (propA - propB) / propA : 0.0);

    // ── ¿SUAVE O PLANO? EL NUMERO DE COLORES DISTINTOS LO DICE ──────────────────────────────────
    //
    // Andoni: *"toda la cara/triangulo se dibuja con la misma iluminacion o color, es algo de Vulkan
    // no de OpenGL"*. Con sombreado SUAVE la luz varia dentro de cada triangulo y la imagen tiene
    // muchos valores; con sombreado PLANO cada triangulo es un color constante y el recuento se
    // desploma. La media de luminancia —lo unico que media este test— no distingue las dos cosas: un
    // triangulo plano y uno con degradado pueden tener la MISMA media.
    //
    // Se cuenta sobre el terreno, que es donde hay miles de triangulos con normales distintas. La
    // cifra se guarda por backend en el careo, que es donde se vera si uno tiene 20 veces menos.
    auto distintos = [&](const std::vector<uint8_t>& px) {
        std::unordered_set<uint32_t> u;
        for (int y = 4; y < ch - 4; ++y)
            for (int x = 4; x < cw / 3; ++x) {          // franja de terreno, lejos del prop
                const size_t k = ((size_t)y * cw + x) * 4;
                u.insert(((uint32_t)px[k] << 16) | ((uint32_t)px[k+1] << 8) | px[k+2]);
            }
        return u.size();
    };
    const size_t nA = distintos(imgA);
    std::printf("      colores distintos en el terreno: %zu  (pocos = cada triangulo de un color = PLANO)\n",
                nA);

    recordShot("escena.terreno+prop sol A", cw, ch, imgA);
    recordShot("escena.terreno+prop sol B", cw, ch, imgB);

    // ── ¿RESPONDEN IGUAL A LA HORA DEL DIA? BARRIDO DEL SOL ─────────────────────────────────────
    //
    // Dos puntos (sol delante / sol detras) no distinguen "responde menos" de "no responde": los dos
    // caian ~70 %. Lo que Andoni ve —*"los props se ven quemados o sin luz"*, arboles BLANCOS sobre un
    // suelo oscuro al atardecer— es lo que pasa EN MEDIO, con el sol rasante.
    //
    // `prop_inst.frag` usa un modelo cel: `litCol = baseColor * (0.80 + 0.25 * sunLightColor)`. Ese
    // **0,80 es un suelo fijo que no depende del sol**; el terreno usa `max(dot(n,L), 0)`, que baja
    // continuamente. El propio comentario del shader dice que la intencion era "que los dos responden
    // igual a la hora del dia" — esto mide si lo hacen.
    std::printf("      barrido del sol (elevacion) · luminancia normalizada a su maximo\n");
    std::printf("        elev    terreno   prop     prop/terreno\n");
    double worstRatio = 0.0;
    {
        double terMax = 0.0, propMax = 0.0;
        std::vector<std::pair<double,double>> curva;
        for (int i = 0; i <= 6; ++i) {
            const float e = (float)(1.0 - 2.0 * (double)i / 6.0);     // +1 (cenit) .. -1 (bajo tierra)
            const float sun[3] = { 0.30f, e, 0.20f };
            std::vector<uint8_t> img;
            if (!frame(sun, img)) continue;
            const double t = lum(img, 4, 4, 44, 44);
            const double p = lum(img, cw/2 - 12, ch/2 - 12, cw/2 + 12, ch/2 + 12);
            terMax = std::max(terMax, t); propMax = std::max(propMax, p);
            curva.emplace_back(t, p);
        }
        for (size_t i = 0; i < curva.size(); ++i) {
            const double tn = terMax > 0 ? curva[i].first  / terMax  : 0.0;
            const double pn = propMax > 0 ? curva[i].second / propMax : 0.0;
            const double r  = tn > 0.02 ? pn / tn : 0.0;
            if (tn > 0.02) worstRatio = std::max(worstRatio, r);
            // ⚠️ `%s` con un `std::string` temporal es UB y salia basura en pantalla. Literal.
            std::printf("        %+5.2f   %6.3f   %6.3f   %s\n",
                        1.0 - 2.0 * (double)i / 6.0, tn, pn,
                        (tn > 0.02 && r > 1.5) ? "  <- el prop NO baja con el sol" : "");
        }
        std::printf("      peor desajuste prop/terreno: %.2fx  (1,00 = responden igual)\n", worstRatio);
    }

    g_dev->destroy(albedo); g_dev->destroy(normal); g_dev->destroy(matUBO);

    CHECK(propA > 8.0, "CONTRAPRUEBA: el prop DIBUJA (si no escribe pixeles, no probaria nada)");
    CHECK(terA  > 8.0, "CONTRAPRUEBA: el terreno DIBUJA");
    CHECK(terA > terB * 1.05 || terB > terA * 1.05,
          "CONTRAPRUEBA: el TERRENO responde al sol (si nada cambia, el test no distingue nada)");
    CHECK(propA < 250.0 && propB < 250.0, "el prop no esta QUEMADO (saturado a blanco) con ningun sol");
    CHECK(nA > 200, "el sombreado del terreno es SUAVE: la luz varia dentro de los triangulos");
}

// ================================================================================================
// ARENA DE INSTANCIAS: memoria host-visible por frame.
//
// Era un anillo fijo: 16 entradas × maxInstances (20 000) × 96 B = 30 MB reservados hiciera falta
// o no, y al pasar de 16 draws por frame (19 prototipos × 3 LOD + sombras) el cursor daba la vuelta
// a mitad de frame y los primeros draws leían las instancias de los últimos (la hierba cercana no
// se dibujaba). Una entrada por draw arreglaba eso pero costaba 22 ms/frame recreando entradas.
// Ahora: UN buffer por frame, sub-asignación lineal con offset. Aquí se mide con el reparto real de
// un frame del juego: 60 draws, la mayoría de decenas o cientos de instancias y tres de miles.
// ================================================================================================
void testInstancingRing()
{
    BEGIN("instancing: arena por frame, sub-asignacion lineal");
    using Haruka::Renderer::GPUInstancing;
    using Haruka::Renderer::InstanceDataFloat;
    Haruka::Renderer::GPUInstancing inst;
    inst.init(20000);
    CHECK(inst.hostBytes() == 0, "init no reserva nada: el arena nace al primer uso");

    std::vector<size_t> lotes;
    for (int i = 0; i < 60; ++i) lotes.push_back(i % 20 == 0 ? 6000 : (i % 5 == 0 ? 400 : 30));
    size_t total = 0; for (size_t n : lotes) total += n;

    auto frame = [&]() {
        inst.beginFrame();
        for (size_t n : lotes) {
            std::vector<Haruka::Renderer::InstanceDataFloat> b(n);
            inst.setInstances(b);
            inst.upload();
        }
    };
    frame(); frame();   // calentamiento: el arena crece dentro del frame hasta que cabe entero
    const auto t0 = std::chrono::steady_clock::now();
    frame();
    const size_t bytes1 = inst.hostBytes();
    frame(); frame();
    const double msPerFrame = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 3.0;
    const size_t bytes3 = inst.hostBytes();
    const size_t fijo = 60u * 20000u * sizeof(Haruka::Renderer::InstanceDataFloat);
    std::printf("    60 draws, %zu instancias (%.1f MB movidos): arena %.1f MB (antes fijo: %.1f MB) · tras 3 frames %.1f MB · %.2f ms/frame de subida\n",
                total, total * sizeof(Haruka::Renderer::InstanceDataFloat) / 1048576.0, bytes1 / 1048576.0, fijo / 1048576.0,
                bytes3 / 1048576.0, msPerFrame);
    CHECK(inst.ringSize() == 60, "60 lotes subidos en el frame");
    CHECK(bytes1 <= total * sizeof(Haruka::Renderer::InstanceDataFloat) * 2, "la reserva no pasa del doble de lo que se mueve");
    CHECK(bytes1 < fijo / 8, "al menos 8x menos que el anillo fijo de 20 000 por entrada");
    CHECK(bytes3 == bytes1, "los frames siguientes no reservan nada mas");
    CHECK(msPerFrame < 8.0, "subir 2,1 MB en 60 lotes baja de 8 ms (con una entrada por draw eran ~22)");
    baselineMetric("instancias.subida_60_lotes_ms", msPerFrame, 1.50, true, true);   // 0,4-0,8 ms de un frame a otro: es ruido de subida
    // Contraprueba: un frame que no cabe crea un arena mayor (el viejo se retira hasta el frame
    // siguiente), y solo entonces. ⚠️ EXACT, NOT DOUBLED (11-10): the arena grows to what is asked
    // for (doubling recreated huge host-visible buffers, 157 -> 278 -> 671 ms measured), so the old
    // "37 040 -> 65 536" no longer applies; what holds is: it grows to hold the frame, not to twice
    // it, and within a frame or two it stops growing.
    lotes[0] = 20000;
    size_t total2 = 0; for (size_t n : lotes) total2 += n;
    frame();
    frame();   // the growing frame sized it for the previous frame's demand: at most one more
    const size_t bytes4 = inst.hostBytes();
    const size_t need2 = total2 * sizeof(Haruka::Renderer::InstanceDataFloat);
    std::printf("    contraprueba: %zu instancias por frame -> arena %.2f MB (lo pedido %.2f MB)\n", total2, bytes4 / 1048576.0, need2 / 1048576.0);
    CHECK(bytes4 > bytes3 && bytes4 >= need2 && bytes4 < need2 * 2, "contraprueba: un frame mayor hace crecer el arena a lo pedido, no al doble");
    frame(); frame(); frame();
    CHECK(inst.hostBytes() == bytes4, "y los frames siguientes ya caben sin crecer");
}

// ================================================================================================
// LA HIERBA EN GPU: un brote se dibuja DONDE debe, y solo el que se pide (GrassSystem).
//
// `prop_burst.vert` no tiene stream de instancia: el brote es el `firstInstance` del comando
// indirecto, leído como `gl_BaseInstanceARB`, y la mata es `gl_InstanceID` MENOS esa base solo en
// Vulkan (donde el backend lo reescribe a `gl_InstanceIndex`, que ya la lleva). Es exactamente el
// tipo de diferencia GL/Vulkan que no da error de compilación: con la resta mal, en un backend se
// lee el brote de otro sitio o una mata fuera de rango y la hierba sale donde no es.
//
// Montaje: dos brotes de UNA mata (celda 0 → la mata cae en el centro exacto), uno a la izquierda y
// otro a la derecha del cuadro (vista y proyección identidad). Se dibuja SOLO el brote 1 con un
// comando indirecto. Tiene que salir la mancha de la derecha y NO la de la izquierda.
// ================================================================================================
void testGrassBurstDraw()
{
    BEGIN("hierba: el brote pedido se dibuja en su sitio (firstInstance, GL y Vulkan)");
    using namespace Haruka::World::Props;
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

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
    pf.sunDir[2] = 1.0f; pf.sunColor[0] = pf.sunColor[1] = pf.sunColor[2] = 1.0f; pf.ambientStrength = 0.5f;
    BufferHandle pfUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pf), &pf, BufferMemory::Dynamic);

    PropParams pp{};
    pp.matPBR = glm::vec4(0.0f, 0.6f, 1.0f, 0.0f);
    pp.flags  = glm::vec4(isVk ? 1.0f : 0.0f, 1.0f, 1.0f, 0.0f);   // y/z = rim y especular, los del juego
    BufferHandle ppUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pp), &pp, BufferMemory::Dynamic);

    const uint8_t grey[4] = { 128, 128, 128, 255 };
    TextureDesc td; td.width = td.height = 1; td.format = Format::RGBA8;
    td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge; td.initialData = grey;
    TextureHandle t[5];
    for (auto& x : t) x = g_dev->createTexture(td);

    // La mata: un triángulo en el plano local XZ (y = 0) alrededor de su raíz, de ±0,15 (en clip).
    const uint32_t nUp = glm::packSnorm3x10_1x2(glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const uint32_t col = glm::packUnorm4x8(glm::vec4(0.9f, 0.9f, 0.9f, 0.25f));
    const PropVertex verts[3] = {
        { {-0.15f, 0.0f, -0.15f}, nUp, col, {0, 0}, 0.0f },
        { { 0.15f, 0.0f, -0.15f}, nUp, col, {1, 0}, 0.0f },
        { { 0.00f, 0.0f,  0.15f}, nUp, col, {0, 1}, 0.0f },
    };
    const uint32_t idx[3] = { 0, 1, 2 };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Index, sizeof(idx), idx, BufferMemory::Static);

    // Dos brotes. La vertical local es +Z (hacia el ojo): la mata se tumba en el plano XY de pantalla.
    MassBurstGpu bursts[2];
    const float zeroCell = 0.0f; uint32_t zeroBits; std::memcpy(&zeroBits, &zeroCell, 4);
    bursts[0].a = glm::vec4(-0.5f, 0.0f, 0.5f, 1.0f); bursts[0].b = glm::vec4(0, 0, 1, 1.0f);
    bursts[0].c = glm::uvec4(11u, 1u, kMassNoState, zeroBits);
    bursts[1].a = glm::vec4( 0.5f, 0.0f, 0.5f, 1.0f); bursts[1].b = glm::vec4(0, 0, 1, 1.0f);
    bursts[1].c = glm::uvec4(22u, 1u, kMassNoState, zeroBits);
    BufferHandle burstBuf = g_dev->createBuffer(BufferUsage::Storage, sizeof(bursts), bursts, BufferMemory::Dynamic);
    const uint32_t stateWord = 0u;
    BufferHandle stateBuf = g_dev->createBuffer(BufferUsage::Storage, sizeof(stateWord), &stateWord, BufferMemory::Dynamic);
    struct Cmd { uint32_t indexCount, instanceCount, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
    const Cmd cmd{ 3u, 1u, 0u, 0, 1u };   // SOLO el brote 1
    BufferHandle cmdBuf = g_dev->createBuffer(BufferUsage::Indirect, sizeof(cmd), &cmd, BufferMemory::Dynamic);

    const std::string sd = Haruka::Renderer::Shader::baseDir() + "shaders/";
    const std::string vs = sd + "prop_burst.vert", fs = sd + "prop_inst.frag";
    PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    appendPropVertexLayout(pd.vertexLayout, false);
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false;
    pd.cull = CullMode::None;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "el pipeline de prop_burst se crea");

    const int W = 256, H = 256;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    if (valid(pipe)) {
        if (Context* c = g_dev->beginFrame()) {
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;
            c->beginRenderPass({}, cv);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, pfUbo);
            c->bindUniformBuffer(6, ppUbo);
            for (int i = 0; i < 5; ++i) c->bindTexture((uint32_t)i, t[i]);
            c->bindStorageBuffer(2, burstBuf);
            c->bindStorageBuffer(3, stateBuf);
            c->bindVertexBuffer(vb, 0);
            c->bindIndexBuffer(ib);
            c->drawIndexedIndirect(cmdBuf, 1, (uint32_t)sizeof(Cmd), 0);
            c->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        }
    }
    // Cuenta píxeles que NO son el clear (magenta) en cada mitad.
    auto painted = [&](int x0, int x1) {
        int n = 0;
        for (int y = 0; y < H; ++y)
            for (int x = x0; x < x1; ++x) {
                const uint8_t* p = &px[((size_t)y * W + x) * 4];
                if (!(p[0] > 200 && p[1] < 60 && p[2] > 200)) ++n;
            }
        return n;
    };
    const int left = painted(0, W / 2), right = painted(W / 2, W);
    std::printf("    pixeles pintados: izquierda (brote 0, no pedido) %d · derecha (brote 1) %d\n", left, right);
    CHECK(right > 200, "el brote 1 se dibuja (a la derecha, donde esta su centro)");
    CHECK(left == 0, "el brote 0 NO se dibuja: el shader lee el brote del firstInstance, no otro");

    if (valid(pipe)) g_dev->destroy(pipe);
    for (BufferHandle b : { pfUbo, ppUbo, vb, ib, burstBuf, stateBuf, cmdBuf }) g_dev->destroy(b);
    for (auto& x : t) g_dev->destroy(x);
}
