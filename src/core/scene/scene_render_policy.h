#pragma once

#include <vector>
#include <memory>

#include "scene_manager.h"
#include "tools/object_types.h"

namespace Haruka {

enum class RenderKind {
    None,
    Model,
    MeshComponent,
    Primitive
};

struct RenderCommand {
    const SceneObject* object = nullptr;
    RenderKind kind = RenderKind::None;
    PrimitiveType primitive = PrimitiveType::NONE;
};

std::vector<RenderCommand> buildSceneRenderQueue(const SceneManager& scene);

}