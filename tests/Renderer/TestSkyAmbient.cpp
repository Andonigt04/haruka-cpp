// ================================================================================================
// El ambiente cielo/suelo en SH (`Core/SkyAmbient.h`): CPU puro.
//
// La paleta de cuatro colores que se integraba aqui ya no existe (el cielo sale de la atmosfera, ver
// `TestSkyAtmosphere.cpp`). Lo que queda es el paso a SH de la luz difusa que calcula `SkyLight`:
// irradiancia arriba y abajo. Se comprueba que el SH la reproduce EXACTA donde debe.
// ================================================================================================
#include "Common/TestCommon.h"

#include <cmath>
#include <glm/glm.hpp>

#include "Haruka/Core/SkyAmbient.h"

using namespace Haruka;

void test_sky_ambient_sh() {
    beginTest("sky_ambient_sh");
    const glm::vec3 up(0.30f, 0.45f, 0.80f), down(0.06f, 0.07f, 0.05f);
    const Core::SkySH sh = Core::skyHemisphereSH(up, down);
    const glm::vec3 vUp = glm::normalize(glm::vec3(0.2f, 0.9f, -0.3f));
    const glm::vec3 side = glm::normalize(glm::cross(vUp, glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 eZen = Core::skyAmbientEval(sh, vUp, vUp);
    const glm::vec3 eNad = Core::skyAmbientEval(sh, -vUp, vUp);
    const glm::vec3 eSide = Core::skyAmbientEval(sh, side, vUp);
    std::printf("    cenit (%.4f, %.4f, %.4f) · nadir (%.4f, %.4f, %.4f) · lado (%.4f, %.4f, %.4f)\n",
                eZen.x, eZen.y, eZen.z, eNad.x, eNad.y, eNad.z, eSide.x, eSide.y, eSide.z);
    CHECK(glm::length(eZen - up) < 1e-5f, "una cara al cenit recibe la irradiancia del cielo");
    CHECK(glm::length(eNad - down) < 1e-5f, "una cara al nadir, la del suelo");
    CHECK(glm::length(eSide - 0.5f * (up + down)) < 1e-5f, "una vertical, la media (mitad cielo, mitad suelo)");
    // Simetria azimutal: girar la normal alrededor de la vertical no cambia nada.
    const glm::vec3 side2 = glm::normalize(glm::cross(vUp, side));
    CHECK(glm::length(Core::skyAmbientEval(sh, side2, vUp) - eSide) < 1e-5f, "sin dependencia del acimut");
}

HARUKA_REGISTER_TESTS(
    { "sky_ambient_sh", test_sky_ambient_sh });
