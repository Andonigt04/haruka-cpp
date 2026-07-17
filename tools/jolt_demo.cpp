// ================================================================================================
// jolt_demo — PRUEBA de integración de Jolt Physics en la estructura del motor. Valida a ojo lo que el
// solucionador a mano NO conseguía: APILADO ESTABLE (cajas amontonadas que no se atraviesan ni se
// hunden), con SAT/manifolds reales. Física LOCAL (marco plano, gravedad −Y) — el mismo marco en el
// que correría en el juego con el origin-shift que ya existe.
//
// Escena:  una TORRE de cajas apilada (debe quedarse en pie) + lluvia de cajas y esferas sobre el suelo.
// Controles:  arrastrar ratón = orbitar · rueda = zoom · R = reiniciar · ESC = salir.
//
// Jolt es GL-free y determinista → encaja bajo HarukaPhysics/IWorldProvider y cumple el DGS. Aquí solo
// se prueba el núcleo de rígidos; el terreno real (malla local → mesh shape) es el siguiente paso.
// ================================================================================================
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>

JPH_SUPPRESS_WARNINGS
using namespace JPH;
using namespace JPH::literals;

// ---------------------------------------------------------------- Jolt: trace/assert + capas (estándar)
static void TraceImpl(const char* fmt, ...) {
    va_list a; va_start(a, fmt); char buf[1024]; vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    std::fprintf(stderr, "[jolt] %s\n", buf);
}
#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* e, const char* m, const char* f, uint l) {
    std::fprintf(stderr, "[jolt] %s:%u (%s) %s\n", f, l, e, m ? m : ""); return true;
}
#endif

namespace Layers { static constexpr ObjectLayer NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2; }
namespace BroadPhaseLayers { static constexpr BroadPhaseLayer NON_MOVING(0), MOVING(1); static constexpr uint NUM_LAYERS(2); }

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() { m[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING; m[Layers::MOVING] = BroadPhaseLayers::MOVING; }
    uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return m[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer) const override { return "layer"; }
#endif
private: BroadPhaseLayer m[Layers::NUM_LAYERS];
};
class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer l1, BroadPhaseLayer l2) const override {
        return l1 == Layers::NON_MOVING ? (l2 == BroadPhaseLayers::MOVING) : true;
    }
};
class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer o1, ObjectLayer o2) const override {
        return o1 == Layers::NON_MOVING ? (o2 == Layers::MOVING) : true;
    }
};

// ---------------------------------------------------------------- GL mínimo (igual patrón que physics_demo)
static GLuint compile(GLenum t, const char* s) {
    GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(sh, 1024, nullptr, l); std::fprintf(stderr, "shader: %s\n", l); }
    return sh;
}
static GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram(), v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p); glDeleteShader(v); glDeleteShader(f); return p;
}
static const char* kVS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal;
uniform mat4 uMVP, uModel; out vec3 vN;
void main(){ vN = mat3(uModel)*aNormal; gl_Position = uMVP*vec4(aPos,1.0); })";
static const char* kFS = R"(#version 330 core
in vec3 vN; out vec4 F; uniform vec3 uColor, uLightDir;
void main(){ float d = max(dot(normalize(vN), normalize(-uLightDir)), 0.0); F = vec4(uColor*(0.3+0.7*d), 1.0); })";

struct Mesh { GLuint vao = 0; GLsizei count = 0; };
static Mesh upload(const std::vector<float>& v, const std::vector<unsigned>& idx) {
    Mesh m; m.count = (GLsizei)idx.size(); GLuint vbo, ebo;
    glGenVertexArrays(1, &m.vao); glBindVertexArray(m.vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12); glEnableVertexAttribArray(1);
    glBindVertexArray(0); return m;
}
static Mesh buildCube() {
    const glm::vec3 nrm[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const int face[6][4] = {{5,4,6,7},{0,1,3,2},{2,3,7,6},{0,4,5,1},{1,5,7,3},{4,0,2,6}};
    const glm::vec3 c[8] = {{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},{-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}};
    std::vector<float> v; std::vector<unsigned> idx;
    for (int f = 0; f < 6; ++f) { unsigned b = (unsigned)(v.size() / 6);
        for (int k = 0; k < 4; ++k) { glm::vec3 p = c[face[f][k]]; v.insert(v.end(), {p.x,p.y,p.z,nrm[f].x,nrm[f].y,nrm[f].z}); }
        idx.insert(idx.end(), {b,b+1,b+2,b,b+2,b+3}); }
    return upload(v, idx);
}
static Mesh buildSphere(int seg = 18, int ring = 12) {
    std::vector<float> v; std::vector<unsigned> idx;
    for (int r = 0; r <= ring; ++r) { float phi = (float)M_PI * r / ring;
        for (int s = 0; s <= seg; ++s) { float th = 2.f*(float)M_PI*s/seg;
            glm::vec3 n(std::sin(phi)*std::cos(th), std::cos(phi), std::sin(phi)*std::sin(th));
            v.insert(v.end(), {n.x,n.y,n.z,n.x,n.y,n.z}); } }
    int st = seg + 1;
    for (int r = 0; r < ring; ++r) for (int s = 0; s < seg; ++s) { unsigned a = r*st+s, b = a+st;
        idx.insert(idx.end(), {a,b,a+1u,a+1u,b,b+1u}); }
    return upload(v, idx);
}

// ---------------------------------------------------------------- cuerpos del demo
struct Obj { BodyID id; bool isBox; glm::vec3 he; float radius; glm::vec3 color; JPH::RVec3 spawn; };

static uint32_t g_seed = 7u;
static float rnd() { g_seed = g_seed * 1664525u + 1013904223u; return (float)(g_seed >> 8) / (float)0xFFFFFF; }

int main() {
    // -------- Jolt init --------
    RegisterDefaultAllocator();
    Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)
    Factory::sInstance = new Factory();
    RegisterTypes();
    TempAllocatorImpl temp(32 * 1024 * 1024);
    JobSystemThreadPool jobs(cMaxPhysicsJobs, cMaxPhysicsBarriers, (int)std::max(1u, std::thread::hardware_concurrency()) - 1);
    BPLayerInterfaceImpl bpli;
    ObjectVsBroadPhaseLayerFilterImpl ovbp;
    ObjectLayerPairFilterImpl ovo;
    PhysicsSystem ps;
    ps.Init(4096, 0, 16384, 16384, bpli, ovbp, ovo);
    ps.SetGravity(Vec3(0, -9.81f, 0));
    BodyInterface& bi = ps.GetBodyInterface();

    // Suelo estático (caja grande y plana).
    bi.CreateAndAddBody(BodyCreationSettings(new BoxShape(Vec3(60, 1, 60)), RVec3(0, -1, 0),
                        Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING), EActivation::DontActivate);

    std::vector<Obj> bodies;
    auto addBox = [&](glm::vec3 pos, glm::vec3 he, glm::vec3 col) {
        BodyCreationSettings s(new BoxShape(Vec3(he.x, he.y, he.z)), RVec3(pos.x, pos.y, pos.z),
                               Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
        BodyID id = bi.CreateAndAddBody(s, EActivation::Activate);
        bodies.push_back({id, true, he, 0.f, col, RVec3(pos.x, pos.y, pos.z)});
    };
    auto addSphere = [&](glm::vec3 pos, float r, glm::vec3 col) {
        BodyCreationSettings s(new SphereShape(r), RVec3(pos.x, pos.y, pos.z),
                               Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
        BodyID id = bi.CreateAndAddBody(s, EActivation::Activate);
        bodies.push_back({id, false, glm::vec3(r), r, col, RVec3(pos.x, pos.y, pos.z)});
    };
    // TORRE de cajas apilada con precisión (la prueba: debe QUEDARSE EN PIE, no colapsar ni temblar).
    const glm::vec3 orange(0.90f, 0.55f, 0.20f), blue(0.30f, 0.55f, 0.95f);
    for (int i = 0; i < 12; ++i) addBox(glm::vec3(0, 1.0f + i * 2.0f, 0), glm::vec3(1.0f), orange);
    // Lluvia de cajas y esferas alrededor.
    for (int i = 0; i < 160; ++i) {
        glm::vec3 p((rnd()-0.5f)*30.f, 15.f + rnd()*40.f, (rnd()-0.5f)*30.f);
        if (i & 1) addBox(p, glm::vec3(0.6f + rnd()*0.9f, 0.6f + rnd()*0.9f, 0.6f + rnd()*0.9f), orange);
        else       addSphere(p, 0.6f + rnd()*0.8f, blue);
    }
    ps.OptimizeBroadPhase();

    // -------- GL / ventana --------
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    int W = 1280, H = 720;
    SDL_Window* win = SDL_CreateWindow("HarukaEngine — Jolt physics (apilado estable)", W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext ctx = SDL_GL_CreateContext(win); SDL_GL_MakeCurrent(win, ctx);
    gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress);
    glEnable(GL_DEPTH_TEST);
    Mesh cube = buildCube(), sphere = buildSphere();
    GLuint prog = program(kVS, kFS);
    GLint uMVP = glGetUniformLocation(prog, "uMVP"), uModel = glGetUniformLocation(prog, "uModel");
    GLint uColor = glGetUniformLocation(prog, "uColor"), uLight = glGetUniformLocation(prog, "uLightDir");

    float yaw = 0.7f, pitch = 0.35f, dist = 55.f; const glm::vec3 target(0, 6, 0); bool drag = false;
    uint64_t prev = SDL_GetPerformanceCounter(); const double freq = (double)SDL_GetPerformanceFrequency();
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) run = false;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) run = false;
                else if (e.key.key == SDLK_R) for (auto& b : bodies) {
                    bi.SetPositionAndRotation(b.id, b.spawn, Quat::sIdentity(), EActivation::Activate);
                    bi.SetLinearVelocity(b.id, Vec3::sZero()); bi.SetAngularVelocity(b.id, Vec3::sZero());
                }
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) drag = true;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) drag = false;
            else if (e.type == SDL_EVENT_MOUSE_MOTION && drag) { yaw += e.motion.xrel*0.005f; pitch = glm::clamp(pitch + e.motion.yrel*0.005f, -1.4f, 1.4f); }
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) dist = glm::clamp(dist - e.wheel.y*3.f, 12.f, 160.f);
            else if (e.type == SDL_EVENT_WINDOW_RESIZED) { W = e.window.data1; H = e.window.data2; }
        }
        uint64_t now = SDL_GetPerformanceCounter(); double dt = (now - prev) / freq; prev = now;
        if (dt > 0.05) dt = 0.05;
        ps.Update((float)dt, 1, &temp, &jobs);

        glViewport(0, 0, W, H); glClearColor(0.55f, 0.70f, 0.88f, 1.f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glm::vec3 eye = target + glm::vec3(std::cos(pitch)*std::sin(yaw), std::sin(pitch), std::cos(pitch)*std::cos(yaw)) * dist;
        glm::mat4 VP = glm::perspective(glm::radians(55.f), (float)W/(float)std::max(1,H), 0.3f, 1000.f) * glm::lookAt(eye, target, glm::vec3(0,1,0));
        glUseProgram(prog); glUniform3f(uLight, -0.4f, -1.f, -0.35f);
        glm::mat4 I(1.f);

        // Suelo (caja plana verde).
        { glm::mat4 m = glm::scale(glm::translate(I, glm::vec3(0,-1,0)), glm::vec3(60,1,60));
          glUniformMatrix4fv(uModel, 1, GL_FALSE, &m[0][0]); glm::mat4 mvp = VP*m; glUniformMatrix4fv(uMVP,1,GL_FALSE,&mvp[0][0]);
          glUniform3f(uColor, 0.34f, 0.42f, 0.24f); glBindVertexArray(cube.vao); glDrawElements(GL_TRIANGLES, cube.count, GL_UNSIGNED_INT, 0); }

        for (auto& b : bodies) {
            RVec3 pos = bi.GetPosition(b.id); Quat q = bi.GetRotation(b.id);
            glm::vec3 p((float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ());
            glm::quat gq(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
            glm::mat4 m = glm::translate(I, p) * glm::mat4_cast(gq) * glm::scale(I, b.isBox ? b.he : glm::vec3(b.radius));
            glUniformMatrix4fv(uModel, 1, GL_FALSE, &m[0][0]); glm::mat4 mvp = VP*m; glUniformMatrix4fv(uMVP,1,GL_FALSE,&mvp[0][0]);
            glUniform3f(uColor, b.color.r, b.color.g, b.color.b);
            const Mesh& mesh = b.isBox ? cube : sphere;
            glBindVertexArray(mesh.vao); glDrawElements(GL_TRIANGLES, mesh.count, GL_UNSIGNED_INT, 0);
        }
        SDL_GL_SwapWindow(win);
    }

    for (auto& b : bodies) { bi.RemoveBody(b.id); bi.DestroyBody(b.id); }
    UnregisterTypes();
    delete Factory::sInstance; Factory::sInstance = nullptr;
    SDL_GL_DestroyContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
