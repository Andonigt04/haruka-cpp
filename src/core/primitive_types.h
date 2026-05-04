#pragma once

// String constants for primitive mesh types.
// Used as keys in SceneObject::properties["meshRenderer"]["meshType"]
// and as object type strings throughout editor/engine.
namespace PrimitiveType {
    constexpr const char* Cube             = "cube";
    constexpr const char* Sphere           = "sphere";
    constexpr const char* Capsule          = "capsule";
    constexpr const char* Plane            = "plane";
    constexpr const char* PointLight       = "PointLight";
    constexpr const char* DirectionalLight = "DirectionalLight";
    constexpr const char* Sun              = "Sun";
    constexpr const char* Planet           = "Planet";
    constexpr const char* Empty            = "Empty";
    constexpr const char* Camera           = "Camera";

    // Uppercase aliases (used in application.cpp).
    constexpr const char* CUBE    = Cube;
    constexpr const char* SPHERE  = Sphere;
    constexpr const char* CAPSULE = Capsule;
    constexpr const char* PLANE   = Plane;
}
