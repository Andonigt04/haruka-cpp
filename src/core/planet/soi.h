#ifndef HARUKA_CORE_PLANET_SOI_H
#define HARUKA_CORE_PLANET_SOI_H
/**
 * @file soi.h
 * @brief Esferas de influencia y gravedad N-cuerpos para lo que el jugador SÍ toca.
 *
 * El reparto
 * ----------
 * Los cuerpos masivos van por Kepler analítico (`orbit.h`): exactos, sin deriva, imposibles de
 * hacer chocar, idénticos en todos los clientes. Eso es el ESCENARIO, y nadie percibe si Júpiter
 * está en un rail. Lo que sí se percibe es la nave, el jugador, los escombros: eso tiene que
 * integrar gravedad de verdad, poder estrellarse, ser capturado o escapar. Este fichero es el puente
 * entre las dos mitades.
 *
 * El bug que arregla, que no es pequeño
 * ------------------------------------
 * `IWorldProvider::gravBodies()` se llenaba desde `WorldSystem::getBodies()`, y esa lista filtra
 * `if (!flags.castLight) continue` — o sea que **solo contiene las estrellas**. El planeta sobre el
 * que caminas nunca estuvo en la lista de gravedad. Los dos caminos de la física lo parcheaban por
 * separado sumando a mano una gravedad radial de `9.81·R²/r²`, con sus comentarios reconociéndolo:
 *
 *     // `gravBodies()` solo trae los cuerpos que EMITEN luz (estrellas) → el planeta sobre el que
 *     // andas NO está ahí y habría gravedad ~0.
 *
 * Ese parche tiene dos consecuencias. La obvia: **todo cuerpo tiene la gravedad de la Tierra**, así
 * que una luna de 1700 km de radio te frena igual que un planeta — 9,81 en vez de los 2,6 m/s² que
 * le tocan por su masa. Y la de fondo: hay dos copias de la regla de gravedad, así que "cuánto tiras
 * hacia abajo" no tiene una definición, tiene dos.
 *
 * Aquí la gravedad es UNA función de la masa de los cuerpos, y la masa sale del radio con una
 * densidad declarada. Con la densidad media real de la Tierra (5514 kg/m³) un cuerpo de radio
 * terrestre da 9,82 m/s² en superficie, o sea que **el jugador en la Tierra no nota el cambio**: lo
 * que cambia es que ahora una luna pesa como una luna. Hay un test que fija ese número.
 *
 * Para qué sirve la SOI, aparte de la gravedad
 * -------------------------------------------
 * Sumar todos los cuerpos ya da la aceleración correcta sin necesidad de saber en qué esfera estás.
 * La SOI hace falta para las preguntas que NO son una suma:
 *
 *  · **¿En el marco de quién estoy?** El cuerpo sobre el que caminas tiene que seguir siendo el
 *    origen del marco local (ver la discusión del marco de referencia): la SOI es lo que lo decide.
 *  · **¿Con qué terreno colisiono, qué clipmap dibujo, qué cielo?**
 *  · **Cónicas parcheadas.** Dentro de una SOI la trayectoria de una nave es una cónica ANALÍTICA,
 *    así que se puede predecir y dibujar sin integrar — que es lo que hace usable un planificador de
 *    maniobras.
 *
 * El radio que se usa es el de LAPLACE, `a·(m/M)^(2/5)`, no el de Hill. Son cosas distintas y se
 * confunden a menudo: Hill (`a·(m/3M)^(1/3)`) es dónde una órbita es ESTABLE, y por eso `orbit.h` lo
 * usa para auditar el sistema; Laplace es dónde la gravedad del cuerpo DOMINA sobre la del padre, que
 * es lo que hace falta para decidir marco y cónica. Para la Tierra: Hill 1,5e9 m, Laplace 9,25e8 m.
 */

#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka {
namespace Planet {

/// Constante de gravitación. ⚠️ Tiene que ser la MISMA que `PhysicsEngine::gravitationalConstant`:
/// dos valores darían dos gravedades, y la del jugador y la de los escombros dejarían de casar.
inline constexpr double kGravConstant = 6.67430e-11;

/// Densidad media de la TIERRA. Es el proxy de masa cuando un cuerpo no declara la suya, y el valor
/// no es arbitrario: es el que hace que un cuerpo de radio terrestre dé 9,82 m/s² en superficie, o
/// sea el que preserva el comportamiento que el jugador ya tenía con el `9.81` a mano.
inline constexpr double kEarthMeanDensity = 5514.0;

/// Parámetro gravitacional estándar GM (m³/s²) de una esfera uniforme.
inline double standardGravParam(double radiusM, double densityKgM3 = kEarthMeanDensity) {
    return kGravConstant * densityKgM3 * (4.0 / 3.0) * 3.14159265358979323846
           * radiusM * radiusM * radiusM;
}

/// Gravedad en la superficie (m/s²) de un cuerpo de ese radio y densidad. Existe para poder ESCRIBIR
/// el test que fija los 9,82 m/s² de la Tierra sin reconstruir la fórmula en el propio test.
inline double surfaceGravity(double radiusM, double densityKgM3 = kEarthMeanDensity) {
    if (radiusM <= 0.0) return 0.0;
    return standardGravParam(radiusM, densityKgM3) / (radiusM * radiusM);
}

/**
 * @brief Radio de la esfera de influencia de LAPLACE: `a·(m/M)^(2/5)`.
 *
 * Es la distancia a la que la gravedad del cuerpo deja de dominar frente a la del padre. NO es el
 * radio de Hill (ver la cabecera): Hill responde "¿es estable una órbita aquí?", Laplace responde
 * "¿de quién es la gravedad que manda aquí?".
 *
 * @return 0 si el cuerpo no orbita nada (el cuerpo raíz: su influencia no tiene límite).
 */
inline double laplaceSoiRadius(double a, double mass, double parentMass) {
    if (a <= 0.0 || mass <= 0.0 || parentMass <= 0.0) return 0.0;
    return a * std::pow(mass / parentMass, 0.4);
}

/**
 * @brief Cuerpo masivo con su sitio en la jerarquía.
 *
 * Es un POD a propósito y SIN nombre: `Physics::GravBody` es un alias de este tipo, así que la física
 * y el motor comparten UNA definición de "cuerpo que ejerce gravedad" en vez de dos que hay que
 * convertir. Guarda `mass` y no GM porque es lo que traen los datos de escena; el GM se saca con
 * `kGravConstant * mass` donde hace falta.
 */
struct GravityBody {
    glm::dvec3  pos{0.0};        ///< centro, en coordenadas de mundo (m)
    double      mass   = 0.0;    ///< masa (kg)
    double      radius = 0.0;    ///< radio físico (m); 0 = puntual
    double      soiRadius = 0.0; ///< radio de Laplace (m). 0 = raíz (influencia sin límite)
    int         parent = -1;     ///< índice del cuerpo central, o -1 si es la raíz
};

/**
 * @brief Índice del cuerpo cuya gravedad DOMINA en `pos`, descendiendo la jerarquía.
 *
 * Arranca en la raíz y baja: si el punto cae dentro de la SOI de un hijo, el hijo manda y se repite
 * con él. El más PROFUNDO gana, que es la respuesta correcta — de pie en la Tierra dominan a la vez
 * el Sol (que contiene a todos) y la Tierra, y el marco que quieres es el de la Tierra.
 *
 * @return índice, o -1 si la lista está vacía.
 */
inline int dominantBodyIndex(const std::vector<GravityBody>& bodies, const glm::dvec3& pos) {
    if (bodies.empty()) return -1;
    // Raíz = el cuerpo sin padre con más GM. Si nadie declara raíz (escena a medio construir), se
    // toma el de más GM y ya: es mejor que devolver -1 y dejar al jugador sin gravedad.
    int cur = -1;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (bodies[i].parent >= 0) continue;
        if (cur < 0 || bodies[i].mass > bodies[(size_t)cur].mass) cur = (int)i;
    }
    if (cur < 0)
        for (size_t i = 0; i < bodies.size(); ++i)
            if (cur < 0 || bodies[i].mass > bodies[(size_t)cur].mass) cur = (int)i;

    // Bajar. El tope de iteraciones no es defensivo por costumbre: `parent` viene de datos de escena
    // y un ciclo (A padre de B, B padre de A) colgaría el frame entero.
    for (size_t guard = 0; guard < bodies.size(); ++guard) {
        int best = -1;
        double bestD2 = 0.0;
        for (size_t i = 0; i < bodies.size(); ++i) {
            const GravityBody& b = bodies[i];
            if (b.parent != cur || (int)i == cur || b.soiRadius <= 0.0) continue;
            const glm::dvec3 d = pos - b.pos;
            const double d2 = glm::dot(d, d);
            if (d2 >= b.soiRadius * b.soiRadius) continue;      // fuera de su influencia
            // Si dos SOI se solapasen (sistema mal generado: `validateOrbits` lo avisa), manda la más
            // cercana. Sin criterio, el resultado dependería del orden de la lista.
            if (best < 0 || d2 < bestD2) { best = (int)i; bestD2 = d2; }
        }
        if (best < 0) break;
        cur = best;
    }
    return cur;
}

/**
 * @brief Aceleración gravitatoria total en `pos`: suma newtoniana de todos los cuerpos.
 *
 * Se suman TODOS, no solo el dominante. A la altura de la superficie el dominante es ~10⁸ veces las
 * perturbaciones, así que da igual para el jugador; importa para una nave a mitad de camino, donde la
 * diferencia entre "solo el Sol" y "el Sol y los planetas" es la diferencia entre llegar y no. En
 * double el término pequeño no se pierde (en float sí: 10⁸ pasa del épsilon relativo de 10⁻⁷).
 *
 * @param skipIndex cuerpo a excluir (para no aplicarse un cuerpo su propia gravedad). -1 = ninguno.
 */
inline glm::dvec3 gravityAt(const std::vector<GravityBody>& bodies, const glm::dvec3& pos,
                            int skipIndex = -1) {
    glm::dvec3 acc(0.0);
    for (size_t i = 0; i < bodies.size(); ++i) {
        if ((int)i == skipIndex) continue;
        const GravityBody& b = bodies[i];
        if (b.mass <= 0.0) continue;
        const double gm = kGravConstant * b.mass;
        const glm::dvec3 d = b.pos - pos;
        const double r2 = glm::dot(d, d);
        if (r2 < 1e-12) continue;
        const double r = std::sqrt(r2);
        // Dentro del cuerpo la gravedad DECRECE linealmente hacia el centro (esfera uniforme: solo
        // atrae la masa interior). Sin esto, un objeto que atraviese el suelo recibe GM/r² → ∞ y sale
        // disparado; con un tunelado de un frame eso es la diferencia entre un empujón y perder el
        // cuerpo. Es física correcta, no un clamp.
        if (b.radius > 0.0 && r < b.radius)
            acc += d * (gm / (b.radius * b.radius * b.radius));
        else
            acc += d * (gm / (r2 * r));
    }
    return acc;
}

/// Velocidad de una órbita CIRCULAR a distancia r del centro de un cuerpo de parámetro `gm`.
inline double circularVelocity(double gm, double r) {
    return (r > 0.0 && gm > 0.0) ? std::sqrt(gm / r) : 0.0;
}
/// Velocidad de ESCAPE a distancia r. Por debajo, la trayectoria es cerrada: el cuerpo vuelve.
inline double escapeVelocity(double gm, double r) {
    return (r > 0.0 && gm > 0.0) ? std::sqrt(2.0 * gm / r) : 0.0;
}

/**
 * @brief Energía orbital específica: `v²/2 − GM/r`. Negativa = órbita cerrada (capturado), positiva =
 *        escapa, cero = parabólica.
 *
 * Es el criterio de "¿esto va a caer, orbitar o irse?" y de paso el invariante con el que se mide si
 * un integrador está derivando: en un problema de dos cuerpos tiene que quedarse constante.
 */
inline double specificOrbitalEnergy(double gm, double r, double speed) {
    if (r <= 0.0) return 0.0;
    return 0.5 * speed * speed - gm / r;
}

}} // namespace Haruka::Planet

#endif // HARUKA_CORE_PLANET_SOI_H
