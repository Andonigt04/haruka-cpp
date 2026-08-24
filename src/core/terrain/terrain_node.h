/**
 * @file terrain_node.h
 * @brief DIRECCIONAMIENTO DE NODOS del quadtree de terreno (v5). Enteros dentro, dirección fuera.
 *
 * Es la pieza F1 de `docs/guides/PLAN_TERRENO_V5.md` y el cimiento de todo el plan: si esto no es
 * exacto, nada de lo que se apoye encima puede serlo.
 *
 * ── QUÉ RESUELVE, Y POR QUÉ NO ES "OTRA FORMA DE HACER LO MISMO" ────────────────────────────────
 *
 * Hoy el clipmap deriva la posición de cada vértice de un MARCO TANGENTE EN FLOAT que se re-ancla
 * con el jugador. Eso tiene dos consecuencias medidas, y las dos desaparecen aquí:
 *
 *   1. `test_clipmap_dir_parity`: la `dir` reconstruida en float se separa **0,9302 m** de la que
 *      calcula la CPU en double, lo que produce hasta **0,1453 m** de diferencia de altura.
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
 * Es la misma garantía que el clipmap compraba redondeando el nivel de tesela a potencia de dos
 * (`clipmap.tesc::edgeFactor`), pero por construcción y sin perder resolución en el redondeo.
 *
 * Medido en `terrain_node_lattice` contra el camino que esto sustituye —reconstruir la dirección por
 * un marco tangente en float, como hacen `ringSample` y `clipmap.tese`— la diferencia es **0 bits
 * aquí contra 0,2290 m de superficie allí**, sobre los mismos puntos.
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
inline StitchRef nodeStitch(uint32_t u, uint32_t v, const int coarser[4]) {
    StitchRef r{ u, v, u, v, 0.0 };
    const uint32_t E = TERRAIN_NODE_CELLS;
    int edge = -1; uint32_t idx = 0;
    if      (u == 0 && coarser[0] > 0) { edge = 0; idx = v; }
    else if (u == E && coarser[1] > 0) { edge = 1; idx = v; }
    else if (v == 0 && coarser[2] > 0) { edge = 2; idx = u; }
    else if (v == E && coarser[3] > 0) { edge = 3; idx = u; }
    if (edge < 0) return r;

    const uint32_t stride = 1u << (uint32_t)coarser[edge];
    const uint32_t b = (idx / stride) * stride;
    const uint32_t nxt = std::min(b + stride, E);
    if (nxt == b) return r;
    r.t = (double)(idx - b) / (double)(nxt - b);
    if (edge <= 1) { r.v0 = b; r.v1 = nxt; }          // aristas verticales: varía v
    else           { r.u0 = b; r.u1 = nxt; }          // horizontales: varía u
    return r;
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

inline void nodeFillHeights(const NodeId& n, double planetRadiusM, float* out,
                            NodeRange* outRange = nullptr,
                            NodeBaseHeightFn baseFn = nullptr, void* baseCtx = nullptr) {
    const float triM = (float)nodeTexelM(n, planetRadiusM);
    NodeRange rg;
    for (uint32_t v = 0; v < TERRAIN_NODE_TEXELS; ++v)
        for (uint32_t u = 0; u < TERRAIN_NODE_TEXELS; ++u) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            float h = Haruka::Planet::terrainDetail(d, planetRadiusM, triM);
            // ⚠️ GEMELO de `terrain_node.comp`: base + detalle atenuado, con el detalle recortado
            // para que no hunda tierra bajo el nivel del mar. Sin `baseFn` esto es SOLO detalle —
            // que es lo que habia antes, y lo que hacia que el nodo describiera un planeta sin
            // continentes ni costa, ±4 km por debajo de lo que dibuja el clipmap.
            if (baseFn) {
                const float baseH = baseFn(d, baseCtx);
                h *= Haruka::Planet::seaLevelAttenuation(baseH);
                if (baseH > 0.0f && h < -baseH) h = -baseH;
                h += baseH;
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

/// Presupuesto de error, en píxeles. 1 px = el error geométrico de un nodo no llega a moverse un
/// píxel en pantalla, así que subdividir más no se puede ver.
inline constexpr double TERRAIN_NODE_ERROR_PX = 1.0;

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
inline bool nodeShouldSplit(const NodeId& n, double planetRadiusM,
                            const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                            double radPerPx, double errorPx = TERRAIN_NODE_ERROR_PX) {
    if (n.level >= TERRAIN_NODE_MAX_LEVEL) return false;
    // Distancia al punto del nodo más cercano a la cámara. Se muestrea su borde y su centro: es
    // barato y no hace falta la distancia exacta a un parche curvo — solo decidir un entero.
    double best = 1e300;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += TERRAIN_NODE_CELLS / 2)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += TERRAIN_NODE_CELLS / 2) {
            const glm::dvec3 p = planetCenter + nodeTexelDir(n, u, v) * planetRadiusM;
            best = std::min(best, glm::length(p - camPos));
        }
    const double dist = std::max(best, 1.0);
    return (nodeTexelM(n, planetRadiusM) / (dist * radPerPx)) > errorPx;
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
                              NodeRangeFn rangeFn = nullptr, void* rangeUser = nullptr) {
    out.clear();
    std::vector<NodeId> stack;
    for (int f = 0; f < 6; ++f) stack.push_back(NodeId{ (PlanetFace)f, 0, 0, 0 });
    while (!stack.empty()) {
        const NodeId n = stack.back(); stack.pop_back();
        // HORIZONTE PRIMERO: descartar lo que no se ve antes de gastarle presupuesto. Ver
        // `nodeBelowHorizon` — sin esto la cara oculta se come las plazas del campo cercano.
        if (nodeBelowHorizon(n, planetRadiusM, camPos, planetCenter)) continue;
        // FRUSTUM después del horizonte: los dos descartan, pero el de horizonte es más barato
        // (sin acos ni asin) y se lleva por delante media esfera antes de que el otro mire nada.
        if (viewDir) {
            // Cota del nodo: la que sepa el pool, y si no la sabe el bound conservador.
            double bound = terrainBoundM;
            if (rangeFn) { const NodeRange r = rangeFn(n, rangeUser); if (r.valid()) bound = r.boundM(); }
            if (nodeOutsideFrustum(n, planetRadiusM, camPos, planetCenter,
                                   *viewDir, coneHalfAngle, bound)) continue;
        }
        const bool room = (out.size() + stack.size() + 4) <= maxNodes;
        if (room && nodeShouldSplit(n, planetRadiusM, camPos, planetCenter, radPerPx, errorPx)) {
            NodeId kids[4]; nodeChildren(n, kids);
            for (const NodeId& k : kids) stack.push_back(k);
        } else {
            out.push_back(n);
        }
    }
}

}} // namespace Haruka::Terrain
