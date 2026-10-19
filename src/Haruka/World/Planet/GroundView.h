#pragma once
/**
 * @file GroundView.h
 * @brief EL SUELO QUE SE DIBUJA ESTE FRAME, para lo que tenga que apoyarse en él en la GPU
 *        (hierba, piedras, huellas, decals…): los nodos hoja resueltos, su mapa de alturas, la tabla
 *        de materiales, la ventana de recorte de las cuevas y los cráteres cercanos.
 *
 * Lo rellena `TerrestrialPlanet::groundView` DESPUÉS de `prepare` (los nodos de este frame ya están
 * resueltos y su compute ya tiene barrera). Los handles son del planeta: valen este frame.
 *
 * ⚠️ NO es el campo de altura (lo que pisa la física): es el mapa del nodo DIBUJADO. Entre los dos
 * hay centímetros (`terrain_node_drawn_vs_field`), y algo apoyado en el campo flotaría.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Terrain/TerrainNodeRenderer.h"   // NearNode

namespace Haruka::World::Planet {

struct GroundView {
    glm::dvec3 planetCenter{0.0};
    double     planetRadiusM = 1.0;
    float      seaLevelM     = 0.0f;
    /// Nodos hoja dibujados que tocan la esfera pedida alrededor de la cámara.
    std::vector<Terrain::TerrainNodeRenderer::NearNode> nodes;
    float      finestTexelM  = 1.0f;     ///< texel (m) del nivel más fino de `nodes`
    RHI::BufferHandle  heights{};        ///< `TerrainNodeRenderer::heightsBuffer()`
    RHI::TextureHandle baseField{};      ///< clima (elev, temp, humedad), con relleno si no hay
    RHI::BufferHandle  materialUBO{};    ///< la tabla de materiales del terreno (cuánto crece en cada material)
    // La ventana de recorte de los vox (misma convención que `TerrainNodeRenderer::Shade`): donde el
    // terreno se recorta para enseñar la boca de una cueva no hay suelo, aunque el mapa diga que sí.
    RHI::TextureHandle cutTex{};
    glm::mat4          cutSpace{1.0f};
    glm::mat4          cutFineSpace{1.0f};
    float              cutFineR = 0.0f;
    glm::vec4          cutAtlas{0.0f};
    uint64_t           cutVersion = 0;   ///< cambia cuando cambia la ventana o aparece un cráter
    /// Cráteres cercanos, relativos al OJO (centro + radio · vertical + hondo).
    std::vector<glm::vec4> craterC, craterU;
};

} // namespace Haruka::World::Planet
