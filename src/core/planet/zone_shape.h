/**
 * @file zone_shape.h
 * @brief ZONAS GEOMÉTRICAS con PERÍMETRO: círculos y polígonos sobre la esfera del planeta.
 *
 * Es la forma de definir "una ciudad es una zona": un área con límites, no un color pintado en
 * un mapa. Determinista y GL-free — solo pregunta si una DIRECCIÓN unitaria del planeta cae
 * dentro de la forma. Las capas de props las referencian por nombre (`PropLayer::zones`), y el
 * scatter las prueba por celda a través de `IPropSphereField::zoneAt`.
 *
 * El perímetro se declara en lat/lon GRADOS (amigable para un IDE): un círculo es un centro más
 * un radio en metros; un polígono es una lista de vértices en orden de borde. Ambos se convierten
 * a direcciones unitarias al probar.
 */
#pragma once
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka { namespace Planet {

/** @brief Perímetro de una zona: UN círculo O UN polígono (los dos pueden estar ausentes = zona
 *  solo por color pintado). El radio del círculo está en METROS sobre la superficie. */
struct ZoneShape {
    bool hasCircle = false;
    glm::dvec2 circleCenterDeg = glm::dvec2(0.0, 0.0);   ///< (lat, lon) en grados
    double     circleRadiusM   = 0.0;

    bool hasPolygon = false;
    std::vector<glm::dvec2> polygonDeg;                  ///< vértices (lat, lon) grados, en orden

    bool hasGeometry() const { return hasCircle || hasPolygon; }
};

/** @brief (lat, lon) en grados → dirección unitaria del planeta. La conversión que unen los
 *  perímetros con las direcciones que pregunta el scatter. */
inline glm::dvec3 latLonDegToDir(double latDeg, double lonDeg) {
    const double lat = glm::radians(latDeg);
    const double lon = glm::radians(lonDeg);
    const double cl = std::cos(lat);
    return glm::dvec3(cl * std::cos(lon), std::sin(lat), cl * std::sin(lon));
}

/**
 * @brief ¿Cae la dirección dentro de la forma de la zona?
 *
 * Círculo: `dot(dir, centro) >= cos(radioM / radioPlaneta)` — comparación angular exacta.
 * Polígono: se proyectan los vértices al plano tangente del CENTROIDE (aproximación plana válida
 * para polígonos ≪ del planeta, que es lo que un autor dibuja como "perímetro de una ciudad") y
 * se aplica point-in-polygon even-odd.
 */
inline bool zoneShapeContains(const ZoneShape& z, const glm::vec3& dir, double planetRadius) {
    if (z.hasCircle) {
        const glm::dvec3 c = latLonDegToDir(z.circleCenterDeg.x, z.circleCenterDeg.y);
        const double arc = (planetRadius > 1e-6) ? (z.circleRadiusM / planetRadius) : 0.0;
        return glm::dot(glm::normalize(glm::dvec3(dir)), c) >= std::cos(arc);
    }
    if (z.hasPolygon && z.polygonDeg.size() >= 3) {
        glm::dvec3 acc(0.0);
        for (const auto& v : z.polygonDeg) acc += latLonDegToDir(v.x, v.y);
        const glm::dvec3 up = glm::normalize(acc);   // centroide → normal del plano tangente
        glm::dvec3 ref = (std::abs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 tu = glm::normalize(glm::cross(ref, up));
        const glm::dvec3 tv = glm::cross(up, tu);
        std::vector<glm::dvec2> poly;
        poly.reserve(z.polygonDeg.size());
        for (const auto& v : z.polygonDeg) {
            const glm::dvec3 d = latLonDegToDir(v.x, v.y);
            poly.push_back(glm::dvec2(glm::dot(d, tu), glm::dot(d, tv)));
        }
        const glm::dvec3 p = glm::normalize(glm::dvec3(dir));
        const glm::dvec2 pt(glm::dot(p, tu), glm::dot(p, tv));
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const glm::dvec2& a = poly[i];
            const glm::dvec2& b = poly[j];
            if (((a.y > pt.y) != (b.y > pt.y)) &&
                (pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x))
                inside = !inside;
        }
        return inside;
    }
    return false;
}

}} // namespace Haruka::Planet
