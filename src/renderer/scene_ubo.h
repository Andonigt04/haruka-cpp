#pragma once
/**
 * @file renderer/scene_ubo.h
 * @brief Los bloques UBO del pase de ESCENA (binding 0 por frame, binding 1 por objeto) y los bits
 *        de mascara de material. std140, gemelos de los shaders: cualquier cambio de layout en GLSL
 *        se refleja aqui (y los `static_assert` lo vigilan). Vivian en application_render.cpp; los
 *        comparten el pase de escena, el frame (luces) y la preview de materiales del editor.
 */
#include <glm/glm.hpp>

namespace Haruka::Renderer {

// std140-compatible structs mirroring the UBO declarations in the shaders.
// Any change to the GLSL UBO layout must be reflected here.
struct alignas(16) PerFrameUBOData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 cameraPos;      float _pad0;
    glm::vec3 sunDirection;   float _pad1;
    glm::vec3 sunLightColor;  float ambientStrength;
    int enableHDR;
    int enableBloom;
    int enableSSAO;
    int enableIBL;
    int enableShadows;
    int _pad3[3];
    // Luz de luna (2ª luz, añadida al final → no mueve offsets previos). Los shaders
    // que no la usan pueden omitir estos campos de su bloque UBO.
    glm::vec3 moonDirection;  float moonIntensity;  // dir hacia la Luna + brillo (fase)
    glm::vec3 moonLightColor; float _pad4;          // color azulado tenue
};
static_assert(sizeof(PerFrameUBOData) == 240, "PerFrameUBOData std140 size mismatch");

struct alignas(16) PerObjectUBOData {
    glm::mat4 model;
    glm::vec4 baseColorAndPlanetRadius; // rgb=color, a=planetRadius
    glm::vec4 planetCenterAndFlag;      // xyz=planetCenter, w=useProceduralTerrain
    // MATERIAL del objeto (MaterialComponent). Va al FINAL a propósito: planet.frag y preview.*
    // declaran su propio bloque con los tres campos de arriba y siguen leyendo los mismos
    // offsets; solo los shaders que quieran material declaran el bloque completo.
    glm::vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas (bits)
    glm::vec4 materialEmission;         // rgb=emisión, a=libre
};
static_assert(sizeof(PerObjectUBOData) == 128, "PerObjectUBOData std140 size mismatch");

// Bits de `materialPBR.w`: qué slots de textura están REALMENTE atados este draw. El shader no
// puede preguntárselo a un sampler (uno sin atar lee negro, que es un valor válido), así que la
// presencia viaja explícita. Los valores son potencias de 2 → w es exacto en float hasta 2^24.
enum MaterialTexBit {
    kTexAlbedo    = 1,
    kTexNormal    = 2,
    kTexMetallic  = 4,
    kTexRoughness = 8,
    kTexAO        = 16,
};

} // namespace Haruka::Renderer
