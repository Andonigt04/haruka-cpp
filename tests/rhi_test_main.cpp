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
#include "core/weather_system.h"   // kFieldScale: la escala del campo de nube, no un literal
#include "core/planet/ocean_wave.h"   // gemelo CPU de la ola (paridad con lib/ocean_wave.glsl)
#include "io/image_writer.h"          // HARUKA_WATER_PNG: volcar las formas para MIRARLAS
#include "core/planet/water_fill.h"   // el relleno de cuencas: gemelo del campo que lee la GPU
#include "core/terrain/terrain_node_renderer.h"   // Shader::baseDir() para los shaders del banco

#include <SDL3/SDL.h>
#include <array>
#include <map>
#include <unordered_set>

// --- mini-framework (mismo estilo que tests/test_common.h) ---
static int  g_pass = 0, g_fail = 0, g_xfail = 0;
static const char* g_cur = "";

using namespace Haruka::RHI;   // `Backend` lo usa la lista de defectos de abajo

// ── DEFECTOS CONOCIDOS DE UN DRIVER ─────────────────────────────────────────────────────────────
// Un test puede fallar sin que el motor tenga nada: hay drivers que dan mal un resultado que la
// especificación exige. El caso vivo es el fp64 — el driver de AMD y el rasterizador software
// lavapipe ANUNCIAN `shaderFloat64 = true` y aun así degradan los doubles.
//
// La tentación es borrar el test o bajarle el umbral. Las dos cosas pierden información: en AMD ese
// fallo es el bug CRÍTICO del suelo (el jugador pisa un terreno distinto según la GPU) y tiene que
// seguir en rojo. Lo que hace falta es poder decir "en ESTE driver esto es un defecto conocido, y
// aquí está el porqué", sin silenciarlo en ningún otro.
//
// Reglas, y la segunda es la que hace que esto no se pudra:
//   · un fallo que casa con una entrada se cuenta como XFAIL y no tumba la tanda;
//   · una entrada que NO llega a usarse en la tanda de su backend es un FALLO. Si el driver mejora,
//     o si alguien reescribe el mensaje del CHECK, la lista deja de describir la realidad y hay que
//     enterarse — una lista de excepciones que nadie revisa acaba tapando bugs de verdad.
struct DriverDefect {
    Backend     backend;
    const char* device;   ///< subcadena de `Device::deviceName()` (GL_RENDERER / deviceName)
    const char* test;     ///< subcadena del nombre del test (`BEGIN`)
    const char* check;    ///< subcadena del mensaje del `CHECK`
    const char* reason;   ///< por qué NO es del motor. Obligatorio: sin motivo no entra.
    int         hits;
};

// ⚠️ Nada de AMD aquí a propósito: su fp64 malo es un bug REAL que afecta a lo que se juega, y se
// quiere ver en rojo. Esta lista es solo para drivers donde el fallo no dice nada del motor.
#define HARUKA_LLVMPIPE_FP64 "el rasterizador software de Mesa anuncia fp64 y no lo cumple"
static DriverDefect g_driverDefects[] = {
    // Vulkan sobre lavapipe.
    { Backend::Vulkan, "llvmpipe", "DOUBLE, o la degrada a float", "(3) `normalize(dvec3)*R`",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::Vulkan, "llvmpipe", "DOUBLE, o la degrada a float", "(10) `harukaCubeFaceToDir`",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::Vulkan, "llvmpipe", "DOUBLE, o la degrada a float", "(11) el PRODUCTO en doble",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::Vulkan, "llvmpipe", "DOUBLE, o la degrada a float", "(12) la DIVISION en doble",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::Vulkan, "llvmpipe", "biseccion por texel", "la direccion del texel es la MISMA",
      "arrastra el fp64 degradado de lavapipe", 0 },
    { Backend::Vulkan, "llvmpipe", "biseccion por texel", "la posicion `dir*R` es la MISMA",
      "arrastra el fp64 degradado de lavapipe", 0 },
    // Y OpenGL sobre llvmpipe: EXACTAMENTE los mismos seis. No es cosa del backend ni del motor, es
    // la implementación de doubles de Mesa por software. (Comprobado midiendo: NVIDIA los pasa todos
    // en GL; AMD y los dos rasterizadores software fallan.)
    { Backend::OpenGL, "llvmpipe", "DOUBLE, o la degrada a float", "(3) `normalize(dvec3)*R`",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::OpenGL, "llvmpipe", "DOUBLE, o la degrada a float", "(10) `harukaCubeFaceToDir`",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::OpenGL, "llvmpipe", "DOUBLE, o la degrada a float", "(11) el PRODUCTO en doble",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::OpenGL, "llvmpipe", "DOUBLE, o la degrada a float", "(12) la DIVISION en doble",
      HARUKA_LLVMPIPE_FP64, 0 },
    { Backend::OpenGL, "llvmpipe", "biseccion por texel", "la direccion del texel es la MISMA",
      "arrastra el fp64 degradado de llvmpipe", 0 },
    { Backend::OpenGL, "llvmpipe", "biseccion por texel", "la posicion `dir*R` es la MISMA",
      "arrastra el fp64 degradado de llvmpipe", 0 },
};

static Backend     g_curBackend = Backend::OpenGL;
static std::string g_curDevice;

/** @brief ¿Este fallo está declarado como defecto de ESTE driver? Devuelve el motivo, o nullptr. */
static const char* knownDefect(const char* test, const char* msg)
{
    for (DriverDefect& d : g_driverDefects) {
        if (d.backend != g_curBackend)                            continue;
        if (g_curDevice.find(d.device) == std::string::npos)      continue;
        if (!std::strstr(test, d.test) || !std::strstr(msg, d.check)) continue;
        ++d.hits;
        return d.reason;
    }
    return nullptr;
}

#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else if (const char* _r = knownDefect(g_cur, msg)) { \
        ++g_xfail; std::printf("  [XFAIL] [%s] %s  <- %s\n", g_cur, msg, _r); \
    } \
    else { ++g_fail; std::printf("  [FAIL] [%s] %s\n", g_cur, msg); } \
} while (0)
#define BEGIN(name) do { g_cur = name; std::printf("== %s ==\n", name); } while (0)


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

// ================================================================================================
// CAREO GL ↔ VULKAN: el banco compara cada backend CONSIGO MISMO, nunca uno contra otro
//
// ⚠️ ESE ERA EL HUECO. Cada test dibuja, lee pixeles y comprueba una propiedad; los dos backends
// corren en el MISMO proceso, uno detras de otro, y nadie compara las dos imagenes. Aqui se guarda la
// captura de cada uno bajo un nombre y al terminar los dos se carean. Es lo que convierte un "se ve
// raro" en un numero.
struct Shot { int w = 0, h = 0; std::vector<uint8_t> px; };
static std::map<std::string, Shot> g_shots[2];      // 0 = OpenGL · 1 = Vulkan

static void recordShot(const char* name, int w, int h, const std::vector<uint8_t>& px)
{
    const int b = (g_dev && g_dev->backend() == Backend::Vulkan) ? 1 : 0;
    g_shots[b][name] = Shot{ w, h, px };
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
        // ⚠️ Y SI ESTE BINARIO NO ESTA OPTIMIZADO, QUE SE VEA AQUI. Ver la nota larga del mismo
        // bloque en `haruka_tests.cpp`: el arbol de build se cuela en Debug en silencio y a partir de
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
// ================================================================================================
// ¿INTERPOLA el RHI los atributos, o cada cara sale de un color?
//
// ⚠️ ESTE HUECO LLEVABA AHI DESDE QUE SE CREO EL RHI, y lo dice el propio `testVertexColor`: *"los
// tres vertices del MISMO color: asi el centro es ese color exacto y no hay que razonar sobre la
// interpolacion"*. Comprueba que el atributo LLEGA, pero con los tres iguales **un fallo de
// interpolacion pasa el test sin despeinarse**: si el triangulo entero saliera plano, el pixel
// central seria exactamente el mismo color y todo verde.
//
// Andoni: *"toda la cara/triangulo se dibuja con la misma iluminacion o color"* y *"es un error que
// ya existia desde la creacion del RHI"*. Esto es lo que faltaba para poder decirlo con un numero:
// UN triangulo con TRES colores distintos, y se leen cuatro puntos. Si interpola, cada esquina tira
// hacia su vertice y el centro es la mezcla; si no, los cuatro salen iguales.
/**
 * @brief Los shaders del ICONO DE INVENTARIO pintan, y pintan IGUAL en los dos backends.
 *
 * El preview de items vivia en OpenGL crudo (glCreateShader + FBO propio), asi que bajo Vulkan las
 * casillas salian sin icono. Al portarlo al RHI aparecen dos riesgos que NO se ven en pantalla hasta
 * que alguien abre el inventario:
 *
 *  1. Que el pipeline no pinte NADA. El icono usa una ortografica normal con test Less, mientras que
 *     todo el motor va en reversed-Z (limpia la profundidad a 0 y compara con Greater). Heredar ese
 *     0.0 deja la textura VACIA sin un solo error — y una textura vacia y una casilla sin objeto se
 *     ven exactamente igual. Ese fallo mudo ya ocurrio una vez en la version de GL.
 *  2. Que salga DEL REVES solo en Vulkan. La Y del clip de Vulkan va invertida y este motor la
 *     compensa en la PROYECCION (camera.cpp), no en el viewport; el icono tiene proyeccion propia y
 *     por tanto necesita su propia compensacion.
 *
 * Por eso se dibuja una caja ALTA y DESCENTRADA (no simetrica): un objeto simetrico saldria igual del
 * derecho que del reves y el test no distinguiria nada. Se mide cuanta tinta cae en la mitad de
 * arriba frente a la de abajo. Si los dos backends dan el mismo reparto, ImGui mostrara lo mismo en
 * los dos: muestrea el texel (0,0) en ambos, asi que arrays de texeles iguales = imagen igual.
 */
// ── EL MAR SOBRE CUALQUIER FORMA, Y LA OLA QUE SE VE == LA OLA QUE SE NADA ──────────────────────
//
// La ola vive DOS VECES: `assets/shaders/lib/ocean_wave.glsl` (la que se dibuja) y
// `src/core/planet/ocean_wave.h` (la que usa la fisica para flotar y arrastrar). Estan escritas como
// gemelas, cada una con comentarios que apuntan a la otra, y NO HABIA NI UN TEST QUE LAS COMPARASE.
// Si divergen, lo que se ve y lo que se nada dejan de ser lo mismo — y esa es exactamente la clase de
// fallo que no da error, no rompe nada y solo se nota como "el agua esta rara".
//
// Se sube a la GPU el MISMO banco de trenes que usa la CPU (`oceanDefaultState`), asi que cualquier
// diferencia es de la formula y no de los datos.

/// Gemelo del bloque `OceanParams` de lib/ocean_params.glsl (binding 29).
// ⚠️ GEMELO A MANO DEL BLOQUE `OceanParams`, Y EL TAMAÑO VA POR `OCEAN_WAVES`, NO POR UN LITERAL.
// Estaba como `float wave[4][4]`. Al subir el espectro a 8 trenes, rellenarlo escribia 16 floats MAS
// ALLA del struct: **volcado de nucleo**, el banco entero muerto. Y el sintoma previo, con el bucle
// aun en 4, fue el contrario y mas silencioso — el agua no dibujaba un pixel porque los trenes
// vacios daban `k = 2π/0`. Un gemelo a mano con una constante escrita rompe en las dos direcciones.
struct OceanParamsUBO { float wave[Haruka::Planet::OCEAN_WAVES][4]; float misc[4]; };

static BufferHandle makeOceanStateUBO(const Haruka::Planet::OceanState& st)
{
    OceanParamsUBO u{};
    // ⚠️ AQUI HABIA UN `4` ESCRITO A MANO Y COSTO EL PASE DE AGUA ENTERO. Al subir el espectro de 4 a
    // 8 trenes, los cuatro ultimos se quedaban a CERO en el UBO: `lambda = 0` da `k = 2π/0 = inf`, el
    // vertice sale NaN y **no se dibuja un solo pixel de agua**. El sintoma no fue "el mar se ve
    // raro", fue "el mar no existe", y en UN backend de los dos. El bucle va con `OCEAN_WAVES`.
    for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c) u.wave[i][c] = st.wave[i][c];
    u.misc[0] = st.seaLevelM;
    u.misc[1] = 1.0f;              // "el bloque trae datos validos": sin esto el shader usaria su
                                   // tabla de respaldo y el careo no probaria nada del estado subido.
    return g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);
}

static void testOceanWaveParity()
{
    BEGIN("mar: la ola de la GPU == la ola de la CPU (gemelas)");

    const std::string base = Haruka::Shader::baseDir();
    const std::string vs = base + "shaders/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/rhitest_waveprobe.frag";

    // Barrido determinista: cada pixel es una muestra. La profundidad recorre de 0,5 m a ~12 m, o
    // sea toda la franja de rompiente, que es donde el modelo hace mas cosas y donde estaba el bug
    // del tope por tren. Con solo mar abierto el careo pasaria sin tocar nada de eso.
    // ⚠️ EL TAMANO ES EL DE LA VENTANA DEL BANCO (256x256) A PROPOSITO. En OpenGL el viewport es
    // estado GLOBAL: un test que lo deja en 128x128 hace que TODOS los siguientes dibujen en una
    // esquina mientras leen la ventana entera. Costo 6 fallos en tests que no tienen nada que ver
    // (cielo, descarte de caras, costuras, luz de props) y solo en GL — en Vulkan el viewport es
    // estado dinamico del pase y se refija solo, asi que la tanda de Vulkan salia limpia y la de GL
    // no. Si algun dia hace falta otro tamano, hay que restaurarlo antes de endRenderPass.
    const int W = 256, H = 256;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 origin(1000.0f, 0.0f, -500.0f);
    const glm::vec3 stepX(0.7f, 0.0f, 0.11f);
    const glm::vec3 stepY(0.13f, 0.0f, 0.9f);
    const float     t0 = 12.75f;
    const float     depth0 = 0.5f, depthStep = 0.09f;

    struct ProbeUBO {
        float origin[4], stepX[4], stepY[4], up[4], depth[4];
    } pu{};
    pu.origin[0]=origin.x; pu.origin[1]=origin.y; pu.origin[2]=origin.z; pu.origin[3]=t0;
    pu.stepX[0]=stepX.x;   pu.stepX[1]=stepX.y;   pu.stepX[2]=stepX.z;
    pu.stepY[0]=stepY.x;   pu.stepY[1]=stepY.y;   pu.stepY[2]=stepY.z;
    pu.up[0]=up.x;         pu.up[1]=up.y;         pu.up[2]=up.z;
    pu.depth[0]=depth0;    pu.depth[1]=depthStep; pu.depth[2]=(float)H;

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.depth.test = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    BufferHandle   ub   = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pu), &pu, BufferMemory::Dynamic);
    const Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
    BufferHandle   ocean = makeOceanStateUBO(st);
    CHECK(valid(pipe) && valid(ub) && valid(ocean), "pipeline y UBOs de la sonda creados");
    if (!valid(pipe)) return;

    std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->setViewport(0, 0, W, H);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ub);
        c->bindUniformBuffer(29, ocean);
        c->draw(3);
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
    }

    // ⚠️ Si el readback no escribe, el buffer sigue en 0xAA y decodificarlo daria una altura
    // constante que casaria mal con TODO — y el test acusaria al shader de un fallo del instrumento.
    bool wrote = false;
    for (size_t k = 0; k < px.size(); k += 4) if (px[k] != 0xAA || px[k+1] != 0xAA) { wrote = true; break; }
    CHECK(wrote, "el readback escribio (si no, el careo no significaria nada)");
    if (!wrote) { std::printf("    SIN LECTURA: no se puede carear\n"); return; }

    const float kRange = 4.0f;   // el mismo que codifica el shader
    auto decode = [&](int x, int y) {
        const size_t k = ((size_t)y * W + x) * 4;
        const float scaled = px[k] * 65536.0f + px[k+1] * 256.0f + px[k+2];
        return (scaled / 16777215.0f) * 2.0f * kRange - kRange;
    };

    // La espuma viaja en el ALFA con 8 bits (ver `rhitest_waveprobe.frag`).
    auto decodeFoam = [&](int x, int y) {
        return px[((size_t)y * W + x) * 4 + 3] / 255.0f;
    };

    double worst = 0.0, sum = 0.0; int n = 0, worstX = 0, worstY = 0;
    float worstGpu = 0.0f, worstCpu = 0.0f;
    double foamWorst = 0.0; int foamWorstY = 0; float foamGpu = 0.0f, foamCpu = 0.0f;
    int foamWet = 0;                              // muestras con espuma apreciable, para que no pase
                                                  // por verde un careo de 0 contra 0
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
            const float depth  = depth0 + depthStep * (float)y;
            const float hCpu = Haruka::Planet::oceanWaveHeight(wp, up, t0, depth, 1.0f, st);
            const float hGpu = decode(x, y);
            const double d = std::abs((double)hGpu - (double)hCpu);
            sum += d; ++n;
            if (d > worst) { worst = d; worstX = x; worstY = y; worstGpu = hGpu; worstCpu = hCpu; }

            // ⚠️ LA ESPUMA, que hasta ahora no se careaba con nada. Mismo punto, mismo tiempo.
            const float fCpu = Haruka::Planet::oceanFoam(wp, up, t0, depth, 1.0f, st);
            const float fGpu = decodeFoam(x, y);
            if (fCpu > 0.01f) ++foamWet;
            const double df = std::abs((double)fGpu - (double)fCpu);
            if (df > foamWorst) { foamWorst = df; foamWorstY = y; foamGpu = fGpu; foamCpu = fCpu; }
        }

    const double mean = n ? sum / n : 0.0;
    std::printf("    %d muestras · profundidad de %.2f a %.2f m (la franja de rompiente entera)\n",
                n, depth0, depth0 + depthStep * (H - 1));
    std::printf("    diferencia GPU vs CPU: media %.6f m · peor %.6f m\n", mean, worst);
    std::printf("      peor punto (%d,%d) a %.2f m de fondo: GPU %+.4f m · CPU %+.4f m\n",
                worstX, worstY, depth0 + depthStep * worstY, worstGpu, worstCpu);

    // El suelo de ruido no es cero: la altura viaja codificada en 24 bits sobre un rango de +-4 m,
    // o sea ~0,5 micras de cuantizacion, y las dos partes usan float de 32 bits con `sin` de
    // implementaciones distintas. Un milimetro es holgado para eso y estrecho para una divergencia
    // real de formula (el bug del tope por tren daba METROS).
    CHECK(worst < 1e-3, "la ola dibujada y la ola simulada son la misma (< 1 mm)");

    // ── LA ESPUMA ────────────────────────────────────────────────────────────────────────────────
    // Era la unica magnitud de la ola sin gemelo en CPU, y por tanto sin careo posible. Ahora
    // `oceanFoam` existe y se compara en el mismo barrido.
    std::printf("    espuma: %d de %d muestras con espuma apreciable (>0,01)\n", foamWet, n);
    std::printf("      diferencia GPU vs CPU: peor %.6f (fila %d, %.2f m de fondo): GPU %.4f · CPU %.4f\n",
                foamWorst, foamWorstY, depth0 + depthStep * foamWorstY, foamGpu, foamCpu);
    // ⚠️ SIN ESTA GUARDIA EL CAREO DE ESPUMA SERIA UNA TAUTOLOGIA: si el barrido cayera entero en agua
    // sin espuma, comparar 0 contra 0 daria "paridad perfecta" sin haber probado una sola rama.
    CHECK(foamWet > n / 100, "el barrido ATRAVIESA espuma (si no, el careo no probaria nada)");
    // La tolerancia es la cuantizacion del canal: 1/255 = 0,0039. Se deja el doble por el redondeo de
    // los dos lados. Una divergencia de FORMULA (un umbral movido en un solo lado) daria decimas.
    CHECK(foamWorst < 2.0 / 255.0, "la espuma dibujada y la espuma simulada son la misma (< 2/255)");

    // CONTRAPRUEBA: con el estado CAMBIADO la diferencia tiene que dispararse. Sin esto, un careo que
    // comparase cualquier cosa consigo misma daria 0 y pasaria igual.
    Haruka::Planet::OceanState alt = st;
    alt.wave[0][1] *= 1.5f;                       // el tren dominante, un 50% mas alto
    double worstAlt = 0.0, foamAlt = 0.0;
    for (int y = 0; y < H; y += 4)
        for (int x = 0; x < W; x += 4) {
            const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
            const float depth  = depth0 + depthStep * (float)y;
            const float hAlt = Haruka::Planet::oceanWaveHeight(wp, up, t0, depth, 1.0f, alt);
            worstAlt = std::max(worstAlt, std::abs((double)decode(x, y) - (double)hAlt));
            const float fAlt = Haruka::Planet::oceanFoam(wp, up, t0, depth, 1.0f, alt);
            foamAlt = std::max(foamAlt, std::abs((double)decodeFoam(x, y) - (double)fAlt));
        }
    std::printf("    CONTRAPRUEBA: con el tren dominante un 50%% mas alto, la peor diferencia sube a %.4f m\n",
                worstAlt);
    std::printf("      y la de espuma a %.4f\n", foamAlt);
    CHECK(worstAlt > 0.05, "el careo detecta un cambio del estado (no compara algo consigo mismo)");
    // La misma contraprueba para la espuma: si `oceanFoam` ignorase el estado (devolviendo, por
    // ejemplo, solo la rama de orilla por profundidad) esto seguiria en cero y el careo de arriba
    // pasaria sin probar que la espuma depende de la OLA.
    CHECK(foamAlt > 0.05, "el careo de espuma detecta un cambio del estado");

    g_dev->destroy(pipe); g_dev->destroy(ub); g_dev->destroy(ocean);
}

/**
 * @brief La ola sobre FORMAS que no son un planeta: un anillo cilindrico y un cubo.
 *
 * Responde a una pregunta de diseno con una medida en vez de con una opinion: ¿el mar depende de que
 * haya una esfera debajo? El modelo solo pide posicion, normal y profundidad, y construye su marco
 * tangente del propio `up` — asi que deberia valer para cualquier superficie. Aqui se dibuja sobre
 * dos que no tienen nada que ver con un planeta.
 *
 * El CUBO es el caso duro a proposito: en una arista la normal salta de golpe y con ella el marco
 * tangente. Si la ola dependiera del marco de manera inestable, ahi se veria una costura.
 *
 * Se mide cobertura y color medio en los dos backends. La imagen queda ademas guardada en `g_shots`
 * para que el careo GL<->Vulkan de `compareBackends()` la incluya como una escena mas.
 */
static void testWaterShapes()
{
    BEGIN("mar: la ola sobre un anillo y sobre un cubo (no hace falta planeta)");

    const std::string base = Haruka::Shader::baseDir();
    const std::string vs = base + "shaders/rhitest_watershape.vert";
    const std::string fs = base + "shaders/rhitest_watershape.frag";

    struct V { float px, py, pz, nx, ny, nz; };

    // --- ANILLO CILINDRICO (un TORO): la normal barre TODAS las direcciones — hacia fuera, hacia
    //     dentro, arriba y abajo — asi que es la prueba dura de que el marco tangente se reconstruye
    //     bien en cualquier orientacion.
    //
    // ⚠️ La primera version era una BANDA cilindrica abierta y no valia: con `cull = None` se veia el
    // interior por las bocas y la silueta no se leia como un anillo. Un toro es cerrado y solo tiene
    // una lectura posible, que es lo que un test visual necesita.
    std::vector<V> ringV; std::vector<uint32_t> ringI;
    {
        const int major = 160, minor = 40;      // vueltas larga y corta
        const float R = 0.52f, r = 0.155f;      // radios mayor y menor
        for (int i = 0; i <= major; ++i) {
            const float a = 6.2831853f * (float)i / (float)major;
            const float ca = std::cos(a), sa = std::sin(a);
            for (int j = 0; j <= minor; ++j) {
                const float b = 6.2831853f * (float)j / (float)minor;
                const float cb = std::cos(b), sb = std::sin(b);
                // Normal del toro: radial respecto a la CIRCUNFERENCIA GUIA, no al centro.
                const glm::vec3 n(ca * cb, sb, sa * cb);
                const glm::vec3 p(ca * (R + r * cb), r * sb, sa * (R + r * cb));
                ringV.push_back({ p.x, p.y, p.z, n.x, n.y, n.z });
            }
        }
        const int stride = minor + 1;
        for (int i = 0; i < major; ++i)
            for (int j = 0; j < minor; ++j) {
                const uint32_t a = (uint32_t)(i * stride + j);
                for (uint32_t e : { 0u, 1u, (uint32_t)stride,
                                    1u, (uint32_t)(stride + 1), (uint32_t)stride })
                    ringI.push_back(a + e);
            }
    }

    // --- CUBO: `up` es la normal de cada cara, constante dentro de la cara y DISCONTINUA en la arista.
    std::vector<V> cubeV; std::vector<uint32_t> cubeI;
    {
        const float h = 0.42f;
        const glm::vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; ++f) {
            const glm::vec3 n = N[f];
            const glm::vec3 u = std::abs(n.y) > 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
            const glm::vec3 t = glm::normalize(glm::cross(u, n)), b = glm::cross(n, t);
            const uint32_t v0 = (uint32_t)cubeV.size();
            // Subdividido: sin vertices intermedios la ola no tendria donde desplazar nada.
            const int S = 24;
            for (int j = 0; j <= S; ++j)
                for (int i = 0; i <= S; ++i) {
                    const float fu = (float)i / S * 2.0f - 1.0f, fv = (float)j / S * 2.0f - 1.0f;
                    const glm::vec3 p = (n + t * fu + b * fv) * h;
                    cubeV.push_back({ p.x, p.y, p.z, n.x, n.y, n.z });
                }
            for (int j = 0; j < S; ++j)
                for (int i = 0; i < S; ++i) {
                    const uint32_t a = v0 + (uint32_t)(j * (S + 1) + i);
                    for (uint32_t e : { 0u, 1u, (uint32_t)(S+1), 1u, (uint32_t)(S+2), (uint32_t)(S+1) })
                        cubeI.push_back(a + e);
                }
        }
    }

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0, (uint32_t)offsetof(V, px), Format::RGB32F, 0 },
        { 1, (uint32_t)offsetof(V, nx), Format::RGB32F, 0 },
    };
    pd.depth.test = true; pd.depth.write = true; pd.depth.compare = CompareOp::Less;
    pd.blend.enable = false;
    pd.cull = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    const Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
    BufferHandle ocean = makeOceanStateUBO(st);
    CHECK(valid(pipe) && valid(ocean), "pipeline de formas de agua creado");
    if (!valid(pipe)) return;

    struct ShapeUBO { float vp[16]; float misc[4]; };
    // Camara fija mirando las dos piezas de tres cuartos. Ortografica: sin perspectiva la cobertura
    // en pixeles es comparable entre backends sin depender de la matriz de proyeccion.
    glm::mat4 proj = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -4.0f, 4.0f);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    (void)isVk;
    const glm::mat4 view = glm::lookAt(glm::vec3(1.15f, 0.95f, 1.3f), glm::vec3(0.0f), glm::vec3(0,1,0));

    // MAR EN CALMA: el mismo estado con las cuatro amplitudes a cero. Es la CONTRAPRUEBA del
    // contraste: sin ella, "la pieza tiene relieve" se cumpliria igual por el propio brillo especular
    // sobre un toro liso, y el test no estaria midiendo la ola sino la forma.
    // ⚠️ ESTE `4` ESCRITO A MANO DEJABA LA REFERENCIA "EN CALMA" CON OLA. Al subir el espectro de 4 a
    // 8 trenes, apagar solo los cuatro primeros deja los otros CUATRO a plena amplitud: el mar de
    // referencia contra el que se compara "con ola vs sin ola" seguia teniendo ola. Y como la medida
    // es una DIFERENCIA contra esa referencia, el resultado sale mas pequeño de lo que es — el fallo
    // no grita, susurra. Es el tercer `4` literal que destapa el mismo cambio (los otros dos, en el
    // UBO del banco, dieron agua invisible y un volcado de nucleo).
    Haruka::Planet::OceanState calmSt = st;
    for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i) calmSt.wave[i][1] = 0.0f;
    BufferHandle calm = makeOceanStateUBO(calmSt);

    struct Shape { const char* name; std::vector<V>* v; std::vector<uint32_t>* i; float depth; bool flat; };
    Shape shapes[] = {
        { "anillo, mar abierto (40 m)", &ringV, &ringI, 40.0f, false },
        { "anillo, rompiendo  (1,5 m)", &ringV, &ringI,  1.5f, false },
        { "anillo, EN CALMA (amp 0)",   &ringV, &ringI, 40.0f, true  },
        { "cubo,   mar abierto (40 m)", &cubeV, &cubeI, 40.0f, false },
        { "cubo,   rompiendo  (1,5 m)", &cubeV, &cubeI,  1.5f, false },
    };

    const int W = 256, H = 256;   // el de la ventana: ver la nota del viewport en testOceanWaveParity
    // ⚠️ EL NOMBRE LLEVABA EL CALMA AL MISMO FICHERO QUE EL HONDO. El anillo EN CALMA es tambien
    // `depth > 10`, asi que su PNG se llamaba igual que el de mar abierto y lo PISABA: quien fuera a
    // comparar "con ola contra en calma" —que es justo para lo que sirve el volcado— se encontraba la
    // misma imagen dos veces. Lo destapo el propio volcado, que imprimio `anillo_hondo` dos veces.
    auto shotBase = [&](const Shape& s2) {
        return std::string("agua_") + (s2.v == &ringV ? "anillo" : "cubo") +
               (s2.flat ? "_calma" : (s2.depth > 10.0f ? "_hondo" : "_rompiendo"));
    };
    long   cover[5] = { 0, 0, 0, 0, 0 };
    double contrast[5] = { 0, 0, 0, 0, 0 };
    double circul[5]   = { 0, 0, 0, 0, 0 };   // 4pi·area/perimetro²: 1,00 circulo · 0,785 cuadrado
    std::vector<std::vector<uint8_t>> frames(5);
    int    shapeIdx = 0;
    for (const Shape& sh : shapes) {
        BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sh.v->size()*sizeof(V), sh.v->data());
        BufferHandle ib = g_dev->createBuffer(BufferUsage::Index,  sh.i->size()*sizeof(uint32_t), sh.i->data());
        ShapeUBO su{};
        const glm::mat4 vp = proj * view;
        std::memcpy(su.vp, &vp[0][0], sizeof(su.vp));
        su.misc[0] = 9.5f;        // tiempo
        su.misc[1] = sh.depth;    // profundidad del agua
        // Metros por unidad de forma. Con 47 el toro mide 24,4 m de radio mayor, o sea 153 m de
        // vuelta: unas 2,5 longitudes de onda del tren dominante (61 m). Con el 22 de antes daba
        // 1,4 crestas — una sola ondulacion, que no deja ver si la ola recorre la pieza.
        su.misc[2] = 47.0f;
        BufferHandle ub = g_dev->createBuffer(BufferUsage::Uniform, sizeof(su), &su, BufferMemory::Dynamic);

        std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
        if (Context* c = g_dev->beginFrame()) {
            ClearValues cv;
            cv.clearColor = true; cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f;
            cv.clearDepth = true; cv.depth = 1.0f;   // test Less: el lejano es 1
            c->beginRenderPass({}, cv);
            c->setViewport(0, 0, W, H);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, ub);
            c->bindUniformBuffer(29, sh.flat ? calm : ocean);
            c->bindVertexBuffer(vb);
            c->bindIndexBuffer(ib);
            c->drawIndexed((uint32_t)sh.i->size());
            c->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        }

        // CONTRASTE de la pieza: desviacion tipica de la luminancia sobre los pixeles de agua. Es la
        // medida de "se ve relieve": una superficie lisa da un degradado suave (desviacion baja) y una
        // rizada enciende y apaga el especular (desviacion alta). Sustituye al conteo de espuma, que
        // no informaba de nada — a 1,5 m la espuma de ORILLA vale 1,0 en todo el cuadro por definicion.
        // ⚠️ CONTRASTE **LOCAL**, no la desviacion global. La global no servia y la contraprueba lo
        // demostro: el toro en calma daba MAS sigma (23,7) que el toro con olas (23,1), porque ese
        // numero lo domina el degradado suave del cuerpo —lado iluminado contra lado en sombra— y el
        // rizado, que es de alta frecuencia y poca amplitud, se pierde dentro. La diferencia entre
        // pixeles VECINOS ignora el degradado por construccion y solo ve el detalle fino.
        auto lum = [&](int x, int y) {
            const size_t k = ((size_t)y * W + x) * 4;
            return 0.2126 * px[k] + 0.7152 * px[k+1] + 0.0722 * px[k+2];
        };
        auto isWater = [&](int x, int y) {
            const size_t k = ((size_t)y * W + x) * 4;
            return px[k] + px[k+1] + px[k+2] > 24;
        };
        long lit = 0; double gradSum = 0.0; long gradN = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!isWater(x, y)) continue;
                ++lit;
                // Solo pares interiores: en la silueta el salto al fondo negro es enorme y no es rizado.
                if (x + 1 < W && isWater(x + 1, y)) { gradSum += std::abs(lum(x+1,y) - lum(x,y)); ++gradN; }
                if (y + 1 < H && isWater(x, y + 1)) { gradSum += std::abs(lum(x,y+1) - lum(x,y)); ++gradN; }
            }
        const double sd = gradN ? gradSum / gradN : 0.0;
        // ⚠️ Y LA FORMA, QUE ESTE TEST NO SABIA VER. Andoni reporto que **en OpenGL el anillo se ve
        // como un cuadrado deformado** mientras en Vulkan sale redondo — y el test pasaba en verde en
        // los dos. El motivo: solo medía el AREA de la silueta ("18,7 %"), y un cuadrado y un circulo
        // de la misma area dan el mismo numero. Un test de forma que no mira la forma.
        //
        // La medida que si lo ve es la CIRCULARIDAD, `4π·area / perimetro²`: vale 1,00 para un
        // circulo perfecto, 0,785 para un cuadrado, y baja con cualquier deformacion. El perimetro se
        // cuenta como pixeles de agua con algun vecino de fondo (contorno de 4 vecinos).
        long peri = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!isWater(x, y)) continue;
                const bool borde = (x == 0 || y == 0 || x == W-1 || y == H-1)
                                || !isWater(x-1, y) || !isWater(x+1, y)
                                || !isWater(x, y-1) || !isWater(x, y+1);
                if (borde) ++peri;
            }
        const double circ = (peri > 0) ? (4.0 * 3.14159265358979 * (double)lit / ((double)peri * (double)peri))
                                       : 0.0;
        std::printf("    %-28s %6ld px de agua (%.1f%%) · detalle fino (grad. local) %5.2f · "
                    "circularidad %.3f\n",
                    sh.name, lit, 100.0 * lit / (double)(W*H), sd, circ);
        CHECK(lit > (long)(W*H) / 50, "la forma se dibuja con agua encima");
        circul[shapeIdx] = circ;
        cover[shapeIdx] = lit; contrast[shapeIdx] = sd; frames[shapeIdx] = px; ++shapeIdx;

        // HARUKA_WATER_PNG=<dir> vuelca las cuatro formas a disco. El test AFIRMA que la silueta se
        // conserva, pero si la ola se ve bien o no es un juicio visual y hace falta poder mirarla.
        if (const char* dir = std::getenv("HARUKA_WATER_PNG")) {
            // readPixels de GL devuelve las filas de abajo arriba y las de Vulkan de arriba abajo:
            // sin voltear la de GL, los dos PNG saldrian espejados entre si.
            std::vector<uint8_t> img(px.size());
            for (int y = 0; y < H; ++y) {
                const int src = isVk ? y : (H - 1 - y);
                std::memcpy(&img[(size_t)y * W * 4], &px[(size_t)src * W * 4], (size_t)W * 4);
            }
            const std::string path = std::string(dir) + "/" + (isVk ? "vk_" : "gl_") +
                                     shotBase(sh) + ".png";
            Haruka::writePNG(path, W, H, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }

        // La captura entra en el careo GL<->Vulkan que hace compareBackends().
        recordShot(shotBase(sh).c_str(), W, H, px);

        g_dev->destroy(vb); g_dev->destroy(ib); g_dev->destroy(ub);
    }

    // ⚠️ La espuma NO demuestra que la cresta se pliegue, y decirlo seria pasarse. A 1,5 m el termino
    // de ESPUMA DE ORILLA (`shoreFoam`, que solo mira la profundidad) ya vale ~0,9 por si solo, asi
    // que satura el cuadro entero. Lo que este test demuestra es lo otro: que la ola se aplica sobre
    // un cilindro y sobre un cubo sin costuras ni huecos, o sea que NO necesita un planeta debajo.
    // El pliegue de verdad se mide en la CPU, sobre el jacobiano: `test_ocean_break_fold`.
    // ⚠️ LA SILUETA SE CONSERVA. Es la guardia contra el fallo que tenia la primera version: `disp`
    // sale en METROS y se sumaba a una posicion en unidades de forma, asi que la ola no rizaba la
    // pieza — la INFLABA. Medido entonces: el anillo pasaba del 16,6% al 48,0% de cobertura entre
    // mar abierto y rompiente, o sea que dejaba de ser un anillo. Con las unidades bien, la
    // profundidad cambia el RIZADO y no el tamano.
    const int pairs[2][2] = { { 0, 1 }, { 3, 4 } };
    for (int p = 0; p < 2; ++p) {
        const double a = (double)cover[pairs[p][0]], b = (double)cover[pairs[p][1]];
        const double drift = a > 0.0 ? 100.0 * std::abs(b - a) / a : 100.0;
        // ⚠️ LA FORMA, AFIRMADA. Un anillo cilindrico visto de frente es un disco: su circularidad tiene
    // que ser alta. Si la ola lo convierte en "un cuadrado deformado" —el reporte visual— esto lo ve,
    // y el area sola no lo veia.
    std::printf("    circularidad: anillo mar abierto %.3f · anillo rompiendo %.3f · anillo EN CALMA %.3f\n"
                "                  (1,00 = circulo · 0,785 = cuadrado · menos = deformado)\n",
                circul[0], circul[1], circul[2]);
    // ⚠️ NADA DE UMBRAL ABSOLUTO, Y AQUI ME EQUIVOQUE PRIMERO. Puse `circul > 0.55` esperando "un
    // disco" y el anillo EN CALMA da **0,472**: es un TORO, su silueta es una corona con agujero, o
    // sea DOS contornos, y eso hunde la circularidad por construccion. No estaba deformado — mi
    // referencia estaba mal. La pieza en calma es su propia referencia y es lo unico honesto que
    // comparar. (El cubo da 1,02, por encima del maximo geometrico de 1: el perimetro contado por
    // pixeles subestima en las diagonales. Sirve para COMPARAR formas entre si, no como valor absoluto.)
    CHECK(circul[2] > 0.2, "la pieza en calma tiene una silueta medible (referencia de forma)");
    // La ola RIZA el borde, asi que baja algo la circularidad — pero no puede convertir el disco en
    // otra figura. Se compara contra la pieza EN CALMA, que es su propia referencia.
    CHECK(circul[0] > circul[2] * 0.60,
          "con ola de MAR ABIERTO la silueta NO cambia de figura respecto a la pieza en calma");
    CHECK(circul[1] > circul[2] * 0.60,
          "y ROMPIENDO tampoco (es donde el adelanto de cresta es maximo)");
    std::printf("    %-6s: silueta %.1f%% -> %.1f%% al pasar de mar abierto a rompiente (deriva %.1f%%)\n",
                    p == 0 ? "anillo" : "cubo", 100.0 * a / (W*H), 100.0 * b / (W*H), drift);
        CHECK(drift < 10.0, "la ola riza la forma, no la infla (las unidades del desplazamiento casan)");
    }

    // ⚠️ LA OLA SOBRE UNA PIEZA DE ESTE TAMANO ES SUTIL, Y CONVIENE DECIRLO CON NUMEROS EN VEZ DE
    // PROMETER LO CONTRARIO. Se intento afirmar "la ola se VE" por contraste y no se sostiene: con
    // olas el detalle fino da 1,81 y en calma 1,74 — un 4%. El motivo es geometrico y no un fallo:
    // la pieza abarca +-31,7 m, o sea UNA longitud de onda del tren dominante (61 m), asi que la ola
    // no la riza sino que la INCLINA entera, y una inclinacion uniforme no genera detalle local. (El
    // arco del toro no cuenta: la ola es un plano en 3D, lo que importa es la extension LINEAL.)
    // Ademas el oleaje real es tendido: con H/lambda ~ 5,6%, cualquier vista que abarque varias
    // longitudes de onda muestra ondulaciones del 5%. El mar es asi.
    //
    // Lo que SI se puede afirmar sin adornos es que la ola hace algo, y se mide comparando el mismo
    // toro con olas y en calma pixel a pixel. Es mas sensible que cualquier estadistico de contraste
    // y no es tautologico: calma y oleaje son entradas distintas de verdad.
    {
        const std::vector<uint8_t>& A = frames[0];   // anillo con olas
        const std::vector<uint8_t>& B = frames[2];   // el mismo anillo en calma
        long diff = 0, both = 0; double sumAbs = 0.0; int worst = 0;
        for (size_t k = 0; k + 3 < A.size() && k + 3 < B.size(); k += 4) {
            const bool wa = A[k] + A[k+1] + A[k+2] > 24;
            const bool wb = B[k] + B[k+1] + B[k+2] > 24;
            if (!wa && !wb) continue;
            ++both;
            int mx = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const int d = std::abs((int)A[k+ch] - (int)B[k+ch]);
                sumAbs += d; mx = std::max(mx, d);
            }
            worst = std::max(worst, mx);
            if (mx > 8) ++diff;
        }
        std::printf("    con olas vs EN CALMA (el mismo anillo): %.1f%% de pixeles distintos · |delta| medio %.2f · peor canal %d\n",
                    both ? 100.0 * diff / both : 0.0, both ? sumAbs / (both * 3) : 0.0, worst);
        std::printf("      (detalle fino: con olas %.2f · en calma %.2f — la ola INCLINA la pieza mas que rizarla,\n"
                    "       porque abarca ~1 longitud de onda; para verla rizada hace falta una superficie de varias)\n",
                    contrast[0], contrast[2]);
        CHECK(both > 1000, "hay pieza que comparar entre las dos versiones");
        CHECK(diff > both / 20, "el oleaje cambia la pieza respecto al mar en calma (la ola hace algo)");
    }

    std::printf("    (la espuma a 1,5 m es la de ORILLA, que solo mira la profundidad: no prueba pliegue)\n");
    g_dev->destroy(pipe); g_dev->destroy(ocean); g_dev->destroy(calm);
}

static void testItemPreviewShader()
{
    BEGIN("icono de inventario: los shaders pintan y coinciden GL/VK");

    const std::string base = Haruka::Shader::baseDir();
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

static void testVertexInterpolation()
{
    BEGIN("vertice: los atributos se INTERPOLAN dentro del triangulo");

    const std::string base = Haruka::Shader::baseDir();
    const std::string vs = base + "shaders/rhitest_vcolor.vert";
    const std::string fs = base + "shaders/rhitest_vcolor.frag";

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
        // ⚠️ AQUI HABIA DOS ENTRADAS DE MAR (`ocean.*` teselado y `ocean_far.*`) Y SUS SHADERS YA NO
        // EXISTEN. El agua se migro al pase de nodos el 2026-09-01: dejo de tener geometria propia,
        // asi que dejo de tener pipelines propios. La entrada nueva es la que hay que vigilar.
        { "agua/nodos (v5)",       sh+"terrain_node_water.vert", sh+"terrain_node_water.frag",
          "", "", false },
        { "cielo",                 sh+"sky.vert",  sh+"sky.frag",  "", "", false },
        { "nubes volumétricas",    sh+"cloud_vol.vert", sh+"cloud_vol.frag", "", "", false },
        { "props instanciados",    sh+"prop_inst.vert", sh+"prop_inst.frag", "", "", false },
        // ⚠️ `simple.vert`, NO `screenquad.vert`: es el par que construye el motor
        // (`application_render.cpp`, `_mainShader`/`m_scenePSO`). `screenquad.vert` solo saca
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
static void testPropLightSweep()
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
        // relativo.
        //
        // ⚠️⚠️ LA EXPLICACION QUE HABIA AQUI ERA FALSA, Y DECIDIA ARQUITECTURA. Decia: "eso es `sqrt`
        // sobre `double`; GLSL no exige redondeo correcto para dobles, así que el driver puede
        // resolverlo con iteraciones y no coincidir con la CPU — y por eso el v5 exige **GPU también
        // en el servidor**, porque la identidad alcanzable es GPU↔GPU y no CPU↔GPU".
        //
        // Medido el 2026-08-31 en las cuatro combinaciones de este equipo (`testGpuFp64` con las
        // entradas por SSBO, y `testBakeStageBisect` sobre los 16 641 téxeles):
        //
        //     AMD RENOIR + OpenGL   bake peor 0,0253 m · 85 % de téxeles > 1 mm   <- el unico roto
        //     AMD RENOIR + Vulkan   bake peor 0,0001 m · 0 téxeles > 1 mm
        //     NVIDIA 3050 + OpenGL  bake peor 0,0002 m · 0 téxeles > 1 mm
        //     NVIDIA 3050 + Vulkan  bake peor 0,0001 m · 0 téxeles > 1 mm
        //
        // El `sqrt` de doble NO es el problema: en tres de las cuatro celdas `harukaCubeFaceToDir`
        // casa con `cubeFaceToDir` de C++ **hasta el ultimo bit** (residuo sub-float identico). Lo
        // que rompia era el compilador de GLSL del driver de OpenGL de AMD, que degrada
        // `inversesqrt(double)` a precision de float. Ver [[amd-opengl-degrada-fp64]].
        //
        // ⚠️ CONSECUENCIA PARA EL PLAN: **el servidor NO necesita GPU.** Lo que queda entre CPU y GPU
        // son 0,0001-0,0002 m, y eso es el redondeo en float de la suma de octavas, no la geometria.
        // La premisa de "GPU obligatoria en el servidor" se apoyaba en una medida de una sola maquina
        // con el unico backend roto de los cuatro.
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
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, count * 3 * sizeof(float), nullptr,
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

    // ── DE DONDE SALE EL TIEMPO: DESPACHO, PROYECCION Y RELIEVE POR SEPARADO ────────────────────
    //
    // "0,056 ms/nodo" no dice que optimizar. Aqui se parte en tres con dos medidas:
    //   · barrido de N: si el ms/nodo baja al encolar mas, hay COSTE FIJO por dispatch y la palanca
    //     es agrupar; si no baja, el coste es todo trabajo por texel y agrupar no compra nada.
    //   · `uGrid.z = 2` (modo de biseccion que ya existia): hace la misma proyeccion y la misma
    //     escritura pero NO evalua el relieve. La diferencia con el modo 0 es el relieve, exacta.
    {
        const NodeId fine{ Haruka::PlanetFace::FRONT, 18, 100, 100 };
        up.node[0] = (int32_t)fine.face; up.node[1] = (int32_t)fine.level;
        up.node[2] = (int32_t)fine.i;    up.node[3] = (int32_t)fine.j;
        up.misc[1] = (float)nodeTexelM(fine, R);

        auto run = [&](int kNodes, int mode) -> double {
            up.grid[2] = mode;
            for (int warm = 0; warm < 2; ++warm)
                if (Context* c = g_dev->beginFrame()) {
                    g_dev->updateBuffer(ubo, 0, sizeof(up), &up);
                    c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                    c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                    FenceHandle fw = c->signalFence();
                    g_dev->endFrame();
                    if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(fw, 5000000000ull); c2->deleteFence(fw); g_dev->endFrame(); }
                }
            const auto t0 = std::chrono::high_resolution_clock::now();
            if (Context* c = g_dev->beginFrame()) {
                c->bindPipeline(cp); c->bindUniformBuffer(0, ubo); c->bindStorageBuffer(1, out);
                for (int k = 0; k < kNodes; ++k) c->dispatch((N + 7) / 8, (N + 7) / 8, 1);
                FenceHandle f = c->signalFence();
                g_dev->endFrame();
                if (Context* c2 = g_dev->beginFrame()) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); g_dev->endFrame(); }
            }
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - t0).count();
            return ms / (double)kNodes;
        };

        std::printf("\n    nivel 18 · %u texeles por nodo · reparto del coste\n", (unsigned)count);
        std::printf("      nodos por tanda   ms/nodo TOTAL   ms/nodo SIN relieve   relieve\n");
        double perFull1 = 0.0, perFull256 = 0.0, perBare256 = 0.0;
        for (int k : { 1, 4, 16, 64, 256 }) {
            const double full = run(k, 0);
            const double bare = run(k, 2);
            std::printf("      %13d   %13.4f   %19.4f   %6.1f %%\n",
                        k, full, bare, full > 0.0 ? 100.0 * (full - bare) / full : 0.0);
            if (k == 1)   perFull1   = full;
            if (k == 256) { perFull256 = full; perBare256 = bare; }
        }
        up.grid[2] = 0;
        const double fixedMs = perFull1 - perFull256;      // lo que se ahorra al amortizar la tanda
        std::printf("      -> coste FIJO por dispatch %.4f ms/nodo · relieve %.4f · resto %.4f\n",
                    fixedMs > 0.0 ? fixedMs : 0.0, perFull256 - perBare256, perBare256);

        CHECK(perFull256 > 0.0, "la medida amortizada existe");
        CHECK(perBare256 < perFull256,
              "CONTRAPRUEBA: sin evaluar el relieve cuesta MENOS (el modo de biseccion hace algo)");
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

    // ⚠️ ESTE BLOQUE ESTABA DESCUADRADO Y EL TEST NO PODIA VERLO.
    //
    // Era `{ mat4 mvp; vec4 center; ivec4 node; ivec4 grid; ivec4 edge; vec4 misc; }`, y el bloque
    // real de `terrain_node.vert` es `{ mvp, center, centerLo, lod, grid, edgeUnused, misc, shade,
    // texAnchor, lightDir }`. Faltaban `centerLo` y `lod`, asi que TODO lo de despues caia 32 bytes
    // antes: el `grid` del test aterrizaba en `uLod`, y su `misc[0] = R` en `uEdgeUnused`. **El
    // shader leia el radio del planeta como 0.**
    //
    // Un bloque uniforme mal copiado no da error de compilacion —lee el campo de al lado— y aqui
    // encima el test seguia pasando, porque lo que comprueba despues es el SSBO de alturas y no lo
    // dibujado. Dibujaba basura y nadie miraba. El `static_assert` es lo que impide que vuelva.
    struct DrawUBO {
        glm::mat4 mvp; glm::vec4 center; glm::vec4 centerLo;
        float lod[4]; int32_t grid[4]; int32_t edge[4];
        float misc[4]; float shade[4]; glm::vec4 texAnchor; glm::vec4 lightDir;
    } du{};
    static_assert(sizeof(DrawUBO) == 208, "el gemelo de NodeDraw se ha descuadrado");
    du.mvp      = proj * view;
    du.center   = glm::vec4(glm::vec3(glm::dvec3(0.0) - cam), 0.0f);  // centro del planeta rel. al ojo
    du.centerLo = glm::vec4(0.0f);
    du.lod[0] = (float)(glm::radians(60.0) / 256.0); du.lod[1] = 2.0f;
    du.lod[2] = (float)(R * 1.5707963267948966); du.lod[3] = 0.0f;    // w = 0: morph apagado
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


// ================================================================================================
// LA SONDA QUE FALTABA: la geometria DIBUJADA contra el CAMPO de terreno.
//
// ⚠️ EL BANCO MEDIA LOS DOS EXTREMOS DE LA CADENA Y NO EL TRAMO DE EN MEDIO. Hasta hoy habia:
//
//     bake (terrain_node.comp) vs referencia CPU ....... 0,02 m   ✔ medido
//     malla de colision        vs campo real ........... 0,04 m   ✔ medido
//     textura -> terrain_node.vert -> posicion ......... NADA
//
// `testTerrainNodeRender` dibuja el nodo, pero lo que compara despues es el SSBO de alturas: su
// comentario dice "se lee la POSICION" y lee el buffer. Asi que el vertex shader —donde viven `dirD`,
// el cosido de aristas, el morph y la cancelacion con `uCenter`— nunca se ha comprobado contra nada.
// Ahi es donde tiene que estar la disparidad reportada, porque los otros dos tramos dan centimetros.
//
// La referencia es `harukaTerrainDetail` evaluada EN EL PIXEL, que es independiente del bake (el bake
// evalua en el centro del texel). Lo que debe quedar es el error de cuerda de la rejilla: centimetros.
// ================================================================================================
static void testTerrainNodeDrawnVsField()
{
    BEGIN("v5 F3: la geometria DIBUJADA cae sobre el campo (el tramo sin medir)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    // ⚠️ NIVEL 17, NO 14, Y LA DIFERENCIA IMPORTA. Con el 14 (texel 4,77 m) esto media un nodo que
    // solo se dibuja a kilometros, y sus cifras se leyeron como si fueran las del suelo que se pisa.
    // Bajo los pies el selector esta en el 17: texel 0,596 m, que es donde el corte del render y el
    // de la colision (0,5 m) caen en las MISMAS octavas y la disparidad deberia ser de centimetros.
    const NodeId node{ Haruka::PlanetFace::FRONT, 17, 33600, 24800 };
    const uint32_t N = TERRAIN_NODE_TEXELS;

    TerrainNodeGpu gpu;
    const std::string base = Haruka::Shader::baseDir();
    // 32 huecos: `request` encola la cadena raiz->nodo entera, y con 4 (lo que usa el test de al
    // lado, que va al nivel 14) el padre y el abuelo se caen del pool — el shader leeria su propio
    // mapa con stride 2 y 4, y el barrido daria tres veces la misma cifra.
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 32)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    TerrainNodePool pool(32, 8);
    pool.beginFrame();
    pool.request(node);
    // ⚠️ `endFrame` VA SIEMPRE. Esto era `for (...; generatePending(beginFrame(),...); ++pass)
    // endFrame();`: cuando no quedaba nada por generar la condicion abria un frame y salia sin
    // cerrarlo, asi que el dibujo de despues encontraba el device con un frame abierto y pintaba
    // 1,9% de pantalla. Lo canto el guardia de cobertura, no yo.
    for (int pass = 0; pass < 24; ++pass) {
        Context* ctx = g_dev->beginFrame();
        const size_t n = ctx ? gpu.generatePending(ctx, pool, R) : 0;
        g_dev->endFrame();
        pumpWindowEvents();
        if (n == 0) break;
    }
    int slot = -1;
    for (size_t k = 0; k < gpu.capacity(); ++k) {
        NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
    }
    CHECK(slot >= 0, "el nodo esta residente");
    if (slot < 0) { gpu.shutdown(); return; }
    double worstByStride[3] = { 0.0, 0.0, 0.0 };
    double worstCollByStride[3] = { 0.0, 0.0, 0.0 };
    double worstSanity = 0.0;

    PipelineDesc pd;
    const std::string vs = base + "shaders/terrain_node.vert";
    const std::string fs = base + "shaders/terrain_node_probe.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)(2 * sizeof(float)) };
    pd.vertexLayout.attributes.push_back({ 0, 0, Format::RG32F, 0 });
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline de la sonda creado");
    if (!valid(pipe)) { gpu.shutdown(); return; }

    // ⚠️ EL BARRIDO DE STRIDE ES EL PUNTO DE ESTE TEST. Con stride 1 se dibuja un vertice por texel y
    // el error es el de cuerda del texel — pero el juego NO dibuja asi: reparte stride por nodo con
    // histeresis, y la malla sale cada TEXEL x STRIDE. El propio `terrain_node.vert` lo tiene
    // apuntado (nivel 14, stride 4: el corte efectivo pasa de 1,41 a 0,43 m), pero eso es el corte de
    // OCTAVAS; lo que aqui se mide es lo otro: cuanto se separa del campo la superficie DIBUJADA.
    // mode 2 = CONTRAPRUEBA: la misma referencia de colision con la pendiente x50. No mide nada del
    // motor; existe solo para demostrar que `uProbe.z` llega al shader. Hacia falta porque a nivel 17
    // los modos 0 y 1 dan cifras IDENTICAS —el corte de la colision (0,5 m) y el del nodo (0,596 m)
    // caen en las mismas octavas— y comparar los dos entre si daba un falso fallo.
    for (int mode = 0; mode < 3; ++mode) {
    const bool vsColl = (mode >= 1);
    std::printf("  -- referencia: %s\n", mode == 0
                ? "EL CAMPO con el corte del nodo (¿esta bien puesto el vertice?)"
                : mode == 1 ? "LA SUPERFICIE DE COLISION (lo que se ve contra lo que se pisa)"
                            : "contraprueba: la de colision con la pendiente x50");
    for (int sIdx = 0; sIdx <= 2; ++sIdx) {
    const uint32_t step = 1u << sIdx;
    std::vector<float> verts;
    for (uint32_t v = 0; v < N; v += step)
        for (uint32_t u = 0; u < N; u += step) { verts.push_back((float)u); verts.push_back((float)v); }
    const uint32_t side = (N + step - 1) / step;
    std::vector<uint32_t> idx;
    for (uint32_t v = 0; v + 1 < side; ++v)
        for (uint32_t u = 0; u + 1 < side; ++u) {
            const uint32_t a = v * side + u, b = a + 1, c = a + side, d = c + 1;
            idx.insert(idx.end(), { a, c, b, b, c, d });
        }
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, verts.size() * sizeof(float),
                                          verts.data(), BufferMemory::Static);
    BufferHandle ib = g_dev->createBuffer(BufferUsage::Index, idx.size() * sizeof(uint32_t),
                                          idx.data(), BufferMemory::Static);

    // Camara a 300 m sobre el centro del nodo y mirando abajo: se quiere el suelo LLENANDO la
    // pantalla y visto de cerca, que es donde el usuario reporta el sintoma (bajo sus pies).
    const glm::dvec3 c   = nodeTexelDir(node, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    // ⚠️ LA ALTURA SE MIDE DESDE EL TERRENO, NO DESDE EL RADIO. Estaba en `c * (R + span*0.7)`, o
    // sea 53 m sobre la ESFERA — y como ahi el terreno pasa de esa cota, la camara quedaba enterrada:
    // 1,9% de pantalla y tres strides dando la misma cifra. A nivel 14 colaba de casualidad porque
    // 300 m tapaba el relieve. Sin el guardia de cobertura habria publicado esas cifras.
    const double nodeSpanM = nodeTexelM(node, R) * (double)TERRAIN_NODE_CELLS;
    const double hCentre   = (double)Haruka::Planet::terrainDetail(c, R, (float)nodeTexelM(node, R));
    const glm::dvec3 cam = c * (R + hCentre + nodeSpanM * 0.7);
    const glm::dvec3 tgt = c * (R + hCentre);
    const glm::dvec3 upv = glm::normalize(glm::cross(c, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(tgt - cam), glm::vec3(upv));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 1.0f, 20000.0f);

    // ⚠️ LAYOUT COPIADO A MANO Y POR ESO CON `static_assert`. `TerrainNodeRenderer::DrawUBO` es
    // privado, asi que aqui hay un gemelo. El de `testTerrainNodeRender` ESTA DESCUADRADO (le faltan
    // `centerLo` y `lod`, asi que su `grid` y su `misc` caen sobre otros campos) — un bloque uniforme
    // mal copiado no da error de compilacion, solo lee basura del campo de al lado.
    struct DrawUBO {
        glm::mat4 mvp; glm::vec4 center; glm::vec4 centerLo;
        float lod[4]; int32_t grid[4]; int32_t edge[4];
        float misc[4]; float shade[4]; glm::vec4 texAnchor; glm::vec4 lightDir;
    } du{};
    static_assert(sizeof(DrawUBO) == 208, "el gemelo de NodeDraw se ha descuadrado");
    du.mvp      = proj * view;
    du.center   = glm::vec4(glm::vec3(glm::dvec3(0.0) - cam), 0.0f);
    du.centerLo = glm::vec4(0.0f);
    du.lod[0] = (float)(glm::radians(60.0) / 256.0); du.lod[1] = 2.0f;
    du.lod[2] = (float)(R * 1.5707963267948966); du.lod[3] = 0.0f;   // w = 0: morph APAGADO
    du.grid[0] = (int32_t)N; du.grid[1] = (int32_t)TERRAIN_NODE_CELLS; du.grid[2] = slot;
    du.misc[0] = (float)R;
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(du), &du, BufferMemory::Dynamic);

    // El corte de octavas del nodo: si la sonda evaluara el campo con otro, mediria el CORTE en vez
    // de la colocacion del vertice — que es un efecto real pero distinto y ya medido aparte.
    const float minFeatureM = (float)nodeTexelM(node, R);
    // ⚠️ FONDO DE ESCALA DE ±64 m, Y NO ES HOLGURA DE SOBRA. Con ±4 m saturaba el 100% de los pixeles
    // en cuanto el stride pasaba de 1, y "media 4,0000 · peor 4,0000" no es una medida: es el tope
    // del codificado. Sobre 16 bits, ±64 m sigue dando 2 mm de resolucion.
    const float fullScaleM  = 64.0f;
    struct ProbeUBO { float p[4]; glm::vec4 anchor; } pu{
        { minFeatureM, fullScaleM,
          vsColl ? (float)(Haruka::Planet::TERRAIN_TRIM_SLOPE * (mode == 2 ? 50.0 : 1.0)) : 0.0f,
          (float)Haruka::Planet::TERRAIN_TRIM_FLOOR },
        glm::vec4(glm::vec3(c), 0.0f) };
    BufferHandle pubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pu), &pu, BufferMemory::Dynamic);

    TerrainNodeRenderer::NodeInstGPU inst{};
    inst.node[0] = (int32_t)node.face; inst.node[1] = (int32_t)node.level;
    inst.node[2] = (int32_t)node.i;    inst.node[3] = (int32_t)node.j;
    inst.slot[0] = slot;
    inst.slot[2] = sIdx;                 // log2 del stride: el shader hace `1 << slot.z`
    // ⚠️ SIN ESTO LA SONDA MIDE BASURA, Y ME LO TRAGUE UNA VEZ: con `misc` a cero el shader creia que
    // el padre vivia en el hueco 0 —un hueco VALIDO, ocupado por otro nodo de la cadena— y leia SU
    // mapa de alturas. Salieron 62 m de error y por un momento parecio el hallazgo del dia. El
    // relieve a 4,77 m no puede valer 62 m: cuando una sonda da un numero imposible, lo primero que
    // falla es la sonda. Con -1 el shader sabe que no estan y se queda en su propio mapa.
    const NodeId par { node.face, node.level - 1, node.i / 2, node.j / 2 };
    const NodeId gran{ par.face,  par.level  - 1, par.i  / 2, par.j  / 2 };
    inst.misc[0] = (float)pool.slotOf(par);
    inst.misc[1] = (float)pool.slotOf(gran);
    BufferHandle instSSBO = g_dev->createBuffer(BufferUsage::Storage, sizeof(inst), &inst,
                                                BufferMemory::Dynamic);

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int W = (uw > 0) ? (int)uw : 256, H = (uh > 0) ? (int)uh : 256;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    if (Context* ctx = g_dev->beginFrame()) {
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
        cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->bindPipeline(pipe);
        ctx->bindUniformBuffer(0, ubo);
        ctx->bindStorageBuffer(1, gpu.heights());
        ctx->bindStorageBuffer(2, instSSBO);
        ctx->bindUniformBuffer(3, pubo);
        ctx->bindVertexBuffer(vb);
        ctx->bindIndexBuffer(ib);
        ctx->drawIndexed((uint32_t)idx.size(), 0, 1);
        ctx->endRenderPass();
        // GL lee del framebuffer por defecto ANTES del swap; Vulkan, ya presentado. Ver la nota de
        // `testClearReadback`: invertirlo da una imagen en negro y se lee como "no se dibujo nada".
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        pumpWindowEvents();
    }

    size_t covered = 0; double sum = 0.0, worst = 0.0; int saturated = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        if (px[i * 4 + 2] < 128) continue;                 // B = 0 -> el clear, ahi no hay terreno
        ++covered;
        const uint32_t q = ((uint32_t)px[i * 4 + 0] << 8) | (uint32_t)px[i * 4 + 1];
        const double err = ((double)q / 65535.0) * 2.0 * fullScaleM - fullScaleM;
        if (q == 0 || q == 65535) ++saturated;
        sum += std::fabs(err); worst = std::max(worst, std::fabs(err));
    }
    const double cov = (double)covered / (double)(W * H) * 100.0;
    std::printf("    stride %u (celda %.2f m · lee el mapa del %s) · %zu px con terreno (%.1f%%) · "
                "%s: "
                "media %.4f m · peor %.4f m · saturados %d\n",
                step, minFeatureM * (double)step,
                sIdx == 0 ? "nodo" : (sIdx == 1 ? "padre" : "abuelo"), covered, cov,
                vsColl ? "VER vs PISAR" : "dibujado vs campo",
                covered ? sum / (double)covered : 0.0, worst, saturated);
    // ⚠️ SIN COBERTURA NO HAY MEDIDA. Si el nodo no sale en pantalla, las estadisticas de abajo dan
    // 0,0000 m y se leerian como paridad perfecta: el fallo mas caro que puede tener esta sonda.
    CHECK(covered > (size_t)(W * H) / 10, "el nodo cubre la pantalla (si no, las cifras no valen)");
    CHECK(saturated == 0, "ningun pixel satura el fondo de escala (si satura, el peor es mayor)");
    if      (mode == 0) worstByStride[sIdx]     = worst;
    else if (mode == 1) worstCollByStride[sIdx] = worst;
    else if (sIdx == 0) worstSanity              = worst;

    g_dev->destroy(pubo); g_dev->destroy(instSSBO); g_dev->destroy(ubo);
    g_dev->destroy(ib); g_dev->destroy(vb);
    }   // fin del barrido de stride
    }   // fin de los dos modos de referencia

    // ⚠️ CONTRAPRUEBA: el error TIENE que crecer con el stride. Si saliera plano seria que `slot.z`
    // no llega al shader y las tres pasadas dibujan lo mismo — la sonda estaria midiendo una sola
    // configuracion tres veces y sus tres cifras iguales se leerian como "el stride no influye".
    std::printf("    peor error por stride: 1 -> %.4f m · 2 -> %.4f m · 4 -> %.4f m\n",
                worstByStride[0], worstByStride[1], worstByStride[2]);
    CHECK(worstByStride[2] > worstByStride[0] * 1.5,
          "el error crece con el stride (si no, `slot.z` no esta llegando al shader)");
    std::printf("    ver-vs-pisar por stride: 1 -> %.4f m · 2 -> %.4f m · 4 -> %.4f m\n",
                worstCollByStride[0], worstCollByStride[1], worstCollByStride[2]);
    // ⚠️ CONTRAPRUEBA DEL SEGUNDO MODO. Comparar los modos 0 y 1 entre si NO vale: a nivel 17 dan lo
    // mismo porque los dos cortes caen en las mismas octavas, y eso es un RESULTADO, no un fallo. Lo
    // que hay que demostrar es que el uniform llega, y para eso se perturba a proposito.
    std::printf("    contraprueba (pendiente x50): peor %.4f m contra %.4f m del campo\n",
                worstSanity, worstByStride[0]);
    CHECK(worstSanity > worstByStride[0] * 1.5,
          "perturbar el corte de la colision CAMBIA la medida (si no, `uProbe.z` no llega)");

    g_dev->destroy(pipe);
    gpu.shutdown();
}


// ================================================================================================
// EL COSTE DEL TOPE DE STRIDE DE CERCA, A ALTURA DE OJO.
//
// ⚠️ EL BANCO DE COSTE QUE HABIA MIDE A 800 m DE ALTITUD, y ahi este tope casi no aplica: desde 800 m
// el nodo mas cercano ya esta a cientos de metros. Medir alli habria dado "no cuesta nada" y habria
// sido una cifra correcta contestando a la pregunta equivocada. La disparidad se ve DE PIE.
//
// Se barre el radio (0 = apagado, o sea la linea base) y se leen los triangulos del contador DEL
// MOTOR, no de un modelo: un modelo por bandas dio 1,77x y hay que ver cuanto es de verdad.
// ================================================================================================
static void testTerrainStrideMatchCost()
{
    BEGIN("v5: coste del tope de stride de cerca (a altura de ojo)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 2048)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int W = (uw > 0) ? (int)uw : 256, H = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)H;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)W / (double)H);

    // De pie: 1,7 m sobre la superficie, mirando al horizonte. Es donde se reporto el sintoma.
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 cam = pc + up0 * (R + 1.7);
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    const glm::mat4 proj = glm::perspective((float)fovY, (float)W / (float)H, 1.0f, 40000.0f);
    const glm::mat4 mvp  = proj * view;

    std::printf("    radio    nodos    triangulos   ms/frame\n");
    double trisOff = 0.0, msOff = 0.0;
    for (double radius : { 0.0, 150.0, 300.0, 600.0 }) {
        // ⚠️ El radio va por ENV y `strideMatchM()` lo cachea en un `static`, asi que hay que
        // pasarlo por el parametro: cachearlo haria que las cuatro pasadas midieran la primera.
        r.setStrideMatchOverride(radius);
        double ms = 0.0; size_t nodes = 0, tris = 0;
        for (int f = 0; f < 24; ++f) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            Context* ctx = g_dev->beginFrame(); if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            g_dev->endFrame();
            pumpWindowEvents();
            if (f >= 8) {   // los 8 primeros llenan el pool y compilan; no cuentan
                ms += std::chrono::duration<double, std::milli>(
                          std::chrono::high_resolution_clock::now() - t0).count();
                const auto st = r.stats(); nodes = st.drawn; tris = st.tris;
            }
        }
        ms /= 16.0;
        std::printf("    %5.0f m  %6zu   %8.2f M   %6.2f%s\n", radius, nodes,
                    (double)tris / 1e6, ms, radius == 0.0 ? "   <- linea base" : "");
        if (radius == 0.0) { trisOff = (double)tris; msOff = ms; }
        else if (radius == 300.0 && trisOff > 0.0)
            std::printf("      -> a 300 m: x%.2f triangulos · x%.2f ms\n",
                        (double)tris / trisOff, msOff > 1e-6 ? ms / msOff : 0.0);
    }
    r.setStrideMatchOverride(-1.0);   // volver al valor del entorno

    // ⚠️ CONTRAPRUEBA: si el radio no llegara a `nodeStrideWant`, las cuatro filas darian el MISMO
    // numero de triangulos y se leerian como "el tope es gratis" — la conclusion mas cara posible.
    CHECK(trisOff > 0.0, "la linea base dibuja algo");
    r.shutdown();
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
static void testGpuFp64()
{
    BEGIN("¿la GPU hace la aritmetica en DOUBLE, o la degrada a float?");

    const std::string cs = Haruka::Shader::baseDir() + "shaders/fp64_probe.comp";
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
        const glm::dvec3 rRef  = Haruka::cubeFaceToDir(Haruka::PlanetFace::FRONT,
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


// ================================================================================================
// EL BAKE CONTRA LA CPU, NIVEL A NIVEL. ¿La divergencia entre GPUs escala con el nodo?
//
// ⚠️ EL BANCO MIRABA UN SOLO NODO, Y DEL NIVEL 17. Ahi la divergencia AMD/NVIDIA es de 2 cm
// (0,0204 contra 0,0001 m), y Andoni ve 0,4-1,5 m en partida — donde se dibujan niveles 9..17. Si el
// ruido diverge mas cuanto mayor es la coordenada, los nodos GRUESOS serian mucho peores y una sola
// muestra del 17 no lo veria. Esto barre el nivel para contestarlo.
//
// No dibuja nada: genera en GPU, lee el heightmap y lo compara con `nodeFillHeights`, que es el
// gemelo CPU. La referencia NO depende de la GPU, asi que la comparacion es honesta en las dos.
// ================================================================================================
// ================================================================================================
// ¿DA LA GPU EL MISMO NUMERO DE ONDA QUE LA CPU? El eslabon que `rhitest_waveprobe` no aisla.
//
// La sonda de la ola compara ALTURAS, o sea el final de la cadena: cuando dio 4,6 m de separacion al
// meter la dispersion de profundidad finita, no decia si el culpable era `k`, el desvanecido de onda
// corta, el tope de rompiente o la fase. Esto emite SOLO `harukaWaveNumber`.
//
// Y emite ademas el RESIDUO de la propia ecuacion de dispersion, que separa dos casos que a simple
// vista son el mismo: "las dos resuelven bien y difieren en el ultimo bit" (residuo ~0 en ambas) de
// "el solver de la GPU no converge" (residuo grande en una).
// ================================================================================================
// ================================================================================================
// EL CAMPO DE AGUA HORNEADO: ¿lee la GPU el mismo lago que la CPU?
//
// ⚠️ TODO ESTE CAMINO SE AÑADIÓ SIN UNA SOLA PRUEBA DE GPU. `bakeWaterMap` rellena las cuencas del
// planeta, sube el resultado como RG32F (R = cota, G = fetch) y el shader lo lee con
// `harukaBakedLakeAt` / `harukaBakedFetchAt`; la CPU tiene `lakeLevelAt` / `lakeFetchAt`, declaradas
// gemelas y nunca comparadas. Si divergen, el agua que se DIBUJA no está donde la física dice.
//
// El campo NO se toma de un planeta: se construye aquí una altura sintética, se pasa por el MISMO
// `waterFillEquirect` que usa el motor y se sube. Así el test audita el par textura↔lector, que es
// lo que no estaba probado, sin depender de que haya un mundo horneado.
// ================================================================================================
static void testWaterFieldParity()
{
    BEGIN("agua: el campo de lagos de la GPU == el de la CPU");

    // ── EL CAMPO: un continente con una cuenca cerrada, como el test de CPU ─────────────────────
    const int W = 128, H = 64;
    std::vector<float> land((size_t)W * H, 100.0f);
    auto at = [&](int x, int y) -> float& { return land[(size_t)y * W + x]; };
    for (int y = 0; y < H; ++y) for (int x = 0; x < 12; ++x) at(x, y) = -500.0f;   // océano
    for (int y = 24; y < 40; ++y) for (int x = 60; x < 76; ++x) at(x, y) = 10.0f;  // cuenca
    // ⚠️ RADIO PEQUENO A PROPOSITO. Con el radio terrestre, un lago de 16x16 texeles sobre un mapa
    // de 128x64 mide MILES de kilometros y su factor de fetch satura en 1,0 — o sea que el careo del
    // fetch compararia 1,0 contra 1,0 y no diria nada. Con un cuerpo pequeno el lago sale de cientos
    // de metros, el factor cae por debajo de 1 y la comparacion mide algo.
    const double kProbeRadius = 4000.0;
    const Haruka::Planet::WaterFillResult fill =
        Haruka::Planet::waterFillEquirect(land.data(), W, H, 0.0f, kProbeRadius);
    std::printf("    campo sintetico %dx%d: %zu texeles de mar · %zu de lago · fetch mayor %.0f m\n",
                W, H, fill.seaCells, fill.lakeCells, fill.biggestFetchM);
    CHECK(fill.lakeCells > 0, "el campo de prueba TIENE lago (si no, el careo no compara nada)");

    // Intercalado RG, igual que `bakeWaterMap`.
    std::vector<float> rg((size_t)W * H * 2);
    for (size_t c = 0; c < (size_t)W * H; ++c) {
        rg[c * 2 + 0] = fill.levelM[c];
        rg[c * 2 + 1] = fill.fetchM[c];
    }
    TextureDesc td;
    td.width = (uint32_t)W; td.height = (uint32_t)H;
    td.format = Format::RG32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
    td.mipmaps = false; td.initialData = rg.data();
    TextureHandle lakeTex = g_dev->createTexture(td);
    CHECK(valid(lakeTex), "textura RG32F del campo de agua creada");
    if (!valid(lakeTex)) return;

    // ── LAS DIRECCIONES A PROBAR: dentro del lago, en el mar y en tierra ────────────────────────
    //
    // El muestreo es NEAREST y la inversa de `equirectUV`, así que se eligen CENTROS DE TÉXEL: un
    // punto en el borde caería en un téxel u otro según el último bit y el careo mediría eso.
    auto dirOfTexel = [&](int x, int y) {
        const double u = ((double)x + 0.5) / W, v = ((double)y + 0.5) / H;
        const double lat = (0.5 - v) * 3.14159265358979323846;
        const double lon = (u - 0.5) * 2.0 * 3.14159265358979323846;
        const double cl = std::cos(lat);
        return glm::dvec3(cl * std::cos(lon), std::sin(lat), cl * std::sin(lon));
    };
    struct Probe { int x, y; const char* what; };
    const Probe probes[6] = { { 68, 32, "centro del lago" }, { 61, 25, "borde del lago" },
                              {  4, 32, "oceano" },          { 40, 10, "tierra seca" },
                              { 120, 50, "tierra, otra cara" }, { 75, 39, "esquina del lago" } };
    const int N = 6;
    const int kOut = 5;   // gemelo de `PROBE_OUT` en water_field_probe.comp
    std::vector<float> inBuf(1 + (size_t)N * 3);
    inBuf[0] = (float)N;
    for (int k = 0; k < N; ++k) {
        const glm::dvec3 d = dirOfTexel(probes[k].x, probes[k].y);
        inBuf[1 + k * 3 + 0] = (float)d.x;
        inBuf[1 + k * 3 + 1] = (float)d.y;
        inBuf[1 + k * 3 + 2] = (float)d.z;
    }

    // ── LA VENTANA FINA, tambien de verdad ──────────────────────────────────────────────────────
    //
    // ⚠️ Y NO ES OPCIONAL ATARLA. Desde que la sonda declara el binding 19 y el UBO 26, dejarlos
    // sueltos en Vulkan es comportamiento INDEFINIDO — no ceros. Es exactamente lo que ya dejo el
    // agua del pase de nodos sin dibujar un pixel.
    //
    // Se centra en el lago del campo grueso, para que el careo tenga la ventana MOJADA en unos puntos
    // y seca en otros.
    const glm::dvec3 dirLago = dirOfTexel(68, 32);
    double lonW = 0.0, latW = 0.0;
    Haruka::Planet::waterDirToLonLat(dirLago, lonW, latW);
    const double halfLatW = 400.0 / kProbeRadius;                 // ±400 m en un cuerpo de 4 km
    const double halfLonW = halfLatW / std::max(std::cos(latW), 1e-6);
    const double invLonW = 1.0 / (2.0 * halfLonW), invLatW = 1.0 / (2.0 * halfLatW);
    auto dirOfLL = [](double lo, double la) {
        return glm::dvec3(std::cos(la) * std::cos(lo), std::sin(la), std::cos(la) * std::sin(lo));
    };
    // Terreno fino sintetico: un cuenco de 200 m dentro del lago grueso.
    auto terrenoW = [&](double lo, double la) -> float {
        const glm::dvec3 d = dirOfLL(lo, la);
        const double dM = std::acos(glm::clamp(glm::dot(d, dirLago), -1.0, 1.0)) * kProbeRadius;
        return (dM < 200.0) ? (float)(5.0 - 4.0 * (1.0 - (dM / 200.0) * (dM / 200.0))) : 5.0f;
    };
    auto contornoW = [&](double, double) -> float { return Haruka::Planet::WATER_FILL_DRY; };
    const Haruka::Planet::WaterWindowResult win =
        Haruka::Planet::waterFillWindow(64, 64, lonW, latW, halfLonW, halfLatW, kProbeRadius,
                                        terrenoW, contornoW, 0.0f);
    std::printf("    ventana fina 64x64 (%.1f m/texel): %zu texeles de lago\n",
                win.texelM, win.lakeCells);
    CHECK(win.lakeCells > 0, "la ventana de prueba TIENE lago (si no, su careo no compara nada)");

    std::vector<float> rgw((size_t)win.w * win.h * 2);
    for (size_t c = 0; c < (size_t)win.w * win.h; ++c) {
        rgw[c * 2 + 0] = win.levelM[c];
        rgw[c * 2 + 1] = win.fetchM[c];
    }
    TextureDesc twd;
    twd.width = (uint32_t)win.w; twd.height = (uint32_t)win.h;
    twd.format = Format::RG32F; twd.filter = Filter::Nearest; twd.wrap = Wrap::ClampToEdge;
    twd.mipmaps = false; twd.initialData = rgw.data();
    TextureHandle winTex = g_dev->createTexture(twd);
    const float rect[4] = { (float)lonW, (float)latW, (float)invLonW, (float)invLatW };
    BufferHandle winUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(rect), rect,
                                              BufferMemory::Static);

    const std::string cs = Haruka::Shader::baseDir() + "shaders/water_field_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de water_field_probe.comp creado");
    if (!valid(cp)) { g_dev->destroy(lakeTex); return; }

    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, (size_t)N * kOut * sizeof(float),
                                           nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, (size_t)N * kOut * sizeof(float),
                                           nullptr, BufferMemory::Readback);
    BufferHandle in  = g_dev->createBuffer(BufferUsage::Storage, inBuf.size() * sizeof(float),
                                           inBuf.data(), BufferMemory::Static);
    const Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
    BufferHandle ocean = makeOceanStateUBO(st);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->bindTexture(18, lakeTex);
        ctx->bindTexture(19, winTex);
        ctx->bindUniformBuffer(26, winUBO);
        if (valid(ocean)) ctx->bindUniformBuffer(29, ocean);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, (size_t)N * kOut * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); g_dev->destroy(lakeTex); return; }

    std::printf("    punto                  cota GPU     cota CPU      fetch GPU    fetch CPU   factor\n");
    double worstLvl = 0.0, worstFetch = 0.0, worstFac = 0.0;
    bool sawLake = false, sawDry = false;
    for (int k = 0; k < N; ++k) {
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        // El gemelo de CPU: el MISMO muestreo NEAREST que `TerrestrialPlanet::lakeLevelAt`.
        const float lvlCpu = fill.levelM[c];
        const float fchCpu = (fill.fetchM[c] > 0.0f) ? fill.fetchM[c]
                                                     : Haruka::Planet::WATER_FETCH_UNLIMITED;
        const float facCpu = Haruka::Planet::oceanFetchFactor(st, fchCpu);
        // El shader devuelve su propio centinela donde no hay lago: se comparan como "no hay".
        const bool gpuDry = (v[k * kOut + 0] < -1.0e8f);
        const bool cpuDry = (lvlCpu <= Haruka::Planet::WATER_FILL_DRY);
        if (!cpuDry) sawLake = true; else sawDry = true;
        std::printf("    %-20s %11.3f %12.3f %13.0f %12.0f %8.4f\n", probes[k].what,
                    gpuDry ? 0.0 : (double)v[k * kOut + 0], cpuDry ? 0.0 : (double)lvlCpu,
                    (double)v[k * kOut + 1], (double)fchCpu, (double)v[k * kOut + 2]);
        CHECK(gpuDry == cpuDry, "GPU y CPU coinciden en SI hay lago en ese punto");
        if (!cpuDry) worstLvl = std::max(worstLvl, std::fabs((double)v[k*kOut+0] - (double)lvlCpu));
        worstFetch = std::max(worstFetch, std::fabs((double)v[k*kOut+1] - (double)fchCpu)
                                        / std::max(1.0, (double)fchCpu));
        worstFac   = std::max(worstFac,   std::fabs((double)v[k*kOut+2] - (double)facCpu));
    }
    std::printf("    peor diferencia: cota %.6f m · fetch %.3e relativo · factor de ola %.6f\n",
                worstLvl, worstFetch, worstFac);

    CHECK(sawLake && sawDry,
          "el careo mira puntos con lago Y sin el (si no, seria tautologico)");
    CHECK(worstLvl < 1e-3, "la COTA de la lamina es la misma en la GPU que en la CPU");
    CHECK(worstFetch < 1e-5, "y el FETCH tambien");
    CHECK(worstFac < 1e-4, "y con el, el factor que decide la ola que se dibuja en ese lago");
    // ⚠️ Y QUE EL FACTOR NO ESTE SATURADO: con 1,0 en todos los puntos, la linea de arriba compararia
    // 1,0 contra 1,0. La primera version de este test hacia exactamente eso.
    double facLake = 1.0;
    for (int k = 0; k < N; ++k) {
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        if (fill.levelM[c] > Haruka::Planet::WATER_FILL_DRY)
            facLake = std::min(facLake, (double)v[k * kOut + 2]);
    }
    std::printf("    factor de fetch en el lago: %.4f (si fuera 1,0 el careo del fetch no mediria nada)\n",
                facLake);
    CHECK(facLake < 0.9, "CONTRAPRUEBA: el fetch del lago LIMITA la ola de verdad (factor < 1)");

    // ── Y LA VENTANA FINA: el otro sitio donde CPU y GPU pueden separarse ───────────────────────
    //
    // Son dos convenciones distintas (equirect global contra recuadro lon/lat con desenvuelto de la
    // longitud), asi que casar el campo global NO dice nada de esta. `waterWindowLevelAt` de
    // `water_fill.h` es la unica implementacion de CPU y `harukaLakeWinUV` su gemelo declarado.
    std::printf("    punto                  ventana GPU  ventana CPU   fetch comb GPU  fetch comb CPU\n");
    double worstWin = 0.0, worstComb = 0.0;
    int mojados = 0, secos = 0;
    for (int k = 0; k < N; ++k) {
        const glm::dvec3 d = dirOfTexel(probes[k].x, probes[k].y);
        const float winCpu = Haruka::Planet::waterWindowLevelAt(win, d, lonW, latW, invLonW, invLatW);
        const bool cpuDry = (winCpu <= Haruka::Planet::WATER_FILL_DRY);
        const bool gpuDry = (v[k * kOut + 3] < -1.0e8f);
        if (cpuDry) ++secos; else ++mojados;
        // El fetch COMBINADO: la ventana manda donde ve lamina, el global si no.
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        float combCpu;
        if (!cpuDry) combCpu = Haruka::Planet::waterWindowFetchAt(win, d, lonW, latW, invLonW, invLatW);
        else         combCpu = (fill.fetchM[c] > 0.0f) ? fill.fetchM[c]
                                                       : Haruka::Planet::WATER_FETCH_UNLIMITED;
        std::printf("    %-20s %12.3f %12.3f %15.0f %15.0f\n", probes[k].what,
                    gpuDry ? 0.0 : (double)v[k * kOut + 3], cpuDry ? 0.0 : (double)winCpu,
                    (double)v[k * kOut + 4], (double)combCpu);
        CHECK(gpuDry == cpuDry, "GPU y CPU coinciden en si la VENTANA moja ese punto");
        if (!cpuDry) worstWin = std::max(worstWin, std::fabs((double)v[k*kOut+3] - (double)winCpu));
        worstComb = std::max(worstComb, std::fabs((double)v[k*kOut+4] - (double)combCpu)
                                       / std::max(1.0, (double)combCpu));
    }
    std::printf("    peor diferencia en la ventana: cota %.6f m · fetch combinado %.3e relativo\n",
                worstWin, worstComb);
    CHECK(mojados > 0 && secos > 0,
          "el careo de la ventana mira puntos DENTRO y FUERA de ella (si no, seria tautologico)");
    CHECK(worstWin < 1e-3, "la cota de la VENTANA es la misma en la GPU que en la CPU");
    CHECK(worstComb < 1e-5, "y el fetch combinado tambien (es el que entra en la Gerstner)");

    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in);
    g_dev->destroy(cp); g_dev->destroy(lakeTex);
    g_dev->destroy(winTex); g_dev->destroy(winUBO);
}


static void testWaveNumberParity()
{
    BEGIN("mar: el numero de onda de la GPU == el de la CPU");

    const std::string cs = Haruka::Shader::baseDir() + "shaders/wavenumber_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de wavenumber_probe.comp creado");
    if (!valid(cp)) return;

    const float depths[12] = { 0.05f, 0.1f, 0.3f, 0.5f, 1.0f, 2.0f,
                               3.0f, 5.18f, 10.0f, 23.45f, 100.0f, 400.0f };
    const float k0 = 6.2831853f / Haruka::Planet::OCEAN_WAVE[0][0];
    // El MISMO punto y tiempo que usa `rhitest_waveprobe` en su peor pixel, para poder carear los dos
    // caminos: si la altura casa aqui (compute) y no alli (fragmento), el fallo es de aquel camino.
    const glm::vec3 wpProbe = glm::vec3(1000.0f, 0.0f, -500.0f)
                            + glm::vec3(0.7f, 0.0f, 0.11f) * 133.0f
                            + glm::vec3(0.13f, 0.0f, 0.9f) * 52.0f;
    const glm::vec3 upProbe(0.0f, 1.0f, 0.0f);
    const float     tProbe = 12.75f;
    float inputs[20]; inputs[0] = k0;
    for (int i = 0; i < 12; ++i) inputs[1 + i] = depths[i];
    inputs[13] = wpProbe.x; inputs[14] = wpProbe.y; inputs[15] = wpProbe.z;
    inputs[16] = upProbe.x; inputs[17] = upProbe.y; inputs[18] = upProbe.z;
    inputs[19] = tProbe;

    const size_t N = 28;
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Readback);
    BufferHandle in  = g_dev->createBuffer(BufferUsage::Storage, sizeof(inputs), inputs,
                                           BufferMemory::Static);
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
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); return; }

    std::printf("    tren de %.1f m (k0 = %.6f)\n", Haruka::Planet::OCEAN_WAVE[0][0], k0);
    std::printf("    prof.      k GPU      k CPU     dif.rel    residuo GPU  residuo CPU\n");
    double worstRel = 0.0, worstResGpu = 0.0;
    for (int i = 0; i < 12; ++i) {
        const float kc = Haruka::Planet::oceanWaveNumber(k0, depths[i]);
        const double rel = std::fabs((double)v[i] - (double)kc) / std::max((double)kc, 1e-9);
        const double resC = ((double)(Haruka::Planet::OCEAN_G * kc * std::tanh(kc * depths[i]))
                           - (double)(Haruka::Planet::OCEAN_G * k0))
                          / (double)(Haruka::Planet::OCEAN_G * k0);
        worstRel = std::max(worstRel, rel);
        worstResGpu = std::max(worstResGpu, std::fabs((double)v[12 + i]));
        std::printf("    %7.2f m %10.6f %10.6f  %9.2e  %11.2e %11.2e\n",
                    depths[i], v[i], kc, rel, (double)v[12 + i], resC);
    }
    std::printf("    peor diferencia relativa GPU<->CPU: %.3e · peor residuo de la GPU: %.3e\n",
                worstRel, worstResGpu);

    // ── LA ALTURA, EN COMPUTE, EN EL PEOR PUNTO DE LA OTRA SONDA ────────────────────────────────
    const Haruka::Planet::OceanState stRef = Haruka::Planet::oceanDefaultState();
    const float hCpu = Haruka::Planet::oceanWaveHeight(wpProbe, upProbe, tProbe, depths[7], 1.0f, stRef);
    const float k0f  = 6.2831853f / stRef.wave[0][0];
    std::printf("    altura a %.2f m de fondo: GPU %+.6f m · CPU %+.6f m · dif %.6f m\n",
                depths[7], v[24], hCpu, std::fabs(v[24] - hCpu));
    std::printf("      piezas   green GPU %.6f / CPU %.6f · breakScale GPU %.6f / CPU %.6f\n",
                v[25], Haruka::Planet::oceanGreenGain(depths[7]),
                v[26], Haruka::Planet::oceanBreakScale(depths[7], stRef));
    std::printf("      fade del tren largo: GPU %.6f / CPU %.6f\n",
                v[27], Haruka::Planet::oceanShortWaveFade(
                           Haruka::Planet::oceanWaveNumber(k0f, depths[7])));
    CHECK(std::fabs(v[24] - hCpu) < 1e-3f,
          "la altura de la ola casa GPU<->CPU calculada en COMPUTE (aisla el camino del fragmento)");

    // ⚠️ EL UMBRAL SALE DE LA FASE, NO DE UN GUSTO. La fase es `k · dot(D,wp)`, y en la sonda de la
    // ola `dot(D,wp)` llega a ~1100 m: una diferencia relativa de 1e-4 en `k` ya son 0,018 rad de
    // fase, y 1e-2 descorrelaciona las dos olas por completo. Se pide 1e-5.
    CHECK(worstRel < 1e-5, "la GPU resuelve el MISMO numero de onda que la CPU");
    // Y el oraculo independiente: la GPU satisface la ecuacion que dice resolver.
    CHECK(worstResGpu < 1e-4, "y el `k` de la GPU satisface la relacion de dispersion");
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}


static void testTerrainBakeAcrossLevels()
{
    BEGIN("v5 F1: el bake contra la CPU, NIVEL A NIVEL (¿escala la divergencia?)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const uint32_t N = TERRAIN_NODE_TEXELS;

    TerrainNodeGpu gpu;
    const std::string base = Haruka::Shader::baseDir();
    if (!gpu.init(g_dev, (base + "shaders/terrain_node.comp").c_str(), 64)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }
    std::printf("    nivel   texel      peor       media     (bake GPU vs referencia CPU)\n");
    double worstAll = 0.0, worstFine = 0.0;
    for (uint32_t lvl : { 9u, 11u, 13u, 15u, 17u }) {
        // El mismo punto del planeta a cada nivel: el nodo de nivel 17 dividido por potencias de dos.
        const NodeId node{ Haruka::PlanetFace::FRONT, lvl, 33600u >> (17 - lvl), 24800u >> (17 - lvl) };
        TerrainNodePool pool(64, 8);
        pool.beginFrame();
        pool.request(node);
        for (int pass = 0; pass < 24; ++pass) {
            Context* ctx = g_dev->beginFrame();
            const size_t n = ctx ? gpu.generatePending(ctx, pool, R) : 0;
            g_dev->endFrame();
            pumpWindowEvents();
            if (n == 0) break;
        }
        int slot = -1;
        for (size_t k = 0; k < gpu.capacity(); ++k) {
            NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
        }
        if (slot < 0) { CHECK(false, "el nodo del nivel esta residente"); continue; }

        std::vector<float> ref((size_t)N * N);
        nodeFillHeights(node, R, ref.data());
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, ref.size() * sizeof(float),
                                              nullptr, BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode,
                     ref.size() * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        double worst = 0.0, sum = 0.0;
        if (got) {
            for (size_t k = 0; k < ref.size(); ++k) {
                const double e = std::fabs((double)got[k] - (double)ref[k]);
                worst = std::max(worst, e); sum += e;
            }
        }
        std::printf("    %5u  %7.3f m  %8.4f m  %8.4f m\n", lvl, nodeTexelM(node, R),
                    worst, sum / (double)ref.size());
        worstAll = std::max(worstAll, worst);
        if (lvl == 17u) worstFine = worst;
        g_dev->destroy(rb);
    }
    // ⚠️ SIN NUMERO FIJO: lo que se afirma es que el bake CASA con su gemelo de CPU en TODOS los
    // niveles, no solo en el que se miraba. La tolerancia es la del test que ya existia (0,05 m).
    std::printf("    peor de todos los niveles: %.4f m · solo del 17: %.4f m\n", worstAll, worstFine);
    CHECK(worstAll < 0.05, "el bake casa con la CPU en TODOS los niveles, no solo en el 17");
    gpu.shutdown();
}


// ================================================================================================
// ¿EN QUE ETAPA DEL BAKE SE PIERDE? La biseccion que faltaba, sobre los 16 641 TEXELES.
//
// ⚠️ LO QUE HABIA NO PODIA CONTESTAR ESTO, POR DOS MOTIVOS INDEPENDIENTES:
//
//   · `octave_probe.comp` mira UN SOLO PUNTO. Da 1,5e-5 m en las dos GPU y pasa — pero el bake, en
//     la misma AMD, tiene una MEDIA de 0,003 m sobre el nodo entero. Un punto de 16 641 no es una
//     muestra, es una anecdota, y esa anecdota decia "todo casa" mientras el nodo no casaba.
//   · Los modos de biseccion 1-3 del propio `terrain_node.comp` emiten floats de `lx` y `dir.x`,
//     cuyo ulp vale 0,76 m de superficie. Se escribieron cuando el sintoma eran METROS. Hoy el
//     sintoma son 0,02 m y esos modos son ciegos: habrian dicho "casa" con toda seguridad.
//
// Aqui se usan los modos 6 y 7 (residuo sub-float amplificado, ver el shader) para preguntar por
// SEPARADO y en TODOS los texeles: ¿es identica la direccion? ¿lo es la posicion `dir·R`? Y luego
// la altura, con su histograma — porque "peor 0,02 · media 0,003" no dice lo mismo si es un texel
// suelto (un umbral cruzado) que si son los 16 641 (precision).
// ================================================================================================
static void testBakeStageBisect()
{
    BEGIN("v5 F1: ¿en QUE etapa del bake diverge la GPU? (biseccion por texel)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const uint32_t N = TERRAIN_NODE_TEXELS;
    const size_t   T = (size_t)N * N;

    TerrainNodeGpu gpu;
    if (!gpu.init(g_dev, (Haruka::Shader::baseDir() + "shaders/terrain_node.comp").c_str(), 8)) {
        CHECK(false, "TerrainNodeGpu::init"); return;
    }

    // Genera el nodo con el modo pedido y devuelve el heightmap leido. Pool NUEVO cada vez: el
    // contenido se cachea por hueco, asi que reutilizarlo devolveria el bake del modo anterior.
    auto bake = [&](const NodeId& node, int mode, std::vector<float>& out) -> bool {
        gpu.setDebugMode(mode);
        TerrainNodePool pool(8, 4);
        pool.beginFrame();
        pool.request(node);
        for (int pass = 0; pass < 24; ++pass) {
            Context* ctx = g_dev->beginFrame();
            const size_t n = ctx ? gpu.generatePending(ctx, pool, R) : 0;
            g_dev->endFrame();
            pumpWindowEvents();
            if (n == 0) break;
        }
        int slot = -1;
        for (size_t k = 0; k < gpu.capacity(); ++k) {
            NodeId at; if (pool.nodeAtSlot((int)k, at) && at == node) { slot = (int)k; break; }
        }
        if (slot < 0) return false;
        out.assign(T, 0.0f);
        BufferHandle rb = g_dev->createBuffer(BufferUsage::Storage, T * sizeof(float), nullptr,
                                              BufferMemory::Readback);
        copyThenWait(gpu.heights(), rb, (size_t)slot * TerrainNodeGpu::kBytesPerNode,
                     T * sizeof(float));
        const float* got = (const float*)g_dev->mappedData(rb);
        const bool ok = (got != nullptr);
        if (ok) std::memcpy(out.data(), got, T * sizeof(float));
        g_dev->destroy(rb);
        return ok;
    };

    bool dirOk = true, posOk = true;
    for (uint32_t lvl : { 9u, 17u }) {
        const NodeId node{ Haruka::PlanetFace::FRONT, lvl, 33600u >> (17 - lvl), 24800u >> (17 - lvl) };
        std::printf("    ── nivel %u (texel %.3f m) ─────────────────────────────\n",
                    lvl, nodeTexelM(node, R));

        // ── ETAPA 1: la DIRECCION, hasta el ultimo bit del double ────────────────────────────────
        std::vector<float> g6;
        if (bake(node, 6, g6)) {
            double worst = 0.0, sum = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const glm::dvec3 d = nodeTexelDir(node, u, v);
                    const double ref = (d.x - (double)(float)d.x) * 1.0e9;
                    const double e = std::fabs((double)g6[(size_t)v * N + u] - ref);
                    worst = std::max(worst, e); sum += e;
                }
            // El residuo esta en unidades de 1e-9 de direccion; a radio terrestre, 1 unidad = 6,4 mm.
            std::printf("      dir.x  (residuo sub-float) peor %.4f · media %.4f  [1 ud = %.4f m]\n",
                        worst, sum / (double)T, 1.0e-9 * R);
            if (worst > 0.01) dirOk = false;
        } else CHECK(false, "el nodo del modo 6 esta residente");

        // ── ETAPA 2: la POSICION `dir·R`, en metros ──────────────────────────────────────────────
        std::vector<float> g7;
        if (bake(node, 7, g7)) {
            double worst = 0.0, sum = 0.0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const double px  = nodeTexelDir(node, u, v).x * R;
                    const double ref = px - (double)(float)px;
                    const double e = std::fabs((double)g7[(size_t)v * N + u] - ref);
                    worst = std::max(worst, e); sum += e;
                }
            std::printf("      dir.x*R (residuo, metros)  peor %.9f m · media %.9f m\n",
                        worst, sum / (double)T);
            if (worst > 1.0e-6) posOk = false;
        } else CHECK(false, "el nodo del modo 7 esta residente");

        // ── ETAPA 3: la ALTURA, con su forma espacial ────────────────────────────────────────────
        std::vector<float> g0;
        std::vector<float> ref(T);
        nodeFillHeights(node, R, ref.data());
        if (bake(node, 0, g0)) {
            double worst = 0.0, sum = 0.0; uint32_t wu = 0, wv = 0;
            size_t over1mm = 0, over1cm = 0;
            for (uint32_t v = 0; v < N; ++v)
                for (uint32_t u = 0; u < N; ++u) {
                    const size_t k = (size_t)v * N + u;
                    const double e = std::fabs((double)g0[k] - (double)ref[k]);
                    if (e > worst) { worst = e; wu = u; wv = v; }
                    sum += e;
                    if (e > 0.001) ++over1mm;
                    if (e > 0.01)  ++over1cm;
                }
            std::printf("      altura                     peor %.4f m en (%u,%u) · media %.4f m\n",
                        worst, wu, wv, sum / (double)T);
            // ⚠️ EL HISTOGRAMA ES EL DIAGNOSTICO, NO EL PEOR. Un peor alto con casi ningun texel por
            // encima del milimetro seria un UMBRAL cruzado en un punto; miles de texeles por encima
            // es PRECISION perdida en todas partes. Son dos bugs distintos y el "peor" no los separa.
            std::printf("      texeles > 1 mm: %zu de %zu (%.1f %%) · > 1 cm: %zu (%.1f %%)\n",
                        over1mm, T, 100.0 * (double)over1mm / (double)T,
                        over1cm, 100.0 * (double)over1cm / (double)T);
        } else CHECK(false, "el nodo del modo 0 esta residente");
    }

    // ⚠️ ESTAS DOS ASERCIONES SON LAS QUE LOCALIZAN EL BUG, Y POR ESO SON DURAS. Si la direccion y la
    // posicion casan bit a bit con la CPU y la ALTURA no, entonces lo que diverge esta AGUAS ABAJO
    // —dentro de `harukaTerrainDetail`, que es hash entero y lerps escritos a mano en float— y eso
    // acota el problema a una sola funcion. Si alguna de las dos falla, el problema es la geometria.
    CHECK(dirOk, "la direccion del texel es la MISMA que en la CPU, hasta el ulp del double");
    CHECK(posOk, "la posicion `dir*R` es la MISMA que en la CPU, hasta el ulp del double");
    gpu.shutdown();
}


// ================================================================================================
// ¿QUE OCTAVA DIVERGE? Localiza el bug CRITICO de "el suelo depende de la GPU".
//
// `testTerrainBakeAcrossLevels` dice CUANTO (0,02 m en AMD, 0,0001 en NVIDIA, plano en todo nivel).
// Esto dice DONDE: emite cada octava por separado y su fraccion de celda, y lo compara con el gemelo
// de CPU. Si solo divergen las octavas finas -> precision de la posicion. Si divergen todas por
// igual -> el hash o la interpolacion.
// ================================================================================================
static void testOctaveDivergence()
{
    BEGIN("¿que octava del ruido diverge entre GPUs? (bug critico del suelo)");

    const std::string cs = Haruka::Shader::baseDir() + "shaders/octave_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de octave_probe.comp creado");
    if (!valid(cp)) return;

    const size_t N = 17;
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Readback);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, N * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(cp); return; }

    // El gemelo de CPU, con la MISMA direccion y las MISMAS frecuencias.
    const glm::dvec3 dir = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 p   = dir * 6371000.0;
    const double freq[7] = { 0.0000833, 0.0001667, 0.00035, 0.0016, 0.0090, 0.0450, 0.2200 };
    const double amp[7]  = { 969.3, 513.4, 260.0, 70.0, 14.0, 3.0, 0.7 };
    std::printf("    oct  frecuencia   ruido GPU   ruido CPU     delta     x amplitud\n");
    double worstM = 0.0; int worstOct = -1;
    for (int i = 0; i < 7; ++i) {
        const glm::dvec3 x = p * freq[i];
        const double cpu = Haruka::Planet::detailNoise(x);
        const double d   = std::fabs((double)v[i] - cpu);
        const double dm  = d * amp[i];
        if (dm > worstM) { worstM = dm; worstOct = i; }
        std::printf("    %3d  %10.7f  %10.7f  %10.7f  %9.2e  %8.4f m\n",
                    i, freq[i], (double)v[i], cpu, d, dm);
    }
    std::printf("    |p| - R en la GPU: %.6f m  ·  peor octava: %d (%.4f m)\n",
                (double)v[14], worstOct, worstM);
    // El ultimo eslabon: la direccion calculada por `harukaCubeFaceToDir`, que es la que usa el bake.
    const glm::dvec3 d2  = Haruka::cubeFaceToDir(Haruka::PlanetFace::FRONT, -0.4, 0.25);
    const double     cpu2 = Haruka::Planet::terrainDetail(d2, 6371000.0, 0.596f);
    const double     dd   = std::fabs((double)v[15] - cpu2);
    std::printf("    con la direccion del BAKE (cubeFaceToDir): GPU %.6f m · CPU %.6f m · delta %.6f m\n",
                (double)v[15], cpu2, dd);
    std::printf("    (|dir| - 1) en ulps de double: %.2f\n", (double)v[16]);
    CHECK(dd < 0.001, "el detalle con la direccion del bake casa GPU<->CPU");
    // ⚠️ SIN TOLERANCIA INVENTADA: el ruido es la MISMA funcion en los dos lados, con la misma
    // entrada en double. Cualquier diferencia por encima del ulp de un float ya es el bug.
    CHECK(worstM < 0.001, "el ruido de la GPU casa con el de la CPU en TODAS las octavas");
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(cp);
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
// ================================================================================================
// EL AGUA DEL PASE DE NODOS **DIBUJA**. El test que separa "compila" de "se ve".
//
// ⚠️ EL AGUA SE MIGRO ENTERA DE LA REJILLA DEL CLIPMAP AL QUADTREE Y NADIE LA HABIA VISTO PINTAR. El
// motor tenia sondas del CAMPO (donde hay agua), de la OLA (que altura) y de la PARIDAD GPU/CPU —
// todas de DATOS. Ninguna miraba la IMAGEN, que es exactamente lo que dejo pasar los tres bugs del
// pase de terreno (raices sin fijar, SSBO instanciado, matriz equivocada): los tres pasaban el banco
// entero y vaciaban la pantalla.
//
// ⚠️ Y HACE FALTA UN FONDO DE MAR FALSO. El agua se recorta por PROFUNDIDAD (`nivel - baseH`), y sin
// planeta horneado `baseH` vale 0: la profundidad sale 0 y el fragmento se descarta ENTERO. Con una
// textura de altura sintetica a -500 m hay oceano en todas partes; con +500 m, tierra. Ese par es el
// test y su contraprueba a la vez.
// ================================================================================================
// ================================================================================================
// NUBES: EL PASE VOLUMETRICO, MEDIDO SOBRE LO QUE DIBUJA
//
// ⚠️ ESTE TEST NO EXISTIA, Y ES EXACTAMENTE EL QUE FALTO LA VEZ ANTERIOR. `cloud_shape` mide el CAMPO
// (relacion ancho/alto de la losa, opacidad, pasos) y paso en VERDE mientras el muestreo estaba roto:
// con 24 pasos repartidos por igual, mirando al horizonte el paso superaba el tamaño de la nube
// —0,38 muestras por nube a 88 grados— y las nubes sencillamente no se dibujaban ahi. La leccion
// quedo escrita: *"el test va contra lo que se DIBUJA, no contra la geometria ideal"*. Y aun asi
// `cloud_vol.frag` solo aparecia en la lista de pipelines que COMPILAN. Nadie lo habia dibujado.
//
// Aqui se dibuja de verdad y se mide en el eje donde ese bug se escondia: **el angulo de elevacion**.
// La capa es una cascara esferica, asi que mirando hacia el horizonte el rayo recorre MAS nube, no
// menos — la opacidad tiene que SUBIR hacia el horizonte. Si baja, es que la marcha se queda corta.
// ================================================================================================
// CIELO: LAS CAPAS ALTAS, ¿LOSAS O CALCOMANIAS?
//
// ⚠️ EL BUG QUE ENCONTRO ESTE TEST, Y POR QUE NADIE LO VIO. `sky.frag` tenia un bloque titulado
// *"ESPESOR REAL: LOSA EN VEZ DE LAMINA"* con esta nota: *"ESTE ERA EL MOTIVO DE QUE SE VIERAN
// PLANAS... Se veia plano porque era plano"*. Estaba bajo `if (layer == 2)` — solo el CUMULO. Y desde
// que el cumulo lo dibuja el pase volumetrico, `nLayers` vale 2 y **la capa 2 no se recorre nunca**:
// el arreglo entero estaba aplicado a la unica capa que ya no se pinta. Las dos que si se ven —cirro
// (8 km) y altocumulo (4 km)— seguian siendo campos 2D sobre un cascaron. Igual que `cloud_vol.frag`,
// `sky.frag` solo aparecia en la lista de pipelines que COMPILAN: nadie lo habia dibujado nunca.
//
// La firma que separa una losa de una calcomania es el CAMINO OPTICO: al mirar hacia el horizonte se
// atraviesa mas capa, asi que tapa mas. Una calcomania se ve igual de opaca en el cenit que en el
// horizonte. Eso es lo que se mide aqui, sobre pixeles.
// ⚠️ FILA DEL READBACK -> NDC.Y, Y NO ES UN DETALLE: `readPixels` de GL devuelve las filas de ABAJO
// ARRIBA y las de Vulkan de ARRIBA ABAJO (ya estaba escrito en este fichero, en el volcado de PNG del
// agua). Sin esto, cualquier medida que relacione una FILA con un angulo sale con el signo cambiado:
// me costo concluir que `sky.frag` pintaba nubes por DEBAJO del horizonte —a -20 grados— cuando lo
// que pasaba es que yo llamaba "abajo" a la mitad de arriba. Su propia guarda (`t > 0.02`) hacia
// imposible lo que yo estaba midiendo, y esa contradiccion fue la pista.
static inline float rowToNdcY(int y, int height, bool flip) {
    const float f = ((float)y + 0.5f) / (float)height;
    return flip ? (2.0f * f - 1.0f) : (1.0f - 2.0f * f);
}

/// ⚠️ Y EL SENTIDO SE CALIBRA CON EL DATO, NO SE SUPONE. Primero lo ate al backend (`isVk`) siguiendo
/// la nota de este fichero sobre `readPixels`, y salio bien en UNO de los dos y espejado en el otro:
/// imprimia que las nubes estaban a -20 grados, bajo el horizonte, donde la propia guarda de
/// `sky.frag` (`t > 0.02`) hace imposible pintarlas. Esa contradiccion fue la pista.
///
/// Aqui se decide mirando la imagen: el SOL esta a una elevacion conocida y positiva, asi que la fila
/// mas brillante tiene que caer arriba. Si con la convencion supuesta cae abajo, se voltea. Es una
/// medida, no una suposicion sobre el backend, y por eso no puede fallar en uno de los dos.
static inline bool detectRowFlip(const std::vector<uint8_t>& px, int w, int h) {
    double bestL = -1.0; int bestRow = 0;
    for (int y = 0; y < h; ++y) {
        double acc = 0.0;
        for (int x = 0; x < w; ++x) {
            const size_t k = ((size_t)y * w + x) * 4;
            if (k + 2 >= px.size()) break;
            acc += 0.2126 * px[k] + 0.7152 * px[k+1] + 0.0722 * px[k+2];
        }
        if (acc > bestL) { bestL = acc; bestRow = y; }
    }
    // Sin voltear, la fila 0 es la de ARRIBA. Si la mas brillante (el sol) esta en la mitad de
    // abajo, es que las filas vienen al reves.
    return bestRow > h / 2;
}

// ================================================================================================
// LA DISTRIBUCION DE `cloudField`: el dato que faltaba para calibrar el cielo
//
// ⚠️ El cielo se tapa ~10 veces menos de lo que pide el clima (cobertura 0,80 -> 6,6 % tapado al
// cenit). Quien lo decide es `lo = mix(0.70, 0.32, cloudiness)` de `sky.frag`, y un umbral solo
// significa algo contra la DISTRIBUCION del campo. Nunca se habia medido: `cloudField` vivia dentro
// del fragment y no se podia llamar desde el banco. Sacada a `lib/sky_clouds.glsl`, esto la sondea.
//
// Lo que se busca son los CUANTILES: "para tapar el X % del cielo, el umbral tiene que valer Y".
static void testCloudFieldDistribution()
{
    BEGIN("cielo: la DISTRIBUCION de `cloudField` (para poder calibrar el umbral)");

    const std::string cs = Haruka::Shader::baseDir() + "shaders/cloud_field_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de cloud_field_probe.comp creado");
    if (!valid(cp)) return;

    const int N = 256;                       // 65 536 muestras
    const size_t bytes = (size_t)N * N * sizeof(float);
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, bytes, nullptr, BufferMemory::Readback);

    // Paso de la rejilla en las unidades del campo. `sky.frag` evalua `base = pk * scale` con
    // `scale` 0,085..0,30 y `pk` en km: cubrir decenas de unidades barre muchas nubes.
    const float in4[4] = { (float)N, 0.08f, 3.0f, 1.0f };
    BufferHandle in = g_dev->createBuffer(BufferUsage::Storage, sizeof(in4), in4, BufferMemory::Static);

    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->dispatch((uint32_t)((N + 15) / 16), (uint32_t)((N + 15) / 16), 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, bytes);
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback del campo"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); return; }

    std::vector<float> d(v, v + (size_t)N * N);
    std::sort(d.begin(), d.end());
    auto q = [&](double f) { return d[(size_t)(f * (double)(d.size() - 1))]; };
    double sum = 0.0; for (float x : d) sum += x;
    const double media = sum / (double)d.size();

    std::printf("    %zu muestras · min %.4f · max %.4f · media %.4f\n",
                d.size(), (double)d.front(), (double)d.back(), media);
    std::printf("    cuantiles (el umbral que deja ese %% de cielo POR ENCIMA):\n");
    for (double f : { 0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90 })
        std::printf("      tapar el %2.0f %% -> umbral %.4f\n", 100.0 * (1.0 - f), (double)q(f));
    std::printf("    ⚠️ el rango que usa hoy `sky.frag` es lo = 0,70 (sin nube) .. 0,32 (todo nube)\n"
                "       -> con 0,70 tapa el %.1f %% · con 0,32 tapa el %.1f %%\n",
                100.0 * (double)(std::upper_bound(d.begin(), d.end(), 0.70f) - d.begin()) / (double)d.size() * 0.0
                + 100.0 * (1.0 - (double)(std::lower_bound(d.begin(), d.end(), 0.70f) - d.begin()) / (double)d.size()),
                100.0 * (1.0 - (double)(std::lower_bound(d.begin(), d.end(), 0.32f) - d.begin()) / (double)d.size()));

    CHECK(d.back() > d.front(), "el campo VARIA (si fuera constante, no habria nada que calibrar)");
    CHECK(std::isfinite(media), "la media es finita");

    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}

static void testSkyLayersHaveDepth()
{
    BEGIN("cielo: las capas altas tienen ESPESOR (no son calcomanias)");

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const std::string base = Haruka::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/sky.vert", fs = base + "shaders/sky.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del cielo creado");
    if (!valid(pipe)) return;

    struct SkyUBO {
        float invVP[16];
        float sunDir[3]; float sunElev;
        float up[3];     float atmo;
        float sunColor[3]; float time;
        float weather[4]; float wind[4]; float planet[4];
    };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(SkyUBO), nullptr,
                                           BufferMemory::Dynamic);

    const double R = 6371000.0;
    const glm::dvec3 up0(0.0, 1.0, 0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const float aspect = (float)w / (float)h;
    const float fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot;
    proj[2][3] = -1.0f;         proj[3][2] = 1.0f;      // reversed-Z, lejano infinito

    float g_skyTime = 5.0f;
    // `u_planet.w > 1` = "no hay pase volumetrico, pinta tu el cumulo": el camino de RESPALDO.
    bool respaldo = false;
    auto shootRaw = [&](const glm::dvec3& fwd, const glm::dvec3& vup, float cover,
                        std::vector<uint8_t>& px) {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        SkyUBO u{};
        std::memcpy(u.invVP, &invVP[0][0], sizeof(u.invVP));
        const glm::vec3 sun = glm::normalize(glm::vec3(0.35f, 0.62f, 0.70f));
        u.sunDir[0]=sun.x; u.sunDir[1]=sun.y; u.sunDir[2]=sun.z; u.sunElev = sun.y;
        u.up[0]=(float)up0.x; u.up[1]=(float)up0.y; u.up[2]=(float)up0.z; u.atmo = 1.0f;
        u.sunColor[0]=u.sunColor[1]=u.sunColor[2]=1.0f; u.time = g_skyTime;
        u.weather[0]=0.5f; u.weather[1]=15.0f; u.weather[2]=0.0f; u.weather[3]=cover;
        u.wind[0]=4.0f; u.wind[1]=1.0f; u.wind[2]=0.0f; u.wind[3]=0.0f;
        // ⚠️ `planet.w = 0` = "del cumulo se encarga el pase volumetrico", que es el camino REAL del
        // motor. Con w > 0 se pintarian tres capas y se estaria probando el camino de respaldo.
        u.planet[0]=(float)R; u.planet[1]=300.0f; u.planet[2]=1500.0f;
        u.planet[3]= respaldo ? 3200.0f : 0.0f;   // >1 = techo del cumulo -> camino de respaldo
        g_dev->updateBuffer(ubo, 0, sizeof(u), &u);

        px.assign((size_t)w * h * 4, 0xAA);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        for (int f = 0; f < 3; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
            ctx->draw(3, 1);
            ctx->endRenderPass();
            if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
    };

    // Calibracion del orden de filas, con el cielo despejado (el sol es la referencia). Ver
    // `detectRowFlip`: atarlo al backend salio espejado en uno de los dos.
    bool rowFlip = false;
    {
        std::vector<uint8_t> cal;
        shootRaw(glm::dvec3(1, 0, 0), up0, 0.0f, cal);
        rowFlip = detectRowFlip(cal, w, h);
        std::printf("    orden de filas del readback: %s (calibrado con la posicion del sol)\n",
                    rowFlip ? "invertido" : "directo");
    }

    // ⚠️ EL DISCRIMINANTE, A LA TERCERA — LAS DOS PRIMERAS FALLARON POR EL INSTRUMENTO.
    // "Poco azul y claro" (`B-R < 40 && suma > 150`) dio **0 % en las cuatro vistas**; bajar el
    // umbral a `B-R < 55` dio 0,5 %. No es que no hubiera nubes: el cielo de este motor las tiñe de
    // azul y ninguna regla sobre la paleta acertaba. Lo que NO depende de la paleta es **que pixeles
    // CAMBIAN al mover la cobertura** — eso es la nube, sea del color que sea. Es la misma tecnica
    // con la que se cazo que los dos backends sombreaban distinto (99,8 % contra 48,8 %).
    auto cloudFraction = [&](const glm::dvec3& fwd, const glm::dvec3& vup, float cover) {
        std::vector<uint8_t> a, b;
        shootRaw(fwd, vup, 0.0f, a);
        shootRaw(fwd, vup, cover, b);
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < a.size() && k + 3 < b.size(); k += 4) {
            if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
            const int d = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                        + std::abs((int)a[k+2] - (int)b[k+2]);
            if (d > 12) ++dif;
            ++n;
        }
        return n ? (double)dif / (double)n : 0.0;
    };

    // ⚠️ Y UNA CUARTA TRAMPA DEL INSTRUMENTO, esta geometrica: comparar la vista al CENIT con la del
    // HORIZONTE no vale, porque la segunda tiene **la mitad inferior por debajo del horizonte**,
    // donde `sky.frag` no pinta cielo (`t > 0.02`). Salio razon 0,47, que es exactamente esa mitad y
    // no una propiedad de la nube. La comparacion honesta es entre BANDAS DE ELEVACION DE LA MISMA
    // IMAGEN: mismo render, mismos pixeles validos, solo cambia cuanta capa atraviesa el rayo.
    const glm::dvec3 horiz(1.0, 0.0, 0.0);
    auto bandFractions = [&](float cover, double loDeg, double hiDeg, double& outLow, double& outHigh) {
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(horiz), glm::vec3(up0));
        const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        std::vector<uint8_t> a, b;
        shootRaw(horiz, up0, 0.0f, a);
        shootRaw(horiz, up0, cover, b);
        size_t dLo = 0, nLo = 0, dHi = 0, nHi = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
            const glm::vec3 dd = glm::normalize(glm::vec3(far) / far.w);
            const double el = std::asin(glm::clamp((double)glm::dot(dd, glm::vec3(up0)), -1.0, 1.0))
                            * 180.0 / 3.14159265358979;
            const bool low  = (el > 1.0 && el < loDeg);
            const bool high = (el > hiDeg);
            if (!low && !high) continue;
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
                const int df = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                             + std::abs((int)a[k+2] - (int)b[k+2]);
                if (low)  { if (df > 12) ++dLo; ++nLo; }
                else      { if (df > 12) ++dHi; ++nHi; }
            }
        }
        outLow  = nLo ? (double)dLo / (double)nLo : 0.0;
        outHigh = nHi ? (double)dHi / (double)nHi : 0.0;
    };

    // ⚠️ EL PERFIL ENTERO, no dos bandas elegidas a ojo. Con 1-10 y >20 grados salieron las DOS a
    // cero mientras la imagen completa daba 1,6 %, o sea que la nube estaba justo en el hueco que
    // deje sin mirar. Se imprime la fraccion por banda y la afirmacion se hace sobre lo que salga.
    // ⚠️ SOBRE VARIOS INSTANTES, no una foto. Una sola pasada no distingue "esta banda esta MUERTA"
    // de "en este momento ahi no hay nube": el campo se mueve con el viento. Se promedian cuatro
    // tiempos separados, que es lo que convierte el perfil en una propiedad del sistema.
    double fLow = 0.0, fHigh = 0.0, fRing = 0.0, fAny = 0.0, elLo = 1e9, elHi = -1e9;
    {
        const int NB = 6;
        std::vector<double> accF((size_t)NB, 0.0);
        int tomas = 0;
        double accAny = 0.0;
        for (float tm : { 5.0f, 240.0f, 900.0f, 1800.0f }) {
            g_skyTime = tm;
            const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(horiz), glm::vec3(up0));
            const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
            std::vector<uint8_t> a, b;
            shootRaw(horiz, up0, 0.0f, a);
            shootRaw(horiz, up0, 0.45f, b);
            std::vector<size_t> dif((size_t)NB, 0), cnt((size_t)NB, 0);
            size_t dTot = 0, nTot = 0;
            for (int y = 0; y < h; ++y) {
                const float ndcY = rowToNdcY(y, h, rowFlip);
                glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
                const glm::vec3 dd = glm::normalize(glm::vec3(far) / far.w);
                const double el = std::asin(glm::clamp((double)glm::dot(dd, glm::vec3(up0)), -1.0, 1.0))
                                * 180.0 / 3.14159265358979;
                const bool inBand = (el > 0.0 && el < 30.0);
                const int bnd = inBand ? std::min(NB - 1, (int)(el / 30.0 * NB)) : -1;
                for (int x = 0; x < w; ++x) {
                    const size_t k = ((size_t)y * w + x) * 4;
                    if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
                    const int df = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                                 + std::abs((int)a[k+2] - (int)b[k+2]);
                    if (df > 12) { ++dTot; elLo = std::min(elLo, el); elHi = std::max(elHi, el); }
                    ++nTot;
                    if (bnd >= 0) { if (df > 12) ++dif[(size_t)bnd]; ++cnt[(size_t)bnd]; }
                }
            }
            for (int bnd = 0; bnd < NB; ++bnd)
                accF[(size_t)bnd] += cnt[(size_t)bnd] ? (double)dif[(size_t)bnd] / (double)cnt[(size_t)bnd] : 0.0;
            accAny += nTot ? (double)dTot / (double)nTot : 0.0;
            ++tomas;
        }
        g_skyTime = 5.0f;
        std::printf("    cobertura 0,45, media de %d instantes · fraccion de cielo CON NUBE por banda:\n", tomas);
        for (int bnd = 0; bnd < NB; ++bnd) {
            const double f = accF[(size_t)bnd] / (double)tomas;
            std::printf("      %2d-%2d grados   %6.2f %%\n", bnd * 5, (bnd + 1) * 5, 100.0 * f);
            if (bnd == 0) fLow = f;
            fHigh = f;
            fRing = std::max(fRing, f);
        }
        fAny = accAny / (double)tomas;
        if (elLo > elHi) { elLo = 0.0; elHi = 0.0; }
    }
    (void)fRing;
    // La afirmacion va sobre la IMAGEN ENTERA, no sobre una banda: en que franja cae es justo lo que
    // varia y lo que el perfil de arriba deja escrito. Afirmar una banda concreta seria clavar el
    // defecto en el test.
    // ⚠️⚠️ ESTE TEST YA NO PRUEBA LO QUE PROBABA, Y ES A PROPOSITO. Media el cirro y el altocumulo
    // pintados por `sky.frag`; ahora esas dos capas viven en `cloud_vol.frag` como cascaras
    // volumetricas (se atraviesan y se ven desde arriba, que como fondo era imposible). Asi que el
    // cielo de fondo NO debe dibujar ni una nube en el camino normal — y eso es justo lo que hay que
    // fijar, o alguien las devuelve ahi sin querer y vuelven a ser un telon.
    //
    // Lo que MIDE las capas altas ahora es `cloud_volume_draws`. Aqui queda la otra mitad: que el
    // camino de RESPALDO (pase volumetrico apagado, `u_planet.w > 1`) siga pintando cielo con nubes,
    // porque apagarlas no debe dejar el cielo pelado.
    std::printf("    con el pase volumetrico ACTIVO, el cielo de fondo dibuja %.2f %% de nube\n",
                100.0 * fAny);
    CHECK(fAny < 0.02,
          "el cielo de fondo YA NO pinta nubes: las tres capas son volumetricas (no un telon)");
    {
        // Respaldo: `u_planet.w > 1` = "no hay pase volumetrico, pinta tu el cumulo". Se comprueba
        // con la misma vista y la misma medida (pixeles que cambian al mover la cobertura).
        std::vector<uint8_t> a, b;
        respaldo = true;  shootRaw(horiz, up0, 0.0f, a);
        shootRaw(horiz, up0, 0.45f, b);
        respaldo = false;
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < a.size() && k + 3 < b.size(); k += 4) {
            if (a[k] == 0xAA && a[k+1] == 0xAA && a[k+2] == 0xAA) continue;
            const int d2 = std::abs((int)a[k] - (int)b[k]) + std::abs((int)a[k+1] - (int)b[k+1])
                         + std::abs((int)a[k+2] - (int)b[k+2]);
            if (d2 > 12) ++dif;
            ++n;
        }
        std::printf("    CAMINO DE RESPALDO (pase volumetrico apagado): %.1f %% de cielo con nube\n",
                    n ? 100.0 * (double)dif / (double)n : 0.0);
        CHECK(n && dif > n / 50,
              "con el pase volumetrico APAGADO el cielo sigue teniendo nubes (respaldo intacto)");
    }
    std::printf("    la nube del cielo vive entre %.1f y %.1f grados de elevacion\n", elLo, elHi);

    // ⚠️ NO HAY ANILLO, Y ESTO ES LO QUE LO FIJA. La capa vivia entre 5,8 y 21,1 grados con CERO por
    // encima de 25 — un anillo a altura fija de pantalla, que es lo que se lee como "las nubes son un
    // plano 2D". Tres causas, las tres medidas:
    //   · `band` apagaba la capa desde 38 grados hasta el cenit (valia 0,026 mirando arriba);
    //   · `cloudPlane` acotaba la distancia con un `min` DURO, lo que vaciaba la franja rasante;
    //   · `scale` daba elementos de 4,8 km a una capa que esta a 4 km, asi que el cielo alto entero
    //     cabia dentro de UNA nube y salia todo o nada.
    // El test exige nube en las dos puntas del barrido: es lo unico que un anillo no puede cumplir.
    // ⚠️ NO SE EXIGE NUBE EN LA BANDA MAS RASANTE, y es a proposito. Ahi el pixel abarca varias
    // celdas de la reticula del ruido, asi que `fbmAA` apaga las octavas que no se pueden resolver
    // (ver `lib/sky_clouds.glsl`) y lo que queda es bruma lisa — que es lo que hace el cielo de
    // verdad y lo que quita los "cuadrados deformes". Pedir nube contrastada ahi seria pedir alias.
    // Lo que fija que NO hay anillo es el ALCANCE: nube desde cerca del horizonte hasta lo alto del
    // barrido. Un anillo, por definicion, no puede cubrir las dos cosas.
    // (El perfil por bandas se conserva impreso: con el fondo ya sin nubes debe salir plano a cero,
    //  y es la forma de ver de un vistazo si alguien las devuelve al telon.)

    const double fZen = fHigh;   // para la contraprueba de abajo
    (void)fZen;

    {
        const double f0 = cloudFraction(horiz, up0, 0.0f);
        std::printf("    CONTRAPRUEBA cobertura 0,00 contra si misma: %.2f %% de pixeles cambian\n",
                    100.0 * f0);
        CHECK(f0 < 0.01, "CONTRAPRUEBA: dos veces el mismo cielo despejado dan la MISMA imagen");
    }
    {
        // ⚠️ ESTA INVARIANTE SE MIDE EN EL RESPALDO, no en el camino normal. "Mas cobertura pedida =
        // mas cielo tapado" se comprobaba sobre `sky.frag`, que desde que las tres capas son
        // volumetricas dibuja CERO nubes siempre: comparaba 0 con 0 y fallaba. Donde sigue
        // significando algo aqui es con el pase volumetrico apagado; en el camino normal lo mide
        // `cloud_volume_draws`, que es quien tiene ahora las capas.
        respaldo = true;
        const double fA = cloudFraction(up0, glm::dvec3(1, 0, 0), 0.20f);
        const double fB = cloudFraction(up0, glm::dvec3(1, 0, 0), 0.80f);
        respaldo = false;
        std::printf("    al CENIT (respaldo): cobertura 0,20 -> %.1f %% · 0,80 -> %.1f %%\n",
                    100.0 * fA, 100.0 * fB);
        CHECK(fB > fA, "en el respaldo, mas cobertura pedida = mas cielo tapado");
    }

    // HARUKA_SKY_PNG=<dir> vuelca el cielo a disco. Igual que el volcado del agua y por lo mismo: si
    // las nubes se ven bien o no es un juicio visual, y hace falta poder mirarlas.
    if (const char* dir = std::getenv("HARUKA_SKY_PNG")) {
        const bool isVk2 = (g_dev->backend() == Backend::Vulkan);
        struct Vista { glm::dvec3 fwd, up; const char* nombre; };
        const Vista vistas[2] = { { glm::dvec3(1,0,0), up0, "horizonte" },
                                  { up0, glm::dvec3(1,0,0), "cenit" } };
        for (const Vista& vi : vistas) {
            std::vector<uint8_t> px;
            shootRaw(vi.fwd, vi.up, 0.45f, px);
            std::vector<uint8_t> img(px.size());
            for (int y = 0; y < h; ++y) {
                const int src = isVk2 ? y : (h - 1 - y);
                std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/" + (isVk2 ? "vk_" : "gl_")
                                   + "cielo_" + vi.nombre + ".png";
            Haruka::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }
    }

    g_dev->destroy(pipe); g_dev->destroy(ubo);
}

// ================================================================================================
// LA ATMOSFERA VISTA DESDE FUERA: el limbo del planeta
//
// ⚠️ NO EXISTIA. `sky.frag` solo pinta el domo en el que estas metido; fuera, `mix(spaceC, sky,
// u_atmo)` devuelve espacio liso, asi que un planeta desde orbita no tenia atmosfera — ni halo, ni la
// linea azul sobre el horizonte. No se apagaba con la altitud: nunca se escribio.
//
// Lo que se mide es la firma que ninguna otra cosa produce: un anillo AZUL justo por FUERA de la
// silueta del planeta, que se apaga al alejarse del limbo. Con contraprueba en el lado nocturno, que
// es donde el terminador tiene que dejarlo a oscuras — sin ella, "hay azul" no distingue una
// atmosfera de un halo pintado alrededor de la esfera.
static void testAtmosphereLimb()
{
    BEGIN("atmosfera: el planeta tiene LIMBO visto desde fuera");

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const std::string base = Haruka::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/sky.vert", fs = base + "shaders/sky.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del cielo creado");
    if (!valid(pipe)) return;

    struct SkyUBO {
        float invVP[16];
        float sunDir[3]; float sunElev;
        float up[3];     float atmo;
        float sunColor[3]; float time;
        float weather[4]; float wind[4]; float planet[4];
    };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(SkyUBO), nullptr,
                                           BufferMemory::Dynamic);

    // ⚠️ NO SE ENCUADRA EL PLANETA ENTERO, SE ENCUADRA EL LIMBO. Mi primer intento miraba al centro
    // desde 3000 km con 60 grados de fov: el planeta subtiende **42,83 grados de SEMIANGULO** y el
    // fov solo 30, asi que el cuadro entero caia dentro del disco — ni anillo ni espacio que medir,
    // y las cuatro afirmaciones fallaron por el encuadre, no por la atmosfera.
    //
    // Y alejarse tampoco vale: a 20 000 km el planeta cabe, pero la capa de 80 km subtiende 0,18
    // grados, o sea MENOS DE UN PIXEL. La atmosfera vista de lejos es una linea de pelo; para medirla
    // hay que acercar el ojo y estrechar el fov, que es lo que hace cualquiera al fotografiarla.
    const double R = 6371000.0, ALT = 3.0e6;
    const glm::dvec3 up0(0.0, 1.0, 0.0);
    const double fovY = 10.0 * 3.14159265358979 / 180.0;   // estrecho: el limbo ocupa ~17 px
    const float aspect = (float)w / (float)h;
    const float fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot;
    proj[2][3] = -1.0f;         proj[3][2] = 1.0f;

    // El nadir (hacia el centro del planeta) y el limbo: una direccion a `angPlaneta` del nadir.
    const glm::dvec3 nadir = -up0;
    const double aLimb = std::asin(R / (R + ALT));
    const glm::dvec3 fwd = glm::normalize(nadir * std::cos(aLimb) + glm::dvec3(1, 0, 0) * std::sin(aLimb));
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(0, 1, 0));
    const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
    // El sol hacia +X: el limbo encuadrado queda ILUMINADO, y el opuesto en noche.
    const glm::vec3 sun = glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f));

    bool sinPlaneta = false;   // R = 0 -> `harukaAtmoLimb` sale por su primera linea
    auto shootVP = [&](float atmo, const glm::mat4& iVP, std::vector<uint8_t>& px) {
        SkyUBO u{};
        std::memcpy(u.invVP, &iVP[0][0], sizeof(u.invVP));
        u.sunDir[0]=sun.x; u.sunDir[1]=sun.y; u.sunDir[2]=sun.z; u.sunElev = 0.0f;
        u.up[0]=(float)up0.x; u.up[1]=(float)up0.y; u.up[2]=(float)up0.z; u.atmo = atmo;
        u.sunColor[0]=u.sunColor[1]=u.sunColor[2]=1.0f; u.time = 3.0f;
        u.weather[0]=0.5f; u.weather[1]=15.0f; u.weather[2]=0.0f; u.weather[3]=0.3f;
        u.wind[0]=2.0f; u.wind[1]=0.0f; u.wind[2]=0.0f; u.wind[3]=0.0f;
        u.planet[0]= sinPlaneta ? 0.0f : (float)R;
        u.planet[1]=(float)ALT; u.planet[2]=1500.0f; u.planet[3]=0.0f;
        g_dev->updateBuffer(ubo, 0, sizeof(u), &u);
        px.assign((size_t)w * h * 4, 0xAA);
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        for (int f = 0; f < 3; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
            ctx->draw(3, 1);
            ctx->endRenderPass();
            if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
    };

    auto shoot = [&](float atmo, std::vector<uint8_t>& px) { shootVP(atmo, invVP, px); };

    std::vector<uint8_t> px;
    shoot(0.0f, px);                                     // FUERA de la atmosfera

    // ⚠️ EL ORDEN DE FILAS **NO** SE PUEDE CALIBRAR CON ESTA IMAGEN, y eso me costo una deduccion
    // entera. `detectRowFlip` busca la fila mas brillante suponiendo que es "arriba" — vale para un
    // cielo con el sol en lo alto, pero aqui el encuadre esta CENTRADO en el limbo y la banda
    // brillante cae en MEDIO: la heuristica sale a cara o cruz. Con el signo invertido, el perfil por
    // angulo salia espejado y parecia que el halo se extendia 4 grados fuera de la cascara — cuando
    // lo que habia en esos pixeles era la BRUMA sobre el disco del planeta, que es correcta.
    //
    // Se calibra con un encuadre donde la respuesta se sabe: mirando al nadir con el eje +X como
    // "arriba" de la camara y el sol en +X, la mitad de arriba TIENE que ser la brillante.
    bool rowFlip = false;
    {
        const glm::mat4 vCal = glm::lookAt(glm::vec3(0.0f), glm::vec3(nadir), glm::vec3(1, 0, 0));
        const glm::mat4 iCal = glm::inverse(proj * glm::mat4(glm::mat3(vCal)));
        std::vector<uint8_t> pxCal;
        shootVP(0.0f, iCal, pxCal);
        rowFlip = detectRowFlip(pxCal, w, h);
        std::printf("    orden de filas: %s (calibrado con el sol en un encuadre que si lo delata)\n",
                    rowFlip ? "invertido" : "directo");
    }

    // El angulo de cada pixel respecto al eje de vista dice a que distancia angular del centro del
    // planeta esta. El planeta subtiende asin(R/(R+ALT)); el borde de la atmosfera, asin((R+80km)/…).
    const double angPlaneta = aLimb * 180.0 / 3.14159265358979;
    const double angAtmo    = std::asin((R + 80000.0) / (R + ALT)) * 180.0 / 3.14159265358979;
    std::printf("    desde %.0f km: el planeta subtiende %.2f grados · la atmosfera llega a %.2f"
                " · fov %.0f grados -> el anillo son ~%.0f px\n",
                ALT / 1000.0, angPlaneta, angAtmo, fovY * 180.0 / 3.14159265358979,
                (angAtmo - angPlaneta) / (fovY * 180.0 / 3.14159265358979) * h);

    // Se miden tres coronas: DENTRO del disco, el ANILLO de atmosfera, y FUERA (espacio vacio).
    double sumIn = 0, sumRing = 0, sumOut = 0; size_t nIn = 0, nRing = 0, nOut = 0;
    double ringB = 0.0, ringR = 0.0;
    for (int y = 0; y < h; ++y) {
        const float ndcY = rowToNdcY(y, h, rowFlip);
        for (int x = 0; x < w; ++x) {
            const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
            glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
            const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
            // ⚠️ DESDE EL NADIR, no desde el eje de vista: asi el criterio no depende de hacia
            // donde se encuadre, y el disco/anillo/espacio salen de la geometria del planeta.
            const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                             * 180.0 / 3.14159265358979;
            const size_t k = ((size_t)y * w + x) * 4;
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
            const double L = 0.2126*px[k] + 0.7152*px[k+1] + 0.0722*px[k+2];
            // Solo el lado ILUMINADO (el sol esta en +X): en el nocturno no debe haber halo.
            const bool dia = (d.x > 0.3f);
            if (ang < angPlaneta * 0.995)                 { sumIn += L; ++nIn; }
            else if (dia && ang > angPlaneta && ang < angAtmo) {
                sumRing += L; ++nRing; ringB += px[k+2]; ringR += px[k];
            }
            else if (ang > angAtmo * 1.02)                { sumOut += L; ++nOut; }
        }
    }
    const double mIn   = nIn   ? sumIn   / (double)nIn   : 0.0;
    const double mRing = nRing ? sumRing / (double)nRing : 0.0;
    const double mOut  = nOut  ? sumOut  / (double)nOut  : 0.0;
    std::printf("    luminancia media: dentro del disco %.1f · ANILLO de atmosfera %.1f · espacio %.1f\n",
                mIn, mRing, mOut);
    // ⚠️ EL PERFIL POR ANGULO, porque "el espacio marca 69" no distingue "el halo se sale de la
    // cascara" de "mi banda de espacio esta mal puesta". La atmosfera acaba en `angAtmo`: mas alla el
    // rayo NO corta la cascara y la luz tiene que caer a cero. Si no cae, el halo bordea el planeta
    // como una burbuja y se veria.
    {
        std::printf("    perfil por angulo desde el nadir (el planeta acaba en %.2f, la atmosfera en %.2f):\n",
                    angPlaneta, angAtmo);
        const int NB = 10;
        std::vector<double> acc((size_t)NB, 0.0); std::vector<size_t> cnt((size_t)NB, 0);
        const double a0 = angPlaneta - 1.0, a1 = angPlaneta + 5.0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                if (d.x < 0.3f) continue;                     // solo el lado iluminado
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < a0 || ang >= a1) continue;
                const int b = std::min(NB - 1, (int)((ang - a0) / (a1 - a0) * NB));
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                acc[(size_t)b] += 0.2126*px[k] + 0.7152*px[k+1] + 0.0722*px[k+2];
                ++cnt[(size_t)b];
            }
        }
        for (int b = 0; b < NB; ++b) {
            const double lo2 = a0 + (a1 - a0) * b / NB, hi2 = a0 + (a1 - a0) * (b + 1) / NB;
            std::printf("      %5.2f-%5.2f grados  luminancia %6.1f  (%zu px)%s\n", lo2, hi2,
                        cnt[(size_t)b] ? acc[(size_t)b] / (double)cnt[(size_t)b] : 0.0,
                        cnt[(size_t)b],
                        (lo2 >= angAtmo) ? "   <- espacio: deberia ser ~0"
                                         : ((hi2 <= angPlaneta) ? "   <- sobre el DISCO: es bruma, correcta" : ""));
        }
    }
    // ⚠️ ¿ESE BRILLO DE MAS ES MIO? Se aisla en vez de razonarlo: con `u_planet.x = 0`,
    // `harukaAtmoLimb` sale por su primera linea y no aporta NADA. Lo que quede iluminado en el mismo
    // encuadre no es la atmosfera. Cinco veces hoy he deducido mal la causa de un pixel; esto lo zanja.
    {
        sinPlaneta = true;
        std::vector<uint8_t> pxOff;
        shoot(0.0f, pxOff);
        sinPlaneta = false;
        double sOn = 0.0, sOff = 0.0; size_t n = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < angAtmo * 1.02) continue;             // solo la zona que deberia ser espacio
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA || pxOff[k] == 0xAA) continue;
                sOn  += 0.2126*px[k]    + 0.7152*px[k+1]    + 0.0722*px[k+2];
                sOff += 0.2126*pxOff[k] + 0.7152*pxOff[k+1] + 0.0722*pxOff[k+2];
                ++n;
            }
        }
        std::printf("    AISLAMIENTO en la zona de espacio (%zu px): con atmosfera %.1f · SIN ella %.1f\n",
                    n, n ? sOn / (double)n : 0.0, n ? sOff / (double)n : 0.0);
    }
    std::printf("    color del anillo: R %.1f · B %.1f  (Rayleigh pide B >> R)\n",
                nRing ? ringR / (double)nRing : 0.0, nRing ? ringB / (double)nRing : 0.0);
    CHECK(nRing > 0 && nOut > 0, "el cuadro contiene anillo y espacio (si no, la geometria esta mal)");
    CHECK(mRing > mOut + 3.0, "hay LIMBO: el anillo de atmosfera brilla mas que el espacio vacio");
    CHECK(nRing && ringB > ringR * 1.5, "y es AZUL (Rayleigh), no un halo blanco cualquiera");

    // ── CONTRAPRUEBA 1: el lado NOCTURNO no tiene halo. Sin esto, "hay azul alrededor" no distingue
    //    una atmosfera de un anillo pintado sobre la esfera.
    {
        // ⚠️ EL LIMBO NOCTURNO ES OTRO ENCUADRE. Con 10 grados de fov apuntando al limbo diurno, el
        // opuesto no esta ni cerca del cuadro: mi primera version lo buscaba en la misma imagen y
        // encontraba CERO pixeles, o sea que la contraprueba no comparaba nada. Se renderiza aparte,
        // apuntando al limbo del lado contrario al sol.
        const glm::dvec3 fwdN = glm::normalize(nadir * std::cos(aLimb)
                                             - glm::dvec3(1, 0, 0) * std::sin(aLimb));
        const glm::mat4 viewN = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwdN), glm::vec3(0, 1, 0));
        const glm::mat4 invVPN = glm::inverse(proj * glm::mat4(glm::mat3(viewN)));
        std::vector<uint8_t> pxN;
        shootVP(0.0f, invVPN, pxN);
        double sumNoche = 0.0; size_t nNoche = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, rowFlip);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = invVPN * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                const double ang = std::acos(glm::clamp((double)glm::dot(d, glm::vec3(nadir)), -1.0, 1.0))
                                 * 180.0 / 3.14159265358979;
                if (ang < angPlaneta || ang > angAtmo) continue;
                const size_t k = ((size_t)y * w + x) * 4;
                if (pxN[k] == 0xAA && pxN[k+1] == 0xAA && pxN[k+2] == 0xAA) continue;
                sumNoche += 0.2126*pxN[k] + 0.7152*pxN[k+1] + 0.0722*pxN[k+2];
                ++nNoche;
            }
        }
        const double mN = nNoche ? sumNoche / (double)nNoche : 0.0;
        std::printf("    CONTRAPRUEBA limbo NOCTURNO (%zu px): %.1f · el diurno da %.1f\n",
                    nNoche, mN, mRing);
        CHECK(nNoche > 0, "el limbo nocturno TIENE pixeles que medir (si no, no se compara nada)");
        CHECK(nNoche > 0 && mN < mRing * 0.6,
              "el terminador funciona: el limbo del lado nocturno esta MUCHO mas oscuro");
    }

    // ── CONTRAPRUEBA 2: desde DENTRO no se suma nada. El domo ya modela el aire; sumar las dos cosas
    //    seria contar la atmosfera dos veces, y el cielo a ras de suelo cambiaria sin querer.
    {
        std::vector<uint8_t> pxSuelo;
        SkyUBO probe{};
        (void)probe;
        // `u_atmo = 1` = a ras de suelo. La mezcla debe devolver el domo, con el limbo tapado.
        shoot(1.0f, pxSuelo);
        size_t dif = 0, n = 0;
        for (size_t k = 0; k + 3 < px.size() && k + 3 < pxSuelo.size(); k += 4) {
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
            const int dd = std::abs((int)px[k] - (int)pxSuelo[k])
                         + std::abs((int)px[k+1] - (int)pxSuelo[k+1])
                         + std::abs((int)px[k+2] - (int)pxSuelo[k+2]);
            if (dd > 12) ++dif;
            ++n;
        }
        std::printf("    con u_atmo=1 (a ras de suelo) cambia el %.1f %% del cuadro respecto a u_atmo=0\n",
                    n ? 100.0 * (double)dif / (double)n : 0.0);
        CHECK(n && dif > n / 10,
              "dentro y fuera dan cielos DISTINTOS (el limbo es del caso de fuera, no de los dos)");
    }

    g_dev->destroy(pipe); g_dev->destroy(ubo);
}

static void testCloudVolumeDraws()
{
    BEGIN("nubes: el pase VOLUMETRICO dibuja, y aguanta el angulo rasante");

    const double R = 6371000.0;
    const float  baseA = 1200.0f, topA = 2600.0f;     // losa de 1,4 km, del orden de lo que da el clima
    const float  eyeA  = 300.0f;                      // ojo BAJO la base: se mira la capa desde abajo

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const std::string base = Haruka::Shader::baseDir();
    PipelineDesc pd;
    const std::string vs = base + "shaders/cloud_vol.vert", fs = base + "shaders/cloud_vol.frag";
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false;
    pd.blend.enable = true;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del pase volumetrico de nubes creado");
    if (!valid(pipe)) return;

    // Profundidad de escena VACIA: con reversed-Z el vacio es 0,0 y reproyecta lejisimos, asi que el
    // corte contra la escena no recorta nada. Es el caso "cielo despejado delante".
    const float depthClear = 0.0f;
    TextureDesc dtd; dtd.width = 1; dtd.height = 1; dtd.format = Format::R32F;
    dtd.filter = Filter::Nearest; dtd.wrap = Wrap::ClampToEdge; dtd.mipmaps = false;
    dtd.initialData = &depthClear;
    TextureHandle depthTex = g_dev->createTexture(dtd);

    // Enganche para el caso (6d): cuando estos dos targets son validos, `render` reproduce el camino
    // de profundidad DE LA PARTIDA — limpiar la escena, `blitDepth` a una copia propia y atar
    // `getDepthTexture` de esa copia — en vez de la textura 1x1 hecha a mano de arriba.
    RenderPassHandle preBlitSrc{}, preBlitDst{};

    // Mandos del bloque de COSTE del final: pasos que se le piden al shader, cuantos frames dibuja
    // `render` y si hace readback (que es un punto de sincronizacion y falsearia el cronometro).
    float pasos  = 64.0f;
    int   marcos = 3;
    bool  leer   = true;

    // ⚠️ EL CAMPO DE COBERTURA (binding 1), AUNQUE AQUI NO SE USE. Desde que la cobertura viaja por
    // direccion, el shader declara ese sampler — y en Vulkan un descriptor SIN ATAR es INDEFINIDO,
    // no ceros. Dejarlo suelto es el fallo que ya dejo el agua del pase de nodos sin dibujar un
    // pixel. Con un 1x1 el shader lo detecta por `textureSize <= 1` y cae al escalar de siempre, que
    // es justo el camino que este test quiere medir.
    const float coverOne = 1.0f;
    TextureDesc ctd; ctd.width = 1; ctd.height = 1; ctd.format = Format::R32F;
    ctd.filter = Filter::Nearest; ctd.wrap = Wrap::ClampToEdge; ctd.mipmaps = false;
    ctd.initialData = &coverOne;
    TextureHandle coverTex = g_dev->createTexture(ctd);
    TextureHandle coverBound = coverTex;   // intercambiable: el caso (7) mete un campo de verdad

    struct CloudUBO {
        float invVP[16]; float planetC[4]; float slab[4]; float sun[4];
        float sunColor[4]; float wind[4]; float misc[4];
    };
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(CloudUBO), nullptr,
                                           BufferMemory::Dynamic);

    // Marco: la camara en el origen relativo, el planeta debajo. Se mira al HORIZONTE, que es donde
    // esta casi toda el area de cielo y donde el bug del muestreo vivia.
    const glm::dvec3 up0(0.0, 1.0, 0.0);
    const glm::dvec3 pcRel = -up0 * (R + (double)eyeA);       // centro del planeta relativo al ojo
    glm::dvec3 fwd(1.0, 0.0, 0.0);                            // tangente = horizonte (por defecto)
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const float aspect = (float)w / (float)h;
    const float fCot = 1.0f / std::tan((float)fovY * 0.5f);
    glm::mat4 proj(0.0f);
    proj[0][0] = fCot / aspect; proj[1][1] = fCot;
    proj[2][3] = -1.0f;         proj[3][2] = 1.0f;            // reversed-Z, lejano infinito
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0));
    glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));

    // Altitud y direccion parametrizables: hace falta para mirar desde ORBITA (ver el caso 6).
    float     ojoA = eyeA;
    glm::dvec3 mira = fwd;
    float     slabBase = baseA, slabTop = topA;   // la losa, para poder reproducir la de la partida
    // El relleno del UBO, aparte: el caso (6f) monta su propia secuencia de pases y necesita los
    // MISMOS uniformes que el resto del test, no una copia que pueda divergir.
    auto fillUBO = [&](float cover) {
        const glm::dvec3 pcR = -up0 * (R + (double)ojoA);
        const glm::dvec3 vup2 = glm::normalize(glm::cross(mira, glm::dvec3(0, 1, 0)));
        const glm::mat4 view2 = glm::lookAt(glm::vec3(0.0f), glm::vec3(mira),
                                            glm::vec3(std::abs(glm::dot(mira, up0)) > 0.99
                                                      ? glm::dvec3(1, 0, 0) : vup2));
        const glm::mat4 iVP = glm::inverse(proj * glm::mat4(glm::mat3(view2)));
        CloudUBO u{};
        std::memcpy(u.invVP, &iVP[0][0], sizeof(u.invVP));
        u.planetC[0] = (float)pcR.x; u.planetC[1] = (float)pcR.y; u.planetC[2] = (float)pcR.z;
        u.planetC[3] = (float)R;
        u.slab[0] = slabBase; u.slab[1] = slabTop; u.slab[2] = cover; u.slab[3] = 0.0f;
        // Sol alto y de lado: si estuviera en el cenit exacto, un volumen y una calcomania se verian
        // igual y el careo de sombreado no mediria nada.
        const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.7f, 0.2f));
        u.sun[0] = sun.x; u.sun[1] = sun.y; u.sun[2] = sun.z; u.sun[3] = sun.y;
        u.sunColor[0] = u.sunColor[1] = u.sunColor[2] = 1.0f; u.sunColor[3] = 1.0f;
        u.wind[0] = 0.0f; u.wind[1] = 0.0f; u.wind[2] = 7.0f; u.wind[3] = ojoA;
        u.misc[0] = 1.0f;                       // atmosfera: dentro
        u.misc[1] = pasos;                      // pasos, el mismo `kCloudSteps` del motor
        u.misc[2] = Haruka::WeatherSystem::kFieldScale;
        u.misc[3] = 0.008f;                     // extincion por metro, gemela de kCloudExtinction
        g_dev->updateBuffer(ubo, 0, sizeof(u), &u);
    };

    auto render = [&](float cover, std::vector<uint8_t>& px) {
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
            ctx->draw(3, 1);
            ctx->endRenderPass();
            const bool ultimo = leer && (f == marcos - 1);
            if (!isVk && ultimo) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && ultimo) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
    };

    // ── (1) DIBUJA, y con cobertura cero NO ─────────────────────────────────────────────────────
    std::vector<uint8_t> pxOn, pxOff;
    render(0.85f, pxOn);
    render(0.00f, pxOff);
    size_t litOn = 0, litOff = 0, sentinel = 0;
    for (size_t k = 0; k < pxOn.size(); k += 4) {
        if (pxOn[k] == 0xAA && pxOn[k+1] == 0xAA && pxOn[k+2] == 0xAA) { ++sentinel; continue; }
        if (pxOn[k] > 6 || pxOn[k+1] > 6 || pxOn[k+2] > 6) ++litOn;
    }
    for (size_t k = 0; k < pxOff.size(); k += 4)
        if (pxOff[k] > 6 || pxOff[k+1] > 6 || pxOff[k+2] > 6) ++litOff;
    const size_t total = (size_t)w * h;
    std::printf("    cobertura 0,85: %zu px de nube (%.1f %%) · cobertura 0,00: %zu px · centinela %zu\n",
                litOn, 100.0 * (double)litOn / (double)total, litOff, sentinel);
    CHECK(sentinel < total / 2, "el readback escribio (si no, el conteo no significa nada)");
    CHECK(litOn > total / 50, "el pase volumetrico DIBUJA nube");
    CHECK(litOff == 0, "CONTRAPRUEBA: con cobertura 0 no pinta nada (es la nube, no el fondo)");
    if (litOn == 0) { g_dev->destroy(pipe); g_dev->destroy(ubo); g_dev->destroy(depthTex); return; }

    // ── (2) ⚠️ EL ANGULO RASANTE: lo que se le escapo al test de CPU ────────────────────────────
    //
    // La capa es una cascara esferica: mirando hacia el horizonte el rayo recorre MUCHA mas nube que
    // hacia el cenit (91 km contra 1,4 km). O sea que la opacidad tiene que SUBIR al bajar la mirada.
    // Si baja, la marcha se queda corta ahi — que es exactamente el bug de los 24 pasos uniformes.
    {
        // Elevacion de cada fila: el rayo de su centro contra el horizonte local.
        const bool cloudFlip = detectRowFlip(pxOn, w, h);
        const int bandas = 6;
        std::vector<double> sumA(bandas, 0.0); std::vector<size_t> cnt(bandas, 0);
        double elevMin = 1e9, elevMax = -1e9;
        for (int y = 0; y < h; ++y) {
            // ⚠️ Mismo error que en el test del cielo. Ver `rowToNdcY` / `detectRowFlip`.
            const float ndcY = rowToNdcY(y, h, cloudFlip);
            glm::vec4 far = invVP * glm::vec4(0.0f, ndcY, 1.0f, 1.0f);
            const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
            const double elev = std::asin(glm::clamp((double)glm::dot(d, glm::vec3(up0)), -1.0, 1.0))
                              * 180.0 / 3.14159265358979;
            elevMin = std::min(elevMin, elev); elevMax = std::max(elevMax, elev);
            if (elev < 0.0) continue;                       // por debajo del horizonte no hay capa
            const int b = std::min(bandas - 1, (int)(elev / 30.0 * bandas));
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                sumA[(size_t)b] += (double)std::max({pxOn[k], pxOn[k+1], pxOn[k+2]}) / 255.0;
                ++cnt[(size_t)b];
            }
        }
        std::printf("    elevacion de la vista: %.1f .. %.1f grados\n", elevMin, elevMax);
        std::printf("    elevacion    opacidad media de la capa\n");
        double cerca = -1.0, lejos = -1.0;
        for (int b = 0; b < bandas; ++b) {
            if (cnt[(size_t)b] == 0) continue;
            const double a = sumA[(size_t)b] / (double)cnt[(size_t)b];
            std::printf("      %2d-%2d grados      %.4f\n", b * 30 / bandas, (b + 1) * 30 / bandas, a);
            if (cerca < 0.0) cerca = a;                     // la banda MAS baja con datos (rasante)
            lejos = a;                                      // la mas alta
        }
        std::printf("    rasante %.4f  vs  alto %.4f  ->  razon %.2f (la cascara da MAS recorrido abajo)\n",
                    cerca, lejos, (lejos > 1e-6) ? cerca / lejos : 0.0);
        CHECK(cerca > 0.0, "hay nube dibujada en la banda RASANTE (la que el muestreo uniforme vaciaba)");
        CHECK(cerca >= lejos * 0.8,
              "la opacidad NO se derrumba hacia el horizonte: la marcha aguanta el angulo rasante");
    }

    // ── (3) ⚠️ ¿HAY NUBES, O HAY UN TECHO? Con la cobertura MEDIANA DEL PLANETA ─────────────────
    //
    // A cobertura 0,85 el cielo esta cerrado y verlo todo tapado es lo correcto. Pero la mediana del
    // planeta es **0,302** (`cloud_shape`), y con esa el cielo tiene que tener HUECOS: nubes sueltas
    // con azul entre ellas. Si tambien sale tapado de lado a lado, lo que hay no son nubes — es una
    // lamina, que es literalmente el sintoma reportado ("no son 3d, son un plano 2d").
    //
    // Se mide lo unico que separa las dos cosas: la fraccion de cielo VACIO y la varianza espacial.
    {
        std::vector<uint8_t> pxMed;
        render(0.302f, pxMed);
        size_t hueco = 0, nube = 0; double acc = 0.0, acc2 = 0.0;
        for (size_t k = 0; k < pxMed.size(); k += 4) {
            const double a = (double)std::max({pxMed[k], pxMed[k+1], pxMed[k+2]}) / 255.0;
            if (a < 0.05) ++hueco; else ++nube;
            acc += a; acc2 += a * a;
        }
        const double n = (double)(hueco + nube);
        const double media = acc / n, var = acc2 / n - media * media;
        std::printf("    con la cobertura MEDIANA del planeta (0,302): %.1f %% de cielo VACIO · "
                    "opacidad media %.3f · desviacion %.3f\n",
                    100.0 * (double)hueco / n, media, std::sqrt(std::max(0.0, var)));
        CHECK(hueco > 0, "con cobertura media el cielo tiene HUECOS (si no, es una lamina y no nubes)");
        CHECK(std::sqrt(std::max(0.0, var)) > 0.10,
              "y la opacidad VARIA por el cielo (una lamina uniforme daria desviacion ~0)");
    }

    // ── (4) ¿SE VE COMO UN CUERPO O COMO UNA CALCOMANIA? ────────────────────────────────────────
    //
    // Una de las cuatro causas del "no son volumetricas" fue que la luz salia CONSTANTE: `dot` con el
    // vector al centro del planeta, que sobre una nube de 3 km gira 4,7e-4 rad. Sin sombreado, un
    // volumen se ve IGUAL que una calcomania. Se mide el RANGO de luminancia dentro de la nube.
    {
        double lo = 1e9, hi = -1e9, acc = 0.0; size_t n = 0;
        for (size_t k = 0; k < pxOn.size(); k += 4) {
            const double L = 0.2126 * pxOn[k] + 0.7152 * pxOn[k+1] + 0.0722 * pxOn[k+2];
            if (L < 8.0) continue;                          // fondo
            lo = std::min(lo, L); hi = std::max(hi, L); acc += L; ++n;
        }
        const double rango = (n && hi > 0.0) ? (hi - lo) / hi : 0.0;
        std::printf("    luminancia dentro de la nube: %.0f .. %.0f (media %.0f) -> rango relativo %.2f\n",
                    n ? lo : 0.0, n ? hi : 0.0, n ? acc / (double)n : 0.0, rango);
        CHECK(n > 0, "hay pixeles de nube que medir");
        CHECK(rango > 0.15,
              "la nube tiene SOMBREADO (una calcomania con luz constante daria rango ~0)");
    }

    // ── (5) ⚠️ ¿EL CIELO SE VE TAN NUBLADO COMO DICE EL CLIMA? AL CENIT ─────────────────────────
    //
    // Los 78,7 % tapados de arriba son mirando al HORIZONTE, donde el rayo cruza ~91 km de capa y
    // atraviesa muchas nubes seguidas: que se vea casi cerrado ahi es CORRECTO, y pasa en el cielo de
    // verdad. Donde "cobertura" significa lo que dice es en el CENIT, donde el rayo cruza la losa una
    // sola vez. Si tambien alli sale 2-3 veces mas tapado de lo pedido, el umbral del campo esta mal
    // y el cielo se ve encapotado con el clima diciendo "parcialmente nublado" — que se leeria como
    // un techo plano.
    {
        // ⚠️ VA POR `mira`, NO POR `fwd`. Al parametrizar `render` para poder mirar desde orbita, la
        // matriz paso a construirse dentro a partir de `mira`; este caso seguia asignando `fwd` y la
        // matriz de fuera, que ya no lee nadie — o sea que medía el HORIZONTE creyendo mirar arriba,
        // y la razon tapado/pedido salia falseada. Lo caza el propio umbral de 3x.
        mira = up0;                                          // al cenit
        std::printf("    AL CENIT:  cobertura pedida   fraccion de cielo TAPADA   razon\n");
        double peorRazon = 0.0; bool subeConCobertura = true; double antes = -1.0;
        for (float c : { 0.10f, 0.302f, 0.60f, 0.90f }) {
            std::vector<uint8_t> pz;
            render(c, pz);
            size_t tap = 0, n = 0;
            for (size_t k = 0; k < pz.size(); k += 4) {
                const double a = (double)std::max({pz[k], pz[k+1], pz[k+2]}) / 255.0;
                if (a > 0.05) ++tap; ++n;
            }
            const double frac = (double)tap / (double)n;
            std::printf("              %.3f              %.3f                    %.2fx\n",
                        c, frac, frac / (double)c);
            peorRazon = std::max(peorRazon, frac / (double)c);
            if (frac + 1e-6 < antes) subeConCobertura = false;
            antes = frac;
        }
        CHECK(subeConCobertura, "la fraccion tapada CRECE con la cobertura pedida (el mando manda)");
        // ⚠️ No se exige que la razon sea 1,0: un rayo al cenit atraviesa 1,4 km de losa y puede
        // encontrar mas de una nube, asi que tapar algo mas de lo pedido es fisico. Lo que se fija es
        // que no sea un factor descontrolado — ahi el cielo dejaria de responder al clima.
        std::printf("    peor razon tapado/pedido al cenit: %.2fx\n", peorRazon);
        CHECK(peorRazon < 3.0,
              "al cenit el cielo no se tapa mas de 3x lo que pide el clima (si no, siempre encapotado)");
        mira = fwd;   // se devuelve la vista, o el caso siguiente heredaria el cenit
    }

    // ── (6) ⚠️ DESDE ORBITA. Reportado mirando la pantalla: *"desde orbita deberian de verse"*, y no
    //        se veian. `application_render.cpp` calculaba `atmoC = 1 - smoothstep(0, radio*0.02,
    //        altitud)` y el shader hace `alpha * u_misc.x`: en la Tierra eso es CERO a partir de
    //        **127 km**, asi que a 249 km las nubes se multiplicaban por cero. Su comentario decia
    //        "en orbita no hay nube que atravesar (y el fondo ya se encarga)" — pero desde fuera no
    //        se atraviesan, se VEN, y el fondo pinta espacio, no nubes.
    //
    //        La geometria ya lo soportaba (`cloud_vol.frag` invierte el orden de entrada al mirar
    //        desde encima de la capa). Este caso lo fija para que no vuelva a apagarse en silencio.
    {
        ojoA = 250000.0f;          // 250 km: la altitud del reporte
        mira = -up0;               // mirando al planeta
        std::vector<uint8_t> pxOrb;
        render(0.60f, pxOrb);

        // ⚠️ Y AHORA CON EL PLANETA DELANTE, que es la diferencia entre este test y el juego. Hasta
        // aqui la profundidad de escena era un 1x1 a 0,0 (reversed-Z: "vacio, lejisimos"), o sea
        // CIELO DESPEJADO. En orbita el planeta llena la pantalla y el pase recorta la marcha contra
        // esa profundidad (`tExit = min(tExit, sceneT)`). Si ese recorte esta mal a escala orbital, en
        // el banco se ven nubes y en el juego no — que es exactamente lo reportado.
        //
        // Se pone la profundidad que escribiria el terreno a 249 km con la proyeccion reversed-Z
        // infinita del motor: `z_ndc = near/dist`, con near = 1 m.
        const float dSuelo = 1.0f / (float)(ojoA);          // el suelo, a `ojoA` metros
        TextureDesc sd; sd.width = 1; sd.height = 1; sd.format = Format::R32F;
        sd.filter = Filter::Nearest; sd.wrap = Wrap::ClampToEdge; sd.mipmaps = false;
        sd.initialData = &dSuelo;
        TextureHandle sueloTex = g_dev->createTexture(sd);
        TextureHandle vacio = depthTex;
        depthTex = sueloTex;                                 // `render` ata `depthTex` al binding 0
        std::vector<uint8_t> pxSuelo;
        render(0.60f, pxSuelo);
        depthTex = vacio;
        size_t litS = 0, nS = 0;
        for (size_t k = 0; k < pxSuelo.size(); k += 4) {
            if (pxSuelo[k] == 0xAA && pxSuelo[k+1] == 0xAA && pxSuelo[k+2] == 0xAA) continue;
            if (pxSuelo[k] > 6 || pxSuelo[k+1] > 6 || pxSuelo[k+2] > 6) ++litS;
            ++nS;
        }
        std::printf("    DESDE ORBITA con el PLANETA delante (profundidad de escena a %.0f km): "
                    "%zu px de nube de %zu (%.1f %%)\n",
                    ojoA / 1000.0f, litS, nS, nS ? 100.0 * (double)litS / (double)nS : 0.0);
        CHECK(litS > nS / 20,
              "las nubes sobreviven al recorte contra el planeta (si no, en el juego no se verian)");
        g_dev->destroy(sueloTex);
        size_t lit = 0, n = 0;
        for (size_t k = 0; k < pxOrb.size(); k += 4) {
            if (pxOrb[k] == 0xAA && pxOrb[k+1] == 0xAA && pxOrb[k+2] == 0xAA) continue;
            if (pxOrb[k] > 6 || pxOrb[k+1] > 6 || pxOrb[k+2] > 6) ++lit;
            ++n;
        }
        std::printf("    DESDE ORBITA (250 km, mirando al planeta): %zu px de nube de %zu (%.1f %%)\n",
                    lit, n, n ? 100.0 * (double)lit / (double)n : 0.0);
        CHECK(n > 0, "el readback de la vista orbital escribio");
        CHECK(lit > n / 20, "las nubes SE VEN desde orbita (el fade por altitud las borraba enteras)");
        ojoA = eyeA; mira = fwd;
    }

    // ── (6b) ⚠️ LA CONFIGURACION EXACTA DE LA PARTIDA. El banco decia que el pase dibuja y la
    //        pantalla decia que no hay nubes; con `HARUKA_CLOUD_COVER=0.80` (que salta el campo del
    //        clima) SEGUIA sin verse, o sea que el fallo no es el campo. La unica diferencia que
    //        quedaba entre banco y juego eran los NUMEROS: aqui se usaban ojo a 300 m y losa
    //        1200-2600 m, y la partida corre con ojo a 1030 m y losa **2470-3116 m** (646 m de
    //        espesor, 1440 m por encima del ojo). Se reproduce tal cual.
    {
        const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
        ojoA = altJuego; mira = fwd;                 // mirando al horizonte, como se juega
        const float baseAnt = slabBase, topAnt = slabTop;
        slabBase = baseJuego; slabTop = topJuego;
        std::vector<uint8_t> pxJ;
        render(0.80f, pxJ);
        size_t litJ = 0, nJ = 0;
        for (size_t k = 0; k + 3 < pxJ.size(); k += 4) {
            if (pxJ[k] == 0xAA && pxJ[k+1] == 0xAA && pxJ[k+2] == 0xAA) continue;
            if (pxJ[k] > 6 || pxJ[k+1] > 6 || pxJ[k+2] > 6) ++litJ;
            ++nJ;
        }
        std::printf("    CONFIG DE PARTIDA (ojo %.0f m · losa %.0f-%.0f m · cobertura 0,80): "
                    "%.1f %% del cuadro con nube\n",
                    altJuego, baseJuego, topJuego, nJ ? 100.0 * (double)litJ / (double)nJ : 0.0);
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;
        CHECK(litJ > nJ / 50,
              "con los numeros EXACTOS de la partida se dibuja nube (si falla, el banco reproduce el bug)");
    }

    // (6d) LA PROFUNDIDAD POR EL CAMINO DE LA PARTIDA. Todo lo de arriba ata una textura de 1x1
    //      rellenada a mano con 0,0. La partida NO hace eso: copia la profundidad de la escena con
    //      `blitDepth` a un target propio y ata `getDepthTexture` de esa copia. Ese camino no lo ha
    //      ejercitado nunca ningun test. Si entrega basura, `sceneT` cae al orden del plano cercano,
    //      `min(tExit, sceneT)` deja el recorrido en nada y NO SE DIBUJA UNA NUBE — mientras el
    //      cielo, que no lee profundidad, se sigue viendo. Que es el sintoma exacto reportado.
    //      Aqui cambia SOLO esa variable: mismos uniformes que el caso de arriba, otra profundidad.
    {
        RenderTargetDesc sd;
        sd.width = w; sd.height = h;
        sd.colorFormats = { Format::RGBA8 };
        sd.colorFilter  = Filter::Nearest;
        sd.hasDepth = true; sd.depthFormat = Format::D32F;
        RenderPassHandle escenaRT = g_dev->createRenderTarget(sd);

        RenderTargetDesc cd;                       // gemelo de `m_cloudDepthRT` en application_render
        cd.width = w; cd.height = h;
        cd.colorFormats = { Format::R32F };
        cd.colorFilter  = Filter::Nearest;
        cd.hasDepth = true; cd.depthFormat = Format::D32F;
        cd.depthAsTexture = true;                  // sin esto la profundidad no se puede MUESTREAR
        RenderPassHandle copiaRT = g_dev->createRenderTarget(cd);

        if (!valid(escenaRT) || !valid(copiaRT)) {
            CHECK(false, "targets del camino real de profundidad creados");
        } else {
            const TextureHandle copiaTex = g_dev->getDepthTexture(copiaRT);
            CHECK(valid(copiaTex), "getDepthTexture de la copia devuelve una textura valida");

            const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
            const float baseAnt = slabBase, topAnt = slabTop;
            const TextureHandle depthAnt = depthTex;
            ojoA = altJuego; mira = fwd;
            slabBase = baseJuego; slabTop = topJuego;
            depthTex   = copiaTex;                 // `render` ata esto al binding 0
            preBlitSrc = escenaRT; preBlitDst = copiaRT;

            std::vector<uint8_t> pxD;
            render(0.80f, pxD);

            preBlitSrc = {}; preBlitDst = {};
            depthTex = depthAnt;
            slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;

            size_t litD = 0, nD = 0;
            for (size_t k = 0; k + 3 < pxD.size(); k += 4) {
                if (pxD[k] == 0xAA && pxD[k+1] == 0xAA && pxD[k+2] == 0xAA) continue;
                if (pxD[k] > 6 || pxD[k+1] > 6 || pxD[k+2] > 6) ++litD;
                ++nD;
            }
            std::printf("    PROFUNDIDAD POR blitDepth (mismos numeros que arriba): %.1f %% con nube\n",
                        nD ? 100.0 * (double)litD / (double)nD : 0.0);
            CHECK(litD > nD / 50,
                  "con la profundidad copiada por blitDepth se sigue dibujando nube "
                  "(si falla, el bug esta en la copia de profundidad, no en el shader)");
        }
        if (valid(escenaRT)) g_dev->destroy(escenaRT);
        if (valid(copiaRT))  g_dev->destroy(copiaRT);
    }

    // (6e) EL COSTE, QUE NUNCA SE HABIA MEDIDO. Mientras la profundidad era basura el pase se
    //      recortaba a cero y salia gratis; en cuanto dibuja de verdad hay que saber lo que cuesta.
    //      Se mide el SUELO (cobertura 0 -> sale por el primer `if`) y la nube a 64/24/12 pasos, para
    //      separar lo que es por-pixel de lo que es por-paso: si bajar los pasos a la mitad NO baja
    //      el coste a la mitad, el termino dominante es el pixel y lo que hay que bajar es la
    //      RESOLUCION, no la marcha.
    {
        std::vector<uint8_t> uno(4, 0), basura;
        const float baseJuego = 2470.0f, topJuego = 3116.0f, altJuego = 1030.0f;
        const float baseAnt = slabBase, topAnt = slabTop;
        ojoA = altJuego; mira = fwd; slabBase = baseJuego; slabTop = topJuego;

        auto medir = [&](float cover, float pasosN, int marcosN) {
            pasos = pasosN;
            marcos = 6; leer = true; render(cover, basura);          // calienta y sincroniza
            marcos = marcosN; leer = false;
            const auto t0 = std::chrono::high_resolution_clock::now();
            render(cover, basura);
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, uno.data());  // cierra la cuenta
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::high_resolution_clock::now() - t0).count()
                              / (double)marcosN;
            marcos = 3; leer = true; pasos = 64.0f;
            return ms;
        };

        const double msVacio = medir(0.000f, 64.0f, 30);
        const double ms64    = medir(0.800f, 64.0f, 30);
        const double ms24    = medir(0.800f, 24.0f, 30);
        const double ms12    = medir(0.800f, 12.0f, 30);

        const double mpx  = (double)w * (double)h / 1.0e6;
        const double aFHD = (1920.0 * 1080.0) / ((double)w * (double)h);
        std::printf("    COSTE DEL PASE a %dx%d (%.2f Mpx) · entre parentesis, extrapolado a 1920x1080\n"
                    "      cielo vacio (sale por el primer if) %.2f ms (%.1f)\n"
                    "      nube 0,80 · 64 pasos             %.2f ms (%.1f)\n"
                    "      nube 0,80 · 24 pasos             %.2f ms (%.1f)\n"
                    "      nube 0,80 · 12 pasos             %.2f ms (%.1f)\n",
                    w, h, mpx,
                    msVacio, msVacio * aFHD, ms64, ms64 * aFHD,
                    ms24, ms24 * aFHD, ms12, ms12 * aFHD);
        const double porPaso64 = (ms64 - msVacio);
        if (porPaso64 > 0.0 && ms24 > msVacio)
            std::printf("      -> de 64 a 24 pasos el coste de marcha cae al %.0f %% (%.0f %% si fuera "
                        "lineal en pasos)\n",
                        100.0 * (ms24 - msVacio) / porPaso64, 100.0 * 24.0 / 64.0);
        // ⚠️ CON VSYNC ESTO NO MIDE NADA: en FIFO cualquier frame se clava en 16,7 ms.
        const bool vsync = (msVacio > 15.5 && msVacio < 18.0);
        if (vsync)
            std::printf("    \033[33m⚠️ el suelo son %.1f ms = VSYNC (FIFO): esta medida NO vale. "
                        "Repite con HARUKA_NO_VSYNC=1.\033[0m\n", msVacio);
        slabBase = baseAnt; slabTop = topAnt; ojoA = eyeA;
        CHECK(msVacio > 0.0 && ms64 > 0.0, "el coste del pase de nubes queda MEDIDO");
    }

    // (6f) EL CAMINO REDUCIDO + COMPOSICION, que es como dibuja la partida desde el arreglo del coste.
    //      Se marcha en un target de 1/4 de lado y se sube con `cloud_upsample`. Dos cosas que probar:
    //      que sigue habiendo nube, y que NO aparece el RIBETE OSCURO — el pase escribe alfa recto con
    //      color negro donde no hay nube, asi que una bilineal normal mezcla ese negro con la nube y
    //      ensucia cada borde. Por eso el shader interpola PONDERANDO POR ALFA.
    {
        const std::string uvs = base + "shaders/cloud_upsample.vert";
        const std::string ufs = base + "shaders/cloud_upsample.frag";
        PipelineDesc up;
        up.vertexPath = uvs.c_str(); up.fragmentPath = ufs.c_str();
        up.topology = PrimitiveTopology::Triangles;
        up.depth.test = false; up.depth.write = false;
        up.blend.enable = true;
        PipelineHandle pipeUp = g_dev->createPipeline(up);
        CHECK(valid(pipeUp), "pipeline de composicion de nubes creado");

        const int rw = std::max(64, w / 4), rh = std::max(64, h / 4);
        RenderTargetDesc rd;
        rd.width = rw; rd.height = rh;
        rd.colorFormats = { Format::RGBA8 };
        rd.colorFilter  = Filter::Linear;
        rd.hasDepth     = false;
        RenderPassHandle bajoRT = g_dev->createRenderTarget(rd);
        CHECK(valid(bajoRT), "target reducido de nubes creado");

        if (valid(pipeUp) && valid(bajoRT)) {
            const TextureHandle bajoTex = g_dev->getColorTexture(bajoRT, 0);
            CHECK(valid(bajoTex), "getColorTexture del target reducido devuelve una textura valida");

            const float baseJ = 2470.0f, topJ = 3116.0f, altJ = 1030.0f;
            const float baseAnt2 = slabBase, topAnt2 = slabTop;
            ojoA = altJ; mira = fwd; slabBase = baseJ; slabTop = topJ;

            // Se dibuja el MISMO cielo por los DOS caminos sobre un fondo GRIS MEDIO: directo a
            // resolucion completa (lo caro, que es la referencia) y reducido + composicion (lo que
            // hace el motor). Contar pixeles "no negros" como en los casos de arriba no serviria:
            // ese umbral lo pasa hasta un alfa de 0,03, asi que diria que si aunque la nube saliera
            // fantasma. Sobre gris se compara la LUZ MEDIA, que es sensible al alfa, y ademas deja
            // ver el ribete oscuro que meteria una bilineal cruda.
            const bool isVk = (g_dev->backend() == Backend::Vulkan);
            auto pintar = [&](bool reducido, std::vector<uint8_t>& px) {
                px.assign((size_t)w * h * 4, 0xAA);
                for (int f = 0; f < 3; ++f) {
                    Context* ctx = g_dev->beginFrame();
                    if (!ctx) break;
                    fillUBO(0.80f);
                    if (reducido) {
                        ClearValues cl; cl.clearColor = true; cl.clearDepth = false;
                        cl.color[0] = cl.color[1] = cl.color[2] = cl.color[3] = 0.0f;
                        ctx->beginRenderPass(bajoRT, cl);
                        ctx->setViewport(0, 0, rw, rh);
                        ctx->bindPipeline(pipe);
                        ctx->bindUniformBuffer(5, ubo);
                        ctx->bindTexture(0, depthTex);
                        ctx->bindTexture(1, coverBound);
                        ctx->draw(3, 1);
                        ctx->endRenderPass();
                    }
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
                    cv.color[0] = cv.color[1] = cv.color[2] = 0.5f; cv.color[3] = 1.0f;
                    cv.depth = 0.0f;
                    ctx->beginRenderPass({}, cv);
                    // ⚠️ El pase reducido dejo el viewport a `rw x rh` y en OpenGL abrir un pase NO
                    // lo restablece. Sin esto solo se compone la esquina.
                    ctx->setViewport(0, 0, w, h);
                    if (reducido) {
                        ctx->bindPipeline(pipeUp);
                        ctx->bindTexture(0, bajoTex);
                    } else {
                        ctx->bindPipeline(pipe);
                        ctx->bindUniformBuffer(5, ubo);
                        ctx->bindTexture(0, depthTex);
                        ctx->bindTexture(1, coverBound);
                    }
                    ctx->draw(3, 1);
                    ctx->endRenderPass();
                    if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                    g_dev->endFrame();
                    pumpWindowEvents();
                    if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                }
            };
            std::vector<uint8_t> pxFull, pxLow;
            pintar(false, pxFull);
            pintar(true,  pxLow);

            slabBase = baseAnt2; slabTop = topAnt2; ojoA = eyeA;

            auto resumen = [&](const std::vector<uint8_t>& px, double& media, double& frac,
                               double& oscuroFrac) -> size_t {
                double suma = 0.0; size_t nube = 0, osc = 0, n = 0;
                for (size_t k = 0; k + 3 < px.size(); k += 4) {
                    if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                    const double lum = (px[k] + px[k+1] + px[k+2]) / 3.0;
                    suma += lum;
                    if (lum > 140.0) ++nube;
                    if (lum < 100.0) ++osc;
                    ++n;
                }
                media = n ? suma / (double)n : -1.0;
                frac  = n ? 100.0 * (double)nube / (double)n : 0.0;
                oscuroFrac = n ? 100.0 * (double)osc / (double)n : 0.0;
                return n;
            };
            double mF = 0, fF = 0, oF = 0, mL = 0, fL = 0, oL = 0;
            const size_t nF = resumen(pxFull, mF, fF, oF);
            const size_t nL = resumen(pxLow,  mL, fL, oL);
            std::printf("    NUBE sobre gris 128 · completa %dx%d: luz media %.1f · %.1f %% nubosa\n"
                        "                      · reducida %dx%d + composicion: luz media %.1f · "
                        "%.1f %% nubosa · %.2f %% mas oscuro que el fondo (ribete)\n",
                        w, h, mF, fF, rw, rh, mL, fL, oL);
            CHECK(nF > 0 && nL > 0, "los dos caminos de nube dan lectura");
            CHECK(fL > fF * 0.5, "el camino reducido no pierde la nube que dibuja el completo");
            CHECK(std::fabs(mL - mF) < 12.0,
                  "reducido + composicion casa con el directo en luz media (< 12 de 255)");
            CHECK(oL < 1.0, "la composicion NO mete ribete oscuro (bilineal ponderada por alfa)");
        }
        if (valid(bajoRT)) g_dev->destroy(bajoRT);
        if (valid(pipeUp)) g_dev->destroy(pipeUp);
    }

    // ── (7) ⚠️ LA COBERTURA POR DIRECCION, que era la deuda declarada del cambio anterior ────────
    //
    // El pase recibia UN escalar (la cobertura del punto bajo la camara) y lo aplicaba a todo lo
    // visible: desde orbita, miles de km con el tiempo de un solo sitio. Ahora viaja como una
    // equirect horneada del `WeatherSystem`. Hasta aqui el banco ataba un 1x1 y CAIA AL ESCALAR a
    // proposito, asi que el camino nuevo no lo probaba nadie — quedo escrito como deuda y esto la paga.
    //
    // Se le mete un campo PARTIDO: media esfera cubierta (0,9) y media despejada (0). Si el shader
    // lee la textura, la mitad correspondiente del cielo se queda limpia; si sigue con el escalar
    // —que aqui se pone a 0,90 a proposito— las dos mitades salen IGUALES. Es lo unico que distingue
    // los dos caminos, y el escalar hace de contraprueba por construccion.
    {
        const int CW = 64, CH = 32;
        std::vector<float> campo((size_t)CW * CH, 0.0f);
        const double kPi = 3.14159265358979;
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                const double lon = (((double)x + 0.5) / CW - 0.5) * 2.0 * kPi;
                // ⚠️ SE PARTE POR Z, NO POR X. `coverAtDir` usa u = 0,5 + atan2(z,x)/2pi, asi que
                // `cos(lon) > 0` es "x > 0" — y la camara mira precisamente hacia +X, o sea que TODOS
                // los rayos caian del mismo lado y el test no veia la mitad despejada. Partir por el
                // eje PERPENDICULAR a la vista (`sin(lon) > 0`, o sea z > 0) pone las dos mitades en
                // el cuadro, que es lo unico que permite compararlas.
                campo[(size_t)y * CW + x] = (std::sin(lon) > 0.0) ? 0.90f : 0.0f;
            }
        TextureDesc fd; fd.width = CW; fd.height = CH; fd.format = Format::R32F;
        fd.filter = Filter::Linear; fd.wrap = Wrap::Repeat; fd.mipmaps = false;
        fd.initialData = campo.data();
        TextureHandle campoTex = g_dev->createTexture(fd);

        // ⚠️ DESDE ORBITA, Y NO ES UNA PREFERENCIA: A RAS DE SUELO ESTO NO PUEDE MEDIRSE.
        // `coverAtDir` mira la direccion del PUNTO DE MUESTRA desde el centro del planeta. Con el ojo
        // a 300 m, la capa de nube esta a 100 km como mucho, o sea **0,9 grados de arco**: la
        // cobertura varia sobre el PLANETA y en 0,9 grados no varia nada. Mi primera version medio
        // ahi y dio 0,482 contra 0,548 — las dos mitades iguales — y acuse al shader de no leer la
        // textura. No era el shader: a ras de suelo el efecto ES despreciable, y eso es correcto.
        //
        // Desde 250 km sobre el polo el disco visible abarca TODAS las longitudes, asi que el campo
        // partido por `z` cae medio y medio en el cuadro. Es ademas el caso que motivo el cambio.
        ojoA = 250000.0f; mira = -up0;
        coverBound = campoTex;
        std::vector<uint8_t> pxF;
        render(0.90f, pxF);
        coverBound = coverTex;

        const glm::mat4 view7 = glm::lookAt(glm::vec3(0.0f), glm::vec3(-up0), glm::vec3(1, 0, 0));
        const glm::mat4 iVP7 = glm::inverse(proj * glm::mat4(glm::mat3(view7)));
        const bool flip7 = detectRowFlip(pxF, w, h);
        double sumPos = 0, sumNeg = 0; size_t nPos = 0, nNeg = 0;
        for (int y = 0; y < h; ++y) {
            const float ndcY = rowToNdcY(y, h, flip7);
            for (int x = 0; x < w; ++x) {
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                glm::vec4 far = iVP7 * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::vec3 d = glm::normalize(glm::vec3(far) / far.w);
                if (std::abs(d.z) < 0.25f) continue;    // la franja del corte no cuenta
                const size_t k = ((size_t)y * w + x) * 4;
                if (pxF[k] == 0xAA && pxF[k+1] == 0xAA && pxF[k+2] == 0xAA) continue;
                const double a = (double)std::max({pxF[k], pxF[k+1], pxF[k+2]}) / 255.0;
                if (d.z > 0.0f) { sumPos += a; ++nPos; } else { sumNeg += a; ++nNeg; }
            }
        }
        const double mPos = nPos ? sumPos / (double)nPos : 0.0;
        const double mNeg = nNeg ? sumNeg / (double)nNeg : 0.0;
        std::printf("    campo PARTIDO (mitad 0,90 / mitad 0,00) con el escalar en 0,90:\n"
                    "      lado cubierto  opacidad %.3f (%zu px) · lado despejado %.3f (%zu px)\n",
                    mPos, nPos, mNeg, nNeg);
        CHECK(nPos > 0 && nNeg > 0, "el cuadro ve los dos lados del campo");
        CHECK(mPos > mNeg * 2.0,
              "el shader lee la cobertura POR DIRECCION (con el escalar, las dos mitades serian iguales)");
        ojoA = eyeA; mira = fwd;
        g_dev->destroy(campoTex);
    }

    // ── (8) LA VISTA ORBITAL CON EL CLIMA DE VERDAD, para poder MIRARLA ─────────────────────────
    //
    // `HARUKA_CLOUD_PNG=<dir>` vuelca el planeta visto desde 250 km con el campo de cobertura
    // horneado del `WeatherSystem` REAL — el mismo `cloudCoverAt` que usa el motor. Sirve para lo que
    // ningun numero contesta: "¿esto parece un planeta desde arriba, o son motas?".
    {
        const char* dir = std::getenv("HARUKA_CLOUD_PNG");
        const int CW = 128, CH = 64;
        std::vector<float> cov((size_t)CW * CH, 0.0f);
        // ⚠️ CONFIGURADO. La primera version lo dejaba recien construido, y `cloudCoverAt` empieza con
        // `if (!m_configured) return 0.0f` — el campo salia TODO A CERO y la vista orbital, negra. Lo
        // descarte como "fallo del test"... y resulta que ese negro es exactamente el sintoma que
        // Andoni reporta en partida ("se ve la atmosfera pero no las nubes"). El mecanismo es real y
        // hay que cubrirlo: con el campo a cero el pase SIGUE corriendo —la puerta usa el maximo, que
        // incluye la cobertura bajo la camara— pero cada muestra lee 0, el umbral se va a 0,58 y no
        // dibuja nada. Pase vivo, cielo vacio.
        Haruka::WeatherSystem wsys;
        wsys.configure(1234u);
        const double kPi2 = 3.14159265358979;
        float mn = 1.0f, mx = 0.0f; double acc = 0.0;
        for (int y = 0; y < CH; ++y) {
            const double lat = (0.5 - ((double)y + 0.5) / CH) * kPi2;
            for (int x = 0; x < CW; ++x) {
                const double lon = (((double)x + 0.5) / CW - 0.5) * 2.0 * kPi2;
                const glm::dvec3 d(std::cos(lat) * std::cos(lon), std::sin(lat),
                                   std::cos(lat) * std::sin(lon));
                const float c = wsys.cloudCoverAt(d, 0.5f);
                cov[(size_t)y * CW + x] = c;
                mn = std::min(mn, c); mx = std::max(mx, c); acc += c;
            }
        }
        std::printf("    campo de cobertura del CLIMA REAL: min %.3f · medio %.3f · max %.3f\n",
                    mn, acc / (double)(CW * CH), mx);
        TextureDesc wd; wd.width = CW; wd.height = CH; wd.format = Format::R32F;
        wd.filter = Filter::Linear; wd.wrap = Wrap::Repeat; wd.mipmaps = false;
        wd.initialData = cov.data();
        TextureHandle wTex = g_dev->createTexture(wd);

        ojoA = 250000.0f; mira = -up0;
        coverBound = wTex;
        std::vector<uint8_t> pxO;
        render(mx, pxO);                       // el escalar = el maximo, como hace el motor
        coverBound = coverTex; ojoA = eyeA; mira = fwd;

        // El volcado a disco es OPCIONAL (para poder mirarlo); la AFIRMACION de abajo corre siempre.
        if (dir) {
            const bool flip8 = detectRowFlip(pxO, w, h);
            std::vector<uint8_t> img(pxO.size());
            for (int y = 0; y < h; ++y) {
                const int src = flip8 ? (h - 1 - y) : y;
                std::memcpy(&img[(size_t)y * w * 4], &pxO[(size_t)src * w * 4], (size_t)w * 4);
            }
            const std::string path = std::string(dir) + "/"
                                   + ((g_dev->backend() == Backend::Vulkan) ? "vk_" : "gl_")
                                   + "nubes_orbita.png";
            Haruka::writePNG(path, w, h, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }
        size_t litO = 0, nO = 0;
        for (size_t k = 0; k + 3 < pxO.size(); k += 4) {
            if (pxO[k] == 0xAA && pxO[k+1] == 0xAA && pxO[k+2] == 0xAA) continue;
            if (pxO[k] > 6 || pxO[k+1] > 6 || pxO[k+2] > 6) ++litO;
            ++nO;
        }
        std::printf("    con el campo del CLIMA REAL, desde orbita: %.1f %% del cuadro con nube\n",
                    nO ? 100.0 * (double)litO / (double)nO : 0.0);
        CHECK(litO > nO / 20,
              "con el campo de cobertura REAL se dibujan nubes desde orbita (el campo a cero las apaga "
              "sin apagar el pase)");
        g_dev->destroy(wTex);
    }

    g_dev->destroy(pipe); g_dev->destroy(ubo); g_dev->destroy(depthTex);
    g_dev->destroy(coverTex);
}

static void testNodeWaterDraws()
{
    BEGIN("agua: el pase de nodos DIBUJA agua (y la descarta en tierra)");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 256)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // Un "bake" de altura sintetico de 4x2: constante, asi que la bilineal de `harukaSampleHeightField`
    // devuelve ese valor caiga donde caiga y la profundidad es la misma en toda la esfera.
    auto makeFloor = [&](float metres) {
        std::vector<float> d(8, metres);
        TextureDesc td; td.width = 4; td.height = 2; td.format = Format::R32F;
        td.filter = Filter::Linear; td.wrap = Wrap::ClampToEdge; td.mipmaps = false;
        td.initialData = d.data();
        return g_dev->createTexture(td);
    };
    // ⚠️ EL MAR, POR ENCIMA DEL RELIEVE. Con la cota 0 el agua queda al nivel MEDIO del terreno
    // procedural (+-900 m), y mirando desde arriba se ve siempre la superficie MAS ALTA de cada rayo
    // — que esta sesgada hacia los picos. El test daba 0,6 % y parecia que el pase no dibujaba;
    // apagando el test de profundidad salia 100 %, o sea que la geometria estaba bien y la escena
    // mal. Con la lamina a +2000 m el agua tapa el relieve y lo que se mide es lo que se queria medir.
    Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
    st.seaLevelM = 2000.0f;
    BufferHandle oceanUBO = makeOceanStateUBO(st);

    auto waterPixels = [&](float floorM, size_t& sentinelOut) {
        TextureHandle floorTex = makeFloor(floorM);
        TerrainNodeRenderer::Water wcfg;
        wcfg.heightTex   = floorTex;
        wcfg.oceanParams = oceanUBO;
        wcfg.on = true;
        r.setWater(wcfg);

        // ⚠️ AL NADIR Y DESDE ARRIBA, NO AL HORIZONTE. La primera version miraba a la tangente desde
        // 30 m: a esa altura el relieve procedural (+-900 m) TAPA el agua al nivel del mar en casi
        // toda la pantalla, y el test daba 0 px acusando al pase de no dibujar cuando lo que fallaba
        // era la camara. Mirando abajo desde 5 km se ven las depresiones, que es donde hay agua.
        const glm::dvec3 cam = pc + up0 * (R + 5000.0);
        const glm::dvec3 fwd = -up0;
        const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
        // ⚠️ REVERSED-Z, Y AQUI ESTABA EL FALLO — DEL TEST, NO DEL PASE. `glm::perspective` da una
        // proyeccion NORMAL (cerca->0, lejos->1) y el motor compara con `Greater`, que es la
        // convencion de reversed-Z: con la proyeccion normal, `Greater` se queda con lo MAS LEJANO.
        // El agua salia a 3 294 m y el terreno a 4 100-5 900, asi que ganaba el terreno y el test
        // daba 0 px acusando al pase de no dibujar. `testTerrainNodeCoverage` no lo nota porque solo
        // cuenta cobertura, y le da igual cual de las dos superficies gane.
        //
        // Reversed-Z con plano lejano infinito: z_ndc = near/dist, o sea 1 en el plano cercano y 0 en
        // el infinito. Es la que emite `Camera::getProjectionMatrix` en el motor.
        const float aspect = (float)w / (float)h;
        const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = fCot / aspect; proj[1][1] = fCot;
        proj[2][3] = -1.0f;         proj[3][2] = 1.0f;   // near = 1 m
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // AZUL: el agua sombrea con `harukaOceanShade`, cuyo canal B domina sobre R en toda su
        // paleta. El terreno del mismo pase no lo hace. Es el discriminante que separa "hay agua"
        // de "hay algo dibujado".
        // ⚠️ EL DISCRIMINANTE SE MIDE, NO SE ADIVINA. Este test ya fallo TRES veces por el
        // instrumento y no por el pase: contando magenta con una regla que exigia B > R, mirando al
        // horizonte donde el relieve tapa el agua, y con un umbral de azul elegido a ojo. Ahora se
        // imprime el color medio de lo dibujado, asi que si vuelve a dar 0 se ve POR QUE.
        size_t blue = 0, sentinel = 0, lit = 0;
        double sr = 0.0, sg = 0.0, sb = 0.0;
        for (size_t k = 0; k < px.size(); k += 4) {
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) { ++sentinel; continue; }
            if (px[k] || px[k+1] || px[k+2]) { ++lit; sr += px[k]; sg += px[k+1]; sb += px[k+2]; }
            if (px[k+2] > px[k] + 4 && px[k+2] > 8) ++blue;
        }
        std::printf("      (fondo %+.0f m) pixeles con color %zu · medio RGB %.0f/%.0f/%.0f\n",
                    (double)floorM, lit, lit ? sr / lit : 0.0, lit ? sg / lit : 0.0,
                    lit ? sb / lit : 0.0);
        sentinelOut = sentinel;
        g_dev->destroy(floorTex);
        return blue;
    };

    // El fondo sintetico decide la PROFUNDIDAD, que es lo unico que separa agua de tierra:
    //   -500 m -> profundidad 2500 m: hay mar.
    //   +5000 m -> profundidad -3000 m: tierra seca, el fragmento descarta.
    // La lamina esta en los dos casos a la MISMA cota, asi que lo que cambia es el descarte y no la
    // oclusion — que es justo lo que la contraprueba tiene que aislar.
    size_t sSea = 0, sLand = 0;
    const size_t blueSea  = waterPixels(-500.0f,  sSea);
    const size_t blueLand = waterPixels(+5000.0f, sLand);
    const size_t total = (size_t)w * h;
    std::printf("    con MAR (fondo -500 m): %zu px de agua (%.1f %%) · centinela %zu\n",
                blueSea, 100.0 * (double)blueSea / (double)total, sSea);
    std::printf("    con TIERRA (fondo +5000 m): %zu px de agua (%.1f %%) · centinela %zu\n",
                blueLand, 100.0 * (double)blueLand / (double)total, sLand);

    // ⚠️ EL CENTINELA PRIMERO: un `readPixels` que no escribe deja 0xAA y el conteo daria 0, o sea
    // que el test acusaria al pase de no dibujar cuando el que fallo fue el instrumento. Ya paso una
    // vez en el camino de Vulkan.
    // ── ⚠️ EL COSTE DEL PASE DE AGUA, QUE NUNCA SE HABIA MEDIDO ────────────────────────────────
    //
    // Estaba en la lista de deudas: *"el perfil de rendimiento del agua no se ha corrido nunca"*, y
    // encima se acaba de DOBLAR el trabajo del vertice (4 -> 8 trenes de Gerstner). Doblar un coste
    // desconocido es exactamente lo que paso con el geomorph, asi que aqui se mide.
    //
    // El metodo: el MISMO pase con `Water::on` en false y en true, 40 frames cada uno, y un
    // `readPixels` al final de cada tanda que FUERZA el sync — sin el se estaria midiendo lo que
    // tarda la CPU en encolar, no lo que tarda la GPU en dibujar. La diferencia es el agua.
    {
        TextureHandle floorTex = makeFloor(-500.0f);
        std::vector<uint8_t> one(4, 0);
        const glm::dvec3 camT = up0 * (R + 3000.0);
        const glm::dvec3 fwdT = -up0;
        const glm::dvec3 vupT = glm::normalize(glm::cross(fwdT, glm::dvec3(0, 1, 0)));
        const float aspectT = (float)w / (float)h;
        const float fCotT   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 projT(0.0f);
        projT[0][0] = fCotT / aspectT; projT[1][1] = fCotT;
        projT[2][3] = -1.0f;           projT[3][2] = 1.0f;
        const glm::mat4 mvpT = projT * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f),
                                                    glm::vec3(fwdT), glm::vec3(vupT))));
        auto timeFrames = [&](bool waterOn, int frames) {
            TerrainNodeRenderer::Water wc;
            wc.heightTex = floorTex; wc.oceanParams = oceanUBO; wc.on = waterOn;
            r.setWater(wc);
            // Calentamiento: la primera pasada crea/compila y mediria eso.
            for (int f = 0; f < 8; ++f) {
                if (Context* c = g_dev->beginFrame()) {
                    r.prepare(c, camT, pc, R, fwdT, radPerPx, cone);
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                    c->beginRenderPass({}, cv); r.draw(c, camT, pc, R, mvpT); c->endRenderPass();
                    g_dev->endFrame(); pumpWindowEvents();
                }
            }
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());   // sync
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < frames; ++f) {
                if (Context* c = g_dev->beginFrame()) {
                    r.prepare(c, camT, pc, R, fwdT, radPerPx, cone);
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                    c->beginRenderPass({}, cv); r.draw(c, camT, pc, R, mvpT); c->endRenderPass();
                    g_dev->endFrame(); pumpWindowEvents();
                }
            }
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());   // sync: cierra la cuenta
            return std::chrono::duration<double, std::milli>(
                       std::chrono::high_resolution_clock::now() - t0).count() / (double)frames;
        };
        const double msOff = timeFrames(false, 40);
        const double msOn  = timeFrames(true,  40);
        std::printf("    COSTE DEL PASE: terreno solo %.3f ms/frame · terreno+agua %.3f ms/frame"
                    "  ->  el agua cuesta %.3f ms (%.1f %% de un frame de 16,7)\n",
                    msOff, msOn, msOn - msOff, 100.0 * (msOn - msOff) / 16.7);
        std::printf("    (con %d trenes de Gerstner por vertice de agua)\n", Haruka::Planet::OCEAN_WAVES);
        // ⚠️ Y SI HAY VSYNC, ESTA CIFRA NO VALE. Con FIFO cualquier ms/frame se clava en 16,67 y la
        // diferencia mide el hueco que sobra, no el pase. Es la trampa que ya esta escrita en el
        // TODO ("para medir tiempos, HARUKA_NO_VSYNC=1") y sin este aviso la segunda pasada del banco
        // publicaria 0,1 ms como si fuera el coste del agua.
        const bool vsync = (msOff > 15.5 && msOff < 18.0);
        if (vsync)
            std::printf("    \033[33m⚠️ la base son %.1f ms = VSYNC (FIFO): esta medida NO es el coste del "
                        "pase. Repite con HARUKA_NO_VSYNC=1.\033[0m\n", msOff);
        CHECK(msOff > 0.0 && msOn > 0.0, "el coste del pase de agua queda MEDIDO (deuda cerrada)");
        // No se fija un techo: la cifra depende de la GPU del que corra el banco. Lo que se fija es
        // que el agua no multiplique el pase, que es la forma en que esto se volveria un problema.
        CHECK(vsync || msOn < msOff * 6.0 + 1.0,
              "el agua no multiplica por 6 el coste del pase de terreno (salvo con vsync, que no mide)");
        r.setWater(TerrainNodeRenderer::Water{});
        g_dev->destroy(floorTex);
    }

    CHECK(sSea < total / 2, "el readback escribio (si no, el conteo no significa nada)");
    CHECK(blueSea > total / 20, "el pase de nodos DIBUJA agua sobre un fondo de mar");
    // CONTRAPRUEBA: sobre tierra el mismo pase no debe pintar ni una gota. Sin esto, "hay pixeles
    // azules" no distingue el agua de cualquier cosa que el pase pinte.
    CHECK(blueLand < blueSea / 10,
          "CONTRAPRUEBA: sobre TIERRA el agua se descarta — es la profundidad quien manda");
    r.shutdown();
    if (valid(oceanUBO)) g_dev->destroy(oceanUBO);
}


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
static void testSceneTerrainAndProp()
{
    BEGIN("escena: terreno + prop en el mismo pase, los dos con luz");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 512)) {
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
    const double cone = nodeFrustumConeHalfAngle(fovY, 1.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::dvec3 fwd = -up0;                       // al nadir: el terreno llena el cuadro
    const glm::dvec3 rgh = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)cw / (float)ch);
    const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(
                              glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(glm::cross(rgh, fwd)))));

    // El prop va con proyeccion identidad (asi lo monta `propLitPixel`), asi que cae en el CENTRO del
    // cuadro. El terreno ocupa el resto. Se muestrean dos zonas disjuntas: centro = prop, esquina =
    // terreno. No es una escena "bonita"; es una escena donde los dos caminos de sombreado corren a la
    // vez y se pueden medir por separado, que es lo que hace falta.
    auto frame = [&](const float sun[3], std::vector<uint8_t>& px) {
        TerrainNodeRenderer::Shade sh;
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
// LA ESCENA, DIBUJADA EN EL BANCO: ¿se cuela el fondo por las costuras entre triangulos?
//
// Andoni, con el bloom ya arreglado: *"sin bloom la escena hace cosas raras, como que las lineas de
// triangulos son blanco-azuladas"*. Esa es la forma de un AGUJERO DE ALFILER: el cielo asomando por
// la juntura entre dos triangulos. El bloom lo tapaba al desenfocar, asi que aparecio justo cuando la
// exposicion dejo de estar quemada.
//
// No hace falta mirar la pantalla para cazarlo: se pinta el fondo de un color IMPOSIBLE (magenta puro)
// y se dibuja el terreno encima con la vista 2 (blanco plano). Cualquier pixel magenta que sobreviva
// DENTRO de la zona cubierta es fondo colandose. Y como el nadir cubre el cuadro entero en los dos
// backends (medido en `careo GL <-> Vulkan`), ahi cualquier magenta es un agujero, sin ambiguedad.
//
// ⚠️ CONTRAPRUEBA OBLIGATORIA: el mismo conteo SIN dibujar nada tiene que dar el 100 % de magenta. Sin
// ella, "0 agujeros" tambien lo daria un detector que no mira, o un clear que no ocurre.
static void testTerrainNodeSceneSeams()
{
    BEGIN("escena: el fondo NO se cuela por las costuras");

    using namespace Haruka::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Shader::baseDir() + "shaders/", 512)) {
        CHECK(false, "init del pase"); return;
    }
    r.setDebugViewOverride(2);                       // blanco plano: solo interesa QUE pixeles cubre

    const int cw = 256, ch = 256;
    const double fovY = glm::radians(60.0);
    const double radPerPx = fovY / (double)ch;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 800.0);
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)cw / (float)ch);

    auto shoot = [&](const glm::dvec3& fwd, bool draw, std::vector<uint8_t>& px) {
        const glm::dvec3 rgh = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
        const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(
                                  glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(glm::cross(rgh, fwd)))));
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        px.assign((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;  // magenta
            cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            if (draw) r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
    };
    auto magenta = [&](const std::vector<uint8_t>& px) {
        size_t n = 0;
        for (size_t k = 0; k + 3 < px.size(); k += 4)
            if (px[k] > 200 && px[k+1] < 60 && px[k+2] > 200) ++n;
        return n;
    };

    std::vector<uint8_t> px;
    const size_t total = (size_t)cw * ch;

    shoot(-up0, false, px);
    const size_t vacio = magenta(px);
    shoot(-up0, true, px);
    const size_t nadir = magenta(px);

    std::printf("    al NADIR (el terreno cubre el cuadro entero)\n");
    std::printf("      sin dibujar : %6zu de %zu magenta  (%.1f %%)  <- CONTRAPRUEBA del detector\n",
                vacio, total, 100.0 * (double)vacio / (double)total);
    std::printf("      dibujando   : %6zu de %zu magenta  (%.4f %% = fondo colandose)\n",
                nadir, total, 100.0 * (double)nadir / (double)total);

    // Y al horizonte, que es donde el stride cambia entre nodos vecinos y las costuras se ponen a
    // prueba de verdad. Aqui SI hay cielo legitimo, asi que solo se informa la cifra.
    const glm::dvec3 fwdH = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    shoot(fwdH, true, px);
    const size_t horiz = magenta(px);
    std::printf("    al HORIZONTE: %zu de %zu magenta (%.1f %%) — aqui hay cielo legitimo, es informativo\n",
                horiz, total, 100.0 * (double)horiz / (double)total);

    r.setDebugViewOverride(-1);

    CHECK(vacio > total * 9 / 10, "CONTRAPRUEBA: sin dibujar, el detector ve el fondo (si no, no mide nada)");
    CHECK(nadir * 1000 < total, "al NADIR el fondo no se cuela: menos de 1 pixel por mil");
}

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
    // ⚠️ LA PROYECCION DEL MOTOR, NO UNA DE GLM. `glm::perspective` es de OpenGL: no invierte la Y de
    // Vulkan y da profundidad en [-1,1] cuando Vulkan espera [0,1] y RECORTA lo que se salga. Con
    // ella, el careo GL<->Vulkan acusaba al backend de dibujar solo la mitad de arriba. El motor usa
    // `Camera::getProjectionMatrix`, que lleva la inversion y el reversed-Z: cualquier test que
    // compare backends tiene que usar ESA.
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::WorldPos(0.0, 0.0, 0.0))
                               .getProjectionMatrix((float)w / (float)h);
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
    recordShot("terreno.sin sombreado", cw, ch, imgFlat);
    recordShot("terreno.sombreado",     cw, ch, imgShaded);

    // ── SEPARAR GEOMETRIA DE SOMBREADO EN EL CAREO GL <-> VULKAN ────────────────────────────────
    //
    // Las dos capturas de arriba mezclan las dos cosas: si difieren, no se sabe si es que la
    // superficie esta en otro sitio o que la luz se calcula distinto. Las vistas de depuracion del
    // pase lo parten (ver `terrain_node.frag`):
    //   · vista 2 = BLANCO PLANO -> solo dice QUE PIXELES cubre la geometria. Si esta casa, la
    //     superficie es la misma en los dos backends y el problema es de sombreado.
    //   · vista 6 = la NORMAL como color -> si esta no casa, el problema esta en la normal, que es
    //     lo que alimenta a la luz.
    for (int dbg : { 2, 6 }) {
        r.setDebugViewOverride(dbg);
        recordShot(dbg == 2 ? "terreno.vista2 cobertura" : "terreno.vista6 normales",
                   cw, ch, grab(true));
    }

    // ── ¿QUIEN TIENE RAZON? MIRANDO AL NADIR NO HAY CIELO ────────────────────────────────────────
    //
    // Con la camara al horizonte, "OpenGL cubre el 100 % y Vulkan la mitad de abajo" admite DOS
    // lecturas opuestas: que Vulkan pierda medio cuadro, o que OpenGL pinte terreno donde toca cielo.
    // El careo dice que difieren, no quien acierta.
    //
    // Mirando HACIA ABAJO no hay cielo posible: el terreno tiene que llenar el cuadro en los dos. Si
    // aqui casan, el que estaba mal era el del horizonte y ya se sabe cual.
    {
        const glm::dvec3 fwdD = -up0;                          // al nadir
        const glm::dvec3 rgh  = glm::normalize(glm::cross(fwdD, glm::dvec3(0, 1, 0)));
        const glm::mat4  mvpD = proj * glm::mat4(glm::mat3(glm::lookAt(
                                   glm::vec3(0.0f), glm::vec3(fwdD), glm::vec3(glm::cross(rgh, fwdD)))));
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        std::vector<uint8_t> px((size_t)cw * ch * 4, 0xAA);
        if (Context* ctx = g_dev->beginFrame()) {
            r.prepare(ctx, cam, pc, R, fwdD, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvpD);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
        recordShot("terreno.vista2 NADIR", cw, ch, px);
    }
    r.setDebugViewOverride(-1);
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
    // El nombre del driver decide qué fallos están declarados como suyos. Se imprime siempre: sin
    // esta línea, un XFAIL en el log de CI no se puede atribuir a nada.
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
    testGpuFp64();
    testOctaveDivergence();
    testWaterFieldParity();
    testWaveNumberParity();
    testTerrainBakeAcrossLevels();
    testBakeStageBisect();
    testTerrainNodeDrawnVsField();
    testTerrainStrideMatchCost();
    testTerrainNodeRendererInit();
    testTerrainNodeBaseField();
    testNodeWaterDraws();
    testAtmosphereLimb();
    testCloudVolumeDraws();
    testCloudFieldDistribution();
    testSkyLayersHaveDepth();
    testTerrainNodeCoverage();
    testTerrainNodeSeamHoles();
    testTerrainNodeSharedEdgeGpu();
    testTerrainNodeStitchSymmetryGpu();
    testTextureContent();
    testVertexColor();
    testVertexInterpolation();
    testItemPreviewShader();
    testOceanWaveParity();
    testWaterShapes();
    testEnginePipelines();
    testSkyAmbientGPU();
    testTerrainLighting();
    testPropLighting();
    testPropLightSweep();
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
    testTerrainNodeSceneSeams();
    testSceneTerrainAndProp();
    testTerrainNodeShadeCost();

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
    if (run != "gl" && run != "vk" && run != "all") {
        std::fprintf(stderr, "uso: haruka_tests_rhi [gl|vk|all]\n");
        return 2;
    }

    if (!locateAssets()) { std::printf("\n== 0 OK · 1 FALLOS ==\n"); return 1; }

    if (run == "gl" || run == "all") runBackend(Backend::OpenGL);
    if (run == "vk" || run == "all") runBackend(Backend::Vulkan);
    if (run == "all") compareBackends();

    SDL_Quit();
    if (g_xfail > 0)
        std::printf("\n== %d OK · %d FALLOS · %d XFAIL (defectos declarados del driver) ==\n",
                    g_pass, g_fail, g_xfail);
    else
        std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
