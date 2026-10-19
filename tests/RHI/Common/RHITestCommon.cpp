#include "RHI/Common/RHITestCommon.h"
#include <cstdio>
#include <cstdlib>

int g_xfail = 0;
int g_fail = 0;
int g_pass = 0;
const char * g_cur = "";
DriverDefect g_driverDefects[16] = {
    // AMD sobre OpenGL: los siete de la sonda de aritmética...
    { Backend::OpenGL, "AMD", "DOUBLE, o la degrada a float", "(3) `normalize(dvec3)*R`",
      HARUKA_AMD_GL_FP64, 0 },
    { Backend::OpenGL, "AMD", "DOUBLE, o la degrada a float", "(6) `inversesqrt(double)`",
      HARUKA_AMD_GL_FP64, 0 },
    // ⚠️ (7) y (8) YA NO ESTAN, Y NO PORQUE EL DRIVER MEJORARA: `harukaCubeFaceToDir` dejo de usar
    // `sqrt(double)` del driver y hace la raiz con `harukaSqrtD` (ver cube_face.glsl, 16-09).
    //
    // ⚠️ Y (10), (11), (12) Y LOS DOS DE LA BISECCION TAMPOCO (16-10): NUNCA FUERON DEL DRIVER. La
    // sonda media el residuo con `a - double(float(a))`, y el compilador (NIR) pliega esa ida y
    // vuelta: el residuo salia CERO con un producto que era EXACTO (los 16 bits bajos de
    // `6371000*0.31f` dan 30720 en la GPU y en la CPU, en GL y en Vulkan). Con el redondeo a float
    // hecho sobre los bits (`lib/fp64.glsl`) los cinco casan con la CPU en AMD+OpenGL, y la regla de
    // "entrada muerta = fallo" lo dijo. En Vulkan el mismo artefacto daba seis FALLOS en rojo.
    // Lo que SI degrada AMD+OpenGL es la raiz en doble: (3) pasa por `normalize` y (6) es la raiz.
    // ...y su consecuencia en el SUELO DIBUJADO: la misma raiz en doble en el vertice. Vulkan, mismo
    // hardware y mismo frame: 0,19 cm de media a menos de 150 m; OpenGL: 4,2 cm (16-10).
    { Backend::OpenGL, "AMD", "un frame entero contra el relieve horneado", "a menos de 150 m",
      HARUKA_AMD_GL_FP64, 0 },
    { Backend::OpenGL, "AMD", "un frame entero contra el relieve horneado", "de 300 a 1000 m",
      HARUKA_AMD_GL_FP64, 0 },
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
Backend g_curBackend = Backend::OpenGL;
std::string g_curDevice;

const char* knownDefect(const char* test, const char* msg)
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
std::map<std::string, double> g_baseline;
std::map<std::string, double> g_baselineNew;
bool g_baselineOk = false;
bool g_baselineLoaded = false;
std::string g_baselineDir;

std::string baselineSlug(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_');
    while (o.size() > 40) o.pop_back();
    return o;
}

std::string baselinePath(bool perGpu) {
    // Las de imagen tambien van por backend: GL y Vulkan no dan la misma imagen (medido: 53,7 % de
    // cirro al cenit en GL contra 67,3 % en Vulkan con el mismo shader), y eso es un hallazgo, no ruido.
    const std::string be = (g_curBackend == Backend::Vulkan) ? "vk" : "gl";
    return g_baselineDir + "/" + (perGpu ? (be + "_" + baselineSlug(g_curDevice)) : be) + ".txt";
}

void baselineLoadFile(bool perGpu) {
    std::ifstream f(baselinePath(perGpu));
    std::string k; double v;
    while (f >> k >> v) g_baseline[(perGpu ? "gpu:" : "any:") + k] = v;
}

void baselineInit() {
    if (g_baselineLoaded) return;
    g_baselineLoaded = true;
    const char* e = std::getenv("HARUKA_BASELINE_DIR");
    g_baselineDir = e ? e : "tests/baseline";
    struct stat st{};
    g_baselineOk = (stat(g_baselineDir.c_str(), &st) == 0);
    if (!g_baselineOk) { std::printf("  (sin directorio de linea base `%s`: las metricas solo se imprimen)\n", g_baselineDir.c_str()); return; }
    baselineLoadFile(false); baselineLoadFile(true);
}

void baselineMetric(const char* key, double value, double tolRel, bool upperOnly, bool perGpu, bool valid)
{
    baselineInit();
    if (!g_baselineOk || !valid) return;
    const std::string full = (perGpu ? "gpu:" : "any:") + std::string(key);
    g_baselineNew[full] = value;
    auto it = g_baseline.find(full);
    if (it == g_baseline.end()) { std::printf("    [linea base] %s = %.4g (NUEVA: se anota; aprobar con HARUKA_BASELINE_UPDATE=1)\n", key, value); return; }
    const double base = it->second;
    // Suelo absoluto: una linea base en 0 (grano 0 %, presencia 0 %) con tolerancia relativa es
    // "exactamente 0 o falla", y 0,009 % de grano no es una regresion. Las de imagen son porcentajes
    // y diferencias /255: medio punto; las de coste, 0,05 ms.
    const double tol = std::max(std::fabs(base) * tolRel, perGpu ? 0.05 : 0.5);
    const bool ok = upperOnly ? (value <= base + tol) : (std::fabs(value - base) <= tol);
    char msg[256];
    std::snprintf(msg, sizeof msg, "linea base %s: %.4g contra %.4g aprobado (tol %.0f %%%s)", key, value, base, tolRel * 100.0, upperOnly ? ", solo subir" : "");
    CHECK(ok, msg);
}
Device * g_dev = nullptr;
std::map<std::string, Shot> g_shots[2];

void recordShot(const char* name, int w, int h, const std::vector<uint8_t>& px)
{
    const int b = (g_dev && g_dev->backend() == Backend::Vulkan) ? 1 : 0;
    g_shots[b][name] = Shot{ w, h, px };
    // `HARUKA_SHOT_DIR=<carpeta>`: además se vuelca a disco (PPM, filas tal cual las leyó el backend)
    // para MIRAR la imagen, que el careo en ASCII no deja ver.
    if (const char* dir = std::getenv("HARUKA_SHOT_DIR")) {
        const std::string path = std::string(dir) + "/" + (b ? "vk_" : "gl_") + baselineSlug(name) + ".ppm";
        if (FILE* fp = std::fopen(path.c_str(), "wb")) {
            std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; ++i) std::fwrite(&px[(size_t)i * 4], 1, 3, fp);
            std::fclose(fp);
        }
    }
}

void pumpWindowEvents()
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) { /* se descartan: el banco no interactúa, solo debe responder */ }
}

void copyThenWait(Haruka::RHI::BufferHandle src, Haruka::RHI::BufferHandle dst,
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
