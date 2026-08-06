/**
 * @file terrain_prop_field.h
 * @brief ADAPTADOR del campo real del planeta a la interfaz plana de los props (IPropField).
 *
 * Los props (PropLayerTable + placeProps) trabajan en un parche LOCAL plano: `sampleAt(px,pz)`
 * con px/pz en metros sobre un terreno tangente. El campo REAL del planeta es GLOBAL y esférico:
 * se muestrea por DIRECCIÓN (`PlanetFields::sample(dir)` / `PlanetarySystem::sampleSurface`).
 *
 * Este adaptador salva esa distancia: dado un ANCLA (dirección del centro del parche + base
 * ortonormal tangente) y un PAR DE CALLBACKS que el orquestador aporta, convierte cada (px,pz)
 * local del parche en una dirección global y pregunta por ella. Del lado del orquestador los
 * callbacks se definen muestreando el PlanetarySystem real:
 *
 *     fieldFn  = [ps](dir) { return sampleSurfaceAtDir(ps, dir); }   // ecología
 *     heightFn = [ps](dir) { return ps->sampleTerrainHeight(...); }   // cota (m)
 *
 * Así el placer de props NO sabe que vive en una esfera: sigue siendo el mismo código plano
 * determinista, y solo el adaptador sabe de radios y direcciones.
 *
 * GL-free y reversible: no depende ni de RHI ni del planeta concreto, solo de `IPropField`.
 */
#pragma once
#include <functional>
#include <string>
#include <glm/glm.hpp>

#include "core/terrain/planet_fields.h"        // FieldSample
#include "tools/procgraph/tree_spawn.h"        // IPropField (Haruka::Tools::ProcGraph)

namespace Haruka {

/** @brief Sonda de ecología que aporta el orquestador: FieldSample de una dirección del parche. */
using PropDirField  = std::function<Haruka::FieldSample(const glm::vec3& dir)>;
/** @brief Sonda de cota (m) que aporta el orquestador: altura del suelo en una dirección. */
using PropDirHeight = std::function<float(const glm::vec3& dir)>;
/** @brief Sonda de densidad por mapa [0,1] en una dirección (1 = sin mapa). La aporta el
 *  orquestador cargando el densityMap de la capa; el adaptador solo traduce el parche a esfera. */
using PropDirDensity = std::function<float(const std::string& mapPath, const glm::vec3& dir)>;
/** @brief Sonda de ZONA en una dirección: nombre del material del zoneMap (vacío = sin zona/mapa).
 *  La aporta el orquestador (TerrestrialPlanet::zoneNameAt); el adaptador solo traduce el parche. */
using PropDirZone = std::function<std::string(const glm::vec3& dir)>;
/** @brief Sonda de MATERIAL del terreno en una dirección (p.ej. "sand"). La aporta el orquestador
 *  (TerrestrialPlanet::materialNameAt); alimenta la condición booleana `when` de cada capa. */
using PropDirLayer = std::function<std::string(const glm::vec3& dir)>;

/**
 * @brief Convierte un parche plano local en el muestreador plano que consume el placer de props.
 *
 *  Comunica:
 *    · `configure(centerDir, radius, u, v)` — ancla el parche: `centerDir` es la dirección del
 *      CENTRO del parche, `radius` el radio del planeta (m), `u`/`v` su base tangente ortonormal.
 *    · `sampleAt(px,pz)` / `heightAt(px,pz)` — pregunta a los callbacks por la dirección que
 *      corresponde a ese punto del parche.
 *
 *  La proyección es tangente-planar (válida para parches ≪ del planeta: un poblado, una granja):
 *  cada metro local del parche corresponde a `1/radius` radianes de desplazamiento angular.
 */
class TerrainPropField : public Haruka::Tools::ProcGraph::IPropField {
public:
    /**
     * @brief Ancla el parche sobre la esfera.
     * @param centerDir  dirección unitaria del CENTRO del parche (origen en el centro del planeta).
     * @param radius     radio del planeta (m): convierte metros locales en desplazamiento angular.
     * @param u, v       base tangente ortonormal: u = dirección de px, v = dirección de pz.
     */
    void configure(const glm::vec3& centerDir, double radius,
                   const glm::vec3& u, const glm::vec3& v) {
        m_c = glm::normalize(centerDir); m_R = radius;
        m_u = u; m_v = v;
    }

    /** @brief La ecología de un punto del parche: FieldSample de su dirección. */
    Haruka::FieldSample sampleAt(float px, float pz) const override {
        return fieldFn ? fieldFn(dirAt(px, pz)) : Haruka::FieldSample{};
    }
    /** @brief Cota (m) del suelo en un punto del parche. */
    float heightAt(float px, float pz) const override {
        return heightFn ? heightFn(dirAt(px, pz)) : 0.0f;
    }
    /** @brief Densidad por mapa [0,1] del punto del parche (1 = sin mapa). `mapPath` identifica
     *  QUÉ mapa (cada capa puede llevar el suyo): se reenvía al callback del orquestador. */
    float mapDensityAt(float px, float pz, const std::string& mapPath) const override {
        return mapFn ? mapFn(mapPath, dirAt(px, pz)) : 1.0f;
    }
    /** @brief Zona (nombre de material del zoneMap) del punto del parche. Se reenvía al callback
     *  del orquestador, que conoce el mapa y la tabla de materiales del planeta. */
    std::string zoneAt(float px, float pz) const override {
        return zoneFn ? zoneFn(dirAt(px, pz)) : std::string{};
    }
    /** @brief Material del terreno del punto del parche. Alimenta la condición `when`. */
    std::string layerAt(float px, float pz) const override {
        return layerFn ? layerFn(dirAt(px, pz)) : std::string{};
    }

    PropDirField  fieldFn;      ///< ecología por dirección (la aporta el orquestador)
    PropDirHeight heightFn;     ///< cota por dirección (la aporta el orquestador)
    PropDirDensity mapFn;       ///< densidad por mapa por dirección (la aporta el orquestador)
    PropDirZone   zoneFn;       ///< zona por dirección (la aporta el orquestador)
    PropDirLayer  layerFn;      ///< material por dirección (la aporta el orquestador)

private:
    /** @brief Dirección unitaria del punto del parche (px,pz): arco de `px·arc` por `u`, etc. */
    glm::vec3 dirAt(float px, float pz) const {
        const double arc = 1.0 / m_R;                    // rad de un metro local
        return glm::normalize(m_c
            + m_u * (px * (float)arc)
            + m_v * (pz * (float)arc));
    }

    glm::vec3 m_c{1, 0, 0};
    glm::vec3 m_u{0, 1, 0};
    glm::vec3 m_v{0, 0, 1};
    double    m_R = 1.0;
};

} // namespace Haruka