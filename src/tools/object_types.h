/**
 * @file object_types.h
 * @brief `ObjectType` enum and string-conversion helpers.
 *
 * `ObjectType` classifies every scene object for editor and runtime dispatch.
 * Helper functions `stringToObjectType` / `objectTypeToString` handle JSON
 * round-trips. `isRenderableObjectType` and `isLightObjectType` are used by
 * the render queue to filter draw-eligible and light-source objects.
 */
#pragma once

#include <string>

namespace Haruka {
    /**
     * @brief Identificador de tipo de objeto para el WorldSystem.
     */
    enum class ObjectType {
        UNKNOWN = 0,
        
        // --- RENDERIZABLES ESTÁNDAR ---
        MESH = 1,              // Primitivas (cubo, esfera, plano)
        MODEL = 2,             // Modelos externos (.obj, .fbx, .gltf)
        
        // --- SISTEMA PLANETARIO ---
        PLANET = 6,            // Cuerpo con QuadTree, LOD y Terreno Procedural
        STAR = 7,              // Astro masivo (Sol)
        SATELLITE = 8,         // Lunas o estaciones espaciales
        
        // --- LUCES ---
        LIGHT = 3,             // Luz puntual
        DIRECTIONAL_LIGHT = 4, // Luz global (Sol distante)
        SPOTLIGHT = 5,         // Foco / Linterna

        // --- UTILIDADES Y JUEGO ---
        CHARACTER = 10,        // Jugador o NPCs
        CAMERA = 11,           
        EMPTY = 12,            // Nodo de transformación vacío
        PARTICLE_SYSTEM = 13,  
        AUDIO_SOURCE = 14,     
        COLLIDER = 15          // Volúmenes de colisión puros
    };

    /**
     * @brief Tipos de primitivas para el MeshRenderer.
     */
    enum class PrimitiveType {
        NONE    = 0,
        CUBE    = 1,
        SPHERE  = 2,
        CAPSULE = 3,
        PLANE   = 4,
    };

    // --- HELPERS DE CONVERSIÓN (Para carga de JSON) ---

    inline ObjectType stringToObjectType(const std::string& s) {
        if (s == "planet")   return ObjectType::PLANET;
        if (s == "model")    return ObjectType::MODEL;
        if (s == "mesh")     return ObjectType::MESH;
        if (s == "light")    return ObjectType::LIGHT;
        if (s == "star")     return ObjectType::STAR;
        if (s == "camera")   return ObjectType::CAMERA;
        return ObjectType::UNKNOWN;
    }

    inline std::string objectTypeToString(ObjectType t) {
        switch (t) {
            case ObjectType::PLANET: return "planet";
            case ObjectType::MODEL:  return "model";
            case ObjectType::MESH:   return "mesh";
            case ObjectType::LIGHT:  return "light";
            case ObjectType::STAR:   return "star";
            default:                 return "unknown";
        }
    }

    // --- FILTROS RÁPIDOS ---

    /** @brief ¿Este objeto requiere el pipeline de terreno procedural? */
    inline bool isPlanetary(ObjectType type) {
        return type == ObjectType::PLANET || type == ObjectType::STAR || type == ObjectType::SATELLITE;
    }

    /** @brief ¿Este objeto debe ser procesado por el renderizador de mallas? */
    inline bool isRenderable(ObjectType type) {
        return type == ObjectType::MESH || type == ObjectType::MODEL || type == ObjectType::PLANET;
    }
}
