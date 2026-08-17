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
#include "core/planet/prop_layer.h"
#include "core/planet/zone_shape.h"
#include "core/planet/orbit.h"                 // OrbitElements (Kepler con elementos precesantes)
#include "core/terrain/planet_fields.h"        // FieldSample (fieldSampleAt)
#include "core/weather_system.h"
#include "tools/procgraph/proc_graph.h"
#include "tools/procgraph/proc_climate.h"

namespace Haruka { namespace Planet {

/** @brief Configuración de superficie: tiling de textura y resolución procedural. */
struct SurfaceConfig {
    float       tiling = 100.0f;
    int         texRes = 512;   // biome map procedural texture resolution (width); height = width/2
    int         macroRes = 2048; // macro-variation texture resolution (width); height = width/2
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
 * @brief Una ZONA del autor con nombre propio.
 *
 * Independiente de los materiales del terreno: una zona es "city" aunque ningún material se llame
 * así. Puede ser GEOMÉTRICA (un círculo o un polígono con perímetro en lat/lon — "la ciudad es
 * esta área"), PINTADA (un color en el zoneMap) o ambas. Las capas de props la referencian por
 * nombre en su campo `zones`. Si la misma área/pintura coincide con el `zone` de un material,
 * MANDA la zona nombrada (el autor la definió a propósito).
 */
struct NamedZone {
    std::string name;
    ZoneShape   shape;                    ///< perímetro (círculo/polígono); vacío = solo pintada
    glm::vec3   color = glm::vec3(-1.0f); ///< color en el zoneMap (0-255); negativo = sin color
    bool hasColor()    const { return color.r >= 0.0f; }
    bool hasGeometry() const { return shape.hasGeometry(); }
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
    /// Elementos completos + tasas de precesión. La base (u,v) del plano se DERIVA de aquí en cada
    /// frame: guardarla congelaba el plano al cargar la escena y la órbita no podía precesar.
    Haruka::Planet::OrbitElements orbit;

    SurfaceConfig surface;                    // textura/mapas (paths)
    /** @brief `surfaceConfig` JSON crudo: biomePalette, materials, seed, … Lo interpreta el planeta. */
    nlohmann::json raw;
    /** @brief Resolución de la retícula base de una cara del cubo (celdas por arista). */
    int faceRes = 256;
};

/*
 * ⚠️ AQUÍ VIVÍA `BiomeTextures` (sand/grass/land/rock × albedo/normal). Retirada entera.
 *
 * Era la ruta anterior a los arrays de material: un sampler por bioma clásico. Ninguno de los ocho lo
 * muestreaba ya ningún shader —comprobado por grep sobre `assets/shaders/`— desde que el terreno elige
 * su textura por CAPA del array. Costaban 8 cargas de ~3 s y ~176 MB de VRAM por planeta.
 *
 * Y de paso se llevó por delante un bug que no se veía: esta estructura se asignaba entera
 * (`m_biomeTex = loadBiomeTextures(...)`) DESPUÉS de que `rebuildTerrainArrays` hubiera creado los
 * arrays de terreno, que vivían dentro de ella. La asignación borraba sus handles y filtraba 404 MB.
 * Los arrays son ahora miembros propios del planeta: dos ciclos de vida distintos no pueden compartir
 * una estructura que se asigna entera.
 *
 * La arena de la ORILLA, que era la última supervivviente, sale de la capa del array que declara
 * `TerrainMaterialTable::shoreLayer()`.
 */

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
    /** @brief Trabajo de COMPUTE del frame (culling de parches). Se llama ANTES de abrir el
     *  render pass de la escena.
     *
     *  ⚠️ No es una optimización ni un capricho de orden: `vkCmdDispatch` dentro de una instancia
     *  de render pass es ILEGAL en Vulkan, y el culling vivía dentro de `render()` porque en OpenGL
     *  eso es perfectamente legal. Cerraba el programa. Ver el comentario largo en `prepare()`. */
    void prepare(const glm::dvec3& cameraPos);

    void render(const glm::dvec3& cameraPos, const glm::mat4& proj, const glm::mat4& view);

    /**
     * @brief Luz del SOL que ilumina el terreno (dirección HACIA el sol, color, ambiente).
     *
     * La pone el orquestador cada frame desde la escena (`WorldSystem` → la estrella real del
     * mundo). Sin llamarla, el planeta usa su luz fija por defecto — lo que hacía que el terreno
     * NUNCA respondiera al sol que se ve en el cielo.
     */
    /**
     * @param ambientColor  Ambiente YA EN COLOR, no un escalar.
     *
     * ⚠️ Era `float ambientStrength` multiplicado por un tinte fijo `(0.55, 0.65, 0.85)` — un número
     * inventado que no sabía nada del cielo bajo el que está el terreno. MEDIDO: el ambiente real
     * del cielo es entre 5 y 8 veces mayor, y de otro color. Con el escalar, cualquier superficie
     * sin sol directo caía a 0,043 de luminancia bajo un cielo de 0,60 — el agujero negro de las
     * sombras. Ahora llega integrado del MISMO cielo que se dibuja (`sky_ambient.cpp`).
     *
     * No se recorta a [0,1]: es un multiplicador de albedo ya normalizado (irradiancia/π) y
     * recortarlo volvería a inventar un techo.
     */
    /** @brief Los 9 coeficientes SH del cielo, para evaluar el ambiente POR NORMAL en el fragment.
     *  Sin ellos el ambiente es una constante y todo lo que está en sombra sale PLANO. */
    void setSkyAmbientSH(const glm::vec3 (&coef)[9]) {
        for (int i = 0; i < 9; ++i) m_skySH[i] = glm::vec4(coef[i], 0.0f);
        m_skySHValid = true;
    }

    void setSunLight(const glm::vec3& dir, const glm::vec3& color, const glm::vec3& ambientColor) {
        if (glm::dot(dir, dir) > 1e-12f) m_sunDir = glm::normalize(dir);
        m_sunColor     = glm::clamp(color, 0.0f, 1.0f);
        m_ambientColor = glm::max(ambientColor, glm::vec3(0.0f));
    }

    /**
     * @brief Estado del SUELO MOJADO / NEVADO y la máscara cenital con la que se recorta.
     *
     * `wet` y `snow` los INTEGRA el orquestador (mojarse cuesta ~30 s, secarse ~4 min), así que aquí
     * solo llegan ya acumulados. La `skyMask` es el mismo pase de profundidad cenital que usa la
     * lluvia para saber si una gota está bajo cubierto: reutilizarlo es lo que hace posible la
     * SILUETA SECA bajo un árbol o un tejado sin ningún sistema nuevo — el suelo pregunta lo mismo
     * que la gota ("¿tengo algo encima?") contra la misma textura.
     *
     * Handle vacío = sin máscara → moja por igual en todas partes.
     */
    void setGroundWet(float wet, float snow, Haruka::RHI::TextureHandle skyMask,
                      const glm::mat4& skySpace) {
        m_groundWet  = glm::clamp(wet,  0.0f, 1.0f);
        m_groundSnow = glm::clamp(snow, 0.0f, 1.0f);
        m_skyMaskTex = skyMask;
        m_skySpace   = skySpace;
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

    /**
     * @brief Ídem, pero con el `minFeatureM` que EXIGE el render en ese punto (§9 Fase 3).
     *
     * La paridad de la física no se negocia: `terrainMesh` (búsqueda de colisión) muestrea cada
     * vértice con el MISMO triM que el render usa ahí — `max(rad·0.002, 2.0)` dentro del clipmap y
     * `max(dist·0.002, 0.5)` en el per-pixel. Con 2.0 fijo (el default) la física y el render solo
     * coinciden en el puro pie; en el anillo 1-2 km y en el per-pixel se separan decímetros.
     */
    double sampleHeight(const glm::dvec3& dir, float minFeatureM) const;

    /**
     * @brief Campo ecológico del planeta en una dirección: cota + clima (temp/humedad).
     *
     * Es el muestreador que el adaptador de props (`TerrainPropField`) usa como `fieldFn`/`heightFn`:
     * la MISMA info de clima con la que se hornearon los biomas, así "donde hay un árbol" se rige
     * por el mismo clima que dibuja la hierba. `elevKm` sale del suelo muestreado (`sampleHeight`),
     * `tempC`/`humidity` del ClimateOutput del planeta.
     */
    Haruka::FieldSample fieldSampleAt(const glm::vec3& dir) const;

    /**
     * @brief Densidad [0,1] de un mapa de distribución (densityMap de una capa de prop) en la
     *  dirección. La textura se carga/cachea por ruta la primera vez (mismo patrón de candidatos
     *  que zoneMap/elevationMap) y se muestrea BILINEAL en equirectangular. Sin mapa = 1.0.
     */
    float densityMapAt(const std::string& path, const glm::vec3& dir) const;

    /**
     * @brief Zona del zoneMap en la dirección: nombre del material que MÁS se parece al color
     *  pintado ahí (el mismo `zoneToMaterial` que usa el terreno para decidir mar/tierra).
     *
     * Vacío = el planeta no tiene zoneMap o el punto no cae en ninguna zona declarada. Es la sonda
     * que usan las capas de props con `zones` (filtro por zona pintada).
     */
    std::string zoneNameAt(const glm::vec3& dir) const;

    /** @brief MATERIAL del terreno en la dirección (p.ej. "sand", "forest"): el mismo
     *  `zoneToMaterial` que usa el terreno, SIN el recubrimiento de zonas nombradas. Vacío = el
     *  planeta no tiene zoneMap. Es la sonda de `layer` para la condición booleana `when` de las
     *  capas de props (`layer != sand || zone == oasis`). */
    std::string materialNameAt(const glm::vec3& dir) const;

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
    /// Sobrecarga heredada: convierte la base (u,v) a elementos y les pone precesión por defecto.
    void setOrbit(int parent, double a, double ecc, double period, double phase,
                  const glm::dvec3& u, const glm::dvec3& v);
    /// Camino preferente: los elementos son la fuente de verdad del plano orbital.
    void setOrbit(int parent, const Haruka::Planet::OrbitElements& el);
    void setWeatherTime(double t) { m_weather.setTime(t); }

    // --- MAR DINÁMICO POR MASA (Hito 1) -------------------------------------------------------
    /** @brief Cuerpo masivo que atrae/mueve el océano de este planeta. */
    struct MassiveBody {
        glm::dvec3 posCenter;   ///< posición relativa al CENTRO del planeta (m)
        double     gm;          ///< G·M del cuerpo (m³/s²)
    };
    /** @brief Fija la lista de cuerpos masivos (soles/planetas/lunas) para la marea del océano.
     *  Vacío ⇒ el agua queda como la esfera rígida de siempre (aditivo/neutro). Se llama cada
     *  frame desde el sistema orbital. */
    void setTidalBodies(std::vector<MassiveBody> bodies) { m_tidalBodies = std::move(bodies); }
    const std::vector<MassiveBody>& tidalBodies() const { return m_tidalBodies; }

    // --- Stats de geometría del último frame (para el panel del editor) ---------------------
    /** @brief Lo que dibujó el planeta el último render: base + clipmap + agua, separados. */
    struct RenderStats {
        uint32_t baseVertices = 0, baseTriangles = 0;     ///< malla del planeta (o teselada)
        uint32_t clipVertices = 0, clipTriangles = 0;     ///< rejilla fina cerca del jugador
        // ⚠️ AQUÍ HABÍA `waterVertices`/`waterTriangles`, y quitarlos rompió el build del EDITOR
        // (`haruka/src/panels/viewport.cpp`), que vive en OTRO repo. El build fallaba, el binario
        // viejo seguía ejecutándose y ningún cambio del motor llegaba a la pantalla — sin ningún
        // aviso. Este struct lo leen TRES árboles: haruka-cpp, Survival y haruka (editor).
        int      drawCalls = 0;                           ///< 1 base + anillos del clipmap
    };
    const RenderStats& lastRenderStats() const { return m_lastRenderStats; }

    /** @brief Vista de depuración del terreno. 0=normal, 1=elevación, 2=zonas, 3=bioma,
     *  4=temperatura, 5=humedad, 6=capas (todas), 10+i=máscara del material i.
     *  `i` es la POSICIÓN en `surface.materials` (0=primero, incluya agua/hielo). La consume el
     *  shader (`uDebug.x`) y el editor la cambia. */
    int debugView() const { return m_debugView; }
    void setDebugView(int v) { m_debugView = v; }

    /** @brief Tabla de materiales del terreno (capas + nombres + zonas). La lee el editor para
     *  poblar el selector de capas. */
    const Haruka::Planet::TerrainMaterialTable& terrainMaterials() const { return m_materialTable; }

    /** @brief Tabla de CAPAS DE PROPS (regla clima/forma → qué objeto instala). La sube `render`
     *  a un UBO para la VISTA DE SPAWN del editor (dónde instalaría cada capa). La lee también el
     *  scatter global (`scatterPropsNear`), que parsea la misma config por separado. */
    const Haruka::Planet::PropLayerTable& propLayers() const { return m_propLayers; }

private:
    /// ¿Se ejecutó el culling por compute en `prepare()` de este frame? Lo consume `render()`
    /// para elegir entre el draw indirecto (culleado) y el camino normal.
    bool m_cullReady = false;

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

    /** @brief Ancho de la biome map horneada (2:1, alto = ancho/2). */
    int m_mapRes = 512;
    /** @brief Ancho del horneado de macro-variación (2:1); la biome map usa m_mapRes. */
    int m_macroRes = 2048;
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
    // Zonas NOMBRADAS del autor (escena `zones`): independientes de los materiales; las capas de
    // props las referencian por nombre. Vacío = solo cuentan los `zoneColor` de los materiales.
    std::vector<NamedZone> m_namedZones;
    // ¿Hace falta el zoneMap para resolver la zona de los PROPS? Solo si hay zonas PINTADAS
    // (con `color`) o alguna capa consulta el material del terreno (`when: layer`). Sin eso, la
    // zona se decide SOLO por geometría y el muestreo del mapa por celda del scatter es ocioso.
    bool m_propNeedZoneMap = false;
    // Tabla de MATERIALES del terreno (regla clima/forma → aspecto). Se sube a un UBO y la recorre
    // el shader: añadir un material es un objeto más en el JSON, no un `if` más en GLSL.
    Haruka::Planet::TerrainMaterialTable m_materialTable =
        Haruka::Planet::TerrainMaterialTable::defaults();
    Haruka::RHI::BufferHandle m_materialUBO;

    // Tabla de CAPAS DE PROPS (regla clima/forma → qué instala). Se sube a un UBO (binding 14) para
    // la VISTA DE SPAWN del editor; el densityMap de la capa seleccionada se sube aparte (textura).
    Haruka::Planet::PropLayerTable m_propLayers;
    Haruka::RHI::BufferHandle m_propUBO;
    // Textura del densityMap de la capa activa (cargada/cacheadas por ruta). Se bindea en el 17 del
    // shader para que la vista de spawn multiplique por el mapa — igual que hace el placer en CPU.
    std::unordered_map<std::string, Haruka::RHI::TextureHandle> m_propDensityTex;
    Haruka::RHI::TextureHandle m_propWhiteTex;   // 1×1 blanco: capa sin densityMap = sin recorte

    /** @brief Carga el densityMap de una capa y lo sube a GPU (misma búsqueda de candidatos que
     *  `densityMapAt`, bilineal + mipmaps). Devuelve la textura, o la blanca si no se encontró. */
    Haruka::RHI::TextureHandle uploadPropDensityMap(const std::string& path);

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
    // Suelo mojado/nevado + máscara cenital (ver setGroundWet). El UBO es UNO solo: su contenido no
    // cambia entre los draws del planeta (base, anillos del clipmap, anillo cercano), así que no
    // puede repetir el fallo del recurso reescrito entre draws.
    float                     m_groundWet  = 0.0f;
    float                     m_groundSnow = 0.0f;
    Haruka::RHI::TextureHandle m_skyMaskTex;
    glm::mat4                 m_skySpace{1.0f};
    Haruka::RHI::BufferHandle m_wetUBO;
    glm::vec3 m_sunColor = glm::vec3(1.0f, 0.95f, 0.9f);
    /// Ambiente en COLOR, integrado del cielo (irradiancia/π). Ver `setSunLight`.
    glm::vec3 m_ambientColor{0.15f, 0.18f, 0.24f};
    glm::vec4 m_skySH[9]{};        ///< cielo en SH (ya convolucionado); ver setSkyAmbientSH
    bool      m_skySHValid = false;
    Haruka::RHI::BufferHandle m_skySHUBO;
    float m_ambientStrength = 0.18f;   ///< legado: lo leen consumidores que aún esperan escalar

    // MAPA DE ELEVACIÓN del autor. Vive como miembro porque la función de altura base lo referencia
    // (le hace pareja al de zonas, que ya vivía en CPU): ambos quedan vivos toda la vida del planeta.
    /** @brief Mapa de elevación equirectangular, canal rojo, normalizado a 16 BITS.
     *
     *  ⚠️ Era RGBA8, y esos 8 bits eran el techo REAL del relieve: sobre un rango de 20 km salen
     *  **78 m por escalón**, así que el terreno no podía expresar nada más fino y salían mesetas
     *  planas (medido en un máster real: gradiente horizontal con mediana 0 m/px y solo 177 valores
     *  distintos). Subir la resolución no lo arreglaba — los escalones seguirían siendo de 78 m, solo
     *  más juntos. Lo que hacía falta eran más bits: con 16, el escalón baja a **0,3 m**.
     *
     *  Un PNG de 8 bits se sigue cargando: se promociona ×257, así que el mapa antiguo da exactamente
     *  los mismos valores que antes. */
    std::vector<uint16_t> m_elevCPU;
    int m_elevW = 0, m_elevH = 0;

    // MAPAS DE DISTRIBUCIÓN de props (densityMap por capa), cargados/cacheados por ruta. Cada uno
    // es RGBA8 equirectangular como zone/elev; `densityMapAt(path, dir)` los muestrea bilineal.
    struct DensityMap {
        std::vector<unsigned char> cpu;
        int w = 0, h = 0;
    };
    mutable std::unordered_map<std::string, DensityMap> m_densityMaps;
    mutable std::vector<std::string> m_densityMapOrder;   // orden de carga (log/diagnóstico)

    // ALTURA BASE HORNEADA (fase 2a): `baseHeight` evaluada por píxel equirect (2:1) y subida como
    // R32F. Es el MAESTRO del "campo lento" — geología + mapa de elevación + recorte de zonas — que
    // hoy se re-evalúa por vértice en la retícula de 39 km y por texel en los shaders. La fase 2b/2c
    // re-cablea tess/clipmap/CPU para que TODOS la lean (paridad por construcción); de momento solo
    // se hornea, cachea y sube.
    Haruka::RHI::TextureHandle m_heightTex;
    // Copia CPU del campo R32F subido a la GPU (misma resolución, mismos valores): `sampleHeight`
    // la muestrea con la MISMA bilineal que los shaders, así que el suelo que se pisa y el que se
    // ve nacen del mismo dato (paridad por construcción, fase 2b).
    std::vector<float> m_heightCPU;
    int m_heightW = 0, m_heightH = 0;

    // CLIPMAP — la rejilla que da los 2 m cerca del jugador.
    Haruka::RHI::BufferHandle m_clipVB, m_clipIB;

    // EL SUELO CERCANO QUE VIENE DE LA FÍSICA (ver `setNearGroundRing`). El planeta no lo genera:
    // lo recibe ya montado y solo lo dibuja con su propio material, que es el punto entero.
    Haruka::RHI::BufferHandle m_nearRingVB{}, m_nearRingIB{}, m_nearRingUBO{};
    uint32_t                  m_nearRingIndices = 0;
    // Ancla del anillo en coordenadas de MUNDO. El clipmap se ancla AQUÍ y no en la cámara: si no,
    // las dos retículas caen en celdas distintas (medido: hasta 4 m) y dejan de compartir vértices.
    glm::dvec3                m_nearRingAnchor{0.0};
    // ¿Se dibuja el anillo cercano ESTE frame? Solo con el clipmap sin estirar (ver ClipParams): su
    // hueco de 192 m únicamente encaja en un borde de parche cuando el parche mide 128 m.
    bool                      m_nearRingVisible = false;
    bool                      m_nearRingAnchored = false;
public:
    /** @brief Entrega el suelo cercano ya montado por la física, en float RELATIVO a `anchorRelEye`.
     *
     *  `anchorRelEye` se recalcula CADA FRAME en double y solo entonces baja a float: los vértices
     *  son relativos a un ancla fija entre reconstrucciones, y esta es la mitad que los lleva al ojo.
     *  Sin ella el parche quedaría pegado a la cámara y se deslizaría sobre el terreno al caminar.
     *
     *  `verts`/`tris` solo hacen falta cuando cambia la revisión; pasar `nullptr` reusa los buffers. */
    /** @brief Publica la superficie del agua interior (ríos/lagos) para que la dibuje el MAR.
     *  @param surfaceM  n×n cotas de la superficie del agua (m sobre el nivel del mar);
     *                   `kNoInlandWater` en las celdas sin agua.
     *  @param n         lado del parche · @param anchorRelEye ancla del parche relativa al ojo
     *  @param tan,bit,up  marco tangente del parche · @param spanM  lado del parche en metros */
    void setInlandWater(const std::vector<float>* surfaceM, int n,
                        const glm::vec3& anchorRelEye, const glm::vec3& tan,
                        const glm::vec3& bit, const glm::vec3& up, float spanM);
    /** @brief Centinela de "aquí no hay agua interior". Muy negativo para que `max(0, x)` lo ignore. */
    static constexpr float kNoInlandWater = -1e9f;

    void setNearGroundRing(const std::vector<glm::vec3>* verts, const std::vector<uint32_t>* tris,
                           const glm::vec3& anchorRelEye, const glm::vec3& anchorUp,
                           const glm::dvec3& anchorWorld);
private:
    uint32_t m_clipIndexCount = 0;
    uint32_t m_clipVertexCount = 0;
    /// UN UBO POR ANILLO, no uno compartido. El clipmap es ahora una PILA de anillos concéntricos
    /// (ver el bloque largo de `draw`) y cada uno se dibuja con su propio ClipParams: escala, hueco
    /// central y anillo de mezcla distintos.
    ///
    /// ⚠️ Y tienen que ser buffers DISTINTOS, no uno reescrito entre draws. En OpenGL eso valdría
    /// —cada draw se ejecuta al vuelo— pero en Vulkan los comandos se GRABAN y se ejecutan al final,
    /// así que los N anillos leerían el ClipParams del ÚLTIMO: todos saldrían con la misma escala,
    /// dibujados unos encima de otros. Es exactamente el fallo que ya tuvieron el UBO de material de
    /// los props y el buffer de instancias (`gpu_instancing.h`).
    /// Tope de anillos. 8 niveles = ±254 km con el exterior, que cubre el horizonte desde los 2,5 km
    /// donde el clipmap se apaga (sqrt(2·R·h) = 178 km). Es lo que permite que la MALLA BASE —la
    /// rejilla gruesa de cientos de metros por polígono— no llegue a verse nunca desde el suelo.
    static constexpr int kMaxClipRings = 8;
    std::vector<Haruka::RHI::BufferHandle> m_clipUBOs;
    int   m_clipRingCount = 1;      ///< anillos dibujados este frame (1 = comportamiento de siempre)
    float m_clipCoverM = 1984.0f;   // semi-lado del clipmap (m), según calidad; lo leen los shaders
    /// ¿Dibujó el clipmap el último frame? Ya NO lleva histéresis: no hay umbral de altura que
    /// cruzar (lo único que lo apaga es estar bajo tierra). Se conserva porque lo consultan el
    /// anillo cercano y las sondas.
    bool  m_clipMapActive = false;

    // Single terrain mesh (all faces combined)
    Haruka::RHI::BufferHandle m_vertexBuffer;
    Haruka::RHI::BufferHandle m_indexBuffer;
    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;

    // TESELACIÓN: mismo vertex buffer, OTRO índice (parches del quad).
    Haruka::RHI::BufferHandle m_patchIB;
    uint32_t m_patchIndexCount = 0;
    // ── Culling de parches en GPU (terrain_cull.comp). Opt-in: HARUKA_GPU_CULL=1 ────────────────
    // La malla base envía 393 216 parches por frame y el TCS mata casi todos (a pie se ve parte de
    // UNO). El compute los descarta antes, con una invocación por parche en vez de cuatro, y
    // compacta los supervivientes para dibujar con `drawIndexedIndirect`.
    uint32_t m_patchCount = 0;                        ///< parches de la malla base
    Haruka::RHI::BufferHandle m_patchBoundsSSBO;      ///< 4 esquinas por parche (estático)
    Haruka::RHI::BufferHandle m_patchIdxSSBO;         ///< el índice de parches como storage (origen)
    Haruka::RHI::BufferHandle m_patchIdxCulled;       ///< índice compactado (destino + draw)
    Haruka::RHI::BufferHandle m_patchCullCmd;         ///< DrawElementsIndirectCommand
    Haruka::RHI::BufferHandle m_patchCullUBO;         ///< CullParams (cámara + radio + nº parches)


    // ── EL MAR ──────────────────────────────────────────────────────────────────────────────────
    // Dos geometrías de UNA superficie, igual que el terreno (malla base + clipmap):
    //  · cerca: los anillos del clipmap (VB/IB/UBOs compartidos con el terreno), teselados y con
    //    olas de Gerstner. No hay buffers propios: solo otro pipeline sobre la misma rejilla.
    //  · lejos: esta esfera a nivel del mar, lisa (a esa distancia la ola es subpíxel).
    /**
     * @brief AGUA INTERIOR (ríos y lagos) como CAMPO, no como una segunda superficie.
     *
     * ⚠️ Ésta es la lección cara de la sesión del agua: había tres sistemas dibujando agua con tres
     * mallas y tres shaders, cada uno con su propia respuesta a "¿aquí hay agua?", y ninguno
     * coincidía con los otros. Ahora hay UNA superficie —la del mar— y la pregunta la responde un
     * solo campo: `nivel del agua − cota del suelo`. Para el mar el nivel es 0; para un lago, la
     * superficie que publica la simulación de aguas someras.
     *
     * Esto es esa publicación: una textura R32F con la COTA de la superficie del agua (m sobre el
     * nivel del mar) en cada celda del parche, y el marco tangente para localizar la celda desde una
     * dirección del planeta. Fuera del parche, o en celda seca, va un centinela muy negativo y el
     * mar se queda con su 0.
     */
    Haruka::RHI::BufferHandle  m_inlandWaterSSBO;   ///< n·n cotas del agua (m sobre el mar)
    Haruka::RHI::BufferHandle  m_inlandWaterUBO;
    bool                       m_inlandWaterValid = false;

    Haruka::RHI::BufferHandle m_oceanFarVB;
    Haruka::RHI::BufferHandle m_oceanFarIB;
    uint32_t m_oceanFarFaceStride = 0;   ///< índices por cara cúbica: permite saltar las 5 ocultas

    // MAR DINÁMICO POR MASA: snapshot de cuerpos masivos (setTidalBodies, desde el sistema orbital).
    // Su ÚNICO consumidor era la pipeline del agua, que se ha retirado; se conserva el dato porque lo
    // sigue leyendo el acoplamiento de la lámina de ríos/lagos (`tidalSeaAlongUpFn`) y porque es la
    // entrada natural del mar que se rehará. Ya no hay UBO que subir.
    std::vector<MassiveBody>        m_tidalBodies;

    // Stats del último render (malla base + clipmap) para el panel del editor.
    RenderStats m_lastRenderStats;

    // Texturas (legacy singles + biome 4-layer + macro/biome map)
    Haruka::RHI::TextureHandle m_albedoTex;
    Haruka::RHI::TextureHandle m_normalTex;
    /// Arrays de terreno (una capa por material de la escena). FUERA de `BiomeTextures` a propósito:
    /// ver la nota de esa estructura — compartirla borraba estos handles al reasignarla.
    Haruka::RHI::TextureHandle m_terrainAlbedoArray, m_terrainNormalArray;
    int m_terrainArrayLayers = 0;
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
    Haruka::RHI::TextureHandle generateProceduralAlbedo(int width, int height) const;
    Haruka::RHI::TextureHandle generateProceduralNormal(int width, int height) const;
    /** @brief Hornea la altura base (fase 2a) con cache en disco y la sube como R32F. */
    void bakeHeightMap();

    // Shaders y UBO COMPARTIDOS entre planetas (estáticos).
    static Haruka::RHI::PipelineHandle s_pipeline;
    static Haruka::RHI::PipelineHandle s_texPipeline;
    static Haruka::RHI::PipelineHandle s_biomePipeline;
    static Haruka::RHI::PipelineHandle s_tessPipeline;
    /// Pipeline de COMPUTE del culling de parches (terrain_cull.comp). Compartido: la malla base es
    /// del mismo tipo en todos los planetas. Inválido = el camino de siempre (draw de todo).
    static Haruka::RHI::PipelineHandle s_cullPipeline;
    static Haruka::RHI::PipelineHandle s_clipPipeline;
    /** @brief Mar cercano: la rejilla del clipmap teselada con oleaje de Gerstner. */
    static Haruka::RHI::PipelineHandle s_oceanPipeline;
    /** @brief Mar lejano: la esfera lisa a nivel del mar, con el MISMO fragment. */
    static Haruka::RHI::PipelineHandle s_oceanFarPipeline;
    static Haruka::RHI::PipelineHandle s_nearRingPipeline;
    static Haruka::RHI::PipelineHandle s_wirePipeline;
    static Haruka::RHI::BufferHandle s_ubo;
    static bool s_shadersReady;
};

}} // namespace Haruka::Planet
