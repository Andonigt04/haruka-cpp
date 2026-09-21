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
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
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

// ════════════════════════════════════════════════════════════════════════════════════════════════
// EL PASO FINO: UNA VENTANA, SOBRE EL TERRENO DE VERDAD
//
// ⚠️ POR QUE HACE FALTA UN SEGUNDO NIVEL, CON LAS CIFRAS. El relleno de arriba corre sobre el bake de
// altura, que es una equirect de `m_mapRes` (512 por defecto). En la Tierra eso son **78,2 km por
// texel**, asi que una cuenca menor que eso NO EXISTE en el campo: el mapa da mares interiores, no
// lagos. Y no se arregla subiendo la resolucion global, porque no llega ni de lejos:
//
//     resolucion    texel      celdas      memoria (altura+nivel+fetch)
//        512 x 256   78,2 km    131 k      1,6 MB
//       2048 x 1024  19,5 km    2,1 M       25 MB
//       8192 x 4096   4,9 km     34 M      402 MB       <- y un lago mide ~1 km
//
// O sea que el nivel global se paga en memoria al cuadrado y aun asi se queda dos ordenes por encima
// del tamaño de un lago. La salida es partir el problema en dos, y se puede porque las dos mitades no
// son la misma clase de cosa:
//
//   · LA COTA DE DERRAME es GLOBAL por naturaleza. `nivel(x) = min sobre caminos hasta el mar del
//     maximo de altura del camino`: depende de la cuenca entera, y no se puede refinar un trozo sin
//     saber lo que pasa fuera. Eso lo da `waterFillEquirect`, una vez, barato.
//   · LA ORILLA es LOCAL: `hay agua ⟺ h(x) < nivel`. Con la cota ya conocida, refinar la orilla es
//     volver a inundar SOLO la ventana, muestreando el terreno de verdad (base + detalle), con lo que
//     el relleno grueso dice en el BORDE como condicion de contorno.
//
// Eso es este `waterFillWindow`. La condicion de contorno es lo que lo hace correcto y no un recorte:
// cada celda del borde entra al monticulo con el nivel que el relleno global le asigno si alli hay
// agua, y con SU PROPIA ALTURA si alli hay tierra (una celda de tierra en el borde es una pared de
// esa altura para cualquier camino que quiera salir por ahi). Sin seedear el borde no hay de donde
// propagar y la ventana sale vacia; seedeandolo mal, la cuenca hereda una cota que no es la suya.
//
// ⚠️ Y EL NIVEL DEL BORDE NUNCA BAJA DE SU PROPIA ALTURA (`max(coarse, height)`), porque el agua no
// puede estar por debajo del suelo. Eso hace que el troceado sea seguro por construccion: un relleno
// grueso equivocado puede hacer que la ventana se inunde de MAS (si dice que fuera hay una lamina mas
// alta), pero no puede vaciarla inventando un desague que el terreno fino no tiene. Lo mide
// `water_fill_window` con las dos condiciones de contorno, seca y con lamina alta.
//
// ⚠️ LO QUE ESTA FUNCION NO HACE, dicho aqui para que no se de por hecho: no la consume nadie
// todavia. Decidir QUE ventana, CUANDO recalcularla y donde cachearla es un sistema de streaming, y
// no esta escrito. Esto es el nucleo algoritmico y su prueba.
// ════════════════════════════════════════════════════════════════════════════════════════════════

/// Resultado del relleno de una ventana. Mismas unidades y centinelas que `WaterFillResult`.
struct WaterWindowResult {
    int   w = 0, h = 0;
    std::vector<float> heightM;   ///< el terreno FINO que se uso (m); util para el consumidor
    std::vector<float> levelM;    ///< cota de la lamina (m). `WATER_FILL_DRY` si no hay
    std::vector<float> fetchM;    ///< diametro equivalente (m). Ver `clippedBodies`
    std::vector<uint8_t> clipped; ///< 1 = esta celda pertenece a una masa que TOCA el borde
    std::size_t lakeCells = 0;
    std::size_t bodies    = 0;
    /// ⚠️ Masas que tocan el borde de la ventana. Su `fetchM` es una COTA INFERIOR: el lago sigue
    /// fuera y aqui no se ve entero. Quien lo consuma debe quedarse con el fetch del relleno GRUESO
    /// para esas, o ampliar la ventana. Sin esta cuenta, un lago cortado daria ola de charca.
    std::size_t clippedBodies = 0;
    double texelM = 0.0;          ///< lado del texel de la ventana (m) en su latitud central
    float  deepestM = 0.0f;
};

/**
 * @brief Relleno FINO de una ventana lat/lon, con el relleno grueso como condicion de contorno.
 *
 * @param w,h          resolucion de la ventana.
 * @param lonC,latC    centro de la ventana (radianes; lat en [-pi/2, pi/2]).
 * @param halfLon,halfLat  medio ancho de la ventana (radianes).
 * @param heightAt     el terreno DE VERDAD en (lon,lat): base + detalle, en metros. Es el mismo que
 *                     pisa el jugador — si aqui se muestreara el bake grueso, la ventana no veria
 *                     nada que el relleno global no viera ya.
 * @param borderLevelM lo que el relleno GRUESO dice en (lon,lat): la cota de la lamina, o
 *                     `WATER_FILL_DRY` si alli no hay agua. Solo se consulta en el BORDE.
 */
inline WaterWindowResult waterFillWindow(int w, int h,
                                         double lonC, double latC, double halfLon, double halfLat,
                                         double planetRadiusM,
                                         const std::function<float(double, double)>& heightAt,
                                         const std::function<float(double, double)>& borderLevelM,
                                         float seaLevelM = 0.0f)
{
    WaterWindowResult out;
    if (w < 4 || h < 4 || !heightAt || !borderLevelM) return out;
    const std::size_t N = (std::size_t)w * (std::size_t)h;
    out.w = w; out.h = h;
    out.heightM.assign(N, 0.0f);
    out.levelM.assign(N, WATER_FILL_DRY);
    out.fetchM.assign(N, 0.0f);
    out.clipped.assign(N, 0);

    const double lon0 = lonC - halfLon, lat0 = latC - halfLat;
    const double dLon = (2.0 * halfLon) / (double)w;
    const double dLat = (2.0 * halfLat) / (double)h;
    out.texelM = planetRadiusM * dLon * std::cos(latC);   // lado E-O en la latitud central

    auto lonOf = [&](int x) { return lon0 + ((double)x + 0.5) * dLon; };
    auto latOf = [&](int y) { return lat0 + ((double)y + 0.5) * dLat; };

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out.heightM[(std::size_t)y * w + x] = heightAt(lonOf(x), latOf(y));

    std::vector<char> done(N, 0);
    std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
                        std::greater<std::pair<float, int>>> heap;

    // ── SEMILLAS ────────────────────────────────────────────────────────────────────────────────
    // (a) el mar, dentro de la ventana: nivel del mar y a propagar.
    // (b) EL BORDE, con lo que dice el relleno grueso. Sin esto la ventana tendria cuatro salidas
    //     libres al mar y saldria seca entera — el error clasico al trocear un priority-flood.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t c = (std::size_t)y * w + x;
            if (out.heightM[c] < seaLevelM) {
                done[c] = 1; out.levelM[c] = seaLevelM; heap.emplace(seaLevelM, (int)c);
                continue;
            }
            const bool border = (x == 0 || y == 0 || x == w - 1 || y == h - 1);
            if (!border) continue;
            const float coarse = borderLevelM(lonOf(x), latOf(y));
            // Agua fuera → se sale por ahi a esa cota. Tierra fuera → pared de su propia altura.
            const float lvl = (coarse > WATER_FILL_DRY) ? std::max(coarse, out.heightM[c])
                                                        : out.heightM[c];
            done[c] = 1; out.levelM[c] = lvl; heap.emplace(lvl, (int)c);
        }
    }
    if (heap.empty()) return out;

    // ── PROPAGACION ─────────────────────────────────────────────────────────────────────────────
    // Idéntica al relleno global salvo la topologia: una ventana NO envuelve, sus bordes ya son
    // semilla. El nivel de un vecino es el mayor entre su altura y el nivel de donde se viene.
    while (!heap.empty()) {
        const auto top = heap.top(); heap.pop();
        const int  c   = top.second;
        const float lv = out.levelM[(std::size_t)c];
        const int cx = c % w, cy = c / w;
        const int nx[4] = { cx - 1, cx + 1, cx, cx };
        const int ny[4] = { cy, cy, cy - 1, cy + 1 };
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || nx[k] >= w || ny[k] < 0 || ny[k] >= h) continue;
            const std::size_t nc = (std::size_t)ny[k] * w + nx[k];
            if (done[nc]) continue;
            done[nc] = 1;
            const float lvl = (out.heightM[nc] > lv) ? out.heightM[nc] : lv;
            out.levelM[nc] = lvl;
            heap.emplace(lvl, (int)nc);
        }
    }

    // ── QUE ES LAGO ─────────────────────────────────────────────────────────────────────────────
    for (std::size_t c = 0; c < N; ++c) {
        if (out.heightM[c] < seaLevelM) { out.levelM[c] = WATER_FILL_DRY; continue; }
        const float depth = out.levelM[c] - out.heightM[c];
        if (out.levelM[c] <= WATER_FILL_DRY || depth <= WATER_FILL_MIN_DEPTH_M) {
            out.levelM[c] = WATER_FILL_DRY;
        } else {
            ++out.lakeCells;
            if (depth > out.deepestM) out.deepestM = depth;
        }
    }

    // ── FETCH, y QUE MASAS ESTAN CORTADAS ───────────────────────────────────────────────────────
    if (out.lakeCells > 0) {
        const double R2 = planetRadiusM * planetRadiusM;
        std::vector<int> label(N, -1);
        std::vector<std::size_t> stack, cells;
        for (std::size_t seed = 0; seed < N; ++seed) {
            if (out.levelM[seed] <= WATER_FILL_DRY || label[seed] >= 0) continue;
            const int id = (int)out.bodies++;
            double area = 0.0; bool touches = false;
            cells.clear(); stack.clear();
            stack.push_back(seed); label[seed] = id;
            while (!stack.empty()) {
                const std::size_t c = stack.back(); stack.pop_back();
                cells.push_back(c);
                const int cx = (int)(c % (std::size_t)w), cy = (int)(c / (std::size_t)w);
                if (cx == 0 || cy == 0 || cx == w - 1 || cy == h - 1) touches = true;
                area += R2 * dLon * dLat * std::cos(latOf(cy));
                const int nx2[4] = { cx - 1, cx + 1, cx, cx };
                const int ny2[4] = { cy, cy, cy - 1, cy + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx2[k] < 0 || nx2[k] >= w || ny2[k] < 0 || ny2[k] >= h) continue;
                    const std::size_t nc = (std::size_t)ny2[k] * w + nx2[k];
                    if (label[nc] >= 0 || out.levelM[nc] <= WATER_FILL_DRY) continue;
                    label[nc] = id; stack.push_back(nc);
                }
            }
            const float fetch = (float)std::sqrt(area > 0.0 ? area : 0.0);
            for (std::size_t c : cells) { out.fetchM[c] = fetch; out.clipped[c] = touches ? 1 : 0; }
            if (touches) ++out.clippedBodies;
        }
    }
    return out;
}

// ── EL MUESTREO DE LA VENTANA, EN UN SOLO SITIO ─────────────────────────────────────────────────
//
// ⚠️ VIVE AQUI Y NO EN `TerrestrialPlanet` A PROPOSITO. Lo consultan la fisica (`lakeLevelAt`), el
// render (`harukaLakeWinUV` del .glsl, su gemelo literal) y el banco. Con tres copias de la misma
// cuenta, la primera que se toque separa el agua que se nada de la que se ve — que es el error que
// este motor ya ha pagado tres veces. Aqui hay UNA, y el test la compara con la de la GPU.
//
// El recuadro se describe con su centro y el inverso de su lado: `uv = delta·inv + 0,5`. La longitud
// se desenvuelve antes, o un recuadro a caballo de ±π rechazaria la mitad de si mismo.

/// (lon, lat) de una direccion, con la MISMA convencion que `equirectUV`.
inline void waterDirToLonLat(const glm::dvec3& d, double& lon, double& lat) {
    const glm::dvec3 n = glm::normalize(d);
    lon = std::atan2(n.z, n.x);
    lat = std::asin(n.y < -1.0 ? -1.0 : (n.y > 1.0 ? 1.0 : n.y));
}

/// UV dentro del recuadro. Fuera de [0,1) = el punto no cae en la ventana. Gemelo de `harukaLakeWinUV`.
inline glm::dvec2 waterWindowUV(const glm::dvec3& dir, double lonC, double latC,
                                double invSpanLon, double invSpanLat) {
    double lon = 0.0, lat = 0.0;
    waterDirToLonLat(dir, lon, lat);
    double dLon = lon - lonC;
    const double kTwoPi = 6.28318530717958647692;
    dLon -= kTwoPi * std::floor(dLon / kTwoPi + 0.5);
    return glm::dvec2(dLon * invSpanLon + 0.5, (lat - latC) * invSpanLat + 0.5);
}

/// Cota de la lamina en la ventana, NEAREST. `WATER_FILL_DRY` si el punto cae fuera o no hay lago.
inline float waterWindowLevelAt(const WaterWindowResult& win, const glm::dvec3& dir,
                                double lonC, double latC, double invSpanLon, double invSpanLat) {
    if (win.w <= 0 || win.h <= 0 || win.levelM.empty()) return WATER_FILL_DRY;
    const glm::dvec2 uv = waterWindowUV(dir, lonC, latC, invSpanLon, invSpanLat);
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) return WATER_FILL_DRY;
    int x = (int)std::floor(uv.x * win.w); x = x < 0 ? 0 : (x >= win.w ? win.w - 1 : x);
    int y = (int)std::floor(uv.y * win.h); y = y < 0 ? 0 : (y >= win.h ? win.h - 1 : y);
    return win.levelM[(std::size_t)y * win.w + x];
}

/// Fetch en la ventana, NEAREST. `WATER_FETCH_UNLIMITED` si cae fuera o no hay masa ahi.
inline float waterWindowFetchAt(const WaterWindowResult& win, const glm::dvec3& dir,
                                double lonC, double latC, double invSpanLon, double invSpanLat) {
    if (win.w <= 0 || win.h <= 0 || win.fetchM.empty()) return WATER_FETCH_UNLIMITED;
    const glm::dvec2 uv = waterWindowUV(dir, lonC, latC, invSpanLon, invSpanLat);
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) return WATER_FETCH_UNLIMITED;
    int x = (int)std::floor(uv.x * win.w); x = x < 0 ? 0 : (x >= win.w ? win.w - 1 : x);
    int y = (int)std::floor(uv.y * win.h); y = y < 0 ? 0 : (y >= win.h ? win.h - 1 : y);
    const float f = win.fetchM[(std::size_t)y * win.w + x];
    return (f > 0.0f) ? f : WATER_FETCH_UNLIMITED;
}

/**
 * @brief ¿Hay que recentrar la ventana en el observador?
 *
 * ⚠️ LA HISTERESIS NO ES UN ADORNO. Si se recalculara al pisar el BORDE, un jugador andando justo por
 * el limite dispararia un recalculo por frame — 65 536 muestras del terreno real cada vez. Y si se
 * recalculara solo al salir del todo, habria frames con el observador FUERA de la ventana y sin lago
 * donde si lo hay.
 *
 * Se recentra al salir de la MITAD interior. Con eso quedan garantizados **medio recuadro de margen
 * por delante** en cualquier direccion (4 km con el recuadro de ±8 km), que es lo que se ve, y entre
 * dos recalculos hay que recorrer al menos esa distancia. Lo fija `water_window_policy`.
 *
 * @param haveWindow  si ya hay una ventana valida (si no, siempre hay que calcularla).
 * @param lonC,latC   centro de la ventana vigente (rad).
 * @param halfM       medio lado del recuadro (m).
 */
inline bool waterWindowNeedsRecenter(const glm::dvec3& observerDir, bool haveWindow,
                                     double lonC, double latC, double halfM, double radiusM) {
    if (!haveWindow || !(radiusM > 0.0) || !(halfM > 0.0)) return true;
    const glm::dvec3 c(std::cos(latC) * std::cos(lonC), std::sin(latC),
                       std::cos(latC) * std::sin(lonC));
    const glm::dvec3 d = glm::normalize(observerDir);
    double dot = glm::dot(d, c);
    dot = dot < -1.0 ? -1.0 : (dot > 1.0 ? 1.0 : dot);
    return (std::acos(dot) * radiusM) > (halfM * 0.5);
}

}} // namespace Haruka::Planet
