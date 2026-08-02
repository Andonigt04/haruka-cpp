#ifndef APPLICATION_INTERNAL_H
#define APPLICATION_INTERNAL_H

// Internal helpers shared between the Application translation units
// (application.cpp / application_render.cpp / application_assets.cpp).
// These were a per-TU anonymous namespace when Application lived in a single
// file; split across files they need external linkage, so they live in a
// named detail namespace instead. Not part of the public engine API.

#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "tools/math_types.h"                 // Haruka::WorldPos
#include "core/scene/scene_render_policy.h"   // RenderCommand / RenderKind / PrimitiveType
#include "rhi/rhi_types.h"                    // Haruka::RHI::TextureHandle

namespace Haruka { namespace Renderer { class Model; class Mesh; } } // Haruka::Renderer::*
using Haruka::Renderer::Model;                            // back-compat aliases (migration)
using Haruka::Renderer::Mesh;
using SimpleMesh = Mesh;   // matches renderer/simple_mesh.h (SimpleMesh is an alias, not a class)
namespace Haruka { struct SceneObject; }

namespace AppInternal {

// Frame render queue: filled by Application::buildRenderQueue (render TU),
// consumed by renderFrameContent (render TU) and cleared by cleanup (core TU).
extern std::vector<Haruka::RenderCommand> g_sceneRenderQueue;

// Camera-relative model matrix in double precision (no jitter at planetary range).
glm::mat4 getTransformMatrix(const Haruka::SceneObject& obj, const Haruka::WorldPos& camPos);

// Lazily-loaded, cached model by path (nullptr for empty path / load failure).
Model* getOrLoadModelCached(const std::string& path);

// Lazily-built shared primitive mesh for a primitive kind (nullptr if unknown).
SimpleMesh* getPrimitiveMesh(Haruka::PrimitiveType primitive);

// Texture of a MaterialComponent slot, by path. Cached (failures included → un PNG que falta
// no se reintenta cada frame). Handle inválido si no se pudo cargar.
Haruka::RHI::TextureHandle getOrLoadMaterialTexture(const std::string& path);

// Releases every GL-owning cache (models + primitive meshes) before the context
// is destroyed. Must run while the GL context is still current.
void cleanupGLStatics();

} // namespace AppInternal

#endif // APPLICATION_INTERNAL_H
