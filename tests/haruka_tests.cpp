/**
 * @file haruka_tests.cpp
 * @brief Banco de pruebas del MOTOR REAL a pequeña escala y headless (ventana GL OCULTA).
 *
 * CLAVE (petición del usuario): NO reimplementa nada — instancia el `PlanetarySystem` REAL y dirige
 * su `update()` REAL (LOD + streaming + generación como en el juego), leyendo su ESTADO real
 * (`getGPUChunkCount`, `validateLOD`). Así lo que se prueba es el motor, no una copia. Sin ventana
 * visible ni bucle de juego: solo un contexto GL oculto (el renderer sube a GPU) + entrada sintética
 * (posiciones de cámara) + verificación de invariantes por consola.
 *
 * Ejecuta:  ./haruka_tests            (todos)
 *           ./haruka_tests lod        (filtro por subcadena)
 * Devuelve 0 si TODO pasa, !=0 si algo falla.
 *
 * Los tests de paridad CPU↔compute-GPU se añadirán aquí mismo (ya hay contexto GL) cargando el
 * compute real; requieren que los assets/shaders sean localizables (asset paths).
 */
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "game/planetary_system.h"
#include "renderer/terrain_renderer.h"   // PoolStats / poolVramCeil / kMaxResidentChunks (tests de pool)
#include "core/terrain/terrain_sampler_v2.h"
#include "core/terrain/gpu_heightfield.h"
#include "renderer/shader.h"

#include <algorithm>
#include <vector>

using namespace Haruka;

// UBOs del pase de terreno (tamaños std140 reales; contenido irrelevante para la lógica CPU de
// eviction/draw-set — solo hacen falta enlazados para que el shader real tenga sus bloques).
struct TerrainDrawGL {
    std::unique_ptr<Shader> shader;
    GLuint uboFrame = 0, uboObj = 0;
    bool ok = false;
    void init() {
        try { shader = std::make_unique<Shader>("shaders/planet.vert", "shaders/planet.frag"); }
        catch (...) { return; }
        if (!shader || shader->ID == 0) return;
        glGenBuffers(1, &uboFrame); glBindBuffer(GL_UNIFORM_BUFFER, uboFrame);
        glBufferData(GL_UNIFORM_BUFFER, 240, nullptr, GL_DYNAMIC_DRAW);
        glGenBuffers(1, &uboObj);   glBindBuffer(GL_UNIFORM_BUFFER, uboObj);
        glBufferData(GL_UNIFORM_BUFFER, 96, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        ok = true;
    }
    void bind() {
        if (!ok) return;
        shader->use();
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, uboFrame);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, uboObj);
    }
};

// Shader del AGUA (single ocean) para cronometrar renderPlanetWater. Best-effort: sin los uniforms de
// costa/oleaje (los pone application_render en el juego) el agua sale degenerada, pero el COSTE CPU
// (construir la rejilla + draw) se mide igual. Reusa el UBO de frame (binding 0) del terreno.
struct WaterDrawGL {
    std::unique_ptr<Shader> shader; bool ok = false;
    void init() {
        try { shader = std::make_unique<Shader>("shaders/water.vert", "shaders/water.frag"); } catch (...) { return; }
        if (shader && shader->ID) ok = true;
    }
    void bind(GLuint uboFrame) { if (ok) { shader->use(); glBindBufferBase(GL_UNIFORM_BUFFER, 0, uboFrame); } }
};

// ------------------------------------------------------------------ mini-framework
static int g_pass = 0, g_fail = 0;
static std::string g_curTest;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  \033[31mFAIL\033[0m [%s] %s  (%s:%d)\n", g_curTest.c_str(), msg, __FILE__, __LINE__); } \
} while (0)
static void beginTest(const char* name) { g_curTest = name; std::printf("• %s\n", name); }

// ------------------------------------------------------------------ contexto GL oculto (real)
struct GLContext {
    SDL_Window* win = nullptr; SDL_GLContext ctx = nullptr; bool ok = false;
    GLContext() {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
            std::printf("  (SDL_Init VIDEO falló: %s)\n", SDL_GetError()); return;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
        win = SDL_CreateWindow("haruka_tests", 256, 256, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!win) { std::printf("  (CreateWindow oculto falló: %s)\n", SDL_GetError()); return; }
        ctx = SDL_GL_CreateContext(win);
        if (!ctx) { std::printf("  (GL_CreateContext falló: %s)\n", SDL_GetError()); return; }
        SDL_GL_MakeCurrent(win, ctx);
        if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) { std::printf("  (gladLoad falló)\n"); return; }
        ok = true;
    }
    ~GLContext() { if (ctx) SDL_GL_DestroyContext(ctx); if (win) SDL_DestroyWindow(win); }
};

// Un planeta terran v2 de prueba (radio pequeño → pocos niveles, test rápido). gpu=true usa el
// CAMINO REAL del juego (compute GPU, terrain_gen.comp desde build/shaders). gpu=false usa el
// fallback CPU async — que HOY corrompe el heap bajo churn (hallazgo del harness; ver notas).
static PlanetarySystem::Planet makeTestPlanet(double radius, uint32_t seed, bool gpu,
                                              int chunkSize = 24, const char* name = "TestPlanet",
                                              bool isHome = true) {
    PlanetarySystem::Planet p;
    p.name = name;
    p.position = WorldPos(0.0);
    p.radius = radius;
    p.isHome = isHome;
    nlohmann::json cfg;
    cfg["genVersion"] = 2; cfg["gpuTerrain"] = gpu; cfg["profile"] = "terran";
    cfg["seed"] = seed; cfg["chunkSize"] = chunkSize; cfg["reliefStrength"] = 1.0;
    p.terrainSettings = nlohmann::json::object();
    p.terrainSettings["config"] = cfg;
    p.terrainSettings["radius"] = radius;
    return p;
}

// Dirige el motor N frames REPLICANDO el frame del juego: cull-matrix + screenK (LOD refina SOLO la
// vista) + update() + renderPlanetTerrain() REALES → ejercita también eviction/keep-radius/draw-set
// (donde vive el bug "el terreno desaparece al girar"). `lookDir` = hacia dónde MIRA la cámara (para
// simular GIRAR EN SITIO sin mover la posición). `gl` != null → dibuja de verdad. Deja respirar los
// workers de generación async.
static void driveEngine(PlanetarySystem& ps, const glm::dvec3& cam, const glm::dvec3& lookDir,
                        int frames, TerrainDrawGL* gl = nullptr, double dt = 1.0/60.0) {
    const float fovY = glm::radians(70.0f);
    glm::mat4 proj = glm::perspective(fovY, 1.0f, 0.1f, 1e9f);
    glm::vec3 fwd = glm::normalize(glm::vec3(lookDir));
    glm::vec3 up  = (std::abs(fwd.y) < 0.95f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f), fwd, up);
    glm::mat4 camRelVP = proj * glm::mat4(glm::mat3(view)); // solo rotación (igual que application_render)
    const double screenK = 256.0 * 0.5 * (double)proj[1][1];
    for (int f = 0; f < frames; ++f) {
        ps.setTerrainCullMatrix(camRelVP);
        ps.setLODScreenK(screenK);
        ps.update(dt, cam);
        if (gl && gl->ok) { gl->bind(); ps.renderPlanetTerrain("TestPlanet", cam); }
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); // deja terminar la generación async
    }
}

// Dirige hasta que el streaming CONVERGE DE VERDAD: el conteo de chunks NO cambia Y no hay nada en
// vuelo (`getPendingChunks()==0`) durante `stableN` frames seguidos. Sin la condición pending==0, se
// paraba en MESETAS transitorias entre niveles de la subida progresiva de LOD → estado no-convergido
// (varianza run-a-run). Con ella, se salta esas mesetas cortas y alcanza el punto fijo determinista.
static bool settle(PlanetarySystem& ps, const glm::dvec3& cam, const glm::dvec3& look,
                   TerrainDrawGL* gl, int stableN = 45, int maxFrames = 3000) {
    int last = -1, stable = 0;
    for (int f = 0; f < maxFrames; ++f) {
        driveEngine(ps, cam, look, 1, gl);
        int c = ps.getGPUChunkCount();
        if (c == last && ps.getPendingChunks() == 0) { if (++stable >= stableN) return true; }
        else { stable = 0; last = c; }
    }
    return false;
}

// ------------------------------------------------------------------ TEST: el motor carga terreno (real)
static void test_engine_loads_terrain(TerrainDrawGL* gl) {
    beginTest("engine_loads_terrain");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(false); // per-chunk (usa planet.vert, sin el SSBO del MultiDraw pooled)
    ps.addPlanet(makeTestPlanet(/*radius*/5000.0, /*seed*/1234, /*gpu*/true));

    const glm::dvec3 cam(0.0, 0.0, 5000.0 + 80.0);         // 80 m sobre la superficie del polo
    const glm::dvec3 look(0, 0, -1);                        // mira hacia el planeta
    driveEngine(ps, cam, look, 100, gl);
    bool settled = settle(ps, cam, look, gl);               // espera a que el streaming se asiente
    CHECK(settled, "el streaming se ASIENTA (conteo estable) en tiempo finito");

    int gpuChunks = ps.getGPUChunkCount();
    CHECK(gpuChunks > 0, "el motor GENERÓ y SUBIÓ chunks a GPU (streaming real funcionando)");
    std::printf("    gpuChunks=%d\n", gpuChunks);
    std::string rep = ps.validateLOD();
    std::printf("    %s\n", rep.substr(0, rep.find('\n')).c_str());
    // Invariantes REALES del screen-space LOD: SIN HUECOS y SIN SOLAPES (doble malla). El "balance"
    // 2:1 NO aplica al LOD screen-space (vecinos a distinta distancia proyectan distinto → niveles
    // distintos, y el morph CDLOD lo disimula) → informativo, no un fallo.
    CHECK(rep.find("holes=0") != std::string::npos,    "sin HUECOS de cobertura");
    CHECK(rep.find("overlaps=0") != std::string::npos, "sin SOLAPES (doble malla)");
}

// ------------------------------------------------------------------ TEST: girar en sitio no pierde terreno (real)
static void test_engine_rotate_keeps_terrain(TerrainDrawGL* gl) {
    beginTest("engine_rotate_keeps_terrain");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(false);
    ps.addPlanet(makeTestPlanet(5000.0, 4242, true));

    const glm::dvec3 cam(0.0, 0.0, 5000.0 + 80.0);
    const glm::dvec3 lookA(0.2, 0.2, -1.0);   // mirando hacia una zona del planeta
    const glm::dvec3 lookB(-1.0, -0.2, -0.2); // GIRAR EN SITIO a otra dirección (misma posición)

    // 1) Carga mirando A (hasta asentar).
    driveEngine(ps, cam, lookA, 80, gl);
    settle(ps, cam, lookA, gl);
    int atA = ps.getGPUChunkCount();
    CHECK(atA > 0, "cargó terreno mirando A");

    // 2) GIRA a B y espera BASTANTE (más que la ventana de eviction sin keep-radius) → el terreno de A
    //    (cerca, detrás de la cámara) NO debe evictarse: keep-radius lo mantiene por DISTANCIA.
    driveEngine(ps, cam, lookB, 200, gl);

    // 3) Vuelve a mirar A y deja RE-ASENTAR. Debe quedar válido y con el terreno de A residente
    //    (keep-radius) — no regenerándose desde cero.
    settle(ps, cam, lookA, gl);
    int backA = ps.getGPUChunkCount();
    std::string rep = ps.validateLOD();
    std::printf("    chunks: A=%d  tras girar+volver=%d\n", atA, backA);
    std::printf("    %s\n", rep.substr(0, rep.find('\n')).c_str());
    CHECK(backA >= atA * 3 / 5, "girar en sitio y volver NO perdió el terreno (keep-radius)");
    CHECK(rep.find("holes=0") != std::string::npos,    "sin huecos tras girar en sitio");
    CHECK(rep.find("overlaps=0") != std::string::npos, "sin solapes tras girar en sitio");
}

// ------------------------------------------------------------------ BENCH: buildDrawSet (cuello CPU #1)
// Mide el coste de buildDrawSet en sus DOS regímenes: (1) todo residente → CAMINO RÁPIDO (~0.1ms),
// (2) a media transmisión tras acercar la cámara (faltan hojas finas) → CAMINO LENTO (canCover/emit
// top-down). El camino lento es el que domina el frame al MOVERTE. Correr: ./haruka_tests drawset
static void test_drawset_bench(TerrainDrawGL* gl) {
    beginTest("drawset_bench");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(false);
    // Planeta GRANDE (radio 1500 km) → LOD profundo (cadenas de ancestros largas), que es donde el
    // camino lento (internal-set + recursión canCover/emit) se dispara. El de 5 km apenas teselaba.
    const double R = 1.5e6;
    ps.addPlanet(makeTestPlanet(R, 1234, true));

    const glm::dvec3 look(0, 0, -1);
    // Barremos varias alturas MIENTRAS transmitimos (streaming por detrás → camino lento) y en cada
    // parada: (1) verificamos que el optimizado == referencia (MISMO conjunto), (2) cronometramos.
    const double alts[] = {300.0, 120.0, 60.0, 500.0, 90.0};
    bool allMatch = true;
    double bestOpt = 1e18, bestRef = 1e18; // el mediano más representativo (el de más carga)
    int reprDrawn = 0;
    for (double alt : alts) {
        const glm::dvec3 cam(0.0, 0.0, R + alt);
        for (int i = 0; i < 120; ++i) driveEngine(ps, cam, look, 1, gl); // acumula hojas, no settle
        double nsOpt = 0, nsRef = 0; int drawn = 0;
        bool ok = ps.checkDrawSetMatchesReference(200, &nsOpt, &nsRef, &drawn);
        std::printf("    alt=%5.0fm  drawn=%-4d  opt=%6.1f us  ref=%6.1f us  %s\n",
                    alt, drawn, nsOpt / 1000.0, nsRef / 1000.0, ok ? "OK==" : "¡DIVERGE!");
        allMatch = allMatch && ok;
        if (drawn > reprDrawn) { reprDrawn = drawn; bestOpt = nsOpt; bestRef = nsRef; }
    }
    CHECK(allMatch, "buildDrawSet OPTIMIZADO produce el MISMO conjunto que la referencia");
    if (bestRef < 1e17)
        std::printf("    → speedup en el caso de más carga (drawn=%d): %.2fx (%.1f→%.1f us)\n",
                    reprDrawn, bestRef / bestOpt, bestRef / 1000.0, bestOpt / 1000.0);
    // DELTA-SPLICE: coste de re-emitir 1 subárbol vs reconstruir todo el draw-set (el caso de
    // moverte+streaming, donde antes se reconstruía completo CADA frame con residencia cambiando).
    double nsFull = 0, nsSplice = 0; size_t drawnN = 0;
    ps.benchDrawSetSplice(200, &nsFull, &nsSplice, &drawnN);
    if (nsSplice > 0)
        std::printf("    → splice (1 nodo) vs rebuild completo [drawn=%zu]: %.1f us vs %.1f us = %.1fx más barato\n",
                    drawnN, nsSplice / 1000.0, nsFull / 1000.0, nsFull / nsSplice);
}

// ------------------------------------------------------------------ TEST: gating del cache del draw-set
// Valida que condicionar m_residentVersion a "chunk en la clausura" NO deja el draw-set CACHEADO
// rancio: tras mover+asentar en varias posiciones, el conjunto cacheado (el que se dibuja) debe
// coincidir con un rebuild fresco. Si el gate se saltara un bump necesario → mismatch aquí (y huecos
// en el juego que validateCoverage no vería por recomputar siempre fresco). Correr: ./haruka_tests gating
static void test_drawset_cache_gating(TerrainDrawGL* gl) {
    beginTest("drawset_cache_gating");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(false);
    ps.addPlanet(makeTestPlanet(5000.0, 777, true));

    struct Stop { glm::dvec3 cam, look; };
    const Stop stops[] = {
        {{0, 0, 5000.0 + 400.0}, {0, 0, -1}},          // acercándose
        {{0, 0, 5000.0 + 90.0},  {0, 0, -1}},          // muy cerca → refina (streaming IN)
        {{300, 0, 5000.0 + 90.0},{-0.3, 0, -1}},       // desplazamiento lateral → evict + load
        {{0, 0, 5000.0 + 800.0}, {0, 0, -1}},          // alejándose → merge (streaming coarsen)
    };
    bool allFresh = true, allValid = true;
    for (const auto& s : stops) {
        driveEngine(ps, s.cam, s.look, 40, gl);
        settle(ps, s.cam, s.look, gl);                  // punto quiescente (sin async en vuelo)
        driveEngine(ps, s.cam, s.look, 1, gl);          // un render más → cache al día con la residencia
        const bool fresh = ps.checkCachedDrawSetFresh();
        std::string rep = ps.validateLOD();
        const bool valid = rep.find("holes=0") != std::string::npos
                        && rep.find("overlaps=0") != std::string::npos;
        std::printf("    cam.z=%.0f  cache %s  cobertura %s\n", s.cam.z,
                    fresh ? "FRESCO" : "¡RANCIO!", valid ? "ok" : "¡MAL!");
        allFresh = allFresh && fresh;
        allValid = allValid && valid;
    }
    CHECK(allFresh, "el draw-set CACHEADO (gateado) coincide con el fresco tras mover+asentar");
    CHECK(allValid, "cobertura sin huecos/solapes en todas las paradas");
    uint64_t changes = 0, bumps = 0;
    ps.residencyGatingStats(changes, bumps);
    if (changes > 0)
        std::printf("    gating: %llu/%llu cambios de residencia invalidaron el draw-set (%.0f%% evitados)\n",
                    (unsigned long long)bumps, (unsigned long long)changes,
                    100.0 * (double)(changes - bumps) / (double)changes);
}

// ------------------------------------------------------------------ TEST: delta-splice del draw-set
// Valida el DELTA-SPLICE (re-emitir solo los subárboles tocados en vez de reconstruir todo) en el
// caso CALIENTE: cámara en MOVIMIENTO continuo + streaming (residencia cambia casi cada frame). Tras
// CADA frame comprueba que el draw-set CACHEADO (actualizado por splice) == un buildDrawSet FRESCO y
// que la cobertura no tiene huecos/solapes. Como addToScene se drena en update() (no en workers), no
// hay carrera entre render y el chequeo. Si el splice divergiera aunque fuese 1 frame → lo pilla aquí.
// También exige que el splice SÍ se aplicara (no que cayera siempre al rebuild). Correr: delta
static void test_drawset_delta_streaming(TerrainDrawGL* gl) {
    beginTest("drawset_delta_streaming");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(false);
    const double R = 1.5e6;                          // planeta grande → LOD profundo (mucho churn)
    ps.addPlanet(makeTestPlanet(R, 31337, true));

    // Trayectoria continua: descenso + deriva lateral (refina+evicta = adds y removes en el draw-set).
    int badFresh = 0, badCover = 0, frames = 0;
    auto step = [&](const glm::dvec3& cam, const glm::dvec3& look) {
        driveEngine(ps, cam, look, 1, gl);           // update()+render() reales; cache al día con residencia
        ++frames;
        if (!ps.checkCachedDrawSetFresh()) ++badFresh;   // cache (splice) == fresco?
        std::string rep = ps.validateLOD();
        if (rep.find("holes=0") == std::string::npos ||
            rep.find("overlaps=0") == std::string::npos) ++badCover;
    };
    // Descenso desde 4 km a 80 m sobre un punto de la cara, mirando abajo-adelante.
    for (int i = 0; i < 240; ++i) {
        double t = i / 239.0;
        double alt = 4000.0 * (1.0 - t) + 80.0 * t;
        double x = 900.0 * t;                        // deriva lateral → evicta lo que dejas atrás
        step({x, 0.0, R + alt}, glm::normalize(glm::dvec3(-0.25, 0.0, -1.0)));
    }
    // Deriva lateral sostenida cerca de superficie (streaming IN por delante, OUT por detrás).
    for (int i = 0; i < 160; ++i) {
        double x = 900.0 + 12.0 * i;
        step({x, 0.0, R + 80.0}, glm::normalize(glm::dvec3(-0.25, 0.0, -1.0)));
    }

    uint64_t applied = 0, full = 0;
    ps.drawSetDeltaStats(applied, full);
    // Asentar al final → converge a cobertura correcta (los blips-MAL de arriba son huecos
    // TRANSITORIOS de streaming del propio algoritmo de referencia, NO del splice: se dan igual con
    // el delta apagado, y `fresco-MAL` demuestra que el cache por splice == el fresco cada frame).
    const glm::dvec3 endCam(900.0 + 12.0 * 159, 0.0, R + 80.0);
    const glm::dvec3 endLook = glm::normalize(glm::dvec3(-0.25, 0.0, -1.0));
    settle(ps, endCam, endLook, gl);
    driveEngine(ps, endCam, endLook, 1, gl);
    std::string finalRep = ps.validateLOD();
    const bool finalClean = finalRep.find("holes=0") != std::string::npos
                         && finalRep.find("overlaps=0") != std::string::npos;
    std::printf("    frames=%d  fresco-MAL=%d  cobertura-MAL(mov,transitorio)=%d  splice=%llu  rebuild=%llu  final=%s\n",
                frames, badFresh, badCover,
                (unsigned long long)applied, (unsigned long long)full, finalClean ? "limpio" : "¡MAL!");
    CHECK(badFresh == 0,   "el draw-set por SPLICE == fresco en TODOS los frames en movimiento (delta correcto)");
    CHECK(ps.checkCachedDrawSetFresh(), "cache por splice == fresco tras asentar");
    CHECK(finalClean,      "cobertura sin huecos/solapes tras asentar");
    CHECK(applied > 0,     "el delta-splice SÍ se aplicó (no cayó siempre al rebuild completo)");
}

// ------------------------------------------------------------------ TEST: sampler determinista (real)
static void test_sampler_determinism() {
    beginTest("sampler_determinism");
    const double R = 6.371e6;
    WorldGenParams W = deriveWorldParams(1234u, R);
    W.profile = 0;
    glm::vec3 d = glm::normalize(glm::vec3(0.3f, 0.7f, 0.5f));
    float e1 = sampleTerrainV2(d, W, R).elevKm;
    float e2 = sampleTerrainV2(d, W, R).elevKm;
    CHECK(e1 == e2, "sampleTerrainV2 determinista");
    int land = 0, sea = 0; uint32_t s = 777u;
    auto rnd = [&]{ s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < 512; ++i) {
        glm::vec3 dir = glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1));
        float e = sampleTerrainV2(dir, W, R).elevKm;
        if (e > 0.0f) ++land; else ++sea;
    }
    CHECK(land > 20 && sea > 20, "el mundo tiene mar Y tierra (no degenerado)");
    std::printf("    muestreo: %d tierra / %d mar de 512\n", land, sea);
}

// ------------------------------------------------------------------ TEST: paridad CPU ↔ compute-GPU
// Muestrea N direcciones y compara la elevación del compute GPU (terrain_gen.comp) vs sampleTerrainV2
// (CPU). Es la RED DE SEGURIDAD de la paridad: si divergen, el terreno visible (GPU) no casa con la
// colisión (CPU) → props flotan. Herramienta para verificar cambios paridad-críticos (p.ej. F4).
// La MEDIANA debe ser ~0 (Perlin GLSL == Perlin CPU); unos pocos outliers grandes = lagos (el
// carvado difiere por celda) → toleramos un % pequeño de puntos con error alto.
static void test_cpu_gpu_parity() {
    beginTest("cpu_gpu_parity");
    const double R = 6.371e6;
    const uint32_t seed = 1234u;
    WorldGenParams W = deriveWorldParams(seed, R);
    W.profile = 0; W.reliefStrength = 1.0f;

    // Direcciones deterministas (mismo PRNG que el test de paridad interno del motor).
    const int N = 4096;
    std::vector<glm::vec3> dirs; dirs.reserve(N);
    uint32_t s = 12345u;
    auto rnd = [&]{ s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < N; ++i)
        dirs.push_back(glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1)));

    // GPU (compute real).
    GpuHeightfield gpu;
    GpuHeightfield::Params gp;
    gp.seed = (int)seed; gp.continentFreqA = W.continentFreqA; gp.reliefStrength = W.reliefStrength;
    gp.seaThreshold = W.seaThreshold; gp.voronoiDensity = W.voronoiDensity;
    gp.lakeDensity = W.lakeDensity; gp.lakeMaxProb = W.lakeMaxProb; gp.coastWidth = W.coastWidth;
    gp.radius = R;
    std::vector<float> ge, gw; std::vector<glm::vec3> gn;
    bool okGpu = gpu.generate(dirs, gp, ge, gn, gw) && ge.size() == dirs.size();
    CHECK(okGpu, "el compute GPU generó (terrain_gen.comp cargado y ejecutado)");
    if (!okGpu) return;

    // CPU + diffs (km).
    std::vector<double> err; err.reserve(N);
    for (int i = 0; i < N; ++i) {
        float cpu = sampleTerrainV2(dirs[i], W, R).elevKm;
        err.push_back(std::abs((double)ge[i] - (double)cpu));
    }
    std::sort(err.begin(), err.end());
    const double median = err[N / 2];
    const double p99    = err[(int)(N * 0.99)];
    const double maxe   = err.back();
    int big = 0; for (double e : err) if (e > 0.010) ++big; // >10 m = divergencia real (o lago)
    std::printf("    error elev (m): mediana=%.4f  p99=%.3f  max=%.3f  · >10m: %d/%d\n",
                median * 1000.0, p99 * 1000.0, maxe * 1000.0, big, N);

    // Paridad: la MEDIANA ~0 (Perlin idéntico). Umbral 1 m (0.001 km) holgado para float32.
    CHECK(median < 0.001, "mediana del error CPU↔GPU < 1 m (Perlin GLSL == CPU)");
    // Pocos outliers grandes (lagos, carvado por celda) — no debe ser masivo (=port roto).
    CHECK(big < N / 20, "menos del 5% de puntos con error >10 m (solo lagos, no divergencia global)");
}

// ------------------------------------------------------------------ TEST: estabilidad de órbita (real)
// Sol estático + Tierra orbitando (Kepler analítico del motor). Dirige update() REAL avanzando el
// tiempo por 2 periodos y verifica: distancia ACOTADA [perihelio, afelio] (no espiral/deriva), la
// órbita CIERRA tras un periodo (posición vuelve al inicio) y es PLANAR. La cámara se pone MUY lejos
// → LOD mínimo (test ligero); el terreno planet-local NO se regenera al orbitar (solo cambia el offset).
static void test_orbit_stability() {
    beginTest("orbit_stability");
    PlanetarySystem ps;
    ps.init();

    const double A = 1.0e7, E = 0.2, T = 100.0;   // Tierra: semi-eje, ecc, periodo (s)
    const double Am = 1.5e6, Tm = 25.0;           // Luna: radio y periodo (más corto)

    PlanetarySystem::Planet sun   = makeTestPlanet(2.0e6, 1u, true); sun.name   = "Sun";
    PlanetarySystem::Planet moon  = makeTestPlanet(3.0e5, 3u, true); moon.name  = "Moon";
    PlanetarySystem::Planet earth = makeTestPlanet(1.0e6, 2u, true); earth.name = "Earth";
    // ORDEN A PROPÓSITO DESORDENADO (Sol, Luna, Tierra): la Luna (idx 1) orbita la Tierra (idx 2, que
    // va DESPUÉS) → prueba que la resolución recursiva de padres NO depende del orden del array.
    earth.orbitParent = 0; earth.orbitA = A; earth.orbitEcc = E; earth.orbitPeriod = T;
    earth.orbitU = glm::dvec3(1, 0, 0); earth.orbitV = glm::dvec3(0, 0, 1);
    moon.orbitParent = 2; moon.orbitA = Am; moon.orbitEcc = 0.0; moon.orbitPeriod = Tm; // orbita la Tierra
    moon.orbitU = glm::dvec3(0, 0, 1); moon.orbitV = glm::dvec3(1, 0, 0);
    ps.addPlanet(sun);   // 0
    ps.addPlanet(moon);  // 1  (referencia a un padre de índice MAYOR)
    ps.addPlanet(earth); // 2

    const glm::dvec3 cam(0.0, 0.0, 5.0e8); // MUY lejos → LOD mínimo
    const int steps = 48;
    const double dt = T / steps;

    double dmin = 1e300, dmax = 0.0, ymax = 0.0;
    double mMin = 1e300, mMax = 0.0;       // distancia Luna↔Tierra (debe seguir a la Tierra que se MUEVE)
    glm::dvec3 startPos(0.0), afterOnePeriod(0.0);
    for (int f = 0; f <= steps * 2; ++f) {
        ps.update(dt, cam);
        const glm::dvec3 sunP   = glm::dvec3(ps.getPlanets()[0].position);
        const glm::dvec3 moonP  = glm::dvec3(ps.getPlanets()[1].position);
        const glm::dvec3 earthP = glm::dvec3(ps.getPlanets()[2].position);
        const glm::dvec3 rel = earthP - sunP;
        const double d = glm::length(rel);
        if (f == 0) startPos = rel;
        if (f == steps) afterOnePeriod = rel;
        dmin = std::min(dmin, d); dmax = std::max(dmax, d);
        ymax = std::max(ymax, std::abs(rel.y));
        const double md = glm::length(moonP - earthP);  // relativo a la Tierra (móvil)
        mMin = std::min(mMin, md); mMax = std::max(mMax, md);
    }
    const double peri = A * (1.0 - E), apo = A * (1.0 + E);
    std::printf("    Tierra↔Sol: min=%.3e (peri=%.3e) max=%.3e (apo=%.3e) |cierre|=%.2e fueraPlano=%.1e\n",
                dmin, peri, dmax, apo, glm::length(afterOnePeriod - startPos), ymax);
    std::printf("    Luna↔Tierra: min=%.3e max=%.3e (Am=%.3e) → sigue a la Tierra móvil\n", mMin, mMax, Am);

    CHECK(std::abs(dmin - peri) < A * 0.02, "Tierra↔Sol mínima = perihelio (acotada, no espiral)");
    CHECK(std::abs(dmax - apo)  < A * 0.02, "Tierra↔Sol máxima = afelio (acotada)");
    CHECK(glm::length(afterOnePeriod - startPos) < A * 0.005, "la órbita de la Tierra CIERRA (estable)");
    CHECK(ymax < A * 1e-6, "la órbita de la Tierra es PLANAR");
    // La Luna orbita la Tierra que a su vez se mueve: su distancia a la Tierra queda ≈ Am (circular),
    // aunque su distancia al Sol varíe muchísimo → resolución recursiva multinivel correcta.
    CHECK(std::abs(mMin - Am) < Am * 0.02 && std::abs(mMax - Am) < Am * 0.02,
          "Luna↔Tierra ≈ constante (Am): órbita anidada multinivel correcta, orden-independiente");
}

// ------------------------------------------------------------------ STRESS: calidad/coste por COORDENADA
// Recorre posiciones de cámara representativas (suelo en varias caras/latitudes, vuelo, órbita, espacio),
// asienta el motor REAL en cada una y mide: chunks GPU, triángulos DIBUJADOS (proxy de detalle/calidad),
// chunks de AGUA, cobertura, y ms de update() y de renderPlanetTerrain() (steady-state). Objetivo: VER
// dónde la calidad/coste flojea según dónde estés (costa gruesa, polos, precisión a escala real, etc.).
static void test_stress(TerrainDrawGL* gl, WaterDrawGL* wgl) {
    beginTest("stress_por_coordenada");
    const double R = 2.0e6; // 2000 km: multinivel real sin explotar el conteo en un test
    struct Scn { const char* name; glm::dvec3 dir; double alt; glm::dvec3 look; };
    const glm::dvec3 up(0,1,0), x(1,0,0);
    const Scn scns[] = {
        { "suelo_polo",     glm::dvec3(0,1,0),                 2.0,      glm::dvec3(1,0,-0.2) },
        { "suelo_ecuador",  glm::dvec3(1,0,0),                 2.0,      glm::dvec3(0,1,-0.2) },
        { "suelo_diagonal", glm::normalize(glm::dvec3(1,1,1)), 2.0,      glm::dvec3(0,1,0)    },
        { "vuelo_bajo",     glm::dvec3(0,1,0),                 2000.0,   glm::dvec3(0,-1,0.3) },
        { "vuelo_alto",     glm::dvec3(0,1,0),                 60000.0,  glm::dvec3(0,-1,0.1) },
        { "orbita",         glm::dvec3(0,1,0),                 R * 0.4,  glm::dvec3(0,-1,0)   },
        { "espacio",        glm::dvec3(0,1,0),                 R * 3.0,  glm::dvec3(0,-1,0)   },
    };
    std::printf("    %-14s %7s %9s %7s %7s %7s %8s %s\n",
                "coord", "chunks", "tris", "agua", "upd_ms", "terr_ms", "agua_ms", "cobertura");
    bool anyValid = true;
    for (const auto& s : scns) {
        PlanetarySystem ps; ps.init(); ps.setTerrainBatching(getenv("HARUKA_POOL_DBG") ? true : false);
        ps.addPlanet(makeTestPlanet(R, 4242, true));
        const glm::dvec3 cam = s.dir * (R + s.alt);
        settle(ps, cam, s.look, gl, /*stableN*/20, /*max*/1500);

        // Steady-state: cronometra update(), renderPlanetTerrain() y renderPlanetWater() ya asentados.
        const int N = 30; double tUpd = 0, tDraw = 0, tWater = 0;
        for (int f = 0; f < N; ++f) {
            const float fovY = glm::radians(70.0f);
            glm::mat4 proj = glm::perspective(fovY, 1.0f, 0.1f, 1e12f);
            glm::vec3 fwd = glm::normalize(glm::vec3(s.look));
            glm::vec3 u2 = (std::abs(fwd.y) < 0.95f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
            glm::mat4 vp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), fwd, u2)));
            ps.setTerrainCullMatrix(vp); ps.setLODScreenK(256.0 * 0.5 * (double)proj[1][1]);
            auto t0 = std::chrono::steady_clock::now();
            ps.update(1.0/60.0, cam);
            auto t1 = std::chrono::steady_clock::now();
            if (gl && gl->ok) { gl->bind(); ps.renderPlanetTerrain("TestPlanet", cam); glFinish(); }
            auto t2 = std::chrono::steady_clock::now();
            if (wgl && wgl->ok && gl) { wgl->bind(gl->uboFrame); ps.renderPlanetWater("TestPlanet", cam); glFinish(); }
            auto t3 = std::chrono::steady_clock::now();
            tUpd   += std::chrono::duration<double, std::milli>(t1 - t0).count();
            tDraw  += std::chrono::duration<double, std::milli>(t2 - t1).count();
            tWater += std::chrono::duration<double, std::milli>(t3 - t2).count();
        }
        auto st = ps.getTerrainDrawStats();
        std::string rep = ps.validateLOD();
        bool cov = rep.find("holes=0") != std::string::npos && rep.find("overlaps=0") != std::string::npos;
        anyValid = anyValid && cov;
        std::printf("    %-14s %7d %9d %7d %7.2f %7.2f %8.2f %s\n", s.name,
                    ps.getGPUChunkCount(), st.triangles, ps.getGPUWaterChunkCount(),
                    tUpd / N, tDraw / N, tWater / N, cov ? "ok" : "HUECOS/SOLAPE");
    }
    CHECK(anyValid, "cobertura válida (sin huecos/solapes) en TODAS las coordenadas");
}

// vértices por chunk para un chunkSize dado: rejilla (cs+1)² + faldas 4·(cs+1). Debe COINCIDIR con
// terrain_generator.cpp (vtxCount + skirtVtxCount) → así el test predice el vertexCount del pool.
static inline uint32_t vcForChunkSize(int cs) { return (uint32_t)((cs + 1) * (cs + 1) + 4 * (cs + 1)); }

// ------------------------------------------------------------------ POOL: capacidad fija, sin realloc gigante
// BLINDAJE del fix "buffer fijo persistente": el pool de geometría del terreno reserva su VRAM UNA vez
// a capacidad máxima y NO vuelve a reasignar. Antes crecía por DUPLICACIÓN (glBufferData de cientos de
// MB→GB en el hilo de render = el stall "el terreno desaparece al girar/mirar mar", RenderDoc 22: 2.76 GB
// en 1 frame). Con streaming pesado (fly-in a suelo en planeta grande → miles de chunks, muy por encima
// del viejo umbral de 1024 que disparaba el primer realloc) verificamos que growPool NUNCA dispara y que
// la VRAM del pool queda EXACTA y acotada a la reserva fija.
static void test_pool_fixed_no_grow(TerrainDrawGL* gl) {
    beginTest("pool_capacidad_fija_sin_realloc");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(true);   // camino POOL (el que arreglamos); OFF sería legacy per-chunk
    const int cs = 24;
    const uint32_t vc = vcForChunkSize(cs);
    const double R = 2.0e6;        // 2000 km → LOD profundo → miles de chunks residentes al bajar a suelo
    ps.addPlanet(makeTestPlanet(R, 4242, /*gpu*/true, cs));

    // CHURN real: entra desde el espacio, baja a suelo (llena el pool), sube y vuelve a bajar. Es
    // justo el pico de streaming donde el viejo pool duplicaba y reasignaba.
    const glm::dvec3 pole(0, 1, 0), look(1, 0, -0.2);
    driveEngine(ps, pole * (R * 2.0), glm::dvec3(0, -1, 0), 40, gl);   // espacio
    settle(ps, pole * (R + 3000.0), glm::dvec3(0, -1, 0.2), gl, 15, 1200); // vuelo bajo
    settle(ps, pole * (R + 2.0),    look,                  gl, 20, 1500); // suelo (pico de chunks)
    driveEngine(ps, pole * (R * 1.5), glm::dvec3(0, -1, 0), 30, gl);   // vuelve a subir
    settle(ps, pole * (R + 2.0),    look,                  gl, 15, 1200); // baja otra vez

    auto st = ps.getTerrainPoolStats();
    int gpuChunks = ps.getGPUChunkCount();
    std::printf("    pools=%d  used=%zu/%zu slots  maxCap=%u  vram=%.1fMB  grow=%llu  gpuChunks=%d\n",
                st.poolCount, st.usedSlots, st.totalSlots, st.maxPoolCap,
                st.vramBytes / 1048576.0, (unsigned long long)st.growCount, gpuChunks);

    CHECK(st.growCount == 0, "growPool NUNCA disparó (cero reasignaciones gigantes = sin el stall)");
    CHECK(gpuChunks > 0, "el churn dejó chunks residentes en el pool");
    CHECK(st.poolCount == 1, "1 sola topología → 1 solo pool (rejilla CDLOD uniforme)");
    // Discriminador FIJO-vs-DUPLICADO: la reserva es la capacidad máxima desde el 1er chunk, aunque
    // solo haya ~cientos residentes. El viejo pool (nacía en 1024, ×2 al llenarse) mostraría cap=1024
    // con este nº de chunks; ver maxCap==kMaxResidentChunks lo distingue sin depender de la escala.
    CHECK(st.maxPoolCap == (uint32_t)TerrainRenderer::kMaxResidentChunks,
          "capacidad FIJA a kMaxResidentChunks desde la 1ª reserva (no creció desde 1024)");
    CHECK(st.maxPoolCap > (uint32_t)gpuChunks,
          "capacidad reservada >> chunks vivos (VRAM plana, no ajustada al pico por realloc)");
    CHECK(st.usedSlots <= st.totalSlots, "slots usados <= reservados (invariante de free-list)");
    CHECK(st.usedSlots == (size_t)gpuChunks, "cada chunk residente ocupa exactamente 1 slot del pool");
    const size_t vramExact = (size_t)TerrainRenderer::kMaxResidentChunks * vc * TerrainRenderer::kPoolBytesPerVertex;
    CHECK(st.vramBytes == vramExact, "VRAM del pool = reserva fija exacta (plana, no crece con el nº de chunks)");
    CHECK(st.vramBytes <= TerrainRenderer::poolVramCeil(st.poolCount, vc), "VRAM dentro del techo teórico");
}

// ------------------------------------------------------------------ POOL: multi-topología acotada
// Segundo agujero de correctitud del fix: si aparecen VARIAS topologías (vertexCount distintos —
// p.ej. dos planetas con distinto chunkSize, o islas/cuevas), cada pool reservaba kMaxResidentChunks
// → la VRAM se MULTIPLICABA por nº de topologías (~N×2GB). El blindaje da capacidad completa solo al
// pool DOMINANTE (el primero) y kSecondaryPoolSlots a los demás. Con dos planetas de chunkSize distinto
// verificamos que, aunque haya 2 pools, la VRAM total queda ACOTADA (no 2× el pool grande).
static void test_pool_multitopology_bounded(TerrainDrawGL* gl) {
    beginTest("pool_multitopologia_acotada");
    PlanetarySystem ps;
    ps.init();
    ps.setTerrainBatching(true);
    const int csA = 24, csB = 16;
    const uint32_t vcMax = std::max(vcForChunkSize(csA), vcForChunkSize(csB));
    const double R = 3.0e5; // 300 km cada uno
    // A (home) en el origen; B (distinto chunkSize → OTRA topología) 700 km más allá en +z. La cámara se
    // sitúa a medio camino, cerca de AMBAS superficies, para que las dos transmitan chunks a la vez.
    ps.addPlanet(makeTestPlanet(R, 111, true, csA, "TestPlanet", /*isHome*/true));
    auto pb = makeTestPlanet(R, 222, true, csB, "PlanetB", /*isHome*/false);
    pb.position = WorldPos(0.0, 0.0, 7.0e5);
    ps.addPlanet(pb);

    const glm::dvec3 cam(0.0, 0.0, 3.5e5);     // punto medio: ~50 km sobre el polo de A y bajo el de B
    settle(ps, cam, glm::dvec3(0, 0, 1),  gl, 15, 1500);   // mira a B
    driveEngine(ps, cam, glm::dvec3(0, 0, -1), 60, gl);    // y a A (ambas quedan residentes)

    auto st = ps.getTerrainPoolStats();
    std::printf("    pools=%d  used=%zu/%zu slots  maxCap=%u  vram=%.1fMB  grow=%llu\n",
                st.poolCount, st.usedSlots, st.totalSlots, st.maxPoolCap,
                st.vramBytes / 1048576.0, (unsigned long long)st.growCount);

    CHECK(st.growCount == 0, "growPool NUNCA disparó con 2 topologías");
    CHECK(st.maxPoolCap <= (uint32_t)TerrainRenderer::kMaxResidentChunks, "ningún pool excede el tope global");
    CHECK(st.vramBytes <= TerrainRenderer::poolVramCeil(st.poolCount, vcMax),
          "VRAM total ACOTADA por el techo (dominante lleno + secundarios acotados), no N× el pool grande");
    if (st.poolCount >= 2) {
        // Prueba fuerte del blindaje: si CADA pool reservara el tope global, totalSlots sería
        // poolCount×kMaxResidentChunks. Con el cap secundario debe ser mucho menor.
        const size_t naive = (size_t)st.poolCount * TerrainRenderer::kMaxResidentChunks;
        const size_t bound = (size_t)TerrainRenderer::kMaxResidentChunks
                           + (size_t)(st.poolCount - 1) * TerrainRenderer::kSecondaryPoolSlots;
        CHECK(st.totalSlots <= bound, "los pools secundarios usan el cap reducido (VRAM no se multiplica)");
        CHECK(st.totalSlots <  naive, "totalSlots < poolCount×tope-global (el agujero multi-topología está tapado)");
        std::printf("    2+ topologías materializadas → bound verificado (naive=%zu, real=%zu)\n",
                    naive, st.totalSlots);
    } else {
        std::printf("    (solo 1 pool residente: B no transmitió; bound multi-topología trivialmente OK)\n");
    }
}

// ------------------------------------------------------------------ SWEEP: calidad vs coste (targetPx)
// Fija el LOD a varios niveles de detalle (targetPx: menor = más fino) en una coordenada y mide
// triángulos DIBUJADOS (calidad) vs ms (coste). Responde "cómo mejorar la calidad": ves la curva y
// cuánto cuesta cada escalón → dónde el LOD deja calidad sobre la mesa y qué presupuesto haría falta.
static void test_quality_sweep(TerrainDrawGL* gl) {
    beginTest("barrido_calidad_targetpx");
    const double R = 2.0e6;
    const glm::dvec3 dir(1, 0, 0), cam = dir * (R + 2.0), look(0, 1, -0.2); // suelo ecuador (el más cargado)
    const double pxLevels[] = { 700, 500, 380, 280, 200, 140 };
    std::printf("    coord=suelo_ecuador  (targetPx menor = más fino)\n");
    std::printf("    %8s %9s %7s %8s %8s\n", "targetPx", "tris", "chunks", "upd_ms", "draw_ms");
    long prevTris = 0;
    for (double px : pxLevels) {
        PlanetarySystem ps; ps.init(); ps.setTerrainBatching(false);
        ps.setAdaptiveLOD(false);            // fijo, sin auto-ajuste
        ps.addPlanet(makeTestPlanet(R, 4242, true));
        ps.setLODTargetPx(px);
        settle(ps, cam, look, gl, 20, 1500);
        const int N = 30; double tUpd = 0, tDraw = 0;
        const float fovY = glm::radians(70.0f);
        glm::mat4 proj = glm::perspective(fovY, 1.0f, 0.1f, 1e12f);
        glm::vec3 fwd = glm::normalize(glm::vec3(look));
        glm::mat4 vp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), fwd, glm::vec3(0,1,0))));
        for (int f = 0; f < N; ++f) {
            ps.setTerrainCullMatrix(vp); ps.setLODScreenK(256.0 * 0.5 * (double)proj[1][1]);
            auto t0 = std::chrono::steady_clock::now();
            ps.update(1.0/60.0, cam);
            auto t1 = std::chrono::steady_clock::now();
            if (gl && gl->ok) { gl->bind(); ps.renderPlanetTerrain("TestPlanet", cam); glFinish(); }
            auto t2 = std::chrono::steady_clock::now();
            tUpd  += std::chrono::duration<double, std::milli>(t1 - t0).count();
            tDraw += std::chrono::duration<double, std::milli>(t2 - t1).count();
        }
        auto st = ps.getTerrainDrawStats();
        const char* mark = (prevTris && st.triangles > prevTris) ? "  <- +detalle" : "";
        std::printf("    %8.0f %9d %7d %8.2f %8.2f%s\n", px, st.triangles, ps.getGPUChunkCount(),
                    tUpd / N, tDraw / N, mark);
        prevTris = st.triangles;
    }
    CHECK(true, "barrido informativo (ver tabla calidad/coste)");
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    std::string filter = (argc > 1) ? argv[1] : "";
    auto want = [&](const char* name) { return filter.empty() || std::string(name).find(filter) != std::string::npos; };

    std::printf("== Haruka tests (MOTOR REAL, headless GL) ==\n");

    // El sampler es puro-CPU (no necesita GL); los de LOD/streaming/paridad SÍ (compute + renderer GPU).
    if (want("sampler")) test_sampler_determinism();

    const bool wantGL = want("lod") || want("engine") || want("parity") || want("gpu") || want("orbit")
                     || want("stress") || want("quality") || want("sweep") || want("drawset")
                     || want("bench") || want("gating") || want("delta") || want("pool");
    if (wantGL) {
        GLContext glc;
        if (!glc.ok) { std::printf("  \033[33mSKIP\033[0m tests GPU: no hay contexto GL headless disponible\n"); }
        else {
            TerrainDrawGL draw; draw.init();
            WaterDrawGL water; water.init();
            if (want("parity") || want("gpu")) test_cpu_gpu_parity();
            if (want("orbit")) test_orbit_stability();
            if (want("lod") || want("engine")) {
                if (!draw.ok) std::printf("  (aviso: shader de terreno no cargó — solo update(); eviction/draw-set no)\n");
                test_engine_loads_terrain(&draw);
                test_engine_rotate_keeps_terrain(&draw);
            }
            if (want("drawset") || want("bench")) test_drawset_bench(&draw);
            if (want("drawset") || want("gating")) test_drawset_cache_gating(&draw);
            if (want("drawset") || want("delta"))  test_drawset_delta_streaming(&draw);
            if (want("pool")) {
                test_pool_fixed_no_grow(&draw);
                test_pool_multitopology_bounded(&draw);
            }
            if (want("stress")) test_stress(&draw, &water);
            if (want("stress") || want("quality") || want("sweep")) test_quality_sweep(&draw);
        }
    }

    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
