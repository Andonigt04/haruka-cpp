#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>

namespace Haruka {

    /**
     * @brief Representación unificada de cualquier entidad en el universo.
     * Diseñado para soportar desde pequeñas naves hasta planetas procedurales.
     */
    struct SceneObject {
        // Identificación
        std::string name;
        std::string type; // "CelestialBody", "Planet", "Spacecraft", "Camera", etc.
        std::string templateName; // El arquetipo del que hereda (opcional)

        // Transformación con precisión astronómica (double precision)
        // Vital para evitar el jittering en escalas de KM
        glm::dvec3 position = glm::dvec3(0.0);
        glm::dvec3 rotation = glm::dvec3(0.0);
        glm::dvec3 scale    = glm::dvec3(1.0);

        // Flags de sistema
        bool hasChunks = false;          // ¿Usa subdivisión QuadTree/Octree?
        bool isPersistent = false;       // ¿Debe estar siempre en RAM? (ej. el Sol)
        bool originShiftingTarget = false; // ¿Es un candidato para ser el centro del mundo?

        // Bloques de datos (JSON)
        // Se mantienen como JSON para que cada sistema (Streaming, Render, Física)
        // extraiga solo lo que necesite sin sobrecargar este struct.
        
        nlohmann::json lodSettings;      // Configuración de distancias o QuadTree
        nlohmann::json streamingSettings;// Modo (Disk/Procedural) y prioridad
        nlohmann::json terrainSettings;  // Capas de ruido, semilla, biomas
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
        
        std::mutex m_mutex; // Para que el StreamingSystem pueda leer mientras el Loader carga
    };

} // namespace Haruka