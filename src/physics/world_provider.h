/**
 * @file world_provider.h
 * @brief Ventana ABSTRACTA al mundo para la física — sin GL, sin renderer, sin assets.
 *
 * La física (`PhysicsEngine`) necesita tres cosas del mundo: la gravedad (posiciones y masas de los
 * cuerpos celestes), el nivel del mar del planeta activo (buoyancy) y la altura del terreno (ground).
 * Antes las pedía directamente a `WorldSystem`/`PlanetarySystem`, que arrastran el motor entero (y GL).
 *
 * Esta interfaz corta ese lazo: la física solo depende de `IWorldProvider` + glm. Así se compila:
 *   - en el CLIENTE, implementada sobre `WorldSystem` + la altura de la malla (F10), y
 *   - en el SERVIDOR (DGS), implementada sobre su estado autoritativo con el sampler analítico,
 *     SIN GL — que es lo que permite validar el movimiento con el MISMO código en ambos lados.
 *
 * Todas las magnitudes en METROS (mismas unidades que las posiciones de los cuerpos físicos).
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace Haruka { namespace Physics {

/** @brief Un cuerpo que ejerce gravedad: posición (mundo, m) + masa (kg). Reducción de
 *  `CelestialBody` a lo único que la gravedad usa → la física no conoce el tipo del motor. */
struct GravBody {
    glm::dvec3 worldPos;   // metros
    double     mass;       // kg
};

class IWorldProvider {
public:
    virtual ~IWorldProvider() = default;

    /** @brief ¿Hay un planeta activo (para buoyancy / nivel del mar)? */
    virtual bool       hasActivePlanet()    const = 0;
    /** @brief Centro del planeta activo (mundo, m). */
    virtual glm::dvec3 activePlanetCenter() const = 0;
    /** @brief Radio del planeta activo = nivel del mar (esfera de referencia, m). */
    virtual double     activePlanetRadius() const = 0;

    /** @brief Cuerpos que ejercen gravedad (Sol, planetas, lunas). La física los suma. */
    virtual const std::vector<GravBody>& gravBodies() const = 0;

    /** @brief Altura del terreno en `worldPos`: metros SOBRE la esfera de referencia (radio). El
     *  ground-snap la usa. Cliente: malla (F10). Servidor: sampler analítico. Mismas unidades. */
    virtual double terrainHeightAt(const glm::dvec3& worldPos) const = 0;
};

}} // namespace Haruka::Physics
