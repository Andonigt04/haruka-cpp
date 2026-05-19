/**
 * @file noise_generator.h
 * @brief Deterministic 3D Perlin noise and fBm helpers for procedural terrain generation.
 */
#pragma once

#include <glm/glm.hpp>

namespace Haruka {

/**
 * @brief Deterministic 3D noise helper for procedural terrain generation.
 */
class NoiseGenerator {
public:
    /**
     * @brief Evaluates deterministic 3D Perlin-style noise.
     * @param pos Sample position in 3D space.
     * @param seed Deterministic seed.
     * @param scale Frequency/scale factor.
     * @return Noise value in the range [-1, 1].
     */
    static float perlin3D(const glm::vec3& pos, int seed = 0, float scale = 1.0f);
    
    /**
     * @brief Fractal Brownian Motion using layered noise octaves.
     * @param pos Sample position.
     * @param seed Deterministic seed.
     * @param octaves Number of octaves.
     * @param persistence Amplitude decay per octave.
     * @param lacunarity Frequency multiplier per octave.
     * @param scale Base frequency/scale.
     * @return Noise value in the range [-1, 1].
     */
    static float fBm(
        const glm::vec3& pos,
        int seed = 0,
        int octaves = 4,
        float persistence = 0.5f,
        float lacunarity = 2.0f,
        float scale = 1.0f
    );

private:
    static float smoothstep(float t);
    static float lerp(float a, float b, float t);
    static float grad(int hash, float x, float y, float z);
    static int hash(int x, int y, int z, int seed);
};

}
