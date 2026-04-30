#define GLM_ENABLE_EXPERIMENTAL
#include "scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <dlfcn.h>
#include "core/game_interface.h"
#include "renderer/primitive_shapes.h"
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace Haruka {

namespace {
template <typename Vec3T>
nlohmann::json vec3ArrayToJson(const std::vector<Vec3T>& values) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& value : values) {
        result.push_back({value.x, value.y, value.z});
    }
    return result;
}

std::vector<glm::vec3> jsonToVec3Array(const nlohmann::json& j) {
    std::vector<glm::vec3> values;
    if (!j.is_array()) return values;
    values.reserve(j.size());
    for (const auto& item : j) {
        if (item.is_array() && item.size() == 3) {
            values.emplace_back(item[0].get<float>(), item[1].get<float>(), item[2].get<float>());
        }
    }
    return values;
}

std::vector<unsigned int> jsonToIndexArray(const nlohmann::json& j) {
    std::vector<unsigned int> values;
    if (!j.is_array()) return values;
    values.reserve(j.size());
    for (const auto& item : j) {
        values.push_back(item.get<unsigned int>());
    }
    return values;
}

glm::mat4 composeLocalTransform(const glm::dvec3& position, const glm::dvec3& rotation, const glm::dvec3& scale) {
    glm::mat4 transform(1.0f);
    transform = glm::translate(transform, glm::vec3(position));
    transform = glm::rotate(transform, glm::radians((float)rotation.x), glm::vec3(1, 0, 0));
    transform = glm::rotate(transform, glm::radians((float)rotation.y), glm::vec3(0, 1, 0));
    transform = glm::rotate(transform, glm::radians((float)rotation.z), glm::vec3(0, 0, 1));
    transform = glm::scale(transform, glm::vec3(scale));
    return transform;
}

void decomposeTransform(const glm::mat4& transform, glm::dvec3& position, glm::dvec3& rotation, glm::dvec3& scale) {
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::vec3 translation;
    glm::quat orientation;
    glm::vec3 localScale;

    glm::decompose(transform, localScale, orientation, translation, skew, perspective);
    position = glm::dvec3(translation);
    scale = glm::dvec3(localScale);

    glm::vec3 euler = glm::degrees(glm::eulerAngles(orientation));
    rotation = glm::dvec3(euler);
}
}

Scene::Scene() : sceneName("Untitled") {}

Scene::Scene(const std::string& name) : sceneName(name) {}

Scene::~Scene() {}

void Scene::addObject(const SceneObject& obj) {
    objects.push_back(obj);
    if (obj.name.find("_chunk_") == std::string::npos) {
        std::cout << "Object added: " << obj.name << std::endl;
    }
}

void Scene::removeObject(const std::string& name) {
    auto it = std::find_if(objects.begin(), objects.end(),
        [&name](const SceneObject& o) { return o.name == name; });
    if (it != objects.end()) {
        objects.erase(it);
        if (name.find("_chunk_") == std::string::npos) {
            std::cout << "Object removed: " << name << std::endl;
        }
    }
}

SceneObject* Scene::getObject(const std::string& name) {
    auto it = std::find_if(objects.begin(), objects.end(),
        [&name](const SceneObject& o) { return o.name == name; });
    if (it != objects.end()) {
        return &(*it);
    }
    return nullptr;
}

glm::mat4 SceneObject::getWorldTransform(const Scene* scene) const {
    glm::mat4 localTransform = composeLocalTransform(position, rotation, scale);
    if (!scene || parentIndex < 0 || parentIndex >= (int)scene->getObjects().size()) {
        return localTransform;
    }

    const auto& parent = scene->getObjects()[parentIndex];
    return parent.getWorldTransform(scene) * localTransform;
}

glm::dvec3 SceneObject::getWorldPosition(const Scene* scene) const {
    glm::mat4 world = getWorldTransform(scene);
    glm::vec4 origin = world * glm::vec4(0, 0, 0, 1);
    return glm::dvec3(origin.x, origin.y, origin.z);
}

glm::dvec3 SceneObject::getWorldRotation(const Scene* scene) const {
    glm::dvec3 worldPosition, worldRotation, worldScale;
    glm::mat4 world = getWorldTransform(scene);
    decomposeTransform(world, worldPosition, worldRotation, worldScale);
    return worldRotation;
}

glm::dvec3 SceneObject::getWorldScale(const Scene* scene) const {
    glm::dvec3 worldPosition, worldRotation, worldScale;
    glm::mat4 world = getWorldTransform(scene);
    decomposeTransform(world, worldPosition, worldRotation, worldScale);
    return worldScale;
}

bool Scene::save(const std::string& filepath) {
    try {
        std::filesystem::path p(filepath);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        // Detectar si es prefab por extensión
        std::string ext = filepath.substr(filepath.find_last_of(".") + 1);
        bool isPrefab = (ext == "prefab");

        nlohmann::json j;
        j["name"] = sceneName;
        
        if (isPrefab) {
            // Guardar como prefab con initializer
            j["initializer"] = initializerPath;
            j["components"] = nlohmann::json::array();
            for (const auto& obj : objects) {
                nlohmann::json comp;
                comp["name"] = obj.name;
                comp["type"] = obj.type;
                comp["modelPath"] = obj.modelPath;
                comp["position"] = {obj.position.x, obj.position.y, obj.position.z};
                comp["rotation"] = {obj.rotation.x, obj.rotation.y, obj.rotation.z};
                comp["scale"]    = {obj.scale.x, obj.scale.y, obj.scale.z};
                comp["color"]    = {obj.color.x, obj.color.y, obj.color.z};
                comp["intensity"] = obj.intensity;
                comp["renderLayer"] = obj.renderLayer;
                if (!obj.properties.is_null() && !obj.properties.empty()) {
                    comp["properties"] = obj.properties;
                }
                if (obj.meshRenderer) {
                    nlohmann::json mesh;
                    mesh["vertices"] = vec3ArrayToJson(obj.meshRenderer->getSourceVertices());
                    mesh["normals"] = vec3ArrayToJson(obj.meshRenderer->getSourceNormals());
                    mesh["indices"] = obj.meshRenderer->getSourceIndices();
                    comp["meshRenderer"] = mesh;
                }
                
                if (obj.material) {
                    comp["material"] = obj.material->toJSON();
                }
                j["components"].push_back(comp);
            }
        } else {
            // Guardar como escena normal
            j["objects"] = nlohmann::json::array();
            for (const auto& obj : objects) {
                nlohmann::json o;
                o["name"] = obj.name;
                o["type"] = obj.type;
                o["modelPath"] = obj.modelPath;
                o["position"] = {obj.position.x, obj.position.y, obj.position.z};
                o["rotation"] = {obj.rotation.x, obj.rotation.y, obj.rotation.z};
                o["scale"]    = {obj.scale.x, obj.scale.y, obj.scale.z};
                o["color"]    = {obj.color.x, obj.color.y, obj.color.z};
                o["intensity"] = obj.intensity;
                o["renderLayer"] = obj.renderLayer;
                o["parentIndex"] = obj.parentIndex;
                o["childrenIndices"] = obj.childrenIndices;
                if (!obj.properties.is_null() && !obj.properties.empty()) {
                    o["properties"] = obj.properties;
                }

                if (obj.material) {
                    o["material"] = obj.material->toJSON();
                }

                if (obj.meshRenderer) {
                    nlohmann::json mesh;
                    mesh["vertices"] = vec3ArrayToJson(obj.meshRenderer->getSourceVertices());
                    mesh["normals"] = vec3ArrayToJson(obj.meshRenderer->getSourceNormals());
                    mesh["indices"] = obj.meshRenderer->getSourceIndices();
                    o["meshRenderer"] = mesh;
                }

                j["objects"].push_back(o);
            }
        }

        std::ofstream out(filepath, std::ios::trunc);
        if (!out.is_open()) return false;
        out << j.dump(4);
        return out.good();
    } catch (...) {
        return false;
    }
}

bool Scene::load(const std::string& filepath) {
    try {
        std::ifstream in(filepath);
        if (!in.is_open()) return false;

        nlohmann::json j;
        in >> j;

        if (j.contains("name")) sceneName = j["name"].get<std::string>();
        if (j.contains("initializer")) initializerPath = j["initializer"].get<std::string>();
        objects.clear();

        // Detectar si es un prefab por extensión
        std::string ext = filepath.substr(filepath.find_last_of(".") + 1);
        bool isPrefab = (ext == "prefab");

        if (isPrefab) {
            // Cargar componentes del prefab como objetos principales
            if (j.contains("components") && j["components"].is_array()) {
                for (const auto& comp : j["components"]) {
                    SceneObject obj;
                    obj.name = comp.value("name", comp.value("type", ""));
                    obj.type = comp.value("type", "");
                    obj.properties = comp;
                    obj.modelPath = comp.value("modelPath", "");
                    obj.color = glm::dvec3(1.0);
                    obj.intensity = 1.0;

                    if (comp.contains("position") && comp["position"].size() == 3)
                        obj.position = {comp["position"][0], comp["position"][1], comp["position"][2]};
                    if (comp.contains("rotation") && comp["rotation"].size() == 3)
                        obj.rotation = {comp["rotation"][0], comp["rotation"][1], comp["rotation"][2]};
                    if (comp.contains("scale") && comp["scale"].size() == 3)
                        obj.scale = {comp["scale"][0], comp["scale"][1], comp["scale"][2]};
                    if (comp.contains("color") && comp["color"].size() == 3)
                        obj.color = {comp["color"][0], comp["color"][1], comp["color"][2]};
                    if (comp.contains("intensity"))
                        obj.intensity = comp["intensity"].get<double>();
                    obj.renderLayer = std::clamp(comp.value("renderLayer", 1), 1, 5);

                    // Cargar material si existe
                    if (comp.contains("material")) {
                        obj.material = std::make_shared<MaterialComponent>();
                        obj.material->fromJSON(comp["material"]);
                    }

                    if (comp.contains("properties")) {
                        obj.properties = comp["properties"];
                    }

                    if (comp.contains("meshRenderer") && comp["meshRenderer"].is_object()) {
                        const auto& mesh = comp["meshRenderer"];
                        if (mesh.contains("vertices") && mesh.contains("indices")) {
                            auto vertices = jsonToVec3Array(mesh["vertices"]);
                            auto normals = mesh.contains("normals") ? jsonToVec3Array(mesh["normals"]) : std::vector<glm::vec3>{};
                            auto indices = jsonToIndexArray(mesh["indices"]);
                            if (!vertices.empty() && !indices.empty()) {
                                obj.meshRenderer = std::make_shared<MeshRendererComponent>();
                                obj.meshRenderer->setMesh(vertices, normals, indices);
                            }
                        }
                    }

                    objects.push_back(obj);
                }
            }
        } else {
            // Cargar escena normal
            if (j.contains("objects") && j["objects"].is_array()) {
                for (const auto& o : j["objects"]) {
                    SceneObject obj = parseSceneObject(o);
                    objects.push_back(obj);
                }
            }
        }

        // Ejecutar inicializador solo en escenas
        if (!isPrefab) {
            executeInitializer(filepath);
        }

        in.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading scene: " << e.what() << std::endl;
        return false;
    }
}

SceneObject Scene::parseSceneObject(const nlohmann::json& o) {
    SceneObject obj;
    obj.properties = o;
    obj.name = o.value("name", "");
    obj.type = o.value("type", "");
    obj.modelPath = o.value("modelPath", "");
    
    if (o.contains("position") && o["position"].size() == 3)
        obj.position = {o["position"][0], o["position"][1], o["position"][2]};
    if (o.contains("rotation") && o["rotation"].size() == 3)
        obj.rotation = {o["rotation"][0], o["rotation"][1], o["rotation"][2]};
    if (o.contains("scale") && o["scale"].size() == 3)
        obj.scale = {o["scale"][0], o["scale"][1], o["scale"][2]};
    if (o.contains("color") && o["color"].size() == 3)
        obj.color = {o["color"][0], o["color"][1], o["color"][2]};
    
    obj.intensity = o.value("intensity", 1.0);
    obj.renderLayer = std::clamp(o.value("renderLayer", 1), 1, 5);
    obj.parentIndex = o.value("parentIndex", -1);
    if (o.contains("childrenIndices")) 
        obj.childrenIndices = o["childrenIndices"].get<std::vector<int>>();

    // Cargar material
    if (o.contains("material")) {
        obj.material = std::make_shared<MaterialComponent>();
        obj.material->fromJSON(o["material"]);
    }

    // Cargar meshRenderer
    if (o.contains("meshRenderer")) {
        obj.meshRenderer = std::make_shared<MeshRendererComponent>();
        const auto& meshJson = o["meshRenderer"];
        std::vector<glm::vec3> verts, norms;
        std::vector<unsigned int> indices;

        if (meshJson.contains("vertices") && meshJson.contains("indices")) {
            for (const auto& item : meshJson["vertices"]) {
                if (item.is_array() && item.size() == 3) {
                    verts.emplace_back(item[0].get<float>(), item[1].get<float>(), item[2].get<float>());
                }
            }
            if (meshJson.contains("normals")) {
                for (const auto& item : meshJson["normals"]) {
                    if (item.is_array() && item.size() == 3) {
                        norms.emplace_back(item[0].get<float>(), item[1].get<float>(), item[2].get<float>());
                    }
                }
            }
            for (const auto& item : meshJson["indices"]) {
                indices.push_back(item.get<unsigned int>());
            }
        } else {
            std::string meshType = meshJson.value("meshType", "cube");
            if (meshType == "sphere") {
                float radius = meshJson.value("radius", 1.0f);
                int segments = meshJson.value("segments", 32);
                PrimitiveShapes::createSphere(radius, segments, segments, verts, norms, indices);
            }
            else if (meshType == "cube") {
                float size = meshJson.value("size", 1.0f);
                PrimitiveShapes::createCube(size, verts, norms, indices);
            }
            else if (meshType == "capsule") {
                float radius = meshJson.value("radius", 0.5f);
                float height = meshJson.value("height", 2.0f);
                int segments = meshJson.value("segments", 24);
                int stacks = meshJson.value("stacks", 16);
                PrimitiveShapes::createCapsule(radius, height, segments, stacks, verts, norms, indices);
            }
        }

        if (!verts.empty() && !indices.empty()) {
            obj.meshRenderer->setMesh(verts, norms, indices);
            std::cout << "[Scene] Objeto '" << obj.name << "' meshRenderer: "
                      << verts.size() << " vértices, "
                      << indices.size() / 3 << " triángulos" << std::endl;
        } else {
            std::cout << "[Scene] Objeto '" << obj.name << "' meshRenderer: SIN MESH" << std::endl;
        }
    }

    // Detectar tipo por extensión si tiene path (Prefab)
    if (o.contains("path")) {
        std::string filePath = o.value("path", "");
        std::string fileExt = filePath.substr(filePath.find_last_of(".") + 1);
        
        if (fileExt == "prefab") {
            if (obj.type.empty()) obj.type = "Prefab";
            loadPrefabComponents(filePath, obj);
        } else if (fileExt == "scene") {
            obj.type = "Scene";
        }
    }
    // Si tipo es Prefab pero sin path, buscar por nombre
    else if (obj.type == "Prefab") {
        std::string prefabPath = "Assets/Prefabs/" + obj.name + ".prefab";
        loadPrefabComponents(prefabPath, obj);
    }

    return obj;
}

void Scene::loadPrefabComponents(const std::string& prefabPath, SceneObject& obj) {
    std::ifstream prefabFile(prefabPath);
    if (!prefabFile.is_open()) return;

    nlohmann::json prefabData;
    prefabFile >> prefabData;
    prefabFile.close();
    
    if (prefabData.contains("components")) {
        for (const auto& comp : prefabData["components"]) {
            SceneObject child;
            child.name = comp.value("type", "");
            child.type = comp.value("type", "");
            child.properties = comp;
            child.modelPath = "";

            // Aplicar componente principal del prefab al objeto padre
            if (comp.contains("meshRenderer") && !obj.meshRenderer) {
                obj.meshRenderer = std::make_shared<MeshRendererComponent>();
                std::vector<glm::vec3> verts, norms;
                std::vector<unsigned int> indices;

                const auto& meshJson = comp["meshRenderer"];
                if (meshJson.contains("vertices") && meshJson.contains("indices")) {
                    verts = jsonToVec3Array(meshJson["vertices"]);
                    if (meshJson.contains("normals")) {
                        norms = jsonToVec3Array(meshJson["normals"]);
                    }
                    indices = jsonToIndexArray(meshJson["indices"]);
                } else {
                    std::string meshType = meshJson.value("meshType", "cube");

                    if (meshType == "sphere") {
                        float radius = meshJson.value("radius", 1.0f);
                        int segments = meshJson.value("segments", 32);
                        PrimitiveShapes::createSphere(radius, segments, segments, verts, norms, indices);
                    } else if (meshType == "cube") {
                        float size = meshJson.value("size", 1.0f);
                        PrimitiveShapes::createCube(size, verts, norms, indices);
                    } else if (meshType == "capsule") {
                        float radius = meshJson.value("radius", 0.5f);
                        float height = meshJson.value("height", 2.0f);
                        int segments = meshJson.value("segments", 24);
                        int stacks = meshJson.value("stacks", 16);
                        PrimitiveShapes::createCapsule(radius, height, segments, stacks, verts, norms, indices);
                    }
                }

                if (!verts.empty() && !indices.empty()) {
                    obj.meshRenderer->setMesh(verts, norms, indices);
                }
            }

            if (comp.contains("material") && !obj.material) {
                obj.material = std::make_shared<MaterialComponent>();
                obj.material->fromJSON(comp["material"]);
            }

            if (comp.contains("color") && comp["color"].size() == 3) {
                obj.color = {comp["color"][0], comp["color"][1], comp["color"][2]};
            }
            
            // Heredar posición/rotación/escala del prefab padre
            if (comp.contains("position") && comp["position"].size() == 3) {
                child.position = {comp["position"][0], comp["position"][1], comp["position"][2]};
            } else {
                child.position = obj.position;
            }
            
            if (comp.contains("rotation") && comp["rotation"].size() == 3) {
                child.rotation = {comp["rotation"][0], comp["rotation"][1], comp["rotation"][2]};
            } else {
                child.rotation = obj.rotation;
            }
            
            if (comp.contains("scale") && comp["scale"].size() == 3) {
                child.scale = {comp["scale"][0], comp["scale"][1], comp["scale"][2]};
            } else {
                child.scale = obj.scale;
            }
            
            obj.children.push_back(child);
        }
    }
}

void Scene::executeInitializer(const std::string& scenePath) {
    std::ifstream in(scenePath);
    if (!in.is_open()) return;
    nlohmann::json j;
    in >> j;
    in.close();
    if (!j.contains("initializer")) return;
    if (j["initializer"].is_string() && j["initializer"].get<std::string>().empty()) return;
    std::filesystem::path scenePath_fs(scenePath);
    std::filesystem::path projectRoot = scenePath_fs.parent_path().parent_path();
    std::string projectName = "";
    std::filesystem::path projectFile = projectRoot / "project.hrk";
    std::ifstream pj(projectFile);
    if (pj.is_open()) {
        std::string line;
        while (std::getline(pj, line)) {
            if (line.find("\"name\"") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t firstQuote = line.find("\"", start) + 1;
                size_t lastQuote = line.find("\"", firstQuote);
                projectName = line.substr(firstQuote, lastQuote - firstQuote);
                break;
            }
        }
        pj.close();
    }
    std::string logicLib = "lib" + projectName + ".so";
    std::filesystem::path libPath = "./" + logicLib;
    std::cout << "Loading initializer: " << libPath.string() << std::endl;
    void* handle = dlopen(libPath.c_str(), RTLD_LAZY);
    if (!handle) {
        std::cerr << "Could not load initializer library: " << dlerror() << std::endl;
        return;
    }

    // Buscar y ejecutar símbolo adecuado
    typedef void (*InitFunc)(Haruka::Scene*);
    typedef Haruka::GameInterface* (*GetGameInterfaceFunc)();
    InitFunc initFunc = nullptr;
    GetGameInterfaceFunc getIface = nullptr;
    bool initialized = false;

    // 1. initializeGame clásico
    initFunc = (InitFunc)dlsym(handle, "initializeGame");
    if (initFunc) {
        std::cout << "Found symbol: initializeGame" << std::endl;
        initFunc(this);
        std::cout << "Initializer executed successfully" << std::endl;
        initialized = true;
    }

    // 2. getGameInterface moderno
    if (!initialized) {
        getIface = (GetGameInterfaceFunc)dlsym(handle, "getGameInterface");
        if (getIface) {
            std::cout << "Found symbol: getGameInterface" << std::endl;
            Haruka::GameInterface* iface = getIface();
            if (iface && iface->onInit) {
                iface->onInit(this);
                std::cout << "GameInterface->onInit executed successfully" << std::endl;
                initialized = true;
            }
        }
    }

    // 3. Mangled names (backward compatibility)
    if (!initialized) {
        const char* mangled[] = {
            "_ZN9GameLogic15GameInitializer14initializeGameEPN6Haruka5SceneE",
            "_ZN9GameLogic16GameInitializer16initializeGameEPN6Haruka5SceneE",
            nullptr
        };
        for (int i = 0; mangled[i] != nullptr; i++) {
            initFunc = (InitFunc)dlsym(handle, mangled[i]);
            if (initFunc) {
                std::cout << "Found symbol: " << mangled[i] << std::endl;
                initFunc(this);
                std::cout << "Initializer executed successfully" << std::endl;
                initialized = true;
                break;
            }
        }
    }

    if (!initialized) {
        std::cerr << "Initializer function not found" << std::endl;
    }
    dlclose(handle);
}

}