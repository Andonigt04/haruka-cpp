#pragma once
/**
 * @file SkyEphemeris.h
 * @brief EL SOL QUIETO Y EL PLANETA QUE SE MUEVE: donde estan el Sol y la Luna vistos desde el planeta.
 *
 * Andoni (17-10): "haz que el sol este quieto y que el planeta se mueva y la luna". Antes el dia era un
 * apaño: `WorldSystem` hacia girar el SOL alrededor del planeta sobre un eje inclinado cualquiera, y la
 * Luna giraba por su orbita sin salir ni ponerse cada dia (no la afectaba el giro del planeta).
 *
 * El modelo, en un marco INERCIAL con el Sol quieto en el origen y la ecliptica en el plano XZ:
 *  - el planeta ORBITA el Sol (un año) y GIRA sobre su eje (un dia), con el eje inclinado `tilt`
 *    respecto a la normal de la ecliptica y FIJO en el espacio → estaciones;
 *  - la Luna orbita el planeta (un mes sidereo) en un plano inclinado `moonIncl` sobre la ecliptica,
 *    cuyo nodo retrocede (18,6 años reales, escalados igual que el resto).
 *
 * ⚠️ Y LO QUE NO SE HACE: mover de verdad las coordenadas del planeta. La fisica (Jolt), el terreno,
 * el agua y la red viven en el marco DEL PLANETA, que no gira; moverlo a 30 km/s lo romperia todo. Aqui
 * se calcula todo en el marco inercial y se pasa al del planeta que gira. Lo que se ve (por donde sale
 * el Sol, la fase de la Luna, la marea) es exactamente lo mismo que si el planeta se moviera.
 *
 * Marco del planeta = el del juego: el eje de giro es +Y (el polo norte del mapa, `lat = asin(y)`), y
 * los ejes son de MANO DERECHA, como la camara (`Character::updateCamera`: derecha = frente x arriba).
 * Mirando al norte, tu derecha —el ESTE que se VE— es -Z. El giro es prograda (antihorario visto desde
 * el norte): el suelo va hacia -Z y el Sol sale por ahi y se pone por +Z.
 *
 * ⚠️ EL MAPA ESTA EN ESPEJO respecto a eso: la longitud del juego es `atan2(z, x)` y crece hacia +Z, que
 * es el OESTE que se ve. Es un defecto anterior de la convencion de longitud (horneados, shaders, red);
 * aqui manda lo que ve el jugador: mirando al norte, el Sol sale por la DERECHA. La primera version
 * siguio al mapa y en el juego el amanecer salia por el oeste (captura del 17-10, rumbo 145).
 *
 * Todo es funcion del tiempo de simulacion (`PlanetarySystem::simulationTime`, el reloj compartido):
 * sin acumular `dt`, igual en todos los clientes.
 */
#include <cmath>
#include <glm/glm.hpp>

namespace Haruka::World::Planet {

struct SkyClock {
    double dayS        = 86400.0;    ///< dia SOLAR (s): de mediodia a mediodia (24 h)
    double yearDays    = 365.25;     ///< año, en dias solares
    double monthDays   = 27.32166;   ///< mes SIDEREO de la Luna, en dias solares
    double nodeDays    = 6798.38;    ///< retroceso del nodo lunar (18,6 años), en dias
    double tiltRad     = 0.40910;    ///< inclinacion del eje (23,44°)
    double moonInclRad = 0.08980;    ///< inclinacion de la orbita lunar sobre la ecliptica (5,145°)
    double spin0       = 0.0;        ///< giro del planeta en t = 0 (rad)
    double orbit0      = 0.0;        ///< longitud del planeta en su orbita en t = 0 (rad)
    double moon0       = 0.0;        ///< argumento de la Luna en su orbita en t = 0 (rad)
};

/// Dia SIDEREO: el giro respecto a las estrellas. Con orbita y giro en el mismo sentido, un dia
/// solar es un poco mas largo (el planeta tiene que girar algo mas para volver a tener el Sol encima).
inline double siderealDayS(const SkyClock& c) { return c.dayS * c.yearDays / (c.yearDays + 1.0); }

/// Giro del planeta (rad) en `t`.
inline double planetSpinAt(const SkyClock& c, double t) {
    return c.spin0 + 2.0 * 3.14159265358979323846 * t / siderealDayS(c);
}

/// Giro de MANO DERECHA de `a` rad alrededor de +Y (lleva +X a (cos a, 0, -sin a)).
inline glm::dvec3 rotY(double a, const glm::dvec3& v) {
    const double c = std::cos(a), s = std::sin(a);
    return glm::dvec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}
/// Giro de mano derecha de `a` rad alrededor de +X.
inline glm::dvec3 rotX(double a, const glm::dvec3& v) {
    const double c = std::cos(a), s = std::sin(a);
    return glm::dvec3(v.x, c * v.y - s * v.z, s * v.y + c * v.z);
}

/// Del marco inercial (ecliptica XZ, normal +Y) al del PLANETA que gira `spin`. Primero la inclinacion
/// (el eje del planeta pasa a +Y), despues el giro. El planeta gira `+spin` alrededor de +Y; lo que esta
/// quieto en el espacio se ve girar `-spin`: hacia +Z, el oeste que se ve.
inline glm::dvec3 planetFromInertial(const SkyClock& c, double spin, const glm::dvec3& v) {
    return rotY(-spin, rotX(c.tiltRad, v));
}

/// Longitud del planeta en su orbita (rad). Prograda: el mismo sentido que el giro (+Y, mano derecha).
inline double planetOrbitAngleAt(const SkyClock& c, double t) {
    return c.orbit0 + 2.0 * 3.14159265358979323846 * t / (c.yearDays * c.dayS);
}

/// Direccion planeta → Sol en el marco INERCIAL (unitaria). El Sol esta quieto en el origen.
inline glm::dvec3 sunDirInertial(const SkyClock& c, double t) {
    return -rotY(planetOrbitAngleAt(c, t), glm::dvec3(1.0, 0.0, 0.0));
}

/// Direccion planeta → Luna en el marco INERCIAL (unitaria): orbita prograda inclinada, nodo que
/// retrocede.
///
/// ⚠️ El angulo EN EL PLANO se mide desde el nodo, y el nodo se mueve: es la longitud MENOS el nodo.
/// Con el mes sidereo puesto directamente ahi, la Luna daba la vuelta respecto al nodo en 27,32 dias
/// (y respecto a las estrellas en 27,43): el mes de las fases salia de 29,66 dias en vez de 29,53
/// (`sky_moon`). Asi la vuelta al nodo es el mes draconitico (27,21) y la de las estrellas el sidereo.
inline glm::dvec3 moonDirInertial(const SkyClock& c, double t) {
    const double tau = 2.0 * 3.14159265358979323846;
    const double node = -tau * t / (c.nodeDays * c.dayS);
    const double u = c.moon0 + tau * t / (c.monthDays * c.dayS) - node;
    // En su plano (prograda), el plano inclinado sobre X, y el plano girado al nodo.
    return rotY(node, rotX(c.moonInclRad, rotY(u, glm::dvec3(1.0, 0.0, 0.0))));
}

struct SkyState {
    double     spin = 0.0;       ///< giro del planeta (rad)
    glm::dvec3 sunDir{0, 1, 0};  ///< planeta → Sol, en el marco del planeta (unitaria)
    glm::dvec3 moonDir{1, 0, 0}; ///< planeta → Luna, en el marco del planeta (unitaria)
};

/// El cielo en `t`, en el marco del planeta (el del juego). `spinOverride >= 0` fija el giro (capturas
/// reproducibles: `HARUKA_DAY_ANGLE`) sin parar el resto.
inline SkyState skyAt(const SkyClock& c, double t, double spinOverride = -1.0) {
    SkyState s;
    s.spin = (spinOverride >= 0.0) ? spinOverride : planetSpinAt(c, t);
    s.sunDir  = planetFromInertial(c, s.spin, sunDirInertial(c, t));
    s.moonDir = planetFromInertial(c, s.spin, moonDirInertial(c, t));
    return s;
}

/// GM de los cuerpos reales (m³/s²): la marea los necesita de verdad, no un radio por densidad.
inline constexpr double kGmSun  = 1.32712440018e20;
inline constexpr double kGmMoon = 4.9048695e12;

}  // namespace Haruka::World::Planet
