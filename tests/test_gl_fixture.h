#ifndef HARUKA_TEST_GL_FIXTURE_H
#define HARUKA_TEST_GL_FIXTURE_H
// ================================================================================================
// Fixtures que REQUIEREN OpenGL: contexto oculto (ventana 256² HIDDEN) + shaders reales de terreno/
// agua + drivers del MOTOR REAL (makeTestPlanet/driveEngine/settle) que replican el frame del juego.
// Header-only (structs con métodos in-class = inline; helpers `static`): lo incluyen SOLO las TUs que
// tocan GL — test_render_gl.cpp y haruka_tests.cpp (main). Los .cpp puros-CPU NO lo incluyen.
// ================================================================================================
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>
#include <thread>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "game/planetary_system.h"
#include "renderer/shader.h"

using namespace Haruka;

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
#endif // HARUKA_TEST_GL_FIXTURE_H
