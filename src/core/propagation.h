#pragma once
/**
 * @file core/propagation.h
 * @brief Navegación/propagación por HUECOS: encuentra el camino de A a B rodeando obstáculos
 *        (paredes = OBBs colocados) y mide cuánta "señal" llega (oclusión por terreno + rodeo).
 *        Compartido por el AUDIO (sonido que entra por la puerta) y por los CONJUROS
 *        serpenteantes (proyectil que rodea la pared). Sin geometría de edificios todavía usa
 *        los objetos colocados (cimientos/estructuras) como muros y el terreno como oclusor.
 */
#include <vector>
#include <glm/glm.hpp>

namespace Haruka {

class Propagation {
public:
    // Camino navegable A→B: [A,...,B]. Recto si no hay muro en medio; si lo hay, rodea por las
    // esquinas. Vacío si no hay paso (totalmente encerrado).
    static std::vector<glm::dvec3> path(const glm::dvec3& a, const glm::dvec3& b);

    // 0 (tapado) .. 1 (libre): oclusión del terreno por tramo del camino × penalización por
    // rodeo (cuanto más largo el desvío, más baja). Para playback de audio Y consultas de IA.
    static float through(const glm::dvec3& a, const glm::dvec3& b);
};

} // namespace Haruka
