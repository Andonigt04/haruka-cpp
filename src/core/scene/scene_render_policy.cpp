#include "scene_render_policy.h"

#include <algorithm>
#include <cctype>

#include "core/components/mesh_renderer_component.h"

namespace {

// Comparaciones SIN reservar memoria. Antes cada una creaba una copia en minúsculas: la cola se
// reconstruye cada vez que cambia el nº de objetos (y con escombros apareciendo y caducando eso es
// constante), así que eran decenas de miles de cadenas nuevas por reconstrucción, para nada.
inline char lowerC(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool iEquals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) if (lowerC(a[i]) != lowerC(b[i])) return false;
    return i == a.size() && b[i] == '\0';
}

bool containsInsensitive(const std::string& value, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > value.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= value.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lowerC(value[i + k]) == lowerC(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

bool iContains(const std::string& value, const char* needle) {
    return containsInsensitive(value, std::string(needle));
}

bool hasVisualMeshPrimitive(const Haruka::SceneObject& obj, Haruka::PrimitiveType& primitive) {
    if (!obj.components.is_object() || !obj.components.contains("visual") || !obj.components["visual"].is_object()) {
        return false;
    }

    const auto& visual = obj.components["visual"];
    if (!visual.contains("mesh") || !visual["mesh"].is_string()) {
        return false;
    }

    const std::string& meshName = visual["mesh"].get_ref<const std::string&>();

    if (iContains(meshName, "sphere")) {
        primitive = Haruka::PrimitiveType::SPHERE;
        return true;
    }
    if (iContains(meshName, "cube")) {
        primitive = Haruka::PrimitiveType::CUBE;
        return true;
    }
    if (iContains(meshName, "capsule")) {
        primitive = Haruka::PrimitiveType::CAPSULE;
        return true;
    }
    if (iContains(meshName, "plane")) {
        primitive = Haruka::PrimitiveType::PLANE;
        return true;
    }
    if (iContains(meshName, "cylinder") || iContains(meshName, "cilinder")) {
        primitive = Haruka::PrimitiveType::CILINDER;
        return true;
    }
    if (iContains(meshName, "triangle")) {
        primitive = Haruka::PrimitiveType::TRIANGLE;
        return true;
    }

    return false;
}

} // namespace

namespace Haruka {
RenderCommand classifySceneObject(const Haruka::SceneObject& obj) {
    Haruka::RenderCommand command;
    command.object = &obj;
    // Se resuelve AQUÍ (una vez por objeto, no una vez por frame): las piezas de construcción y de
    // edificio van al pase instanciado. Consultar este mapa JSON en el bucle de dibujo costaba dos
    // búsquedas por cadena × objeto × frame.
    command.instanced = obj.properties.is_object() &&
                        (obj.properties.value("construction", false) ||
                         obj.properties.value("building", false));

    // Dispatch rápido por ObjectType enum (evita substring matching)
    switch (obj.objectType) {
        case ObjectType::CAMERA:
        case ObjectType::EMPTY:
            return command;

        case ObjectType::MESH:
            if (obj.meshRenderer && obj.meshRenderer->isResident()) {
                command.kind = Haruka::RenderKind::MeshComponent;
            } else {
                Haruka::PrimitiveType p = Haruka::PrimitiveType::NONE;
                if (hasVisualMeshPrimitive(obj, p)) {
                    command.kind = Haruka::RenderKind::Primitive;
                    command.primitive = p;
                }
            }
            return command;

        case ObjectType::MODEL:
            // ⚠️ A MODEL WITH NO MODEL DRAWS NOTHING, AND SAYS NOTHING. The draw case does
            // `getOrLoadModelCached(obj->modelPath)` and `break`s on null, so an object with an empty
            // path is added to the scene, moved every frame, culled, counted — and never appears.
            // That is exactly how the networked players were invisible before (they classified as
            // UNKNOWN, same silence, different door), and every entity the world feed spawns that is
            // not a player or an NPC still arrives here with no path at all.
            //
            // A capsule is not a nice model. It is a VISIBLE one: something standing where the server
            // says something is standing beats an empty patch of ground that looks exactly like a
            // working game. Anything that has a path keeps going to the model renderer untouched.
            if (obj.modelPath.empty() && !(obj.meshRenderer && obj.meshRenderer->isResident())) {
                command.kind = Haruka::RenderKind::Primitive;
                command.primitive = Haruka::PrimitiveType::CAPSULE;
                return command;
            }
            command.kind = Haruka::RenderKind::Model;
            return command;

        case ObjectType::PLANET:
        case ObjectType::SATELLITE:
            // Cuerpos con terreno: la geometría la suministra el TerrainRenderer.
            // Un cuerpo LUMINOSO (suelta/emisor, p.ej. el Sol con `castLight`) NO tiene terreno:
            // se dibuja como esfera emissiva en el pase normal (estilo del sol anime).
            if (obj.flags.castLight) {
                command.kind = Haruka::RenderKind::Primitive;
                command.primitive = Haruka::PrimitiveType::SPHERE;
            }
            return command;

        case ObjectType::STAR:
            command.kind = Haruka::RenderKind::Primitive;
            command.primitive = Haruka::PrimitiveType::SPHERE;
            return command;

        case ObjectType::CHARACTER:
            command.kind = Haruka::RenderKind::Primitive;
            command.primitive = Haruka::PrimitiveType::CAPSULE;
            return command;

        case ObjectType::LIGHT:
        case ObjectType::DIRECTIONAL_LIGHT:
        case ObjectType::SPOTLIGHT:
            command.kind = Haruka::RenderKind::Primitive;
            command.primitive = Haruka::PrimitiveType::SPHERE;
            return command;

        default:
            break;
    }

    // Objetos con ObjectType::CUSTOM o UNKNOWN: resuelve por heurísticas legacy
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
    if (obj.flags.castLight) {
        command.kind = Haruka::RenderKind::Primitive;
        command.primitive = Haruka::PrimitiveType::SPHERE;
        return command;
    }

    return command;
}

std::vector<RenderCommand> buildSceneRenderQueue(const SceneManager& scene) {
    std::vector<RenderCommand> commands;
    const auto& objects = scene.getAllObjects();
    commands.reserve(objects.size());

    for (const auto& obj : objects) {
        if (!obj) continue;

        RenderCommand command = classifySceneObject(*obj);
        if (command.kind != RenderKind::None) {
            commands.push_back(command);
        }
    }

    return commands;
}

} // namespace Haruka