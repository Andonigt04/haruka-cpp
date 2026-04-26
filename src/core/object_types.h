#ifndef OBJECT_TYPES_H
#define OBJECT_TYPES_H

#include <string>

namespace Haruka {
    /**
     * @brief Scene object classification used by editor/runtime systems.
     */
    enum class ObjectType {
        UNKNOWN = 0,
        
        // Renderizables
        MESH = 1,              // Primitivo (cubo, esfera, plano)
        MODEL = 2,             // Modelo 3D externo
        
        // Luces (No renderizables pero afectan iluminación)
        LIGHT = 3,             // Luz puntual
        DIRECTIONAL_LIGHT = 4, // Luz direccional
        SPOTLIGHT = 5,         // Foco
        
        // Otros (No renderizables)
        CHARACTER = 10,        // Personaje/Jugador
        CAMERA = 11,           // Camera
        EMPTY = 12,            // Objeto vacío
        PARTICLE_SYSTEM = 13,  // Particle system
        AUDIO_SOURCE = 14,     // Fuente de audio
        COLLIDER = 15,         // Colisionador
    };
    
    /** @brief Converts a type string into an `ObjectType` enum value. */
    inline ObjectType stringToObjectType(const std::string& typeStr) {
        if (typeStr == "Mesh") return ObjectType::MESH;
        if (typeStr == "Cube") return ObjectType::MESH;
        if (typeStr == "Sphere") return ObjectType::MESH;
        if (typeStr == "Plane") return ObjectType::MESH;
        if (typeStr == "Model") return ObjectType::MODEL;
        if (typeStr == "Light") return ObjectType::LIGHT;
        if (typeStr == "DirectionalLight") return ObjectType::DIRECTIONAL_LIGHT;
        if (typeStr == "Spotlight" || typeStr == "PointLight") return ObjectType::SPOTLIGHT;
        if (typeStr == "Character") return ObjectType::CHARACTER;
        if (typeStr == "Camera") return ObjectType::CAMERA;
        if (typeStr == "Empty") return ObjectType::EMPTY;
        if (typeStr == "ParticleSystem") return ObjectType::PARTICLE_SYSTEM;
        if (typeStr == "AudioSource") return ObjectType::AUDIO_SOURCE;
        if (typeStr == "Collider") return ObjectType::COLLIDER;
        return ObjectType::UNKNOWN;
    }
    
    /** @brief Converts an `ObjectType` enum value into its string form. */
    inline std::string objectTypeToString(ObjectType type) {
        switch (type) {
            case ObjectType::MESH: return "Mesh";
            case ObjectType::MODEL: return "Model";
            case ObjectType::LIGHT: return "Light";
            case ObjectType::DIRECTIONAL_LIGHT: return "DirectionalLight";
            case ObjectType::SPOTLIGHT: return "Spotlight";
            case ObjectType::CHARACTER: return "Character";
            case ObjectType::CAMERA: return "Camera";
            case ObjectType::EMPTY: return "Empty";
            case ObjectType::PARTICLE_SYSTEM: return "ParticleSystem";
            case ObjectType::AUDIO_SOURCE: return "AudioSource";
            case ObjectType::COLLIDER: return "Collider";
            default: return "Unknown";
        }
    }
    
    /** @brief Returns true for object types that should be rendered. */
    inline bool isRenderableObjectType(ObjectType type) {
        return type == ObjectType::MESH ||
               type == ObjectType::MODEL ||
               type == ObjectType::LIGHT ||
               type == ObjectType::DIRECTIONAL_LIGHT ||
               type == ObjectType::SPOTLIGHT ||
               type == ObjectType::CHARACTER;
    }
    
    /** @brief Returns true for object types that represent lights. */
    inline bool isLightObjectType(ObjectType type) {
        return type == ObjectType::LIGHT || 
               type == ObjectType::DIRECTIONAL_LIGHT || 
               type == ObjectType::SPOTLIGHT;
    }
}

#endif