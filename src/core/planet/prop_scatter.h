/**
 * @file prop_scatter.h
 * @brief Scatter GLOBAL de props, determinista por CELDA MUNDIAL, con LOD por bandas. GL-free.
 *
 * Es el corazón de la colocación "el mismo árbol está siempre en el mismo sitio": NO hay un
 * parche anclado al ecuador ni un hash dependiente de la cámara. Cada instancia vive en una
 * CELDA CÚBICA DEL MUNDO (coordenadas absolutas, cuantizadas a `cellM`), y su identidad,
 * posición, escala, tinte y estado salen SOLO del hash de esa celda + la semilla del planeta.
 *
 * El enumerador usa una rejilla TANGENTE alrededor del jugador (como el sistema de props del
 * Survival) SOLO para saber qué celdas del mundo existen cerca; la colocación en sí no depende
 * de esa rejilla. Resultado:
 *   · vuelves a un sitio → el mismo árbol en el mismo sitio (sin "tp" al cruzar fronteras).
 *   · el jugador se mueve → entran celdas por delante y salen por detrás, sin que nada se mueva.
 *   · es indistinguible de "todo el planeta existe": la misma función evaluada en cualquier radio.
 *
 * LOD: el radio se cubre en BANDAS de celda creciente (cerca = fina, lejos = gruesa) con FUNDIDO
 * en el borde de cada banda (encoge antes de cruzar), así NO se ve un "anillo" cortado. A 6 km en
 * un planeta de 6371 km ya estás por debajo del horizonte: no hay que cubrir la esfera entera.
 *
 * El clima/forma se pregunta al MISMO campo que pinta el planeta (humedad/temp/pendiente +
 * densityMap), a través de `IPropSphereField` — el espejo esférico de `IPropField`. Los tests lo
 * simulan; el motor lo implementa muestreando PlanetFields/SimplePlanet por dirección.
 */
#pragma once
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "core/planet/prop_layer.h"        // PropLayerTable, PlacedProp
#include "core/terrain/planet_fields.h"    // FieldSample
#include "core/terrain/cube_sphere.h"      // cubeFaceToDir / dirToCubeFaceClosed: el cuantizador
#include "tools/procgraph/tree_spawn.h"    // slopeAt pattern, IPropField
#include "tools/procgraph/tree_prop.h"     // treePoisson
#include "tools/procgraph/proc_noise.h"    // hash32

namespace Haruka { namespace Planet {
namespace PG = Haruka::Tools::ProcGraph;

/** @brief Interfaz del campo ESFÉRICO (ecología + cota por DIRECCIÓN). La implementa el motor
 *  muestreando el PlanetarySystem real; los tests la simulan. Es el espejo de `IPropField`
 *  (plano) pero sin parche tangente: se pregunta por la dirección unitaria del planeta. */
class IPropSphereField {
public:
    virtual ~IPropSphereField() = default;

    /// Ecología del punto (humedad/temp/…).
    virtual FieldSample sampleAt(const glm::vec3& dir) const = 0;
    /// Cota del suelo (m) sobre el radio en la dirección, como ver para asentar el prop.
    virtual float heightAt(const glm::vec3& dir) const = 0;
    /// Distribución por mapa [0,1] en la dirección (1 = sin mapa de distribución).
    virtual float mapDensityAt(const glm::vec3& dir, const std::string& mapPath) const { return 1.0f; }
    /// Zona del zoneMap en la dirección: nombre del material asignado (vacío = sin zona/mapa).
    virtual std::string zoneAt(const glm::vec3& dir) const { return {}; }
    /// MATERIAL del terreno en la dirección (p.ej. "sand", "forest"). Distinto de la zona nombrada.
    virtual std::string layerAt(const glm::vec3& dir) const { return {}; }
};

/** @brief Pendiente local por diferencias finitas sobre el campo esférico. EPS en metros; se
 *  convierte a desplazamiento angular (arc = eps/radio). Igual idea que `slopeAt` plano.
 *  Este variante toma la altura del centro YA muestreada (el scatter la reusa en vez de volver
 *  a muestrear `dir` — ahorra una `sampleHeight` por celda, la parte cara del scatter).
 *  Nombre distinto a propósito: con `float` como 4º arg, un overload con `h0` sería ambiguo. */
inline float sphereSlopeWithHeightAt(const IPropSphereField& f, const glm::vec3& dir,
                                     double radius, float h0, float eps = 2.0f) {
    const glm::vec3 up = dir;
    glm::vec3 ref = (std::abs(up.y) < 0.99f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    const glm::vec3 t = glm::normalize(glm::cross(ref, up));
    const glm::vec3 b = glm::cross(up, t);
    const float arc = (float)(eps / radius);
    const glm::vec3 dtx = glm::normalize(up * std::cos(arc) + t * std::sin(arc));
    const glm::vec3 dty = glm::normalize(up * std::cos(arc) + b * std::sin(arc));
    const float hx = f.heightAt(dtx);
    const float hy = f.heightAt(dty);
    const float dx = (hx - h0) / eps;
    const float dy = (hy - h0) / eps;
    return std::atan(std::sqrt(dx * dx + dy * dy));   // rad
}

/** @brief Igual, muestreando el centro dentro (comodidad: llama a sphereSlopeWithHeightAt). */
inline float sphereSlopeAt(const IPropSphereField& f, const glm::vec3& dir,
                           double radius, float eps = 2.0f) {
    return sphereSlopeWithHeightAt(f, dir, radius, f.heightAt(dir), eps);
}

/** @brief Configuración del scatter: las BANDAS de LOD que cubren el radio alrededor del jugador. */
struct PropScatterLod {
    float radiusM  = 1000.0f;   ///< radio exterior de la banda (m)
    float cellM    = 12.0f;     ///< tamaño de celda del mundo (m) — resolución del Poisson
    float minSep   = 0.30f;     ///< separación mínima dentro de la celda (fracción de celda)
};

/** @brief Configuración global del scatter. Por defecto: 3 bandas que llegan al horizonte. */
struct PropScatterParams {
    std::vector<PropScatterLod> lods = {
        { 1000.0f, 12.0f,  0.30f },
        { 3000.0f, 40.0f,  0.30f },
        { 6000.0f, 120.0f, 0.30f },
    };
    uint32_t seed    = 1u;       ///< semilla del planeta
    double   radius  = 6371000.0;///< radio del planeta (m) — convierte m locales en arco
    int      maxProps = 200000;  ///< cota de seguridad de instancias por scatter
};

/** @brief Una instancia colocada con coordenadas de MUNDO (dir + cota), lista para instancing.
 *  El renderer la convierte en una entrada del buffer de instancias: malla compartida por
 *  `mesh`, transform por esta instancia. El `state` permite ocultar/destruir (tocón ausente). */
struct ScatteredProp {
    std::string mesh;             ///< tipo de objeto ("tree", "rock", …) → prototipo
    int         layerIndex = -1;  ///< de qué capa de la tabla salió
    glm::vec3   dir        = glm::vec3(0,1,0);  ///< dirección unitaria sobre la esfera
    float       heightM    = 0.0f;              ///< cota del suelo (m)
    uint32_t    cellSeed   = 0;                 ///< semilla determinista (celda del mundo)
    float       scale      = 1.0f;              ///< escala de la capa × variación de celda
    glm::vec3   tint       = glm::vec3(1.0f);   ///< tinte por instancia (variedad sin geometría)
    uint32_t    state      = 0;                 ///< 0 = vivo · >0 = oculto/destruido (tocón)
};

/** @brief Coloca las capas de props delante del jugador, determinista por celda mundial.
 *
 * @param field    Campo esférico real (ecología + cota por dirección).
 * @param camPos   Posición del jugador en el MUNDO (m), para saber qué celdas enumerar.
 * @param planetC  Centro del planeta en el MUNDO (m).
 * @param params   Bandas de LOD + semilla + radio del planeta.
 * @param table    Capas de props en orden de prioridad (primero instala = mayor prioridad).
 * @return         Instancias colocadas (ya con estado/tinte deterministas), en orden de banda.
 */
/** @brief Por qué cada capa colocó lo que colocó. Diagnóstico opcional del scatter.
 *
 *  Sin esto, una capa a cero es indistinguible de otra: no se sabe si sus condiciones nunca pasan o
 *  si una capa de mayor prioridad le come las celdas antes de que le toque el turno. Son dos bugs
 *  con arreglos opuestos —tocar la capa, o reordenar la tabla— y el motor no lo decía.
 */
struct PropScatterStats {
    std::vector<int> offered;     ///< celdas en las que la capa LLEGÓ a evaluarse (tuvo turno)
    std::vector<int> noSubmerged; ///< descartes por estar bajo el agua sin declarar `submerged`
    std::vector<int> noCoverage;  ///< descartes por `coverage()` == 0 (total)
    std::vector<int> noDensity;   ///< descartes por el dado de `density`
    std::vector<int> placed;      ///< colocadas
    // Desglose de `noCoverage` por factor: es lo que dice QUÉ campo del JSON tocar.
    std::vector<int> failZone, failWhen, failHum, failTemp, failSlope, failMap;

    // Rangos que el terreno REALMENTE tiene en las celdas visitadas. Sin esto el desglose dice
    // "la humedad no pasa" pero no que tu banda pide 0.35-0.95 donde el terreno da 0.02-0.14.
    float humLo = 1e9f, humHi = -1e9f, tempLo = 1e9f, tempHi = -1e9f, slopeLo = 1e9f, slopeHi = -1e9f;
    int   cells = 0;

    void resize(size_t n) { offered.assign(n,0); noSubmerged.assign(n,0); noCoverage.assign(n,0);
                            noDensity.assign(n,0); placed.assign(n,0);
                            failZone.assign(n,0); failWhen.assign(n,0); failHum.assign(n,0);
                            failTemp.assign(n,0); failSlope.assign(n,0); failMap.assign(n,0); }
};

inline std::vector<ScatteredProp> scatterPropsNear(
    const IPropSphereField& field,
    const glm::dvec3& camPos,
    const glm::dvec3& planetC,
    const PropScatterParams& params,
    const PropLayerTable& table,
    PropScatterStats* stats = nullptr) {
    if (stats) stats->resize(table.layers.size());

    std::vector<ScatteredProp> out;

    // Dirección del jugador desde el centro del planeta → base tangente para enumerar.
    const glm::dvec3 toCam = camPos - planetC;
    const double cdist = glm::length(toCam);
    if (cdist < 1.0) return out;
    const glm::vec3 dirCam = glm::normalize(glm::vec3(toCam));
    glm::vec3 ref = (std::abs(dirCam.y) < 0.99f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    const glm::vec3 Td = glm::normalize(glm::cross(ref, dirCam));
    const glm::vec3 Bd = glm::cross(dirCam, Td);

    const double R = params.radius;

    // ¿Alguna capa usa `zones` o la condición `when`? Si ninguna, NO se muestrean el zoneMap y el
    // material por celda (2 muestreos + el match por celda): es el grueso del coste del pase en
    // la tabla por defecto (~61k celdas).
    bool needZone  = false;
    bool needLayer = false;
    for (const auto& L : table.layers) {
        if (!L.zones.empty() || L.when.find("zone") != std::string::npos) needZone = true;
        if (L.when.find("layer") != std::string::npos) needLayer = true;
    }

    std::unordered_set<uint64_t> seenCells;

    // ⚠️ LAS BANDAS SON ANULARES. Antes cada una recorría su radio ENTERO desde el centro y el
    // deduplicado era POR BANDA (`cellKey ^ bi`), así que el mismo sitio recibía un prop de la banda
    // 0, otro de la 1 y otro de la 2: en los primeros 1000 m se apilaban TRES conjuntos de props
    // unos encima de otros. Cada banda cubre ahora solo lo que la anterior no alcanza.
    //
    // ⚠️ EL CORTE ERA DE CHEBYSHEV (max(|x|,|y|)) Y SE VEIA EL CUADRADO. Andoni: "se ve como hay un
    // LOD cuadrado para maximizar el render de props en ese cuadrado". Literal: con Chebyshev cada
    // banda barre un CUADRADO de semilado `radiusM`, asi que el borde exterior de la ultima banda es
    // un cuadrado y en las esquinas los props llegan `sqrt(2)` mas lejos que de frente.
    //
    // El comentario anterior defendia Chebyshev diciendo que con un corte circular "quedarian las
    // esquinas del cuadrado interior sin cubrir". Eso solo pasa si se mezclan metricas: si el corte
    // interior es circular y el exterior cuadrado, si queda un anillo de huecos. Con las DOS
    // euclideas cada banda es una CORONA y encajan sin hueco ni solape, que es lo que hay ahora.
    //
    // De rebote sale mas barato: un circulo es el 78,5% del cuadrado que lo contiene, asi que se
    // enumeran ~21% menos celdas — y la parte cara del scatter es justo muestrear la celda.
    float prevRad = 0.0f;
    for (size_t bi = 0; bi < params.lods.size(); ++bi) {
        const PropScatterLod& lod = params.lods[bi];
        const float cell  = lod.cellM;
        const float rad   = lod.radiusM;
        // ── ⚠️ SE ENUMERA AL DOBLE DE FINO QUE LA CELDA, Y NO ES DERROCHE: ES LO QUE LO HACE ESTABLE.
        //
        // Aqui habia UN punto por celda, y eso es un aliasing de libro: la rejilla de enumeracion va
        // anclada a la CAMARA y la cuantizacion va a la rejilla del MUNDO, las dos con el mismo paso.
        // Dos rejillas iguales desplazadas una fraccion arbitraria —donde este el jugador— no se
        // corresponden una a una: hay celdas del mundo que no reciben ningun punto (y su prop
        // desaparece) y otras que reciben dos (se deduplican). Al caminar cambia cuales.
        //
        // Medido con `prop_scatter_stability`, andando 5 m: **desaparecia el 15,9% de los props de
        // los primeros 900 m**. No era el borde de banda, era en todas partes.
        //
        // Un cuadrado de lado `cell` SIEMPRE contiene al menos un punto de una rejilla de paso
        // `cell/2`, asi que con esto ninguna celda del mundo se queda sin visitar. Cuesta 4x puntos
        // de enumeracion, pero el trabajo caro (muestrear el campo) va DESPUES del deduplicado, asi
        // que se paga en la parte barata. Medido: **15,9% -> 3,2%** de props perdidos al andar 5 m.
        //
        // ⚠️ EL 3% QUE QUEDA ES ESTRUCTURAL Y NO SE ARREGLA SOBREMUESTREANDO MAS. La celda del mundo
        // es un CUBO de una rejilla 3D, y lo que se recorre es la SUPERFICIE de una esfera: la
        // interseccion de un cubo con la esfera tiene area muy variable —casi cero cuando la esfera
        // roza una esquina— y esas celdas diminutas se saltan aunque afines el paso. Es el
        // cuantizador equivocado para una esfera.
        //
        // Lo correcto es direccionar los props por la parametrizacion de CARA DE CUBO del planeta
        // (`face,level,i,j`, la misma que los nodos del terreno): celdas de area pareja, enumeracion
        // exacta recorriendo enteros, e identidad que no depende de donde este la camara. Es el
        // "sistema propio con enlace al del planeta" — y ahora hay medida que lo justifica.
        const float step  = cell * 0.5f;
        const int   n     = (int)std::ceil(rad / step);

        // ── ⚠️ LA CELDA ES DE LA CARA DEL CUBO DEL PLANETA, NO DE UNA REJILLA CUBICA 3D ──────────
        //
        // Antes se cuantizaba con `floor(posicionEnLaEsfera / cell)` sobre una rejilla cubica de 3D.
        // Ese es el cuantizador equivocado para una superficie esferica: la interseccion de un cubo
        // con la esfera tiene area MUY variable —casi cero cuando la esfera roza una esquina del
        // cubo— y esas celdas diminutas se saltaban en la enumeracion aunque se afinara el paso. Es
        // lo que dejaba un 3% de props parpadeando al caminar despues de arreglar el aliasing.
        //
        // Ahora la celda es `(cara, nivel, i, j)` de la misma parametrizacion que usan los nodos del
        // terreno: celdas de area pareja, identidad que no depende de donde este la camara, y la
        // enumeracion solo tiene que ACERTAR la celda, no adivinar su tamaño.
        //
        // El nivel sale del `cellM` que pide la banda: una cara abarca `pi/2` de arco, asi que al
        // nivel L la celda mide `R*(pi/2)/2^L`. Se redondea al nivel mas cercano — el `cellM` de la
        // configuracion es un objetivo, no un contrato.
        const double faceArc = R * 1.5707963267948966;
        const int    lvl     = std::max(0, std::min(28,
                                   (int)std::lround(std::log2(faceArc / (double)cell))));
        const double cells   = (double)(1u << lvl);      // celdas por lado de cara
        const float inner = prevRad;      // lo que ya cubre la banda anterior
        prevRad = rad;

        for (int j = -n; j <= n; ++j) {
            for (int i = -n; i <= n; ++i) {
                // Corona euclidea: dentro de ESTA banda y fuera de la anterior. El corte exterior
                // es lo que quita el cuadrado; el interior evita apilar un prop sobre otro.
                {
                    const float cx = (float)i * step, cy = (float)j * step;
                    const float d  = std::sqrt(cx * cx + cy * cy);
                    if (d >= rad) continue;                  // fuera del alcance de la banda
                    if (inner > 0.0f && d < inner) continue; // ya la sembro la banda anterior
                }
                // Punto de la rejilla tangente (solo ENUMERA qué zona cubrir).
                const glm::dvec3 wp = planetC + glm::dvec3(dirCam) * R
                                    + glm::dvec3(Td) * (double)(i * step)
                                    + glm::dvec3(Bd) * (double)(j * step);
                // CELDA DEL PLANETA: (cara, nivel, i, j). La identidad de un prop sale SOLO de eso
                // y de la semilla — ni de la camara, ni de la banda, ni del paso de enumeracion.
                Haruka::PlanetFace face; double lx = 0.0, ly = 0.0;
                Haruka::dirToCubeFaceClosed(glm::normalize(wp - planetC), face, lx, ly);
                const int gi = (int)std::floor((lx * 0.5 + 0.5) * cells);
                const int gj = (int)std::floor((ly * 0.5 + 0.5) * cells);
                if (gi < 0 || gj < 0 || gi >= (int)cells || gj >= (int)cells) continue;
                // ⚠️ La clave lleva el NIVEL, no el indice de banda. Con `bi` dentro, el mismo sitio
                // del mundo visto desde dos bandas distintas daba dos props apilados; con el nivel,
                // dos bandas que compartieran nivel comparten celda, que es lo correcto.
                const uint64_t cellKey = ((uint64_t)face << 60) ^ ((uint64_t)lvl << 54)
                                       ^ ((uint64_t)(uint32_t)gi << 27) ^ (uint64_t)(uint32_t)gj;
                if (!seenCells.insert(cellKey).second) continue;

                const uint32_t hc = PG::hash32(PG::hash32(PG::hash32(PG::hash32(
                    params.seed ^ (uint32_t)face) ^ (uint32_t)lvl) ^ (uint32_t)gi) ^ (uint32_t)gj);

                // Centro de la celda + jitter DENTRO de ella, en coordenadas de la cara: asi el
                // desplazamiento es una fraccion de celda de verdad, no de un cubo que la corta.
                const double cw = 2.0 / cells;            // ancho de celda en (lx,ly)
                const double jx = (double)PG::WhiteNode::hashFloat((int)hc, 1, 0, params.seed) - 0.5;
                const double jy = (double)PG::WhiteNode::hashFloat((int)hc, 2, 0, params.seed) - 0.5;
                const double clx = -1.0 + ((double)gi + 0.5 + jx * 0.7) * cw;
                const double cly = -1.0 + ((double)gj + 0.5 + jy * 0.7) * cw;
                const glm::vec3 dir = glm::vec3(Haruka::cubeFaceToDir(face, clx, cly));

                // Cota de seguridad: no rellenar el buffer si hay demasiado (LOD bajo / planeta raro).
                if ((int)out.size() >= params.maxProps) return out;

                // Ecología/pendiente locales → ¿cuánto cumple la capa? 0 = fuera de su territorio.
                // La cota del suelo se muestrea UNA vez y se reusa para la pendiente y para asentar
                // el prop (antes se llamaba `heightAt(dir)` 2× más dentro de sphereSlopeAt).
                const FieldSample fs = field.sampleAt(dir);
                const float hM    = field.heightAt(dir);         // cota del suelo (m), precisión exacta
                // BAJO EL NIVEL DEL MAR se salta por CAPA, no global: la mayoría de los props
                // (árbol/casa/camino) no deben instalarse sumergidos — se dibujan igual pero el
                // océano los tapa (coste de draw para algo que no se ve). Las capas que declaran
                // `submerged` (la roca del lecho) sí se asientan sobre el fondo. `hM` ya es altura
                // sobre el nivel del mar.
                const float slope = sphereSlopeWithHeightAt(field, dir, R, hM);
                // Zona del zoneMap en esta celda (NO depende de la capa): se muestrea UNA vez y las
                // capas con `zones` declaradas la filtran. Vacío = planeta sin mapa de zonas.
                const std::string zone  = needZone ? field.zoneAt(dir) : std::string{};
                // Material del terreno en la celda: alimenta la condición booleana `when` de cada capa.
                const std::string layer = needLayer ? field.layerAt(dir) : std::string{};

                // Lo que el terreno DA aquí, para poder contrastarlo con lo que las bandas PIDEN.
                if (stats) {
                    ++stats->cells;
                    stats->humLo   = std::min(stats->humLo,   fs.humidity);
                    stats->humHi   = std::max(stats->humHi,   fs.humidity);
                    stats->tempLo  = std::min(stats->tempLo,  fs.tempC);
                    stats->tempHi  = std::max(stats->tempHi,  fs.tempC);
                    stats->slopeLo = std::min(stats->slopeLo, slope);
                    stats->slopeHi = std::max(stats->slopeHi, slope);
                }

                // El orden de la tabla es la PRIORIDAD de construcción: cada capa reclama.
                for (int li = 0; li < (int)table.layers.size(); ++li) {
                    const PropLayer& L = table.layers[li];
                    if (L.mesh.empty()) continue;                 // capa informativa: no instala
                    // `offered` se cuenta AQUÍ: es el turno real. Una capa con offered=0 no es que
                    // rechace nada — es que nunca llegó, porque otra de más prioridad hizo `break`.
                    if (stats) ++stats->offered[(size_t)li];
                    if (hM < 0.0f && !L.submerged) {              // sumergido: solo capas `submerged`
                        if (stats) ++stats->noSubmerged[(size_t)li];
                        continue;
                    }
                    const float mapD = L.densityMap.empty() ? 1.0f
                                                            : field.mapDensityAt(dir, L.densityMap);
                    const float cov  = L.coverage(fs, slope, mapD, zone, layer);
                    if (cov <= 0.0f) {
                        if (stats) {
                            ++stats->noCoverage[(size_t)li];
                            const uint32_t m = L.coverageFailMask(fs, slope, mapD, zone, layer);
                            if (m & PropLayer::FAIL_ZONE)  ++stats->failZone[(size_t)li];
                            if (m & PropLayer::FAIL_WHEN)  ++stats->failWhen[(size_t)li];
                            if (m & PropLayer::FAIL_HUM)   ++stats->failHum[(size_t)li];
                            if (m & PropLayer::FAIL_TEMP)  ++stats->failTemp[(size_t)li];
                            if (m & PropLayer::FAIL_SLOPE) ++stats->failSlope[(size_t)li];
                            if (m & PropLayer::FAIL_MAP)   ++stats->failMap[(size_t)li];
                        }
                        continue;
                    }

                    // Densidad local: huecos sin patrón (hash de la celda × peso de volumen).
                    const float r = PG::WhiteNode::hashFloat((int)hc, 200 + li, 0, params.seed);
                    if (r > L.density * cov) { if (stats) ++stats->noDensity[(size_t)li]; continue; }
                    if (stats) ++stats->placed[(size_t)li];

                    ScatteredProp pr;
                    pr.mesh       = L.mesh;
                    pr.layerIndex = li;
                    pr.dir        = dir;
                    pr.heightM    = hM;
                    pr.cellSeed   = hc;
                    pr.scale      = L.scaleMin + (L.scaleMax - L.scaleMin) *
                                    PG::WhiteNode::hashFloat((int)hc, 300 + li, 0, params.seed);
                    // Tinte determinista por celda (variedad de color sin geometría nueva).
                    const float tv = PG::WhiteNode::hashFloat((int)hc, 400 + li, 0, params.seed);
                    pr.tint = glm::vec3(0.85f + 0.30f * tv);
                    pr.state = 0;
                    out.push_back(pr);
                    break;   // una celda instala UNA capa (la de mayor prioridad que la acepte)
                }
            }
        }
    }
    return out;
}

}} // namespace Haruka::Planet
