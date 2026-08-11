// ================================================================================================
// Órbitas con elementos PRECESANTES (core/planet/orbit.h).
//
// Lo que este banco fija no es "la fórmula está escrita": son las tres PROPIEDADES por las que se
// eligió este diseño frente a integrar N-cuerpos, y cada una se comprueba de una forma en la que
// podría fallar de verdad.
//
//  1. COMPATIBILIDAD — elementos a cero reproducen la base (u,v) que el motor ya usaba, bit a bit en
//     la práctica. Sin esto, migrar habría movido todos los planetas de todas las escenas.
//  2. NO HAY DERIVA — el radio se queda dentro de su intervalo tras cientos de miles de órbitas. Es
//     justo lo que un integrador no puede prometer, y es lo que hace demostrable el no-choque.
//  3. NO ES UN RAIL — con precesión la órbita NO se repite tras un periodo, y sin ella sí. Las dos
//     mitades importan: la segunda prueba que el test detecta periodicidad de verdad.
// ================================================================================================
#include "test_common.h"
#include "core/planet/orbit.h"

#include <cmath>
#include <cstdio>

using namespace Haruka::Planet;

namespace {
double maxComp(const glm::dvec3& d) {
    return std::max(std::abs(d.x), std::max(std::abs(d.y), std::abs(d.z)));
}
} // namespace

void test_orbit_basis() {
    beginTest("orbit_basis");

    // (1) COMPATIBILIDAD: los defaults históricos del motor eran u=(1,0,0), v=(0,0,1). Con todos los
    // elementos a cero la base tiene que salir EXACTAMENTE esa, o migrar habría desplazado cada
    // planeta de cada escena existente.
    {
        glm::dvec3 u, v;
        orbitBasis(0.0, 0.0, 0.0, u, v);
        CHECK(maxComp(u - glm::dvec3(1, 0, 0)) < 1e-15, "elementos a 0 -> u = (1,0,0) (compat)");
        CHECK(maxComp(v - glm::dvec3(0, 0, 1)) < 1e-15, "elementos a 0 -> v = (0,0,1) (compat)");
    }

    // La base es ortonormal para cualquier combinación de elementos. Si no lo fuera, la órbita se
    // deformaría al precesar (una elipse con ejes no perpendiculares no es una elipse kepleriana).
    for (int i = 0; i < 12; ++i) {
        const double inc  = -1.4 + 0.24 * i;
        const double node = 0.37 * i;
        const double argP = -2.1 + 0.51 * i;
        glm::dvec3 u, v;
        orbitBasis(inc, node, argP, u, v);
        CHECK(std::abs(glm::length(u) - 1.0) < 1e-14, "u unitario");
        CHECK(std::abs(glm::length(v) - 1.0) < 1e-14, "v unitario");
        CHECK(std::abs(glm::dot(u, v)) < 1e-14,       "u perpendicular a v");
    }

    // Ida y vuelta base -> elementos -> base. Es lo que permite migrar una escena que solo tenía la
    // base: si la inversa no fuera exacta, convertir movería la órbita.
    for (int i = 0; i < 10; ++i) {
        const double inc  = 0.15 + 0.25 * i;      // fuera de la degeneración i=0 (nodo indefinido)
        const double node = -2.6 + 0.55 * i;
        const double argP = 0.4 * i - 1.0;
        glm::dvec3 u, v;
        orbitBasis(inc, node, argP, u, v);
        double i2, n2, w2;
        orbitElementsFromBasis(u, v, i2, n2, w2);
        glm::dvec3 u2, v2;
        orbitBasis(i2, n2, w2, u2, v2);
        CHECK(maxComp(u - u2) < 1e-12, "ida y vuelta reconstruye u");
        CHECK(maxComp(v - v2) < 1e-12, "ida y vuelta reconstruye v");
    }

    // Caso DEGENERADO: con i=0 la línea de nodos no existe (los planos coinciden) y Ω es libre. La
    // inversa tiene que devolver una base equivalente, no NaN — y es el caso más común, porque una
    // escena que no declara inclinación cae justo aquí.
    {
        glm::dvec3 u, v;
        orbitBasis(0.0, 0.0, 0.9, u, v);
        double i2, n2, w2;
        orbitElementsFromBasis(u, v, i2, n2, w2);
        CHECK(std::isfinite(i2) && std::isfinite(n2) && std::isfinite(w2),
              "i=0 no produce NaN (nodo degenerado)");
        glm::dvec3 u2, v2;
        orbitBasis(i2, n2, w2, u2, v2);
        CHECK(maxComp(u - u2) < 1e-12, "i=0 reconstruye u pese al nodo libre");
        CHECK(maxComp(v - v2) < 1e-12, "i=0 reconstruye v pese al nodo libre");
    }
}

void test_orbit_no_drift() {
    beginTest("orbit_no_drift");

    OrbitElements el;
    el.a = 1.496e11; el.e = 0.2; el.period = 31557600.0;
    el.incRad = 0.3; el.nodeRad = 1.1; el.argPRad = 0.4;
    defaultPrecession(12345u, el);
    CHECK(el.apsidalCycles > 0.0, "la precesion por defecto no es nula (si no, seria un rail)");
    CHECK(el.nodalCycles   < 0.0, "los nodos REGRESAN (signo real)");

    // (2) NO HAY DERIVA. Se recorren 400 000 órbitas y el radio tiene que seguir dentro del intervalo
    // que declara `periapsisMin/apoapsisMax`. Un integrador acumularía error en `a` y se saldría; aquí
    // `a` no se toca nunca, así que la cota es estructural. Es la propiedad que hace demostrable el
    // no-choque, y por eso se comprueba con un número de órbitas absurdo, no con dos.
    const double rMin = el.periapsisMin(), rMax = el.apoapsisMax();
    double worstLow = 1e300, worstHigh = 0.0;
    for (int i = 0; i < 40000; ++i) {
        const double t = el.period * (10.0 * i + 0.37);   // 400 000 órbitas, en fases dispares
        const double r = glm::length(orbitPositionAt(el, t));
        worstLow  = std::min(worstLow, r);
        worstHigh = std::max(worstHigh, r);
    }
    CHECK(worstLow  >= rMin * (1.0 - 1e-9), "tras 400k orbitas el radio no baja del periastro minimo");
    CHECK(worstHigh <= rMax * (1.0 + 1e-9), "tras 400k orbitas el radio no pasa del apoastro maximo");

    // La excentricidad efectiva se queda en su intervalo por construcción (el clamp de `orbitStateAt`),
    // y nunca negativa: una elipse con e<0 no existe y el resolvedor de Kepler daría basura.
    for (int i = 0; i < 5000; ++i) {
        const OrbitState st = orbitStateAt(el, el.period * 0.113 * i);
        CHECK(st.e >= 0.0 && st.e <= el.eMax() + 1e-12, "e(t) dentro de [0, eMax]");
        if (st.e < 0.0) break;   // no inundar la salida si falla
    }

    // Y sigue siendo función PURA de t: dos evaluaciones del mismo instante dan lo mismo. Es lo que
    // hace que todos los clientes del DGS coincidan y que se pueda saltar tiempo en O(1).
    const glm::dvec3 p1 = orbitPositionAt(el, 987654321.0);
    const glm::dvec3 p2 = orbitPositionAt(el, 987654321.0);
    CHECK(maxComp(p1 - p2) == 0.0, "la posicion es funcion pura de t (bit a bit)");
}

void test_orbit_precession() {
    beginTest("orbit_precession");

    OrbitElements el;
    el.a = 4.0e10; el.e = 0.15; el.period = 1000.0;
    el.incRad = 0.2; el.nodeRad = 0.0; el.argPRad = 0.0;

    // (3a) SIN precesión la órbita es EXACTAMENTE periódica. Esto no es solo documentación del caso
    // base: es lo que valida el test siguiente. Si la posición no se repitiera ni aquí, el de abajo
    // pasaría por la razón equivocada.
    {
        OrbitElements rail = el;   // apsidalCycles/nodalCycles/eAmp a cero
        const glm::dvec3 p0 = orbitPositionAt(rail, 0.0);
        const glm::dvec3 pT = orbitPositionAt(rail, rail.period * 50.0);
        CHECK(maxComp(p0 - pT) < el.a * 1e-12, "sin precesion: la orbita se repite (es un rail)");
    }

    // (3b) CON precesión NO se repite: tras 50 vueltas el cuerpo pasa por la misma anomalía media
    // pero la elipse se ha girado, así que está en otro sitio. Y el desvío tiene que ser GRANDE
    // (>0.1% del semieje), no ruido numérico — si fuera del orden del épsilon, la precesión estaría
    // puesta pero sería invisible, que es el fallo silencioso que de verdad importa.
    {
        OrbitElements pre = el;
        defaultPrecession(777u, pre);
        const glm::dvec3 p0 = orbitPositionAt(pre, 0.0);
        const glm::dvec3 pT = orbitPositionAt(pre, pre.period * 50.0);
        const double moved = glm::length(p0 - pT);
        CHECK(moved > el.a * 1e-3, "con precesion: 50 vueltas NO devuelven al mismo punto");
    }

    // El plano gira de verdad (precesión nodal), no solo la elipse dentro del plano: la normal cambia.
    {
        OrbitElements pre = el;
        pre.nodalCycles = 120.0;
        const OrbitState s0 = orbitStateAt(pre, 0.0);
        const OrbitState s1 = orbitStateAt(pre, pre.period * 30.0);
        const glm::dvec3 n0 = glm::cross(s0.u, s0.v);
        const glm::dvec3 n1 = glm::cross(s1.u, s1.v);
        CHECK(glm::length(n0 - n1) > 1e-3, "la precesion nodal mueve el PLANO de la orbita");
    }

    // Determinismo de las tasas: la misma semilla da la misma precesión. Sin esto, dos clientes del
    // DGS precesarían distinto y sus planetas se separarían para siempre.
    {
        OrbitElements a = el, b = el;
        defaultPrecession(42u, a);
        defaultPrecession(42u, b);
        CHECK(a.apsidalCycles == b.apsidalCycles && a.nodalCycles == b.nodalCycles,
              "defaultPrecession es determinista por semilla");
        OrbitElements c = el;
        defaultPrecession(43u, c);
        CHECK(c.apsidalCycles != a.apsidalCycles,
              "semillas distintas -> tasas distintas (el sistema no vuelve a ser periodico)");
    }
}

void test_orbit_no_collision() {
    beginTest("orbit_no_collision");

    // Par SEPARADO: la Tierra y Marte con excentricidades reales. Tiene que pasar.
    OrbitElements earth; earth.a = 1.496e11; earth.e = 0.017; earth.period = 3.156e7;
    OrbitElements mars;  mars.a  = 2.279e11; mars.e  = 0.093; mars.period  = 5.936e7;
    CHECK(orbitsSeparated(earth, mars), "Tierra/Marte: no se cruzan");

    // Par que SE CRUZA: el apoastro del interior pasa del periastro del exterior. Es el caso que la
    // auditoría tiene que atrapar, y el que un integrador convertiría en colisión al cabo de horas.
    OrbitElements inner; inner.a = 1.0e11; inner.e = 0.5; inner.period = 1.0e7;   // apo = 1.5e11
    OrbitElements outer; outer.a = 1.4e11; outer.e = 0.0; outer.period = 2.0e7;   // peri = 1.4e11
    CHECK(!orbitsSeparated(inner, outer), "orbitas que se cruzan: RECHAZADAS");

    // Y el criterio tiene que usar la e MÁXIMA, no la media. Una órbita casi circular cuya
    // excentricidad OSCILA hasta cruzar al vecino debe rechazarse igual — es exactamente el agujero
    // que abriría la precesión si el test mirara solo `e`.
    {
        OrbitElements a; a.a = 1.0e11; a.e = 0.05; a.period = 1.0e7;
        OrbitElements b; b.a = 1.2e11; b.e = 0.0;  b.period = 2.0e7;
        CHECK(orbitsSeparated(a, b), "con e media baja: separadas");
        a.eAmp = 0.30; a.eCycles = 400.0;          // oscila hasta e=0.35 -> apo = 1.35e11
        CHECK(!orbitsSeparated(a, b),
              "la oscilacion de e las cruza -> RECHAZADAS (el test usa eMax, no e)");
    }

    // Un cuerpo estático (period <= 0) no orbita, así que no puede cruzar nada.
    {
        OrbitElements st;   // period = 0
        CHECK(orbitsSeparated(st, earth), "cuerpo estatico: nunca cruza");
    }

    // --- Separación en radios de Hill mutuos ---
    const double sunMass   = bodyMassFromRadius(6.963e8, 1408.0);   // densidad solar real
    const double earthMass = bodyMassFromRadius(6.371e6, 5514.0);
    const double marsMass  = bodyMassFromRadius(3.390e6, 3933.0);
    const double delta = mutualHillSeparation(earth.a, earthMass, mars.a, marsMass, sunMass);
    CHECK(delta > 10.0, "Tierra/Marte estan a mas de 10 radios de Hill mutuos (sistema real)");

    // Simétrico: el orden de los argumentos no puede cambiar la respuesta.
    const double deltaSwap = mutualHillSeparation(mars.a, marsMass, earth.a, earthMass, sunMass);
    CHECK(std::abs(delta - deltaSwap) < 1e-9 * delta, "la separacion de Hill es simetrica");

    // Dos cuerpos pegados dan Δ pequeño: es la señal de "no chocan pero el par queda apretado".
    const double deltaTight = mutualHillSeparation(1.00e11, earthMass, 1.01e11, earthMass, sunMass);
    CHECK(deltaTight < 10.0, "dos orbitas pegadas: Delta por debajo del umbral recomendado");
    CHECK(deltaTight > 0.0,  "Delta positivo");

    // Un semieje de luna estable está por debajo del radio de Hill de su planeta. La Luna real está a
    // 3.84e8 m y el radio de Hill de la Tierra es ~1.5e9: tiene que caber.
    const double moonMax = moonSemiMajorMax(earth.a, earthMass, sunMass);
    CHECK(moonMax > 3.84e8, "la Luna real cabe en el tope de semieje estable de la Tierra");
    CHECK(moonMax < hillRadius(earth.a, earthMass, sunMass), "el tope esta por debajo del radio de Hill");
}

// ================================================================================================
// ESFERAS DE INFLUENCIA Y GRAVEDAD N-CUERPOS (core/planet/soi.h)
//
// Aquí lo que se fija son NÚMEROS DEL SISTEMA REAL, no propiedades abstractas: la SOI de la Tierra
// mide 9,25e8 m y su gravedad de superficie 9,81 m/s². Un test así falla si la fórmula se escribe con
// el exponente equivocado (2/5 y 1/3 se confunden constantemente entre Laplace y Hill) o si alguien
// cambia la densidad del proxy sin darse cuenta de que decide la gravedad que siente el jugador.
// ================================================================================================
#include "core/planet/soi.h"

void test_soi_gravity() {
    beginTest("soi_gravity");

    // (1) EL NÚMERO QUE NO PUEDE MOVERSE. El motor tenía `9.81` a mano para el planeta activo; ahora
    // la gravedad sale de la masa. Con la densidad media terrestre, un cuerpo de radio terrestre da
    // 9,81 m/s² — o sea que el jugador en la Tierra NO nota el cambio. Si este test falla, se acaba de
    // alterar el salto y la caída de todo el juego.
    const double gEarth = surfaceGravity(6.371e6);
    CHECK(std::abs(gEarth - 9.81) < 0.05, "un cuerpo de radio terrestre da 9.81 m/s2 en superficie");

    // Y lo que el parche hacía mal: una luna NO tiene la gravedad de la Tierra. Con densidad terrestre
    // uniforme sale algo más que los 1,62 m/s² reales (la Luna es menos densa), pero tiene que quedar
    // MUY por debajo: es la diferencia que el 9.81 fijo borraba.
    const double gMoon = surfaceGravity(1.737e6);
    CHECK(gMoon < gEarth * 0.35, "una luna pesa como una luna, no como la Tierra");
    CHECK(gMoon > 1.0, "y aun asi tiene gravedad apreciable");

    // (2) LA SOI DE LA TIERRA. Valor real: 9,25e8 m. Es el test que distingue Laplace de Hill — con el
    // exponente de Hill (1/3) saldría 1,5e9, un 62% más, y la transición de marco ocurriría donde no
    // toca.
    const double earthMass = 5.972e24, sunMass = 1.989e30;
    const double soi = laplaceSoiRadius(1.496e11, earthMass, sunMass);
    CHECK(std::abs(soi - 9.25e8) < 0.1e8, "la SOI de la Tierra mide 9.25e8 m (Laplace, no Hill)");
    CHECK(soi < hillRadius(1.496e11, earthMass, sunMass),
          "la SOI de Laplace es MENOR que el radio de Hill (son cosas distintas)");
    // La Luna está dentro: si no lo estuviera, al ir a la Luna el marco no cambiaría a la Tierra.
    CHECK(3.844e8 < soi, "la Luna orbita DENTRO de la SOI de la Tierra");

    // (3) QUIÉN MANDA. Sol en el origen, Tierra a 1 UA con su SOI.
    std::vector<GravityBody> sys;
    sys.push_back({ glm::dvec3(0.0),                   sunMass,   6.963e8, 0.0, -1 });   // raíz
    sys.push_back({ glm::dvec3(1.496e11, 0.0, 0.0),    earthMass, 6.371e6, soi,  0 });
    const glm::dvec3 earthPos = sys[1].pos;

    CHECK(dominantBodyIndex(sys, earthPos + glm::dvec3(0, 6.4e6, 0)) == 1,
          "de pie en la Tierra manda la Tierra, no el Sol");
    CHECK(dominantBodyIndex(sys, earthPos + glm::dvec3(0, 3.8e8, 0)) == 1,
          "en la orbita lunar sigue mandando la Tierra");
    CHECK(dominantBodyIndex(sys, earthPos + glm::dvec3(0, 2.0e9, 0)) == 0,
          "mas alla de la SOI manda el Sol");
    CHECK(dominantBodyIndex(sys, glm::dvec3(3.0e11, 0, 0)) == 0, "lejos de todo manda el Sol");
    CHECK(dominantBodyIndex(std::vector<GravityBody>{}, glm::dvec3(0.0)) == -1,
          "lista vacia -> -1, sin reventar");

    // La transición es LIMPIA: monótona en el radio, sin ida y vuelta. Si oscilara, el marco de
    // referencia parpadearía al cruzar la frontera y con él el terreno y el cielo.
    {
        int flips = 0, prev = dominantBodyIndex(sys, earthPos + glm::dvec3(0, 1.0e7, 0));
        for (int i = 1; i <= 400; ++i) {
            const double rr = 1.0e7 + (3.0e9 - 1.0e7) * (i / 400.0);
            const int d = dominantBodyIndex(sys, earthPos + glm::dvec3(0, rr, 0));
            if (d != prev) { ++flips; prev = d; }
        }
        CHECK(flips == 1, "cruzar la SOI cambia de cuerpo UNA vez (sin parpadeo)");
    }

    // Un ciclo en la jerarquía (dato de escena corrupto) no puede colgar el frame.
    {
        std::vector<GravityBody> bad;
        bad.push_back({ glm::dvec3(0.0),            1e30, 1e8, 1e12, 1 });
        bad.push_back({ glm::dvec3(1e10, 0.0, 0.0), 1e24, 1e6, 1e11, 0 });
        const int d = dominantBodyIndex(bad, glm::dvec3(1e10, 0, 0));
        CHECK(d >= 0 && d < 2, "una jerarquia ciclica termina y devuelve algo valido");
    }

    // (4) LA SUMA. En la superficie de la Tierra el resultado tiene que ser la gravedad terrestre: la
    // contribución del Sol es real pero 1600 veces menor, y en double no se pierde ninguna de las dos.
    {
        const glm::dvec3 g = gravityAt(sys, earthPos + glm::dvec3(0, 6.371e6, 0));
        CHECK(std::abs(glm::length(g) - 9.8) < 0.2, "la suma en superficie da la gravedad terrestre");
        CHECK(glm::dot(glm::normalize(g), glm::dvec3(0, -1, 0)) > 0.999, "y apunta al centro");
    }

    // (5) DENTRO DEL CUERPO la gravedad DECRECE hacia el centro (esfera uniforme: solo atrae la masa
    // interior). Con GM/r² a secas, un objeto que atraviese el suelo un frame recibe una aceleración
    // que tiende a infinito y sale disparado — se pierde el cuerpo. Esto no es un clamp, es la física
    // correcta, y por eso se comprueba que sea LINEAL.
    {
        const double R = 6.371e6;
        const double gSurf  = glm::length(gravityAt(sys, earthPos + glm::dvec3(0, R, 0)));
        const double gHalf  = glm::length(gravityAt(sys, earthPos + glm::dvec3(0, R * 0.5, 0)));
        const double gTenth = glm::length(gravityAt(sys, earthPos + glm::dvec3(0, R * 0.1, 0)));
        CHECK(std::abs(gHalf  - gSurf * 0.5) < gSurf * 0.02, "a R/2 la gravedad es la mitad (lineal)");
        CHECK(std::abs(gTenth - gSurf * 0.1) < gSurf * 0.02, "a R/10 es un decimo (lineal)");
        CHECK(gTenth < gSurf, "no explota al acercarse al centro");
    }

    // `skipIndex`: un cuerpo no se aplica su propia gravedad. Sin esto, meter los planetas en la lista
    // haría que cada uno se atrajera a sí mismo con r → 0.
    {
        const glm::dvec3 all = gravityAt(sys, earthPos + glm::dvec3(0, 1e7, 0));
        const glm::dvec3 noE = gravityAt(sys, earthPos + glm::dvec3(0, 1e7, 0), 1);
        CHECK(glm::length(noE) < glm::length(all) * 0.01, "skipIndex excluye el cuerpo indicado");
    }
}

void test_soi_orbital_energy() {
    beginTest("soi_orbital_energy");

    const double gm = kGravConstant * 5.972e24;         // GM de la Tierra
    const double r  = 6.771e6;                          // 400 km de altitud (la ISS)
    const double vc = circularVelocity(gm, r);
    CHECK(std::abs(vc - 7670.0) < 60.0, "la velocidad orbital a 400 km es ~7.67 km/s (ISS)");
    const double ve = escapeVelocity(gm, 6.371e6);
    CHECK(std::abs(ve - 11186.0) < 100.0, "la velocidad de escape en superficie es 11.19 km/s");
    CHECK(escapeVelocity(gm, r) > circularVelocity(gm, r) * 1.41,
          "escapar cuesta raiz de 2 veces mas que orbitar");

    // El criterio de "¿esto cae, orbita o se va?" — es lo que decide el destino de una nave y lo que
    // hace que el N-cuerpos de los cuerpos pequeños sea interesante en vez de decorativo.
    CHECK(specificOrbitalEnergy(gm, r, vc)       < 0.0, "a velocidad circular: orbita CERRADA");
    CHECK(specificOrbitalEnergy(gm, r, vc * 0.5) < 0.0, "mas despacio: sigue cerrada (cae)");
    CHECK(specificOrbitalEnergy(gm, r, escapeVelocity(gm, r) * 1.1) > 0.0,
          "por encima del escape: se va");
    CHECK(std::abs(specificOrbitalEnergy(gm, r, escapeVelocity(gm, r))) < 1e-6 * gm / r,
          "exactamente a la velocidad de escape: energia cero (parabolica)");

    // (6) LA PRUEBA DE FUEGO DEL N-CUERPOS: integrar una órbita circular y comprobar que se CIERRA. Es
    // lo que valida que la gravedad y el integrador juntos sirven para una nave.
    std::vector<GravityBody> one;
    one.push_back({ glm::dvec3(0.0), 5.972e24, 6.371e6, 0.0, -1 });
    glm::dvec3 pos(r, 0.0, 0.0), vel(0.0, vc, 0.0);
    const double period = kTwoPi * std::sqrt(r * r * r / gm);
    const double dt = period / 4000.0;
    const double e0 = specificOrbitalEnergy(gm, glm::length(pos), glm::length(vel));
    double rMin = 1e300, rMax = 0.0;
    // Leapfrog (kick-drift-kick): simpléctico, así que la energía OSCILA pero no deriva. Un Euler
    // explícito con el mismo paso perdería el radio de forma monótona — es justo la diferencia que
    // motiva que los planetas NO se integren.
    vel += gravityAt(one, pos) * (dt * 0.5);
    for (int i = 0; i < 4000 * 10; ++i) {              // 10 vueltas
        pos += vel * dt;
        vel += gravityAt(one, pos) * dt;
        const double rr = glm::length(pos);
        rMin = std::min(rMin, rr);
        rMax = std::max(rMax, rr);
    }
    const double e1 = specificOrbitalEnergy(gm, glm::length(pos), glm::length(vel));
    CHECK(rMax / rMin - 1.0 < 0.01, "10 vueltas con leapfrog: la orbita se mantiene circular al 1%");
    CHECK(std::abs((e1 - e0) / e0) < 1e-4, "y la energia especifica se conserva (simplectico)");
    CHECK(std::abs(glm::length(pos) - r) < r * 0.01, "vuelve al radio de partida");
}
