#include "scene_render_policy.h"

#include <algorithm>
#include <cctype>

#include "core/components/mesh_renderer_component.h"

namespace {

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsInsensitive(const std::string& value, const std::string& needle) {
    return toLowerCopy(value).find(toLowerCopy(needle)) != std::string::npos;
}

bool hasVisualMeshPrimitive(const Haruka::SceneObject& obj, Haruka::PrimitiveType& primitive) {
    if (!obj.components.is_object() || !obj.components.contains("visual") || !obj.components["visual"].is_object()) {
        return false;
    }

    const auto& visual = obj.components["visual"];
    if (!visual.contains("mesh") || !visual["mesh"].is_string()) {
        return false;
    }

    const std::string meshName = visual["mesh"].get<std::string>();
    const auto lower = toLowerCopy(meshName);

    if (lower.find("sphere") != std::string::npos) {
        primitive = Haruka::PrimitiveType::SPHERE;
        return true;
    }
    if (lower.find("cube") != std::string::npos) {
        primitive = Haruka::PrimitiveType::CUBE;
        return true;
    }
    if (lower.find("capsule") != std::string::npos) {
        primitive = Haruka::PrimitiveType::CAPSULE;
        return true;
    }
    if (lower.find("plane") != std::string::npos) {
        primitive = Haruka::PrimitiveType::PLANE;
        return true;
    }

    return false;
}

Haruka::RenderCommand classifyObject(const Haruka::SceneObject& obj) {
    Haruka::RenderCommand command;
    command.object = &obj;

    const std::string typeLower = toLowerCopy(obj.type);
    if (typeLower == "camera" || typeLower == "empty") {
        return command;
    }

    if (obj.meshRenderer && obj.meshRenderer->isResident()) {
        command.kind = Haruka::RenderKind::MeshComponent;
        return command;
    }

    if (!obj.modelPath.empty()) {
        command.kind = Haruka::RenderKind::Model;
        return command;
    }

    Haruka::PrimitiveType primitive = Haruka::PrimitiveType::NONE;
    if (hasVisualMeshPrimitive(obj, primitive)) {
        command.kind = Haruka::RenderKind::Primitive;
        command.primitive = primitive;
        return command;
    }

    if (obj.terrainSettings || obj.lodSettings || obj.flags.hasChunks || containsInsensitive(typeLower, "planet") || containsInsensitive(typeLower, "celestialbody") || containsInsensitive(typeLower, "satellite")) {
        // Terrain-streamed bodies: skip primitive sphere — TerrainRenderer supplies the geometry.
        return command; // RenderKind::None
    }

    if (containsInsensitive(typeLower, "star")) {
        command.kind = Haruka::RenderKind::Primitive;
        command.primitive = Haruka::PrimitiveType::SPHERE;
        return command;
    }

    if (containsInsensitive(typeLower, "spacecraft") || containsInsensitive(typeLower, "character")) {
        command.kind = Haruka::RenderKind::Primitive;
        command.primitive = Haruka::PrimitiveType::CAPSULE;
        return command;
    }

    if (containsInsensitive(typeLower, "light") || obj.flags.castLight) {
        command.kind = Haruka::RenderKind::Primitive;
        command.primitive = Haruka::PrimitiveType::SPHERE;
        return command;
    }

    return command;
}

} // namespace

namespace Haruka {

std::vector<RenderCommand> buildSceneRenderQueue(const SceneManager& scene) {
    std::vector<RenderCommand> commands;
    const auto& objects = scene.getAllObjects();
    commands.reserve(objects.size());

    for (const auto& obj : objects) {
        if (!obj) continue;

        RenderCommand command = classifyObject(*obj);
        if (command.kind != RenderKind::None) {
            commands.push_back(command);
        }
    }

    return commands;
}

} // namespace Haruka