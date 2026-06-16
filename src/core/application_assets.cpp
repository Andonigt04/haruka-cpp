// Application — asset/GL caches.
// Owns the model cache and the shared primitive meshes (the GL-owning statics
// that used to be a per-TU anonymous namespace), plus the camera-relative
// transform helper and the model-bounds query. Kept apart from rendering and
// lifecycle so those files read as orchestration, not asset bookkeeping.

#include "application.h"
#include "application_internal.h"

#include <unordered_map>
#include <memory>

#include <glm/gtx/euler_angles.hpp>

#include "renderer/model.h"
#include "renderer/simple_mesh.h"
#include "renderer/primitive_shapes.h"
#include "core/scene/scene_manager.h"   // Haruka::SceneObject

namespace AppInternal {

std::vector<Haruka::RenderCommand> g_sceneRenderQueue;

namespace {
// GL-owning caches at namespace scope so cleanupGLStatics() can reset them
// before the GL context is destroyed. Function-local statics would destruct
// at program exit (after main() returns), which is after the context is gone.
std::unordered_map<std::string, std::shared_ptr<Model>> g_modelCache;
std::unique_ptr<SimpleMesh> g_sphereMesh;
std::unique_ptr<SimpleMesh> g_cubeMesh;
std::unique_ptr<SimpleMesh> g_capsuleMesh;
std::unique_ptr<SimpleMesh> g_planeMesh;
std::unique_ptr<SimpleMesh> g_cylinderMesh;
std::unique_ptr<SimpleMesh> g_triangleMesh;
} // namespace

void cleanupGLStatics() {
    g_modelCache.clear();
    g_sphereMesh.reset();
    g_cubeMesh.reset();
    g_capsuleMesh.reset();
    g_planeMesh.reset();
    g_cylinderMesh.reset();
    g_triangleMesh.reset();
}

glm::mat4 getTransformMatrix(const Haruka::SceneObject& obj, const Haruka::WorldPos& camPos) {
    // Camera-relative in DOUBLE precision: the position-minus-camera subtraction is done
    // in double and ONLY the offset (small near the camera) is cast to float. Avoids the
    // ~0.5 m jitter that appeared when casting surface positions (millions of m) to float.
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(obj.position - camPos));

    glm::mat4 rotation = glm::eulerAngleXYZ(
        glm::radians(static_cast<float>(obj.rotation.x)),
        glm::radians(static_cast<float>(obj.rotation.y)),
        glm::radians(static_cast<float>(obj.rotation.z))
    );

    transform *= rotation;
    return glm::scale(transform, glm::vec3(obj.scale));
}

Model* getOrLoadModelCached(const std::string& path) {
    if (path.empty()) return nullptr;   // item sin modelo (p.ej. minerales) → no cargar ""
    auto it = g_modelCache.find(path);
    if (it != g_modelCache.end()) {
        return it->second.get();
    }

    auto model = std::make_shared<Model>(path);
    Model* modelPtr = model.get();
    g_modelCache.emplace(path, std::move(model));
    return modelPtr;
}

SimpleMesh* getPrimitiveMesh(Haruka::PrimitiveType primitive) {
    switch (primitive) {
        case Haruka::PrimitiveType::CUBE:
            if (!g_cubeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCube(1.0f, vertices, normals, indices);
                g_cubeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_cubeMesh.get();
        case Haruka::PrimitiveType::SPHERE:
            if (!g_sphereMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createSphereLOD(1.0f, 24, 16, vertices, normals, indices);
                g_sphereMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_sphereMesh.get();
        case Haruka::PrimitiveType::CAPSULE:
            if (!g_capsuleMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCapsule(0.5f, 1.5f, 24, 12, vertices, normals, indices);
                g_capsuleMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_capsuleMesh.get();
        case Haruka::PrimitiveType::PLANE:
            if (!g_planeMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createPlane(1.0f, 1.0f, 1, vertices, normals, indices);
                g_planeMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_planeMesh.get();
        case Haruka::PrimitiveType::CILINDER:
            if (!g_cylinderMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createCylinder(0.5f, 1.0f, 24, vertices, normals, indices);
                g_cylinderMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_cylinderMesh.get();
        case Haruka::PrimitiveType::TRIANGLE:
            if (!g_triangleMesh) {
                std::vector<glm::vec3> vertices;
                std::vector<glm::vec3> normals;
                std::vector<unsigned int> indices;
                PrimitiveShapes::createTriangle(0.5f, vertices, normals, indices);
                g_triangleMesh = std::make_unique<SimpleMesh>(vertices, normals, indices);
            }
            return g_triangleMesh.get();
        default:
            return nullptr;
    }
}

} // namespace AppInternal

// Model bounds (AABB) — an ASSET query (Application owns the model cache).
// Not collision logic: the physics module uses these bounds for its collider.
namespace Haruka { namespace Core {
bool Application::getModelBounds(const std::string& path, glm::vec3& outMin, glm::vec3& outMax) {
    Model* m = AppInternal::getOrLoadModelCached(path);
    if (!m || !m->hasBounds()) return false;
    outMin = m->boundsMin();
    outMax = m->boundsMax();
    return true;
}
}} // namespace Haruka::Core
