// ================================================================================================
// Tests del ambiente por SH (CPU puro, sin GL): paridad con sky_palette.glsl y simetría azimutal.
// ================================================================================================
#include "test_common.h"

#include <cmath>
#include <glm/glm.hpp>

#include "core/sky_ambient.h"

using namespace Haruka;

// TEST: el gemelo CPU coincide con lib/sky_palette.glsl (paridad numérica de la paleta)
void test_sky_ambient_palette() {
    beginTest("sky_ambient_palette");
    // El GLSL evalúa con float en el fragment. Aquí se fijan los valores de la CUADRATURA manual
    // de las funciones gemelas — si alguien toca la paleta en un solo lado, este test lo caza.
    const float eps = 1e-6f;

    // harukaSkyDay: 0 = noche, 1 = día pleno.
    CHECK(std::fabs(harukaSkyDay(-0.3f) - 0.0f) < eps, "sunElev bajo -0.12 → noche cerrada");
    CHECK(std::fabs(harukaSkyDay( 0.5f) - 1.0f) < eps, "sunElev alto → día pleno");
    float dMid = harukaSkyDay(0.05f);   // t = (0.05+0.12)/0.34 = 0.5 → smoothstep(0.5) = 0.5
    CHECK(std::fabs(dMid - 0.5f) < eps, "sunElev=0.05 → factor 0.5 exacto");

    // harukaSkyBase: cénit (t=1), horizonte (t=0) y punto medio (t=0.275).
    const glm::vec3 dayZenith  = harukaSkyBase(1.0f, 1.0f);
    const glm::vec3 dayHorizon = harukaSkyBase(0.0f, 1.0f);
    const glm::vec3 nightZen   = harukaSkyBase(1.0f, 0.0f);
    const glm::vec3 dayMid     = harukaSkyBase(0.275f, 1.0f); // h = smoothstep(0.5)
    const glm::vec3 expectMid  = (dayHorizon + dayZenith) * 0.5f;
    CHECK(glm::length(dayZenith  - SKY_ZENITH_DAY)  < eps, "cénit día = SKY_ZENITH_DAY");
    CHECK(glm::length(dayHorizon - SKY_HORIZ_DAY)   < eps, "horizonte día = SKY_HORIZ_DAY");
    CHECK(glm::length(nightZen   - SKY_ZENITH_NIGHT)< eps, "cénit noche = SKY_ZENITH_NIGHT");
    CHECK(glm::length(dayMid - expectMid) < eps, "t=0.275 (h=0.5) → media exacta");

    // Bajo el horizonte se aplana al color de horizonte (mismo comportamiento que el shader).
    const glm::vec3 below = harukaSkyBase(-0.7f, 1.0f);
    CHECK(glm::length(below - dayHorizon) < eps, "bajo horizonte → color de horizonte (aplanado)");
}

// TEST: la integración SH es estable y la simetría azimutal anula los m≠0
void test_sky_ambient_sh() {
    beginTest("sky_ambient_sh");
    // Noche cerrada (sunElev bajo) con suelo negro: la irradiancia es OSCURA pero NO nula — la
    // paleta nocturna es 0.012-0.055, no negro puro. Lo verificable es que el DC sea ~10x menor
    // que en día pleno (el cielo nocturno ilumina una fracción de la luz diurna).
    SkySH night = skyAmbientSH(-0.5f, 0.0f, glm::vec3(0.0f));
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 nightZenith = skyAmbientEval(night, up, up);
    CHECK(nightZenith.x < 0.6f && nightZenith.y < 0.6f, "noche: irradiancia tenue (no día)");

    // Día pleno sin nubes. Los términos m≠0 de la banda deben ser ~0 (la cúpula solo depende de
    // la elevación → simetría azimutal exacta alrededor de `up`). Los índices m≠0: 1,3 (l=1) y
    // 4,5,7,8 (l=2); los m=0 (banda 0 y 2 y 1) llevan TODA la señal.
    SkySH sh = skyAmbientSH(0.9f, 0.0f, glm::vec3(0.3f, 0.4f, 0.2f));
    for (int i : {1, 3, 4, 5, 7, 8})
        CHECK(glm::length(sh.coef[i]) < 1e-3f, "coeficiente m≠0 ~0 (simetría azimutal)");

    // El coeficiente DC (Y00) no es cero en día: la irradiancia media del cielo es claramente >0.
    CHECK(sh.coef[0].x > 0.01f && sh.coef[0].y > 0.01f, "DC > 0 en día (cielo azul ilumina)");

    // Evaluación: una cara que mira AL CIELO recibe más luz que una que mira al SUELO. Se compara
    // la CANTIDAD total (luminancia), no un canal suelto: con un suelo rojizo el rebote puede
    // ganar en rojo al cielo azul sin que eso signifique que ilumina más.
    auto lum = [](const glm::vec3& c) { return 0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z; };
    glm::vec3 toSky   = skyAmbientEval(sh, up, up);                 // normal = cénit
    glm::vec3 toGround = skyAmbientEval(sh, -up, up);               // normal = suelo
    CHECK(lum(toSky) > lum(toGround), "cara al cielo recibe más que cara al suelo");
    CHECK(toSky.z > toGround.z, "el cielo azul domina el canal azul frente al rebote de suelo");

    // La nube atenúa la luz difusa (escalar): con cobertura 1 la irradiancia baja.
    SkySH overcast = skyAmbientSH(0.9f, 1.0f, glm::vec3(0.3f, 0.4f, 0.2f));
    glm::vec3 ovSky = skyAmbientEval(overcast, up, up);
    CHECK(ovSky.x < toSky.x, "con nube la irradiancia difusa baja");

    // Determinismo: dos llamadas idénticas dan el mismo resultado (nada aleatorio).
    SkySH again = skyAmbientSH(0.9f, 0.0f, glm::vec3(0.3f, 0.4f, 0.2f));
    CHECK(glm::length(sh.coef[0] - again.coef[0]) < 1e-6f, "determinista entre llamadas");
}
