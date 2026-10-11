#pragma once

// Internal helpers shared between the Application translation units
// (Application.cpp / ApplicationRender.cpp / ApplicationAssets.cpp).
// These were a per-TU anonymous namespace when Application lived in a single
// file; split across files they need external linkage, so they live in a
// named detail namespace instead. Not part of the public engine API.

#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/Tools/MathTypes.h"                 // Haruka::Tools::WorldPos
#include "Haruka/Core/Scene/SceneRenderPolicy.h"   // RenderCommand / RenderKind / PrimitiveType
#include "Haruka/RHI/RHITypes.h"                    // Haruka::RHI::TextureHandle

namespace Haruka { namespace Renderer { class Model; class Mesh; } } // Haruka::Renderer::*
namespace Haruka::Core::Scene { struct SceneObject; }

namespace Haruka::Core::AppInternal {

// Frame render queue: filled by Application::buildRenderQueue (render TU),
// consumed by renderFrameContent (render TU) and cleared by cleanup (core TU).
extern std::vector<Haruka::Core::Scene::RenderCommand> g_sceneRenderQueue;

// Camera-relative model matrix in double precision (no jitter at planetary range).
glm::mat4 getTransformMatrix(const Haruka::Core::Scene::SceneObject& obj, const Haruka::Tools::WorldPos& camPos);
/// Un PERSONAJE: de pie sobre la vertical del planeta (`up`), girado su `rotation.y` alrededor de
/// ella, con la base (el pie) en `obj.position`. La matriz general aplica Euler XYZ en el marco del
/// mundo: a cualquier latitud que no sea el ecuador el personaje salia tumbado.
glm::mat4 getCharacterTransform(const Haruka::Core::Scene::SceneObject& obj, const Haruka::Tools::WorldPos& camPos, const glm::dvec3& up);

// Lazily-loaded, cached model by path (nullptr for empty path / load failure).
Haruka::Renderer::Model* getOrLoadModelCached(const std::string& path);

// Lazily-built shared primitive mesh for a primitive kind (nullptr if unknown).
Renderer::Mesh* getPrimitiveMesh(Haruka::Tools::PrimitiveType primitive);

// Texture of a MaterialComponent slot, by path. Cached (failures included → un PNG que falta
// no se reintenta cada frame). Handle inválido si no se pudo cargar.
Haruka::RHI::TextureHandle getOrLoadMaterialTexture(const std::string& path);

// Releases every GL-owning cache (models + primitive meshes) before the context
// is destroyed. Must run while the GL context is still current.
void cleanupGLStatics();

} // namespace Haruka::Core::AppInternal

