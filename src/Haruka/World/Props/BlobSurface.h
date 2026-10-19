#pragma once
/**
 * @file BlobSurface.h
 * @brief UNA MALLA FUNDIDA a partir de BULTOS: la superficie de la unión SUAVE de elipsoides, con un
 *        relieve de bolitas encima y la base plana, convertida en triángulos (surface nets). Se hornea
 *        una vez (al arrancar) y se instancia: el coste de fundir los bultos no se paga por píxel.
 *
 * ── POR QUÉ ──────────────────────────────────────────────────────────────────────────────────────
 * Dibujados uno a uno, los bultos de un cúmulo se leían como GLOBOS (cada esfera con su contorno y su
 * pliegue con la vecina), por bien que se iluminaran. Recorrer en cada píxel la unión suave daba la
 * forma buena pero costaba 85 ms. Horneada, la unión es una malla más: una superficie continua, sin
 * costuras de bola, con LOD por píxeles como cualquier prop.
 *
 * No sabe de nubes: recibe los bultos (`ClumpBlob`, centro/semiejes/giro, en el marco del objeto con
 * y = altura sobre la base) y los mandos de la fusión y del relieve.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/World/ShellClumps.h"
#include "Haruka/World/Props/PropGpu.h"

namespace Haruka::World::Props {

struct BlobSurfaceParams {
    float    smoothK  = 0.12f;   ///< radio de la fusión entre bultos (unidades del objeto)
    float    bump     = 0.20f;   ///< tamaño de una bolita del relieve (0 = sin relieve)
    float    bumpAmp  = 0.09f;   ///< lo que saca el relieve hacia fuera
    float    baseFade = 0.15f;   ///< altura (unidades) en que el relieve nace desde la base plana
    uint32_t seed     = 1u;
    int      cells    = 48;      ///< celdas de la rejilla a lo largo del lado horizontal mayor
};

/// Distancia con signo (aprox.) a la superficie en `p` (marco del objeto). Negativa dentro.
float blobSurfaceSdf(const std::vector<ClumpBlob>& blobs, const BlobSurfaceParams& prm, const glm::vec3& p);

/**
 * @brief Hornea la superficie: vértices de prop (normal suave del gradiente, color blanco) e índices
 * antihorarios vistos desde fuera. Devuelve false si no sale nada.
 */
bool bakeBlobSurface(const std::vector<ClumpBlob>& blobs, const BlobSurfaceParams& prm,
                     std::vector<PropVertex>& verts, std::vector<uint32_t>& idx);

} // namespace Haruka::World::Props
