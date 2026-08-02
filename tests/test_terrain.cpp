// ================================================================================================
// Tests de TERRENO puros-CPU (sin GL): cubemap inversa, clima, capa granular.
// ================================================================================================
#include "test_common.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "core/terrain/cube_sphere.h"
#include "core/weather_system.h"
#include "core/ground_layer.h"

using namespace Haruka;

// TEST: la inversa cube-sphere (dir ↔ cara+UV) es precisa y determinista
void test_cube_sphere_inverse() {
    beginTest("cube_sphere_inverse");
    uint32_t st = 4242u;
    auto rnd = [&]{ st = st * 1664525u + 1013904223u; return double(st >> 8) * (1.0 / 16777216.0); };

    double maxErrDir = 0.0, maxErrLx = 0.0;
    int wrongFace = 0;
    for (int i = 0; i < 20000; ++i) {
        const PlanetFace face = (PlanetFace)(int)(rnd() * 5.999);
        const double lx = rnd() * 2.0 - 1.0, ly = rnd() * 2.0 - 1.0;
        const glm::dvec3 dir = cubeFaceToDir(face, lx, ly);

        PlanetFace f2; double lx2, ly2;
        dirToCubeFace(dir, f2, lx2, ly2);

        const bool onEdge = (std::abs(lx) > 0.999 || std::abs(ly) > 0.999);
        if (f2 != face && !onEdge) ++wrongFace;

        const glm::dvec3 d2 = cubeFaceToDir(f2, lx2, ly2);
        maxErrDir = std::max(maxErrDir, glm::length(d2 - dir));

        if (f2 == face) {
            maxErrLx = std::max(maxErrLx, std::abs(lx2 - lx));
        }
    }
    std::printf("    caras mal=%d  ·  error max en lx=%.2e  ·  error angular max=%.2e (%.3f m en la Tierra)\n",
                wrongFace, maxErrLx, maxErrDir, maxErrDir * 6.371e6);
    CHECK(wrongFace == 0, "la CARA se identifica bien (salvo en la arista, donde hay dos válidas)");
    CHECK(maxErrDir * 6.371e6 < 0.5, "la inversa cae en el mismo punto (< 0.5 m en la Tierra)");

    // La forma CERRADA sola, sin Newton. Es la que se porta al shader del clipmap
    // (assets/shaders/lib/cube_face.glsl), y el shader no puede permitirse 12 evaluaciones del
    // spherify por vértice teselado. Esto es lo que ata las dos implementaciones: si alguien
    // "simplifica" la fórmula de una, esta comprobación cae.
    //
    // Importa porque el fallo anterior fue justo este y era MUDO: el clipmap invertía con la
    // proyección gnómica a secas (y con el signo de v cambiado), leía la altura de otro punto del
    // planeta —normalmente fondo oceánico—, se hundía kilómetros y no pintaba un solo píxel. Ni
    // error de GL, ni shader que no compila: terreno sin relieve y ninguna pista.
    double maxErrClosed = 0.0;
    int    wrongFaceClosed = 0;
    st = 4242u;
    for (int i = 0; i < 20000; ++i) {
        const PlanetFace face = (PlanetFace)(int)(rnd() * 5.999);
        const double lx = rnd() * 2.0 - 1.0, ly = rnd() * 2.0 - 1.0;
        const glm::dvec3 dir = cubeFaceToDir(face, lx, ly);

        PlanetFace f2; double lx2, ly2;
        dirToCubeFaceClosed(dir, f2, lx2, ly2);

        const bool onEdge = (std::abs(lx) > 0.999 || std::abs(ly) > 0.999);
        if (f2 != face) { if (!onEdge) ++wrongFaceClosed; continue; }
        maxErrClosed = std::max(maxErrClosed, std::max(std::abs(lx2 - lx), std::abs(ly2 - ly)));
    }
    // El umbral está en téxeles de la retícula, que es lo que de verdad importa: un error de 1e-6
    // en (lx,ly) sobre una cara de 256 celdas es 1e-4 téxeles, o sea nada de altura.
    std::printf("    forma cerrada: caras mal=%d  ·  error max en lx=%.2e (%.1e téxeles con res=256)\n",
                wrongFaceClosed, maxErrClosed, maxErrClosed * 128.0);
    CHECK(wrongFaceClosed == 0, "la forma cerrada identifica la misma CARA");
    CHECK(maxErrClosed * 128.0 < 1e-3, "la forma cerrada cae en el mismo téxel que el Newton");
}

// TEST: el CLIMA es del mundo, y la lluvia CAE DE LAS NUBES
void test_weather_fronts() {
    beginTest("weather_fronts");
    Haruka::WeatherSystem W;
    W.configure(4242u);

    int  total = 0, wet = 0, snowy = 0, clear = 0;
    float maxCover = 0.0f;
    bool  rainWithoutCloud = false;
    for (int ti = 0; ti < 24; ++ti) {
        W.setTime(ti * 137.0);
        int clearThisTick = 0;
        for (int i = 0; i < 400; ++i) {
            const float z  = 1.0f - 2.0f * (i + 0.5f) / 400.0f;
            const float r  = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float th = 2.39996323f * i;
            const glm::dvec3 d(r * std::cos(th), r * std::sin(th), z);
            const float humid = 0.15f + 0.8f * ((i * 37) % 100) / 100.0f;
            const float tempC = -12.0f + 40.0f * ((i * 53) % 100) / 100.0f;

            const Haruka::WeatherSample s = W.sampleAt(d, tempC, humid);
            ++total;
            maxCover = std::max(maxCover, s.cloudCover);
            if (s.cloudCover < 0.15f) { ++clear; ++clearThisTick; }
            if (s.precip > 0.0f) {
                ++wet;
                if (s.cloudCover < Haruka::WeatherSystem::kPrecipCover) rainWithoutCloud = true;
                if (s.type == Haruka::Precip::Snow) {
                    ++snowy;
                    CHECK(tempC <= Haruka::WeatherSystem::kSnowTempC, "solo nieva bajo cero");
                } else {
                    CHECK(tempC > Haruka::WeatherSystem::kSnowTempC, "por encima de cero, llueve");
                }
            } else {
                CHECK(s.type == Haruka::Precip::None, "sin precipitación no hay tipo");
            }
            CHECK(s.cloudCover >= 0.0f && s.cloudCover <= 1.0f, "cobertura acotada a [0,1]");
        }
        CHECK(clearThisTick > 0, "a cualquier hora queda cielo DESPEJADO en alguna parte del planeta");
    }
    std::printf("    clima: %d muestras · %d con precipitacian (%.1f%%, %d de nieve) · %d despejadas · cobertura max %.2f\n",
                total, wet, 100.0f * wet / total, snowy, clear, maxCover);
    CHECK(!rainWithoutCloud, "NUNCA llueve sin nube encima (precip>0 ⇒ cobertura ≥ kPrecipCover)");
    CHECK(wet > 0, "en algïn sitio y momento SA precipita (el test no pasa por vacuidad)");
    CHECK(maxCover > 0.8f, "los frentes llegan a cerrar el cielo de verdad");

    Haruka::WeatherSystem W2; W2.configure(4242u); W2.setTime(1000.0);
    W.setTime(1000.0);
    const glm::dvec3 probe = glm::normalize(glm::dvec3(0.3, 0.7, 0.5));
    const Haruka::WeatherSample a = W.sampleAt(probe, 12.0f, 0.7f);
    const Haruka::WeatherSample b = W2.sampleAt(probe, 12.0f, 0.7f);
    CHECK(a.cloudCover == b.cloudCover && a.precip == b.precip, "determinista: misma seed+tiempo = mismo clima");

    Haruka::WeatherSystem W3; W3.configure(99u); W3.setTime(1000.0);
    float diff = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const float z  = 1.0f - 2.0f * (i + 0.5f) / 200.0f;
        const float r  = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float th = 2.39996323f * i;
        const glm::dvec3 d(r * std::cos(th), r * std::sin(th), z);
        diff += std::abs(W.cloudCoverAt(d, 0.6f) - W3.cloudCoverAt(d, 0.6f));
    }
    CHECK(diff / 200.0f > 0.05f, "otra seed = otro clima (frentes en otro sitio)");

    float minC = 1e9f, maxC = -1e9f;
    for (int ti = 0; ti < 120; ++ti) {
        W.setTime(ti * 30.0);
        const float c2 = W.cloudCoverAt(probe, 0.6f);
        minC = std::min(minC, c2); maxC = std::max(maxC, c2);
    }
    std::printf("    sobre un punto fijo en 1 h: cobertura %.2f → %.2f\n", minC, maxC);
    CHECK(maxC - minC > 0.25f, "los frentes PASAN: sobre un punto fijo el cielo cambia con el tiempo");
}

// TEST: la CAPA GRANULAR y sus HUELLAS
void test_ground_layer() {
    beginTest("ground_layer");
    using GL = Haruka::GroundLayer;
    using M  = Haruka::GroundMaterial;

    CHECK(GL::lifetimeFor(M::Snow) > GL::lifetimeFor(M::Sand) * 3.0f,
          "la huella en NIEVE dura mucho más que en arena");
    CHECK(GL::sinkDepthFor(M::Snow) > GL::sinkDepthFor(M::Sand),
          "en nieve te hundes más que en arena");
    CHECK(GL::speedFactorFor(M::Snow) < GL::speedFactorFor(M::Sand),
          "la nieve frena más al andar que la arena");
    CHECK(GL::speedFactorFor(M::None) == 1.0f, "sin capa NO se frena (suelo firme)");

    GL::Stamp snow; snow.material = M::Snow; snow.bornAt = 0.0; snow.depth = 1.0f;
    GL::Stamp sand; sand.material = M::Sand; sand.bornAt = 0.0; sand.depth = 1.0f;
    const float fSnow = GL::fade(snow, GL::lifetimeFor(M::Snow) * 0.5);
    const float fSand = GL::fade(sand, GL::lifetimeFor(M::Sand) * 0.5);
    std::printf("    a media vida: nieve %.2f · arena %.2f\n", fSnow, fSand);
    CHECK(fSnow > 0.8f, "a media vida la huella en nieve sigue casi entera");
    CHECK(fSand < 0.35f, "a media vida la huella en arena ya se ha desmoronado");
    CHECK(GL::fade(snow, GL::lifetimeFor(M::Snow) + 1.0) == 0.0f, "pasada su vida, la huella no existe");

    Haruka::GroundLayer L;
    const glm::dvec3 base(6371000.0, 0.0, 0.0);
    for (int i = 0; i < 5000; ++i) {
        GL::Stamp s; s.pos = base + glm::dvec3(0, i * 0.85, 0); s.bornAt = 0.0; s.material = M::Snow;
        L.stamp(s);
    }
    CHECK(L.size() == GL::kMaxStamps, "la lista de huellas está ACOTADA (es un rastro, no un historial)");

    Haruka::GroundLayer P;
    GL::Stamp p; p.pos = base; p.radius = 0.25f; p.depth = 1.0f; p.bornAt = 0.0; p.material = M::Snow;
    P.stamp(p);
    const float atCenter = P.trampledAt(base, 1.0);
    const float atEdge   = P.trampledAt(base + glm::dvec3(0.0, 0.24, 0.0), 1.0);
    const float outside  = P.trampledAt(base + glm::dvec3(0.0, 0.60, 0.0), 1.0);
    std::printf("    pisada: centro %.2f · borde %.2f · fuera %.2f\n", atCenter, atEdge, outside);
    CHECK(atCenter > 0.9f, "el centro de la pisada está hundido del todo");
    CHECK(atEdge < atCenter, "el borde de la pisada hunde menos que el centro");
    CHECK(outside == 0.0f, "fuera del radio la capa queda INTACTA (la huella no es global)");

    P.update(GL::lifetimeFor(M::Snow) + 1.0);
    CHECK(P.size() == 0, "update() retira las huellas caducadas");

    Haruka::GroundLayer N;
    GL::Stamp none; none.material = M::None; none.pos = base; none.bornAt = 0.0;
    N.stamp(none);
    CHECK(N.size() == 0, "sin capa que pisar no se registra huella");

    Haruka::GroundLayer B;
    GL::Stamp sn; sn.pos = base;                          sn.bornAt = 0.0; sn.material = M::Snow; sn.radius = 0.25f;
    GL::Stamp sa; sa.pos = base + glm::dvec3(0, 5.0, 0);  sa.bornAt = 0.0; sa.material = M::Sand; sa.radius = 0.25f;
    B.stamp(sn); B.stamp(sa);
    B.update(60.0);
    CHECK(B.size() == 1, "a los 60 s solo sobrevive la huella de NIEVE (la de arena ya se desmoronó)");
    CHECK(B.trampledAt(base, 60.0) > 0.5f, "la huella de nieve sigue marcada a los 60 s");
    CHECK(B.trampledAt(base + glm::dvec3(0, 5.0, 0), 60.0) == 0.0f, "la de arena ya no marca");
}
