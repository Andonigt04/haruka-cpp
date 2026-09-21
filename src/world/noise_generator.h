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
     * @brief Igual, con la posición en DOBLE. **Es la entrada buena para el terreno.**
     *
     * La red del Perlin se calcula como `pos·scale`, se le quita la parte entera y solo la FRACCIÓN
     * entra en la interpolación. Si ese producto se hace en float, a `scale` alta (6000, 20000) el
     * espaciado de float en esa magnitud ya es ~1e-3 de celda: la posición efectiva se cuantiza y dos
     * implementaciones de la misma fórmula (CPU y GPU) caen en puntos distintos. Sobre un planeta de
     * 6371 km, 1 ulp de la dirección son ~0.38 m de superficie y, por la pendiente, centímetros de
     * altura — justo lo que impedía cumplir el requisito de 1 cm entre lo que se ve y lo que se pisa.
     * Haciendo `pos·scale` y el `floor` en DOUBLE, la fracción llega a float con precisión de sobra y
     * el resultado deja de depender del último bit de la dirección.
     *
     * Las versiones `vec3` delegan aquí: UNA sola implementación (dos copias de una fórmula acaban
     * divergiendo, y en este proyecto eso significa "dos terrenos").
     */
    static float perlin3D(const glm::dvec3& pos, int seed = 0, double scale = 1.0);
    
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
    /** @brief fBm con la posición en DOBLE (ver perlin3D(dvec3)). */
    static float fBm(
        const glm::dvec3& pos,
        int seed = 0,
        int octaves = 4,
        float persistence = 0.5f,
        float lacunarity = 2.0f,
        double scale = 1.0
    );

    /**
     * @brief Perlin noise CON derivada analítica (gradiente exacto).
     * @param pos    Posición de muestreo.
     * @param outGrad [out] Gradiente analítico del ruido respecto a @p pos (∂n/∂pos).
     * @param seed   Semilla determinista.
     * @param scale  Factor de frecuencia/escala.
     * @return Valor del ruido en [-1, 1].
     *
     * El gradiente sale del MISMO cálculo que el valor (sin diferencias finitas),
     * así la normal analítica no aliasa. Ver Fase 1/2 del rediseño de terreno.
     */
    static float perlin3D_d(const glm::vec3& pos, glm::vec3& outGrad,
                            int seed = 0, float scale = 1.0f);
    /** @brief Igual con la posición en DOBLE (ver perlin3D(dvec3)). */
    static float perlin3D_d(const glm::dvec3& pos, glm::vec3& outGrad,
                            int seed = 0, double scale = 1.0);

    /**
     * @brief fBm CON derivada analítica: acumula valor y gradiente por octava.
     * @param outGrad [out] Gradiente analítico del fBm respecto a @p pos.
     * Mismos parámetros que fBm(); normalizado por la suma de amplitudes.
     */
    static float fBm_d(
        const glm::vec3& pos,
        glm::vec3& outGrad,
        int seed = 0,
        int octaves = 4,
        float persistence = 0.5f,
        float lacunarity = 2.0f,
        float scale = 1.0f
    );
    /** @brief fBm con derivada y posición en DOBLE (ver perlin3D(dvec3)). */
    static float fBm_d(
        const glm::dvec3& pos,
        glm::vec3& outGrad,
        int seed = 0,
        int octaves = 4,
        float persistence = 0.5f,
        float lacunarity = 2.0f,
        double scale = 1.0
    );

private:
    /** @brief grad() + su coeficiente de gradiente constante (grad es lineal en x,y,z). */
    static float gradd(int hash, float x, float y, float z, glm::vec3& outCoef);
    static float smoothstep(float t);
    static float lerp(float a, float b, float t);
    static float grad(int hash, float x, float y, float z);
    static int hash(int x, int y, int z, int seed);
};

}
