/**
 * @file ibl.h
 * @brief IBL precomputation — HDRI to cubemap, irradiance map, prefilter map, BRDF LUT.
 */
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

/**
 * @brief Image-based lighting precomputation helper.
 *
 * Manages environment cubemap generation, irradiance, prefilter, and BRDF LUT.
 */
class IBL {
public:
    /** @brief Constructs an empty IBL pipeline object. */
    IBL();
    ~IBL();

    /** @brief Fills the environment with a solid sky color and regenerates IBL maps.
     *  Called automatically in the constructor. Call again after loadHDRI() to override. */
    void generateDefaultSky(glm::vec3 skyColor = glm::vec3(0.15f, 0.20f, 0.40f));
    /** @brief Loads an HDR environment map from disk. */
    void loadHDRI(const std::string& imagePath);
    /** @brief Generates diffuse irradiance cubemap. */
    void generateIrradianceMap();
    /** @brief Generates specular prefilter cubemap. */
    void generatePrefilterMap();
    /** @brief Generates BRDF integration LUT texture. */
    void generateBRDFLUT();

    /** @brief Returns irradiance cubemap id. */
    unsigned int getIrradianceMap() const { return irradianceMap; }
    /** @brief Returns prefilter cubemap id. */
    unsigned int getPrefilterMap() const { return prefilterMap; }
    /** @brief Returns BRDF LUT texture id. */
    unsigned int getBRDFLUT() const { return brdfLUT; }
    /** @brief Returns environment cubemap id. */
    unsigned int getEnvCubemap() const { return envCubemap; }

    /** @brief Binds the prefilter cubemap to a texture unit. */
    void bindPrefilterMap(unsigned int textureUnit);
    /** @brief Binds the BRDF LUT texture to a texture unit. */
    void bindBRDFLUT(unsigned int textureUnit);

private:
    /** @brief Allocates cubemap and LUT resources. */
    void setupCubemap();
    /** @brief Draws a unit cube used during cubemap rendering. */
    void renderCube();
    /** @brief Draws a fullscreen quad used for LUT generation. */
    void renderQuad();

    unsigned int envCubemap = 0;
    unsigned int irradianceMap = 0;
    unsigned int prefilterMap = 0;
    unsigned int brdfLUT = 0;

    unsigned int cubeVAO = 0, cubeVBO = 0;
    unsigned int quadVAO = 0, quadVBO = 0;
};