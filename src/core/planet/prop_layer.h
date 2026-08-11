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

#include "core/terrain/planet_fields.h"             // FieldSample
#include "core/planet/prop_cond.h"                  // PropCond, parsePropCond
#include "tools/procgraph/tree_spawn.h"             // IPropField, PropGrid, slopeAt (GL-free)
#include "tools/procgraph/proc_noise.h"             // hash32

namespace Haruka { namespace Planet {

/** @brief Banda suave de un rango [min,max] con `feather` en los dos extremos. Devuelve [0,1]:
 *  0 fuera (con degradado en los bordes), 1 dentro. Replica las bandas de TerrainMaterial. */
inline float softBand(float v, float minV, float maxV, float feather) {
    if (feather <= 0.0f) return (v >= minV && v <= maxV) ? 1.0f : 0.0f;
    if (v < minV)       return glm::smoothstep(minV - feather, minV, v);
    if (v > maxV)       return glm::smoothstep(maxV + feather, maxV, v);
    return 1.0f;
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

    /** @brief Qué objeto instala. NO es un enum del motor: es un identificador que este .cpp
     *  apenas conoce ("tree", "rock", "house", "path"…). Vacío = esta capa nunca instala; el
     *  planner la empareja con su mesh/bake procedure (bakeTreeMesh) fuera de aquí. */
    std::string mesh;
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
     *  `zone` = zona nombrada (vacío = sin zona). Sin `when` → siempre true. Un `when` mal
     *  escrito (que el SceneValidator debería haber cazado) se ignora a propósito: una capa no
     *  debe dejar de instalar por una expresión rota. El parseo se cachea en `m_when`. */
    bool whenAllowed(const std::string& layer, const std::string& zone) const {
        if (when.empty()) return true;
        if (!m_when) {
            std::string err;
            m_when = parsePropCond(when, err);
            if (!m_when) return true;
        }
        PropCondCtx ctx{layer, zone};
        return m_when->eval(ctx);
    }

    /** @brief Cuánto CUMPLE esta capa en un punto del campo, [0,1] = producto de las bandas de
     *  clima/forma. 0 = no es su territorio; >0 = cuánto es. `zoneName` (opcional) aplica el
     *  filtro de zona declarado en `zones`; `layerName` (opcional) alimenta la condición `when`. */
    float coverage(const FieldSample& fs, float slopeRad, float mapDensity = 1.0f,
                   const std::string& zoneName = {}, const std::string& layerName = {}) const {
        if (!zoneAllowed(zoneName)) return 0.0f;
        if (!whenAllowed(layerName, zoneName)) return 0.0f;
        float h = softBand(fs.humidity, humMin, humMax, feather);
        float t = softBand(fs.tempC,   tempMin, tempMax, feather);
        float s = softBand(glm::clamp(slopeRad, 0.0f, 1.0f), slopeMin, slopeMax, feather);
        return h * t * s * glm::clamp(mapDensity, 0.0f, 1.0f);
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
    };

    /** @brief Qué factores de `coverage()` valen 0 en este punto, como máscara de `FailBit`.
     *
     *  Evalúa lo MISMO que `coverage()` —los mismos `softBand` con los mismos límites— pero en vez
     *  de multiplicar informa de cuáles se anulan. Se llama solo desde el diagnóstico, así que
     *  puede permitirse mirar todos los factores en vez de cortocircuitar en el primero: si tienes
     *  la humedad Y la pendiente mal, quieres saberlo de una vez, no en dos arranques. */
    uint32_t coverageFailMask(const FieldSample& fs, float slopeRad, float mapDensity = 1.0f,
                              const std::string& zoneName = {},
                              const std::string& layerName = {}) const {
        uint32_t m = 0;
        if (!zoneAllowed(zoneName))                  m |= FAIL_ZONE;
        if (!whenAllowed(layerName, zoneName))       m |= FAIL_WHEN;
        if (softBand(fs.humidity, humMin, humMax, feather) <= 0.0f) m |= FAIL_HUM;
        if (softBand(fs.tempC,    tempMin, tempMax, feather) <= 0.0f) m |= FAIL_TEMP;
        if (softBand(glm::clamp(slopeRad, 0.0f, 1.0f), slopeMin, slopeMax, feather) <= 0.0f)
            m |= FAIL_SLOPE;
        if (glm::clamp(mapDensity, 0.0f, 1.0f) <= 0.0f) m |= FAIL_MAP;
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
            out.push_back(nlohmann::json::object({
                {"name", L.name}, {"mesh", L.mesh},
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
                {"feather", L.feather}
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