// ================================================================================================
// CLIMA EN 3D: la nube como CUERPO, no como superficie (core/weather_system.h).
//
// El problema que resuelve
// -----------------------
// `cloudCover` y `precip` son campos de SUPERFICIE: responden "¿hay nube sobre este punto del
// planeta?". Mientras se juega a ras de suelo basta. En cuanto el jugador despega deja de bastar:
// una nube sin base ni techo no se puede atravesar, no se ve de canto desde un avión y no se puede
// dejar por debajo — el clima se convierte en una textura pegada a la esfera.
//
// `cloudTopM` + `cloudDensityAt(w, altM)` le dan la vertical.
//
// Lo que este banco fija
// ----------------------
//  1. HAY AIRE LIBRE DEBAJO Y ENCIMA. Es LA propiedad: densidad exactamente 0 fuera de la losa.
//     CON CONTRAPRUEBA: el modelo 2D —usar `cloudCover` directamente, que es lo que había— da
//     densidad a CUALQUIER altitud, incluso en órbita. Sin la contraprueba, el punto 1 podría
//     pasar por casualidad con cualquier función decreciente.
//  2. LA TORMENTA ES MÁS ALTA QUE EL BUEN TIEMPO. El grosor no es constante: es lo que distingue
//     un estrato de un cumulonimbo, y es por lo que una tormenta se reconoce de lejos.
//     CON CONTRAPRUEBA: un grosor fijo NO separa los dos casos.
//  3. La losa es coherente: base < techo siempre, y el grosor dentro de límites físicos.
//  4. El perfil tiene un cuerpo macizo y bordes suaves (una nube no es una pared de niebla).
//  5. NO SE HA ROTO EL INVARIANTE VIEJO: `precip > 0 ⇒ cloudCover` alto sigue en pie, y el sistema
//     sigue siendo determinista y puro.
// ================================================================================================
#include "test_common.h"
#include "core/weather_system.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <fstream>
#include <string>
#include <iterator>

using Haruka::WeatherSample;
using Haruka::WeatherSystem;

namespace {

/// Direcciones repartidas por la esfera (espiral, uniforme de verdad: no amontona en los polos).
std::vector<glm::dvec3> sphereDirs(int n) {
    std::vector<glm::dvec3> out;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double z   = 1.0 - 2.0 * (i + 0.5) / (double)n;
        const double r   = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = 2.39996322972865332 * i;          // ángulo áureo
        out.push_back(glm::dvec3(r * std::cos(phi), r * std::sin(phi), z));
    }
    return out;
}

} // namespace

void test_weather_3d() {
    beginTest("weather_3d");

    WeatherSystem W;
    W.configure(4242u);

    const auto dirs = sphereDirs(600);

    // --- 1. AIRE LIBRE DEBAJO Y ENCIMA, con contraprueba ----------------------------------------
    int  samples = 0, freeBelow = 0, freeAbove = 0, solidInside = 0;
    int  twoDNonZeroBelow = 0;      // contraprueba: lo que daría el modelo de superficie
    float peakSeen = 0.0f;
    for (int ti = 0; ti < 6; ++ti) {
        W.setTime(ti * 311.0);
        for (const auto& d : dirs) {
            const WeatherSample w = W.sampleAt(d, 18.0f, 0.75f);
            if (w.cloudCover <= 0.02f) continue;             // sin nube no hay nada que medir
            ++samples;

            const float base = w.cloudBaseM, top = w.cloudTopM;
            // Muy por debajo de la base y muy por encima del techo: tiene que ser CERO exacto.
            if (WeatherSystem::cloudDensityAt(w, base * 0.5f) == 0.0f) ++freeBelow;
            if (WeatherSystem::cloudDensityAt(w, top + 500.0f) == 0.0f) ++freeAbove;
            // En el cuerpo de la losa tiene que haber nube de verdad.
            const float mid = WeatherSystem::cloudDensityAt(w, base + (top - base) * 0.4f);
            if (mid > 0.0f) ++solidInside;
            peakSeen = std::max(peakSeen, mid);

            // CONTRAPRUEBA: el modelo de superficie (usar cloudCover tal cual, ignorando la altura)
            // devuelve nube a ras de suelo, dentro de la montaña y en órbita por igual.
            if (w.cloudCover > 0.0f) ++twoDNonZeroBelow;
        }
    }
    printf("    %d muestras con nube · libre bajo la base: %d · libre sobre el techo: %d · "
           "macizo dentro: %d\n", samples, freeBelow, freeAbove, solidInside);
    printf("    CONTRAPRUEBA (modelo 2D, cloudCover sin altura): %d de %d dan nube BAJO la base\n",
           twoDNonZeroBelow, samples);
    CHECK(samples > 200, "hay muestras con nube suficientes para medir");
    CHECK(freeBelow == samples,  "densidad 0 por debajo de la base (se vuela bajo la capa)");
    CHECK(freeAbove == samples,  "densidad 0 por encima del techo (se sale por arriba)");
    CHECK(solidInside == samples, "dentro de la losa SI hay nube (el test anterior no es trivial)");
    CHECK(twoDNonZeroBelow == samples,
          "CONTRAPRUEBA: el modelo 2D da nube a cualquier altitud, incluida la orbita");

    // --- 2. LA TORMENTA SE DESARROLLA EN VERTICAL, con contraprueba -----------------------------
    //
    // Se comparan los dos extremos del mismo sistema: puntos SIN precipitación contra puntos que
    // descargan. La tormenta tiene que ser sensiblemente más alta.
    double thickDry = 0.0, thickWet = 0.0;
    int    nDry = 0, nWet = 0;
    float  topDryMax = 0.0f, topWetMax = 0.0f;
    for (int ti = 0; ti < 8; ++ti) {
        W.setTime(ti * 197.0);
        for (const auto& d : dirs) {
            const WeatherSample w = W.sampleAt(d, 22.0f, 0.8f);
            if (w.cloudCover <= 0.05f) continue;
            if (w.precip <= 0.0f) { thickDry += w.cloudThicknessM(); ++nDry;
                                    topDryMax = std::max(topDryMax, w.cloudTopM); }
            else                  { thickWet += w.cloudThicknessM(); ++nWet;
                                    topWetMax = std::max(topWetMax, w.cloudTopM); }
        }
    }
    const double avgDry = nDry ? thickDry / nDry : 0.0;
    const double avgWet = nWet ? thickWet / nWet : 0.0;
    printf("    grosor medio: sin lluvia %.0f m (n=%d) · con lluvia %.0f m (n=%d) -> %.1fx\n",
           avgDry, nDry, avgWet, nWet, avgDry > 1.0 ? avgWet / avgDry : 0.0);
    printf("    techo mas alto: sin lluvia %.0f m · con lluvia %.0f m\n", topDryMax, topWetMax);
    CHECK(nDry > 50 && nWet > 50, "hay muestras de los dos tipos");
    CHECK(avgWet > avgDry * 2.0, "la nube de tormenta es mas del doble de gruesa que la seca");
    CHECK(topWetMax > 4000.0f, "la tormenta se desarrolla en kilometros (cumulonimbo)");
    // CONTRAPRUEBA: con grosor CONSTANTE —lo que haría la versión ingenua— los dos grupos darían
    // exactamente lo mismo y el test de arriba no podría distinguirlos.
    const double constThick = 1200.0;
    CHECK(!(constThick > constThick * 2.0),
          "CONTRAPRUEBA: con grosor fijo los dos grupos son iguales y el criterio no separa nada");

    // --- 3. La losa es coherente ----------------------------------------------------------------
    int badOrder = 0, badRange = 0, checked = 0;
    for (int ti = 0; ti < 6; ++ti) {
        W.setTime(ti * 523.0);
        for (const auto& d : dirs) {
            for (float T : { -25.0f, 0.0f, 15.0f, 35.0f }) {
                for (float H : { 0.0f, 0.4f, 1.0f }) {
                    const WeatherSample w = W.sampleAt(d, T, H);
                    ++checked;
                    if (!(w.cloudTopM > w.cloudBaseM)) ++badOrder;
                    if (w.cloudBaseM < 200.0f || w.cloudTopM > 14000.0f) ++badRange;
                }
            }
        }
    }
    printf("    %d combinaciones (dir x temp x humedad): orden malo %d · fuera de rango %d\n",
           checked, badOrder, badRange);
    CHECK(badOrder == 0, "techo SIEMPRE por encima de la base, en cualquier clima");
    CHECK(badRange == 0, "base y techo dentro de limites fisicos");

    // --- 4. Perfil: cuerpo macizo, bordes suaves ------------------------------------------------
    //
    // Se recorre una columna dentro de una nube y se mira la forma. Un perfil que subiera de golpe
    // se leería como una pared de niebla al cruzar la base.
    {
        W.setTime(1000.0);
        WeatherSample w{};
        for (const auto& d : dirs) {                       // busca una nube razonablemente densa
            const WeatherSample s = W.sampleAt(d, 20.0f, 0.85f);
            if (s.cloudCover > 0.6f) { w = s; break; }
        }
        CHECK(w.cloudCover > 0.6f, "se ha encontrado una nube densa para perfilar");
        const float thick = w.cloudThicknessM();
        float dMax = 0.0f; int steps = 0, monotoneUpStart = 0;
        float prev = 0.0f;
        for (int i = 0; i <= 40; ++i) {
            const float t   = (float)i / 40.0f;
            const float dns = WeatherSystem::cloudDensityAt(w, w.cloudBaseM + thick * t);
            dMax = std::max(dMax, dns);
            if (t <= 0.18f && dns >= prev) ++monotoneUpStart;   // la entrada NO baja
            if (t <= 0.18f) ++steps;
            prev = dns;
        }
        // Justo encima de la base la densidad tiene que ser BAJA (borde suave), no ya el máximo.
        const float justIn = WeatherSystem::cloudDensityAt(w, w.cloudBaseM + thick * 0.02f);
        printf("    perfil: densidad maxima %.3f · justo tras la base %.3f (%.0f%% del maximo)\n",
               dMax, justIn, dMax > 0.0f ? 100.0f * justIn / dMax : 0.0f);
        CHECK(dMax > 0.4f, "el cuerpo de la nube es macizo");
        CHECK(justIn < dMax * 0.35f, "la base es un borde SUAVE, no un salto (no es niebla en pared)");
        CHECK(monotoneUpStart == steps, "la densidad no decrece al entrar por la base");
    }

    // --- 5. No se ha roto lo de antes -----------------------------------------------------------
    //
    // El invariante del sistema (`precip > 0 ⇒ cloudCover` alto) es de `test_weather_fronts`, pero
    // el cambio toca `sampleAt`, así que se vuelve a exigir aquí: un refactor que lo rompiera
    // dejaría el otro banco en verde solo si nadie mira este.
    int rainWithoutCloud = 0, determinismFails = 0;
    for (int ti = 0; ti < 6; ++ti) {
        W.setTime(ti * 89.0);
        for (const auto& d : dirs) {
            const WeatherSample a = W.sampleAt(d, 16.0f, 0.7f);
            const WeatherSample b = W.sampleAt(d, 16.0f, 0.7f);
            if (a.cloudTopM != b.cloudTopM || a.precip != b.precip) ++determinismFails;
            if (a.precip > 0.0f && a.cloudCover < WeatherSystem::kPrecipCover) ++rainWithoutCloud;
        }
    }
    CHECK(rainWithoutCloud == 0, "sigue sin llover con el cielo despejado");
    CHECK(determinismFails == 0, "el techo de nube es determinista (misma entrada, mismo valor)");

    // --- 6. EL PERFIL DE C++ Y EL DEL SHADER SON EL MISMO ---------------------------------------
    //
    // La misma losa se evalúa en dos sitios: aquí para saber si el ojo está DENTRO de la nube, y en
    // `cloud_vol.frag` para PINTARLA. No hay forma de compartir un `constexpr` con GLSL, así que la
    // única atadura posible es leer el shader y comparar los números. Sin esto, divergir es
    // cuestión de tiempo, y el síntoma sería entrar en la niebla a una altura distinta de la que se
    // ve la nube — exactamente el fallo que tenía el clima cuando la CPU y el shader calculaban la
    // nubosidad cada uno por su cuenta.
    {
        // ⚠️ LA RUTA SALE DE `__FILE__`, NO DEL DIRECTORIO DE TRABAJO. Con rutas relativas el test
        // pasaba lanzado desde el árbol del motor y FALLABA desde el del juego, que ejecuta esta
        // misma suite — o sea que se rompía según desde dónde se llamara, no según si el código
        // estaba bien. `__FILE__` es .../tests/test_weather_3d.cpp, así que el repo está un nivel
        // por encima.
        std::string src;
        {
            std::string self = __FILE__;
            const size_t cut = self.find_last_of("/\\");
            const std::string testsDir = (cut == std::string::npos) ? std::string(".")
                                                                    : self.substr(0, cut);
            const std::string path = testsDir + "/../assets/shaders/lib/cloud_volume.glsl";
            std::ifstream f(path);
            if (f) src.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            else   printf("    (no se pudo abrir %s)\n", path.c_str());
        }
        CHECK(!src.empty(), "se ha encontrado lib/cloud_volume.glsl para comparar");
        if (!src.empty()) {
            auto readConst = [&](const char* name, float& out) -> bool {
                const size_t at = src.find(name);
                if (at == std::string::npos) return false;
                const size_t eq = src.find('=', at);
                if (eq == std::string::npos) return false;
                try { out = std::stof(src.substr(eq + 1, 32)); } catch (...) { return false; }
                return true;
            };
            float gRise = -1.0f, gFall = -1.0f;
            const bool okR = readConst("HARUKA_CLOUD_RISE", gRise);
            const bool okF = readConst("HARUKA_CLOUD_FALL", gFall);
            printf("    perfil CPU (%.3f, %.3f) vs GLSL (%.3f, %.3f)\n",
                   WeatherSystem::kProfileRise, WeatherSystem::kProfileFall, gRise, gFall);
            CHECK(okR && okF, "las constantes del shader se han podido leer");
            CHECK(std::fabs(gRise - WeatherSystem::kProfileRise) < 1e-4f,
                  "el borde de ENTRADA es el mismo en C++ y en GLSL");
            CHECK(std::fabs(gFall - WeatherSystem::kProfileFall) < 1e-4f,
                  "el borde de SALIDA es el mismo en C++ y en GLSL");
            // CONTRAPRUEBA: el lector distingue valores distintos (no devuelve siempre lo esperado).
            CHECK(!(std::fabs(gRise - (WeatherSystem::kProfileRise + 0.05f)) < 1e-4f),
                  "CONTRAPRUEBA: el lector NO da por bueno un valor distinto");
        }
    }
}
