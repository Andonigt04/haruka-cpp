/**
 * @file RHITestMain.cpp
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
#include "RHI/Common/RHITestCommon.h"

static void baselineWriteIfAsked();
static void baselineReset() {   // por backend: otro fichero de linea base
    baselineWriteIfAsked();
    g_baseline.clear(); g_baselineNew.clear(); g_baselineLoaded = false;
}
static void baselineWriteIfAsked() {
    if (!g_baselineLoaded || !g_baselineOk || !std::getenv("HARUKA_BASELINE_UPDATE")) return;
    for (int perGpu = 0; perGpu < 2; ++perGpu) {
        std::map<std::string, double> merged;
        const std::string pre = perGpu ? "gpu:" : "any:";
        for (auto& kv : g_baseline)    if (kv.first.rfind(pre, 0) == 0) merged[kv.first.substr(4)] = kv.second;
        for (auto& kv : g_baselineNew) if (kv.first.rfind(pre, 0) == 0) merged[kv.first.substr(4)] = kv.second;
        if (merged.empty()) continue;
        std::ofstream f(baselinePath(perGpu != 0));
        for (auto& kv : merged) f << kv.first << ' ' << kv.second << '\n';
        std::printf("  [linea base] escrita %s (%zu metricas)\n", baselinePath(perGpu != 0).c_str(), merged.size());
    }
}

/// Compara las capturas de los dos backends. Se llama desde `main` DESPUES de correr los dos.
static void compareBackends()
{
    BEGIN("careo GL <-> Vulkan de las imagenes");
    if (g_shots[0].empty() || g_shots[1].empty()) {
        std::printf("    solo se ha corrido un backend: nada que carear\n");
        return;
    }
    std::printf("    %-28s  tamano      pixeles distintos   |delta| medio   peor canal\n", "captura");
    size_t compared = 0, plain = 0;
    for (const auto& kv : g_shots[0]) {
        const auto it = g_shots[1].find(kv.first);
        if (it == g_shots[1].end()) {
            std::printf("    %-28s  SOLO EN OPENGL\n", kv.first.c_str());
            continue;
        }
        const Shot& a = kv.second; const Shot& b = it->second;
        if (a.w != b.w || a.h != b.h || a.px.size() != b.px.size()) {
            std::printf("    %-28s  %dx%d vs %dx%d  TAMANOS DISTINTOS\n",
                        kv.first.c_str(), a.w, a.h, b.w, b.h);
            CHECK(false, "las dos capturas tienen el mismo tamano");
            continue;
        }
        // ⚠️ CONTRAPRUEBA DEL PROPIO CAREO: una imagen PLANA (todo el mismo color) casa con
        // cualquier otra imagen plana del mismo color, asi que "0 % distinto" no probaria nada.
        // Se mide la varianza de la de OpenGL y se avisa si no tiene contenido.
        // ⚠️ "Plana" = SIN RANGO, no "casi todo igual al primer pixel". Con el criterio anterior una
        // imagen de cobertura (mitad blanca, mitad negra) se descartaba por plana y TAPABA el fallo.
        size_t distinct = 0;
        { uint8_t lo = 255, hi = 0;
          for (size_t k = 0; k < a.px.size(); k += 4)
              for (int ch = 0; ch < 3; ++ch) { lo = std::min(lo, a.px[k+ch]); hi = std::max(hi, a.px[k+ch]); }
          distinct = (hi - lo > 16) ? a.px.size() : 0; }
        const size_t n = a.px.size() / 4;

        // ⚠️ Y LO PRIMERO ES DESCARTAR UN FALLO DE ESTE MISMO CAREO. `readPixels` devuelve la imagen
        // de OpenGL de ABAJO ARRIBA y la de Vulkan de ARRIBA ABAJO. Comparar a ciegas dos imagenes
        // espejadas da ~50 % de pixeles distintos en cualquier escena no simetrica — que se leeria
        // como "Vulkan dibuja la mitad" siendo solo el origen de la lectura. Se mide de las dos
        // formas y se informa de cual casa: si gana la espejada, el motor esta bien y lo que hay que
        // arreglar es el banco.
        auto measure = [&](bool mirrorB, double& outPct, double& outMean, int& outWorst) {
            size_t d8 = 0; double sum = 0.0; int worst = 0;
            for (int y = 0; y < a.h; ++y)
                for (int x = 0; x < a.w; ++x) {
                    const size_t ka = ((size_t)y * a.w + x) * 4;
                    const int yb = mirrorB ? (a.h - 1 - y) : y;
                    const size_t kb = ((size_t)yb * a.w + x) * 4;
                    int mx = 0;
                    for (int ch = 0; ch < 3; ++ch) {
                        const int d = std::abs((int)a.px[ka+ch] - (int)b.px[kb+ch]);
                        sum += d; mx = std::max(mx, d);
                    }
                    worst = std::max(worst, mx);
                    if (mx > 8) ++d8;
                }
            outPct = n ? 100.0 * (double)d8 / (double)n : 0.0;
            outMean = sum / (double)(n * 3);
            outWorst = worst;
        };
        double pctD = 0, meanD = 0, pctM = 0, meanM = 0; int worstD = 0, worstM = 0;
        measure(false, pctD, meanD, worstD);
        measure(true,  pctM, meanM, worstM);
        const bool mirrored = pctM < pctD;
        const double pct = mirrored ? pctM : pctD;

        // ⚠️ "% de pixeles distintos" no distingue MAS CLARO de MAS OSCURO, y ese es justo el sintoma
        // que reporta Andoni ("se ve quemado o sin luz"). El color medio y el rango lo dicen: si un
        // backend sale sistematicamente mas alto es exposicion; si sale con menos rango, es que le
        // falta la luz y todo tiende a plano.
        auto stats = [](const Shot& sh, double m[3], int lo[3], int hi[3]) {
            double acc[3] = {0,0,0};
            for (int c = 0; c < 3; ++c) { lo[c] = 255; hi[c] = 0; }
            const size_t n = sh.px.size() / 4;
            for (size_t k = 0; k < sh.px.size(); k += 4)
                for (int c = 0; c < 3; ++c) {
                    const int v = sh.px[k+c];
                    acc[c] += v; lo[c] = std::min(lo[c], v); hi[c] = std::max(hi[c], v);
                }
            for (int c = 0; c < 3; ++c) m[c] = n ? acc[c] / (double)n : 0.0;
        };
        double ma[3], mb[3]; int la[3], ha[3], lb[3], hb[3];
        stats(a, ma, la, ha); stats(b, mb, lb, hb);
        std::printf("    %-28s  %4dx%-4d   %8.2f %%          %8.2f       %3d%s%s\n",
                    kv.first.c_str(), a.w, a.h, pct,
                    mirrored ? meanM : meanD, mirrored ? worstM : worstD,
                    mirrored ? "   [ESPEJADA EN Y: casa mejor al invertir]" : "",
                    (distinct * 20 < n) ? "   (imagen casi PLANA)" : "");
        if (mirrored)
            std::printf("      %-26s  directo %.2f %% · espejado %.2f %% -> el origen de lectura NO coincide\n",
                        "", pctD, pctM);
        std::printf("      color medio  OpenGL %5.1f %5.1f %5.1f (rango %d-%d)  ·  "
                    "Vulkan %5.1f %5.1f %5.1f (rango %d-%d)%s\n",
                    ma[0], ma[1], ma[2], la[1], ha[1], mb[0], mb[1], mb[2], lb[1], hb[1],
                    ((mb[0]+mb[1]+mb[2]) > (ma[0]+ma[1]+ma[2]) * 1.15) ? "   Vulkan MAS CLARO"
                  : ((ma[0]+ma[1]+ma[2]) > (mb[0]+mb[1]+mb[2]) * 1.15) ? "   Vulkan MAS OSCURO" : "");
        if (distinct * 20 < n) { ++plain; continue; }
        ++compared;

        // ── LA FORMA DEL ARTEFACTO IDENTIFICA LA CAUSA ──────────────────────────────────────────
        //
        // Es la regla que ya cerro las "capas que tapan el terreno" (ver TODO): una mancha, un
        // circulo y un cuadrado eran tres bugs distintos y lo que los separo fue mirar la FORMA, no
        // depurar. Aqui no hay pantalla, asi que se imprime: el mapa dice de un vistazo si falta la
        // mitad de arriba (eje Y), bandas (viewport/escala), un damero (instancias) o disperso
        // (profundidad).
        if (pct >= 5.0) {
            const int BW = 24, BH = 12;
            // ⚠️ EL MAPA DE DIFERENCIAS SOLO DICE DONDE, NO QUE. Que la mitad de abajo "difiera del
            // todo" es compatible con "uno dibuja y el otro no" y con "los dos dibujan cosas
            // distintas", que son bugs distintos. Asi que primero se imprime lo que dibuja CADA UNO.
            auto mapOf = [&](const Shot& sh, const char* who) {
                std::printf("      %s (espacio: - vacio  : oscuro  * medio  # claro):\n", who);
                for (int by = 0; by < BH; ++by) {
                    std::printf("        ");
                    for (int bx = 0; bx < BW; ++bx) {
                        double acc = 0.0; int cnt = 0;
                        for (int y = by * sh.h / BH; y < (by + 1) * sh.h / BH; ++y)
                            for (int x = bx * sh.w / BW; x < (bx + 1) * sh.w / BW; ++x) {
                                const size_t k = ((size_t)y * sh.w + x) * 4;
                                acc += (sh.px[k] + sh.px[k+1] + sh.px[k+2]) / 3.0; ++cnt;
                            }
                        const double m = cnt ? acc / cnt : 0.0;
                        std::putchar(m < 8 ? '-' : (m < 64 ? ':' : (m < 160 ? '*' : '#')));
                    }
                    std::putchar('\n');
                }
            };
            mapOf(a, "OpenGL"); mapOf(b, "Vulkan");
            std::printf("      mapa de diferencias (. igual  : leve  * fuerte  # total):\n");
            for (int by = 0; by < BH; ++by) {
                std::printf("        ");
                for (int bx = 0; bx < BW; ++bx) {
                    double acc = 0.0; int cnt = 0;
                    for (int y = by * a.h / BH; y < (by + 1) * a.h / BH; ++y)
                        for (int x = bx * a.w / BW; x < (bx + 1) * a.w / BW; ++x) {
                            const size_t ka = ((size_t)y * a.w + x) * 4;
                            const int yb = mirrored ? (a.h - 1 - y) : y;
                            const size_t kb = ((size_t)yb * a.w + x) * 4;
                            int mx = 0;
                            for (int ch = 0; ch < 3; ++ch)
                                mx = std::max(mx, std::abs((int)a.px[ka+ch] - (int)b.px[kb+ch]));
                            acc += mx; ++cnt;
                        }
                    const double m = cnt ? acc / cnt : 0.0;
                    std::putchar(m < 8 ? '.' : (m < 64 ? ':' : (m < 160 ? '*' : '#')));
                }
                std::putchar('\n');
            }
        }
        // ⚠️ ESTO MIDE, NO SENTENCIA — y la razon esta medida. Al horizonte GL cubre el 100 % del
        // cuadro y Vulkan solo la mitad de abajo; parecia que Vulkan perdia geometria. **Mirando al
        // NADIR los dos cubren el 100 % y el |delta| medio cae de 131 a 5,6**, o sea que Vulkan no
        // pierde nada: dibuja cielo encima del horizonte y GL pinta terreno ahi. Hasta saber cual es
        // el correcto en el JUEGO, marcar esto como fallo acusaria al backend equivocado. Queda como
        // 🔴 ABIERTO en el TODO con sus cifras.
        if (kv.first.find("NADIR") != std::string::npos)
            CHECK(pct < 90.0, "al NADIR los dos backends cubren el cuadro (control del careo)");
    }
    CHECK(compared > 0, "hay al menos una captura CON CONTENIDO que carear (si no, el careo es vacio)");
    if (plain) std::printf("    (%zu capturas descartadas por planas)\n", plain);
}



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
    const char* kWitness = "shaders/tests/rhitest_fullscreen.vert";

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
        Haruka::Renderer::Shader::setBaseDir(c.c_str());
        std::printf("== assets: %s ==\n", c.c_str());
        // ⚠️ Y SI ESTE BINARIO NO ESTA OPTIMIZADO, QUE SE VEA AQUI. Ver la nota larga del mismo
        // bloque en `HarukaTests.cpp`: el arbol de build se cuela en Debug en silencio y a partir de
        // ahi todo va a -O0 — la suite tarda 4x y las cifras de coste que imprime este banco (ms por
        // frame, ns por nodo) dejan de significar nada. Lo dice el compilador (`__OPTIMIZE__`), no
        // una variable de CMake que puede quedarse desincronizada.
#ifdef __OPTIMIZE__
        std::printf("== build: optimizado ==\n");
#else
        std::printf("\033[33m== build: SIN OPTIMIZAR (-O0): los tiempos de este banco NO valen ==\n"
                    "   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\033[0m\n");
#endif

        return true;
    }
    std::printf("[FAIL] no encuentro los assets (probadas %zu rutas). "
                "Compila con `./build.sh` para que se desplieguen.\n", cands.size());
    return false;
}


static std::string g_filter;   // segundo argumento del banco: grupo o test (subcadena)
static int runBackend(Backend backend)
{
    const char* name = (backend == Backend::Vulkan) ? "Vulkan" : "OpenGL";
    Uint32 winFlags = (backend == Backend::Vulkan) ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;

    // Como el motor (Core/Window.cpp): si el video no inicializó, intenta forzar el driver
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
    // El nombre del driver decide qué fallos están declarados como suyos. Se imprime siempre: sin
    // esta línea, un XFAIL en el log de CI no se puede atribuir a nada.
    baselineReset();   // cierra la linea base del backend ANTERIOR (escribe si se pidio) antes de cambiar de nombre
    g_curBackend = backend;
    g_curDevice  = dev->deviceName();
    std::printf("== Backend %s activo · dispositivo: %s ==\n", name, g_curDevice.c_str());

    // ⚠️ EXIGIR UN DISPOSITIVO, Y QUE FALLE SI NO ES ESE. Pedirlo por variables de entorno NO basta:
    // en un equipo con NVIDIA, `/usr/share/glvnd/egl_vendor.d/10_nvidia.json` ordena antes que
    // `50_mesa.json`, así que el vendor EGL de NVIDIA gana y `LIBGL_ALWAYS_SOFTWARE=1` —que es de
    // Mesa— no hace absolutamente nada. La tanda corre en la GPU dedicada creyendo que va por
    // software, y una cifra atribuida al rasterizador software es en realidad de otra cosa.
    // Con esto CI declara sobre qué quiere medir y se entera si no lo consigue.
    if (const char* want = std::getenv("HARUKA_REQUIRE_DEVICE")) {
        if (g_curDevice.find(want) == std::string::npos) {
            std::printf("[FAIL] HARUKA_REQUIRE_DEVICE=\"%s\" pero el dispositivo activo es \"%s\"\n",
                        want, g_curDevice.c_str());
            ++g_fail;
            setDevice(nullptr); dev.reset(); SDL_DestroyWindow(w);
            return 1;
        }
    }

    // ── LOS TESTS, POR GRUPOS (Andoni, 20-09: "agrupados, contador secundario, de mas simples a mas
    //    complejos"). Cada test imprime `[grupo i/N · test j/M]` antes de su propia cabecera. El
    //    segundo argumento del banco filtra: `haruka_tests_rhi vk nubes` corre el grupo cuya clave
    //    o nombre contenga la palabra, y ademas cualquier test cuyo nombre la contenga.
    // ⚠️ EL ORDEN DENTRO DE "escena" NO ES LIBRE: `testTerrainNodeSceneSeams` dibuja a un render
    //    target offscreen y deja el backend en un estado que hace fallar a quien lea el framebuffer
    //    por defecto despues (no he encontrado que queda mal: el viewport lo restaura
    //    `beginRenderPass`, el target se destruye, un frame de restauracion no lo arregla). Es un
    //    gap del RHI, no del terreno; por eso ese grupo va el ultimo y esos tres al final de el.
    struct RhiTest  { const char* name; void (*fn)(); };
    struct RhiGroup { const char* name; const char* what; std::vector<const char*> keys; std::vector<RhiTest> tests; };
    const std::vector<RhiGroup> groups = {
        { "RHI basico", "texturas, buffers, render targets, ciclo de frame, bindings, compute, instancing, pipelines del motor",
          { "rhi", "basic", "basico", "textura", "buffer", "target", "frame", "pipeline", "instanc" },
          { { "TextureFormats", testTextureFormats }, { "Buffers", testBuffers }, { "RenderTargets", testRenderTargets }, { "ArrayAndCube", testArrayAndCube }, { "FrameCycle", testFrameCycle }, { "BindingPersistence", testBindingPersistence }, { "ComputeArraySampler", testComputeArraySampler }, { "DispatchInsideRenderPass", testDispatchInsideRenderPass }, { "TextureContent", testTextureContent }, { "VertexColor", testVertexColor }, { "VertexInterpolation", testVertexInterpolation }, { "EnginePipelines", testEnginePipelines }, { "ItemPreviewShader", testItemPreviewShader }, { "InstancingRing", testInstancingRing }, { "SoftBodyMesh", testSoftBodyMesh } } },
        { "fp64 y paridad GPU", "la aritmetica de la GPU contra la CPU: fp64, octavas, el adaptador y el horneado por etapas",
          { "fp64", "parity", "paridad", "bisect", "bake", "horneado" },
          { { "GpuFp64", testGpuFp64 }, { "OctaveDivergence", testOctaveDivergence }, { "NodeGpuAdapterBisect", testNodeGpuAdapterBisect }, { "TerrainNodeGpuParity", testTerrainNodeGpuParity }, { "BakeStageBisect", testBakeStageBisect }, { "TerrainBakeAcrossLevels", testTerrainBakeAcrossLevels } } },
        { "terreno", "el quadtree v5 en la GPU: coste, pool, dibujado contra el campo, zancadas, cosido, horizonte",
          { "terreno", "terrain", "node", "v5", "stride", "zancada", "seam", "costura" },
          { { "TerrainNodeGpuCost", testTerrainNodeGpuCost }, { "TerrainNodePoolGpu", testTerrainNodePoolGpu }, { "TerrainNodeRender", testTerrainNodeRender }, { "TerrainNodeDrawnVsField", testTerrainNodeDrawnVsField }, { "TerrainStrideMatchCost", testTerrainStrideMatchCost }, { "TerrainNodeRendererInit", testTerrainNodeRendererInit }, { "TerrainNodeBaseField", testTerrainNodeBaseField }, { "NodeVoxCut", testNodeVoxCut }, { "NodeVoxCutFine", testNodeVoxCutFine }, { "VoxFineBricks", testVoxFineBricks }, { "HorizonPlateau", testHorizonPlateau }, { "StrideRingsOnLand", testStrideRingsOnLand }, { "FrameDrawnVsBaked", testFrameDrawnVsBaked }, { "TerrainNodeCoverage", testTerrainNodeCoverage }, { "TerrainNodeSeamHoles", testTerrainNodeSeamHoles }, { "TerrainNodeSharedEdgeGpu", testTerrainNodeSharedEdgeGpu }, { "TerrainNodeStitchSymmetryGpu", testTerrainNodeStitchSymmetryGpu } } },
        { "agua", "campo del agua, numero de onda, nodos con agua, olas y pliegue, formas de la profundidad",
          { "agua", "water", "ocean", "mar", "wave", "ola" },
          { { "WaterFieldParity", testWaterFieldParity }, { "WaveNumberParity", testWaveNumberParity }, { "NodeWaterDraws", testNodeWaterDraws }, { "OceanWaveParity", testOceanWaveParity }, { "OceanFoldParity", testOceanFoldParity }, { "WaterShapes", testWaterShapes } } },
        { "hierba", "briznas en GPU sobre el suelo dibujado, cono, contrapruebas y el mapa de presion",
          { "hierba", "grass", "brizna" },
          { { "GrassGen", testGrassGen } } },
        { "cielo y nubes", "limbo de la atmosfera, el pase volumetrico, el campo, la forma, la columna y el ruido; ambiente SH",
          { "cielo", "sky", "nube", "cloud", "atmos", "ambient" },
          { { "AtmosphereLimb", testAtmosphereLimb }, { "CloudPassDraws", testCloudPassDraws }, { "CloudPassOrbit", testCloudPassOrbit }, { "CloudHighLayers", testCloudHighLayers }, { "CloudPassSceneDepth", testCloudPassSceneDepth }, { "CloudFlatTop", testCloudFlatTop }, { "CloudMotionFrames", testCloudMotionFrames }, { "CloudMarchConvergence", testCloudMarchConvergence }, { "CloudPassCost", testCloudPassCost }, { "CloudCoverageByDirection", testCloudCoverageByDirection }, { "CloudOrbitView", testCloudOrbitView }, { "CloudFieldDistribution", testCloudFieldDistribution }, { "CloudShape3D", testCloudShape3D }, { "CloudColumnParity", testCloudColumnParity }, { "CloudNoiseParity", testCloudNoiseParity }, { "SkyLayersHaveDepth", testSkyLayersHaveDepth }, { "SkyAmbientGPU", testSkyAmbientGPU } } },
        { "luz y sombra", "iluminacion del terreno y de los props, el barrido de luz, el pase de sombra",
          { "luz", "light", "sombra", "shadow", "prop" },
          { { "TerrainLighting", testTerrainLighting }, { "PropLighting", testPropLighting }, { "PropLightSweep", testPropLightSweep }, { "TerrainShadow", testTerrainShadow } } },
        { "escena", "todo junto: culling con proyeccion, costuras de la escena, terreno + prop, coste del sombreado (los ultimos a proposito)",
          { "escena", "scene", "cull", "shade" },
          { { "CullWindingWithProjection", testCullWindingWithProjection }, { "TerrainNodeSceneSeams", testTerrainNodeSceneSeams }, { "SceneTerrainAndProp", testSceneTerrainAndProp }, { "TerrainNodeShadeCost", testTerrainNodeShadeCost } } },
    };
    // El filtro admite ALTERNATIVAS con `|` ("hierba|cielo"): para reproducir una fuga de estado de
    // un grupo al siguiente hace falta correr los dos en el mismo proceso, y solo esos.
    std::vector<std::string> alts;
    { size_t a = 0; while (a <= g_filter.size()) { const size_t b = g_filter.find('|', a);
        alts.push_back(g_filter.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break; a = b + 1; } }
    auto has = [&](const std::string& s) { for (const auto& f : alts) if (s.find(f) != std::string::npos) return true; return false; };
    struct Plan { const RhiGroup* g; std::vector<const RhiTest*> tests; };
    std::vector<Plan> plan;
    for (const RhiGroup& g : groups) {
        bool whole = g_filter.empty() || has(g.name);
        for (const char* k : g.keys) if (!g_filter.empty() && has(k)) whole = true;
        Plan p{ &g, {} };
        for (const RhiTest& t : g.tests) if (whole || has(t.name)) p.tests.push_back(&t);
        if (!p.tests.empty()) plan.push_back(p);
    }
    size_t total = 0; for (const Plan& p : plan) total += p.tests.size();
    std::printf("%zu grupos · %zu tests%s%s\n", plan.size(), total, g_filter.empty() ? "" : " · filtro: ", g_filter.c_str());
    struct Result { const char* name; size_t tests; int pass, fail, xfail; double secs; };
    std::vector<Result> results;
    for (size_t gi = 0; gi < plan.size(); ++gi) {
        const Plan& p = plan[gi];
        std::printf("\n\033[1m==== [%zu/%zu] %s\033[0m · %zu tests · %s ====\n", gi + 1, plan.size(), p.g->name, p.tests.size(), p.g->what);
        const int pass0 = g_pass, fail0 = g_fail, xfail0 = g_xfail;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t ti = 0; ti < p.tests.size(); ++ti) {
            std::printf("[%zu/%zu · %zu/%zu] %s\n", gi + 1, plan.size(), ti + 1, p.tests.size(), p.tests[ti]->name);
            std::fflush(stdout);
            p.tests[ti]->fn();
            // `HARUKA_BENCH_FRAMES=<dir>`: lo que la VENTANA enseña al acabar cada test, a PNG. Es
            // para saber que test dibujo lo que se ve en la ventana del banco (Andoni, 21-09: una
            // captura de la ventana con "creo que es el problema del terreno" que no decia de que
            // test era). Lee el backbuffer tal cual queda (GL) o la ultima imagen presentada (VK).
            if (const char* dir = std::getenv("HARUKA_BENCH_FRAMES")) {
                uint32_t fw = 0, fh = 0; g_dev->framebufferSize(fw, fh);
                if (fw > 0 && fh > 0) {
                    std::vector<uint8_t> px((size_t)fw * fh * 4, 0), img(px.size());
                    g_dev->readPixels(0, 0, (int)fw, (int)fh, Format::RGBA8, px.data());
                    const bool vk = (g_dev->backend() == Backend::Vulkan);
                    for (uint32_t y = 0; y < fh; ++y)
                        std::memcpy(&img[(size_t)y * fw * 4], &px[(size_t)(vk ? y : (fh - 1 - y)) * fw * 4], (size_t)fw * 4);
                    const std::string path = std::string(dir) + "/" + (vk ? "vk_" : "gl_") + p.tests[ti]->name + ".png";
                    Haruka::IO::writePNG(path, (int)fw, (int)fh, 4, img.data());
                }
            }
        }
        results.push_back({ p.g->name, p.tests.size(), g_pass - pass0, g_fail - fail0, g_xfail - xfail0,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() });
    }
    std::printf("\n== por grupo (%s) ==\n", name);
    for (const Result& r : results)
        std::printf("  %-22s %3zu tests · %6d comprobaciones · %s%d fallos\033[0m · %d xfail · %6.1f s\n",
                    r.name, r.tests, r.pass + r.fail, r.fail ? "\033[31m" : "", r.fail, r.xfail, r.secs);

    // ── ¿SIGUE SIENDO CIERTA LA LISTA DE DEFECTOS? ──────────────────────────────────────────────
    // Una entrada que no llegó a usarse en la tanda de SU backend y SU dispositivo describe algo que
    // ya no pasa: o el driver se arregló, o alguien tocó el mensaje del CHECK. Las dos cosas hay que
    // saberlas, porque una lista de excepciones que nadie revisa termina tapando bugs de verdad. Por
    // eso esto FALLA en vez de avisar.
    for (const DriverDefect& d : g_driverDefects) {
        if (d.backend != backend)                            continue;
        if (g_curDevice.find(d.device) == std::string::npos) continue;
        if (d.hits > 0)                                      continue;
        ++g_fail;
        std::printf("  [FAIL] [defectos de driver] la entrada \"%s\" / \"%s\" (%s) NO se uso: "
                    "el fallo ya no ocurre, o el mensaje cambio. Revisa la lista.\n",
                    d.test, d.check, d.device);
    }

    setDevice(nullptr);
    dev.reset();
    SDL_DestroyWindow(w);
    return 0;
}

int main(int argc, char** argv)
{
    std::string run = (argc > 1) ? argv[1] : "all";
    g_filter = (argc > 2) ? argv[2] : "";
    if (run != "gl" && run != "vk" && run != "all") {
        std::fprintf(stderr, "uso: haruka_tests_rhi [gl|vk|all]\n");
        return 2;
    }

    if (!locateAssets()) { std::printf("\n== 0 OK · 1 FALLOS ==\n"); return 1; }

    // ⚠️ LOS DOS BACKENDS EN LA MISMA GPU. En un portatil con dos GPUs, OpenGL sale por defecto en
    // la integrada (AMD Renoir aqui) y Vulkan elige la dedicada (NVIDIA): el banco comparaba dos
    // tarjetas creyendo comparar dos backends (costes x3, y el fp64 "degradado" era el de la AMD).
    // Con el driver de NVIDIA presente se pide PRIME render offload para GL, salvo que se diga
    // `HARUKA_GL_IGPU=1` (para medir la integrada a proposito).
    if (!std::getenv("HARUKA_GL_IGPU")) {
        struct stat st{};
        if (stat("/proc/driver/nvidia/version", &st) == 0) {
            setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
            setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        }
    }
    // ⚠️ SIN LA CACHE DE SHADERS EN DISCO DE NVIDIA. Con ella (driver 615.71.09, RTX 3050 Laptop) el
    // pase de nodos del terreno COLGABA la GPU en uno de cada tres arranques: `vkDeviceWaitIdle`
    // devolvia -4 (DEVICE_LOST) a los 4,3 s en el segundo o tercer frame de un `TerrainNodeRenderer`
    // recien creado, o se quedaba bloqueado sin volver. Medido el 20-09 sobre `vk NodeWaterDraws`
    // y `vk TerrainNodeCoverage` (35 % y 40 %):
    //   · no es el contenido: las instancias del frame que cuelga son BIT A BIT las del anterior;
    //   · no es sincronizacion: validacion de sync y GPU-AV limpias (los dos hazards que si habia se
    //     arreglaron aparte y no cambiaron la tasa); RADV en la AMD integrada 0/10;
    //   · SI es la cache: con `__GL_SHADER_DISK_CACHE=0` 0/20 (intercalado con 4/10 con cache), y con
    //     una cache NUEVA en un directorio vacio la primera corrida (que la escribe) pasa y las
    //     siguientes (que la LEEN) vuelven a colgar 2/12. Es el driver cargando el pipeline desde
    //     disco, no una entrada corrupta.
    // El banco es reproducible o no es banco, asi que aqui se apaga; `HARUKA_NV_SHADER_CACHE=1` la
    // deja como esta para volver a medirlo. El juego no lo toca: es decision de Andoni.
    if (!std::getenv("HARUKA_NV_SHADER_CACHE")) setenv("__GL_SHADER_DISK_CACHE", "0", 0);

    if (run == "gl" || run == "all") runBackend(Backend::OpenGL);
    if (run == "vk" || run == "all") runBackend(Backend::Vulkan);
    if (run == "all") compareBackends();

    SDL_Quit();
    baselineWriteIfAsked();
    if (g_xfail > 0)
        std::printf("\n== %d OK · %d FALLOS · %d XFAIL (defectos declarados del driver) ==\n",
                    g_pass, g_fail, g_xfail);
    else
        std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
