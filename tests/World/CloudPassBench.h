#pragma once
// El banco del pase volumetrico de nubes: la preparacion comun (pipeline, UBO, profundidad, el campo
// de cobertura) y los mandos que cada caso mueve. Los casos estan en TestCloud{Pass,Orbit,Formation}GPU.cpp;
// cada test monta su propio banco, asi que el orden de los casos no cambia el resultado.
#include "RHI/Common/RHITestCommon.h"

// ── EL BANCO DEL PASE VOLUMETRICO ──────────────────────────────────────────────────────────────
// Antes era UNA funcion de ~2 200 lineas (`testCloudVolumeDraws`): la preparacion y diez casos que se
// pasaban el estado por variables capturadas — la camara (`ojoA`, `mira`, `upCur`), la losa, los
// modos, `pasos`/`marcos`/`leer`, la profundidad atada. Cada caso tenia que DEVOLVER lo que tocaba
// para no contaminar al siguiente, y no todos lo hacian (el de profundidad dejaba `depthTex` y los
// targets de blit; el de coste, `marcos`/`leer`/`pasos`/`hCur`; el de convergencia, `ojoA = 1030`).
// Y un `return` temprano en un caso se saltaba todos los de detras.
// Ahora cada caso es un test con su PROPIO banco recien montado: el orden no cambia el resultado y un
// caso que aborta no se lleva a los demas. Los casos son metodos para que su codigo no cambie: lo que
// antes era una variable capturada es un miembro.
struct CloudPassBench {
    double R = 0.0; float baseA = 0.0f, topA = 0.0f, eyeA = 0.0f;
    int w = 256, h = 256;
    std::string base; PipelineDesc pd; std::string vs, fs;
    PipelineHandle pipe{};
    float depthClear = 0.0f; TextureDesc dtd; TextureHandle depthTex{};
    // Enganche para el caso (6d): cuando estos dos targets son validos, `render` reproduce el camino
    // de profundidad DE LA PARTIDA (ver `sceneDepth`).
    RenderPassHandle preBlitSrc{}, preBlitDst{};
    // Mandos del bloque de COSTE: pasos que se le piden al shader, cuantos frames dibuja `render` y si
    // hace readback (que es un punto de sincronizacion y falsearia el cronometro).
    float pasos = 64.0f; int marcos = 3; bool leer = true;
    TextureHandle coverTex{}, hiDummy{}, coverBound{};
    // El "horneado anterior" (bindings 3/4) y el fundido: por defecto el mismo y peso 1 (sin fundir).
    TextureHandle coverPrevBound{}, hiPrevBound{};
    float blendCur = 1.0f;
    float windOffX = 0.0f, windOffY = 0.0f;   // la deriva del campo (unidades), para el caso MOVIMIENTO
    float aerialLm = 60000.0f;                // perspectiva aerea (m): aire limpio; SILUETAS la acorta
    struct CloudUBO {
        float invVP[16]; float planetC[4]; float slab[4]; float sun[4];
        float sunColor[4]; float wind[4]; float misc[4];
        // Embudos: ninguno en el banco (a cero). El layout tiene que casar con `CloudParams`.
        float vortexPos[4][4]; float vortexInfo[4][4]; float vortexN[4];
        // ⚠️ FALTABA. `CloudParams` crecio a 320 B con `u_aerial` y el banco seguia en 304: el shader
        // leia `u_aerial` FUERA del buffer (lo que hubiera). Con L = 60 km (aire limpio) el tope de
        // marcha a 4 L son 240 km y no toca ninguna medida del banco.
        float aerial[4];
        float blend[4];    // x = 1: el banco solo tiene UN horneado (bindings 3/4 = el mismo)
        float vortexEmit[4][4];   // embudos del JUEGO (hechizos): ninguno en el banco (a cero)
    };
    static_assert(sizeof(CloudUBO) == 400, "CloudUBO del banco desincronizado de CloudParams");
    BufferHandle ubo{};
    const glm::dvec3 up0{0.0, 1.0, 0.0};
    glm::dvec3 pcRel{0.0}, fwd{1.0, 0.0, 0.0};
    double fovY = 0.0; float aspect = 1.0f, fCot = 1.0f;
    glm::mat4 proj{0.0f}, view{1.0f}, invVP{1.0f};
    // Altitud y direccion parametrizables (mirar desde ORBITA, la losa de la partida, el target reducido).
    float ojoA = 0.0f; glm::dvec3 mira{1.0, 0.0, 0.0};
    float slabBase = 0.0f, slabTop = 0.0f; int hCur = 256;
    int modosCur = 0;                          // HARUKA_CLOUD_MODES por caso (0 = el del entorno)
    glm::dvec3 upCur{0.0, 1.0, 0.0}; glm::vec3 sunCur{0.0f, 1.0f, 0.0f};
    bool ok = false;                           // false si el pipeline no se pudo crear

    CloudPassBench() {
        R = 6371000.0;
        baseA = 1200.0f; topA = 2600.0f;     // losa de 1,4 km, del orden de lo que da el clima
        eyeA  = 300.0f;                      // ojo BAJO la base: se mira la capa desde abajo

        uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
        w = (uw > 0) ? (int)uw : 256; h = (uh > 0) ? (int)uh : 256;

        base = Haruka::Renderer::Shader::baseDir();

        vs = base + "shaders/cloud_vol.vert"; fs = base + "shaders/cloud_vol.frag";
        pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
        pd.topology = PrimitiveTopology::Triangles;
        pd.depth.test = false; pd.depth.write = false;
        pd.blend.enable = true;
        pipe = g_dev->createPipeline(pd);
        CHECK(valid(pipe), "pipeline del pase volumetrico de nubes creado");
        if (!valid(pipe)) return;

        // Profundidad de escena VACIA: con reversed-Z el vacio es 0,0 y reproyecta lejisimos, asi que el
        // corte contra la escena no recorta nada. Es el caso "cielo despejado delante".

        dtd.width = 1; dtd.height = 1; dtd.format = Format::R32F;
        dtd.filter = Filter::Nearest; dtd.wrap = Wrap::ClampToEdge; dtd.mipmaps = false;
        dtd.initialData = &depthClear;
        depthTex = g_dev->createTexture(dtd);

        // ⚠️ EL CAMPO DE COBERTURA (binding 1), AUNQUE AQUI NO SE USE. Desde que la cobertura viaja por
        // direccion, el shader declara ese sampler — y en Vulkan un descriptor SIN ATAR es INDEFINIDO,
        // no ceros. Dejarlo suelto es el fallo que ya dejo el agua del pase de nodos sin dibujar un
        // pixel. Con un 1x1 el shader lo detecta por `textureSize <= 1` y cae al escalar de siempre, que
        // es justo el camino que este test quiere medir.
        // ⚠️ EL CIELO VA EN CUATRO CANALES: (cobertura, base m, techo m, precip) por direccion — ver
        // `WeatherSystem::bakeSky`. Con un R32F el shader leeria base = 0 y techo = 1 y no dibujaria
        // NADA. El banco expande su campo de cobertura a la losa del test para poder seguir midiendo
        // lo mismo que media: la cobertura por direccion.

        coverTex = makeSkyTex(std::vector<float>{1.0f}, 1, 1, baseA, topA,
                              Filter::Nearest, Wrap::ClampToEdge);
        // Binding 2: las capas altas. 1x1 = "no hay" (el shader lo detecta por tamaño).
        hiDummy = makeSkyTex(std::vector<float>{0.0f}, 1, 1, 0.0f, 0.0f,
                             Filter::Nearest, Wrap::ClampToEdge);
        coverBound = coverTex;   // intercambiable: el caso (7) mete un campo de verdad


        ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(CloudUBO), nullptr,
                                  BufferMemory::Dynamic);

        // Marco: la camara en el origen relativo, el planeta debajo. Se mira al HORIZONTE, que es donde
        // esta casi toda el area de cielo y donde el bug del muestreo vivia.

        pcRel = -up0 * (R + (double)eyeA);       // centro del planeta relativo al ojo
        fovY = 60.0 * 3.14159265358979 / 180.0;
        aspect = (float)w / (float)h;
        fCot = 1.0f / std::tan((float)fovY * 0.5f);
        proj = glm::mat4(0.0f);
        proj[0][0] = fCot / aspect; proj[1][1] = fCot;
        proj[2][3] = -1.0f;         proj[3][2] = 1.0f;            // reversed-Z, lejano infinito
        view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
        invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));

        // Altitud y direccion parametrizables: hace falta para mirar desde ORBITA (ver el caso 6).
        ojoA = eyeA;
        mira = fwd;
        slabBase = baseA; slabTop = topA;   // la losa, para poder reproducir la de la partida
        hCur = h;                           // alto del target que se dibuja (el reducido lo cambia)
        upCur = up0;                       // la vertical del ojo (el caso 6c' se va del polo)
        sunCur = glm::normalize(glm::vec3(0.4f, 0.7f, 0.2f));   // el Sol (el caso de LUZ lo mueve)
        ok = true;
    }
    ~CloudPassBench() {
        if (valid(pipe)) g_dev->destroy(pipe);
        if (valid(ubo)) g_dev->destroy(ubo);
        if (valid(depthTex)) g_dev->destroy(depthTex);
        if (valid(coverTex)) g_dev->destroy(coverTex);
        if (valid(hiDummy)) g_dev->destroy(hiDummy);   // antes no se liberaba nunca
    }
    CloudPassBench(const CloudPassBench&) = delete;
    CloudPassBench& operator=(const CloudPassBench&) = delete;

    TextureHandle makeSkyTex(const std::vector<float>& cover, int W, int H, float base, float top,
                              Filter filt, Wrap wrap) {
            std::vector<float> rgba((size_t)W * H * 4);
            for (size_t i = 0; i < (size_t)W * H; ++i) {
                rgba[i * 4 + 0] = cover[i]; rgba[i * 4 + 1] = base; rgba[i * 4 + 2] = top; rgba[i * 4 + 3] = 0.0f;
            }
            TextureDesc d; d.width = W; d.height = H; d.format = Format::RGBA32F;
            d.filter = filt; d.wrap = wrap; d.mipmaps = false; d.initialData = rgba.data();
            return g_dev->createTexture(d);
        }

    // El relleno del UBO, aparte: el caso de coste monta su propia secuencia de pases y necesita los
    // MISMOS uniformes que el resto, no una copia que pueda divergir.
    void fillUBO(float cover) {
            const glm::dvec3 pcR = -upCur * (R + (double)ojoA);
            // "Arriba" de la camara = la vertical del OJO. Antes era `cross(mira, +Y)`, que es un vector
            // HORIZONTAL: la camara salia rodada 90 grados y el horizonte (y el canto de cada capa) se
            // pintaba como una franja VERTICAL en los PNG y en el detector de cortes. Solo al mirar al
            // cenit/nadir (mira || vertical) hace falta otro eje.
            const glm::mat4 view2 = glm::lookAt(glm::vec3(0.0f), glm::vec3(mira),
                                                glm::vec3(std::abs(glm::dot(mira, upCur)) > 0.99
                                                          ? glm::dvec3(1, 0, 0) : upCur));
            const glm::mat4 iVP = glm::inverse(proj * glm::mat4(glm::mat3(view2)));
            CloudUBO u{};
            std::memcpy(u.invVP, &iVP[0][0], sizeof(u.invVP));
            u.planetC[0] = (float)pcR.x; u.planetC[1] = (float)pcR.y; u.planetC[2] = (float)pcR.z;
            u.planetC[3] = (float)R;
            u.slab[0] = slabBase; u.slab[1] = slabTop; u.slab[2] = cover; u.slab[3] = 0.0f;
            // Sol alto y de lado: si estuviera en el cenit exacto, un volumen y una calcomania se verian
            // igual y el careo de sombreado no mediria nada.
            const glm::vec3 sun = sunCur;
            u.sun[0] = sun.x; u.sun[1] = sun.y; u.sun[2] = sun.z; u.sun[3] = sun.y;
            u.sunColor[0] = u.sunColor[1] = u.sunColor[2] = 1.0f; u.sunColor[3] = 1.0f;
            u.wind[0] = windOffX; u.wind[1] = windOffY; u.wind[2] = 7.0f; u.wind[3] = ojoA;
            u.misc[0] = 1.0f;                       // atmosfera: dentro
            u.misc[1] = pasos;                      // pasos, el mismo `kCloudSteps` del motor
            u.misc[2] = Haruka::World::WeatherSystem::kFieldScale;
            u.misc[3] = 0.008f;                     // extincion por metro, gemela de kCloudExtinction
            u.vortexN[2] = (float)fovY / (float)hCur; // angulo de un pixel del target que se dibuja
            u.aerial[0] = 1.0f / aerialLm; u.aerial[1] = 1.0f;
            u.aerial[2] = ojoA; u.aerial[3] = 4000.0f;   // altitud del ojo y escala de altura del aire, como el motor
            u.blend[0] = blendCur;
            {   // los bits altos (>= 8) del entorno son DIAGNOSTICO del shader y se conservan por caso
                const int env = std::getenv("HARUKA_CLOUD_MODES") ? std::atoi(std::getenv("HARUKA_CLOUD_MODES")) : 7;
                u.vortexN[3] = (float)((modosCur > 0) ? (modosCur | (env & ~7)) : env);
            }
            g_dev->updateBuffer(ubo, 0, sizeof(u), &u);
        }

    void render(float cover, std::vector<uint8_t>& px) {
            fillUBO(cover);
            px.assign((size_t)w * h * 4, 0xAA);
            const bool isVk = (g_dev->backend() == Backend::Vulkan);
            for (int f = 0; f < marcos; ++f) {
                Context* ctx = g_dev->beginFrame();
                if (!ctx) break;
                if (valid(preBlitDst)) {
                    // El gemelo exacto de la partida: la escena deja su profundidad en su target (aqui
                    // VACIA, que con reversed-Z es 0,0 y no recorta nada) y el pase de nubes se la lleva
                    // a una copia propia con `blitDepth`.
                    ClearValues ce; ce.clearColor = true; ce.clearDepth = true;
                    ce.color[0] = ce.color[1] = ce.color[2] = 0.0f; ce.color[3] = 1.0f; ce.depth = 0.0f;
                    ctx->beginRenderPass(preBlitSrc, ce);
                    ctx->endRenderPass();
                    ctx->blitDepth(preBlitSrc, preBlitDst, w, h);
                }
                ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
                ctx->beginRenderPass({}, cv);
                ctx->bindPipeline(pipe);
                ctx->bindUniformBuffer(5, ubo);
                ctx->bindTexture(0, depthTex);
                ctx->bindTexture(1, coverBound);
                ctx->bindTexture(2, hiDummy);   // sin capas altas: el banco mide el cumulo
                ctx->bindTexture(3, valid(coverPrevBound) ? coverPrevBound : coverBound);   // "anterior"
                ctx->bindTexture(4, valid(hiPrevBound) ? hiPrevBound : hiDummy);
                ctx->draw(3, 1);
                ctx->endRenderPass();
                const bool ultimo = leer && (f == marcos - 1);
                if (!isVk && ultimo) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                g_dev->endFrame();
                pumpWindowEvents();
                if (isVk && ultimo) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            }
        }

        // GRANO: media de |pixel - media 3x3| sobre los pixeles con nube (canal R). Una nube lisa da
        // unidades; un dither de muestreo (un valor distinto por pixel) da decenas. Es la medida de
        // "se ve como pixeles" que los pixeles AISLADOS no capturan cuando el grano es denso.
        // Devuelve el PORCENTAJE de pixeles con nube que se separan mas de 10/255 de su media 3x3: en
        // una nube lisa es ~0; en un dither, la mitad. (La media de |diff| no sirve: un cumulo grande y
        // liso la diluye aunque al lado haya una mancha entera de grano.)
    double grano(const std::vector<uint8_t>& px) {
            double acc = 0.0; size_t n = 0, bad = 0;
            for (int y = 1; y < h - 1; ++y)
                for (int x = 1; x < w - 1; ++x) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    if (px[k] <= 12) continue;
                    double m = 0.0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            m += px[((size_t)(y + dy) * w + (x + dx)) * 4];
                    m /= 9.0;
                    const double d = std::fabs((double)px[k] - m);
                    acc += d; ++n; if (d > 10.0) ++bad;
                }
            (void)acc;
            return n ? 100.0 * (double)bad / (double)n : 0.0;
        }

    void draws();
    void orbit();
    void highLayers();
    void sceneDepth();
    void flatTop();
    void motion();
    void convergence();
    void cost();
    void coverageByDir();
    void orbitView();
};
