#pragma once
/**
 * @file water_fill.h
 * @brief DÓNDE HAY AGUA QUIETA EN EL PLANETA — priority-flood GLOBAL sobre el bake de altura.
 *
 * ── POR QUÉ EXISTE, Y QUÉ SUSTITUYE ─────────────────────────────────────────────────────────────
 *
 * Los lagos y ríos los sembraba `ShallowWaterSim::seedLakes` sobre un PARCHE de 420 m anclado al
 * jugador, y de ahí colgaban tres limitaciones que su propia cabecera documenta:
 *
 *   · **Fuera de 420 m no hay agua interior.** Ninguna.
 *   · **El resultado depende del BORDE del parche**, porque el borde es por donde el agua escapa:
 *     una cuenca que asome por él se considera abierta y no se llena, y al re-anclar 160 m más allá
 *     puede quedar dentro y sí llenarse. Un lago puede APARECER al caminar.
 *   · **No es función de la posición**, así que dos clientes no pueden coincidir — y el requisito
 *     que Andoni fijó el 2026-08-31 es que lo visible sea colisionable e igual entre clientes.
 *
 * Las tres son la misma causa: el agua estaba anclada al observador. Aquí se corre el MISMO
 * algoritmo una sola vez sobre el planeta entero, al crear el mundo. El borde desaparece porque una
 * esfera no tiene borde, y el resultado pasa a ser función pura de la posición.
 *
 * ── EL ALGORITMO ────────────────────────────────────────────────────────────────────────────────
 *
 * Priority-flood, el estándar para "rellenar depresiones" en un heightfield:
 *   1. EL MAR ES LA SEMILLA. Todo téxel por debajo del nivel del mar entra en un montículo por
 *      altura. Es por donde el agua escapa del continente.
 *   2. Se saca el más bajo y se propaga a sus vecinos: el nivel de un vecino es el MÁXIMO entre el
 *      nivel desde el que llegamos y su propio terreno. Ese máximo ES su punto de derrame.
 *   3. El agua de un téxel es `nivel − terreno`.
 *
 * ⚠️ SIN MAR NO HAY AGUA, Y ESO NO ES UN ATAJO: ES EL RESULTADO. Sin téxeles bajo el nivel del mar
 * no hay semilla, el montículo nace vacío y el mapa sale seco. Un cuerpo sin océano —una luna— da
 * `hasWater = false` **derivado del propio campo**, no configurado. Es lo que permite que el resto
 * del motor salga por la puerta rápida en vez de pagar un sistema de agua que no tiene nada que
 * hacer. Andoni lo pidió explícitamente: *"quiero que haya planetas sin agua"*.
 *
 * ⚠️ Y ES LO CORRECTO, no una simplificación cómoda: este relleno da el nivel MÁXIMO que una cuenca
 * puede retener (su punto de derrame), y quien lo llena es la hidrología. Sin océano no hay ciclo
 * del agua que modelar aquí.
 *
 * ── TOPOLOGÍA DE ESFERA ─────────────────────────────────────────────────────────────────────────
 *
 * ⚠️ EL MAPA ES EQUIRECT PERO EL PLANETA ES UNA ESFERA, y tratarlo como un rectángulo reintroduce
 * justo el bug del que se viene huyendo: el meridiano ±180° y los dos polos serían BORDES, o sea
 * fugas de agua donde no las hay. La longitud envuelve, y la fila del polo conecta con ella misma
 * desplazada media vuelta (`x + w/2`), que es el vecino de verdad al cruzar el polo.
 *
 * ── DETERMINISMO ────────────────────────────────────────────────────────────────────────────────
 *
 * El montículo ordena por `pair<nivel, índice>`, así que dos téxeles con el mismo nivel salen
 * SIEMPRE en el mismo orden. Sin el desempate por índice el resultado dependería del orden de
 * inserción, y dos clientes con el mismo planeta podrían obtener lagos distintos — que es
 * exactamente lo que este fichero viene a impedir.
 */

#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace Haruka { namespace Planet {

/// Cota que marca "aquí no hay lámina". No es 0: 0 es el nivel del mar y es una cota válida.
inline constexpr float WATER_FILL_DRY = -1.0e30f;

/// Profundidad mínima para llamar lago a una hondonada. Sin ella, cada dolina de un téxel se llena y
/// el resultado es un moteado de charcos de milímetros por todo el planeta — ruido, no lagos.
/// Es el mismo criterio (y el mismo número) que usaba `seedLakes` en el parche.
inline constexpr float WATER_FILL_MIN_DEPTH_M = 0.25f;

/// "Aquí el viento tiene todo el recorrido que quiera": el océano. Se elige un valor explícito, y no
/// un infinito, para que el gemelo GLSL pueda compararlo con el mismo número.
inline constexpr float WATER_FETCH_UNLIMITED = 1.0e7f;

/// El resultado del relleno. `levelM` tiene un valor por téxel del bake, en el mismo orden.
struct WaterFillResult {
    std::vector<float> levelM;        ///< cota de la lámina (m sobre el nivel del mar). `WATER_FILL_DRY` si no hay.
    /// ── FETCH: cuánto recorrido de agua abierta hay para que el viento levante ola ──────────────
    ///
    /// ⚠️ SIN ESTO, UN LAGO DE MONTAÑA TIENE OLAS DE MAR ABIERTO, y no es estético: medido, un lago
    /// de 3,21 m de fondo recibía **3,46 m de ola** — más alta que profundo era el lago. La causa es
    /// que el oleaje sale de un único `OceanState` global (el viento sobre el océano) y el bajío lo
    /// AMPLIFICA al entrar en agua somera, en vez de reconocer que ahí no hay recorrido para
    /// levantarla.
    ///
    /// El fetch es la variable física que lo decide, y aquí sale casi gratis: el relleno ya conoce
    /// las masas de agua conexas. Se guarda el DIÁMETRO EQUIVALENTE (`sqrt(área)`).
    std::vector<float> fetchM;        ///< diámetro equivalente de la masa de agua (m). 0 si no hay.
    std::size_t        lakeCells = 0; ///< téxeles de LAGO (sin contar el océano)
    std::size_t        bodies    = 0; ///< masas de agua conexas encontradas (lagos)
    float              biggestFetchM = 0.0f;  ///< el fetch del lago más grande
    std::size_t        seaCells  = 0; ///< téxeles por debajo del nivel del mar
    bool               hasWater  = false;  ///< ¿hay océano o lagos? Derivado, no configurado.
    float              deepestM  = 0.0f;   ///< el lago más hondo encontrado (m)
};

/**
 * @brief Rellena las cuencas del planeta hasta su punto de derrame.
 *
 * @param heightM   bake de altura equirect, en metros sobre el nivel del mar, fila mayor.
 * @param w,h       dimensiones del bake (equirect 2:1, `h = w/2`).
 * @param seaLevelM cota de la lámina del océano (normalmente 0; la marea NO entra aquí, porque esto
 *                  se hornea una vez y la marea cambia cada minuto).
 */
inline WaterFillResult waterFillEquirect(const float* heightM, int w, int h, float seaLevelM = 0.0f,
                                        double planetRadiusM = 6371000.0) {
    WaterFillResult out;
    if (!heightM || w < 4 || h < 2) return out;
    const std::size_t N = (std::size_t)w * (std::size_t)h;
    out.levelM.assign(N, WATER_FILL_DRY);
    out.fetchM.assign(N, 0.0f);

    std::vector<char> done(N, 0);
    // Montículo por nivel, el más BAJO primero. El `int` del par desempata: ver la nota de arriba.
    std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
                        std::greater<std::pair<float, int>>> heap;

    // ── PASO 1: EL MAR ES LA SEMILLA ────────────────────────────────────────────────────────────
    for (std::size_t c = 0; c < N; ++c) {
        if (heightM[c] < seaLevelM) {
            done[c] = 1;
            // El océano no retiene: su nivel es el del mar, y de ahí parte la propagación.
            out.levelM[c] = seaLevelM;
            heap.emplace(seaLevelM, (int)c);
            ++out.seaCells;
        }
    }
    // Sin mar no hay semilla: el mapa sale seco y `hasWater` es false. Ver la nota de la cabecera.
    if (heap.empty()) return out;

    // Los cuatro vecinos de `(x,y)` en la ESFERA. Devuelve cuántos ha escrito (siempre 4; la función
    // existe para que la topología esté en UN sitio y no repartida por el bucle).
    auto neighbours = [w, h](int x, int y, int* nx, int* ny) {
        // Longitud: envuelve.
        nx[0] = (x + 1) % w;      ny[0] = y;
        nx[1] = (x - 1 + w) % w;  ny[1] = y;
        // Latitud: al pasarse del polo se sale por el otro lado del meridiano.
        if (y + 1 < h) { nx[2] = x; ny[2] = y + 1; }
        else           { nx[2] = (x + w / 2) % w; ny[2] = h - 1; }
        if (y - 1 >= 0) { nx[3] = x; ny[3] = y - 1; }
        else            { nx[3] = (x + w / 2) % w; ny[3] = 0; }
    };

    // ── PASO 2: PROPAGAR ────────────────────────────────────────────────────────────────────────
    int nx[4], ny[4];
    while (!heap.empty()) {
        const std::pair<float, int> top = heap.top(); heap.pop();
        const float lv = top.first;
        const int   c  = top.second;
        const int   cx = c % w, cy = c / w;
        neighbours(cx, cy, nx, ny);
        for (int k = 0; k < 4; ++k) {
            const std::size_t nc = (std::size_t)ny[k] * (std::size_t)w + (std::size_t)nx[k];
            if (nc == (std::size_t)c || done[nc]) continue;
            done[nc] = 1;
            // El nivel del vecino no puede bajar del que traemos: si su terreno es más bajo, queda
            // SUMERGIDO hasta el punto de derrame por el que hemos llegado. Eso es el lago.
            const float lvl = (heightM[nc] > lv) ? heightM[nc] : lv;
            out.levelM[nc] = lvl;
            heap.emplace(lvl, (int)nc);
        }
    }

    // ── PASO 3: QUÉ ES LAGO Y QUÉ ES SUELO MOJADO DE NADA ───────────────────────────────────────
    //
    // Sólo se conserva la lámina donde de verdad hay una: en el resto se pone el centinela, para que
    // quien lea el mapa no tenga que restar para saber si hay agua. El océano se DESCARTA aquí a
    // propósito: lo decide la cota (`elevKm < seaKm`), que es la regla del shader, y dejarlo también
    // aquí daría dos respuestas a "¿hay agua?" — el error que este motor ya ha pagado tres veces.
    for (std::size_t c = 0; c < N; ++c) {
        if (heightM[c] < seaLevelM) { out.levelM[c] = WATER_FILL_DRY; continue; }
        const float depth = out.levelM[c] - heightM[c];
        if (out.levelM[c] <= WATER_FILL_DRY || depth <= WATER_FILL_MIN_DEPTH_M) {
            out.levelM[c] = WATER_FILL_DRY;
        } else {
            ++out.lakeCells;
            if (depth > out.deepestM) out.deepestM = depth;
        }
    }
    out.hasWater = (out.seaCells > 0) || (out.lakeCells > 0);

    // ── PASO 4: EL FETCH DE CADA MASA DE AGUA ───────────────────────────────────────────────────
    //
    // Componentes conexas sobre los téxeles de LAGO, con la MISMA topología de esfera que el relleno
    // (si no, un lago a caballo del meridiano contaría como dos y su fetch saldría a la mitad).
    // Recorrido con pila explícita: la recursión sobre un lago continental la desbordaría.
    //
    // ⚠️ EL ÁREA SE MIDE EN LA ESFERA, NO EN TÉXELES. Un téxel equirect cerca del polo cubre mucha
    // menos superficie que uno en el ecuador (factor `cos(lat)`), así que contar téxeles le daría a
    // un lago ártico un fetch varias veces mayor del real — justo donde los lagos abundan.
    if (out.lakeCells > 0) {
        const double kPi  = 3.14159265358979323846;
        const double dLon = 2.0 * kPi / (double)w;
        const double dLat = kPi / (double)h;
        const double R2   = planetRadiusM * planetRadiusM;
        std::vector<int> label(N, -1);
        std::vector<std::size_t> stack, cells;
        for (std::size_t seed = 0; seed < N; ++seed) {
            if (out.levelM[seed] <= WATER_FILL_DRY || label[seed] >= 0) continue;
            const int id = (int)out.bodies++;
            double area = 0.0;
            cells.clear();
            stack.clear(); stack.push_back(seed); label[seed] = id;
            while (!stack.empty()) {
                const std::size_t c = stack.back(); stack.pop_back();
                cells.push_back(c);
                const int cx = (int)(c % (std::size_t)w), cy = (int)(c / (std::size_t)w);
                const double lat = (0.5 - ((double)cy + 0.5) / (double)h) * kPi;
                area += R2 * dLon * dLat * std::cos(lat);
                neighbours(cx, cy, nx, ny);
                for (int k = 0; k < 4; ++k) {
                    const std::size_t nc = (std::size_t)ny[k] * (std::size_t)w + (std::size_t)nx[k];
                    if (label[nc] >= 0 || out.levelM[nc] <= WATER_FILL_DRY) continue;
                    label[nc] = id;
                    stack.push_back(nc);
                }
            }
            const float fetch = (float)std::sqrt(area > 0.0 ? area : 0.0);
            for (std::size_t c : cells) out.fetchM[c] = fetch;
            if (fetch > out.biggestFetchM) out.biggestFetchM = fetch;
        }
    }
    // El OCÉANO no se etiqueta: es una sola masa que da la vuelta al planeta y su fetch es, a efectos
    // del oleaje, ilimitado. Se marca con el centinela para que quien lea sepa "no lo limites".
    for (std::size_t c = 0; c < N; ++c)
        if (heightM[c] < seaLevelM) out.fetchM[c] = WATER_FETCH_UNLIMITED;
    return out;
}

}} // namespace Haruka::Planet
