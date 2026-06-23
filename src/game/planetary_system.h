#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "core/modules.h"
#include "tools/math_types.h"
#include "core/scene/scene_manager.h"
#include "core/terrain/terrain_sample.h"     // WorldGenParams (terreno)
#include "tools/planetary_types.h"           // PlanetChunkKey (descarga diferida)
#include "core/lod_system.h"                  // LODUpdate (cache del último set)

namespace Haruka { namespace Renderer { class Texture; } } using Haruka::Renderer::Texture;

namespace Haruka {

class ChunkCache;
class TerrainGenerator;
class TerrainRenderer;
class WaterRenderer;
class FloatingIslandRenderer;
class TerrainStreamingSystem;
class LODSystem;
class DeformationField;

class PlanetarySystem {
public:
    PlanetarySystem();
    ~PlanetarySystem();

    // Estructura limpia para un planeta
    struct Planet {
        std::string name;
        Haruka::WorldPos position;
        double radius;
        nlohmann::json terrainSettings; // Semilla, capas de ruido, etc.
        bool isHome = false;            // planeta del jugador (flags.originShiftingTarget)
        // Aquí podrías añadir parámetros orbitales (semi-eje mayor, etc.)
    };

    void init();

    /** @brief Actualiza órbitas, LOD y streaming. */
    void update(double dt, const glm::dvec3& cameraPos);

    void addPlanet(const Planet& planet);

    /** @brief Renderiza el terrain de un planeta (shader ya activo, UBO model ya subido). */
    void renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Renderiza el océano de un planeta (shader de agua ya activo). */
    void renderPlanetWater(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Renderiza las islas flotantes (reusa el shader de planeta, ya activo). */
    void renderPlanetIslands(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Datos del planeta v2 activo (el que tiene props/recursos) para que el JUEGO
     *  genere su sistema de recursos. center/radius/seed/relief. false si no hay. Los
     *  recursos (árboles/rocas/minerales) son del juego, no del motor. */
    bool getActivePlanet(glm::dvec3& center, double& radius, uint32_t& seed, float& reliefStrength) const;

    /** @brief Sets camera-relative VP for frustum culling terrain+water chunks. */
    void setTerrainCullMatrix(const glm::mat4& camRelViewProj);

    int getGPUWaterChunkCount() const;

    const std::vector<Planet>& getPlanets() const { return m_planets; }
          std::vector<Planet>& getPlanets()       { return m_planets; }

    void syncFromScene(const SceneManager& scene);

    /** @brief Construye TODOS los cuerpos celestes desde la escena: planetas (los
     *  que tienen terrainSettings → terreno por chunks/LOD) y cuerpos con geometría
     *  PROCEDURAL (p.ej. la Luna: esfera + cráteres, vía properties.proceduralMesh).
     *  Punto de entrada único — toda la interpretación escena→cuerpos vive aquí, no
     *  en Application. Requiere contexto GL (genera mallas). */
    void buildFromScene(SceneManager& scene);

    int getGPUChunkCount()    const;
    int getPendingChunks()    const;  // chunks generating async (in-flight, few)
    int getQueuedChunks()     const;  // chunks still queued to generate (the real backlog)
    int getCachedChunks()     const;
    int getCacheMemoryMB()    const;
    int getCacheMaxMemoryMB() const;
    /** @brief Sets the chunk cache memory budget (MB). Evicts immediately if over. */
    void setCacheMaxMemoryMB(int mb);
    /** @brief Tunes terrain LOD detail (lower = fewer chunks = cheaper). */
    void setLODParams(double splitFactor, int maxLOD);

    struct TerrainDrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
    TerrainDrawStats getTerrainDrawStats() const;

    /** @brief Valida los invariantes del LOD este frame (cobertura sin huecos/solapes,
     *  balance 2:1) y devuelve un resumen legible. Para el comando de consola `lodcheck`. */
    std::string validateLOD() const;
    /** @brief Modo vigilancia: si está activo, update() valida cada frame y avisa por
     *  stderr SOLO cuando el LOD es inválido (throttled). Para `lodcheck watch`. */
    void setLODValidateWatch(bool on) { m_lodWatch = on; }
    bool getLODValidateWatch() const { return m_lodWatch; }

    /**
     * @brief Applies a terrain edit (dig/crater/build) at a world position and
     *        regenerates the affected chunks so the mesh updates immediately.
     * @param worldPos centre of the edit
     * @param radius   metres of influence
     * @param strength metres of displacement (positive); dig=true subtracts.
     */
    void editTerrain(const glm::dvec3& worldPos, double radius, double strength, bool dig);

    /**
     * @brief Nivela (aplana) el terreno hacia una altura objetivo en un radio. El centro
     *        queda exactamente a targetHeightM y los bordes mezclan suavemente.
     * @param targetHeightM elevación destino en metros sobre la esfera de referencia.
     */
    void levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeightM);

    /**
     * @brief Nivela con la HUELLA (caja orientada) de un objeto en vez de un círculo:
     *        el área aplanada tiene el tamaño/forma/orientación del objeto colocado.
     * @param halfExtents medias extensiones de la huella (m), en el espacio de rot.
     * @param rot         orientación de la caja (columnas = ejes locales del objeto).
     * @param band        ancho de transición del borde (m).
     */
    void levelTerrainBox(const glm::dvec3& center, const glm::dvec3& halfExtents,
                         const glm::dmat3& rot, double targetHeightM, double band);

    /** @brief The terrain-edit field (null until init, or always null if the
     *  DEFORM module is compiled out). For save/restore of edits. */
    DeformationField* deformationField() {
#ifdef HARUKA_MOD_DEFORM
        return m_deform.get();
#else
        return nullptr;
#endif
    }

    /** @brief Drops all GPU terrain chunks so they regenerate (e.g. after bulk
     *  restoring deformation brushes from a save). */
    void invalidateAllChunks();

    /**
     * @brief Returns the terrain height (in metres) above the reference sphere
     *        surface at the given world position, for the nearest planet.
     *        Returns 0 if no planet is found or terrain settings are missing.
     */
    double sampleTerrainHeight(const glm::dvec3& worldPos) const;

    /** @brief (F3) Muestreo CANÓNICO de superficie en una posición de mundo: devuelve
     *  el TerrainSample completo (elev, agua, wetness, clima…) del planeta más cercano,
     *  con deform aplicado. Colisión, puntería, agua y siembra deben usar ESTO. */
    Haruka::TerrainSample sampleSurface(const glm::dvec3& worldPos) const;

    /** @brief (F3) Params EXACTOS del generador para el planeta ACTIVO (home): seed +
     *  reliefStrength + profile + radio. Fuente única de params → el juego no re-deriva
     *  los suyos (evita divergencias, p.ej. el profile). Devuelve false si no hay planeta. */
    bool getActivePlanetParams(Haruka::WorldGenParams& out, double& outRadius) const;

    /**
     * @brief Mean sea surface for the nearest planet (sea level = planet radius).
     * @return true if a planet was found.
     */
    bool getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const;

private:
    // Invalida/regenera los chunks que toca una edición de terreno (centro + radio).
    void invalidateEditedChunks(const glm::dvec3& center, double radius);

    // Componentes del motor de terreno (Los "músculos")
    std::unique_ptr<ChunkCache> m_cache;
    std::unique_ptr<TerrainGenerator> m_generator;
    std::unique_ptr<TerrainRenderer> m_renderer;
    std::unique_ptr<WaterRenderer> m_waterRenderer;
    std::unique_ptr<FloatingIslandRenderer> m_islandRenderer;
    bool       m_islandsGenerated = false;
    glm::dvec3 m_islandGenCamDir{0.0}; // dirección cámara en la última (re)generación de islas

    // (Los recursos —árboles/rocas/minerales/cosecha— se movieron al juego: Survival
    //  ResourceSystem. El motor solo expone el planeta activo vía getActivePlanet.)

    // Texturas de bioma del terreno (OPCIONALES, declaradas en la escena
    // terrainSettings.textures). Sin config → terreno procedural. El tier de
    // resolución se elige por TextureQuality. Cargadas/bindeadas aquí (no en el engine core).
    std::unique_ptr<Texture> m_texSandAlbedo,  m_texSandNormal;
    std::unique_ptr<Texture> m_texGrassAlbedo;
    std::unique_ptr<Texture> m_texLandAlbedo,  m_texLandNormal;
    int         m_texTier = -1;        // tier cargado (px); -1 = aún no
    std::string m_texDir;              // dir base de la última carga
    void bindTerrainTextures(const Planet& planet); // carga diferida + bind + uniforms
    std::unique_ptr<TerrainStreamingSystem> m_streaming;
    std::unique_ptr<LODSystem> m_lod;
    // Último set deseado por planeta. Cuando el LOD está en THROTTLE (cámara quieta)
    // NO recalculamos, pero SÍ reprocesamos este último set para SUBIR a GPU lo que
    // se haya generado mientras tanto (si no, los chunks no aparecen hasta moverte).
    std::vector<LODUpdate> m_lastUpdates;
    // Ventana de "catch-up": solo subimos lo recién generado mientras haya generación
    // activa (+1 s de margen). Cuando todo está cargado y la cámara quieta, el
    // catch-up se apaga → sin el pico periódico de subida.
    int m_catchupGrace = 0;
#ifdef HARUKA_MOD_DEFORM
    std::unique_ptr<DeformationField> m_deform; // player terrain edits
#endif

    // Datos del universo
    std::vector<Planet> m_planets;
    double m_simulationTime = 0.0;
    const double G = 6.67430e-11;

    // LOD throttle: skip the (allocating) quadtree rebuild when the camera has
    // barely moved since the last recompute. Per-planet last cam pos; force on
    // first frame / new planet.
    std::vector<glm::dvec3> m_lastLODCamPos;
    bool m_forceLOD = true;
    bool m_lodWatch = false; // valida el LOD cada frame y avisa al volverse inválido

    // Predicción de movimiento: velocidad suavizada de la cámara para PEDIR chunks
    // por delante del jugador (lookahead) → menos pop-in al moverse/volar rápido. El
    // render usa la cámara REAL; solo el LOD/streaming mira el punto adelantado.
    glm::dvec3 m_prevCamPos{0.0};
    glm::dvec3 m_camVel{0.0};
    bool       m_havePrevCam = false;

    void updateOrbits(double dt);
};

}