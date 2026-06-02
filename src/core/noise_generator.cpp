#include "noise_generator.h"
#include <cmath>

namespace Haruka {

float NoiseGenerator::smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float NoiseGenerator::lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

int NoiseGenerator::hash(int x, int y, int z, int seed) {
    // Use x/y/z directly (not >> 2) so adjacent lattice cells produce distinct
    // gradients. The >> 2 shift mapped 4 consecutive integers to the same value,
    // making cells 4x coarser than intended and producing near-flat noise.
    int h = seed;
    h ^= 61 ^ x;
    h += (h << 3);
    h ^= (h >> 4);
    h += (h << 3) ^ y;
    h += (h << 3) ^ z;
    h ^= (h >> 4);
    h += (h << 3);
    return h & 0x7fffffff;
}

float NoiseGenerator::grad(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 8 ? y : z;
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float NoiseGenerator::perlin3D(const glm::vec3& pos, int seed, float scale) {
    glm::vec3 p = pos * scale;

    // Cell origin as full integers (NO premature & 255 — that wrapped 255→0 at
    // every 256-cell boundary, so one cube corner hashed index 255 and its
    // neighbour index 0: unrelated gradients → a hard discontinuity / cliff there.
    // High octaves (scale·lacunarity^i) easily exceed 256, so these cliffs peppered
    // the terrain. We mask each corner index INDIVIDUALLY and consistently below.)
    int x0 = (int)std::floor(p.x);
    int y0 = (int)std::floor(p.y);
    int z0 = (int)std::floor(p.z);

    float xf = p.x - (float)x0;
    float yf = p.y - (float)y0;
    float zf = p.z - (float)z0;

    float u = smoothstep(xf);
    float v = smoothstep(yf);
    float w = smoothstep(zf);

    // Mask each corner's integer index separately so x0 and x0+1 wrap coherently.
    int xi = x0 & 255, yi = y0 & 255, zi = z0 & 255;
    int xj = (x0 + 1) & 255, yj = (y0 + 1) & 255, zj = (z0 + 1) & 255;

    // Calcular gradientes en 8 esquinas del cubo
    float g000 = grad(hash(xi, yi, zi, seed), xf, yf, zf);
    float g100 = grad(hash(xj, yi, zi, seed), xf - 1.0f, yf, zf);
    float g010 = grad(hash(xi, yj, zi, seed), xf, yf - 1.0f, zf);
    float g110 = grad(hash(xj, yj, zi, seed), xf - 1.0f, yf - 1.0f, zf);
    float g001 = grad(hash(xi, yi, zj, seed), xf, yf, zf - 1.0f);
    float g101 = grad(hash(xj, yi, zj, seed), xf - 1.0f, yf, zf - 1.0f);
    float g011 = grad(hash(xi, yj, zj, seed), xf, yf - 1.0f, zf - 1.0f);
    float g111 = grad(hash(xj, yj, zj, seed), xf - 1.0f, yf - 1.0f, zf - 1.0f);
    
    // Interpolar
    float n00 = lerp(g000, g100, u);
    float n10 = lerp(g010, g110, u);
    float n0 = lerp(n00, n10, v);
    
    float n01 = lerp(g001, g101, u);
    float n11 = lerp(g011, g111, u);
    float n1 = lerp(n01, n11, v);
    
    return lerp(n0, n1, w);
}

float NoiseGenerator::fBm(
    const glm::vec3& pos,
    int seed,
    int octaves,
    float persistence,
    float lacunarity,
    float scale
) {
    if (octaves <= 0) return 0.0f;

    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        value += perlin3D(pos, seed + i, scale * frequency) * amplitude;
        maxValue += amplitude;

        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / maxValue;
}

}
