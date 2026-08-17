/**
 * @file terrain_material.h
 * @brief Tabla de MATERIALES del terreno, definida como DATO y no en GLSL.
 *
 * Un material del terreno no es una textura: es una REGLA sobre el clima y la forma
 * (humedad, temperatura, pendiente) más cómo se ve lo que cae dentro de ella. La regla vive aquí
 * y viaja a la GPU en un UBO, así que se puede cambiar desde el JSON de la escena sin recompilar
 * ni tocar shader.
 *
 * Antes esto era una cadena de `if` en el shader (`H > 0.42 ? hierba : tierra`, más un caso de
 * roca): dos materiales fijos y ninguna forma de añadir un tercero. Con la tabla, un planeta puede
 * declarar los suyos —lava, sal, ceniza, líquen— sin que el motor sepa que existen.
 *
 * El COLOR no está aquí: lo pone el mapa de biomas, que sale del clima. El material aporta el
 * matiz, cuánto grano deja pasar la textura y cuánto relieve saca de su normal map.
 */
#pragma once
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka { namespace Planet {

/** @brief Sin textura: superficie lisa (hielo, sal, lava vidriada). Cualquier otro valor es una
 *  CAPA del array de terreno, y la asigna el motor por posición en la tabla. */
constexpr int kNoTerrainTile = -1;

/**
 * @brief Un material: dónde se aplica (rangos de clima/forma) y cómo se ve.
 *
 * Los rangos son BANDAS SUAVES, no cortes: cada límite se difumina con `feather` para que dos
 * materiales contiguos se mezclen en vez de dejar una línea recta sobre el terreno.
 */
struct TerrainMaterial {
    std::string name = "default";

    // Dónde se aplica.
    float humMin   =  0.0f,  humMax   = 1.0f;    ///< humedad [0,1]
    float tempMin  = -1e3f,  tempMax  = 1e3f;    ///< temperatura en °C
    float slopeMin =  0.0f,  slopeMax = 1.0f;    ///< 0 llano … 1 vertical
    /** @brief Banda de ALTURA sobre el nivel del mar, en KM. Mar<0, tierra>0.
     *
     *  Es el eje que faltaba, y era el más importante para "pintar el planeta a mano": sin él no se
     *  puede decir "roca sobre 1,5 km", "nieve sobre 3 km" ni "arena solo en las cotas bajas". El
     *  clima da la LATITUD del paisaje, la pendiente da las laderas, y la altura da los pisos
     *  altitudinales — que es lo que hace que una montaña se lea como montaña.
     *
     *  ⚠️ En KM, igual que `vClimate.x` y que la elevación que maneja `biome.frag`: mezclar unidades
     *  aquí es la clase de error que sale como "el material no aparece nunca" sin explicación. */
    float elevMinKm = -1e3f, elevMaxKm = 1e3f;
    float feather  =  0.08f;                     ///< anchura del degradado en los límites
    /** @brief Degradado de la banda de altura, en KM. Separado del `feather` general a propósito:
     *  ese está en la escala [0,1] de humedad/pendiente, y aplicado a kilómetros daría 80 m de
     *  transición — un corte duro para una ladera. 0,25 km funde el piso sobre ~500 m de desnivel. */
    float elevFeatherKm = 0.25f;

    // Cómo se ve.
    glm::vec3 tint   = glm::vec3(1.0f);          ///< multiplica al color de bioma (NO lo sustituye)
    float     grain  = 1.0f;                     ///< cuánto moteado aporta su textura [0..2]
    float     detail = 1.0f;                     ///< cuánto relieve aporta su normal map [0..2]

    /**
     * @name Textura del material
     * Rutas RELATIVAS a `assets/textures/terrain/<tier>/`. Vacías = material sin textura (lisa).
     *
     * Las pone el PROYECTO, no el motor: aquí no hay ninguna lista de "sand/grass/land/rock".
     * El motor sabe que un material puede tener textura; qué texturas existen es del juego, igual
     * que los edificios y los imperios salen de archivos y no de tablas en el .cpp.
     */
    ///@{
    std::string albedo;
    std::string normal;
    ///@}

    /** @brief Capa en el array de terreno. La ASIGNA el motor por posición en la tabla; no se
     *  declara. -1 = sin textura. */
    int layer = kNoTerrainTile;

    /**
     * @name Zona pintada (mapa equirectangular de zonas)
     *
     * `zoneColor` es el color con el que este material está pintado en `surface.zoneMap`. Cuando el
     * planeta trae mapa, la ZONA MANDA sobre las reglas de clima: donde el mapa dice "agua", es
     * agua, aunque la humedad y la temperatura de ese punto dijeran otra cosa. Las reglas siguen
     * decidiendo en los materiales que NO declaran zona.
     *
     * `submerged` es lo que hace que el mapa sea creíble: una zona de agua tiene que estar POR
     * DEBAJO del nivel del mar en el campo de alturas. Sin eso saldría terreno asomando por encima
     * del océano pintado — el mapa diría una cosa y la geometría otra.
     */
    ///@{
    glm::vec3 zoneColor = glm::vec3(-1.0f);      ///< negativo = este material no tiene zona
    bool      submerged = false;

    /**
     * @brief Papel del material en la COLUMNA del suelo (ver `terrain_strata.h`).
     *
     * `Bedrock` = la roca de debajo, la que asoma donde el sedimento no se agarra.
     * `Cover`   = el manto suelto de encima (arena, tierra, regolito), con espesor.
     *
     * ⚠️ NO es una etiqueta decorativa: decide en QUÉ concurso entra el material. Antes todos
     * competían contra todos y el resultado era su MEDIA — de ahí el velo gris que teñía el planeta.
     * Con el papel, se elige un lecho y una cobertura por separado y se mezclan por espesor: dos
     * listas cortas en vez de una larga donde todo pelea con todo.
     *
     * Por defecto `Cover`, que es lo que era todo hasta ahora: sin declararlo, la escena se comporta
     * como antes salvo que el lecho lo pone quien sí lo declare.
     */
    enum class Role : int { Cover = 0, Bedrock = 1 };
    Role role = Role::Cover;
    bool hasZone() const { return zoneColor.r >= 0.0f; }
    ///@}

    /**
     * @name Color propio del material
     *
     * Cuando se declara, SUSTITUYE al color del mapa de biomas donde manda este material. Es la
     * coherencia que faltaba: la zona ya decidía el material y la elevación, pero no el tono, así
     * que una isla pintada salía con forma y textura de isla y COLOR de desierto — el clima de esa
     * latitud ganaba. Pintas una zona, se ve como esa zona.
     *
     * `colorWeight` permite quedarse a medio camino: 0 manda el clima, 1 manda el material. Vale 1
     * por defecto cuando hay color, porque es lo que espera quien acaba de pintar un mapa.
     */
    ///@{
    glm::vec3 baseColor   = glm::vec3(-1.0f);    ///< negativo = sin color propio (manda el bioma)
    float     colorWeight = 1.0f;
    bool hasColor() const { return baseColor.r >= 0.0f; }
    ///@}

    /** @brief Desempate cuando dos materiales solapan. El mayor gana el TILE; el color se mezcla. */
    float priority = 1.0f;
};

/**
 * @brief La tabla completa. Vacía = usar `defaults()`.
 *
 * El límite de 16 es el del UBO: con std140 cada material ocupa 4 vec4 = 64 B, así que 16 son
 * 1 KiB + cabecera. Subirlo es cambiar una constante en los dos sitios (aquí y el shader).
 */
struct TerrainMaterialTable {
    static constexpr int kMaxMaterials = 16;
    std::vector<TerrainMaterial> materials;

    /**
     * @brief Reglas de clima por defecto, SIN texturas.
     *
     * Es el respaldo para una escena que no declare `surface.materials`: el terreno sale con el
     * color del bioma y sin grano, que es honesto —no hay texturas porque nadie ha dicho cuáles—
     * en vez de que el motor invente unos nombres de PNG que el proyecto quizá no tiene.
     * Las bandas de temperatura son las MISMAS que las de `BiomeConfig`: si divergen, el color
     * diría "taiga" donde el relieve dice "pradera".
     */
    static TerrainMaterialTable defaults() {
        TerrainMaterialTable t;
        auto add = [&](const char* n, float hMin, float hMax, float tMin, float tMax,
                       glm::vec3 tint, float grain, float detail, float prio) {
            TerrainMaterial m;
            m.name = n;
            m.humMin = hMin; m.humMax = hMax;
            m.tempMin = tMin; m.tempMax = tMax;
            m.tint = tint; m.grain = grain; m.detail = detail; m.priority = prio;
            t.materials.push_back(m);
        };
        //   nombre     humedad       temperatura   tinte                    grano detalle prio
        add("dry",    0.00f, 0.42f,  11.0f, 1e3f,  {1.02f, 1.00f, 0.97f},   1.00f, 0.90f, 1.0f);
        add("green",  0.42f, 1.00f,  11.0f, 1e3f,  {1.00f, 1.00f, 1.00f},   1.00f, 1.00f, 1.0f);
        add("tundra", 0.00f, 0.42f, -12.0f, 11.0f, {0.98f, 0.98f, 1.00f},   0.70f, 0.80f, 1.1f);
        add("taiga",  0.42f, 1.00f, -12.0f, 11.0f, {0.96f, 1.00f, 0.98f},   0.90f, 1.00f, 1.1f);
        add("ice",    0.00f, 1.00f,  -1e3f, -12.0f, {1.06f, 1.08f, 1.12f},  0.15f, 0.35f, 1.2f);
        // La ROCA es de PENDIENTE, no de clima: vale a cualquier humedad y temperatura, y gana a
        // todas por prioridad allí donde el terreno se empina.
        TerrainMaterial rock;
        rock.name = "rock";
        rock.slopeMin = 0.55f; rock.slopeMax = 1.0f; rock.feather = 0.12f;
        rock.tint = {1.00f, 0.98f, 0.96f}; rock.grain = 1.30f; rock.detail = 1.40f;
        rock.priority = 3.0f;
        t.materials.push_back(rock);
        return t;
    }

    /**
     * @brief Asigna a cada material su capa: la POSICIÓN en la tabla si tiene textura, -1 si no.
     *
     * Que la capa sea la posición y no un número declarado quita de en medio toda una clase de
     * fallo: un índice escrito a mano en el JSON puede apuntar a la capa de otro material, y el
     * síntoma sería "la roca tiene textura de hierba" sin nada que lo explique.
     */
    void assignLayers() {
        // ⚠️ ANTES: `m.layer = next++` — una capa POR MATERIAL, mirara o no el mismo fichero.
        //
        // Medido en la escena real: 13 materiales daban 9 capas que contenían **4 imágenes
        // distintas** (`grass_albedo.png` cargada CUATRO veces, `sand` y `rock` dos cada una). Cada
        // duplicado es un PNG de 4096² descomprimido, reescalado y subido otra vez: ~220 MB de VRAM
        // repetida entre albedo y normal, y ~18 s de arranque cargando cuatro veces la misma hierba.
        //
        // Y no era solo memoria. Todas las capas del array se igualan a la MÁS PEQUEÑA, así que el
        // presupuesto que se comían los duplicados es el que obligaba a bajar de 4096 a 2048 — la
        // mitad de la resolución de cada textura, que es la pixelación que se ve en el suelo.
        //
        // Ahora la capa se asigna por FICHERO: dos materiales con el mismo albedo comparten capa. El
        // par (albedo, normal) va junto en la clave, porque son la misma capa en dos arrays y
        // separarlos los descuadraría.
        std::vector<std::pair<std::string, std::string>> seen;
        int next = 0;
        for (auto& m : materials) {
            if (m.albedo.empty()) { m.layer = kNoTerrainTile; continue; }
            const auto key = std::make_pair(m.albedo, m.normal);
            int found = kNoTerrainTile;
            for (size_t i = 0; i < seen.size(); ++i)
                if (seen[i] == key) { found = (int)i; break; }
            if (found != kNoTerrainTile) { m.layer = found; continue; }
            seen.push_back(key);
            m.layer = next++;
        }
    }

    /**
     * @brief Capa del material de ORILLA (la playa que se mezcla en la línea de agua), o -1.
     *
     * `biome.frag` pinta arena a los dos lados del nivel del mar ENCIMA del material que gane ahí, así
     * que necesita una textura distinta de la del ganador — no le sirve `tile`. Antes venía de un
     * sampler suelto (`uSandAlbedo`) que cargaba OTRA VEZ el mismo PNG que ya está en el array: dos
     * rutas al mismo fichero, 22 MB duplicados y dos verdades sobre "cuál es la arena".
     *
     * El índice sale de aquí y no de un literal porque `assignLayers` lo asigna por POSICIÓN: atarlo a
     * "0" en el shader se rompería en cuanto alguien reordenara los materiales de la escena.
     *
     * Se localiza por nombre. Es una heurística y se documenta como tal: lo correcto a futuro es un
     * flag declarado en la escena (`"shore": true`), pero eso cambia el formato. Sin coincidencia
     * devuelve -1 y la orilla se pinta con su color sin textura — degradación visible, no basura.
     */
    int shoreLayer() const {
        for (const auto& m : materials) {
            if (m.layer == kNoTerrainTile) continue;
            if (m.name == "sand" || m.name == "arena" || m.name == "shore" || m.name == "playa")
                return m.layer;
        }
        return -1;
    }

    /** @brief Rutas de albedo de los materiales CON textura, en orden de capa. */
    std::vector<std::string> albedoPaths() const { return layerPaths(true); }
    /** @brief Ídem para las normales. Vacía en un material con albedo = capa plana sin relieve. */
    std::vector<std::string> normalPaths() const { return layerPaths(false); }

private:
    /** @brief Rutas EN ORDEN DE CAPA y sin repetir: una entrada por capa real del array.
     *  Gemela de `assignLayers` — si una asignara capas y la otra listara ficheros con otro
     *  criterio, el `tile` de un material apuntaría a la textura de otro. */
    std::vector<std::string> layerPaths(bool albedo) const {
        std::vector<std::string> v;
        for (const auto& m : materials) {
            if (m.layer == kNoTerrainTile) continue;
            if (m.layer < (int)v.size()) continue;          // capa ya emitida por un material previo
            v.resize((size_t)m.layer + 1);
            v[(size_t)m.layer] = albedo ? m.albedo : m.normal;
        }
        return v;
    }
};

}} // namespace Haruka::Planet
