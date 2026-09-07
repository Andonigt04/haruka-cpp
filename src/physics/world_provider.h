/**
 * @file world_provider.h
 * @brief Ventana ABSTRACTA al mundo para la física — sin GL, sin renderer, sin assets.
 *
 * La física (`PhysicsEngine`) necesita tres cosas del mundo: la gravedad (posiciones y masas de los
 * cuerpos celestes), el nivel del mar del planeta activo (buoyancy) y la altura del terreno (ground).
 * Antes las pedía directamente a `WorldSystem`/`PlanetarySystem`, que arrastran el motor entero (y GL).
 *
 * Esta interfaz corta ese lazo: la física solo depende de `IWorldProvider` + glm. Así se compila:
 *   - en el CLIENTE, implementada sobre `WorldSystem` + la altura de la malla (F10), y
 *   - en el SERVIDOR (DGS), implementada sobre su estado autoritativo con el sampler analítico,
 *     SIN GL — que es lo que permite validar el movimiento con el MISMO código en ambos lados.
 *
 * Todas las magnitudes en METROS (mismas unidades que las posiciones de los cuerpos físicos).
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cmath>
#include "core/planet/soi.h"        // GravityBody + gravedad N-cuerpos (matemática pura, sin motor)
#include "core/planet/terrain_lod.h" // geometría de los anillos de colisión (constantes compartidas)

namespace Haruka { namespace Physics {

/** @brief Un cuerpo que ejerce gravedad.
 *
 *  Es un ALIAS de `Haruka::Planet::GravityBody`, no una copia reducida. Antes era un struct propio
 *  `{worldPos, mass}` y el motor tenía que traducir; con dos definiciones de "cuerpo que ejerce
 *  gravedad" cualquier campo nuevo hay que añadirlo dos veces y convertirlo en medio. Ese header es
 *  matemática pura (sin dependencias del motor ni del render), así que la física puede incluirlo sin
 *  romper el aislamiento que esta interfaz existe para mantener.
 *
 *  Los campos `radius`, `soiRadius` y `parent` no son adorno: sin ellos la física no puede responder
 *  dentro de qué cuerpo estás (la gravedad de una esfera uniforme DECRECE hacia el centro; con GM/r²
 *  a secas, atravesar el suelo un frame te dispara al infinito) ni en la esfera de influencia de
 *  quién estás (qué cuerpo define tu marco, tu terreno y tu cielo). */
using GravBody = Haruka::Planet::GravityBody;

class IWorldProvider {
public:
    virtual ~IWorldProvider() = default;

    /** @brief ¿Hay un planeta activo (para buoyancy / nivel del mar)? */
    virtual bool       hasActivePlanet()    const = 0;
    /** @brief Centro del planeta activo (mundo, m). */
    virtual glm::dvec3 activePlanetCenter() const = 0;
    /** @brief Radio del planeta activo = nivel del mar (esfera de referencia, m). */
    virtual double     activePlanetRadius() const = 0;

    /** @brief Cuerpos que ejercen gravedad (Sol, planetas, lunas). La física los suma. */
    virtual const std::vector<GravBody>& gravBodies() const = 0;

    /** @brief Índice en `gravBodies()` del cuerpo cuya gravedad DOMINA en `worldPos`, o -1.
     *
     *  No es lo mismo que "el que más tira": es el más PROFUNDO en la jerarquía cuya esfera de
     *  influencia contiene el punto, y es quien define el marco local, el terreno con el que
     *  colisionas y el cielo que ves. La implementación por defecto lo resuelve con la jerarquía que
     *  ya viene en `gravBodies()`, así que un proveedor no tiene que hacer nada para tenerlo. */
    virtual int dominantBody(const glm::dvec3& worldPos) const {
        return Haruka::Planet::dominantBodyIndex(gravBodies(), worldPos);
    }

    /** @brief Altura del terreno en `worldPos`: metros SOBRE la esfera de referencia (radio). El
     *  ground-snap la usa. Cliente: malla (F10). Servidor: sampler analítico. Mismas unidades. */
    virtual double terrainHeightAt(const glm::dvec3& worldPos) const = 0;

    /** @brief Centinela de "aquí no hay agua". Gemelo de `PlanetarySystem::kNoWater`. */
    static constexpr double kNoWater = -1e30;

    /**
     * @brief Cota de la SUPERFICIE DEL AGUA en `worldPos`: metros sobre la esfera de referencia,
     *        las mismas unidades que `terrainHeightAt`. `kNoWater` si aquí no hay agua.
     *
     * ⚠️ ES LA COTA CON LA OLA PUESTA, no el nivel del mar en reposo. Ésa es toda la diferencia entre
     * flotar sobre el mar que se ve y flotar sobre la esfera de radio R — que es lo que hacía la
     * física antes de existir esta consulta (ver `PhysicsEngine::integrateForces`).
     *
     * El default devuelve `kNoWater`: un proveedor que no sepa de agua (el DGS, los tests) deja la
     * flotación apagada en vez de inventarse un mar. Un cero aquí sería un dato falso con pinta de
     * dato — el mismo error que `groundHeightKmAtDir` documenta haber pagado ya.
     */
    virtual double waterSurfaceAt(const glm::dvec3& /*worldPos*/) const { return kNoWater; }

    /**
     * @brief Velocidad del agua en la superficie (m/s, marco del mundo). Cero si no hay agua.
     *
     * El movimiento orbital de la ola. Es lo que ARRASTRA a un cuerpo flotante: sin ella la flotación
     * es puramente vertical y nada llega nunca a la orilla por sí solo.
     */
    virtual glm::dvec3 waterVelocityAt(const glm::dvec3& /*worldPos*/) const { return glm::dvec3(0.0); }

    /**
     * @brief ESPUMA en la superficie del agua, 0..1. 0 = agua limpia · 1 = agua blanca.
     *
     * El agua blanca de una rompiente esta AIREADA: pesa menos (menos empuje) y arrastra mucho mas
     * (la ola te lleva). Sin esto, una ola que revienta lo hacia solo para la camara.
     *
     * El default es 0 —agua limpia— y no es un dato inventado: es exactamente lo que significa "no
     * se de espuma" para quien la consume, porque anula los dos efectos y deja la flotacion como
     * estaba. Un proveedor sin mar (el DGS, los tests) no cambia de comportamiento.
     */
    virtual float waterFoamAt(const glm::dvec3& /*worldPos*/) const { return 0.0f; }

    /** @brief (Fase 2) Geometría LOCAL del terreno alrededor de `center` (radio `radius`, m), para
     *  colisión REAL con la malla (no una altura vertical). Rellena `outVerts` (posiciones mundo) y
     *  `outTris` (índices de triángulo, 3 por cara) y devuelve true si hay malla. Por defecto false →
     *  el motor usa la aproximación de esfera. Cliente: triángulos de los chunks residentes; servidor:
     *  sin malla (usa la altura analítica). Se consulta al entrar en una región nueva o al editar. */
    virtual bool terrainMesh(const glm::dvec3& /*center*/, double /*radius*/,
                             std::vector<glm::dvec3>& /*outVerts*/,
                             std::vector<uint32_t>& /*outTris*/) const { return false; }

    /** @brief El BLOQUE CERCANO como HEIGHTFIELD: una rejilla uniforme de alturas, sin malla.
     *
     *  Por qué existe además de `terrainMesh`
     *  --------------------------------------
     *  El bloque uniforme ya ES una rejilla regular alineada al quad del render; entregarlo como
     *  triángulos obliga a Jolt a guardar posiciones, índices y un árbol AABB para una geometría cuya
     *  única información son las alturas. `HeightFieldShape` la guarda cuantizada sobre una rejilla
     *  implícita: mismos vértices, una fracción de la memoria y construcción lineal.
     *
     *  `outSamples` son `N·N` alturas EN EL MARCO TANGENTE ANCLADO (fila mayor), o sea la componente
     *  radial respecto al punto de superficie del ancla — NO la altitud sobre el planeta. La
     *  diferencia es la sagita de la esfera (5 mm a 256 m sobre la Tierra) y va incluida en la
     *  muestra, porque un heightfield es plano y el terreno no.
     *
     *  `outOrigin` es ese punto de superficie en coordenadas de mundo y `outT1/outUp/outT2` el triedro
     *  con el que interpretar la rejilla: la muestra `(i,j)` está en
     *  `outOrigin + outT1·(LO + i·cell) + outT2·(LO + j·cell) + outUp·sample[j·N+i]`.
     *
     *  Devuelve false si el provider no puede darlo (servidor, tests): entonces la malla lo cubre todo
     *  y `terrainMesh` no debe dejar hueco. */
    virtual bool terrainHeightField(const glm::dvec3& /*center*/,
                                    std::vector<float>& /*outSamples*/, uint32_t& /*outN*/,
                                    double& /*outCell*/, glm::dvec3& /*outOrigin*/,
                                    glm::dvec3& /*outT1*/, glm::dvec3& /*outUp*/,
                                    glm::dvec3& /*outT2*/) const { return false; }

    /** @brief Un nivel de la pila de ANILLOS de heightfield (ver `terrainHeightFieldRings`). */
    struct TerrainRing {
        std::vector<float>          samples;  ///< `n·n` alturas radiales (fila mayor); NaN = sin superficie.
        Haruka::Planet::TerrainRingSpec spec; ///< Celda, alcance, hueco y tamaño. NO es igual en todos.
    };

    /** @brief EL TERRENO ENTERO como una PILA DE ANILLOS de heightfield, sin un solo triángulo.
     *
     *  Sustituye al par `terrainMesh` + `terrainHeightField`: el nivel 0 es el bloque cercano de
     *  siempre y cada nivel siguiente dobla la celda y cubre el doble, con el centro AGUJEREADO
     *  exactamente donde llega el nivel de dentro. Once niveles alcanzan ±262 km.
     *
     *  Por qué, con el número que lo decide: entregar el campo lejano como malla obliga a Jolt a
     *  construir un árbol AABB sobre 479 814 triángulos, que son 802-1274 ms MEDIDOS en el juego —
     *  el 65 % del coste de rehacer el suelo. Los once anillos cuestan 18,3 ms. Ver el bloque de
     *  `terrain_lod.h` para el desglose completo y para por qué trocear la malla en parches NO es
     *  una alternativa (0 % de nodos reutilizables al derivar).
     *
     *  TODOS los anillos comparten el marco tangente anclado que se devuelve una sola vez, así que
     *  son la misma superficie a distintas resoluciones y no varias aproximaciones. La muestra
     *  `(i,j)` del anillo `k` está en
     *  `outOrigin + outT1·terrainRingNode(i, cell) + outT2·terrainRingNode(j, cell) + outUp·sample`.
     *
     *  `firstRing` salta los anillos interiores sin muestrearlos. Existe porque el anillo 0 y los de
     *  fuera tienen CADENCIAS distintas: el 0 sigue al anclaje del clipmap (cada 4 m, ~20 ms) y los
     *  demás a la deriva de 48 m. Rehacerlos juntos obligaba a elegir entre pagar 300 ms cada 4 m o
     *  dejar el suelo que se pisa desincronizado del que se dibuja hasta 48 m — que es justo la
     *  disparidad que se está persiguiendo. La geometría de cada anillo no depende de los de dentro
     *  (su hueco sale de `TerrainRingSpec`), así que saltarlos es seguro. */
    virtual bool terrainHeightFieldRings(const glm::dvec3& /*center*/, double /*halfExtent*/,
                                         std::vector<TerrainRing>& /*outRings*/,
                                         glm::dvec3& /*outOrigin*/, glm::dvec3& /*outT1*/,
                                         glm::dvec3& /*outUp*/, glm::dvec3& /*outT2*/,
                                         size_t /*firstRing*/ = 0) const {
        return false;
    }

    /** @brief EL ESCALÓN EN LAS COSTURAS entre anillos, en metros: el peor y en qué nivel.
     *
     *  Es el precio declarado del diseño de anillos y hay que poder verlo, no suponerlo. La rejilla
     *  por anillos de `terrainRingGrid` es un producto tensorial y no tiene ni una costura; una pila
     *  de heightfields sí. En la frontera entre el nivel `k` (celda `c`) y el `k−1` (celda `c/2`) los
     *  nodos gruesos son un SUBCONJUNTO de los finos, así que coinciden uno sí y uno no. En los que
     *  no —el punto medio de cada arista gruesa— el lado fino tiene una muestra propia y el grueso
     *  solo la recta entre sus vecinos: la diferencia es la sagita de la celda gruesa, y se ve como
     *  un escalón.
     *
     *  Se mide sobre las muestras REALES que se le van a entregar a Jolt, no sobre una idealización
     *  del terreno: las dos caras salen de anillos distintos de `outRings`, que es la única forma de
     *  que el número signifique algo. Comparar la función de altura consigo misma daría 0 siempre.
     *
     *  ⚠️ EL PEOR SOLO NO SIRVE, y por eso `outPerLevel` no es opcional en la práctica. El escalón va
     *  con la sagita, o sea con el CUADRADO de la celda: el nivel 10 (celda 4096 m) da ~110 m, pero su
     *  costura está a 131 km del jugador, donde nadie pisa y donde la malla a la que sustituye tenía
     *  celdas de 20 km — peores. Reportar solo el máximo condena el diseño por su parte irrelevante.
     *  Lo que decide es el escalón en los primeros niveles, que es donde se camina.
     *
     *  Devuelve 0 si hay menos de dos anillos (sin dos niveles no hay costura). */
    static double terrainRingSeamStep(const std::vector<TerrainRing>& rings,
                                      int* outWorstLevel = nullptr,
                                      std::vector<double>* outPerLevel = nullptr) {
        double worst = 0.0; int worstK = -1;
        if (outPerLevel) outPerLevel->assign(rings.size(), 0.0);
        auto at = [](const TerrainRing& r, int i, int j) {
            return (double)r.samples[(size_t)j * r.spec.samples + (size_t)i];
        };
        for (size_t k = 1; k < rings.size(); ++k) {
            const TerrainRing& F = rings[k - 1];
            const TerrainRing& C = rings[k];
            const double e = F.spec.extent;
            if (std::abs(C.spec.hole - e) > 1e-9) continue;   // no comparten costura
            // Cuántas celdas GRUESAS caben en una FINA. Si vale 1 los dos anillos tienen la misma
            // celda (pasa dentro de la caja del clipmap, donde la celda va topada): entonces cada
            // nodo fino tiene gemelo grueso y NO HAY escalón que medir.
            const double rho = C.spec.cell / F.spec.cell;
            // Índice del nodo de coordenada `x`: `x/celda + half + 1` (ver `terrainRingNode`).
            const int cq = (int)std::llround(e / C.spec.cell);
            const int fineEdge[2]   = { F.spec.half + 1 - F.spec.half, F.spec.half + 1 + F.spec.half };
            const int coarseEdge[2] = { C.spec.half + 1 - cq,          C.spec.half + 1 + cq };
            for (int side = 0; side < 2; ++side) {
                for (int m = -F.spec.half; m <= F.spec.half; ++m) {
                    const double u  = (double)m / rho;            // el nodo fino, en celdas gruesas
                    const double u0 = std::floor(u);
                    if (std::abs(u - u0) < 1e-9) continue;        // tiene gemelo grueso: coinciden
                    const double w  = u - u0;
                    const int jf = m + F.spec.half + 1;
                    const int c0 = (int)u0 + C.spec.half + 1, c1 = c0 + 1;
                    if (c0 < 0 || c1 >= (int)C.spec.samples) continue;
                    // El nodo fino contra la recta que une los dos gruesos que lo rodean. Se hace en
                    // las dos orientaciones: la costura es un cuadrado, así que hay columnas (x fijo)
                    // y filas (z fijo).
                    auto lerp = [&](double a, double b) { return a + (b - a) * w; };
                    const double colF = at(F, fineEdge[side], jf);
                    const double colC = lerp(at(C, coarseEdge[side], c0), at(C, coarseEdge[side], c1));
                    const double rowF = at(F, jf, fineEdge[side]);
                    const double rowC = lerp(at(C, c0, coarseEdge[side]), at(C, c1, coarseEdge[side]));
                    for (double d : { std::abs(colF - colC), std::abs(rowF - rowC) }) {
                        if (!(d == d)) continue;                 // NaN: nodo sin superficie
                        if (d > worst) { worst = d; worstK = (int)k; }
                        if (outPerLevel && d > (*outPerLevel)[k]) (*outPerLevel)[k] = d;
                    }
                }
            }
        }
        if (outWorstLevel) *outWorstLevel = worstK;
        return worst;
    }

    /** @brief SONDA DE PARIDAD: qué suelo hay en `worldPos` según cada descripción.
     *
     *  `outFuncH` = la FUNCIÓN continua (`terrainHeightAt`), que es la que evalúa el render por
     *  vértice. `outMeshH` = la superficie POLIGONAL que de verdad se colisiona: la malla es lineal
     *  dentro de cada celda de su rejilla, así que entre vértices NO vale la función.
     *
     *  La diferencia es la disparidad real entre lo que se pisa y lo que se ve, y no se puede
     *  deducir comparando la función consigo misma — hay que reconstruir la celda. `meshCenter` es
     *  el centro con el que se construyó la malla vigente (el jugador se aleja de él hasta que se
     *  reconstruye, y la celda crece con esa distancia).
     *
     *  Devuelve false si el proveedor no tiene malla (servidor): entonces no hay disparidad que medir. */
    virtual bool terrainParityProbe(const glm::dvec3& /*worldPos*/, const glm::dvec3& /*meshCenter*/,
                                    double& /*outFuncH*/, double& /*outMeshH*/,
                                    double& /*outCellM*/) const { return false; }
};

}} // namespace Haruka::Physics
