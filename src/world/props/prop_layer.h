/**
 * @file prop_layer.h
 * @brief Tabla de CAPAS de props, definida como DATO y no como `if` en el código de generación.
 *
 * Espejo de `TerrainMaterialTable` (terrain_material.h), que es el patrón de capas del terreno:
 * un material es una REGLA sobre clima/forma más cómo se ve lo que cae dentro. Aquí una capa de
 * prop es la MISMA regla de clima/forma más QUÉ OBJETO instala (árbol, roca, casa, camino,
 * construcción) y cómo lo escala.
 *
 * ── EL ORDEN ES LA PRIORIDAD DE CONSTRUCCIÓN ───────────────────────────────────────────────
 * A diferencia de los materiales (donde `priority` desempata la textura de forma LOCAL), los
 * props ocupan ESPACIO. Por eso cada instancia RECLAMA un radio, y el orden de las capas en la
 * tabla decide quién instala primero:
 *
 *     [0] house → sitio de construcción: instala y reclama su radio de exclusión.
 *     [1] path  → camino: serpentea ENTRE lo ya reclamado y reclama su corredor.
 *     [2] tree/rock → árbol/roca: sus spots de Poisson que caen DENTRO de un claim previo se
 *                     DESCARTAN; el resto se instalan (el árbol pierde el spot que tomó la casa).
 *
 * El pase recorre las capas EN ORDEN; cada capa respeta los claims que dejaron las anteriores
 * (y los que él ya puso). Así "se construyó una casa, se pusieron caminos después, y el árbol
 * que tenía ese spot ya no se instancia" es una propiedad, no un parche.
 *
 * El motor no conoce ninguna capa concreta: un planeta declara las suyas (casa, camino, árbol,
 * roca, liquen… desde `raw[ "propLayers" ]`) sin que este .cpp sepa que existen — la misma
 * información de diseño que la tabla de materiales.
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "world/planet/planet_fields.h"             // FieldSample
#include "world/props/prop_cond.h"                  // PropCond, parsePropCond
#include "world/planet/biomes.h"                     // biomeFromKey / forestBiomeForTemp (mapForcesBiome)
#include "tools/procgraph/tree_spawn.h"             // IPropField, PropGrid, slopeAt (GL-free)
#include "tools/procgraph/proc_noise.h"             // hash32

namespace Haruka { namespace Planet {

/**
 * @brief Ruido de VALOR 3D trilineal, [0,1], anclado al mundo (la entrada es un punto en metros
 *  sobre la esfera): determinista por (posición, semilla), sin cámara. Base del "clump" de las capas.
 */
inline float valueNoise3(const glm::vec3& p, int seed) {
    const glm::vec3 f0 = glm::floor(p);
    const glm::vec3 t  = p - f0;
    const glm::vec3 u  = t * t * (3.0f - 2.0f * t);
    const int ix = (int)f0.x, iy = (int)f0.y, iz = (int)f0.z;
    auto h = [&](int dx, int dy, int dz) {
        return Haruka::Tools::ProcGraph::WhiteNode::hashFloat(ix + dx, iy + dy, iz + dz, seed);
    };
    const float x00 = glm::mix(h(0,0,0), h(1,0,0), u.x), x10 = glm::mix(h(0,1,0), h(1,1,0), u.x);
    const float x01 = glm::mix(h(0,0,1), h(1,0,1), u.x), x11 = glm::mix(h(0,1,1), h(1,1,1), u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
}

/** @brief fbm de 3 octavas sobre `valueNoise3`, [0,1] aprox. (media 0,5). */
inline float fbmNoise3(const glm::vec3& p, int seed) {
    float a = 0.5f, sum = 0.0f, norm = 0.0f;
    glm::vec3 q = p;
    for (int o = 0; o < 3; ++o) {
        sum  += a * valueNoise3(q, seed + o * 131);
        norm += a;
        a *= 0.5f; q = q * 2.03f + glm::vec3(17.1f, 9.7f, 3.3f);
    }
    return sum / norm;
}

/** @brief Banda suave de un rango [min,max] con `feather` en los dos extremos. Devuelve [0,1]:
 *  0 fuera (con degradado en los bordes), 1 dentro. Replica las bandas de TerrainMaterial. */
inline float softBand(float v, float minV, float maxV, float feather) {
    if (feather <= 0.0f) return (v >= minV && v <= maxV) ? 1.0f : 0.0f;
    if (v < minV)       return glm::smoothstep(minV - feather, minV, v);
    if (v > maxV)       return glm::smoothstep(maxV + feather, maxV, v);
    return 1.0f;
}

/**
 * @brief Una VARIANTE de lo que instala una capa: otro mesh, con su peso y su condición.
 *
 * Existe porque una capa tenía UN mesh y el registro UN prototipo por mesh: todos los árboles del
 * planeta eran la misma frondosa. Con variantes, la capa "tree" decide el SITIO (bandas, mapa,
 * claims) y la variante decide QUÉ árbol va ahí: `conifer` donde `biome == taiga`, `palm` en la
 * playa… Se sortea por hash de celda entre las variantes cuya `when` pasa, con `weight`.
 */
struct PropVariant {
    std::string mesh;          ///< prototipo ("conifer", "palm", "shrub"…): el estilo sale del nombre
    float       weight = 1.0f; ///< peso relativo en el sorteo entre las variantes elegibles
    std::string when;          ///< condición (misma sintaxis que la de la capa); vacío = siempre elegible
    int         seeds  = 1;    ///< nº de mallas distintas de esta variante ("conifer#0".."#k-1")
    mutable std::shared_ptr<PropCond> m_when;

    bool allowed(const std::string& layer, const std::string& zone, const std::string& biome) const {
        if (when.empty()) return true;
        if (!m_when) { std::string err; m_when = parsePropCond(when, err); if (!m_when) return true; }
        PropCondCtx ctx{layer, zone, biome};
        return m_when->eval(ctx);
    }
};

/** @brief Nombre de prototipo de una malla con varias semillas: `mesh#k`. Con una sola, el mesh
 *  a secas (así una escena sin `seeds` sigue registrando los mismos prototipos que antes). */
inline std::string propProtoName(const std::string& mesh, int seedIdx, int seeds) {
    return seeds > 1 ? mesh + "#" + std::to_string(seedIdx) : mesh;
}

/**
 * @brief Una capa de PROP: dónde instala (bandas de clima/forma) y QUÉ instala.
 *
 * Con las mismas bandas y `feather` que `TerrainMaterial`, así "donde sale un árbol" y "dónde se
 * pinta hierba" se rigen por la misma regla, no por dos fórmulas que divergen.
 */
struct PropLayer {
    std::string name = "default";

    // Dónde se aplica (bandas suaves, mismas del terreno).
    float humMin   =  0.0f,  humMax  = 1.0f;    ///< humedad [0,1]
    float tempMin  = -1e3f,  tempMax = 1e3f;    ///< temperatura en °C
    float slopeMin =  0.0f,  slopeMax = 1.0f;   ///< pendiente: 0 llano … 1 vertical (≈1.57 rad)
    float feather  =  0.08f;                     ///< degradado en los límites
    /** @brief Banda de COTA (m sobre el nivel del mar), con su propio degradado en metros: la línea
     *  de árboles no es sólo temperatura (a 2 500 m en el trópico hace 10 °C y no hay bosque por el
     *  viento y el suelo). Por defecto abierta. */
    float elevMinM = -1e9f, elevMaxM = 1e9f, elevFeatherM = 60.0f;
    /** @brief MANCHAS: longitud de onda (m) de un fbm anclado al mundo que multiplica la densidad
     *  (bosque con claros, pedregales en vez de piedras repartidas). 0 = apagado (ruido blanco, como
     *  siempre). `clumpAmount` [0,1] dice cuánto manda la mancha frente a la densidad plana. */
    float clumpM = 0.0f, clumpAmount = 1.0f;
    /** @brief Instancias POR CELDA cuando la capa gana la celda (hierba: 5 matas en 12 m, no una).
     *  Cada una con su jitter, escala y giro por hash. Las bandas de LOD son globales al scatter,
     *  así que es la forma de que una capa sea más fina que la celda sin re-enumerar. */
    int perCell = 1;
    /** @brief Alcance (m): la capa no se instala en bandas de LOD cuyo radio exterior pase de aquí.
     *  0 = todas. Es para la hierba: a 3 km no se ve y sí se paga (12 triángulos × 100 000). */
    float maxDistM = 0.0f;

    /** @brief Qué objeto instala. NO es un enum del motor: es un identificador que este .cpp
     *  apenas conoce ("tree", "rock", "house", "path"…). Vacío = esta capa nunca instala; el
     *  planner la empareja con su mesh/bake procedure (bakeTreeMesh) fuera de aquí. */
    std::string mesh;
    /** @brief Variantes del mesh (ver `PropVariant`). Vacío = la capa instala `mesh` a secas. */
    std::vector<PropVariant> variants;
    /** @brief Nº de mallas distintas (semillas) de `mesh` cuando no hay variantes: 1 = un solo
     *  prototipo, como siempre; 4 = cuatro árboles distintos repartidos por hash de celda. */
    int seeds = 1;
    /** @brief Multiplicador de DENSIDAD local [0,1]: 1 = todos los sitios válidos. El placer
     *  pincha con un hash de la celda contra este peso para dejar huecos sin patrón. */
    float density = 1.0f;
    /** @brief ¿Instala BAJO el nivel del mar? Por defecto falso: un prop sumergido se dibuja
     *  igual pero el océano lo tapa (coste de draw para algo que no se ve), así que el scatter se
     *  salta las celdas con `heightM < 0`. Con `true`, la capa se asienta sobre el LECHO (la roca
     *  "en el fondo"); coherente con `TerrainMaterial::submerged`. */
    bool submerged = false;
    /** @brief Mapa de distribución opcional (ruta a textura). Cuando existe, MANDA sobre las
     *  bandas: `coverage` se multiplica por su valor [0,1] en el punto — el mismo rol que el
     *  `zoneMap` del terreno, pero por capa (pintas dónde crecen los árboles). Vacío = sin mapa. */
    std::string densityMap;
    /** @brief El `densityMap` MANDA SOBRE EL BIOMA para esta capa: donde el mapa vale ≥ 0,5 el punto
     *  se trata como el bosque que toca por temperatura (`forestBiomeForTemp`) aunque el clima diga
     *  matorral. Es la regla "donde está pintado bosque, hay bosque" (decisión de Andoni, 15-09):
     *  el perfil de humedad del clima deja el spawn de Survival en matorral con 0,12 y sin esto
     *  ninguna variante `biome == bosque` saldría ahí. Mar, acantilado y hielo no se fuerzan. */
    bool mapForcesBiome = false;

    /** @brief Bioma que VE esta capa en un punto: el clasificado, o el bosque por temperatura si
     *  `mapForcesBiome` y el mapa lo pinta. `biome` = clave de `biomeKey` (vacío = sin clasificar). */
    std::string biomeFor(const std::string& biome, float mapDensity, float tempC) const {
        if (!mapForcesBiome || densityMap.empty() || mapDensity < 0.5f || biome.empty()) return biome;
        const Biome b = biomeFromKey(biome);
        if (b == Biome::COUNT || !biomeAcceptsForestMap(b)) return biome;
        return biomeKey(forestBiomeForTemp(tempC));
    }

    /** @brief ZONAS donde esta capa puede instalar: lista de NOMBRES de zona de la escena (las
     *  declaradas en `surfaceConfig.zones`, con perímetro GEOMÉTRICO —círculo/polígono, "una
     *  ciudad es esta área"— o color pintado en el zoneMap). Vacío = sin restricción de zona (solo
     *  mandan las bandas de clima). Cuando la lista NO está vacía y el punto no cae en ninguna de
     *  esas zonas —o el planeta no declara la zona— la capa NO instala ahí: declarar zona es un
     *  filtro DURO, no una banda. Mismo rol que el `zoneMap` sobre los materiales: delimitas
     *  dónde, y los props siguen el perímetro. */
    std::vector<std::string> zones;
    /** @brief Condición BOOLEANA sobre las identidades del punto (`layer` = material del terreno,
     *  `zone` = zona nombrada). Sintaxis en `core/planet/prop_cond.h`: `layer != sand || zone ==
     *  oasis`, con `&&`, `||`, `!`, `==`, `!=` y paréntesis. Vacío = sin condición. Se evalúa
     *  ADEMÁS de `zones` (que es el veto duro); cuando ambos existen, el punto tiene que pasar la
     *  lista y la expresión. El SceneValidator valida su sintaxis al cargar la escena. */
    std::string when;
    /// Rango de escala por instancia (muestreado por hash de la celda).
    float scaleMin = 0.8f, scaleMax = 1.3f;
    /** @brief Radio de exclusión (m) que deja CADA instancia de esta capa. Los spots de las capas
     *  posteriores que caigan dentro se DESCARTAN. 0 = no reclama (p. ej. líquen, hierba). */
    float claimRadius = 0.0f;

    /** @brief ¿Alguna condición de esta capa (la suya o la de sus variantes) mira al bioma? */
    bool usesBiomeAnywhere() const {
        if (usesBiome() || mapForcesBiome) return true;
        for (const auto& v : variants) if (v.when.find("biome") != std::string::npos) return true;
        return false;
    }
    /** @brief ¿Alguna condición mira al material del terreno (`layer`)? */
    bool usesLayerAnywhere() const {
        if (when.find("layer") != std::string::npos) return true;
        for (const auto& v : variants) if (v.when.find("layer") != std::string::npos) return true;
        return false;
    }
    /** @brief ¿Alguna condición o la lista `zones` mira a la zona? */
    bool usesZoneAnywhere() const {
        if (!zones.empty() || when.find("zone") != std::string::npos) return true;
        for (const auto& v : variants) if (v.when.find("zone") != std::string::npos) return true;
        return false;
    }

    /** @brief Todos los nombres de prototipo que esta capa puede instalar (para registrarlos). */
    std::vector<std::string> prototypeNames() const {
        std::vector<std::string> out;
        if (mesh.empty()) return out;
        if (variants.empty()) {
            for (int k = 0; k < std::max(1, seeds); ++k) out.push_back(propProtoName(mesh, k, seeds));
            return out;
        }
        for (const auto& v : variants) {
            if (v.mesh.empty()) continue;
            for (int k = 0; k < std::max(1, v.seeds); ++k) out.push_back(propProtoName(v.mesh, k, v.seeds));
        }
        return out;
    }

    /**
     * @brief Prototipo que va en UNA celda: la variante elegida (sorteo con `weight` entre las que
     *  pasan su `when` con las identidades del punto) y su semilla. `r0`, `r1` en [0,1) salen del
     *  hash de la celda: misma celda, mismo árbol, siempre. Vacío = ninguna variante es elegible
     *  aquí (la celda no instala nada: la capa dijo "sí" pero ninguna de sus especies vive en este
     *  bioma).
     */
    std::string pickPrototype(const std::string& layerName, const std::string& zoneName,
                              const std::string& biomeKey, float r0, float r1) const {
        if (variants.empty()) {
            const int n = std::max(1, seeds);
            return propProtoName(mesh, std::min(n - 1, (int)(r1 * (float)n)), n);
        }
        float total = 0.0f;
        for (const auto& v : variants)
            if (!v.mesh.empty() && v.weight > 0.0f && v.allowed(layerName, zoneName, biomeKey)) total += v.weight;
        if (total <= 0.0f) return {};
        float acc = 0.0f;
        const float pick = r0 * total;
        for (const auto& v : variants) {
            if (v.mesh.empty() || v.weight <= 0.0f || !v.allowed(layerName, zoneName, biomeKey)) continue;
            acc += v.weight;
            if (pick < acc || &v == &variants.back()) {
                const int n = std::max(1, v.seeds);
                return propProtoName(v.mesh, std::min(n - 1, (int)(r1 * (float)n)), n);
            }
        }
        return {};
    }

    /** @brief ¿Está permitida esta capa en la zona dada? `zoneName` es el NOMBRE del material que
     *  el zoneMap asigna al punto (vacío = el planeta no declara zona ahí). Vacío sin restricción
     *  de `zones` → siempre permitido. Con `zones` declaradas, el nombre tiene que estar en la
     *  lista; si no hay zona (mapa ausente o punto sin zona) → DENEGADO: "solo en bosque" con un
     *  planeta sin mapa de zonas no debe colar un árbol en mitad del desierto por defecto. */
    bool zoneAllowed(const std::string& zoneName) const {
        if (zones.empty()) return true;
        if (zoneName.empty()) return false;
        for (const auto& z : zones) if (z == zoneName) return true;
        return false;
    }

    /** @brief ¿Cumple la condición booleana `when`? `layer` = material del terreno en el punto,
     *  `zone` = zona nombrada (vacío = sin zona), `biome` = clave de `biomeKey()` (vacío = quien
     *  llama no clasificó; entonces `biome == X` es falso y `biome != X` cierto). Sin `when` →
     *  siempre true. Un `when` mal
     *  escrito (que el SceneValidator debería haber cazado) se ignora a propósito: una capa no
     *  debe dejar de instalar por una expresión rota. El parseo se cachea en `m_when`. */
    bool whenAllowed(const std::string& layer, const std::string& zone,
                     const std::string& biome = {}) const {
        if (when.empty()) return true;
        if (!m_when) {
            std::string err;
            m_when = parsePropCond(when, err);
            if (!m_when) return true;
        }
        PropCondCtx ctx{layer, zone, biome};
        return m_when->eval(ctx);
    }

    /// ¿Menciona `when` al bioma? Quien evalúa solo clasifica si alguna capa lo pide (es barato,
    /// pero es una rama por celda × capa que no hace falta pagar en una tabla sin `biome`).
    bool usesBiome() const { return when.find("biome") != std::string::npos; }

    /** @brief Cuánto CUMPLE esta capa en un punto del campo, [0,1] = producto de las bandas de
     *  clima/forma. 0 = no es su territorio; >0 = cuánto es. `zoneName` (opcional) aplica el
     *  filtro de zona declarado en `zones`; `layerName` y `biomeKey` (opcionales) alimentan la
     *  condición `when`. */
    float coverage(const FieldSample& fs, float slopeRad, float mapDensity = 1.0f,
                   const std::string& zoneName = {}, const std::string& layerName = {},
                   const std::string& biomeKey = {}) const {
        if (!zoneAllowed(zoneName)) return 0.0f;
        if (!whenAllowed(layerName, zoneName, biomeKey)) return 0.0f;
        float h = softBand(fs.humidity, humMin, humMax, feather);
        float t = softBand(fs.tempC,   tempMin, tempMax, feather);
        float s = softBand(glm::clamp(slopeRad, 0.0f, 1.0f), slopeMin, slopeMax, feather);
        float e = softBand(fs.elevKm * 1000.0f, elevMinM, elevMaxM, elevFeatherM);
        return h * t * s * e * glm::clamp(mapDensity, 0.0f, 1.0f);
    }

    /** @brief Factor de MANCHA [0,1] en un punto del mundo (`dir` unitario, `radiusM` del planeta).
     *  1 sin `clumpM`. Con él, un fbm de esa longitud de onda con contraste: el tercio bajo del ruido
     *  da 0 (claro), el tercio alto 1 (espesura). `layerIndex` separa las manchas de cada capa: el
     *  claro del bosque no es el pedregal. */
    float clumpFactor(const glm::vec3& dir, double radiusM, uint32_t seed, int layerIndex) const {
        if (clumpM <= 0.0f) return 1.0f;
        const glm::vec3 p = dir * (float)(radiusM / (double)clumpM);
        const float n = fbmNoise3(p, (int)(seed ^ (uint32_t)(layerIndex * 7919)));
        const float f = glm::clamp((n - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f);
        return glm::mix(1.0f, f, glm::clamp(clumpAmount, 0.0f, 1.0f));
    }

    /// Bits de POR QUÉ `coverage()` dio 0. Sin esto, "coverage=53753" no dice nada útil: hay SEIS
    /// formas de anularla y cada una se arregla tocando un campo distinto del JSON.
    enum FailBit : uint32_t {
        FAIL_ZONE  = 1u << 0,   ///< el punto no cae en ninguna zona de `zones` (filtro DURO)
        FAIL_WHEN  = 1u << 1,   ///< la expresión `when` no pasa
        FAIL_HUM   = 1u << 2,   ///< humedad fuera de [humMin, humMax]
        FAIL_TEMP  = 1u << 3,   ///< temperatura fuera de [tempMin, tempMax]
        FAIL_SLOPE = 1u << 4,   ///< pendiente fuera de [slopeMin, slopeMax]
        FAIL_MAP   = 1u << 5,   ///< el `densityMap` vale 0 aquí
        FAIL_ELEV  = 1u << 6,   ///< cota fuera de [elevMinM, elevMaxM]
    };

    /** @brief Qué factores de `coverage()` valen 0 en este punto, como máscara de `FailBit`.
     *
     *  Evalúa lo MISMO que `coverage()` —los mismos `softBand` con los mismos límites— pero en vez
     *  de multiplicar informa de cuáles se anulan. Se llama solo desde el diagnóstico, así que
     *  puede permitirse mirar todos los factores en vez de cortocircuitar en el primero: si tienes
     *  la humedad Y la pendiente mal, quieres saberlo de una vez, no en dos arranques. */
    uint32_t coverageFailMask(const FieldSample& fs, float slopeRad, float mapDensity = 1.0f,
                              const std::string& zoneName = {},
                              const std::string& layerName = {},
                              const std::string& biomeKey = {}) const {
        uint32_t m = 0;
        if (!zoneAllowed(zoneName))                       m |= FAIL_ZONE;
        if (!whenAllowed(layerName, zoneName, biomeKey))  m |= FAIL_WHEN;
        if (softBand(fs.humidity, humMin, humMax, feather) <= 0.0f) m |= FAIL_HUM;
        if (softBand(fs.tempC,    tempMin, tempMax, feather) <= 0.0f) m |= FAIL_TEMP;
        if (softBand(glm::clamp(slopeRad, 0.0f, 1.0f), slopeMin, slopeMax, feather) <= 0.0f)
            m |= FAIL_SLOPE;
        if (glm::clamp(mapDensity, 0.0f, 1.0f) <= 0.0f) m |= FAIL_MAP;
        if (softBand(fs.elevKm * 1000.0f, elevMinM, elevMaxM, elevFeatherM) <= 0.0f) m |= FAIL_ELEV;
        return m;
    }

    /// Cache del parseo de `when` (parseo perezoso; el string es el dato fuente).
    mutable std::shared_ptr<PropCond> m_when;
};

/**
 * @brief La tabla completa de capas de props. Vacía = `defaults()`.
 *
 * EL ORDEN DE LA LISTA ES LA PRIORIDAD DE CONSTRUCCIÓN: el placer recorre `layers` de delante
 * a atrás; cada capa instala y reclama, y las de detrás respetan lo reclamado.
 */
struct PropLayerTable {
    static constexpr int kMaxLayers = 16;
    std::vector<PropLayer> layers;

    /** @brief Tabla por defecto (sin JSON declarado). Orbita informativa: aquí se declaran las
     *  capas del PLAN_TERRENO_V3 para que los tests y el prototipo vean el patrón completo. */
    static PropLayerTable defaults() {
        PropLayerTable t;

        // 0. CONSTRUCCIÓN: solo en suelo habitable (equivalente a "green" del terreno).
        PropLayer build;
        build.name  = "build";
        build.mesh  = "house";
        build.humMin  = 0.35f; build.humMax = 0.95f;
        build.tempMin = -5.0f; build.tempMax = 38.0f;
        build.slopeMax = 0.35f;                // casas no en vertientes
        build.density  = 0.20f;
        build.claimRadius = 6.0f;             // un solar reclama bastante
        t.layers.push_back(build);

        // 1. CAMINO: corre entre lo construido y reclama su corredor.
        PropLayer path;
        path.name = "path";
        path.mesh = "path";
        path.humMin = 0.30f; path.humMax = 0.98f;
        path.tempMin = -10.0f; path.tempMax = 35.0f;
        path.slopeMax = 0.45f;
        path.density  = 0.08f;
        path.claimRadius = 0.8f;
        // un camino ES el prop: serializado como listado de parches, control del corredor.
        t.layers.push_back(path);

        // 3. ÁRBOL: SOLO en hierba y bosque — los materiales húmedos del terreno. Coherente con
        //    los nombres reales de `TerrainMaterialTable::defaults()` (`terrain_material.h`):
        //    "grass/forest" son ahí `green` (húmedo cálido) y `taiga` (húmedo frío). El filtro
        //    `layer` exige un zoneMap en el planeta (sin mapa, layerAt=="" y no instala ningún
        //    árbol). La costa seca/sana (dry/tundra/rock) y lo empinado quedan fuera.
        PropLayer tree;
        tree.name    = "tree";
        tree.mesh    = "tree";
        tree.humMin  = 0.15f; tree.humMax = 1.0f;
        tree.tempMin = -2.0f; tree.tempMax = 32.0f;
        tree.slopeMax = 0.6f;
        tree.when     = "layer == green || layer == taiga";
        tree.density  = 0.9f;
        tree.claimRadius = 0.3f;   // reclama poco: solo el radio del tronco/raíz
        t.layers.push_back(tree);

        // 4. ROCA: en pendiente levemente empinada o saliente; reclama poco. Coherente con el
        //    material `rock` del terreno: solo roca en suelo árido/empinado y en el lecho/costa,
        //    NUNCA en hierba-bosque (green/taiga) que le toca al árbol. Sin `when` se colaría en
        //    las zonas verdes que la pendiente baja y la humedad alta permiten.
        PropLayer rock;
        rock.name = "rock";
        rock.mesh = "rock";
        rock.slopeMin = 0.2f; rock.slopeMax = 1.0f;
        rock.tempMin = -1e3f; rock.tempMax = 1e3f;
        rock.when     = "layer != green && layer != taiga";
        rock.submerged = true;   // las rocas también cubren el LECHO (el "fondo" de la costa)
        rock.density = 0.4f;
        rock.claimRadius = 0.4f;
        t.layers.push_back(rock);
        return t;
    }

    /** @brief Índice de la capa cuyo `name` coincide, o -1. */
    int indexOf(const std::string& name) const {
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].name == name) return (int)i;
        return -1;
    }

    /** @brief Índice de la capa cuyo `mesh` coincide, o -1. Las capas de la tabla pueden
     *  llamarse distinto que lo que instalan ("build" instala mesh "house"), y la PRIORIDAD de
     *  construcción se lee por posión de lista, así que buscar por mesh es lo robusto. */
    int indexOfMesh(const std::string& mesh) const {
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].mesh == mesh) return (int)i;
        return -1;
    }

    /** @brief Serializa a JSON (formato `surfaceConfig["propLayers"]` del IDE). EL ORDEN de la
     *  lista se conserva: es la prioridad de construcción, tal y como la edita el inspector. */
    nlohmann::json toJSON() const {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& L : layers) {
            nlohmann::json vars = nlohmann::json::array();
            for (const auto& v : L.variants)
                vars.push_back(nlohmann::json::object({
                    {"mesh", v.mesh}, {"weight", v.weight}, {"when", v.when}, {"seeds", v.seeds}}));
            out.push_back(nlohmann::json::object({
                {"name", L.name}, {"mesh", L.mesh},
                {"seeds", L.seeds},
                {"variants", vars},
                {"density", L.density},
                {"submerged", L.submerged},
                {"densityMap", L.densityMap},
                {"zones", L.zones},
                {"when", L.when},
                {"scaleMin", L.scaleMin}, {"scaleMax", L.scaleMax},
                {"claimRadius", L.claimRadius},
                {"humMin", L.humMin}, {"humMax", L.humMax},
                {"tempMin", L.tempMin}, {"tempMax", L.tempMax},
                {"slopeMin", L.slopeMin}, {"slopeMax", L.slopeMax},
                {"feather", L.feather},
                {"elevMinM", L.elevMinM}, {"elevMaxM", L.elevMaxM}, {"elevFeatherM", L.elevFeatherM},
                {"clumpM", L.clumpM}, {"clumpAmount", L.clumpAmount},
                {"mapForcesBiome", L.mapForcesBiome},
                {"perCell", L.perCell},
                {"maxDistM", L.maxDistM}
            }));
        }
        return out;
    }

    /** @brief Reconstruye la tabla desde el JSON del planeta (`propLayers`). Conserva el ORDEN
     *  de la lista (prioridad). Entradas inválidas se ignoran; JSON ausente = `defaults()`. */
    static PropLayerTable fromJSON(const nlohmann::json& raw) {
        PropLayerTable t;
        if (!raw.is_object() || !raw.contains("propLayers") || !raw["propLayers"].is_array())
            return t;
        for (const auto& jl : raw["propLayers"]) {
            if (!jl.is_object()) continue;
            PropLayer L;
            L.name         = jl.value("name", std::string());
            L.mesh         = jl.value("mesh", std::string());
            L.density      = jl.value("density", 1.0f);
            L.submerged    = jl.value("submerged", false);
            L.densityMap   = jl.value("densityMap", std::string());
            L.zones        = jl.value("zones", std::vector<std::string>{});
            L.when         = jl.value("when", std::string());
            L.m_when.reset();
            L.seeds        = std::max(1, jl.value("seeds", 1));
            if (jl.contains("variants") && jl["variants"].is_array()) {
                for (const auto& jv : jl["variants"]) {
                    if (!jv.is_object()) continue;
                    PropVariant v;
                    v.mesh   = jv.value("mesh", std::string());
                    v.weight = jv.value("weight", 1.0f);
                    v.when   = jv.value("when", std::string());
                    v.seeds  = std::max(1, jv.value("seeds", 1));
                    if (!v.mesh.empty()) L.variants.push_back(v);
                }
            }
            L.scaleMin     = jl.value("scaleMin", 0.8f);
            L.scaleMax     = jl.value("scaleMax", 1.3f);
            L.claimRadius  = jl.value("claimRadius", 0.0f);
            L.humMin       = jl.value("humMin", 0.0f);
            L.humMax       = jl.value("humMax", 1.0f);
            L.tempMin      = jl.value("tempMin", -1e3f);
            L.tempMax      = jl.value("tempMax", 1e3f);
            L.slopeMin     = jl.value("slopeMin", 0.0f);
            L.slopeMax     = jl.value("slopeMax", 1.0f);
            L.feather      = jl.value("feather", 0.08f);
            L.elevMinM     = jl.value("elevMinM", -1e9f);
            L.elevMaxM     = jl.value("elevMaxM", 1e9f);
            L.elevFeatherM = jl.value("elevFeatherM", 60.0f);
            L.clumpM       = jl.value("clumpM", 0.0f);
            L.clumpAmount  = jl.value("clumpAmount", 1.0f);
            L.mapForcesBiome = jl.value("mapForcesBiome", false);
            L.perCell      = std::max(1, jl.value("perCell", 1));
            L.maxDistM     = jl.value("maxDistM", 0.0f);
            if (L.name.empty() && L.mesh.empty()) continue;   // entrada basura
            t.layers.push_back(L);
        }
        return t;
    }
};

/**
 * @brief Una instancia colocada: qué capa la puso, en qué punto, con qué escala.
 *  Local a la retícula (localPos sobre la superficie, heightM = cota, seed para el bake).
 */
struct PlacedProp {
    std::string mesh;
    int         layerIndex = -1;   ///< de qué capa de la tabla salió
    glm::vec3   localPos   = glm::vec3(0.0f);
    float       heightM    = 0.0f;
    uint32_t    seed       = 0;
    float       scale      = 1.0f;
    uint32_t    meshSeed   = 0;    ///< semilla de la MÓDULO (bake determinista)
};

}} // namespace Haruka::Planet