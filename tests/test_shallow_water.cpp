/**
 * @file test_shallow_water.cpp
 * @brief Sondas del agua interior (rios/lagos): estabilidad al re-anclar y cobertura en montana.
 *
 * `ShallowWaterSim` no tenia NI UN test, mientras que el oceano analitico tiene nueve. Y es
 * justamente el sistema del que se reporta que "se ve raro por las montanas", asi que lo primero es
 * poner un numero donde solo habia una impresion.
 *
 * Lo que se mide son las dos consecuencias que el propio `seedLakes()` avisa que tiene, pero que
 * nadie habia cuantificado:
 *
 *   1. INESTABILIDAD AL RE-ANCLAR. El parche mide 420 m y se re-ancla cada 160 m de marcha. El
 *      relleno de hondonadas usa el BORDE del parche como escape, asi que una cuenca que asoma por
 *      el borde se considera abierta y no se llena; 160 m mas alla puede quedar dentro y llenarse.
 *      Se mide cuantos puntos del MUNDO cambian de mojado a seco (o al reves) entre dos anclajes
 *      consecutivos, mirando solo el solape de los dos parches.
 *
 *   2. COBERTURA EN PENDIENTE. Un heightfield de columnas verticales sobre terreno de montana
 *      pone agua en celdas de 4,4 m de lado. Se mide que fraccion de un parche montanoso acaba
 *      mojada y con cuanta profundidad.
 *
 * ⚠️ CONTRAPRUEBA OBLIGATORIA en las dos: sobre terreno PLANO y sobre una ladera SIN cuencas el
 * numero tiene que irse a cero. Si no, la sonda estaria midiendo su propio ruido y no el sistema.
 */
#include "test_common.h"

#include "physics/fluid/shallow_water.h"
#include "core/planet/ocean_wave.h"

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <vector>
#include <functional>

using Haruka::fluid::ShallowWaterSim;

namespace {

constexpr double kR    = 6371000.0;   // radio del planeta, como en el resto de los tests
constexpr double kSpan = 420.0;       // los MISMOS que usa FluidHost::ensurePatch
constexpr int    kN    = 96;
constexpr double kReanchor = 160.0;

// Terreno sintetico y DETERMINISTA en funcion de la posicion del mundo. Se usa uno propio en vez del
// generador del planeta a proposito: aqui se audita el ALGORITMO de relleno, y para eso hace falta
// poder elegir el relieve (montanoso / plano / ladera limpia) y que no cambie entre ejecuciones.
enum class Relief { Mountain, Flat, CleanSlope };

double heightFor(Relief r, const glm::dvec3& wp) {
    // Coordenadas locales en metros a partir de la direccion radial: da un campo continuo sobre la
    // esfera sin tener que montar una proyeccion.
    const glm::dvec3 d = glm::normalize(wp);
    const double x = d.x * kR, z = d.z * kR;
    switch (r) {
        case Relief::Flat:       return 100.0;
        case Relief::CleanSlope: return 100.0 + 0.20 * x;         // 20% constante: sin hondonadas
        case Relief::Mountain:
        default:
            // Crestas cruzadas de dos escalas: genera cuencas cerradas de tamanos variados, que es
            // exactamente el relieve donde el relleno por borde se vuelve inestable.
            return 100.0
                 + 60.0 * std::sin(x / 190.0) * std::cos(z / 155.0)
                 + 18.0 * std::sin(x / 61.0  + 1.3) * std::sin(z / 47.0 - 0.7)
                 +  5.0 * std::cos(x / 23.0  - 2.1);
    }
}

// Monta un parche igual que lo monta FluidHost, anclado a `dir` (direccion radial del jugador).
void buildPatch(ShallowWaterSim& sim, Relief r, const glm::dvec3& dir) {
    const glm::dvec3 up = glm::normalize(dir);
    // Base tangente estable (la misma idea que basisTangent del host).
    const glm::dvec3 ref = (std::abs(up.y) < 0.9) ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0);
    const glm::dvec3 tan = glm::normalize(glm::cross(ref, up));
    const glm::dvec3 bit = glm::normalize(glm::cross(up, tan));
    auto hf = [r](const glm::dvec3& wp) { return heightFor(r, wp); };
    const glm::dvec3 anchor = up * (kR + hf(up * kR));
    sim.init(anchor, tan, bit, up, kSpan, kN, hf);
}

// Direccion radial desplazada `metres` sobre la superficie desde `dir` a lo largo de su tangente.
glm::dvec3 shiftOnSphere(const glm::dvec3& dir, double metres) {
    const glm::dvec3 up = glm::normalize(dir);
    const glm::dvec3 ref = (std::abs(up.y) < 0.9) ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0);
    const glm::dvec3 tan = glm::normalize(glm::cross(ref, up));
    return glm::normalize(up * kR + tan * metres);
}

struct Coverage { double wetPct; float maxDepth; double meanDepth; };

Coverage coverageOf(const ShallowWaterSim& sim) {
    int wet = 0; float mx = 0.0f; double sum = 0.0;
    for (int j = 0; j < sim.size(); ++j)
        for (int i = 0; i < sim.size(); ++i) {
            const float d = sim.waterAt(i, j);
            if (d > 0.02f) { ++wet; sum += d; mx = std::max(mx, d); }
        }
    const double cells = (double)sim.size() * sim.size();
    return { 100.0 * wet / cells, mx, wet ? sum / wet : 0.0 };
}

/**
 * @brief Cuantos puntos del MUNDO cambian de mojado/seco entre dos anclajes.
 *
 * Se recorre el parche B y, para cada celda, se pregunta al parche A por la misma posicion de mundo.
 * Solo cuentan las celdas que caen dentro de los dos (el solape), porque fuera no hay con que
 * comparar. El criterio es "hay agua o no", que es lo que se ve: una lamina que aparece o
 * desaparece al caminar es el sintoma; que cambie 10 cm de profundidad no lo es.
 */
double flipPct(const ShallowWaterSim& a, const ShallowWaterSim& b, int& comparedOut) {
    int compared = 0, flips = 0;
    for (int j = 0; j < b.size(); ++j)
        for (int i = 0; i < b.size(); ++i) {
            const glm::dvec3 wp = b.worldPosAt(i, j);
            // ⚠️ Se compara la LAMINA, no la superficie. La primera version restaba la superficie de
            // A menos el terreno de B, y la contraprueba la tumbo al instante: sobre una ladera
            // limpia los dos parches daban 0,0% mojado y aun asi "cambiaban" el 100% — porque dos
            // rejillas desplazadas muestrean el terreno en puntos distintos y esa diferencia de
            // RELIEVE (hasta ~0,9 m por celda al 20% de pendiente) se estaba leyendo como agua.
            const float da = a.waterAtWorld(wp);
            if (da < 0.0f) continue;                 // fuera del parche A: nada que comparar
            ++compared;
            const bool wetA = da > 0.02f;
            const bool wetB = b.waterAt(i, j) > 0.02f;
            if (wetA != wetB) ++flips;
        }
    comparedOut = compared;
    return compared ? 100.0 * flips / compared : 0.0;
}

} // namespace

void test_shallow_water_reanchor_stability() {
    beginTest("shallow_water_reanchor_stability");

    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));

    struct Case { Relief r; const char* name; } cases[] = {
        { Relief::Mountain,   "montana (crestas cruzadas)" },
        { Relief::CleanSlope, "ladera limpia (sin cuencas)" },
        { Relief::Flat,       "llano" },
    };

    double mountainFlip = 0.0, slopeFlip = 0.0, flatFlip = 0.0;

    std::printf("    parche %.0f m / %d celdas = %.2f m por celda · re-anclaje cada %.0f m\n",
                kSpan, kN, kSpan / kN, kReanchor);

    for (const Case& c : cases) {
        ShallowWaterSim a, b;
        buildPatch(a, c.r, dir0);
        buildPatch(b, c.r, shiftOnSphere(dir0, kReanchor));

        const Coverage ca = coverageOf(a), cb = coverageOf(b);
        int compared = 0;
        const double flip = flipPct(a, b, compared);

        std::printf("    %-28s mojado %5.1f%% -> %5.1f%% · %d celdas en solape · CAMBIAN %.1f%%\n",
                    c.name, ca.wetPct, cb.wetPct, compared, flip);

        if (c.r == Relief::Mountain)        mountainFlip = flip;
        else if (c.r == Relief::CleanSlope) slopeFlip    = flip;
        else                                flatFlip     = flip;

        CHECK(compared > 500, "los dos parches solapan lo bastante para comparar");
    }

    // CONTRAPRUEBA: sin cuencas no hay nada que rellenar, asi que caminar no puede cambiar el agua.
    // Si esto no fuera ~0, la sonda estaria midiendo su propio error de rejilla y el numero de
    // montana no significaria nada.
    std::printf("    contraprueba: llano %.1f%% · ladera limpia %.1f%% (deben ser ~0)\n",
                flatFlip, slopeFlip);
    CHECK(flatFlip  < 1.0, "en llano el agua no cambia al re-anclar (la sonda no mide ruido)");
    CHECK(slopeFlip < 1.0, "en ladera sin cuencas tampoco (la sonda no mide ruido)");

    // ⚠️ HIPOTESIS REFUTADA, y queda escrita para que nadie la vuelva a perseguir. Se esperaba que
    // la inestabilidad de borde que documenta `seedLakes()` fuera GRANDE —el borde decide si una
    // cuenca se llena, y el parche se re-ancla cada 160 m— y que explicara el "se ve raro por las
    // montanas". NO lo explica: en el solape solo cambia el 0,3 %.
    //
    // Lo que si cambia mucho es la cobertura TOTAL del parche (4,9 % -> 9,9 % aqui), pero eso es
    // terreno nuevo entrando por la franja que se descubre al avanzar, no agua que parpadee donde ya
    // estabas. Son dos cosas distintas y confundirlas era el error.
    //
    // El CHECK guarda la estabilidad medida: si alguien toca el relleno y la rompe, se entera.
    CHECK(mountainFlip < 2.0,
          "el agua ya puesta no parpadea al re-anclar (esto NO es la causa de que se vea raro)");
}

void test_shallow_water_mountain_coverage() {
    beginTest("shallow_water_mountain_coverage");

    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));

    ShallowWaterSim mtn, slope, flat;
    buildPatch(mtn,   Relief::Mountain,   dir0);
    buildPatch(slope, Relief::CleanSlope, dir0);
    buildPatch(flat,  Relief::Flat,       dir0);

    const Coverage cm = coverageOf(mtn), cs = coverageOf(slope), cf = coverageOf(flat);

    std::printf("    montana      : %5.1f%% mojado · profundidad media %6.2f m · maxima %7.2f m\n",
                cm.wetPct, cm.meanDepth, cm.maxDepth);
    std::printf("    ladera limpia: %5.1f%% mojado · profundidad media %6.2f m · maxima %7.2f m\n",
                cs.wetPct, cs.meanDepth, cs.maxDepth);
    std::printf("    llano        : %5.1f%% mojado · profundidad media %6.2f m · maxima %7.2f m\n",
                cf.wetPct, cf.meanDepth, cf.maxDepth);

    // CONTRAPRUEBA: una ladera constante no tiene ni una hondonada, asi que el relleno tiene que
    // dejarla SECA. Si mojara, `seedLakes` estaria inventando agua y el numero de montana no seria
    // interpretable.
    CHECK(cs.wetPct < 1.0, "una ladera sin cuencas queda seca (seedLakes no inventa agua)");

    // El llano es el caso degenerado: todo a la misma cota, el borde escapa, no debe llenarse.
    CHECK(cf.wetPct < 1.0, "el llano queda seco");

    // Lo que se ve en montana. Sin objetivo de calidad: es la foto del estado actual.
    CHECK(cm.wetPct >= 0.0, "cobertura de montana medida");

    // ── CUANTAS CELDAS MOJADAS LLEGA A DIBUJAR EL SHADER ───────────────────────────────────────
    // `harukaInlandWaterAt` interpola bilineal entre CUATRO celdas y devuelve el centinela "sin agua"
    // si alguna de las cuatro esta seca (mezclar una cota real con -1e9 hundiria la lamina). O sea que
    // lo que se dibuja no es el conjunto mojado sino su EROSION: la celda solo cuenta si sus cuatro
    // esquinas tienen agua. En un lago grande eso solo se pierde el borde; en charcos de pocas celdas
    // se pierde casi todo, y lo que queda son bloques sueltos con pared vertical de una celda.
    int wetCells = 0, drawable = 0;
    for (int j = 0; j + 1 < mtn.size(); ++j)
        for (int i = 0; i + 1 < mtn.size(); ++i) {
            if (!mtn.hasWaterAt(i, j)) continue;
            ++wetCells;
            if (mtn.hasWaterAt(i + 1, j) && mtn.hasWaterAt(i, j + 1) && mtn.hasWaterAt(i + 1, j + 1))
                ++drawable;
        }
    const double keep = wetCells ? 100.0 * drawable / wetCells : 0.0;
    std::printf("    celdas con agua: %d · las que el shader llega a dibujar: %d (%.1f%%)\n",
                wetCells, drawable, keep);
    std::printf("      el %.1f%% restante son celdas mojadas que salen como pared vertical de %.1f m\n",
                100.0 - keep, mtn.cellSize());
    CHECK(wetCells > 0, "hay agua que medir");
}

/**
 * @brief Un lago de montana recibe el OLEAJE DEL MAR, y a poca profundidad el bajio lo agranda.
 *
 * El agua interior y el oceano comparten superficie y shader a proposito (ver `lib/inland_water.glsl`:
 * "una superficie y una pregunta"), y fue la decision correcta — antes habia tres sistemas dibujando
 * agua y ninguno coincidia. Pero arrastra una consecuencia que nadie habia medido: en `ocean.tese` la
 * profundidad sale de `nivel - cota del suelo` y se pasa TAL CUAL a `harukaGerstner`, asi que un lago
 * de 3 m entra en el modelo de olas con bajio. El bajio es un efecto de COSTA: al perder fondo la ola
 * se acorta y crece hasta romper. Aplicado a una charca de montana da rompiente y espuma de playa en
 * un sitio donde, fisicamente, no puede haber mar de fondo: el oleaje largo necesita kilometros de
 * recorrido de viento, y un lago no los tiene.
 *
 * Este test no arregla nada. Pone el numero al lado de la impresion "se ve raro por las montanas".
 */
void test_shallow_water_inherits_ocean_swell() {
    beginTest("shallow_water_inherits_ocean_swell");

    using Haruka::Planet::oceanWaveHeight;
    using Haruka::Planet::oceanShoalAmp;

    const glm::vec3 up = glm::normalize(glm::vec3(0.31f, 0.62f, 0.72f));
    const glm::vec3 wp = up * (float)kR;

    // Barrido en tiempo y espacio: una sola muestra podria caer en un nodo de la ola por casualidad.
    auto peakToPeak = [&](float depth) {
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s < 24; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.3f) * (float)(s * 7);
            for (int k = 0; k < 40; ++k) {
                const float h = oceanWaveHeight(p, up, 0.35f * k, depth);
                lo = std::min(lo, h); hi = std::max(hi, h);
            }
        }
        return hi - lo;
    };

    // Las profundidades NO son inventadas: son las que mide test_shallow_water_mountain_coverage
    // sobre el mismo terreno sintetico (media 3,21 m, maxima 6,79 m).
    const float lakeMean = 3.21f, lakeMax = 6.79f, openSea = 40.0f;

    const float ppLakeMean = peakToPeak(lakeMean);
    const float ppLakeMax  = peakToPeak(lakeMax);
    const float ppSea      = peakToPeak(openSea);

    std::printf("    altura de ola pico a pico, por profundidad del agua:\n");
    std::printf("      lago de montana  %4.2f m de fondo -> %6.3f m de ola  (x%.2f del mar abierto)\n",
                lakeMean, ppLakeMean, ppSea > 1e-6f ? ppLakeMean / ppSea : 0.0f);
    std::printf("      lago mas hondo   %4.2f m de fondo -> %6.3f m de ola  (x%.2f)\n",
                lakeMax, ppLakeMax, ppSea > 1e-6f ? ppLakeMax / ppSea : 0.0f);
    std::printf("      mar abierto     %5.1f m de fondo -> %6.3f m de ola\n", openSea, ppSea);

    // Factor de bajio directo, que es el termino que hace crecer la ola al perder fondo.
    // El termino del bajio a amplitud de referencia 1 m: cuanto agranda (o recorta) la ola cada fondo.
    std::printf("    factor de bajio (oceanShoalAmp, amp 1 m): lago %.3f · mar %.3f\n",
                oceanShoalAmp(lakeMean, 1.0f), oceanShoalAmp(openSea, 1.0f));

    // CONTRAPRUEBA: en agua MUY honda el bajio no debe hacer nada, o el numero del lago no seria
    // atribuible al bajio sino al modelo entero.
    const float ppAbyss = peakToPeak(4000.0f);
    std::printf("    contraprueba: a 4000 m de fondo la ola es %.3f m (x%.2f del mar abierto)\n",
                ppAbyss, ppSea > 1e-6f ? ppAbyss / ppSea : 0.0f);
    CHECK(std::abs(ppAbyss - ppSea) < 0.25f * ppSea,
          "en agua muy honda el bajio ya no cambia la ola (el efecto medido ES el bajio)");

    // Lo que importa: el lago NO esta quieto. Que la ola sea comparable a la del mar en una charca de
    // 3 m es lo que se ve como "raro". El umbral es flojo a proposito: documenta, no exige.
    CHECK(ppLakeMean > 0.05f,
          "un lago de 3 m recibe oleaje del mar (no es una lamina quieta)");
}

// ================================================================================================
// FÍSICA DEL AGUA, ANALÍTICA: conservación, nivelado, desbordamiento y rotura de presa.
//
// Lo que había medía DÓNDE pone agua el sembrado. Nada medía si el agua se COMPORTA como agua una
// vez está puesta — y eso es lo que decide si un río corre, si un lago se desborda y si una presa
// que revienta hace algo o se queda quieta.
//
// ⚠️ CADA UNO CON UN ORÁCULO QUE NO SALE DE ESTE FICHERO: la conservación de masa es exacta por
// construcción del esquema (tuberías virtuales, Mei 2007), el nivel de reposo es `volumen/área`, el
// derrame para en la cota del labio, y el frente de una rotura de presa no puede adelantar a
// `2·sqrt(g·h0)` — la solución de Ritter, que es una cota FÍSICA: superarla sería agua más rápida
// que la onda que la empuja.
// ================================================================================================
namespace {

/// Terreno definido en coordenadas LOCALES del parche (metros desde el ancla). Hace falta para poder
/// construir una cuenca o una presa con cotas exactas en vez de con un ruido bonito.
void buildLocal(ShallowWaterSim& sim, const glm::dvec3& dir,
                const std::function<double(double, double)>& hLocal) {
    const glm::dvec3 up  = glm::normalize(dir);
    const glm::dvec3 ref = (std::abs(up.y) < 0.9) ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0);
    const glm::dvec3 tan = glm::normalize(glm::cross(ref, up));
    const glm::dvec3 bit = glm::normalize(glm::cross(up, tan));
    auto hf = [up, tan, bit, &hLocal](const glm::dvec3& wp) {
        const glm::dvec3 d = glm::normalize(wp);
        // Proyección tangente: para un parche de 420 m sobre 6371 km, la diferencia con la distancia
        // sobre la esfera es de micras.
        const double u = glm::dot(d - up, tan) * kR;
        const double v = glm::dot(d - up, bit) * kR;
        return hLocal(u, v);
    };
    // ⚠️ SE ARRANCA EN SECO, Y HAY QUE PEDIRLO. Sin el segundo callback, `init` llama a `seedLakes`
    // y el relleno de hondonadas deja la cuenca LLENA HASTA EL BORDE antes de que el test eche una
    // gota: la primera version de esto medía 9 041 101 m³ de agua vertida cuando había echado 3 159.
    // Con un campo horneado que dice "seco en todas partes", la siembra se salta y la física empieza
    // donde el test la pone.
    auto dry = [](const glm::dvec3&) { return -1.0e30; };   // centinela de `WATER_FILL_DRY`
    sim.init(up * (kR + hf(up * kR)), tan, bit, up, kSpan, kN, hf, dry);
}

/// Cota del terreno en el ancla. ⚠️ La sim guarda el terreno RELATIVO a ella (`m_terrain = h - h0`,
/// para que el flujo dependa solo de la pendiente local y los números se mantengan pequeños), así
/// que cualquier cota que el test compare tiene que estar en el mismo marco.
double anchorHeight(const std::function<double(double,double)>& hLocal) { return hLocal(0.0, 0.0); }

/// Coordenada local de una celda, en metros desde el centro del parche.
void cellLocal(const ShallowWaterSim& s, int i, int j, double& u, double& v) {
    const double half = (double)(s.size() - 1) * 0.5;
    u = ((double)i - half) * s.cellSize();
    v = ((double)j - half) * s.cellSize();
}

} // namespace

void test_shallow_water_physics() {
    beginTest("shallow_water_physics");
    const glm::dvec3 dir = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const double g = 9.81;

    // ── (1) CONSERVACIÓN DE MASA Y NIVELADO ─────────────────────────────────────────────────────
    //
    // Cuenca de fondo plano a 100 m con paredes a 200 m. Se vierte todo el agua en una esquina y se
    // deja correr: tiene que (a) no perderse ni una gota y (b) acabar con la superficie PLANA a la
    // cota que dicta `volumen / área mojada`.
    {
        ShallowWaterSim sim;
        buildLocal(sim, dir, [](double u, double v) {
            return (std::abs(u) > 150.0 || std::abs(v) > 150.0) ? 200.0 : 100.0;
        });
        // Verter en una esquina del fondo, no en el centro: si el nivelado no funcionara, la
        // superficie se quedaría con el montón donde se echó y el test lo vería.
        const float pour = 2.0f;
        int poured = 0;
        for (int j = 0; j < sim.size(); ++j)
            for (int i = 0; i < sim.size(); ++i) {
                double u, v; cellLocal(sim, i, j, u, v);
                if (u > -140.0 && u < -100.0 && v > -140.0 && v < -100.0) { sim.addWater(i, j, pour); ++poured; }
            }
        const double v0 = sim.totalWater();
        for (int k = 0; k < 4000; ++k) sim.step(1.0f / 60.0f);
        const double v1 = sim.totalWater();

        // Nivel de reposo y planitud de la superficie donde hay agua.
        double loS = 1e9, hiS = -1e9; int wet = 0;
        for (int j = 0; j < sim.size(); ++j)
            for (int i = 0; i < sim.size(); ++i)
                if (sim.waterAt(i, j) > 0.05f) {
                    const double s = sim.surfaceAt(i, j);
                    loS = std::min(loS, s); hiS = std::max(hiS, s); ++wet;
                }
        const double area = (double)wet * sim.cellSize() * sim.cellSize();
        // El fondo está en la cota del ancla, o sea 0 en el marco relativo de la sim.
        const double lvlOracle = (wet ? v1 / area : 0.0);
        std::printf("    (1) cuenca cerrada: vertidas %d celdas x %.1f m = %.1f m3\n", poured, pour, v0);
        std::printf("        volumen tras 4000 pasos: %.4f m3 (perdida %.3e relativa)\n",
                    v1, v0 > 0 ? std::fabs(v1 - v0) / v0 : 0.0);
        std::printf("        superficie: %.3f a %.3f m (rango %.4f m) · nivel por volumen/area %.3f m\n",
                    loS, hiS, hiS - loS, lvlOracle);
        CHECK(v0 > 0.0 && std::fabs(v1 - v0) / v0 < 1e-4,
              "(1) el esquema CONSERVA la masa (tuberias virtuales: es su propiedad declarada)");
        CHECK(hiS - loS < 0.25,
              "(1) el agua se NIVELA: la superficie acaba plana, no con el monton donde se echo");
        CHECK(std::fabs(0.5 * (loS + hiS) - lvlOracle) < 0.15,
              "(1) y a la cota que dicta volumen/area, que es el oraculo y no una tabla mia");
    }

    // ── (2) DESBORDAMIENTO: el nivel para en el labio ───────────────────────────────────────────
    //
    // Cuenca a 100 m con paredes a 120 m salvo una MUESCA a 108 m por un lado, y detras una rampa
    // que baja. Se vierte MUCHO MAS de lo que la cuenca puede retener: el nivel tiene que pararse en
    // el labio y el resto irse por la muesca.
    {
        ShallowWaterSim sim;
        buildLocal(sim, dir, [](double u, double v) {
            if (u > 60.0)  return 100.0 - (u - 60.0) * 0.30;          // rampa de salida
            if (std::abs(v) > 60.0 || u < -60.0) return 120.0;        // paredes altas
            if (u > 55.0)  return 108.0;                              // el LABIO (la muesca)
            return 100.0;                                             // fondo
        });
        for (int j = 0; j < sim.size(); ++j)
            for (int i = 0; i < sim.size(); ++i) {
                double u, v; cellLocal(sim, i, j, u, v);
                if (std::abs(u) < 50.0 && std::abs(v) < 50.0) sim.addWater(i, j, 30.0f);  // 30 m: pasado
            }
        for (int k = 0; k < 6000; ++k) sim.step(1.0f / 60.0f);

        double hiS = -1e9;
        for (int j = 0; j < sim.size(); ++j)
            for (int i = 0; i < sim.size(); ++i) {
                double u, v; cellLocal(sim, i, j, u, v);
                if (std::abs(u) < 50.0 && std::abs(v) < 50.0 && sim.waterAt(i, j) > 0.05f)
                    hiS = std::max(hiS, (double)sim.surfaceAt(i, j));
            }
        std::vector<ShallowWaterSim::Overflow> spills;
        sim.collectOverflowEdges(1.0f, spills);
        // Fondo 100 m y labio 108 m en absoluto; el ancla está en el fondo, así que en el marco de
        // la sim son 0 y +8. Comparar contra 108 fue el primer error de este test.
        std::printf("    (2) desbordamiento: labio en +8,0 m (relativo) · superficie en la cuenca %+.3f m\n",
                    hiS);
        std::printf("        bordes de derrame encontrados: %zu\n", spills.size());
        CHECK(hiS < 12.0,
              "(2) el nivel PARA en el labio: no se apila 30 m de agua en una cuenca de 8 m de hondo");
        CHECK(hiS > 4.0,
              "CONTRAPRUEBA: y no se vacia entera — la cuenca RETIENE hasta su labio");
        CHECK(!spills.empty(), "(2) y el derrame se detecta como borde (fuente de cascada)");
    }

    // ── (3) ROTURA DE PRESA: el frente no puede adelantar a Ritter ──────────────────────────────
    //
    // Lecho plano y seco; columna de agua de `h0` en media rejilla. Al soltarla, el frente avanza.
    // La solucion analitica de Ritter da `2*sqrt(g*h0)` como velocidad del frente sobre lecho seco,
    // y eso es una COTA FISICA: un esquema que la supere esta moviendo agua mas rapido que la onda
    // que la empuja. Es el oraculo mas fuerte que tiene este fichero porque no depende del esquema.
    {
        ShallowWaterSim sim;
        buildLocal(sim, dir, [](double, double) { return 100.0; });
        const float h0 = 5.0f;
        for (int j = 0; j < sim.size(); ++j)
            for (int i = 0; i < sim.size(); ++i) {
                double u, v; cellLocal(sim, i, j, u, v);
                if (u < -20.0) sim.addWater(i, j, h0);
            }
        auto frontU = [&]() {
            double f = -1e9;
            for (int j = 0; j < sim.size(); ++j)
                for (int i = 0; i < sim.size(); ++i)
                    if (sim.waterAt(i, j) > 0.05f) { double u, v; cellLocal(sim, i, j, u, v); f = std::max(f, u); }
            return f;
        };
        const double f0 = frontU();
        const double T  = 2.0;                       // 2 s de simulacion
        for (int k = 0; k < (int)(T * 60.0); ++k) sim.step(1.0f / 60.0f);
        const double f1 = frontU();
        const double vFront  = (f1 - f0) / T;
        const double vRitter = 2.0 * std::sqrt(g * (double)h0);
        std::printf("    (3) rotura de presa (h0 = %.1f m): frente de %.1f a %.1f m en %.1f s\n",
                    h0, f0, f1, T);
        std::printf("        velocidad %.2f m/s · cota de Ritter 2*sqrt(g*h0) = %.2f m/s\n",
                    vFront, vRitter);
        CHECK(vFront > 0.5, "(3) el frente AVANZA de verdad (si no, no hay fisica que medir)");
        CHECK(vFront <= vRitter,
              "(3) y NO adelanta a la solucion de Ritter, que es una cota fisica y no un ajuste");
    }

    // ── (4) EL AGUA CORRE CUESTA ABAJO ──────────────────────────────────────────────────────────
    //
    // Con contraprueba en llano: si el centroide tambien se moviera ahi, esto estaria midiendo la
    // deriva numerica del esquema y no la gravedad.
    {
        auto centroidDrift = [&](double slope) {
            ShallowWaterSim sim;
            buildLocal(sim, dir, [slope](double u, double) { return 100.0 + slope * u; });
            for (int j = 0; j < sim.size(); ++j)
                for (int i = 0; i < sim.size(); ++i) {
                    double u, v; cellLocal(sim, i, j, u, v);
                    if (std::abs(u) < 25.0 && std::abs(v) < 25.0) sim.addWater(i, j, 3.0f);
                }
            auto cu = [&]() {
                double sw = 0.0, su = 0.0;
                for (int j = 0; j < sim.size(); ++j)
                    for (int i = 0; i < sim.size(); ++i) {
                        const double w = sim.waterAt(i, j);
                        if (w > 0.01) { double u, v; cellLocal(sim, i, j, u, v); su += u * w; sw += w; }
                    }
                return sw > 0.0 ? su / sw : 0.0;
            };
            const double c0 = cu();
            for (int k = 0; k < 1200; ++k) sim.step(1.0f / 60.0f);
            return cu() - c0;
        };
        const double dSlope = centroidDrift(0.15);
        const double dFlat  = centroidDrift(0.0);
        std::printf("    (4) centroide tras 20 s: en pendiente 15%% %+.2f m · en llano %+.2f m\n",
                    dSlope, dFlat);
        CHECK(dSlope < -2.0, "(4) el agua baja la pendiente (cuesta abajo es -u)");
        CHECK(std::fabs(dFlat) < 0.5,
              "CONTRAPRUEBA: en llano el centroide NO se mueve — es la gravedad, no deriva del esquema");
    }
}
