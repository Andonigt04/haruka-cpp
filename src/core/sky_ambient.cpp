/**
 * @file sky_ambient.cpp
 * @brief Gemelo CPU de `sky_palette.glsl` + integración a SH de la irradiancia difusa.
 *
 * Ver `sky_ambient.h` para el contrato de paridad con el GLSL. Este fichero tiene DOS partes:
 *   1. Las funciones gemelas (`harukaSkyDay`, `harukaSkyBase`) — mismas cifras y mismas fórmulas
 *      que `lib/sky_palette.glsl`, para que CPU y GPU dibujen/iluminen con el mismo cielo.
 *   2. La integración a SH (`skyAmbientSH`, `skyAmbientEval`) — el fragmento que el shader solo
 *      evalúa, pero que aquí se verifica con los tests (paridad y simetría).
 */
#include "sky_ambient.h"

#include <cmath>

namespace Haruka {

// ---------------------------------------------------------------------------
// Parte 1 — GEMELOS de lib/sky_palette.glsl (CUALQUIER CAMBIO EN LOS DOS A LA VEZ)
// ---------------------------------------------------------------------------

float harukaSkyDay(float sunElev) {
    // smoothstep(-0.12, 0.22, x) = t²(3-2t) con t = clamp((x+0.12)/0.34, 0, 1).
    float t = (sunElev + 0.12f) / 0.34f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

glm::vec3 harukaSkyBase(float t, float day) {
    const glm::vec3 zenithC = SKY_ZENITH_NIGHT + (SKY_ZENITH_DAY - SKY_ZENITH_NIGHT) * day;
    const glm::vec3 horizC  = SKY_HORIZ_NIGHT  + (SKY_HORIZ_DAY  - SKY_HORIZ_NIGHT)  * day;

    // smoothstep(0.0, 0.55, max(t, 0.0)); el GLSL aplana bajo el horizonte al color de horizonte.
    float h = std::max(t, 0.0f);
    h = h / 0.55f;
    h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
    h = h * h * (3.0f - 2.0f * h);
    return horizC + (zenithC - horizC) * h;
}

// ---------------------------------------------------------------------------
// Parte 2 — SH de irradiancia difusa
// ---------------------------------------------------------------------------

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

// --- Kernel difuso CONVOLUCIONADO (Ramamoorthi & Hanrahan), en la convención "media sobre la
// esfera" de este fichero. La literatura usa A0=π, A1=2π/3, A2=π/4 con coeficientes definidos
// como c_lm = ∫L·Y_lm·dω; aquí los coeficientes se normalizan por 1/4π (m_lm = c_lm/4π, así
// coef[0] ≈ color medio del cielo), luego el kernel se compensa por 4π para que
// `skyAmbientEval` devuelva la irradiancia ESTÁNDAR E(n)=Σ A_l·c_lm·Y_lm(n). ---
inline float diffuseKernel(int l) {
    switch (l) {
        case 0: return 39.4784176044f;  // π · 4π
        case 1: return 26.3189450696f;  // (2π/3) · 4π
        case 2: return  9.8696044011f;  // (π/4) · 4π
        default: return 0.0f;
    }
}

} // namespace

SkySH skyAmbientSH(float sunElev, float cloudCover, const glm::vec3& groundColor) {
    const float day = harukaSkyDay(sunElev);

    // Cuadratura esférica por bandas de cénit y acimut. Resolución fina y barata (el módulo se
    // llama ~una vez por frame): el error de convergencia queda ~1e-3, muy por debajo del salto
    // perceptible de `ambientStrength`. Los tests fijan los números.
    const int NZ = 48;  // bandas de cénit (θ)
    const int NA = 64;  // acimuts (φ)

    // Pesos de cuadratura (2D): dω = sinθ dθ dφ → sumamos Σ f(θ,φ)·sinθ·(π/NZ)·(2π/NA).
    const float wTheta = 3.1415926536f / NZ;
    const float wPhi   = 6.2831853072f / NA;

    // La media sobre la esfera usa 1/área para que los coeficientes sean los de la base
    // ortonormal (c_lm = ∫ L·Y_lm dω / 4π ... integrado con el peso angular explícito abajo).
    const float invArea = 1.0f / (4.0f * 3.1415926536f);

    // Luz del cielo por muestra, ya atenuada por nube (escalar; la FORMA de la nube no importa
    // para la irradiancia difusa — el GLSL lo dice: solo cuánta hay). Bajo el horizonte, el
    // REBOTE DE SUELO (color del bioma) sustituye al aplanado azul de horizonte del shader.
    const float cloudDim = 1.0f - 0.45f * std::max(0.0f, std::min(1.0f, cloudCover));

    // Acumuladores en `double`: sumas de ~3k términos en float acumulan error ~1e-4 que, al
    // convolucionar, se amplifica. El gemelo GLSL corre en float pero ahí cada píxel es
    // independiente (sin suma larga) — la paridad no se rompe.
    SkySH out;

    // La cúpula y la nube NO dependen del acimut: `harukaSkyBase(c, day)` es igual para todo φ.
    // El bucle de φ solo sirve para integrar la parte angular de cada base Y_lm → se mantiene
    // (la simetría azimutal aparece SOLA en los m≠0, que se anulan — verificable por test).
    for (int i = 0; i < 9; ++i) {
        double ar = 0.0, ag = 0.0, ab = 0.0;
        for (int iz = 0; iz < NZ; ++iz) {
            const float theta = (iz + 0.5f) * wTheta;
            const float c = std::cos(theta), s = std::sin(theta);   // c = cosθ = t del gradiente
            glm::vec3 L;
            if (c >= 0.0f) L = harukaSkyBase(c, day) * cloudDim;
            else           L = groundColor * (0.85f * cloudDim);    // rebote de suelo
            const float weight = s * wTheta * wPhi * invArea;       // · dω / área de la esfera
            for (int ia = 0; ia < NA; ++ia) {
                const float phi = (ia + 0.5f) * wPhi;
                const float x = s * std::cos(phi), y = s * std::sin(phi), z = c;
                const float Y = shY(i, x, y, z);
                ar += (double)(Y * weight) * (double)L.x;
                ag += (double)(Y * weight) * (double)L.y;
                ab += (double)(Y * weight) * (double)L.z;
            }
        }
        // Convolución con el kernel difuso: el SH de irradiancia es A_l·c_lm.
        const int l = i == 0 ? 0 : (i < 4 ? 1 : 2);
        const float A = diffuseKernel(l);
        out.coef[i] = glm::vec3((float)(ar * A), (float)(ag * A), (float)(ab * A));
    }

    return out;
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

} // namespace Haruka
