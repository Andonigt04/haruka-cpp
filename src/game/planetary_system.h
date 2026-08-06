/**
 * @file planetary_system.h
 * @brief Planetas, órbitas, terreno, clima y render SimplePlanet.
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
 */
#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include "tools/math_types.h"
#include "planet.h"                            // TerrestrialPlanet (el planeta se crea a sí mismo)
#include "core/scene/scene_manager.h"
#include "core/terrain/terrain_sample.h"     // WorldGenParams
#include "core/terrain/reference_surface.h"  // ReferenceSurface
#include "core/weather_system.h"              // WeatherSystem
#include "core/ground_layer.h"                // GroundMaterial
#include "tools/planetary_types.h"           // PlanetFace (GL-free)
#include "rhi/rhi_types.h"

namespace Haruka { namespace Planet {
    struct GeologyConfig;
    struct TerrainGridConfig;
}}

namespace Haruka {

/**
 * @brief Sistema planetario: órbitas, terreno, clima y renderizado simple.
 *
 * Gestiona la lista de planetas (con órbitas Kepler analíticas), el terreno
 * muestreable (ReferenceSurface + altura editada), el clima global (WeatherSystem),
 * y los planetas "SimplePlanet" para testing/rendering con texturas procedurales.
 */
class PlanetarySystem {
public:
    PlanetarySystem();
    ~PlanetarySystem();

    /** @brief Configuración de superficie: tiling de textura y resolución procedural. */
    struct SurfaceConfig {
        float       tiling = 100.0f;
        int         texRes = 512;   // biome map procedural texture resolution (width); height = width/2
        int         macroRes = 2048; // macro-variation texture resolution (width); height = width/2
        /**
         * @brief Fracción de la superficie por ENCIMA del nivel del mar [0,1].
         *
         * El nivel del mar se DERIVA de esto (cuantil de la distribución de alturas), no al revés.
         * Así "cuánta tierra tiene el planeta" es un número que se declara y se cumple, en vez de
         * lo que salga del reparto de placas de esa semilla. 0.29 = la Tierra.
         */
        float       landFraction = 0.29f;
        /**
         * @brief Mapa EQUIRECTANGULAR de zonas (PNG con colores planos), o vacío.
         *
         * Cada color corresponde al `zone` de un material de `materials`. Cuando existe, decide
         * qué material hay en cada punto y qué está bajo el agua — y entonces `landFraction` deja
         * de usarse: el mar lo dibuja el mapa, no una estadística.
         */
        std::string zoneMap;

        /**
         * @brief Mapa EQUIRECTANGULAR de ELEVACIÓN (PNG en gris), o vacío.
         *
         * El canal rojo, 0..255, se mapea linealmente a `elevationRange` en metros. Sustituye al
         * relieve de las placas, que es casi BINARIO —fondo oceánico o meseta continental— y por eso
         * no puede tener cordilleras: casi toda la tierra acaba a la misma cota.
         *
         * A 4096² son ~9,8 km por téxel. Suena basto y no lo es para este uso: la malla base tiene
         * un vértice cada 39 km, así que el mapa es 4× más fino que lo que puede representar. El
         * relieve por debajo de eso lo pone el detalle procedural.
         */
        std::string elevationMap;
        /** @brief Metros a los que corresponden el 0 y el 255 del mapa. */
        glm::vec2   elevationRange = glm::vec2(-4000.0f, 4000.0f);
    };

    /** @brief Planeta con órbita Kepler analítica. */
    struct Planet {
        std::string name;
        Haruka::WorldPos position;
        double radius;
        SurfaceConfig surface;              // texture-driven surface config
        bool isHome = false;                // planeta del jugador (flags.originShiftingTarget)
        uint32_t seed = 0;                  // semilla auto-generada para generación determinista

        // --- ÓRBITA (Kepler ANALÍTICO): posición = función del tiempo → ESTABLE (sin deriva de
        // integración) y depurable. orbitParent = índice del cuerpo central (Sol) en m_planets, o -1
        // (estático). Plano orbital = base ortonormal (u,v); foco en el cuerpo padre. ---
        int    orbitParent = -1;          // índice del cuerpo central en m_planets (-1 = estático)
        double orbitA      = 0.0;         // semi-eje mayor (m)
        double orbitEcc    = 0.0;         // excentricidad [0,1) (0 = círculo)
        double orbitPeriod = 0.0;         // periodo (s); <=0 = no orbita
        double orbitPhase  = 0.0;         // anomalía media en t=0 (rad)
        glm::dvec3 orbitU  = glm::dvec3(1, 0, 0); // eje del periastro (plano orbital)
        glm::dvec3 orbitV  = glm::dvec3(0, 0, 1); // eje perpendicular en el plano (sentido del avance)
    };

    void init();
    /** @brief Actualiza órbitas, clima, y SimplePlanets. @param dt Delta time en segundos. */
    void update(double dt, const glm::dvec3& cameraPos);

    /** @brief Configura órbita Kepler para un planeta. @return false si no encuentra planetName. */
    bool setPlanetOrbit(const std::string& planetName, const std::string& parentName,
                        double periodSeconds, double ecc = 0.0);

    /** @brief Obtiene centro y radio del planeta activo (el más cercano a la cámara). */
    bool getActivePlanet(glm::dvec3& center, double& radius) const;
    /** @brief Overload de compatibilidad (ignora reliefStrength). */
    bool getActivePlanet(glm::dvec3& center, double& radius, float& /*reliefStrength*/) const {
        return getActivePlanet(center, radius);
    }
    /** @brief Nombre del planeta activo. */
    std::string getActivePlanetName() const;

    /** @brief TerrestrialPlanet del planeta activo (el que da el campo ecológico + cota de props).
     *  Busca por NOMBRE (m_planets y m_simplePlanets no comparten índice cuando el planeta se
     *  añadió por `addSimplePlanet`). nullptr si no hay planeta con superficie. */
    const Haruka::Planet::TerrestrialPlanet* activeTerrestrial() const;

    /** @brief Parámetros de generación del planeta activo. */
    bool getActivePlanetParams(Haruka::WorldGenParams& out, double& outRadius) const;

    /** @brief Constante para "sin agua". */
    static constexpr double kNoWater = -1e30;
    /** @brief Nivel del agua en un punto del mundo (kNoWater si no hay). */
    double sampleWaterLevel(const glm::dvec3& worldPos) const;

    /** @brief Cobertura de suelo en un punto. */
    struct GroundCover {
        Haruka::GroundMaterial material = Haruka::GroundMaterial::None;
        float                  amount   = 0.0f;
    };
    GroundCover groundCoverAt(const glm::dvec3& worldPos, float snowAccum) const;

    const std::vector<Planet>& getPlanets() const { return m_planets; }
          std::vector<Planet>& getPlanets()       { return m_planets; }

    /** @brief Sincroniza desde SceneManager (carga/reloading). */
    void syncFromScene(const SceneManager& scene);
    /** @brief Construye planetas desde los objetos de escena con surfaceConfig. */
    void buildFromScene(SceneManager& scene);
    /**
     * @brief Regenera UN planeta leyendo SU surfaceConfig actual del SceneManager.
     *
     * Camino del editor: cambias seed/landFraction/mapas en el objeto y este método lo relee
     * (misma interpretación que `buildFromScene`) y reconstruye ese TerrestrialPlanet. Si la
     * escena aún no tiene objeto con ese nombre, no hace nada.
     */
    void updatePlanetFromScene(SceneManager& scene, const std::string& name);

    /** @brief Vista de depuración para TODOS los planetas renderizables (ver TerrestrialPlanet::setDebugView). */
    void setDebugView(int view);
    /** @brief Vista de depuración del planeta activo. */
    int  debugView() const;

    /**
     * @brief Nombres de las CAPAS de textura del planeta activo, en orden de capa.
     *
     * Solo los materiales con albedo (textura) son una capa del array de terreno; los que no
     * (hielo, sal, agua…) no aparecen. El editor los lista en el selector de capas para revelar
     * cómo se aplica cada textura. Vacío si no hay planeta.
     */
    std::vector<std::string> activeTerrainLayerNames() const;

    /**
     * @brief Nombres de las CAPAS DE PROPS del planeta activo, en orden de prioridad.
     *
     * Las lista el editor en el selector de vista para pintar el ÁREA DE SPAWN de cada capa
     * (dónde instalaría esa capa sus objetos: bandas de clima/forma × densityMap). Vacío si el
     * planeta no declara `propLayers`.
     */
    std::vector<std::string> activePropLayerNames() const;

    /**
     * @brief Stats de geometría del último frame, sumadas sobre TODOS los SimplePlanets.
     * Lo consume el panel de performance del editor (sustituye al "Chunk Streaming" legacy).
     */
    Haruka::Planet::TerrestrialPlanet::RenderStats getTerrainRenderStats() const;

    double simulationTime() const { return m_simulationTime; }
    const Haruka::WeatherSystem& weather() const { return m_weather; }
    Haruka::WeatherSystem&       weatherMutable() { return m_weather; }

    /** @brief Muestra de clima (temp, humedad, precipitación) en un punto. */
    Haruka::WeatherSample weatherAt(const glm::dvec3& worldPos) const;

    double sampleTerrainHeight(const glm::dvec3& worldPos) const;
    bool groundHeightKmAtDir(const glm::dvec3& dir, float& outElevKm) const;

    /// Dynamic terrain editing: add/subtract height within a radius (meters)
    void editTerrain(const glm::dvec3& worldPos, double radius, double step, bool dig);
    /// Flatten terrain to a target height (meters) within a radius
    void levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeight);

    Haruka::ReferenceSurface& referenceSurface() const { return m_refSurface; }
    /** @brief Muestrea la superficie (altura, normal, material) en un punto del mundo. */
    Haruka::TerrainSample sampleSurface(const glm::dvec3& worldPos) const;
    bool getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const;

    // ── SimplePlanet ──────────────────────────────────────────────────────
    /**
     * @brief Planeta de terreno simple (una malla, texturas procedurales).
     *
     * Alias de `Haruka::Planet::TerrestrialPlanetConfig` — la identidad + config cruda con la que
     * un planeta SE CREA A SÍ MISMO. La API pública (getSimplePlanet/renderSimplePlanet) no cambia:
     * el nombre y el radio que leían los consumidores siguen ahí, y `seed`/`raw`/`faceRes` son
     * campos extra que la escena puede (o no) rellenar.
     */
    using SimplePlanet = Haruka::Planet::TerrestrialPlanetConfig;

    /**
     * @brief Añade un SimplePlanet para rendering.
     * @param orbit Configuración orbital y de superficie.
     * @param geo   Configuración geológica (placas, erosión).
     * @param grid  Configuración de la malla (faceRes, etc.).
     */
    void addSimplePlanet(const SimplePlanet& orbit,
                         const Haruka::Planet::GeologyConfig& geo,
                         const Haruka::Planet::TerrainGridConfig& grid);

    /**
     * @brief Reconstruye el terreno de un SimplePlanet (nueva semilla).
     */
    void rebuildSimplePlanet(const std::string& name,
                             const Haruka::Planet::GeologyConfig& geo,
                             const Haruka::Planet::TerrainGridConfig& grid);

    /**
     * @brief Renderiza un SimplePlanet (pipelines RHI con shaders inline).
     */
    void renderSimplePlanet(const std::string& name, const glm::dvec3& cameraPos,
                            const glm::mat4& proj, const glm::mat4& view);

    /**
     * @brief Propaga la luz del sol a todos los SimplePlanet.
     *
     * La llama el orquestador cada frame con la luz real de la escena (WorldSystem); sin ella los
     * planetas iluminaban con una dirección fija y el terreno no respondía al sol del cielo.
     */
    void setSunLight(const glm::vec3& dir, const glm::vec3& color, float ambientStrength);

    size_t getSimplePlanetCount() const { return m_simplePlanets.size(); }
    /** @brief Config del SimplePlanet por índice. */
    const SimplePlanet& getSimplePlanet(size_t i) const;

    /** @brief Genera chunks LOD @param name Nombre del planeta. @param lod Nivel LOD. */
    void generateSimpleLOD(const std::string& name, int lod);

private:
    std::vector<Planet> m_planets;
    std::vector<std::unique_ptr<Haruka::Planet::TerrestrialPlanet>> m_simplePlanets;
    double m_simulationTime = 0.0;

    // EL SUELO DEL JUEGO (ver sampleTerrainHeight)
    mutable Haruka::ReferenceSurface m_refSurface;
    void ensureReferenceSurface() const;

    // Alturas editadas (deformación dinámica del terreno)
    // Key = hash de la dirección del punto, Value = offset en metros sobre la altura base
    mutable std::unordered_map<uint64_t, float> m_heightEdits;
    /// Computes a spatial key for a direction (face + grid coords at reference resolution)
    static uint64_t dirToHeightKey(const glm::dvec3& dir, int refLod, int chunkSize);
    void rebuildPlanetMeshes();
    static void editHeightsInRadius(std::unordered_map<uint64_t, float>& edits,
                                    const glm::dvec3& center, double radius,
                                    const std::vector<Planet>& planets,
                                    int refLod, int chunkSize,
                                    std::function<float(float)> modifyFn);

    // CLIMA del mundo + caché del clima del CAMPO
    mutable Haruka::WeatherSystem m_weather;
    mutable glm::dvec3            m_weatherFieldPos{1e300};
    mutable float                 m_weatherFieldTempC = 15.0f;
    mutable float                 m_weatherFieldHumid = 0.5f;
    const double G = 6.67430e-11;

    void updateOrbits(double dt);
    void updateSimpleOrbits(double dt);
};

}