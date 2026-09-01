/**
 * @file terrain_node.h
 * @brief DIRECCIONAMIENTO DE NODOS del quadtree de terreno (v5). Enteros dentro, dirección fuera.
 *
 * Es la pieza F1 de `docs/guides/PLAN_TERRENO_V5.md` y el cimiento de todo el plan: si esto no es
 * exacto, nada de lo que se apoye encima puede serlo.
 *
 * ── QUÉ RESUELVE, Y POR QUÉ NO ES "OTRA FORMA DE HACER LO MISMO" ────────────────────────────────
 *
 * El clipmap derivaba la posición de cada vértice de un MARCO TANGENTE EN FLOAT que se re-anclaba
 * con el jugador. Eso tenía dos consecuencias medidas, y las dos desaparecen aquí:
 *
 *   1. La `dir` reconstruida en float se separa **0,9302 m** de la que calcula la CPU en double, lo
 *      que produce hasta **0,1453 m** de diferencia de altura. Lo medía `test_clipmap_dir_parity`,
 *      borrado con el clipmap; hoy la propiedad la vigila `terrain_node_lattice` (ver abajo).
 *   2. `collision-mesh-rebuild-cost`: al derivar 48 m, **8 de 257 049** nodos son reutilizables —
 *      porque el ancla se mueve y con ella TODOS los nodos.
 *
 * Aquí la posición de un téxel es función EXACTA de enteros (cara, nivel, índice de nodo, índice de
 * téxel). No hay ancla, así que andar no invalida nada, y no hay float en el camino hasta el final.
 *
 * ── LA PROPIEDAD QUE LO HACE FUNCIONAR: NUMERADOR ENTERO, UNA SOLA DIVISIÓN ─────────────────────
 *
 * La coordenada de cara de un téxel es `lx = -1 + 2·num / den`, con `num` y `den` calculados en
 * ENTEROS y una única división al final.
 *
 * ⚠️ Aquí ponía que la exactitud venía del denominador potencia de dos. **Es falso, y el test lo
 * demostró**: el hijo calcula `2·num / 2·den` y el padre `num / den`, y duplicar numerador y
 * denominador es exacto en cualquier float de base 2 — así que padre e hijo coinciden con CUALQUIER
 * número de celdas. Lo que da la exactitud es no meter float en el camino hasta el final.
 *
 * Lo que sí aporta la potencia de dos es que cada `lx` sea **exactamente representable**, lo cual
 * importa aguas abajo (float y GPU), no aquí. Por eso se conserva, pero por el motivo correcto.
 *
 * De todo ello salen, sin tolerancia, las tres propiedades que el quadtree necesita:
 *
 *   · **Grueso ⊂ fino**: el denominador grueso DIVIDE al fino, así que un téxel de nivel L cae
 *     BIT A BIT sobre uno de nivel L+1. No "casi": el mismo double.
 *   · **Sin grietas**: dos nodos vecinos evalúan su arista compartida con el mismo numerador y el
 *     mismo denominador → la misma dirección. La costura no puede abrirse.
 *   · **Determinismo**: mismos enteros, mismos bits, en cualquier máquina y en cualquier orden.
 *
 * Es la misma garantía que el clipmap compraba redondeando el nivel de tesela a potencia de dos,
 * pero por construcción y sin perder resolución en el redondeo.
 *
 * Medido en `terrain_node_lattice` contra el camino que esto sustituye —reconstruir la dirección por
 * un marco tangente en float, como sigue haciendo `ringSample`— la diferencia es **0 bits aquí
 * contra 0,2290 m de superficie allí**, sobre los mismos puntos.
 *
 * ⚠️ NO duplica la proyección. La cara→esfera es `cubeFaceToDir` (Cobb), que ya existe, es GL-free
 * y tiene gemelo GLSL en `lib/cube_face.glsl`. Aquí solo se decide QUÉ (lx, ly) evaluar.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>   // memcpy — el hash golden va sobre los BITS, no sobre el valor
#include <glm/glm.hpp>

#include "tools/planetary_types.h"   // PlanetFace
#include "cube_sphere.h"             // cubeFaceToDir — la proyección, que NO se reimplementa aquí
#include "core/planet/terrain_detail.h"   // terrainDetail(dvec3,...) — la altura, gemela del .glsl

namespace Haruka { namespace Terrain {

/**
 * @brief Téxeles por lado de un nodo.
 *
 * `N-1` es potencia de dos para que cada `lx` sea exactamente representable (ver la nota de arriba;
 * NO es lo que da la coincidencia padre↔hijo). 129 → 128 celdas. El `+1` es porque los nodos comparten
 * la fila del borde con sus vecinos (si no, la arista compartida no tendría téxel a los dos lados y
 * la costura habría que interpolarla, que es de donde salen las grietas).
 */
inline constexpr uint32_t TERRAIN_NODE_TEXELS = 129u;
inline constexpr uint32_t TERRAIN_NODE_CELLS  = TERRAIN_NODE_TEXELS - 1u;   // 128 = 2^7

/**
 * @brief Nivel máximo. **Lo fija la escalera de ruido, NO la pantalla.**
 *
 * ⚠️ ESTABA EN 20 Y ERA TIRAR MEMORIA. A nivel 20 el téxel mide 7,5 cm, pero la octava más fina de
 * `terrain_detail.h` es λ = 4,5 m: por debajo de ~1,1 m/téxel (λ/4, donde esa octava ya entra a peso
 * completo) el nodo guarda una interpolación de sí mismo. Información que no existe.
 *
 * Y se paga: medido con el selector a 2 m de altura, los niveles 18-20 eran **2 732 de 5 439 nodos**
 * — la mitad del presupuesto de VRAM dedicada a detalle que el terreno no tiene. El nivel 17 da
 * 0,60 m/téxel, ya por debajo del umbral, así que es el primero que lo resuelve del todo.
 *
 * El detalle más fino que eso es cosa de las TEXTURAS del material, no de la geometría. Si algún día
 * la escalera de ruido gana una octava por debajo de λ 4,5 m, este número sube con ella — y
 * `terrain_node_scale` lo vigila.
 */
inline constexpr uint32_t TERRAIN_NODE_MAX_LEVEL = 17u;

/** @brief Identidad de un nodo del quadtree. Solo enteros: es la clave del pool y del test golden. */
struct NodeId {
    PlanetFace face  = PlanetFace::FRONT;
    uint32_t   level = 0;   ///< 0 = la cara entera; 2^level nodos por lado
    uint32_t   i     = 0;   ///< índice de nodo en el eje lx, [0, 2^level)
    uint32_t   j     = 0;   ///< índice de nodo en el eje ly, [0, 2^level)

    bool operator==(const NodeId& o) const {
        return face == o.face && level == o.level && i == o.i && j == o.j;
    }
};

/**
 * @brief Coordenada de cara de un téxel, EXACTA. `u`,`v` ∈ [0, TERRAIN_NODE_TEXELS-1].
 *
 * `lx = -1 + 2·(i·CELLS + u) / (2^level · CELLS)`.
 *
 * Numerador en enteros y UNA división. **No meter float antes del final** ni partir la división en
 * dos pasos: la exactitud es la característica, no un efecto secundario, y `terrain_node_lattice` la
 * mide contra el camino que esto sustituye.
 */
inline void nodeTexelFaceCoord(const NodeId& n, uint32_t u, uint32_t v,
                               double& outLx, double& outLy) {
    const double den = (double)(1ull << n.level) * (double)TERRAIN_NODE_CELLS;
    outLx = -1.0 + 2.0 * ((double)((uint64_t)n.i * TERRAIN_NODE_CELLS + u) / den);
    outLy = -1.0 + 2.0 * ((double)((uint64_t)n.j * TERRAIN_NODE_CELLS + v) / den);
}

/** @brief Dirección unitaria del téxel. Cara→esfera por `cubeFaceToDir` (Cobb), sin copia local. */
inline glm::dvec3 nodeTexelDir(const NodeId& n, uint32_t u, uint32_t v) {
    double lx, ly;
    nodeTexelFaceCoord(n, u, v, lx, ly);
    return cubeFaceToDir(n.face, lx, ly);
}

/**
 * @brief El vecino de `n` cruzando la arista `edge`, AUNQUE ESTÉ EN OTRA CARA DEL CUBO.
 *
 * ── POR QUÉ NO HAY UNA TABLA DE 24 CASOS ────────────────────────────────────────────────────────
 *
 * La adyacencia entre caras de un cubo son 24 combinaciones (6 caras x 4 aristas) y cada una lleva
 * su rotación y su posible reflejo. Escribirlas a mano es la receta clásica para un bug que solo
 * aparece en una arista de una cara y que nadie sabe reproducir.
 *
 * Aquí se DERIVA de la geometría, que ya está probada: se coge el punto medio de la arista, se
 * empuja media celda hacia afuera, se convierte a dirección con `cubeFaceToDir` y se vuelve con
 * `dirToCubeFaceClosed`. Ese par es exacto a 1e-8 (test `cube_sphere_inverse`), así que la cara y
 * las coordenadas que salen son las del vecino de verdad — con su rotación y su reflejo incluidos,
 * sin haberlos escrito.
 *
 * ⚠️ EL EMPUJE VA EN 3D, NO EN COORDENADAS DE CARA. Primera version: sumar un cell a `lx`/`ly` y
 * convertir. A nivel 1 un cell vale 1,0, asi que pedia `cubeFaceToDir(-1.5, -0.5)` — muy fuera del
 * dominio del spherify, y las 24 esquinas del cubo salian todas a la misma cara. Medido: 46 de 1488
 * cruces sin simetria, TODOS de nivel 1.
 *
 * Ahora se toma el punto medio de la arista (que esta EXACTAMENTE sobre la costura y es comun a las
 * dos caras) y se empuja desde el centro del nodo hacia afuera en el espacio de direcciones:
 * `normalize(em + (em - dc)·½)`. Eso son ~un cuarto de celda al otro lado, escala con el nivel solo,
 * y nunca evalua el mapeo fuera de [-1,1].
 *
 * @return false si la arista no sale de la cara (el vecino es el trivial `i±1`/`j±1`).
 */
inline bool nodeNeighbourAcrossFace(const NodeId& n, int edge, NodeId& out) {
    const uint64_t lim  = 1ull << n.level;
    const double   step = 2.0 / (double)lim;              // lado del nodo en coordenadas de cara
    const int dx[4] = { -1, +1,  0,  0 };
    const int dy[4] = {  0,  0, -1, +1 };
    const int64_t ni = (int64_t)n.i + dx[edge], nj = (int64_t)n.j + dy[edge];
    if (ni >= 0 && nj >= 0 && ni < (int64_t)lim && nj < (int64_t)lim) return false;   // misma cara

    const double cx = -1.0 + step * ((double)n.i + 0.5);
    const double cy = -1.0 + step * ((double)n.j + 0.5);
    // Punto medio de la ARISTA que se cruza: está sobre la costura, o sea en |lx|=1 o |ly|=1.
    const double ex = cx + (double)dx[edge] * step * 0.5;
    const double ey = cy + (double)dy[edge] * step * 0.5;

    const glm::dvec3 dc = cubeFaceToDir(n.face, cx, cy);   // centro del nodo
    const glm::dvec3 em = cubeFaceToDir(n.face, ex, ey);   // costura
    const glm::dvec3 d  = glm::normalize(em + (em - dc) * 0.5);   // un cuarto de celda al otro lado

    PlanetFace nf; double nlx, nly;
    dirToCubeFaceClosed(d, nf, nlx, nly);

    const double fi = (nlx + 1.0) * 0.5 * (double)lim;
    const double fj = (nly + 1.0) * 0.5 * (double)lim;
    const int64_t qi = (int64_t)std::floor(fi), qj = (int64_t)std::floor(fj);
    out.face  = nf;
    out.level = n.level;
    out.i = (uint32_t)std::min<int64_t>(std::max<int64_t>(qi, 0), (int64_t)lim - 1);
    out.j = (uint32_t)std::min<int64_t>(std::max<int64_t>(qj, 0), (int64_t)lim - 1);
    return true;
}

/** @brief Los cuatro hijos, en el orden (i,j) = (0,0) (1,0) (0,1) (1,1). */
inline void nodeChildren(const NodeId& n, NodeId out[4]) {
    for (int k = 0; k < 4; ++k) {
        out[k].face  = n.face;
        out[k].level = n.level + 1;
        out[k].i     = n.i * 2 + (uint32_t)(k & 1);
        out[k].j     = n.j * 2 + (uint32_t)((k >> 1) & 1);
    }
}

/**
 * @brief Lado del nodo en metros sobre la superficie, aproximado.
 *
 * ⚠️ APROXIMADO Y A PROPÓSITO: la proyección de Cobb no es equiárea, así que el lado real varía
 * dentro de la cara (menos que normalizando el cubo, que es la razón de usar Cobb, pero varía). Vale
 * para elegir nivel por error en pantalla; NO vale como distancia. Un cuarto de circunferencia cubre
 * una cara, de ahí el `π/2`.
 */
inline double nodeSpanM(const NodeId& n, double planetRadiusM) {
    return planetRadiusM * 1.5707963267948966 / (double)(1ull << n.level);
}

/** @brief Metros por téxel del nodo, con la misma salvedad que `nodeSpanM`. */
inline double nodeTexelM(const NodeId& n, double planetRadiusM) {
    return nodeSpanM(n, planetRadiusM) / (double)TERRAIN_NODE_CELLS;
}

// =================================================================================================
// COSTURA ENTRE NIVELES (T-junctions) — sin faldas, exacta
// =================================================================================================
//
// El problema clásico del quadtree: un nodo fino pegado a uno más grueso tiene el DOBLE de vértices
// en la arista compartida. Los que caen sobre un vértice del grueso coinciden (eso lo garantiza el
// direccionamiento: `terrain_node_lattice` lo mide, 0 distintos bit a bit). Los de en medio NO
// existen en el grueso, así que se salen de su arista recta y abren una grieta por la que se ve el
// cielo.
//
// ⚠️ LA SOLUCIÓN HABITUAL SON FALDAS —geometría extra colgando del borde para tapar el hueco— y aquí
// NO hacen falta. Como el vértice grueso cae bit a bit sobre el fino, el vértice sobrante se puede
// COLOCAR en la recta que une a sus dos vecinos que sí existen en el grueso. No queda hueco que
// tapar: la arista fina describe exactamente la misma recta que la gruesa.
//
// Faldas habría significado geometría extra, un borde que asoma en pendientes fuertes y una decisión
// de "cuánto cuelga" que nadie sabe justificar. Esto no tiene parámetros.

/** @brief A qué dos téxeles y con qué peso hay que ir para coser un vértice de borde. */
struct StitchRef {
    uint32_t u0 = 0, v0 = 0;   ///< primer ancla (existe en el vecino grueso)
    uint32_t u1 = 0, v1 = 0;   ///< segunda ancla
    double   t  = 0.0;         ///< 0 = solo la primera; el vértice va en `mix(a0, a1, t)`
    bool identity() const { return u0 == u1 && v0 == v1; }
};

/**
 * @brief Cosido de un vértice del nodo. `coarser[e]` = cuántos NIVELES más grueso es el vecino de
 *        esa arista (0 = mismo nivel o más fino → no hay nada que coser).
 *
 * Aristas: 0 = izquierda (u=0) · 1 = derecha (u=CELLS) · 2 = abajo (v=0) · 3 = arriba (v=CELLS).
 *
 * Las ESQUINAS no necesitan caso especial: su índice es 0 o CELLS, que son múltiplos de cualquier
 * potencia de dos, así que caen sobre un vértice del grueso y el peso sale 0 solo.
 */
inline StitchRef nodeStitchStep(uint32_t u, uint32_t v, const uint32_t step[4]) {
    StitchRef r{ u, v, u, v, 0.0 };
    const uint32_t E = TERRAIN_NODE_CELLS;
    int edge = -1; uint32_t idx = 0;
    if      (u == 0 && step[0] > 1) { edge = 0; idx = v; }
    else if (u == E && step[1] > 1) { edge = 1; idx = v; }
    else if (v == 0 && step[2] > 1) { edge = 2; idx = u; }
    else if (v == E && step[3] > 1) { edge = 3; idx = u; }
    if (edge < 0) return r;

    const uint32_t stride = step[edge];
    const uint32_t b = (idx / stride) * stride;
    const uint32_t nxt = std::min(b + stride, E);
    if (nxt == b) return r;
    if (edge <= 1) { r.v0 = b; r.v1 = nxt; }          // aristas verticales: varía v
    else           { r.u0 = b; r.u1 = nxt; }          // horizontales: varía u

    // ── ⚠️ EL VÉRTICE SOBRANTE SE COLAPSA (t = 0), NO SE INTERPOLA ──────────────────────────────
    //
    // Aquí ponía `r.t = (idx - b) / (nxt - b)`: el vértice se colocaba INTERPOLADO sobre la recta del
    // vecino grueso. Era correcto en el mundo —los tests lo miden en 0,000000 m y el careo GPU↔GPU da
    // 0 bits— **y roto en pantalla**: topológicamente seguía siendo un vértice que el otro lado no
    // tiene, o sea una T-JUNCTION. El rasterizador cuantiza a una rejilla sub-píxel, así que dos
    // aristas matemáticamente coincidentes se separan una fracción de píxel y dejan pinholes.
    //
    // Es lo único que ningún modelo de world-space podía ver, y costó SIETE candidatos eliminados con
    // medida antes de llegar aquí (el dato del generador, la fórmula, la precisión float, el morph,
    // la histéresis, el punto de evaluación del morph y la zancada enviada — todos limpios).
    //
    // Colapsando sobre el vértice del grueso, los triángulos que lo usaban quedan DEGENERADOS (área
    // cero, ni un fragmento) y la arista pasa a tener exactamente los mismos vértices que el vecino.
    // Medido con el shader (`v5 F3: grietas entre nodos vecinos`, barrido de 7 casos): de 21 agujeros
    // en el peor caso a **0 en los siete**.
    //
    // `u1`/`v1` se conservan: el shader los usa para la NORMAL, y quitarlos cambiaría el sombreado.
    r.t = 0.0;
    return r;
}

/**
 * @brief Envoltura histórica: `coarser[e]` = **cuántos niveles más grueso** es el vecino.
 *
 * Equivale a una zancada de `2^coarser` téxeles. Sigue valiendo mientras los dos lados dibujen con
 * el MISMO stride; en cuanto los strides difieren hay que dar la zancada explícita
 * (`nodeStitchStep`), porque la densidad de vértices ya no la fija solo el nivel.
 */
inline StitchRef nodeStitch(uint32_t u, uint32_t v, const int coarser[4]) {
    uint32_t step[4];
    for (int e = 0; e < 4; ++e)
        step[e] = (coarser[e] > 0) ? (1u << (uint32_t)coarser[e]) : 0u;
    return nodeStitchStep(u, v, step);
}

/** @brief Como `nodeStitchedDir` pero con la ZANCADA dada en téxeles (ver `nodeStitchStep`). */
inline glm::dvec3 nodeStitchedDirStep(const NodeId& n, uint32_t u, uint32_t v,
                                      const uint32_t step[4]) {
    const StitchRef s = nodeStitchStep(u, v, step);
    if (s.identity()) return nodeTexelDir(n, u, v);
    const glm::dvec3 a = nodeTexelDir(n, s.u0, s.v0);
    const glm::dvec3 b = nodeTexelDir(n, s.u1, s.v1);
    return a + (b - a) * s.t;
}

/** @brief Dirección del vértice YA COSIDA. Idéntica a `nodeTexelDir` cuando no hay nada que coser. */
inline glm::dvec3 nodeStitchedDir(const NodeId& n, uint32_t u, uint32_t v, const int coarser[4]) {
    const StitchRef s = nodeStitch(u, v, coarser);
    if (s.identity()) return nodeTexelDir(n, u, v);
    // ⚠️ Se interpolan las DIRECCIONES y luego se normaliza, que es lo que hace la rasterización del
    // vecino grueso: su arista es una RECTA entre sus dos vértices, no un arco. Interpolar los
    // ángulos daría el arco y volvería a abrir la grieta, más pequeña pero grieta.
    const glm::dvec3 a = nodeTexelDir(n, s.u0, s.v0);
    const glm::dvec3 b = nodeTexelDir(n, s.u1, s.v1);
    return a + (b - a) * s.t;                          // SIN normalizar: la recta es la recta
}

/**
 * @brief REFERENCIA del contenido de un nodo: llena `out` con `TEXELS²` alturas en metros.
 *
 * Es la implementación de referencia contra la que se valida el compute de GPU, y la que hace que el
 * test golden pueda correr HEADLESS (el banco de pruebas no tiene GPU). No es un "gemelo CPU" en el
 * sentido del v4 —el servidor del v5 tiene GPU y corre el mismo shader— sino la vara de medir.
 *
 * ⚠️ Usa la entrada en DOUBLE de `terrainDetail`. La de `vec3` redondearía la dirección a float y con
 * ella la coordenada del ruido, tirando por la firma justo la exactitud que `nodeTexelDir` calcula
 * desde enteros: a radio terrestre eso son 0,38-0,76 m de superficie. Ver la nota de esa sobrecarga.
 *
 * `minFeatureM` sale del téxel del propio nodo, no de la distancia a la cámara: un nodo describe la
 * misma superficie lo mires desde donde lo mires. **Es la diferencia de fondo con el clipmap**, donde
 * `triM` depende de dónde estás y por eso el suelo cambia al acercarte.
 */
/** @brief Rango de elevación de un nodo (m). Lo que hace viable el recorte de frustum cercano. */
struct NodeRange {
    float minM =  1e30f;
    float maxM = -1e30f;
    bool valid() const { return maxM >= minM; }
    /// Semi-amplitud + margen: el radio que hay que añadir a la esfera envolvente del nodo.
    double boundM() const { return valid() ? (double)std::max(std::abs(minM), std::abs(maxM)) : 5000.0; }
};

/// Altura base del bake (m) en una dirección. Es el `sampleBase(dir).x` de los shaders, inyectado
/// para que `terrain_node.h` NO dependa del planeta ni de la GPU (sigue siendo testeable headless).
using NodeBaseHeightFn = float (*)(const glm::dvec3& dir, void* ctx);

/**
 * @brief Rango de elevación de un nodo, muestreando la superficie en una rejilla gruesa.
 *
 * ── POR QUÉ ESTO TENÍA QUE EXISTIR ──────────────────────────────────────────────────────────────
 *
 * `TerrainNodeGpu::generatePending` publicaba `NodeRange{}` — **vacío** — para todos los nodos. El
 * log del juego lo delató: `SIN RANGO 3070` de 3070. Y `rangeOf` alimenta a TRES sitios:
 *
 *   · `nodeShouldSplit` — sin cota, la distancia se mide al NIVEL DEL MAR. En una ladera de +1 km
 *     el suelo está 1 km más cerca de lo que cree, así que subdivide de MENOS justo donde más se
 *     mira. Es el bug "mirando cuesta arriba sale disparidad" que se dio por arreglado y nunca
 *     llegó a funcionar en el juego, porque el arreglo dependía de un rango que nadie rellenaba.
 *   · `nodeStrideIndex` — igual: con el jugador a 2 m del suelo pero 1028 m sobre el mar, cree que
 *     el nodo está a 1028 m y deja el stride en 4. Síntoma: `stride 4..4` en vez de `1..4`.
 *   · `nodeOutsideFrustum` — cae al bound conservador de 5000 m.
 *
 * Se muestrea en 5×5 y se ensancha un 25 % de la amplitud, porque 25 puntos no ven el pico. Errar
 * por ARRIBA es el lado seguro: el nodo parece más cerca y se subdivide de más.
 */
inline NodeRange nodeEstimateRange(const NodeId& n, double planetRadiusM,
                                   NodeBaseHeightFn heightFn, void* ctx) {
    NodeRange r;
    if (!heightFn) return r;
    const uint32_t step = TERRAIN_NODE_CELLS / 4;      // 5x5 puntos contando los bordes
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
            const float h = heightFn(nodeTexelDir(n, u, v), ctx);
            r.minM = std::min(r.minM, h);
            r.maxM = std::max(r.maxM, h);
        }
    if (!r.valid()) return NodeRange{};
    const float pad = 0.25f * (r.maxM - r.minM);
    r.minM -= pad; r.maxM += pad;
    (void)planetRadiusM;
    return r;
}


inline void nodeFillHeights(const NodeId& n, double planetRadiusM, float* out,
                            NodeRange* outRange = nullptr,
                            NodeBaseHeightFn baseFn = nullptr, void* baseCtx = nullptr) {
    const float triM = (float)nodeTexelM(n, planetRadiusM);
    NodeRange rg;
    for (uint32_t v = 0; v < TERRAIN_NODE_TEXELS; ++v)
        for (uint32_t u = 0; u < TERRAIN_NODE_TEXELS; ++u) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            // ⚠️ GEMELO de `terrain_node.comp`, y el RADIO importa: el detalle se evalúa con
            // `baseR = R + baseH`, igual que `clipmap.tese` y que `TerrestrialPlanet::sampleHeight`.
            // `terrainDetail` muestrea el ruido en `dir·radius`; 4 km de diferencia desplazan la
            // octava fina 880 unidades de ruido, o sea un relieve distinto. Sin `baseFn` esto es SOLO
            // detalle sobre R — un planeta sin continentes ni costa.
            // ⚠️ SIN `baseFn` NO SE ATENUA: `seaLevelAttenuation(0)` vale **0** y dejaría el nodo
            // PLANO. Lo cazó el hash golden. Con bake, la composición es la del clipmap.
            float h;
            if (baseFn) {
                const float baseH = baseFn(d, baseCtx);
                h = Haruka::Planet::terrainDetail(d, planetRadiusM + (double)baseH, triM)
                  * Haruka::Planet::seaLevelAttenuation(baseH);
                if (baseH > 0.0f && h < -baseH) h = -baseH;
                h += baseH;
            } else {
                h = Haruka::Planet::terrainDetail(d, planetRadiusM, triM);
            }
            out[(size_t)v * TERRAIN_NODE_TEXELS + u] = h;
            rg.minM = std::min(rg.minM, h);
            rg.maxM = std::max(rg.maxM, h);
        }
    // ⚠️ EL RANGO SALE GRATIS: el generador ya recorre los 16 641 téxeles. Y vale 106 MB — sin él,
    // el recorte de frustum en el campo cercano no puede descartar nada, porque la esfera envolvente
    // del nodo hay que inflarla ±5 km "por si acaso" (ver `nodeBoundingSphere`). Medido en
    // `terrain_node_frustum`: 2 842 nodos sin rango contra 1 164 con él.
    if (outRange) *outRange = rg;
}

/**
 * @brief Hash del contenido de un nodo. Es el TEST GOLDEN de F1.
 *
 * Sobre los BITS de cada float, no sobre su valor: un cambio de un ulp —por un `fma` contraído
 * distinto, por un driver, por un refactor "equivalente"— cambia el hash. Eso es exactamente lo que
 * se quiere detectar, porque un ulp de deriva entre cliente y servidor es una desincronización.
 *
 * Mismo mezclador entero que `detailHash`: envuelve módulo 2³² igual en C++ y en GLSL, así que el
 * día que el compute lo calcule en GPU los dos números son comparables.
 */
inline uint32_t nodeContentHash(const float* heights, size_t count) {
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < count; ++k) {
        uint32_t bits;
        std::memcpy(&bits, &heights[k], sizeof(bits));
        h = (h ^ bits) * 16777619u;
        h ^= h >> 13;
    }
    return h;
}

// =================================================================================================
// SELECCIÓN DE NODOS — qué nodos hay que tener residentes, por ERROR EN PANTALLA
// =================================================================================================
//
// F2 del plan. El criterio NO es la distancia: es cuántos PÍXELES de error comete un nodo si se
// dibuja a su resolución en vez de a la de su hijo. Eso desacopla el LOD del tamaño del mundo y lo
// ata a la pantalla, que es lo único que decide si algo se ve.
//
// ⚠️ Es la diferencia de fondo con el clipmap, que reparte por anillos de distancia fija: sus quads
// salen a 4,1 px constantes a >1 km pero a **20,6 px a 200 m** (el tope de tesela satura, medido).
// Con error en pantalla no hay tal saturación: un nodo se parte hasta cumplir el presupuesto.

/// Presupuesto de error, en píxeles: cuántos TÉXELES DE HEIGHTMAP hay por píxel de pantalla.
///
/// ⚠️ **NO ES LA DENSIDAD DE TRIÁNGULOS.** Esa la fija `vertexPx` (4) junto con el stride. Con este
/// valor en 1 el heightmap tenía un téxel por píxel mientras se dibujaba un vértice cada cuatro:
/// **16 téxeles por vértice**, o sea VRAM y nodos gastados en detalle que ninguna geometría podía
/// mostrar.
///
/// ⚠️ **ESTABA EN 1,0 Y ERA INSOSTENIBLE** desde que el pool publica rangos de verdad (antes salían
/// vacíos y el criterio medía al nivel del mar, subdividiendo de menos). Con el rango bueno la
/// demanda mide **6 811 nodos contra un presupuesto de 3 072** — hambre ×2,2 permanente. Y un
/// selector hambriento produce las dos cosas que se reportaron a la vez: vecinos con más de un nivel
/// de diferencia (**pinchos**, el geomorph solo cierra uno) y un reparto que cambia con el menor
/// movimiento de cámara (**parpadeo**).
///
/// Medido en `terrain_node_demand_with_range`:
///
///     errorPx   demanda   hambre   triángulos
///        1        6 811    2,2x      13,9 M
///        2        2 187    0,7x      17,9 M   <- cabe
///        3        1 160    0,4x      38,0 M
///
/// Cerca NO se pierde nada: a 2 m del suelo el nodo de nivel 17 comete 634 px de error, así que se
/// alcanza el nivel máximo con cualquier umbral razonable y la paridad con la colisión se mantiene.
/// Lo que se recorta es el campo medio. `HARUKA_TERRAIN_V5_ERRPX` lo cambia sin recompilar.
inline constexpr double TERRAIN_NODE_ERROR_PX = 2.0;

/**
 * @brief Radio (m) dentro del cual el render NO puede engrosar por encima del corte de la colision.
 *
 * Dentro de este radio el stride se fuerza a 1, y hasta el doble a 2 como maximo. Es lo que cierra la
 * disparidad ver-vs-pisar donde se ve; ver la nota larga en `nodeStrideWant`. 300 m sale de dividir
 * la disparidad por la distancia: a 300 m son 1,6 px de media, y mas alla ya no se distingue.
 *
 * ⚠️ 150 Y NO 300, Y LA DIFERENCIA LA DECIDIO EL BANCO (`terrain_stride_match_cost`, a altura de
 * ojo y con HARUKA_NO_VSYNC=1; sin eso Vulkan se clava en 16,6 ms y parece gratis):
 *
 *     radio     triangulos   OpenGL           Vulkan
 *       0 m       1,22 M     4,06 ms          7,66 ms      <- linea base
 *     150 m       1,65 M     5,35 (+1,3)      7,76 (+0,1)
 *     300 m       2,38 M     7,60 (+3,5)     10,77 (+3,1)
 *     600 m       2,93 M     9,23 (+5,2)     13,04 (+5,4)
 *
 * 150 m se lleva la banda donde la disparidad vale de 5,6 a 21 px por +0,1 ms en Vulkan. Subir a 300
 * cuesta 3 ms mas para bajar de 2,8 a 1,6 px de media: mal negocio. Y con el tramo graduado, 150
 * tambien deja 150-300 m en stride 2 como maximo, que ya era el peor punto (antes saltaba a 4).
 */
inline constexpr double TERRAIN_NODE_STRIDE_MATCH_M = 150.0;

/**
 * @brief ¿Hay que subdividir este nodo? Criterio de error en pantalla.
 *
 * El error geométrico de un nodo es del orden de su TÉXEL (lo que la rejilla no puede representar);
 * proyectado a `dist` metros de la cámara ocupa `texel/dist` radianes. Con `radPerPx` = FOV/altura,
 * el error en píxeles es `texel / (dist · radPerPx)`.
 *
 * `dist` es a la ESQUINA MÁS CERCANA del nodo, no a su centro: con el centro, un nodo grande a tus
 * pies (cuyo centro cae lejos) no se subdividiría y tendrías el suelo basto justo donde miras.
 */
/// Error en píxeles del nodo: su téxel proyectado a la distancia a la que de verdad está el terreno.
/// Es la métrica que decide subdividir y también cuánto morfea hacia el padre — una sola definición.
inline double nodeScreenError(const NodeId& n, double planetRadiusM,
                              const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                              double radPerPx, double nodeElevM = 0.0) {
    double best = 1e300;
    const double surfR = planetRadiusM + nodeElevM;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += TERRAIN_NODE_CELLS / 2)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += TERRAIN_NODE_CELLS / 2) {
            const glm::dvec3 p = planetCenter + nodeTexelDir(n, u, v) * surfR;
            best = std::min(best, glm::length(p - camPos));
        }
    const double dist = std::max(best, 1.0);
    return nodeTexelM(n, planetRadiusM) / (dist * radPerPx);
}

/**
 * @brief Téxeles por vértice de UN nodo, como índice de potencia de dos (0 → 1, 1 → 2, 2 → 4…).
 *
 * ── POR QUÉ NO PUEDE SER GLOBAL ─────────────────────────────────────────────────────────────────
 *
 * Aquí había un stride único para todo el frame, con este argumento: *"el selector ya iguala el
 * tamaño en pantalla de los nodos, así que todos quieren el mismo"*. Es cierto **para la pantalla**
 * y falso para lo que de verdad fallaba: el nodo bajo tus pies se dibujaba a `stride 4`, o sea un
 * vértice cada 2,386 m, mientras la colisión bajo esos mismos pies tiene celdas de 0,5 m. Medido:
 * el triángulo del render se separa **0,0727 m** del campo que el propio nodo guarda. Eso es la
 * disparidad ver↔pisar que quedaba, y es DIEZ VECES el twist (0,0076 m) que se estuvo puliendo
 * mientras este término ni se medía, porque se dio por hecho que el render dibujaba al téxel.
 *
 * ── LOS DOS TOPES ───────────────────────────────────────────────────────────────────────────────
 *
 *   · **PANTALLA** — `vertexPx / errorPx`, la regla de siempre. Cuántos téxeles caben en el
 *     tamaño de vértice que se quiere en píxeles.
 *   · **COLISIÓN** — la malla que se pisa NO tiene celda constante: escalona con la distancia
 *     igual que los anillos (`terrainRingLayout`), celda `fineCell·2^k` con medio alcance
 *     `fineCell·2^k·CELLS/2`. Despejando, a distancia `d` la celda que hay bajo esa `d` es
 *     `max(fineCell, d/(CELLS/2))`. El render no necesita ser más fino que eso: sería gastar
 *     triángulos en relieve que la física no tiene.
 *
 * Se toma el MENOR de los dos: los dos son cotas superiores del stride y hay que respetar ambas.
 * Lejos manda la pantalla (y sale el mismo 4 de antes); cerca manda la colisión y baja a 1.
 *
 * ⚠️ La transición entre strides SÍ mueve la superficie —es el mismo efecto medido arriba— y NO está
 * morfeada: el geomorph existente va entre NIVELES, no entre strides. O sea que es un pop real al
 * caminar. Medido en `test_terrain_node_stride_pop`, en píxeles y no en metros, porque 5 cm a 3 m se
 * ven y a 300 m no:
 *
 *     1→2 a  76 m: 0,0133 m = 0,185 px      2→4 a 153 m: 0,0354 m = 0,247 px
 *     4→8 a 305 m: 0,1877 m = 0,654 px  ← el peor, y sigue por debajo del píxel
 *
 * Cae dentro del píxel en las tres fronteras porque el brinco crece más despacio que la distancia a
 * la que ocurre. Si algún día se afina `TERRAIN_RING_FINE_CELL` o se ensancha el fov, eso deja de
 * ser verdad y hará falta morfear también entre strides — el test lo dirá antes que el ojo.
 *
 * @param collisionFineCellM celda del anillo más fino de la física (`TERRAIN_RING_FINE_CELL`).
 * @param maxIndex           mayor índice con index buffer construido.
 */
/**
 * @brief METROS de relieve que se pierden al dibujar con vano `spanM` un dato horneado a `texelM`.
 *
 * ── QUE ES ESTE NUMERO Y POR QUE FALTABA ────────────────────────────────────────────────────────
 *
 * El stride dibuja triangulos entre texeles NO CONSECUTIVOS: el dato tiene detalle hasta `texelM`
 * pero la malla solo puede describir hasta `spanM = stride x texelM`. Lo que hay entre esas dos
 * longitudes de onda existe en el heightmap y no se dibuja — es el error de cuerda, en metros.
 *
 * Y se puede calcular EXACTO sin muestrear nada, porque la tabla de octavas de `terrain_detail` es
 * explicita: amplitudes 969,3 / 513,4 / 260 / 70 / 14 / 3 / 0,7 con sus longitudes de onda, y un peso
 * `octaveWeight` que dice cuanto entra cada una a un corte dado. Lo que se pierde es, octava a
 * octava, lo que el dato SI tiene y la malla NO:
 *
 *     perdido = sum_i  0,5 x A_i x [ w(lambda_i, texelM) - w(lambda_i, spanM) ]
 *
 * El 0,5 es porque `detailNoise` va de 0 a 1 y entra como `(n - 0,5)`, o sea semi-amplitud.
 *
 * ⚠️ GEMELO DE LA TABLA DE `terrain_detail.h`. Si alli cambia una amplitud o una longitud de onda,
 * aqui tambien: si divergen, el LOD decide con un error que el terreno no tiene.
 */
inline double nodeMissingReliefM(double texelM, double spanM) {
    // (longitud de onda, amplitud) — el MISMO orden y las MISMAS cifras que `terrainDetail`.
    static constexpr double kOct[7][2] = {
        { 12000.0, 969.3 }, { 6000.0, 513.4 }, { 2857.0, 260.0 },
        {   625.0,  70.0 }, {  111.0,  14.0 }, {   22.0,   3.0 }, { 4.5, 0.7 },
    };
    auto w = [](double lambdaM, double minFeatureM) {
        const double t = lambdaM / std::max(minFeatureM * 2.0, 1e-3);
        return std::min(std::max(t - 1.0, 0.0), 1.0);
    };
    double lost = 0.0;
    for (const auto& o : kOct) lost += 0.5 * o[1] * std::max(w(o[0], texelM) - w(o[0], spanM), 0.0);
    return lost;
}

inline double nodeStrideWant(const NodeId& n, double planetRadiusM,
                             const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                             double errorPx, double vertexPx,
                             double collisionFineCellM, double nodeElevM = 0.0,
                             double strideMatchM = TERRAIN_NODE_STRIDE_MATCH_M,
                             double nodeReliefM = -1.0, double nodeReliefRefM = 0.0,
                             double radPerPx = 0.0) {
    // ── EL PRESUPUESTO DE VERTICES SE REPARTE POR DISTANCIA Y NADA MAS ──────────────────────────
    //
    // `screenWant` es `vertexPx / errorPx`: una CONSTANTE, igual para todos los nodos. Asi que un
    // prado liso recibe exactamente la misma densidad de vertices que una ladera rota, aunque en el
    // prado no haya nada que describir y en la ladera el diezmado se vea.
    //
    // El error que deja el stride es de CUERDA: la superficie dibujada corta entre muestras que si
    // existen, y lo que se pierde depende de cuanto sube y baja el terreno dentro de ese vano — no
    // de lo lejos que esta. Por eso gastar por distancia reparte mal: paga igual donde no hace falta.
    //
    // ⚠️ SE INTENTO REPARTIR POR RELIEVE Y NO SALE NEUTRO. MEDIDO, NO SUPUESTO.
    //
    // La idea: un prado liso no necesita tantos vertices como una ladera rota, asi que mover el
    // presupuesto del uno al otro daria menos error de cuerda por el MISMO numero de triangulos.
    // Con `nodeReliefM` y una referencia (`nodeReliefRefM`) el nodo liso pide stride mas grueso y el
    // escarpado mas fino. Suena bien y no funciona, por el `min(screenWant, collWant)` de abajo:
    //
    //   · Afinar SI pasa: `screenWant` baja y el min lo deja pasar.
    //   · Bastecer NO: `screenWant` sube, pero `collWant` lo tapa y el nodo se queda igual.
    //
    // O sea que la mitad que devuelve presupuesto no devuelve nada y solo queda la que gasta.
    // Medido en la misma escena (2,5 M de triangulos con el reparto de siempre):
    //
    //     referencia CONSTANTE (8 x texel)   -> 8,6 M   (3,4x: casi todos los nodos la superan)
    //     media ARITMETICA de la escena      -> 3,3 M
    //     media GEOMETRICA de la escena      -> 4,3 M   (la correcta para un stride en potencias de 2)
    //
    // Ninguna es neutra. Para que lo fuera habria que repartir DESPUES del min, o sea sobre el stride
    // ya decidido, y eso es otra estructura. Queda detras de `HARUKA_TERRAIN_V5_RELIEF`, APAGADO por
    // defecto, para que el numero este a mano si alguien retoma la idea por el lado correcto.
    double screenWant = vertexPx / std::max(errorPx, 1e-3);

    // ── CRITERIO POR ERROR, NO POR DENSIDAD (`radPerPx > 0`) ────────────────────────────────────
    //
    // ⚠️ `vertexPx / errorPx` es una CONSTANTE: fija cuantos vertices por pixel y punto. Eso reparte
    // el presupuesto sin mirar lo que hay que describir — el mismo gasto en un prado liso que en un
    // cantil. Un LOD de quadtree no se decide por densidad sino por ERROR: se engrosa mientras lo que
    // se pierde siga por debajo de un umbral EN PANTALLA.
    //
    // Aqui el error se sabe en metros sin muestrear (`nodeMissingReliefM`) y se proyecta dividiendo
    // por `distancia x radianes por pixel`, que es la misma conversion que usa `nodeScreenError`. Se
    // busca el vano MAS GRUESO que aun cabe en `errorPx`, o sea el maximo ahorro admisible.
    //
    // Lo que cambia frente a la constante: donde las octavas que se pierden son pequenas —terreno
    // suave, o vanos por debajo de la octava mas fina— el stride puede crecer y se ahorran
    // triangulos; donde son grandes, se frena aunque la densidad "sobrara". Mismo umbral de calidad,
    // menos gasto donde no aporta.
    if (radPerPx > 0.0) {
        const double texelM = nodeTexelM(n, planetRadiusM);
        double dist = 1e300;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += TERRAIN_NODE_CELLS / 2)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += TERRAIN_NODE_CELLS / 2)
                dist = std::min(dist, glm::length(planetCenter
                       + nodeTexelDir(n, u, v) * (planetRadiusM + nodeElevM) - camPos));
        dist = std::max(dist, 1.0);
        const double mPerPx = dist * radPerPx;          // metros que mide un pixel a esa distancia
        const double budgetM = errorPx * mPerPx;        // lo que se puede perder sin que se note
        double allowed = 1.0;
        for (double s = 2.0; s <= 64.0; s *= 2.0) {
            if (nodeMissingReliefM(texelM, texelM * s) > budgetM) break;
            allowed = s;
        }
        screenWant = std::max(screenWant, allowed);
    }
    if (nodeReliefM >= 0.0 && nodeReliefRefM > 0.0) {
        // Relieve de referencia: el que un nodo "normal" tiene sobre su propio vano de texel. Por
        // debajo se puede diezmar mas; por encima hay que afinar. La raiz es porque el error de
        // cuerda va con el CUADRADO del vano: para doblar el error admisible basta 1,41x de vano.
        // ⚠️ LA REFERENCIA ES LA MEDIA DE LA ESCENA, NO UNA CONSTANTE. El primer intento uso un
        // numero fijo (`8 x texel`) y NO era neutro: en este terreno casi todos los nodos lo superan,
        // asi que todos se afinaban y los triangulos pasaban de 2,8 a 8,6 M — o sea "gastar 3x", que
        // es exactamente lo que esto venia a NO hacer. Con la media del conjunto dibujado, la mitad
        // de los nodos sube y la otra baja: el total se conserva y solo cambia el REPARTO.
        // El llamante la pasa en `nodeReliefRefM`; con <= 0 no se aplica nada.
        const double relief = std::max(nodeReliefM, 1e-3);
        const double f      = std::sqrt(std::max(nodeReliefRefM, 1e-3) / relief);
        screenWant *= std::min(std::max(f, 0.5), 2.0);   // ni un cuarto ni cuatro veces: x0,5 a x2
    }

    const double surfR = planetRadiusM + nodeElevM;
    double nearest = 1e300;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += TERRAIN_NODE_CELLS / 2)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += TERRAIN_NODE_CELLS / 2)
            nearest = std::min(nearest,
                               glm::length(planetCenter + nodeTexelDir(n, u, v) * surfR - camPos));
    const double cellM    = std::max(collisionFineCellM,
                                     nearest / (double)(TERRAIN_NODE_CELLS / 2));
    const double collWant = cellM / std::max(nodeTexelM(n, planetRadiusM), 1e-9);

    // ── EL TOPE DE CERCA: el render no puede engrosar por encima de lo que corta la COLISION ─────
    //
    // ⚠️ ESTA ES LA DISPARIDAD VER-vs-PISAR, Y ESTABA AQUI. Con stride >= 2 el vertice lee la altura
    // del ANCESTRO (`skMap` en `terrain_node.vert`), asi que el render dibuja una superficie con
    // MENOS OCTAVAS mientras la colision muestrea el campo fino. Medido con la sonda
    // `terrain_node_drawn_vs_field` (dibujado real contra `harukaTerrainDetail` en el pixel):
    //
    //     stride 1 (mapa del nodo)     ver-vs-pisar  0,111 m de media · 0,433 m el peor
    //     stride 2 (mapa del padre)                  0,409 m          · 1,556 m
    //     stride 4 (mapa del abuelo)                 0,475 m          · 1,816 m
    //
    // Y coincide al 2% con la diferencia de octavas entre los dos cortes, o sea que no es colocacion
    // ni precision: son dos superficies distintas por construccion.
    //
    // ── POR QUE UN RADIO Y NO IGUALAR LAS DOS LEYES ─────────────────────────────────────────────
    //
    // Igualarlas en todo el rango son **10,6x mas triangulos** (modelo por bandas con este mismo
    // selector): sobre los 2,0 M medidos serian ~21 M, y VERTPX=4 ya costaba 29,8 ms con 6,9 M.
    // Descartado por coste. Pero la disparidad solo SE VE de cerca — hay que dividir por la
    // distancia, que es lo que faltó la vez del geomorph. A 60 grados y 1080p:
    //
    //     disparidad de 0,41 m ->  5,6 px a 76 m ·  2,8 px a 150 m ·  1,6 px a 300 m ·  0,8 px a 600 m
    //     el peor de 1,56 m    ->   21 px        ·   11 px         ·   6,3 px        ·   3,1 px
    //
    // Se corrige la banda donde se ve (que ademas casi no aporta triangulos: 76-300 m es +0,20 M en
    // el modelo) y se deja la lejana, que es la cara (15,8x) y ya es sub-pixel de media.
    //
    // ⚠️ Y VA GRADUADO, NO DE GOLPE. Saltar de stride 1 a 4 en el borde del radio pondria vecinos con
    // zancada 1 y 4 a la vez: el cosido lo aguanta (`max(mio, vecino << niveles)`) pero el escalon es
    // el mayor posible. Con el tramo intermedio la secuencia es 1 -> 2 -> 4, monotona.
    double want = std::min(screenWant, collWant);
    if      (nearest <=       strideMatchM) want = std::min(want, 1.0);
    else if (nearest <= 2.0 * strideMatchM) want = std::min(want, 2.0);
    return want;
}

/**
 * @brief El stride, ya cuantizado a potencia de dos. `prevIndex` da HISTERESIS.
 *
 * ── POR QUÉ HACE FALTA LA HISTÉRESIS, Y QUÉ PASA SIN ELLA ───────────────────────────────────────
 *
 * Sin `prevIndex` esto es una función pura de la distancia, y ahí está el fallo: un nodo parado
 * JUSTO en un umbral cambia de stride en cuanto la cámara se mueve un milímetro. Medido en
 * `terrain_node_walk_shimmer` con el micro-temblor de cualquier controlador de personaje (±1 cm):
 * **119 cambios en 120 frames**, y cada uno mueve la superficie ~1 cm. Eso es exactamente el
 * "el terreno tiembla al mover el personaje" que se reportó.
 *
 * ⚠️ Un salto aislado al cruzar un umbral es un POP y ya está medido como sub-píxel. Lo que se ve no
 * es el salto: es que se repita ida y vuelta cada frame. La cota en píxeles del pop NO dice nada
 * sobre el parpadeo, y confundir las dos cosas fue lo que dejó esto pasar.
 *
 * La banda muerta es del 25 %: para cambiar de stride hay que pasarse del umbral un cuarto, no
 * rozarlo. El temblor de 1 cm a 110 m es un 0,01 % — tres órdenes de magnitud por debajo.
 *
 * @param prevIndex el stride que este nodo tuvo el frame pasado, o `kNoPrevStride` si es nuevo.
 */
inline constexpr uint32_t kNoPrevStride = 0xFFFFFFFFu;

inline uint32_t nodeStrideQuantise(double want, uint32_t maxIndex,
                                   uint32_t prevIndex = kNoPrevStride) {
    uint32_t sk = 0;
    while (sk + 1 <= maxIndex && (double)(1u << (sk + 1)) <= want) ++sk;
    if (prevIndex == kNoPrevStride || prevIndex > maxIndex || sk == prevIndex) return sk;

    const double kBand = 0.25;
    if (sk > prevIndex) {                     // quiere ENGROSAR: exigir pasarse del umbral
        if (want < (double)(1u << (prevIndex + 1)) * (1.0 + kBand)) return prevIndex;
    } else {                                  // quiere AFINAR: idem por el otro lado
        if (want > (double)(1u << prevIndex) / (1.0 + kBand)) return prevIndex;
    }
    return sk;
}

/** @brief Atajo sin histeria ni historia: para tests y para el primer frame de un nodo. */
inline uint32_t nodeStrideIndex(const NodeId& n, double planetRadiusM,
                                const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                                double errorPx, double vertexPx,
                                double collisionFineCellM, uint32_t maxIndex,
                                double nodeElevM = 0.0, uint32_t prevIndex = kNoPrevStride) {
    return nodeStrideQuantise(nodeStrideWant(n, planetRadiusM, camPos, planetCenter, errorPx,
                                             vertexPx, collisionFineCellM, nodeElevM),
                              maxIndex, prevIndex);
}

inline bool nodeShouldSplit(const NodeId& n, double planetRadiusM,
                            const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                            double radPerPx, double errorPx = TERRAIN_NODE_ERROR_PX,
                            double nodeElevM = 0.0) {
    if (n.level >= TERRAIN_NODE_MAX_LEVEL) return false;
    // ⚠️ LA DISTANCIA VA AL TERRENO, NO A LA ESFERA DE REFERENCIA.
    //
    // Esto medía a `planetCenter + dir·R`, o sea al nivel del mar, ignorando el relieve. En una
    // ladera de +2 km el suelo real está 2 km MÁS CERCA de una cámara por encima, así que el criterio
    // sobreestimaba la distancia y **subdividía de menos**: terreno basto justo donde más se mira.
    //
    // Y el error depende de la elevación LOCAL, así que el síntoma es direccional — mirando cuesta
    // arriba salía disparidad y cuesta abajo no. Reportado así, y es lo que lo explica: no era el
    // ángulo, era la altura del terreno que hay en esa dirección.
    //
    // Se usa la cota MÁXIMA del nodo porque un pico es lo más cercano a una cámara que está arriba;
    // sin rango conocido vale 0 y esto es exactamente lo que había.
    return nodeScreenError(n, planetRadiusM, camPos, planetCenter, radPerPx, nodeElevM) > errorPx;
}

/**
 * @brief Cuánto MORFEA este nodo hacia su padre: 0 recién nacido, 1 justo antes de fundirse en él.
 *
 * ⚠️ ESTO ES LO QUE QUITA EL POPPING AL MOVER LA CÁMARA, y es distinto del cosido entre vecinos.
 *
 * El geomorph de aristas cierra el escalón entre dos nodos que se dibujan A LA VEZ. Pero cuando te
 * alejas y un nodo se funde en su padre, la superficie SALTA de una función a otra — reportado como
 * "sobre todo al mover la cámara". Aquí se cierra en el TIEMPO: el nodo va adoptando la altura de su
 * padre conforme se acerca el relevo, así que cuando ocurre ya son idénticos y no hay salto.
 *
 * El umbral sale de la propia regla de subdivisión, no de un número elegido: un nodo es hoja mientras
 * su error <= `errorPx`, y su padre —cuyo téxel es el doble, o sea error doble— pasa a serlo cuando
 * el del nodo baja a `errorPx/2`. Ésa es exactamente la ventana del morph.
 */
inline float nodeParentMorph(const NodeId& n, double planetRadiusM,
                             const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                             double radPerPx, double errorPx = TERRAIN_NODE_ERROR_PX,
                             double nodeElevM = 0.0) {
    if (n.level == 0) return 0.0f;                     // una raíz no tiene padre en el que fundirse
    const double e = nodeScreenError(n, planetRadiusM, camPos, planetCenter, radPerPx, nodeElevM);
    const double t = 2.0 * (errorPx - e) / std::max(errorPx, 1e-9);
    return (float)glm::clamp(t, 0.0, 1.0);
}

/**
 * @brief Morph hacia el padre de UN VÉRTICE. Gemelo exacto de `terrain_node.vert`.
 *
 * ── POR QUÉ NO PUEDE SER POR NODO ───────────────────────────────────────────────────────────────
 *
 * `nodeParentMorph` (arriba) devuelve un escalar para todo el nodo, y esa es justo la razón de que
 * el pase v5 no pudiera casar en las aristas: dos vecinos a distancias distintas evalúan el MISMO
 * punto 3D de la arista compartida como `mix(propia, padre, morphA)` por un lado y `morphB` por el
 * otro. La grieta es `|morphA − morphB| · (padre − propia)`, y medida sobre un frame real daba
 * **10,07 m** con 1175 de 1327 parejas adyacentes discrepando.
 *
 * Aquí la entrada es la DIRECCIÓN del vértice, que las dos caras de una arista compartida calculan
 * bit a bit igual (ésa es la propiedad de la retícula, `terrain_node_lattice`). Mismo nivel y misma
 * dirección → mismo morph, así que sobre una arista compartida la grieta no puede existir.
 *
 * ⚠️ El radio es el BASE, sin relieve: el relieve es lo que se está mezclando, y además cada lado
 * tiene su propio mapa de alturas — usarlo rompería justo la entrada que los dos comparten.
 */
inline float nodeVertexMorph(uint32_t level, const glm::dvec3& texelDir, double planetRadiusM,
                             const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                             double radPerPx, double errorPx = TERRAIN_NODE_ERROR_PX) {
    if (level == 0) return 0.0f;                        // una raíz no tiene padre en el que fundirse
    const glm::dvec3 p = planetCenter + texelDir * planetRadiusM;
    const double dist = std::max(glm::length(p - camPos), 1.0);
    const double texM = (planetRadiusM * 1.5707963267948966 / (double)(1ull << level))
                      / (double)TERRAIN_NODE_CELLS;     // gemelo de `nodeTexelM` para ese nivel
    const double e = texM / (dist * radPerPx);
    return (float)glm::clamp(2.0 * (errorPx - e) / std::max(errorPx, 1e-9), 0.0, 1.0);
}

/**
 * @brief ¿Está este nodo ENTERAMENTE bajo el horizonte? Entonces no se ve y no se selecciona.
 *
 * ⚠️ SIN ESTO EL SELECTOR NO SIRVE, y está medido: sin recorte de horizonte, una cámara a 2 m de
 * altura gastaba las 4 095 plazas del presupuesto repartidas por TODA la esfera —el nodo más lejano
 * a 12 734 km, o sea el diámetro— y el más fino que llegaba a colocar bajo los pies caía a **9,7 km**.
 * El campo cercano se quedaba sin nodos porque la cara oculta se los comía.
 *
 * Es el principio 2 de `PLAN_LOD_V3`: *"el coste alto NO es el detalle visible — es renderizar lo
 * OCULTO. La oclusión es lo que acota el coste."*
 *
 * El criterio es exacto para una esfera: desde una cámara a distancia `D` del centro, un punto de la
 * superficie es visible sii `dot(P̂, Ĉ) > R/D`. Se prueban las cuatro esquinas y el centro; si
 * NINGUNO pasa, el nodo entero está detrás del horizonte.
 *
 * ⚠️ CONSERVADOR A PROPÓSITO: con muestrear cinco puntos puede sobrevivir algún nodo que en realidad
 * no se ve (se dibuja de más, que es barato) pero no puede descartarse uno que sí — que sería un
 * agujero en el suelo. Nunca al revés.
 */
inline bool nodeBelowHorizon(const NodeId& n, double planetRadiusM,
                             const glm::dvec3& camPos, const glm::dvec3& planetCenter) {
    const glm::dvec3 rel = camPos - planetCenter;
    const double D = glm::length(rel);
    if (D <= planetRadiusM) return false;          // cámara dentro del planeta: no se recorta nada
    const glm::dvec3 camHat = rel / D;
    const double cosHorizon = planetRadiusM / D;

    // ── REGLA 1: ¿está el punto BAJO LA CÁMARA dentro de este nodo? Entonces se ve, y punto. ─────
    //
    // ⚠️ SIN ESTA REGLA EL RECORTE DESCARTA EL SUELO QUE PISAS, y no es una hipótesis: la primera
    // versión probaba cinco puntos (esquinas + centro) y a 2 m de altura devolvía **0 nodos**. A esa
    // altura el horizonte está a 0,045°, así que en un nodo de nivel 0 —que abarca 90°— las cinco
    // sondas caen fuera aunque la zona visible esté justo en su INTERIOR.
    //
    // La región visible es un disco alrededor del punto sub-cámara. Si ese disco no toca el borde
    // del nodo, está entero dentro; y entonces la única sonda que puede verlo es el propio punto
    // sub-cámara. Por eso esta regla no es una optimización: es el caso que las sondas no cubren.
    PlanetFace camFace; double camLx, camLy;
    dirToCubeFaceClosed(camHat, camFace, camLx, camLy);
    if (camFace == n.face) {
        double lx0, ly0, lx1, ly1;
        nodeTexelFaceCoord(n, 0, 0, lx0, ly0);
        nodeTexelFaceCoord(n, TERRAIN_NODE_CELLS, TERRAIN_NODE_CELLS, lx1, ly1);
        if (camLx >= lx0 && camLx <= lx1 && camLy >= ly0 && camLy <= ly1) return false;
    }

    // ── REGLA 2: sondas por el BORDE. Si el disco visible no contiene al punto sub-cámara, tiene
    // que cruzar el borde del nodo para tocarlo, así que muestrear el perímetro lo detecta.
    //
    // ⚠️ CONSERVADOR A PROPÓSITO: puede sobrevivir un nodo que no se ve (se dibuja de más, barato)
    // pero no puede descartarse uno que sí — eso sería un agujero en el suelo. Nunca al revés.
    constexpr uint32_t kProbes = 8;                       // por lado del perímetro
    const uint32_t step = TERRAIN_NODE_CELLS / kProbes;
    for (uint32_t t = 0; t <= TERRAIN_NODE_CELLS; t += step) {
        const uint32_t E = TERRAIN_NODE_CELLS;
        if (glm::dot(nodeTexelDir(n, t, 0), camHat) > cosHorizon) return false;
        if (glm::dot(nodeTexelDir(n, t, E), camHat) > cosHorizon) return false;
        if (glm::dot(nodeTexelDir(n, 0, t), camHat) > cosHorizon) return false;
        if (glm::dot(nodeTexelDir(n, E, t), camHat) > cosHorizon) return false;
    }
    return true;
}

/**
 * @brief Esfera envolvente del nodo, en coordenadas de mundo.
 *
 * Se usa para recortar contra el frustum. Con una esfera el test es exacto y CONSERVADOR para nodos
 * de cualquier tamaño — a diferencia de muestrear puntos, que en un nodo grande puede no tocar la
 * parte visible (el fallo que ya dio el recorte de horizonte: 0 nodos a 2 m de altura).
 *
 * ⚠️ `terrainBoundM` infla el radio para cubrir el relieve, que no está en la esfera de referencia.
 * Por defecto 5 000 m: la escalera de ruido suma ±348 m de amplitud y el bake llega a ±4 km. Es
 * CONSERVADOR a propósito — inflar de más recorta de menos (se dibuja algo invisible, barato) y
 * nunca al revés (un agujero en el suelo). Se puede apretar cuando cada nodo lleve su min/max real,
 * que es información que el generador ya podría emitir.
 */
inline void nodeBoundingSphere(const NodeId& n, double planetRadiusM,
                               const glm::dvec3& planetCenter, double terrainBoundM,
                               glm::dvec3& outCenter, double& outRadius) {
    const uint32_t H = TERRAIN_NODE_CELLS / 2, E = TERRAIN_NODE_CELLS;
    outCenter = planetCenter + nodeTexelDir(n, H, H) * planetRadiusM;
    double r = 0.0;
    const uint32_t corner[4][2] = { {0,0}, {E,0}, {0,E}, {E,E} };
    for (const auto& c : corner)
        r = std::max(r, glm::length(planetCenter + nodeTexelDir(n, c[0], c[1]) * planetRadiusM - outCenter));
    outRadius = r + terrainBoundM;
}

/**
 * @brief ¿Está el nodo ENTERAMENTE fuera del cono de visión? Entonces no se dibuja.
 *
 * ⚠️ ES LO QUE HACE VIABLE EL PRESUPUESTO DE MEMORIA. Sin él, el selector elige los 360° alrededor
 * de la cámara: medido a 2 m de altura son **5 439 nodos**, y a 16 641 téxeles × 4 B por nodo eso son
 * **362 MB** residentes. Con frustum solo queda lo que de verdad se mira.
 *
 * Se recorta contra un CONO que envuelve el frustum, no contra sus seis planos. Es más simple, es
 * conservador (el cono contiene al frustum, así que nunca descarta algo visible) y para un quadtree
 * planetario la diferencia es despreciable: lo que decide es la cara oculta y la espalda, no las
 * esquinas del rectángulo. El semiángulo que envuelve un frustum de FOV vertical `fovY` y relación
 * de aspecto `a` es `atan(tan(fovY/2)·√(1+a²))`.
 *
 * El test es esfera-cono, exacto: el nodo se descarta si el ángulo entre su centro y el eje de
 * visión, MENOS su radio angular, supera el semiángulo del cono.
 */
inline double nodeFrustumConeHalfAngle(double fovYRad, double aspect) {
    const double t = std::tan(fovYRad * 0.5);
    return std::atan(t * std::sqrt(1.0 + aspect * aspect));
}

inline bool nodeOutsideFrustum(const NodeId& n, double planetRadiusM,
                               const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                               const glm::dvec3& viewDir, double coneHalfAngle,
                               double terrainBoundM) {
    glm::dvec3 sc; double sr;
    nodeBoundingSphere(n, planetRadiusM, planetCenter, terrainBoundM, sc, sr);
    const glm::dvec3 to = sc - camPos;
    const double d = glm::length(to);
    if (d <= sr) return false;                       // la cámara está DENTRO de la envolvente: se ve
    const double cosA = glm::clamp(glm::dot(to / d, viewDir), -1.0, 1.0);
    const double angle = std::acos(cosA);
    const double angRadius = std::asin(glm::clamp(sr / d, 0.0, 1.0));
    return (angle - angRadius) > coneHalfAngle;
}

/**
 * @brief Conjunto de nodos a tener residentes: las HOJAS del quadtree bajo el criterio de error.
 *
 * Recorre las seis caras desde el nivel 0 y baja donde `nodeShouldSplit` lo pida. El resultado
 * TESELA la esfera sin huecos ni solapes por construcción: cada nodo o se emite o se sustituye por
 * sus cuatro hijos, nunca las dos cosas.
 *
 * `maxNodes` es un tope duro. No es cosmético: sin él, un error en el criterio (una cámara dentro
 * del planeta, un `radPerPx` a cero) subdividiría hasta agotar la memoria antes de que nadie viera
 * un frame. Al alcanzarlo se deja de partir y se emiten los nodos tal cual — degrada la imagen, no
 * el proceso.
 */
/**
 * @brief De dónde saca el selector la cota de un nodo que aún NO se ha generado.
 *
 * Es la pieza que rompe el círculo: para decidir si un nodo entra hay que acotar su cota, y para
 * conocer su cota hay que generarlo. La salida es que el hijo HEREDA el rango de su padre —el área
 * del hijo está contenida en la del padre, así que su rango también— hasta que se genere y publique
 * el suyo, más ajustado.
 *
 * El pool implementa esto consultando lo que ya tiene residente. Devolver un rango inválido significa
 * "no lo sé", y entonces se usa el bound conservador de ±5 km.
 */
using NodeRangeFn = NodeRange (*)(const NodeId&, void* user);

inline void nodeSelectVisible(double planetRadiusM, const glm::dvec3& camPos,
                              const glm::dvec3& planetCenter, double radPerPx,
                              std::vector<NodeId>& out, size_t maxNodes = 4096,
                              double errorPx = TERRAIN_NODE_ERROR_PX,
                              const glm::dvec3* viewDir = nullptr,
                              double coneHalfAngle = 0.0, double terrainBoundM = 5000.0,
                              NodeRangeFn rangeFn = nullptr, void* rangeUser = nullptr,
                              size_t* outCulledHorizon = nullptr,
                              size_t* outCulledFrustum = nullptr) {
    out.clear();
    // ── EL PRESUPUESTO VA AL QUE MAS ERROR TIENE, NO AL QUE EL RECORRIDO PILLE ANTES ─────────────
    //
    // Esto era una PILA (LIFO) sembrada con las seis caras del cubo. Mientras sobra presupuesto da
    // igual el orden: todo el que quiere dividirse se divide. En cuanto `room` se agota deja de dar
    // igual, porque el que se queda sin dividir se dibuja BASTO — y quien se quedaba basto lo
    // decidia el orden de recorrido, no la necesidad.
    //
    // Medido en `terrain_node_budget_starvation`, apretando el tope: nodos bastos a 485 m mientras
    // se dibujaba fino a 1405 m. Y como el orden efectivo cambia con lo que recorta el cono, el
    // reparto cambiaba AL GIRAR LA CAMARA — que es como se reporto el sintoma.
    //
    // Con un monticulo por error en pantalla, al agotarse el presupuesto lo que queda en la cola es
    // por construccion lo de MENOS error, y eso es lo que se emite basto. La degradacion deja de ser
    // arbitraria y pasa a ser la correcta.
    //
    // ⚠️ EL DESEMPATE VA POR CLAVE, no "da igual": dos nodos con el mismo error tienen que salir
    // siempre en el mismo orden o el selector deja de ser determinista, que es una propiedad
    // declarada de este fichero (mismos enteros, mismos bits, en cualquier maquina).
    // ⚠️ DOS PASADAS, Y LA CARA SOLO CUANDO HACE FALTA. Ordenar por error cuesta **x1,56** medido en
    // -O2 (2,12 -> 3,32 ms), o sea +1,2 ms de frame, y NO sirve de nada mientras sobre presupuesto:
    // si todo el que quiere dividirse puede, el corte final es el mismo salga en el orden que salga
    // (comprobado: mismo conjunto y mismo peor error a 2000 y 2500 nodos).
    //
    // Asi que primero va la pila barata. Si NUNCA se quedo sin sitio, su resultado ya es el optimo y
    // se devuelve tal cual. Solo si se agoto el presupuesto se repite ordenando por error — que son
    // los frames donde importa quien se queda basto.
    struct Cand { double err; uint64_t key; NodeId n; };
    const auto worse = [](const Cand& a, const Cand& b) {
        return (a.err != b.err) ? (a.err < b.err) : (a.key > b.key);
    };
    std::vector<Cand> stack;
    bool byError = false, starved = false;
    // Clave local: `nodeKey` vive en el pool y depender de el aqui invertiria la direccion del
    // include. Solo hace falta que sea inyectiva y estable, no que coincida con la del pool.
    const auto keyOf = [](const NodeId& n) {
        return ((uint64_t)n.face << 58) | ((uint64_t)n.level << 52)
             | ((uint64_t)n.i << 26) | (uint64_t)n.j;
    };
    const auto errOf = [&](const NodeId& n) {
        double elevM = 0.0;
        if (rangeFn) { const NodeRange r = rangeFn(n, rangeUser); if (r.valid()) elevM = r.maxM; }
        return nodeScreenError(n, planetRadiusM, camPos, planetCenter, radPerPx, elevM);
    };
    // ⚠️ RECORTAR AL METER, NO AL SACAR. La pila descartaba al sacar, que con una pila da igual —
    // meter es gratis—. Con un monticulo NO: meter cuesta un `push_heap` y, sobre todo, calcular el
    // error en pantalla (nueve direcciones). Hacerlo para nodos que el horizonte va a tirar salia
    // **x1,51** frente a la pila. Recortando antes de meter, ese trabajo no se hace nunca.
    // ⚠️ LOS DESCARTES SE CUENTAN AQUI DENTRO, y no re-ejecutando el selector fuera.
    //
    // El renderer los sacaba llamando DOS VECES MAS a `nodeSelectVisible` y restando tamaños... con
    // los MISMOS argumentos en las dos llamadas. O sea que la resta era siempre 0 y el log decia
    // `horizonte 0` en todos los frames por construccion, atribuyendole al cono todo lo que
    // descartaban los dos. Ademas costaba dos pasadas enteras del selector por frame (~4,5 ms en
    // -O2) para producir una constante.
    const auto admit = [&](const NodeId& n) {
        // HORIZONTE PRIMERO: descartar lo que no se ve antes de gastarle presupuesto. Ver
        // `nodeBelowHorizon` — sin esto la cara oculta se come las plazas del campo cercano.
        if (nodeBelowHorizon(n, planetRadiusM, camPos, planetCenter)) {
            if (outCulledHorizon) ++*outCulledHorizon;
            return;
        }
        // FRUSTUM después del horizonte: los dos descartan, pero el de horizonte es más barato
        // (sin acos ni asin) y se lleva por delante media esfera antes de que el otro mire nada.
        if (viewDir) {
            // Cota del nodo: la que sepa el pool, y si no la sabe el bound conservador.
            double bound = terrainBoundM;
            if (rangeFn) { const NodeRange r = rangeFn(n, rangeUser); if (r.valid()) bound = r.boundM(); }
            if (nodeOutsideFrustum(n, planetRadiusM, camPos, planetCenter,
                                   *viewDir, coneHalfAngle, bound)) {
                if (outCulledFrustum) ++*outCulledFrustum;
                return;
            }
        }
        stack.push_back(Cand{ errOf(n), keyOf(n), n });
        if (byError) std::push_heap(stack.begin(), stack.end(), worse);
    };
    for (int pass = 0; pass < 2; ++pass) {
    byError = (pass == 1);
    out.clear(); stack.clear(); starved = false;
    // La segunda pasada rehace el recorrido entero: los contadores se reinician o saldrian dobles.
    if (outCulledHorizon) *outCulledHorizon = 0;
    if (outCulledFrustum) *outCulledFrustum = 0;
    for (int f = 0; f < 6; ++f) admit(NodeId{ (PlanetFace)f, 0, 0, 0 });
    while (!stack.empty()) {
        if (byError) std::pop_heap(stack.begin(), stack.end(), worse);
        const Cand cand = stack.back(); stack.pop_back();
        const NodeId n = cand.n;
        // La cota del nodo, si el pool la sabe: decide la DISTANCIA real al terreno (ver la nota de
        // `nodeShouldSplit`). Se resuelve una vez y sirve para el recorte y para la subdivisión.
        // El error ya viaja con el candidato: se calculo al meterlo, con la misma cota del pool que
        // usaria `nodeShouldSplit`. No se recalcula.
        const bool room = (out.size() + stack.size() + 4) <= maxNodes;
        const bool wants = (n.level < TERRAIN_NODE_MAX_LEVEL && cand.err > errorPx);
        if (!room && wants) starved = true;   // alguien se queda basto: el orden pasa a importar
        if (room && wants) {
            NodeId kids[4]; nodeChildren(n, kids);
            for (const NodeId& k : kids) admit(k);
        } else {
            out.push_back(n);
        }
    }
    if (!starved) break;      // sobro presupuesto: este corte ya es el optimo, no hay que reordenar
    }
}

}} // namespace Haruka::Terrain
