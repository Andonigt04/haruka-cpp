#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "core/terrain/cave_system.h"   // CaveMap (el mapa gris horneado) y el patrón de cache

/**
 * @file island_system.h
 * @brief Islas flotantes: volúmenes de ROCA SOBRE la superficie, definidos por MAPAS EN FICHEROS.
 *
 * Es el espejo de `cave_system.h`. Una cueva es aire que se QUITA al campo del mundo; una isla es
 * roca que se PONE. Las dos son el mismo gesto que el pico del jugador (un trazo, ver
 * `VoxWorld::stroke`), puestas de antemano porque son deterministas; y las dos se definen igual:
 * una función pura hornea unos mapas, se persisten en `bakes/` con una clave que incluye todo lo
 * que los afecta, y a partir de ahí **el fichero es la fuente de verdad** (se pueden pintar).
 *
 * Los tres ficheros de una isla, en `bakes/`:
 *
 *   `island_<clave>_planta.png` (gris) — la SILUETA vista desde arriba, como distancia con signo a su
 *                                        borde (0,5 = el borde, blanco = 24 m dentro, negro = 24 m fuera).
 *                                        Es lo que tiene sentido pintar a mano.
 *   `island_<clave>_cima.png`   (gris) — el RELIEVE de la cima: fracción de `topM` sobre la base.
 *   `island_<clave>_raiz.png`   (gris) — la RAÍZ: fracción de `rootM` bajo la base (el cono).
 *
 * El campo es analítico sobre los tres mapas (`islandDensityAt`): no hace falta volumen 3D porque
 * cima y raíz son dos alturas sobre la misma planta, y una isla no tiene conectividad que garantizar
 * más allá de "es UNA pieza" (se garantiza en la planta: componente mayor).
 *
 * ⚠️ DÓNDE VIVE. La isla NO tiene malla ni colisión propias: se rasteriza en el canal de ROCA AÑADIDA
 * de los chunks (`VoxChunk::a`) igual que la cueva se rasteriza en el de aire, y a partir de ahí la
 * malla, la colisión y el render son los del campo — los mismos que las cuevas y que lo que
 * construye el jugador. Una isla es "lo que sería levantar un parapeto, pero a 200 m de altura".
 */
namespace Haruka {

    /** @brief Marco local de una isla: una caja centrada en su BASE, flotando sobre el relieve.
     *
     *  Coordenadas locales `(x, y, h)` en METROS: `x`,`y` tangentes, `h` altura SOBRE la base
     *  (positiva hacia arriba; la raíz está en h < 0). */
    struct IslandBox {
        glm::dvec3 centerDir{0.0, 1.0, 0.0};
        glm::dvec3 tangentU{1.0, 0.0, 0.0};
        glm::dvec3 tangentV{0.0, 0.0, 1.0};
        float  radiusM   = 0.0f;   ///< medio lado tangencial de la caja (m)
        float  topM      = 0.0f;   ///< relieve máximo de la cima sobre la base (m)
        float  rootM     = 0.0f;   ///< profundidad máxima de la raíz bajo la base (m)
        float  altitudeM = 0.0f;   ///< base sobre la COTA DEL TERRENO en el centro (m)
        /// Radio del planeta + cota del terreno en el centro + `altitudeM` = radio de la BASE.
        /// Double por lo mismo que `CaveBox::surfaceRM`: medio metro de ulp en float.
        double baseRM = 0.0;
        uint32_t cell = 0;
        uint32_t seed = 0;

        glm::vec3  toLocal(const glm::dvec3& p) const;
        glm::dvec3 toWorld(const glm::vec3& local) const;
        /// ¿Cae dentro de la caja (con el margen vertical del campo saturado)?
        bool contains(const glm::dvec3& p) const;
    };

    struct IslandSystem {
        IslandBox box;
        CaveMap   planta;   ///< distancia con signo a la silueta, [0,1] ↔ ±kIslandRangeM
        CaveMap   cima;     ///< fracción de topM
        CaveMap   raiz;     ///< fracción de rootM
        /// Estadística del horneado: x = componentes de la silueta ANTES de quedarse con la mayor ·
        /// y = texels de isla · z = 0.
        glm::ivec3 stats{0, 0, 0};
        bool valid() const { return !planta.empty() && !cima.empty() && !raiz.empty(); }
    };

    /** @brief La definición de una isla DE LA ESCENA (`"type": "Island"`), sin semilla: centro,
     *  tamaños y sus tres mapas como assets por ruta. Espejo de `CaveDef`. */
    struct IslandDef {
        std::string name;
        glm::dvec3  centerDir{0.0, 1.0, 0.0};
        float       radiusM = 260.0f, topM = 30.0f, rootM = 110.0f, altitudeM = 200.0f;
        std::string planta, cima, raiz;          ///< rutas PNG; vacías o inexistentes = borrador
        uint32_t    draftSeed = 0;
    };
    IslandBox islandBoxFromDef(const IslandDef& def, double planetRadiusM,
                               float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);
    /** @brief Carga una isla de la escena: mapas de sus rutas, o borrador guardado en ellas si
     *  faltan. No hay nada derivado que cachear (el campo es analítico sobre los mapas). */
    bool islandLoad(const IslandDef& def, IslandSystem& out, double planetRadiusM, int mapRes = 256,
                    float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);
    bool islandLoadBox(const IslandDef& def, const IslandBox& box, IslandSystem& out, int mapRes = 256);

    /// Lado angular de la celda de siembra de islas (rad). 0,0032 rad ≈ 20 km en la Tierra.
    inline constexpr double kIslandCellRad = 0.0032;
    /// Lado de los mapas.
    inline constexpr int    kIslandMapRes  = 256;
    /// Rango del campo en el PNG de planta: ±24 m, el mismo tope que el volumen de las cuevas.
    inline constexpr float  kIslandRangeM  = 24.0f;

    /// Siembra automática (fracción de celdas). Defecto CERO: las islas se colocan a mano, como las
    /// cuevas (`VoxWorld::placeIsland`).
    void  islandSetAutoProbability(float p);
    float islandAutoProbability();

    /** @brief La caja de la celda que contiene `dir`, exista o no isla sembrada (para colocarla a
     *  mano). La forma sale de (seed, celda), determinista. */
    void islandBoxForCell(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, IslandBox& out,
                          float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);
    /** @brief ¿Siembra isla la celda de `dir`? Si sí, su caja. */
    bool islandBoxAt(uint32_t seed, const glm::dvec3& dir, double planetRadiusM, IslandBox& out,
                     float (*surfaceElevM)(const glm::dvec3&, void*) = nullptr, void* user = nullptr);

    /** @brief Hornea los tres mapas. Pura: misma caja → mismos texels. */
    void islandBakeMaps(const IslandBox& box, int res, CaveMap& planta, CaveMap& cima, CaveMap& raiz,
                        glm::ivec3* outStats = nullptr);

    std::string islandCacheKey(const IslandBox& box, int mapRes);

    /** @brief Carga de `bakes/` o hornea y guarda. */
    bool islandLoadOrBake(const IslandBox& box, IslandSystem& out, const std::string& dir = std::string(),
                          int mapRes = kIslandMapRes);

    /** @brief El campo de la isla en un punto: > 0 ROCA (dentro), < 0 aire, metros aprox.
     *  Fuera de la caja devuelve −1e9 ("aquí no hay isla"): el centinela es de AIRE, el contrario
     *  al de la cueva, porque la isla es lo que se añade. Quien lo combine tiene que saturarlo. */
    float islandDensityAt(const IslandSystem& sys, const glm::dvec3& p);

    /** @brief Alturas de cima y raíz (m sobre la base, raíz negativa) en una dirección; false fuera
     *  de la silueta. Es lo que consulta quien quiera "el suelo de la isla" sin muestrear el campo. */
    bool islandColumnAt(const IslandSystem& sys, const glm::dvec3& dir, float& topH, float& bottomH);

} // namespace Haruka
