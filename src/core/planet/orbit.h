#ifndef HARUKA_CORE_PLANET_ORBIT_H
#define HARUKA_CORE_PLANET_ORBIT_H
/**
 * @file orbit.h
 * @brief Órbitas keplerianas con ELEMENTOS QUE PRECESAN, y la prueba de que no chocan.
 *
 * El problema que resuelve
 * -----------------------
 * Hay dos formas de mover un sistema solar y las dos tienen un defecto:
 *
 *  · **Integrar N-cuerpos.** Es "de verdad", y es inservible aquí. Acumula deriva secular en el
 *    semieje mayor, así que en una partida larga los cuerpos acaban chocando o siendo eyectados —
 *    justo lo que no queremos. Y hay dos requisitos del motor que la descartan por sí solos: el DGS
 *    necesita que TODOS los clientes coincidan en dónde está cada planeta (dos sumas en float en
 *    otro orden divergen, y divergen para siempre), y el guardado necesita la posición en `t + 8 h`
 *    en O(1), no en 10⁶ pasos.
 *
 *  · **Kepler con elementos CONSTANTES.** Estable y exacto, pero *exactamente periódico*: la misma
 *    elipse, para siempre. Eso sí se percibe, y es lo que se lee como "va en un rail".
 *
 * La salida es que los planetas de verdad no hacen ninguna de las dos cosas: sus elementos
 * **precesan**. El perihelio de Mercurio avanza 5,6″ por siglo, los nodos regresan. Así que aquí no
 * se integran posiciones, se hace que los ELEMENTOS sean funciones lentas del tiempo:
 *
 *     ω(t) = ω₀ + ω̇·t     la elipse gira dentro de su plano   (precesión apsidal)
 *     Ω(t) = Ω₀ + Ω̇·t     el plano gira alrededor del polo    (precesión nodal)
 *     e(t) = e₀ + eAmp·sin(...)                               (oscilación secular acotada)
 *
 * Sigue siendo O(1) en `t`, sigue siendo idéntico en cada cliente, sigue sin deriva — y la órbita
 * **nunca se repite**. Es la diferencia entre un rail y un sistema que evoluciona.
 *
 * Por qué el no-choque es DEMOSTRABLE
 * -----------------------------------
 * Precesar no toca `a` y deja `e` acotada por construcción, así que el radio de cada órbita vive
 * SIEMPRE en [a(1−eMax), a(1+eMax)] — un intervalo fijo, conocido antes de arrancar. Comparar dos
 * intervalos es una comprobación, no una esperanza: `orbitsSeparated` la hace una vez en el
 * generador y vale para todo t. Integrando no existe ese intervalo, y de ahí que no se pueda
 * garantizar nada.
 *
 * Convención del marco
 * --------------------
 * Plano de referencia = XZ, con los ejes (e1, e2) = (+X, +Z) y las anomalías medidas desde +X. Se
 * elige así —y no con el polo en +Y como es habitual— para que **elementos todos a cero reproduzcan
 * exactamente la base (u,v) = ((1,0,0),(0,0,1)) que el motor ya usaba**: las escenas existentes se
 * comportan igual y hay un test que lo fija. El precio es que el polo del plano de referencia queda
 * en −Y, o sea que una inclinación positiva inclina hacia −Y. Es una convención, no un error, pero
 * hay que saberla al escribir una escena a mano.
 */

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Haruka {
namespace Planet {

/// 2π en double, para no repetir el literal ni depender de M_PI (que no es estándar).
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

/**
 * @brief Elementos orbitales, con sus tasas de precesión.
 *
 * Las tasas NO se guardan en rad/s sino como "cuántas órbitas tarda en dar una vuelta completa"
 * (`apsidalCycles`, `nodalCycles`). Eso es deliberado: atadas al periodo del propio cuerpo, escalan
 * solas con la compresión temporal del juego (un día dura 240 s aquí, o sea ~131 000× el real). Una
 * tasa en rad/s escrita a mano se quedaría absurda en cuanto se cambiara la escala de tiempo.
 *
 * Signo: positivo = avanza (progrado). Los nodos reales REGRESAN, así que su valor por defecto es
 * negativo. Cero = sin precesión (Kepler clásico, exactamente periódico).
 */
struct OrbitElements {
    double a       = 0.0;   ///< semieje mayor (m)
    double e       = 0.0;   ///< excentricidad media [0, 1)
    double incRad  = 0.0;   ///< inclinación sobre el plano de referencia (rad)
    double nodeRad = 0.0;   ///< longitud del nodo ascendente Ω₀ (rad)
    double argPRad = 0.0;   ///< argumento del periastro ω₀ (rad)
    double meanAnom0 = 0.0; ///< anomalía media en t=0 (rad)
    double period  = 0.0;   ///< periodo orbital (s); <= 0 = no orbita

    /// Órbitas por vuelta completa de la línea de apsides. 0 = sin precesión apsidal.
    double apsidalCycles = 0.0;
    /// Órbitas por vuelta completa de la línea de nodos. Negativo = regresión (lo normal).
    double nodalCycles   = 0.0;
    /// Amplitud de la oscilación secular de la excentricidad. Se acota para que e(t) ∈ [0, 0.95).
    double eAmp   = 0.0;
    /// Órbitas por ciclo de la oscilación de excentricidad. 0 = e constante.
    double eCycles = 0.0;

    /// Excentricidad MÁXIMA que este cuerpo puede alcanzar en cualquier t. Es la cota que usa la
    /// prueba de no-choque: sin ella no habría nada que comparar.
    double eMax() const { return std::min(0.95, std::abs(e) + std::abs(eAmp)); }
    /// Excentricidad MÍNIMA (nunca negativa: una elipse no tiene excentricidad negativa).
    double eMin() const { return std::max(0.0, std::abs(e) - std::abs(eAmp)); }
    /// Radios extremos alcanzables para todo t.
    double periapsisMin() const { return a * (1.0 - eMax()); }
    double apoapsisMax()  const { return a * (1.0 + eMax()); }
};

/**
 * @brief Base ortonormal del plano orbital a partir de (inclinación, nodo, argumento del periastro).
 * @param outU dirección del PERIASTRO (donde el cuerpo pasa más cerca del foco).
 * @param outV perpendicular a U en el plano, en el sentido del avance.
 *
 * Es la cadena de rotaciones clásica Ω → i → ω, escrita con vectores en vez de matrices porque solo
 * se necesitan dos columnas del resultado.
 */
inline void orbitBasis(double incRad, double nodeRad, double argPRad,
                       glm::dvec3& outU, glm::dvec3& outV) {
    const glm::dvec3 e1(1.0, 0.0, 0.0);
    const glm::dvec3 e2(0.0, 0.0, 1.0);
    const glm::dvec3 pole = glm::cross(e1, e2);          // = (0,-1,0): ver la nota de convención
    const double cO = std::cos(nodeRad), sO = std::sin(nodeRad);
    // Línea de nodos y su perpendicular DENTRO del plano de referencia.
    const glm::dvec3 nodeDir =  e1 * cO + e2 * sO;
    const glm::dvec3 inPlane = -e1 * sO + e2 * cO;
    // Inclinar el plano girando `inPlane` hacia el polo (la línea de nodos no se mueve: es el eje).
    const double cI = std::cos(incRad), sI = std::sin(incRad);
    const glm::dvec3 tilted = inPlane * cI + pole * sI;
    // Y dentro del plano ya inclinado, girar ω para colocar el periastro.
    const double cW = std::cos(argPRad), sW = std::sin(argPRad);
    outU =  nodeDir * cW + tilted * sW;
    outV = -nodeDir * sW + tilted * cW;
}

/**
 * @brief Inversa de `orbitBasis`: saca (i, Ω, ω) de una base (u,v) ya construida.
 *
 * Existe para MIGRAR sin romper nada. El motor derivaba (u,v) de la posición del cuerpo al cargar la
 * escena, así que los elementos no existían en ningún sitio; con esto una base heredada se convierte
 * en elementos una vez y a partir de ahí ya puede precesar.
 */
inline void orbitElementsFromBasis(const glm::dvec3& u, const glm::dvec3& v,
                                   double& outInc, double& outNode, double& outArgP) {
    const glm::dvec3 e1(1.0, 0.0, 0.0);
    const glm::dvec3 e2(0.0, 0.0, 1.0);
    const glm::dvec3 pole = glm::cross(e1, e2);
    // Normal del plano orbital. Su componente sobre el polo da la inclinación.
    const glm::dvec3 w = glm::normalize(glm::cross(u, v));
    outInc = std::acos(glm::clamp(glm::dot(w, pole), -1.0, 1.0));
    // La línea de nodos es la intersección de los dos planos = polo × normal. Degenera cuando los
    // planos coinciden (i = 0 o π): entonces Ω es libre y se fija a 0, y ω absorbe todo el giro.
    glm::dvec3 nodeDir = glm::cross(pole, w);
    const double nl = glm::length(nodeDir);
    if (nl < 1e-12) {
        outNode = 0.0;
        outArgP = std::atan2(glm::dot(u, e2), glm::dot(u, e1));
        return;
    }
    nodeDir /= nl;
    outNode = std::atan2(glm::dot(nodeDir, e2), glm::dot(nodeDir, e1));
    // ω = ángulo del periastro medido desde la línea de nodos, dentro del plano orbital.
    const glm::dvec3 inPlane = glm::cross(w, nodeDir);
    outArgP = std::atan2(glm::dot(u, inPlane), glm::dot(u, nodeDir));
}

/// Elementos EVALUADOS en un instante: es aquí donde deja de ser un rail.
struct OrbitState {
    double a = 0.0, e = 0.0;
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 0.0, 1.0};
};

/**
 * @brief Evalúa los elementos en `t` (segundos de simulación absolutos) aplicando la precesión.
 *
 * `a` no se toca NUNCA: es lo que mantiene el radio dentro de su intervalo y hace demostrable el
 * no-choque. Lo que evoluciona es la orientación de la elipse y su achatamiento.
 */
inline OrbitState orbitStateAt(const OrbitElements& el, double t) {
    OrbitState st;
    st.a = el.a;
    if (el.period <= 0.0) {
        st.e = el.e;
        orbitBasis(el.incRad, el.nodeRad, el.argPRad, st.u, st.v);
        return st;
    }
    const double orbits = t / el.period;                     // vueltas completadas (fraccionario)
    const double argP = el.argPRad + (el.apsidalCycles != 0.0
                        ? kTwoPi * orbits / el.apsidalCycles : 0.0);
    const double node = el.nodeRad + (el.nodalCycles != 0.0
                        ? kTwoPi * orbits / el.nodalCycles : 0.0);
    // La excentricidad oscila alrededor de su media, ACOTADA a [eMin, eMax]. El clamp no es un
    // parche: es lo que hace que el intervalo de radios de la cabecera sea cierto por construcción y
    // no por confianza en que la amplitud esté bien elegida.
    double e = el.e;
    if (el.eAmp != 0.0 && el.eCycles != 0.0)
        e += el.eAmp * std::sin(kTwoPi * orbits / el.eCycles);
    st.e = glm::clamp(e, el.eMin(), el.eMax());
    orbitBasis(el.incRad, node, argP, st.u, st.v);
    return st;
}

/**
 * @brief Posición relativa al foco en el instante `t`. Kepler resuelto por Newton (converge en <8
 *        iteraciones para e < 0.95; el caso hiperbólico no existe aquí, las órbitas son cerradas).
 */
inline glm::dvec3 orbitPositionAt(const OrbitElements& el, double t) {
    if (el.period <= 0.0 || el.a <= 0.0) return glm::dvec3(0.0);
    const OrbitState st = orbitStateAt(el, t);
    const double e = glm::clamp(st.e, 0.0, 0.95);
    const double M = el.meanAnom0 + kTwoPi * (t / el.period);
    double E = M;
    for (int i = 0; i < 8; ++i) {
        const double den = 1.0 - e * std::cos(E);
        E -= (E - e * std::sin(E) - M) / (std::abs(den) > 1e-12 ? den : 1e-12);
    }
    const double x = st.a * (std::cos(E) - e);
    const double y = st.a * std::sqrt(std::max(0.0, 1.0 - e * e)) * std::sin(E);
    return st.u * x + st.v * y;
}

// ── LA PRUEBA DE NO-CHOQUE ──────────────────────────────────────────────────────────────────────
//
// Dos criterios, y hacen falta los dos porque miden cosas distintas:
//
//  1. `orbitsSeparated` — GEOMÉTRICO. Los intervalos de radio no se solapan, así que las órbitas no
//     se cruzan jamás. Es condición SUFICIENTE y conservadora: dos órbitas con inclinaciones
//     distintas pueden solaparse en radio y no cruzarse nunca en 3D, y este test las rechazaría. Se
//     prefiere así — un falso rechazo cuesta regenerar, un falso positivo cuesta una colisión.
//
//  2. `mutualHillSeparation` — DINÁMICO. Que no se cruzen no basta: dos órbitas próximas se
//     perturban hasta cruzarse en sistemas reales. La separación en radios de Hill mutuos es el
//     criterio estándar de estabilidad a largo plazo, y con Δ > ~10 un sistema sobrevive escalas de
//     gigaaños. Es además un GENERADOR: muestrear los semiejes en progresión geométrica con razón
//     por encima de ese límite es lo único que produce sistemas estables — y es la razón de que los
//     sistemas reales se parezcan a Titius-Bode, que no es una regla estética sino un superviviente.

/// @return true si las dos órbitas NO pueden cruzarse para ningún t. `margin` es holgura relativa.
inline bool orbitsSeparated(const OrbitElements& inner, const OrbitElements& outer,
                            double margin = 0.05) {
    if (inner.period <= 0.0 || outer.period <= 0.0) return true;   // un cuerpo estático no cruza
    const double apoIn   = inner.apoapsisMax();
    const double periOut = outer.periapsisMin();
    return apoIn * (1.0 + margin) < periOut;
}

/// Masa por proxy de densidad uniforme, la MISMA convención que ya usa PlanetarySystem para la marea
/// (3000 kg/m³): el motor no guarda masas, y dos proxies distintos darían dos verdades.
inline double bodyMassFromRadius(double radiusM, double densityKgM3 = 3000.0) {
    return densityKgM3 * (4.0 / 3.0) * 3.14159265358979323846 * radiusM * radiusM * radiusM;
}

/**
 * @brief Separación en radios de Hill MUTUOS entre dos órbitas del mismo cuerpo central.
 *
 *   R_H = ((m₁+m₂)/(3·M))^(1/3) · (a₁+a₂)/2      Δ = (a₂−a₁)/R_H
 *
 * @return Δ. Por encima de ~10 el par es estable a escala de gigaaños; por debajo de ~3.5 se cruzan.
 */
inline double mutualHillSeparation(double a1, double m1, double a2, double m2, double centralMass) {
    if (centralMass <= 0.0 || a1 <= 0.0 || a2 <= 0.0) return 0.0;
    if (a1 > a2) { std::swap(a1, a2); std::swap(m1, m2); }
    const double rh = std::cbrt((m1 + m2) / (3.0 * centralMass)) * (a1 + a2) * 0.5;
    return (rh > 0.0) ? (a2 - a1) / rh : 0.0;
}

/// Radio de Hill de un cuerpo respecto a su padre: el techo del semieje de una luna estable
/// (≈0.4·R_Hill en la práctica, por eso `moonSemiMajorMax`).
inline double hillRadius(double a, double mass, double centralMass) {
    if (centralMass <= 0.0) return 0.0;
    return a * std::cbrt(mass / (3.0 * centralMass));
}
inline double moonSemiMajorMax(double a, double mass, double centralMass) {
    return 0.4 * hillRadius(a, mass, centralMass);
}

/**
 * @brief Tasas de precesión por defecto para un cuerpo, en "órbitas por vuelta".
 *
 * Los números están en el orden de magnitud de un sistema con vecinos masivos (apsides avanzando en
 * cientos de órbitas, nodos regresando algo más despacio) y se deshacen del semieje a propósito: lo
 * que importa es que NO sean múltiplos racionales sencillos entre cuerpos, porque entonces el
 * sistema entero volvería a ser periódico y se leería otra vez como un rail. Por eso se derivan de
 * la semilla del cuerpo con factores irracionales.
 */
inline void defaultPrecession(uint32_t seed, OrbitElements& el) {
    // Hash barato y determinista: el mismo cuerpo siempre precesa igual, en cualquier cliente.
    uint32_t h = seed * 2654435761u + 0x9E3779B9u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    const double r1 = (double)(h & 0xFFFFu) / 65535.0;
    const double r2 = (double)((h >> 16) & 0xFFFFu) / 65535.0;
    el.apsidalCycles =  160.0 + 220.0 * r1;    // avanza
    el.nodalCycles   = -(210.0 + 300.0 * r2);  // regresa (signo real)
    // La excentricidad oscila poco y SIEMPRE deja e(t) ≥ 0: la amplitud no puede pasar de la media.
    el.eAmp   = std::min(0.06, el.e * 0.5);
    el.eCycles = 380.0 + 260.0 * r1;
}

}} // namespace Haruka::Planet

#endif // HARUKA_CORE_PLANET_ORBIT_H
