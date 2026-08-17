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
#include "core/weather_system.h"              // WeatherSystem
#include "core/ground_layer.h"                // GroundMaterial
#include "core/planet/orbit.h"                // OrbitElements (Kepler con elementos precesantes)
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
        // ⚠️ La FUENTE DE VERDAD del plano orbital son los ELEMENTOS, no una base (u,v) guardada.
        // Con la base congelada al cargar la escena la órbita no puede precesar, y sin precesión es
        // exactamente periódica: la misma elipse para siempre, que es lo que se lee como "rail".
        // Ver core/planet/orbit.h. `orbitA/Ecc/Period/Phase` siguen aquí porque son parte de los
        // elementos y hay API pública que los expone.
        double orbitA      = 0.0;         // semi-eje mayor (m)
        double orbitEcc    = 0.0;         // excentricidad MEDIA [0,1) (0 = círculo)
        double orbitPeriod = 0.0;         // periodo (s); <=0 = no orbita
        double orbitPhase  = 0.0;         // anomalía media en t=0 (rad)
        Haruka::Planet::OrbitElements orbit;   // elementos completos + tasas de precesión
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
    /** @brief Nombre del planeta activo, por REFERENCIA.
     *
     *  ⚠️ Devolvía `std::string` por VALOR, y está en el camino caliente: `sampleTerrainHeight` lo
     *  llama para elegir el planeta, y el solver de partículas del fluido llama a `sampleTerrainHeight`
     *  ~1600 veces por frame (400 partículas × 3 iteraciones de densidad). Una asignación de heap por
     *  muestreo de terreno, para comparar un nombre que no cambia. */
    const std::string& getActivePlanetName() const;

    /** @brief TerrestrialPlanet del planeta activo (el que da el campo ecológico + cota de props).
     *  Busca por NOMBRE (m_planets y m_simplePlanets no comparten índice cuando el planeta se
     *  añadió por `addSimplePlanet`). nullptr si no hay planeta con superficie. */
    const Haruka::Planet::TerrestrialPlanet* activeTerrestrial() const;
    /** @brief Igual, MUTABLE: hace falta para entregarle al planeta activo el suelo cercano que
     *  monta la física (`setNearGroundRing`). Delega en la versión const — una sola búsqueda. */
    Haruka::Planet::TerrestrialPlanet* activeTerrestrialMut() {
        return const_cast<Haruka::Planet::TerrestrialPlanet*>(activeTerrestrial());
    }

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
    double sampleTerrainHeight(const glm::dvec3& worldPos, float minFeatureM) const;
    bool groundHeightKmAtDir(const glm::dvec3& dir, float& outElevKm) const;

    /// Dynamic terrain editing: add/subtract height within a radius (meters)
    void editTerrain(const glm::dvec3& worldPos, double radius, double step, bool dig);
    /// Flatten terrain to a target height (meters) within a radius
    void levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeight);

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
    /** @brief Lanza el trabajo de COMPUTE de los planetas (culling de parches). DEBE llamarse
     *  ANTES de abrir el render pass de la escena: `vkCmdDispatch` dentro de un render pass es
     *  ilegal en Vulkan y cerraba el programa. Ver `TerrestrialPlanet::prepare`. */
    void prepareSimplePlanets(const glm::dvec3& cameraPos);

    void renderSimplePlanet(const std::string& name, const glm::dvec3& cameraPos,
                            const glm::mat4& proj, const glm::mat4& view);

    /**
     * @brief Propaga la luz del sol a todos los SimplePlanet.
     *
     * La llama el orquestador cada frame con la luz real de la escena (WorldSystem); sin ella los
     * planetas iluminaban con una dirección fija y el terreno no respondía al sol del cielo.
     */
    void setSunLight(const glm::vec3& dir, const glm::vec3& color, const glm::vec3& ambientColor);
    /** @brief Reparte los 9 coeficientes SH del cielo a todos los planetas. */
    void setSkyAmbientSH(const glm::vec3 (&coef)[9]);

    /** @brief Reparte a los planetas el estado de SUELO MOJADO/NEVADO y la máscara cenital con la que
     *  se recorta (la misma que usa la lluvia para saber si una gota está bajo cubierto).
     *  Ver `TerrestrialPlanet::setGroundWet`. */
    void setGroundWet(float wet, float snow, Haruka::RHI::TextureHandle skyMask,
                      const glm::mat4& skySpace);

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
    // ⚠️ AQUÍ VIVÍA `ReferenceSurface`: 12 KB de máquina —snapshot atómico, caché de 1 M entradas con
    // mutex, bilineal sobre retícula cube-sphere— cuyo `cornerM` acababa en `return 0.0`. Calculaba
    // CERO, y los dos llamantes reales la esquivaban con un comentario que lo decía. Colgaba de
    // `sampleTerrainV2`, que en esta rama es un stub.
    //
    // Lo único que se conserva es el paso de retícula (`terrainLatticeStep`, en el .cpp), porque la
    // clave de las deformaciones lo usa. Y esa vía es inerte de todas formas: ver `warnDeformationInert`.

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
public:
    /**
     * @brief Audita el sistema: ¿puede alguna pareja de órbitas cruzarse en algún instante?
     *
     * Con elementos precesantes `a` es constante y `e` está acotada, así que el radio de cada cuerpo
     * vive siempre en un intervalo fijo y la respuesta se puede DEMOSTRAR una vez para todo t (ver
     * core/planet/orbit.h). Se llama sola al resolver las órbitas de la escena.
     *
     * @return nº de problemas encontrados (0 = sistema limpio). Cada uno se registra con su causa.
     */
    int validateOrbits() const;
private:
};

}