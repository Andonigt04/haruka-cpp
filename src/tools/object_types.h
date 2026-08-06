/**
 * @file object_types.h
 * @brief `ObjectType` enum and string-conversion helpers.
 *
 * `ObjectType` classifies every scene object for editor and runtime dispatch.
 * Helper functions `stringToObjectType` / `objectTypeToString` handle JSON
 * round-trips. `isRenderableObjectType` and `isLightObjectType` are used by
 * the render queue to filter draw-eligible and light-source objects.
 *
 * Custom object types (minerals, trees, monsters, projectiles, etc.) can be
 * registered at runtime via `customTypeId()` and objects created with
 * `ObjectType::CUSTOM`. Game code attaches per-object data via
 * `SceneObject::userData` (std::any).
 */
#pragma once

#include <string>
#include <unordered_map>

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
        COLLIDER = 15,          // Volúmenes de colisión puros

        // --- TIPO GENÉRICO PARA ENTIDADES CUSTOM ---
        CUSTOM = 999            // Cualquier tipo definido por el juego (mineral, árbol, monstruo, proyectil...)
    };

    /**
     * @brief Tipos de primitivas para el MeshRenderer.
     */
    enum class PrimitiveType {
        NONE     = 0,
        CUBE     = 1,
        SPHERE   = 2,
        CAPSULE  = 3,
        CILINDER = 4,
        PLANE    = 5,
        TRIANGLE = 6,
    };

    inline PrimitiveType stringToPrimitiveType(const std::string& s) {
        if (s == "Cube" || s == "cube") return PrimitiveType::CUBE;
        if (s == "Sphere" || s == "sphere") return PrimitiveType::SPHERE;
        if (s == "Capsule" || s == "capsule") return PrimitiveType::CAPSULE;
        if (s == "Cylinder" || s == "cylinder") return PrimitiveType::CILINDER;
        if (s == "Plane" || s == "plane") return PrimitiveType::PLANE;
        return PrimitiveType::NONE;
    }

    inline std::string primitiveTypeToString(PrimitiveType type) {
        switch (type) {
            case PrimitiveType::CUBE: return "Cube";
            case PrimitiveType::SPHERE: return "Sphere";
            case PrimitiveType::CAPSULE: return "Capsule";
            case PrimitiveType::CILINDER: return "Cylinder";
            case PrimitiveType::PLANE: return "Plane";
            default: return "None";
        }
    }

    /// Runtime registry of custom type names → numeric IDs.
    /// Game code calls `registerCustomType("Monster")` at init; the returned
    /// ID can be stored or compared, but dispatch is typically via
    /// `SceneObject::type` string comparison in game callbacks.
    inline int registerCustomType(const std::string& name) {
        static std::unordered_map<std::string, int> s_customIds;
        static int s_nextId = 1000;
        auto it = s_customIds.find(name);
        if (it != s_customIds.end()) return it->second;
        int id = s_nextId++;
        s_customIds[name] = id;
        return id;
    }

    /// Returns 0 if `name` was never registered.
    inline int lookupCustomType(const std::string& name) {
        static std::unordered_map<std::string, int> s_customIds;
        auto it = s_customIds.find(name);
        return it != s_customIds.end() ? it->second : 0;
    }

    /// Classifies a freeform type string into the `ObjectType` enum.
    /// Built-in types (planet, model, mesh, …) map to their enum values;
    /// anything else returns `ObjectType::CUSTOM` so the game can handle it.
    inline ObjectType classifyObjectType(const std::string& s) {
        if (s == "planet" || s == "Planet" || s == "CelestialBody" || s == "celestialbody" || s == "satellite" || s == "Satellite")
            return ObjectType::PLANET;
        if (s == "star" || s == "Star")
            return ObjectType::STAR;
        if (s == "model" || s == "Model")
            return ObjectType::MODEL;
        if (s == "mesh" || s == "Mesh")
            return ObjectType::MESH;
        if (s == "light" || s == "Light" || s == "PointLight")
            return ObjectType::LIGHT;
        if (s == "directional_light" || s == "DirectionalLight")
            return ObjectType::DIRECTIONAL_LIGHT;
        if (s == "spotlight" || s == "SpotLight" || s == "Spotlight")
            return ObjectType::SPOTLIGHT;
        if (s == "camera" || s == "Camera")
            return ObjectType::CAMERA;
        if (s == "character" || s == "Character")
            return ObjectType::CHARACTER;
        if (s == "empty" || s == "Empty")
            return ObjectType::EMPTY;
        if (s == "particle_system" || s == "ParticleSystem" || s == "particles")
            return ObjectType::PARTICLE_SYSTEM;
        if (s == "audio_source" || s == "AudioSource")
            return ObjectType::AUDIO_SOURCE;
        if (s == "collider" || s == "Collider")
            return ObjectType::COLLIDER;
        return ObjectType::CUSTOM;   // game-defined: mineral, tree, monster, projectile, …
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
