#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

/**
 * @file bake_util.h
 * @brief Lo que COMPARTEN los horneados de features 3D (cuevas, islas) y el campo volumétrico:
 *        la retícula de celdas por caras del cubo, el hash y el ruido de horneado, la clave FNV y
 *        el PNG gris de ida y vuelta.
 *
 * ⚠️ UNA SOLA COPIA. `dirToFaceUV`/`faceUVToDir` estaban duplicadas en `cave_system.cpp` y en
 * `vox_world.cpp` (con la nota "si cambia allí, cambia aquí"); las islas serían la tercera. Una
 * cueva se rasteriza en chunks con la misma fórmula con la que se sembró: si la fórmula está
 * repetida, el día que una cambie el chunk deja de casar con la cueva y no hay test que lo avise
 * antes que el jugador. Aquí sólo hay funciones puras y sin estado.
 */
namespace Haruka { namespace BakeUtil {

    /// Dirección unitaria → (cara del cubo, u, v) con u,v ∈ [-1,1].
    inline void dirToFaceUV(const glm::dvec3& d, int& face, double& u, double& v) {
        const double ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
        if (ax >= ay && ax >= az)      { face = (d.x > 0) ? 0 : 1; u = d.y / ax; v = d.z / ax; }
        else if (ay >= az)             { face = (d.y > 0) ? 2 : 3; u = d.x / ay; v = d.z / ay; }
        else                           { face = (d.z > 0) ? 4 : 5; u = d.x / az; v = d.y / az; }
    }
    inline glm::dvec3 faceUVToDir(int face, double u, double v) {
        switch (face) {
            case 0:  return glm::normalize(glm::dvec3( 1.0, u,   v));
            case 1:  return glm::normalize(glm::dvec3(-1.0, u,   v));
            case 2:  return glm::normalize(glm::dvec3( u,   1.0, v));
            case 3:  return glm::normalize(glm::dvec3( u,  -1.0, v));
            case 4:  return glm::normalize(glm::dvec3( u,   v,   1.0));
            default: return glm::normalize(glm::dvec3( u,   v,  -1.0));
        }
    }
    /// Clave estable de una celda (cara, i, j) de una retícula de `n` celdas por lado de cara.
    inline uint32_t cellKey(int face, int i, int j, int n) {
        return (uint32_t)face * (uint32_t)(n * n) + (uint32_t)j * (uint32_t)n + (uint32_t)i;
    }
    /// La celda (cara, i, j) de una retícula de `n` por cara que contiene `dir`.
    inline void cellOf(const glm::dvec3& dir, int n, int& face, int& i, int& j) {
        double u, v;
        dirToFaceUV(glm::normalize(dir), face, u, v);
        i = std::min(std::max((int)std::floor((u + 1.0) * 0.5 * n), 0), n - 1);
        j = std::min(std::max((int)std::floor((v + 1.0) * 0.5 * n), 0), n - 1);
    }

    // ── Hash y ruido: SÓLO para hornear (el runtime lee mapas, no evalúa esto) ─────────────────
    inline uint32_t hashU(uint32_t x) {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return x;
    }
    inline float hash01(uint32_t a, uint32_t b) { return (float)(hashU(a ^ hashU(b)) & 0xFFFFFF) / 16777216.0f; }
    inline float vnoise2(float x, float y, uint32_t seed) {
        const int ix = (int)std::floor(x), iy = (int)std::floor(y);
        const float fx = x - (float)ix, fy = y - (float)iy;
        const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        auto h = [&](int a, int b) { return hash01((uint32_t)(a * 73856093) ^ (uint32_t)(b * 19349663), seed); };
        const float a = h(ix, iy),     b = h(ix + 1, iy);
        const float c = h(ix, iy + 1), d = h(ix + 1, iy + 1);
        return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
    }
    /// fBm normal: manchas.
    inline float fbm2(float x, float y, uint32_t seed, int oct) {
        float v = 0.0f, amp = 0.5f, f = 1.0f, norm = 0.0f;
        for (int i = 0; i < oct; ++i) {
            v += amp * vnoise2(x * f, y * f, seed + (uint32_t)i * 977u);
            norm += amp; amp *= 0.5f; f *= 2.0f;
        }
        return v / std::max(norm, 1e-6f);
    }
    /// fBm RIDGED (`1-|2n-1|`): crestas en LÍNEA (corredores de cueva, aristas de roca).
    inline float ridged2(float x, float y, uint32_t seed, int oct) {
        float v = 0.0f, amp = 0.5f, f = 1.0f, norm = 0.0f;
        for (int i = 0; i < oct; ++i) {
            const float n = vnoise2(x * f, y * f, seed + (uint32_t)i * 6151u);
            v += amp * (1.0f - std::fabs(2.0f * n - 1.0f));
            norm += amp; amp *= 0.5f; f *= 2.0f;
        }
        return v / std::max(norm, 1e-6f);
    }

    /// FNV-1a, igual que `bakeCacheKey` del planeta.
    inline void mix(uint64_t& h, const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    }
    inline std::string hex16(uint64_t h) {
        char hex[20];
        std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
        return std::string(hex);
    }

    /// Carpeta de horneados con barra final; vacío = `bakes/` del proyecto.
    std::string bakesDir(const std::string& dir);
    /// PNG gris de 8 bits (guardado como RGBA con los tres canales iguales), ida y vuelta.
    bool savePNGGray(const std::string& path, int w, int h, const std::vector<float>& v);
    bool loadPNGGray(const std::string& path, int& w, int& h, std::vector<float>& v);

    /// Distancia con signo 2D (chamfer 3-4, en TEXELS) a la frontera de una máscara binaria:
    /// positiva dentro (`inside[i]`), negativa fuera. Dos pasadas; exacta a ±4 % del euclídeo.
    void chamferSigned2D(int w, int h, const std::vector<uint8_t>& inside, std::vector<float>& outDist);

    /// Se queda con la componente conexa MAYOR (4 vecinos) de la máscara. Devuelve cuántas había.
    int keepLargestComponent2D(int w, int h, std::vector<uint8_t>& inside);

} } // namespace Haruka::BakeUtil
