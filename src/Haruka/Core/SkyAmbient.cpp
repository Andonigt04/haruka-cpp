/**
 * @file SkyAmbient.cpp
 * @brief SH del ambiente cielo/suelo y su evaluación. Ver `SkyAmbient.h`.
 */
#include "SkyAmbient.h"

#include <cmath>

namespace Haruka::Core {

namespace {

// --- Base SH real ortonormal, banda 0..2 (evaluadas en un vector unitario (x,y,z)). ---
// Orden del índice de `SkySH`: i = l(l+1)+m → 0:(0,0) 1:(1,-1) 2:(1,0) 3:(1,1)
//                                       4:(2,-2) 5:(2,-1) 6:(2,0) 7:(2,1) 8:(2,2)
inline float shY(int i, float x, float y, float z) {
    switch (i) {
        case 0: return 0.2820947918f;                                  // Y00
        case 1: return  0.4886025119f * y;                             // Y1,-1
        case 2: return  0.4886025119f * z;                             // Y10
        case 3: return  0.4886025119f * x;                             // Y11
        case 4: return  1.0925484306f * x * y;                         // Y2,-2
        case 5: return  1.0925484306f * y * z;                         // Y2,-1
        case 6: return  0.3153915653f * (3.0f * z * z - 1.0f);         // Y20
        case 7: return  1.0925484306f * x * z;                         // Y21
        case 8: return  0.5462742153f * (x * x - y * y);               // Y22
        default: return 0.0f;
    }
}

} // namespace

SkySH skyHemisphereSH(const glm::vec3& up, const glm::vec3& down) {
    SkySH sh{};
    // Y00 = 0,28209 · Y10 = 0,48860·z: E(n) = c0·Y00 + c2·Y10 = (arriba+abajo)/2 + (arriba−abajo)/2·z.
    sh.coef[0] = 0.5f * (up + down) / 0.2820947918f;
    sh.coef[2] = 0.5f * (up - down) / 0.4886025119f;
    return sh;
}

glm::vec3 skyAmbientEval(const SkySH& sh, const glm::vec3& n, const glm::vec3& up) {
    // Triedro local alrededor de `up`: el SH se integró en ese frame. Gram-Schmidt para que sea
    // ortonormal aunque `n` y `up` no sean perpendiculares (no lo son en general).
    // OJO: hay que elegir el eje auxiliar ANTES de normalizar — si `up` es paralelo a él, el
    // producto vectorial es 0 y `normalize` daría NaN (no un vector corto que se pueda detectar).
    const glm::vec3 aux = std::fabs(up.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 e1 = glm::normalize(glm::cross(up, aux));
    const glm::vec3 e2 = glm::cross(up, e1);

    const float x = glm::dot(n, e1), y = glm::dot(n, e2), z = glm::dot(n, up);

    glm::vec3 v(0.0f);
    for (int i = 0; i < 9; ++i)
        v += sh.coef[i] * shY(i, x, y, z);
    return v;
}

} // namespace Haruka::Core
