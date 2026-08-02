/**
 * @file scene_manager.h
 * @brief Gestión de escenas: carga, almacenamiento y acceso a objetos del universo.
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
 */
#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <any>

#include "tools/math_types.h"
#include "tools/object_types.h"

namespace Haruka {

    class MeshRendererComponent;
    class EventManager;
    class MaterialComponent;

    struct LODLayerSettings {
        double threshold = 0.0;
        std::string asset;
    };

    struct Flags {
        bool hasChunks = false;          // ¿Usa subdivisión QuadTree/Octree?
        bool isPersistent = false;       // ¿Debe estar siempre en RAM? (ej. el Sol)
        bool originShiftingTarget = false; // ¿Es un candidato para ser el centro del mundo?
        bool castLight = false;          // ¿Se comporta como una luz emisiva?
    };

    struct LODSettings {
        std::string type;
        int maxDepth = 0;
        double splitThreshold = 0.0;
        std::vector<double> thresholds;
        std::vector<std::string> assets;
    };

    struct StreamingSettings {
        std::string mode;
        std::string priority;
        bool enabled = true;
    };

    // (Aquí estaba `TerrainLayerSettings` {freq, octaves, strength} y el mapa `layers` de
    //  TerrainGeneratorSettings: los parámetros por capa de ruido del generador v1. Ese generador se
    //  borró —la altura sale de la cadena geología→erosión→campo— y con él el esquema entero. Lo que
    //  la escena sí sigue autorizando viaja en `rawConfig` sin filtrar.)

    struct TerrainGeneratorSettings {
        std::string type;
        std::string shader;
        int seed = 0;
        int chunkSize = 0;
        // Raw "config" JSON straight from the scene, UNFILTERED. The typed fields
        // above are a convenience subset; rawConfig preserves every parameter
        // (landHeight, oceanDepth, sharpness, belt, trench, future sandbox keys…)
        // so the generator/editor see exactly what the scene authored.
        nlohmann::json rawConfig;
    };

    /**
     * @brief Representación unificada de cualquier entidad en el universo.
     * Diseñado para soportar desde pequeñas naves hasta planetas procedurales.
     */
    /** @brief Id ÚNICO y creciente por objeto. Hace falta porque la cola de render guarda punteros a
     *  objetos que pueden DESTRUIRSE: comparar por puntero no vale (una dirección liberada se reutiliza
     *  para el objeto siguiente y se confundirían) y desreferenciarlo para comprobarlo sería peor. */
    inline uint64_t nextSceneObjectUid() {
        static std::atomic<uint64_t> s_next{1};
        return s_next.fetch_add(1, std::memory_order_relaxed);
    }

    struct SceneObject {
        // Identificación
        uint64_t    uid = nextSceneObjectUid();   // identidad estable (ver nextSceneObjectUid)
        std::string name;
        std::string type; // "CelestialBody", "Planet", "Spacecraft", "Camera", etc.
        ObjectType  objectType = ObjectType::UNKNOWN; // clasificación rápida para dispatch
        std::string templateName; // El arquetipo del que hereda (opcional)

        // Transformación con precisión astronómica (double precision)
        // Vital para evitar el jittering en escalas de KM
        Haruka::WorldPos position = Haruka::WorldPos(0.0);
        Haruka::Rotation rotation = Haruka::Rotation(); // Euler angles en grados
        Haruka::DScale   scale    = Haruka::DScale(1.0);
        // ORIENTACIÓN EXACTA (opcional). Quien ya tiene un cuaternión NO debe pasarlo por Euler: el
        // viaje quat→Euler→matriz se degrada en la singularidad del cardán y devuelve orientaciones
        // basura (síntoma: piezas de un muro entero desperdigadas y girando mal). Con `useOrientation`
        // el render usa este cuaternión tal cual y se salta `rotation`.
        glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
        bool       useOrientation = false;

        
        glm::dvec3 color = glm::dvec3(1.0);
        double intensity = 1.0;
        int renderLayer = 1;
        std::string modelPath;
        std::shared_ptr<MaterialComponent> material;
        std::shared_ptr<MeshRendererComponent> meshRenderer;
        
        // Flags de sistema
        Flags flags;
        // Bloques tipados opcionales: solo existen cuando el tipo de objeto los necesita.
        // El resto de metadatos sigue viviendo en JSON para compatibilidad y extensibilidad.
        std::optional<LODSettings> lodSettings;      // Configuración de distancias o QuadTree
        std::optional<StreamingSettings> streamingSettings;// Modo (Disk/Procedural) y prioridad
        std::optional<TerrainGeneratorSettings> terrainSettings;  // Terreno procedural del objeto
        nlohmann::json surfaceConfig;       // Surface texture config (albedo/normal/height paths)
        nlohmann::json components;       // Luces, scripts, colisionadores
        nlohmann::json properties;       // Metadatos extra (velocidad, facción, etc.)

        // Jerarquía (Opcional para naves dentro de planetas o lunas)
        int parentIndex = -1;
        std::vector<int> childrenIndices;

        /**
         * @brief Helper para verificar si un objeto tiene un componente específico.
         */
        bool hasComponent(const std::string& componentName) const {
            return components.contains(componentName);
        }

        /**
         * @brief Helper rápido para obtener una propiedad sin lanzar excepciones.
         */
        template<typename T>
        T getProperty(const std::string& key, T defaultValue) const {
            if (properties.contains(key)) {
                return properties[key].get<T>();
            }
            return defaultValue;
        }

        /** @brief Dato arbitrario del juego asociado a este objeto (mineral data, AI state, etc.).
         *  No se serializa — es solo para runtime. */
        std::any userData;
    };

    /**
     * @brief Clase unificada para gestión de escenas.
     * Carga, Valida y Almacena en una sola unidad.
     */
    class SceneManager {
    public:
        SceneManager() = default;

        /**
         * @brief Registra un objeto cargado en el sistema.
         * Esta es la función que llama el SceneLoader.
         */
        void addLoadedObject(std::shared_ptr<SceneObject> obj) {
            if (!obj) return;

            std::lock_guard<std::mutex> lock(m_mutex);
            
            // Guardar en la lista principal
            m_objects.push_back(obj);
            
            // Registro por nombre para búsquedas rápidas O(1)
            m_registry[obj->name] = obj;
            
            // Registro por ID único (hash)
            uint64_t id = std::hash<std::string>{}(obj->name);
            m_idRegistry[id] = obj;
        }

        void setName(const std::string& name) { m_name = name; }
        const std::string& getName() const { return m_name; }

        void setEventManager(EventManager* eventManager) { m_eventManager = eventManager; }
        EventManager* getEventManager() const { return m_eventManager; }

        std::vector<SceneObject> getObjects() const {
            std::vector<SceneObject> objects;
            objects.reserve(m_objects.size());
            for (const auto& obj : m_objects) {
                if (obj) objects.push_back(*obj);
            }
            return objects;
        }

        std::vector<std::shared_ptr<SceneObject>>& getObjectsMutable() { return m_objects; }
        const std::vector<std::shared_ptr<SceneObject>>& getObjectsMutable() const { return m_objects; }

        std::shared_ptr<SceneObject> getObject(const std::string& name) { return getObjectByName(name); }
        const std::shared_ptr<SceneObject> getObject(const std::string& name) const {
            auto it = m_registry.find(name);
            return (it != m_registry.end()) ? it->second : nullptr;
        }

        bool load(const std::string& filepath);
        bool save(const std::string& filepath) const;

        void removeObject(const std::string& name) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                [&](auto& obj) { return obj->name == name; }), m_objects.end());
            m_registry.erase(name);
        }

        /**
         * @brief Busca un objeto por su nombre.
         */
        std::shared_ptr<SceneObject> getObjectByName(const std::string& name) {
            auto it = m_registry.find(name);
            return (it != m_registry.end()) ? it->second : nullptr;
        }

        /**
         * @brief Obtiene la lista completa para el renderizado o streaming.
         */
        const std::vector<std::shared_ptr<SceneObject>>& getAllObjects() const {
            return m_objects;
        }

        /**
         * @brief Crea un objeto con tipo inferido del string type.
         * El ObjectType se deduce automáticamente vía classifyObjectType().
         */
        std::shared_ptr<SceneObject> createObject(const std::string& name, const std::string& type) {
            auto obj = std::make_shared<SceneObject>();
            obj->name = name;
            obj->type = type;
            obj->objectType = Haruka::classifyObjectType(type);
            return obj;
        }

        /**
         * @brief Thread-safe snapshot of current scene objects.
         */
        std::vector<std::shared_ptr<SceneObject>> getAllObjectsSnapshot() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_objects;
        }

        /**
         * @brief Limpia la escena actual.
         */
        void clear() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_objects.clear();
            m_registry.clear();
            m_idRegistry.clear();
        }

    private:
        std::vector<std::shared_ptr<SceneObject>> m_objects;
        std::unordered_map<std::string, std::shared_ptr<SceneObject>> m_registry;
        std::unordered_map<uint64_t, std::shared_ptr<SceneObject>> m_idRegistry;
        std::string m_name = "Untitled";
        EventManager* m_eventManager = nullptr;
        
        mutable std::mutex m_mutex; // Para que el StreamingSystem pueda leer mientras el Loader carga
    };

} // namespace Haruka