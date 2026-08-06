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
#include "game/planetary_system.h"      // radio real de un planeta (getObjectBoundingRadius)
#include "core/asset_paths.h"
#include "core/logger.h"
#include "rhi/rhi_device.h"
#include <stb_image.h>
#include <cstdio>
#include <algorithm>
#include <cmath>

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

// Texturas de MATERIAL por ruta. Cachea también los FALLOS (handle inválido): sin eso, un
// material que apunta a un PNG que no existe reintentaría el stbi_load de cada objeto y de
// cada frame — un fallo de carga se paga una vez, no 60 veces por segundo.
std::unordered_map<std::string, Haruka::RHI::TextureHandle> g_materialTexCache;
} // namespace

void cleanupGLStatics() {
    if (Haruka::RHI::Device* dev = Haruka::RHI::device()) {
        for (auto& [path, tex] : g_materialTexCache)
            if (Haruka::RHI::valid(tex)) dev->destroy(tex);
    }
    g_materialTexCache.clear();
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

    // Orientación EXACTA si el objeto la trae como cuaternión: el rodeo por Euler se degrada en la
    // singularidad del cardán (una pieza con una orientación perfectamente válida sale girada al azar).
    glm::mat4 rotation = obj.useOrientation
        ? glm::mat4_cast(glm::quat(obj.orientation))
        : glm::eulerAngleXYZ(
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

Haruka::RHI::TextureHandle getOrLoadMaterialTexture(const std::string& path) {
    using namespace Haruka;
    if (path.empty()) return {};
    auto it = g_materialTexCache.find(path);
    if (it != g_materialTexCache.end()) return it->second;   // incluido el fallo cacheado

    RHI::Device* dev = RHI::device();
    if (!dev) return {};

    // Las rutas de material vienen del JSON de la escena y las escribe el IDE como
    // "assets/textures/<obj>_<slot>.png" (relativa a la raíz del proyecto). Se prueban las
    // formas en las que puede llegar antes de darla por perdida: tal cual (cwd), contra la
    // raíz de assets del MOTOR (el IDE la fija a la suya) y contra la del PROYECTO abierto.
    const std::string candidates[] = {
        path,
        AssetPaths::assetsDir() + path,
        AssetPaths::textures() + path,
        AssetPaths::projectRoot() + path,
    };
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = nullptr;
    for (const std::string& candidate : candidates) {
        if (candidate.empty()) continue;
        pixels = stbi_load(candidate.c_str(), &w, &h, &n, 4);
        if (pixels) break;
    }
    if (!pixels) {
        HARUKA_LOGW("Material", "textura no encontrada: %s", path.c_str());
        g_materialTexCache.emplace(path, RHI::TextureHandle{});
        return {};
    }

    RHI::TextureDesc td;
    td.width = (uint32_t)w; td.height = (uint32_t)h;
    td.format = RHI::Format::RGBA8;
    td.mipmaps = true;
    td.maxAnisotropy = 8.0f;
    td.initialData = pixels;
    RHI::TextureHandle handle = dev->createTexture(td);
    stbi_image_free(pixels);

    g_materialTexCache.emplace(path, handle);
    return handle;
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
Haruka::Renderer::Model* Application::getModel(const std::string& path) {
    return AppInternal::getOrLoadModelCached(path);   // misma caché perezosa que getModelBounds
}

bool Application::getModelBounds(const std::string& path, glm::vec3& outMin, glm::vec3& outMax) {
    Model* m = AppInternal::getOrLoadModelCached(path);
    if (!m || !m->hasBounds()) return false;
    outMin = m->boundsMin();
    outMax = m->boundsMax();
    return true;
}

double Application::getObjectBoundingRadius(const Haruka::SceneObject& obj) {
    const double maxScale = std::max({ std::abs(obj.scale.x),
                                       std::abs(obj.scale.y),
                                       std::abs(obj.scale.z) });

    // 1. ¿Es un planeta? Lo sabe el PlanetarySystem, no el objeto: en el JSON de escena un planeta
    //    es un SceneObject más y su radio vive en la configuración que interpreta el motor.
    if (_planetarySystem) {
        for (const auto& pl : _planetarySystem->getPlanets())
            if (pl.name == obj.name) return pl.radius;
        for (size_t i = 0; i < _planetarySystem->getSimplePlanetCount(); ++i) {
            const auto& sp = _planetarySystem->getSimplePlanet(i);
            if (sp.name == obj.name) return sp.radius;
        }
    }

    // 2. ¿Tiene modelo? Media diagonal de su AABB local, escalada.
    if (!obj.modelPath.empty()) {
        glm::vec3 mn, mx;
        if (getModelBounds(obj.modelPath, mn, mx)) {
            const double halfDiag = glm::length(glm::dvec3(mx) - glm::dvec3(mn)) * 0.5;
            if (halfDiag > 0.0) return halfDiag * maxScale;
        }
    }

    // 3. Primitivas y objetos sin geometría (luces, spawns, entidades). `PrimitiveShapes` las genera
    //    de tamaño UNIDAD centradas en el origen → media diagonal del cubo unidad = √3/2.
    const double kUnitPrimitiveRadius = 0.8660254;
    const double r = kUnitPrimitiveRadius * maxScale;
    return (r > 1e-6) ? r : kUnitPrimitiveRadius;   // nunca 0: sería la cámara DENTRO del objeto
}
}} // namespace Haruka::Core
