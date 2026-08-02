/**
 * @file planet.h
 * @brief TerrestrialPlanet: un planeta que SE CREA A SÍ MISMO a partir de su config.
 *
 * Antes el planeta ("SimplePlanetInternal") era un struct privado de PlanetarySystem, y el sistema
 * hacía de TODO: interpretaba la config de superficie, elegía los materiales, calculaba el nivel del
 * mar, construía la malla, subía las texturas y renderizaba. Cada pieza nueva que el planeta
 * necesitaba (un mapa, un material, una biomePalette) se añadía como un paso más en el sistema.
 *
 * Ahora el planeta es una clase propia: recibe su identidad + su config cruda (el JSON de
 * `surfaceConfig` del SceneObject) y se construye SOLO. PlanetarySystem queda como orquestador fino:
 * decide por nombre qué planeta planificar/construir/actualizar/render, resuelve las cadenas de
 * órbita (que son un asunto del sistema, no de un planeta suelto) y delega el resto en el planeta.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

#include "tools/math_types.h"
#include "rhi/rhi_types.h"
#include "core/planet/geology.h"
#include "core/planet/climate.h"
#include "core/planet/biomes.h"
#include "core/planet/terrain_material.h"
#include "core/weather_system.h"
#include "tools/procgraph/proc_graph.h"
#include "tools/procgraph/proc_climate.h"

namespace Haruka { namespace Planet {

/** @brief Configuración de superficie: tiling de textura y resolución procedural. */
struct SurfaceConfig {
    float       tiling = 100.0f;
    int         texRes = 512;   // procedural texture resolution (width); height = width/2
    /**
     * @brief Fracción de la superficie por ENCIMA del nivel del mar [0,1].
     *
     * El nivel del mar se DERIVA de esto (cuantil de la distribución de alturas), no al revés.
     */
    float       landFraction = 0.29f;
    /**
     * @brief Mapa EQUIRECTANGULAR de zonas (PNG con colores planos), o vacío.
     *
     * Cada color corresponde al `zone` de un material de `materials`. Cuando existe, decide qué
     * material hay en cada punto y qué está bajo el agua.
     */
    std::string zoneMap;
    /**
     * @brief Mapa EQUIRECTANGULAR de ELEVACIÓN (PNG en gris), o vacío.
     *
     * El canal rojo, 0..255, se mapea linealmente a `elevationRange` en metros.
     */
    std::string elevationMap;
    /** @brief Metros a los que corresponden el 0 y el 255 del mapa. */
    glm::vec2   elevationRange = glm::vec2(-4000.0f, 4000.0f);
};

/**
 * @brief Identidad + config cruda de un planeta.
 *
 * Es TODO lo que el planeta necesita para construirse a sí mismo. `raw` lleva el `surfaceConfig`
 * JSON original del SceneObject (biomePalette, materials, …) que el planeta interpreta en su
 * `build()`. Mantiene el nombre público `SimplePlanet` del PlanetarySystem vía alias, así que el
 * API exterior no cambia.
 */
struct TerrestrialPlanetConfig {
    std::string name;
    glm::dvec3  position{0.0};
    double      radius = 6371000.0;
    uint32_t    seed = 0;                     // semilla auto-generada para generación determinista

    // --- ÓRBITA (Kepler analítico). La RESUELVE el PlanetarySystem (cadena de padres); el planeta
    // solo la guarda y la aplica. ---
    int    orbitParent  = -1;
    double orbitA       = 0.0;
    double orbitEcc     = 0.0;
    double orbitPeriod  = 0.0;
    double orbitPhase   = 0.0;
    glm::dvec3 orbitU   = glm::dvec3(1, 0, 0);
    glm::dvec3 orbitV   = glm::dvec3(0, 0, 1);

    SurfaceConfig surface;                    // textura/mapas (paths)
    /** @brief `surfaceConfig` JSON crudo: biomePalette, materials, seed, … Lo interpreta el planeta. */
    nlohmann::json raw;
    /** @brief Resolución de la retícula base de una cara del cubo (celdas por arista). */
    int faceRes = 256;
};

/**
 * @brief Paquete de texturas de bioma de la ruta "4 capas".
 *
 * Cada bioma clásico (sand/grass/land/rock) con su albedo + normal. Los arrays de terreno
 * (una capa por material, elegidos por el proyecto) viven aparte en `albedoArray/normalArray`.
 */
struct BiomeTextures {
    Haruka::RHI::TextureHandle sandAlbedo, sandNormal;
    Haruka::RHI::TextureHandle grassAlbedo, grassNormal;
    Haruka::RHI::TextureHandle landAlbedo, landNormal;
    Haruka::RHI::TextureHandle rockAlbedo, rockNormal;
    Haruka::RHI::TextureHandle albedoArray, normalArray;
    int arrayLayers = 0;
};

/**
 * @brief Un planeta renderizable. Se construye a sí mismo en `build(config)`.
 *
 * El planeta es dueño de TODO su estado renderizable: geología/clima/biomas, mapa de zonas y de
 * elevación, nivel del mar, retícula base (CPU para la física + GPU para el clipmap), malla del
 * planeta (más índice de parches para la teselación), clipmap, texturas de bioma, tabla de
 * materiales y mapas procedurales. `render` se basta solo con la cámara.
 *
 * Los pipelines/shader-handles son ESTÁTICOS: un programa se comparte entre planetas, y
 * `ensureShaders` lo crea una sola vez.
 */
class TerrestrialPlanet {
public:
    TerrestrialPlanet();
    ~TerrestrialPlanet();
    TerrestrialPlanet(const TerrestrialPlanet&) = delete;
    TerrestrialPlanet& operator=(const TerrestrialPlanet&) = delete;

    /**
     * @brief EL PLANETA SE CREA A SÍ MISMO a partir de su config.
     *
     * Interpreta `cfg.raw` (biomePalette, materials), deriva la geología de `cfg.seed`, carga los
     * mapas de zona/elevación si los trae, calcula el nivel del mar, construye la malla (con el
     * recorte de zonas aplicado), las texturas de bioma, el mapa de biomas y el de macro-variación.
     *
     * @return false si no hay device RHI (el planeta queda inerte, sin GPU que subirle).
     */
    bool build(const TerrestrialPlanetConfig& cfg);

    /**
     * @brief Reconstruye el terreno (nueva semilla/geología) manteniendo el resto.
     *
     * Regenera la geología con la config dada, reconstruye la malla y refresca texturas/mapas si
     * aún no existieran. Es el camino de "regenerar el mundo" del PlanetarySystem.
     *
     * @param faceRes Resolución de la retícula base (celdas por arista); <=0 conserva la actual.
     */
    void rebuild(const Haruka::Planet::GeologyConfig& geo, int faceRes = 0);

    /**
     * @brief Reconstruye la malla sumando EDICIONES del orquestador a la base que ya usó `build`.
     *
     * `build` guarda la función de altura con la que construyó su malla (misma base, mismo recorte
     * de zonas). Este método la reutiliza y le suma el desfase (`editFn`, en metros) que le pasa el
     * PlanetarySystem — las marcas de excavación del jugador se quedan donde se hicieron, sin tocar
     * geología ni mapas. Es el camino de `editTerrain`/`levelTerrain`.
     */
    void rebuildWithEdits(const std::function<float(const glm::dvec3&)>& editFn);

    /** @brief Renderiza el planeta (malla + clipmap + agua). Coplanaridad y orden gestionados aquí. */
    void render(const glm::dvec3& cameraPos, const glm::mat4& proj, const glm::mat4& view);

    /**
     * @brief Luz del SOL que ilumina el terreno (dirección HACIA el sol, color, ambiente).
     *
     * La pone el orquestador cada frame desde la escena (`WorldSystem` → la estrella real del
     * mundo). Sin llamarla, el planeta usa su luz fija por defecto — lo que hacía que el terreno
     * NUNCA respondiera al sol que se ve en el cielo.
     */
    void setSunLight(const glm::vec3& dir, const glm::vec3& color, float ambientStrength) {
        if (glm::dot(dir, dir) > 1e-12f) m_sunDir = glm::normalize(dir);
        m_sunColor         = glm::clamp(color, 0.0f, 1.0f);
        m_ambientStrength  = glm::clamp(ambientStrength, 0.0f, 1.0f);
    }

    /** @brief Construye la malla con la función de altura dada (el orquestador aporta las ediciones). */
    void buildMesh(const std::function<float(const glm::dvec3&)>& heightFn, double radius);

    /**
     * @brief Altura del terreno en una dirección, en METROS sobre el radio del planeta.
     *
     * Reproduce EXACTAMENTE lo que dibuja el tessellation evaluation: bilineal de las alturas de los
     * 4 vértices de la celda base + el detalle procedural compartido. Es el contrato del suelo.
     */
    double sampleHeight(const glm::dvec3& dir) const;

    /** @brief Libera los recursos GPU de ESTE planeta. */
    void clearGPU();
    /** @brief Destruye los pipelines/handles ESTÁTICOS compartidos (una vez, al cerrar el motor). */
    static void cleanupStatics();

    // --- Acceso al estado (el orquestador lo lee / lo ajusta) --------------------------------
    const TerrestrialPlanetConfig& config() const { return m_config; }
    const std::string& name() const { return m_config.name; }
    glm::dvec3 position() const { return m_config.position; }
    double radius() const { return m_config.radius; }
    void setPosition(const glm::dvec3& p) { m_config.position = p; }
    int  orbitParent() const { return m_config.orbitParent; }
    double orbitPeriod() const { return m_config.orbitPeriod; }
    void setOrbit(int parent, double a, double ecc, double period, double phase,
                  const glm::dvec3& u, const glm::dvec3& v);
    void setWeatherTime(double t) { m_weather.setTime(t); }

    /** @brief Vista de depuración del terreno. 0=normal, 1=elevación, 2=zonas, 3=bioma,
     *  4=temperatura, 5=humedad, 6=capas (todas), 10+i=máscara del material i.
     *  `i` es la POSICIÓN en `surface.materials` (0=primero, incluya agua/hielo). La consume el
     *  shader (`uDebug.x`) y el editor la cambia. */
    int debugView() const { return m_debugView; }
    void setDebugView(int v) { m_debugView = v; }

    /** @brief Tabla de materiales del terreno (capas + nombres + zonas). La lee el editor para
     *  poblar el selector de capas. */
    const Haruka::Planet::TerrainMaterialTable& terrainMaterials() const { return m_materialTable; }

private:
    TerrestrialPlanetConfig m_config;
    Haruka::Planet::GeologyOutput m_geology;
    Haruka::Planet::ClimateOutput m_climate;
    Haruka::Planet::BiomesOutput m_biomes;
    Haruka::WeatherSystem m_weather;

    /**
     * @brief Altura (m) que se resta al campo para que el NIVEL DEL MAR quede en 0.
     *
     * Cuantil de la distribución de alturas que deja la fracción de tierra emergida pedida.
     */
    float m_seaLevelOffsetM = 0.0f;

    /** @brief Lado de los mapas equirectangulares GENERADOS (biomas, macro-variación). */
    int m_mapRes = 512;
    /**
     * @brief Semilla EFECTIVA con la que se generó la geología (la usa la clave del cache de
     * horneados). Es `cfg.seed` si viene, si no `hash(name)`: el mismo número que `build` metió en
     * `geo.seed`. Un planeta regenerado con otra semilla hornea a otro archivo, no reutiliza el viejo.
     */
    uint32_t m_seed = 0;

    // MAPA DE ZONAS. Vive en las DOS memorias a propósito: en CPU la ELEVACIÓN se calcula al
    // construir la malla (una zona de agua tiene que quedar hundida = geometría); en GPU el MATERIAL
    // se decide por píxel.
    std::vector<unsigned char> m_zoneCPU;   // RGBA8 equirectangular
    int m_zoneW = 0, m_zoneH = 0;
    Haruka::RHI::TextureHandle m_zoneTex;
    Haruka::Tools::ProcGraph::BiomeConfig m_biomeConfig;
    // Tabla de MATERIALES del terreno (regla clima/forma → aspecto). Se sube a un UBO y la recorre
    // el shader: añadir un material es un objeto más en el JSON, no un `if` más en GLSL.
    Haruka::Planet::TerrainMaterialTable m_materialTable =
        Haruka::Planet::TerrainMaterialTable::defaults();
    Haruka::RHI::BufferHandle m_materialUBO;

    // Retícula base del terreno — el suelo que se PISA (ver notas en planetary_system.cpp antiguo).
    std::vector<float> m_baseHeights;   // metros, [face][j][i] con lado (faceRes+1)
    int   m_baseRes = 0;
    float m_baseRadius = 0.0f;
    Haruka::RHI::TextureHandle m_baseFieldTex;  // misma retícula como array de 6 capas (elev,temp,hum)

    /** @brief Función de altura BASE (m) con la que se construyó la malla; `rebuildWithEdits` la reusa. */
    std::function<float(const glm::dvec3&)> m_baseHeightFn;

    /** @brief Vista de depuración activa (ver `debugView()`); se sube al shader por frame. */
    int m_debugView = 0;
    // Luz del sol para el UBO del planeta. Por defecto la antigua fija; la sobrescribe
    // `setSunLight` (el orquestador la lee de la escena). Sin esto el terreno era un color
    // plano: el `uLightDir`/`uLightColor`/`uAmbient` venían fijos y no seguían al sol.
    glm::vec3 m_sunDir  = glm::vec3(0.3f, 0.8f, 0.5f);
    glm::vec3 m_sunColor = glm::vec3(1.0f, 0.95f, 0.9f);
    float m_ambientStrength = 0.18f;

    // MAPA DE ELEVACIÓN del autor. Vive como miembro porque la función de altura base lo referencia
    // (le hace pareja al de zonas, que ya vivía en CPU): ambos quedan vivos toda la vida del planeta.
    std::vector<unsigned char> m_elevCPU;   // RGBA8 equirectangular (se lee el canal rojo)
    int m_elevW = 0, m_elevH = 0;

    // CLIPMAP — la rejilla que da los 2 m cerca del jugador.
    Haruka::RHI::BufferHandle m_clipVB, m_clipIB;
    uint32_t m_clipIndexCount = 0;
    Haruka::RHI::BufferHandle m_clipUBO;

    // Single terrain mesh (all faces combined)
    Haruka::RHI::BufferHandle m_vertexBuffer;
    Haruka::RHI::BufferHandle m_indexBuffer;
    uint32_t m_indexCount = 0;

    // TESELACIÓN: mismo vertex buffer, OTRO índice (parches del quad).
    Haruka::RHI::BufferHandle m_patchIB;
    uint32_t m_patchIndexCount = 0;

    // Water mesh (single sphere at ocean radius)
    Haruka::RHI::BufferHandle m_waterVB;
    Haruka::RHI::BufferHandle m_waterIB;
    uint32_t m_waterIndexCount = 0;

    // Texturas (legacy singles + biome 4-layer + macro/biome map)
    Haruka::RHI::TextureHandle m_albedoTex;
    Haruka::RHI::TextureHandle m_normalTex;
    BiomeTextures m_biomeTex;
    Haruka::RHI::TextureHandle m_macroTex;
    Haruka::RHI::TextureHandle m_biomeMapTex;
    float m_tiling = 100.0f;

    int m_faceSubdiv = 16;

    // --- Helpers internos (migrados de SimplePlanetInternal) ---------------------------------
    void ensureShaders();
    /** @brief Altura (m) que pone la GEOLOGÍA (placas) en una dirección: km → m. */
    float rawHeight(const glm::dvec3& dir) const;
    /** @brief Altura (m) que pone el MAPA DE ELEVACIÓN del autor (bilineal); 0 sin mapa. */
    float sampleElevMap(const glm::dvec3& d) const;
    /**
     * @brief Altura (m) BASE del terreno en una dirección — la misma regla que usa `build`.
     *
     * Con mapa de elevación manda el autor (bilineal); sin él, la geología menos el nivel del mar.
     * Si hay mapa de zonas, el signo lo recorta el material (submerged → mar, el resto → tierra).
     * Es lo que guarda `m_baseHeightFn` y lo que `rebuild` recalcula al regenerar la geología.
     */
    float baseHeight(const glm::dvec3& dir) const;
    glm::vec3 biomeColor(const glm::dvec3& dir, float elevKm) const;
    static Haruka::RHI::TextureHandle loadTexture(const std::string& path);
    static Haruka::RHI::TextureHandle loadTextureArray(const std::vector<std::string>& paths,
                                                       int& outLayers);
    void rebuildTerrainArrays();
    static BiomeTextures loadBiomeTextures(const Haruka::Planet::ClimateOutput& climate,
                                            const Haruka::Planet::GeologyOutput& geology);
    Haruka::RHI::TextureHandle generateProceduralAlbedo(int width, int height) const;
    Haruka::RHI::TextureHandle generateProceduralNormal(int width, int height) const;

    // Shaders y UBO COMPARTIDOS entre planetas (estáticos).
    static Haruka::RHI::PipelineHandle s_pipeline;
    static Haruka::RHI::PipelineHandle s_texPipeline;
    static Haruka::RHI::PipelineHandle s_biomePipeline;
    static Haruka::RHI::PipelineHandle s_tessPipeline;
    static Haruka::RHI::PipelineHandle s_clipPipeline;
    static Haruka::RHI::PipelineHandle s_waterPipeline;
    static Haruka::RHI::PipelineHandle s_wirePipeline;
    static Haruka::RHI::BufferHandle s_ubo;
    static bool s_shadersReady;
};

}} // namespace Haruka::Planet
