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
    int h = seed;
    h ^= 61 ^ (x >> 2);
    h += (h << 3);
    h ^= (h >> 4);
    h += (h << 3) ^ (y >> 2);
    h += (h << 3) ^ (z >> 2);
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
    
    int xi = (int)std::floor(p.x) & 255;
    int yi = (int)std::floor(p.y) & 255;
    int zi = (int)std::floor(p.z) & 255;
    
    float xf = p.x - std::floor(p.x);
    float yf = p.y - std::floor(p.y);
    float zf = p.z - std::floor(p.z);
    
    float u = smoothstep(xf);
    float v = smoothstep(yf);
    float w = smoothstep(zf);
    
    // Calcular gradientes en 8 esquinas del cubo
    float g000 = grad(hash(xi, yi, zi, seed), xf, yf, zf);
    float g100 = grad(hash(xi + 1, yi, zi, seed), xf - 1.0f, yf, zf);
    float g010 = grad(hash(xi, yi + 1, zi, seed), xf, yf - 1.0f, zf);
    float g110 = grad(hash(xi + 1, yi + 1, zi, seed), xf - 1.0f, yf - 1.0f, zf);
    float g001 = grad(hash(xi, yi, zi + 1, seed), xf, yf, zf - 1.0f);
    float g101 = grad(hash(xi + 1, yi, zi + 1, seed), xf - 1.0f, yf, zf - 1.0f);
    float g011 = grad(hash(xi, yi + 1, zi + 1, seed), xf, yf - 1.0f, zf - 1.0f);
    float g111 = grad(hash(xi + 1, yi + 1, zi + 1, seed), xf - 1.0f, yf - 1.0f, zf - 1.0f);
    
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
