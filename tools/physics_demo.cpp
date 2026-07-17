// ================================================================================================
// physics_demo — banco VISUAL de la física del motor: cubos y esferas cayendo sobre terreno
// irregular. Comprueba a ojo la capacidad del motor (gravedad radial + ground-snap sobre un
// heightfield) y demuestra la SELECCIÓN DE API GRÁFICA vía la fábrica real del RHI.
//
// Uso:  physics_demo [--api gl|vk] [--bodies N]
//   --api gl   (por defecto) → backend OpenGL del RHI.
//   --api vk               → Vulkan solicitado; como el backend Vulkan aún no existe, el RHI cae a
//                            OpenGL (aviso claro). La INFRAESTRUCTURA de selección es real: cuando el
//                            backend Vulkan aterrice, este mismo demo lo usará sin tocar nada.
//
// Los cuerpos BROTAN de un punto (fuente) y el número CRECE con el tiempo hasta `--bodies` (tope).
// Al caer se DESLIZAN cuesta abajo por la pendiente del terreno (gravedad tangente + rozamiento) y
// RUEDAN (rotación ω = v/r) → no se clavan donde caen.
//
// Controles:  arrastrar ratón = orbitar · rueda = zoom · R = reiniciar al punto · ESC = salir.
//
// La física es la REAL (`Haruka::Physics::PhysicsEngine`): los cuerpos son esferas (radius) que caen
// por gravedad radial y se asientan sobre el terreno vía `IWorldProvider::terrainHeightAt`. El MISMO
// ruido `terrainH()` alimenta la MALLA y la COLISIÓN → lo que ves es exactamente donde chocan.
// ================================================================================================
#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/physics_engine.h"
#include "physics/world_provider.h"
#include "rhi/rhi_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <memory>

using namespace Haruka;

// ---------------------------------------------------------------- terreno irregular (ruido compartido)
// Value-noise 2D determinista + fBm. LA MISMA función la usan la malla y la colisión → paridad exacta.
static float hash2(int x, int z) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF; // [0,1]
}
static float vnoise(float x, float z) {
    int xi = (int)std::floor(x), zi = (int)std::floor(z);
    float fx = x - xi, fz = z - zi;
    float u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
    float a = hash2(xi, zi),     b = hash2(xi + 1, zi);
    float c = hash2(xi, zi + 1), d = hash2(xi + 1, zi + 1);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v);
}
// Altura del terreno (m) en (x,z) del plano local. fBm de 4 octavas, ~14 m de relieve.
static double terrainH(double x, double z) {
    float h = 0.0f, amp = 8.0f, freq = 0.045f;
    for (int o = 0; o < 4; ++o) {
        h += (vnoise((float)x * freq, (float)z * freq) - 0.5f) * 2.0f * amp;
        amp *= 0.5f; freq *= 2.03f;
    }
    return (double)h;
}
// ---------------------------------------------------------------- mundo para la física (IWorldProvider)
// Un "planeta" ENORME centrado muy por debajo → el parche es localmente plano y la gravedad apunta a
// −Y. El ground-snap del motor asienta cada cuerpo en `radio + terrainH(x,z) + body.radius`.
struct DemoWorld : Physics::IWorldProvider {
    static constexpr double R = 6.371e6;                 // radio grande = parche plano, gravedad ~−Y
    glm::dvec3 center{0.0, -R, 0.0};                     // centro muy abajo
    std::vector<Physics::GravBody> grav;
    DemoWorld() {
        const double G = 6.67430e-11, g = 9.81;
        grav.push_back({ center, g * R * R / G });        // masa que da 9.81 m/s² en la superficie
    }
    bool       hasActivePlanet()    const override { return true; }
    glm::dvec3 activePlanetCenter() const override { return center; }
    double     activePlanetRadius() const override { return R; }
    const std::vector<Physics::GravBody>& gravBodies() const override { return grav; }
    // Altura del terreno SOBRE la esfera de referencia en la vertical de `worldPos` (x,z locales).
    double     terrainHeightAt(const glm::dvec3& p) const override { return terrainH(p.x, p.z); }
};

// ---------------------------------------------------------------- utilidades GL (shaders inline)
static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::fprintf(stderr, "shader: %s\n", log); }
    return s;
}
static GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static const char* kVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMVP; uniform mat4 uModel;
out vec3 vN; out vec3 vW;
void main(){ vW=(uModel*vec4(aPos,1.0)).xyz; vN=mat3(uModel)*aNormal; gl_Position=uMVP*vec4(aPos,1.0); }
)";
static const char* kFS = R"(#version 330 core
in vec3 vN; in vec3 vW; out vec4 FragColor;
uniform vec3 uColor; uniform vec3 uLightDir;
void main(){
    vec3 N=normalize(vN); float d=max(dot(N,normalize(-uLightDir)),0.0);
    float rim=pow(1.0-max(dot(N,vec3(0,1,0)),0.0),1.5)*0.15;
    vec3 c=uColor*(0.28+0.72*d)+rim;
    FragColor=vec4(c,1.0);
}
)";

// ---------------------------------------------------------------- geometría (malla con posición+normal)
struct Mesh { GLuint vao=0, vbo=0, ebo=0; GLsizei count=0; };
static Mesh upload(const std::vector<float>& interleaved /*pos3,nrm3*/, const std::vector<unsigned>& idx) {
    Mesh m; m.count = (GLsizei)idx.size();
    glGenVertexArrays(1, &m.vao); glBindVertexArray(m.vao);
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, interleaved.size() * sizeof(float), interleaved.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned), idx.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);            glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    return m;
}

// Malla del terreno: rejilla NxN sobre [-half,half]², altura = terrainH, normal por diferencias finitas.
static Mesh buildTerrain(int N, float half) {
    std::vector<float> v; std::vector<unsigned> idx;
    const float step = (2 * half) / (N - 1);
    for (int j = 0; j < N; ++j) for (int i = 0; i < N; ++i) {
        float x = -half + i * step, z = -half + j * step;
        float y  = (float)terrainH(x, z);
        float hl = (float)terrainH(x - step, z), hr = (float)terrainH(x + step, z);
        float hd = (float)terrainH(x, z - step), hu = (float)terrainH(x, z + step);
        glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));
        v.insert(v.end(), { x, y, z, n.x, n.y, n.z });
    }
    for (int j = 0; j < N - 1; ++j) for (int i = 0; i < N - 1; ++i) {
        unsigned a = j * N + i, b = a + 1, c = a + N, d = c + 1;
        idx.insert(idx.end(), { a, c, b, b, c, d });
    }
    return upload(v, idx);
}

static Mesh buildCube() {
    // 6 caras, normal por cara. Half-size 1 (se escala por el radio al dibujar).
    const glm::vec3 nrm[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const int face[6][4] = {{5,4,6,7},{0,1,3,2},{2,3,7,6},{0,4,5,1},{1,5,7,3},{4,0,2,6}};
    const glm::vec3 c[8] = {{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},{-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}};
    std::vector<float> v; std::vector<unsigned> idx;
    for (int f = 0; f < 6; ++f) {
        unsigned base = (unsigned)(v.size() / 6);
        for (int k = 0; k < 4; ++k) { glm::vec3 p = c[face[f][k]]; v.insert(v.end(), { p.x, p.y, p.z, nrm[f].x, nrm[f].y, nrm[f].z }); }
        idx.insert(idx.end(), { base, base+1, base+2, base, base+2, base+3 });
    }
    return upload(v, idx);
}

static Mesh buildSphere(int seg = 20, int ring = 14) {
    std::vector<float> v; std::vector<unsigned> idx;
    for (int r = 0; r <= ring; ++r) {
        float phi = (float)M_PI * r / ring;
        for (int s = 0; s <= seg; ++s) {
            float th = 2.0f * (float)M_PI * s / seg;
            glm::vec3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
            v.insert(v.end(), { n.x, n.y, n.z, n.x, n.y, n.z }); // radio 1 → pos == normal
        }
    }
    int stride = seg + 1;
    for (int r = 0; r < ring; ++r) for (int s = 0; s < seg; ++s) {
        unsigned a = r * stride + s, b = a + stride;
        idx.insert(idx.end(), { a, b, a + 1, a + 1, b, b + 1 });
    }
    return upload(v, idx);
}

// ---------------------------------------------------------------- cuerpos del demo
struct Body { std::shared_ptr<Physics::RigidBody> rb; bool isCube; glm::vec3 color; glm::mat4 rot{1.0f}; };

static uint32_t g_seed = 12345u;
static float rnd01() { g_seed = g_seed * 1664525u + 1013904223u; return (float)(g_seed >> 8) / (float)0xFFFFFF; }

// Genera UN cuerpo en el punto de spawn (con un pelín de dispersión y velocidad → fuente). El número
// total crece llamando a esto por un temporizador en el bucle.
static void spawnOne(Physics::PhysicsEngine& phys, std::vector<Body>& bodies, const glm::dvec3& at) {
    auto rb = std::make_shared<Physics::RigidBody>();
    const double r       = 0.6 + rnd01() * 1.1;
    const double density = 0.5 + rnd01() * 2.5;                       // densidad ALEATORIA por objeto
    rb->radius = r;
    rb->mass   = density * (4.0 / 3.0) * 3.14159265 * r * r * r;      // masa ∝ densidad·radio³ → distinta
    rb->isKinematic = false;
    rb->position = at + glm::dvec3((rnd01() - 0.5) * 2.5, 0.0, (rnd01() - 0.5) * 2.5);
    rb->velocity = glm::dvec3((rnd01() - 0.5) * 6.0, 1.0 + rnd01() * 2.0, (rnd01() - 0.5) * 6.0);
    rb->name = "demo";
    const bool cube = (bodies.size() & 1) != 0;
    if (cube) {                                  // CAJA real: colisiona por sus esquinas (según su forma)
        rb->shape = Physics::RigidBody::Shape::Box;
        // Proporciones VARIADAS (plano, alto, cúbico) → la forma REAL importa: un tocho plano se apoya
        // en su cara y uno alto vuelca fácil. La colisión sigue las 8 esquinas de ESTAS medidas.
        rb->halfExtents = glm::dvec3(r * (0.5 + rnd01() * 0.7), r * (0.4 + rnd01() * 0.9), r * (0.5 + rnd01() * 0.7));
        rb->radius = glm::length(rb->halfExtents);   // radio ENVOLVENTE (broad-phase + cuerpo-cuerpo)
        rb->comOffset = glm::dvec3((rnd01() - 0.5), (rnd01() - 0.5) * 0.2, (rnd01() - 0.5)) * (r * 0.15); // balance leve
    }
    phys.addBody(rb);
    Body b; b.rb = rb; b.isCube = cube;
    b.color = cube ? glm::vec3(0.90f, 0.55f, 0.20f) : glm::vec3(0.30f, 0.55f, 0.95f);
    bodies.push_back(b);
}

int main(int argc, char** argv) {
    RHI::Backend backend = RHI::Backend::OpenGL;
    int nBodies = 3000;   // TOPE: los cuerpos brotan de un punto y el número crece hasta aquí
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--api") && i + 1 < argc) {
            ++i; if (!std::strcmp(argv[i], "vk") || !std::strcmp(argv[i], "vulkan")) backend = RHI::Backend::Vulkan;
        } else if (!std::strcmp(argv[i], "--bodies") && i + 1 < argc) { nBodies = std::atoi(argv[++i]); }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    int W = 1280, H = 720;
    SDL_Window* win = SDL_CreateWindow("HarukaEngine — physics demo", W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, ctx);
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) { std::fprintf(stderr, "gladLoad falló\n"); return 1; }

    // SELECCIÓN DE API GRÁFICA vía la fábrica REAL del RHI. Vulkan cae a OpenGL (aún no hay backend).
    auto device = RHI::Device::create(backend, win);
    const char* apiName = "OpenGL";
    char title[128];
    if (backend == RHI::Backend::Vulkan)
        std::snprintf(title, sizeof(title), "HarukaEngine — physics demo · API: Vulkan solicitado → OpenGL (fallback)");
    else
        std::snprintf(title, sizeof(title), "HarukaEngine — physics demo · API: OpenGL");
    SDL_SetWindowTitle(win, title);
    std::fprintf(stderr, "[demo] backend pedido=%s → RHI activo=%s (device=%s)\n",
                 backend == RHI::Backend::Vulkan ? "Vulkan" : "OpenGL", apiName, device ? "OK" : "null");

    glEnable(GL_DEPTH_TEST);

    const float HALF = 64.0f;
    Mesh terrain = buildTerrain(160, HALF);
    Mesh cube = buildCube();
    Mesh sphere = buildSphere();
    GLuint prog = program(kVS, kFS);
    GLint uMVP = glGetUniformLocation(prog, "uMVP");
    GLint uModel = glGetUniformLocation(prog, "uModel");
    GLint uColor = glGetUniformLocation(prog, "uColor");
    GLint uLight = glGetUniformLocation(prog, "uLightDir");

    DemoWorld world;
    Physics::PhysicsEngine phys;
    phys.setWorldProvider(&world);
    std::vector<Body> bodies;
    const int maxBodies = nBodies;                     // --bodies = TOPE; el número CRECE hasta él
    const glm::dvec3 SPAWN(0.0, 40.0, 0.0);            // TODOS brotan de este punto (fuente)
    double spawnTimer = 0.0; const double spawnEvery = 0.08;   // ~12 cuerpos/s
    int frameN = 0;

    // Cámara orbital.
    float yaw = 0.7f, pitch = 0.5f, dist = 130.0f;
    const glm::vec3 target(0.0f, 8.0f, 0.0f);
    bool dragging = false;

    uint64_t prev = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) running = false;
                else if (e.key.key == SDLK_R) { for (auto& b : bodies) { b.rb->position = SPAWN + glm::dvec3((rnd01()-0.5)*2.5, 0, (rnd01()-0.5)*2.5); b.rb->velocity = glm::dvec3(0.0); } }  // reiniciar al punto
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) dragging = true;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP)   dragging = false;
            else if (e.type == SDL_EVENT_MOUSE_MOTION && dragging) {
                yaw   += e.motion.xrel * 0.005f;
                pitch = glm::clamp(pitch + e.motion.yrel * 0.005f, -1.4f, 1.4f);
            }
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) dist = glm::clamp(dist - e.wheel.y * 6.0f, 25.0f, 320.0f);
            else if (e.type == SDL_EVENT_WINDOW_RESIZED) { W = e.window.data1; H = e.window.data2; }
        }

        uint64_t now = SDL_GetPerformanceCounter();
        double dt = (now - prev) / freq; prev = now;
        if (dt > 0.05) dt = 0.05;

        // FUENTE: el número CRECE desde el punto de spawn hasta el tope.
        spawnTimer += dt;
        while (spawnTimer >= spawnEvery && (int)bodies.size() < maxBodies) { spawnTimer -= spawnEvery; spawnOne(phys, bodies, SPAWN); }

        // El DESLIZAMIENTO cuesta abajo por la pendiente ya lo hace el MOTOR (ground-snap con la
        // componente tangente de la gravedad + ángulo de reposo) → aquí solo integramos.
        phys.advance(dt);

        // Rodadura COSMÉTICA de las ESFERAS (ω = v/r). Los CUBOS rotan por el MOTOR (vuelco por balance).
        for (auto& b : bodies) {
            if (b.isCube) continue;
            const glm::dvec3 vh(b.rb->velocity.x, 0.0, b.rb->velocity.z);
            const double sp = glm::length(vh);
            if (sp > 0.05) {
                const glm::dvec3 axis = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), vh));
                const float ang = (float)(sp * dt / std::max(0.1, b.rb->radius));
                b.rot = glm::rotate(glm::mat4(1.0f), ang, glm::vec3(axis)) * b.rot;
            }
        }
        if ((++frameN % 20) == 0) { char t[192]; std::snprintf(t, sizeof(t), "%s · cuerpos: %zu/%d", title, bodies.size(), maxBodies); SDL_SetWindowTitle(win, t); }

        glViewport(0, 0, W, H);
        glClearColor(0.55f, 0.70f, 0.88f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::vec3 eye = target + glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)) * dist;
        glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
        glm::mat4 proj = glm::perspective(glm::radians(55.0f), (float)W / (float)std::max(1, H), 0.5f, 2000.0f);
        glm::mat4 VP = proj * view;

        glUseProgram(prog);
        glUniform3f(uLight, -0.4f, -1.0f, -0.35f);

        // Terreno.
        glm::mat4 I(1.0f);
        glUniformMatrix4fv(uModel, 1, GL_FALSE, &I[0][0]);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, &VP[0][0]);
        glUniform3f(uColor, 0.34f, 0.42f, 0.24f);
        glBindVertexArray(terrain.vao);
        glDrawElements(GL_TRIANGLES, terrain.count, GL_UNSIGNED_INT, 0);

        // Cuerpos (cubos y esferas) en la posición que dicta la física.
        for (const auto& b : bodies) {
            glm::vec3 p = glm::vec3(b.rb->position);
            // Cubo: CAJA real de medios-lados NO uniformes (halfExtents) con la orientación del MOTOR →
            // se apoya en su cara y vuelca sobre la arista. Esfera: radio de colisión + rodadura.
            glm::mat4 model;
            if (b.isCube) {
                glm::mat4 rot = glm::mat4_cast(glm::quat(b.rb->orientation));
                model = glm::translate(I, p) * rot * glm::scale(glm::mat4(1.0f), glm::vec3(b.rb->halfExtents));
            } else {
                model = glm::translate(I, p) * b.rot * glm::scale(glm::mat4(1.0f), glm::vec3((float)b.rb->radius));
            }
            glm::mat4 mvp = VP * model;
            glUniformMatrix4fv(uModel, 1, GL_FALSE, &model[0][0]);
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, &mvp[0][0]);
            glUniform3f(uColor, b.color.r, b.color.g, b.color.b);
            const Mesh& m = b.isCube ? cube : sphere;
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, 0);
        }

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
