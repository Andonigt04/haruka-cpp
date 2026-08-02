/// Planet Demo — muestra un planeta con chunks toggleables
/// Controles: Arrastrar ratón = orbitar, Scroll = zoom, Click en lista = toggle chunk

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <map>

#include <SDL3/SDL.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/planet/planet.h"
#include "core/planet/geology.h"
#include "core/planet/climate.h"
#include "core/planet/biomes.h"

// ---- ImGui ----
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

// ===========================================================================
// Embedded shaders
// ===========================================================================
static const char* kVertSrc = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNorm;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;
uniform vec3 uCenter;

out vec3 vNorm;
out vec3 vFragPos;
out vec2 vUv;
out vec3 vColor;

void main() {
    vec3 worldPos = (uModel * vec4(aPos + uCenter, 1.0)).xyz;
    vFragPos = worldPos;
    vNorm = normalize(mat3(uModel) * aNorm);
    vUv = aUv;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos + uCenter, 1.0);
}
)";

static const char* kFragSrc = R"(#version 460 core
in vec3 vNorm;
in vec3 vFragPos;
in vec2 vUv;
in vec3 vColor;

uniform vec3 uLightDir = vec3(0.3, 0.8, 0.5);
uniform vec3 uLightColor = vec3(1.0, 0.95, 0.9);
uniform vec3 uAmbient = vec3(0.05, 0.08, 0.12);
uniform bool uWireframe = false;

out vec4 fragColor;

void main() {
    if (uWireframe) {
        fragColor = vec4(0.0, 1.0, 0.0, 1.0);
        return;
    }
    vec3 n = normalize(vNorm);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 color = vColor * (uAmbient + uLightColor * diff);
    fragColor = vec4(color, 1.0);
}
)";

static const char* kWireVertSrc = R"(#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uCenter;
void main() {
    gl_Position = uMVP * vec4(aPos + uCenter, 1.0);
}
)";

static const char* kWireFragSrc = R"(#version 460 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.3, 0.1, 0.6); }
)";

// ===========================================================================
// Simple shader compile helper
// ===========================================================================
static GLuint compileShader(const char* src, GLenum type) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error (%s): %s\n",
                type == GL_VERTEX_SHADER ? "vert" : "frag", log);
    }
    return s;
}

static GLuint linkShaders(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error: %s\n", log);
    }
    return p;
}

// ===========================================================================
// Orbit camera
// ===========================================================================
struct OrbitCamera {
    float theta = 0.0f, phi = 0.6f;
    float dist = 4.0f;
    glm::dvec3 target{0};

    glm::mat4 viewProj(float aspect) const {
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.01f, 1000.0f);
        glm::vec3 eye = target + glm::dvec3(
            dist * cos(theta) * cos(phi),
            dist * sin(phi),
            dist * sin(theta) * cos(phi));
        glm::mat4 view = glm::lookAt(glm::vec3(eye), glm::vec3(target), glm::vec3(0, 1, 0));
        return proj * view;
    }
};

// ===========================================================================
// Per-chunk GPU data
// ===========================================================================
struct GpuChunk {
    GLuint vao = 0, vbo = 0, ebo = 0;
    uint32_t indexCount = 0;
    glm::dvec3 center{0};
    Haruka::Planet::ChunkKey key;
    bool enabled = true;

    void upload(const Haruka::Planet::Chunk& chunk,
                const std::vector<glm::vec3>& colors) {
        if (vao) { glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo); glDeleteBuffers(1, &ebo); }

        // Interleaved: pos3 + norm3 + uv2 + col3 = 44 bytes
        struct Vertex { float px, py, pz, nx, ny, nz, u, v, cr, cg, cb; };
        std::vector<Vertex> verts(chunk.positions.size());
        for (size_t i = 0; i < chunk.positions.size(); i++) {
            verts[i] = {
                chunk.positions[i].x, chunk.positions[i].y, chunk.positions[i].z,
                chunk.normals[i].x, chunk.normals[i].y, chunk.normals[i].z,
                chunk.uvs[i].x, chunk.uvs[i].y,
                colors[i].x, colors[i].y, colors[i].z
            };
        }

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);

        glBindVertexArray(vao);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     chunk.indices.size() * sizeof(uint32_t),
                     chunk.indices.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, px));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, cr));
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);

        center = chunk.center;
        key = chunk.key;
        enabled = chunk.enabled;
        indexCount = (uint32_t)chunk.indices.size();
    }

    void draw(GLuint prog, const glm::mat4& mvp) const {
        if (!vao || !enabled) return;
        glBindVertexArray(vao);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform3fv(glGetUniformLocation(prog, "uCenter"), 1, glm::value_ptr(glm::vec3(center)));
        glDrawElements(GL_TRIANGLES, (GLsizei)indexCount, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }
};

// ===========================================================================
// Main
// ===========================================================================
int main(int, char**) {
    // ---- SDL3 init ----
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow("Planet Demo", 1280, 720,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_GLContext glCtx = SDL_GL_CreateContext(win);
    if (!glCtx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(win, glCtx);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "gladLoadGLLoader failed\n"); return 1;
    }
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glClearColor(0.02f, 0.02f, 0.05f, 1.0f);

    // ---- ImGui init ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(win, glCtx);
    ImGui_ImplOpenGL3_Init("#version 460");

    // ---- Shaders ----
    GLuint vs = compileShader(kVertSrc, GL_VERTEX_SHADER);
    GLuint fs = compileShader(kFragSrc, GL_FRAGMENT_SHADER);
    GLuint prog = linkShaders(vs, fs);
    glDeleteShader(vs); glDeleteShader(fs);

    GLuint wireVs = compileShader(kWireVertSrc, GL_VERTEX_SHADER);
    GLuint wireFs = compileShader(kWireFragSrc, GL_FRAGMENT_SHADER);
    GLuint wireProg = linkShaders(wireVs, wireFs);
    glDeleteShader(wireVs); glDeleteShader(wireFs);

    // ---- Planet with Geology + Erosion + Biomes ----
    Haruka::Planet::GeologyConfig gcfg;
    gcfg.seed = 42;
    gcfg.numPlates = 12;
    auto geology = Haruka::Planet::generateGeology(gcfg);

    Haruka::Planet::ClimateOutput climate;
    Haruka::Planet::BiomesOutput biomes;

    // Build TerrainGrid from geology (pre-computed erosion, always-on)
    auto geoHeight = [&](const glm::dvec3& dir) -> float {
        return (float)geology.elevationModifier(dir);
    };
    Haruka::Planet::TerrainGridConfig tcfg;
    tcfg.faceRes = 256;
    tcfg.erosionIters = 5;
    Haruka::Planet::TerrainGrid terrainGrid;
    terrainGrid.build(geoHeight, tcfg);

    Haruka::Planet::Planet planet(6371.0, 16, 6);
    planet.setTerrainGrid(terrainGrid);
    planet.setDetailNoise(0.15f, 25.0f, 1.2f);

    // Generate initial LODs
    planet.generateLOD(0);
    planet.generateLOD(1);
    planet.generateLOD(2);
    planet.generateLOD(3);

    // Helper: compute biome color for a direction
    auto biomeColor = [&](const glm::dvec3& dir, float elevKm) -> glm::vec3 {
        int pid = geology.plateId(dir);
        bool isOcean = (pid >= 0) ? geology.plates[pid].oceanic : true;
        double temp = climate.temperature(dir, elevKm, isOcean);
        double prec = climate.precipitation(dir, elevKm, isOcean);
        double lat = std::asin(glm::clamp(dir.y, -1.0, 1.0));
        auto b = biomes.classify(elevKm, temp, prec, isOcean, lat);
        return biomes.color(b);
    };

    // Upload all chunks to GPU (with biome colors)
    std::map<uint64_t, GpuChunk> gpuChunks;
    planet.forEach([&](Haruka::Planet::Chunk& chunk) {
        // Compute biome colors for each vertex
        std::vector<glm::vec3> colors(chunk.positions.size());
        for (size_t i = 0; i < chunk.positions.size(); i++) {
            glm::dvec3 worldPos = chunk.center + glm::dvec3(chunk.positions[i]);
            glm::dvec3 dir = glm::normalize(worldPos);
            float elevKm = (float)(glm::length(worldPos) / 1000.0);
            colors[i] = biomeColor(dir, elevKm);
        }
        GpuChunk gc;
        gc.upload(chunk, colors);
        gpuChunks[chunk.key.hash()] = gc;
    });

    // ---- Camera ----
    OrbitCamera cam;
    cam.dist = planet.radius() / 1000.0 * 4.0f;

    // ---- State ----
    bool wireframeMode = false;
    bool showGrid = true;
    int currentLOD = 3;

    // ---- Main loop ----
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
            if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                cam.dist *= (e.wheel.y > 0) ? 0.9f : 1.1f;
            }
            if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
                cam.theta -= e.motion.xrel * 0.005f;
                cam.phi   += e.motion.yrel * 0.005f;
                cam.phi = glm::clamp(cam.phi, -1.2f, 1.2f);
            }
        }

        int winW, winH;
        SDL_GetWindowSize(win, &winW, &winH);
        float aspect = (float)winW / (float)winH;

        // ---- ImGui new frame ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ---- Render planet ----
        glViewport(0, 0, winW, winH);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glm::mat4 mvp = cam.viewProj(aspect);
        // Model matrix: planet centered at origin
        glm::mat4 model(1.0f);

        // Solid pass
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, glm::value_ptr(model));
        glUniform1i(glGetUniformLocation(prog, "uWireframe"), 0);

        for (auto& [h, gc] : gpuChunks) {
            if (!gc.enabled) continue;
            // Simple frustum culling by distance
            glm::dvec3 toChunk = gc.center - cam.target;
            double dist = glm::length(toChunk);
            double chunkSize = planet.radius() / 1000.0 * 0.3;
            if (dist > cam.dist * 1.5 + chunkSize) continue;

            glBindVertexArray(gc.vao);
            glUniform3fv(glGetUniformLocation(prog, "uCenter"), 1,
                         glm::value_ptr(glm::vec3(gc.center)));
            glDrawElements(GL_TRIANGLES, (GLsizei)gc.indexCount, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(0);
        }

        // Wireframe pass
        if (wireframeMode) {
            glUseProgram(wireProg);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (auto& [h, gc] : gpuChunks) {
                if (!gc.enabled) continue;
                glBindVertexArray(gc.vao);
                glUniformMatrix4fv(glGetUniformLocation(wireProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
                glUniform3fv(glGetUniformLocation(wireProg, "uCenter"), 1,
                             glm::value_ptr(glm::vec3(gc.center)));
                glDrawElements(GL_TRIANGLES, (GLsizei)gc.indexCount, GL_UNSIGNED_INT, nullptr);
                glBindVertexArray(0);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_BLEND);
        }

        // ---- ImGui ----
        ImGui::Begin("Planet Controls");

        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Wireframe", &wireframeMode);
            ImGui::Checkbox("Show grid", &showGrid);

            if (ImGui::SliderInt("LOD", &currentLOD, 0, planet.maxLOD())) {
                // Generate new LOD if needed
                for (int l = 0; l <= currentLOD; l++) {
                    planet.generateLOD(l);
                }
                // Upload new chunks with biome colors
                planet.forEach([&](Haruka::Planet::Chunk& chunk) {
                    uint64_t h = chunk.key.hash();
                    if (gpuChunks.find(h) == gpuChunks.end()) {
                        std::vector<glm::vec3> colors(chunk.positions.size());
                        for (size_t i = 0; i < chunk.positions.size(); i++) {
                            glm::dvec3 worldPos = chunk.center + glm::dvec3(chunk.positions[i]);
                            glm::dvec3 dir = glm::normalize(worldPos);
                            float elevKm = (float)(glm::length(worldPos) / 1000.0);
                            colors[i] = biomeColor(dir, elevKm);
                        }
                        GpuChunk gc;
                        gc.upload(chunk, colors);
                        gpuChunks[h] = gc;
                    } else {
                        gpuChunks[h].enabled = chunk.enabled;
                    }
                });
            }

            ImGui::Text("Radius: %.0f km", planet.radius());
            ImGui::Text("Chunks: %zu (enabled: %zu)",
                        planet.chunkCount(),
                        gpuChunks.size());
            ImGui::Text("Camera dist: %.1f km", cam.dist);
        }

        if (ImGui::CollapsingHeader("Chunks", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Click to toggle chunk on/off:");
            ImGui::Separator();
            ImGui::BeginChild("ChunkList", ImVec2(0, 400), true);

            int idx = 0;
            planet.forEach([&](Haruka::Planet::Chunk& chunk) {
                auto& gc = gpuChunks[chunk.key.hash()];
                bool enabled = gc.enabled;
                char label[64];
                snprintf(label, sizeof(label), "[%d] F%d L%d %d,%d %s",
                         idx, chunk.key.face, chunk.key.lod,
                         chunk.key.x, chunk.key.y,
                         enabled ? "ON" : "OFF");

                if (enabled)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 1, 0, 1));
                else
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));

                if (ImGui::Selectable(label, false)) {
                    gc.enabled = !gc.enabled;
                    planet.setEnabled(chunk.key, gc.enabled);
                }
                ImGui::PopStyleColor();
                idx++;
            });

            ImGui::EndChild();
        }

        if (ImGui::Button("Enable All")) {
            planet.forEach([&](Haruka::Planet::Chunk& chunk) {
                chunk.enabled = true;
                gpuChunks[chunk.key.hash()].enabled = true;
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Disable All")) {
            planet.forEach([&](Haruka::Planet::Chunk& chunk) {
                chunk.enabled = false;
                gpuChunks[chunk.key.hash()].enabled = false;
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Regenerate")) {
            // New geology seed → rebuild everything
            static unsigned geoSeed = 100;
            geoSeed++;
            gcfg.seed = geoSeed;
            geology = Haruka::Planet::generateGeology(gcfg);

            auto newGeoHeight = [&](const glm::dvec3& dir) -> float {
                return (float)geology.elevationModifier(dir);
            };
            terrainGrid.build(newGeoHeight, tcfg);

            planet = Haruka::Planet::Planet(6371.0, 16, 6);
            planet.setTerrainGrid(terrainGrid);
            planet.setDetailNoise(0.15f, 25.0f, 1.2f);
            planet.generateLOD(currentLOD);
            gpuChunks.clear();
            planet.forEach([&](Haruka::Planet::Chunk& chunk) {
                std::vector<glm::vec3> colors(chunk.positions.size());
                for (size_t i = 0; i < chunk.positions.size(); i++) {
                    glm::dvec3 worldPos = chunk.center + glm::dvec3(chunk.positions[i]);
                    glm::dvec3 dir = glm::normalize(worldPos);
                    float elevKm = (float)(glm::length(worldPos) / 1000.0);
                    colors[i] = biomeColor(dir, elevKm);
                }
                GpuChunk gc;
                gc.upload(chunk, colors);
                gpuChunks[chunk.key.hash()] = gc;
            });
        }

        ImGui::End();

        // ---- ImGui render ----
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(win);
    }

    // ---- Cleanup ----
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(glCtx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
