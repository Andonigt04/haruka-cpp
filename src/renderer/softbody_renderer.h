/**
 * @file softbody_renderer.h
 * @brief Renders XPBD softbodies by streaming particle positions into a dynamic
 *        VBO each frame (Fase B). Self-contained (own VAO/VBO/shader), drawn from
 *        the game's onRenderWorld hook — decoupled like the water/terrain renderers.
 *
 * Positions are camera-relative (particle.origin - cameraPos + localPos), so the
 * vertex shader only needs view-rotation + projection, matching the planet/water
 * convention and staying float-safe at planetary distances.
 */
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include "physics/xpbd/softbody.h"
#include "tools/math_types.h"

class Shader;

namespace Haruka {

class SoftBodyRenderer {
public:
    SoftBodyRenderer() = default;
    ~SoftBodyRenderer();

    /** @brief Registers a softbody (handle + which solver owns its particles). */
    void add(const xpbd::SoftBodyHandle& handle, xpbd::XPBDSolver* solver,
             const glm::vec3& color = glm::vec3(0.8f, 0.2f, 0.25f));

    /** @brief Uploads current particle positions and draws all softbodies.
     *  Expects view/projection already available via the per-frame UBO (binding 0). */
    void render(const Haruka::WorldPos& cameraPos);

    void clear();

private:
    struct Entry {
        xpbd::SoftBodyHandle handle;
        xpbd::XPBDSolver*    solver = nullptr;
        glm::vec3            color{0.8f};
        GLuint vao = 0, vbo = 0, ebo = 0;
        std::vector<float> cpuVerts; // interleaved pos(3)+normal(3)
        bool initialized = false;
    };
    std::vector<Entry> m_entries;
    std::unique_ptr<Shader> m_shader;

    void ensureShader();
    void uploadEntry(Entry& e, const Haruka::WorldPos& cameraPos);
};

} // namespace Haruka
