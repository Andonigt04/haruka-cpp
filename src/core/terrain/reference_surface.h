/**
 * @file reference_surface.h
 * @brief LA SUPERFICIE DEL JUEGO: una sola definición del suelo, independiente de la cámara.
 *
 * ── EL PROBLEMA QUE RESUELVE ────────────────────────────────────────────────────────────────────
 * Llegamos a tener TRES suelos a la vez, y por eso el autor veía "dos terrenos":
 *   1. Jolt/física  → sampler analítico PUNTUAL, en una retícula de 8 m inventada.
 *   2. Props, scripts y tests → `meshHeightKmAt` = la malla DIBUJADA (depende del LOD y del frustum).
 *   3. El render    → la malla dibujada.
 * Ninguno coincide con otro salvo por casualidad. Medido con meso ON: 0.62 m de media y 7.81 m de
 * pico entre (1) y (3) — y el test que decía vigilar eso comparaba (2) contra (2), o sea la malla
 * consigo misma: daba 0.0000 m mientras el jugador caminaba sobre otra cosa.
 *
 * ── LA DEFINICIÓN ───────────────────────────────────────────────────────────────────────────────
 * La superficie de referencia es el terreno analítico **muestreado exactamente en la retícula de
 * vértices del LOD de referencia**, e interpolado bilineal como lo hace la malla. No es "otro
 * sampler": es LA MISMA rejilla que dibuja el render, evaluada en CPU.
 *
 *   · retícula: en coords de cara [-1,1], paso `2 / (2^refLod · chunkSize)` alineado en lx = -1.
 *     Es literalmente lo que produce `TerrainGenerator::getLocalPosition` (mismo lattice global: el
 *     vértice x del chunk (key.x) cae en el índice key.x·chunkSize + x).
 *   · interpolación: bilineal en (u,v) de cara — la misma que `meshHeightKmAt` usa para leer lo que
 *     se dibuja. La malla es lineal a trozos: evaluarla en un punto interior es esto.
 *
 * Consecuencia: donde el render dibuja el LOD de referencia (que es SIEMPRE alrededor del jugador,
 * porque `LODSystem::setKeepRadius` lo garantiza mires donde mires), lo que se pisa y lo que se ve
 * son la misma superficie por CONSTRUCCIÓN, no por coincidencia. El error que queda es solo la
 * paridad CPU↔GPU de la fórmula, que es un eje aparte y tiene su propio test (`parity`).
 *
 * ── POR QUÉ NO ES EL ANALÍTICO PUNTUAL ──────────────────────────────────────────────────────────
 * Porque el analítico tiene detalle INFINITO y la malla no. Muestrear "la fórmula" en un punto
 * cualquiera devuelve un pico que ningún vértice representa; la diferencia son metros (7.81 m
 * medidos con meso, que añade detalle a 40 m/téxel). El 1 cm es inalcanzable contra el analítico
 * puntual, con pirámide de mips o sin ella. Contra la retícula, es alcanzable por definición.
 *
 * ── POR QUÉ NO ES LA MALLA DIBUJADA ─────────────────────────────────────────────────────────────
 * Porque el draw set depende de hacia dónde mira la cámara: al girar, los chunks bajo los pies salen
 * del frustum, el draw set colapsa al ancestro grueso y el suelo salta cientos de metros → caes. Y
 * porque el SERVIDOR (DGS) no tiene malla: necesita poder calcular el mismo suelo que el cliente
 * para validar. Esta clase es GL-free y vive en `haruka_simbase` justo por eso.
 *
 * ── EL MESO ─────────────────────────────────────────────────────────────────────────────────────
 * Con el meso encendido, cada esquina EXIGE su tesela (`ensureTile`) antes de evaluarse. No es un
 * lujo: `sampleTerrainV2` cae al macro cuando la tesela no está residente, y la esquina quedaría
 * cacheada con ese valor PARA SIEMPRE → dentro del mismo parche de colisión convivirían esquinas
 * macro y esquinas meso, con escalones de hasta ±45 m entre vértices separados 6 m. El coste está
 * acotado: una tesela cubre ~5 km y el juego solo consulta el suelo en un radio de cientos de
 * metros, así que el conjunto de trabajo real son 1-4 teselas.
 */
#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <mutex>
#include <cstdint>

#include "tools/planetary_types.h"            // PlanetFace (GL-free)
#include "core/terrain/terrain_sample.h"      // WorldGenParams

namespace Haruka {

class ReferenceSurface {
public:
    /** @param refLod    LOD de referencia (= el más fino que el streaming produce, `getMaxLOD()`).
     *  @param chunkSize vértices por lado de chunk. refLod+chunkSize DEFINEN la retícula: cambiarlos
     *                   cambia el suelo, así que reconfigurar tira la caché. */
    void configure(const WorldGenParams& W, double radius, int refLod, int chunkSize);
    bool ready() const { return m_refLod >= 0 && m_R > 0.0; }

    /** @brief Elevación (m sobre la esfera de referencia) en la dirección `dir`. Sin deform: el hoyo
     *  cavado lo aplica el llamador (es dinámico y no se puede cachear). */
    double elevM(const glm::dvec3& dir) const;

    /** @brief Paso de la retícula en coords de cara [-1,1]. Público porque los tests comprueban que
     *  coincide con el espaciado real de los vértices de la malla. */
    static double latticeStep(int refLod, int chunkSize) {
        return 2.0 / (double(1u << refLod) * (double)chunkSize);
    }

    void   clear();
    size_t cachedCorners() const;

    // Introspección: los tests comprueban que la retícula es la MISMA que la de la malla dibujada.
    int    refLod()    const { return m_refLod; }
    int    chunkSize() const { return m_chunkSize; }
    double step()      const { return m_step; }

private:
    /** @brief Altura (m) de UN nudo de la retícula, cacheada. La clave es el índice GLOBAL del nudo
     *  en su cara, no una posición relativa al jugador: así la comparten todos los parches que la
     *  tocan (acierto ~100% al revisitar) y el valor es el mismo se pregunte desde donde se pregunte. */
    double cornerM(PlanetFace f, int64_t k, int64_t l) const;

    WorldGenParams m_W{};
    double m_R = 0.0;
    double m_step = 0.0;
    int    m_refLod = -1;
    int    m_chunkSize = 0;

    mutable std::unordered_map<uint64_t, float> m_cache;
    mutable std::mutex                          m_mx;   // la física consulta desde su propio hilo
};

} // namespace Haruka
