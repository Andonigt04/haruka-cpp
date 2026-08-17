// ================================================================================================
// FORMA DE LA NUBE: que sea un CUERPO y no una lámina.
//
// El fallo que este banco existe para no repetir
// ----------------------------------------------
// El pase volumétrico (`cloud_vol.frag`) estaba escrito, compilaba, tenía la suite en verde y al
// mirarlo en pantalla se veía **una sábana, y casi ninguna**. No era un bug de código: era que tres
// cifras, cada una razonable por su cuenta, se contradecían entre sí. Los tres se midieron con una
// sonda antes de tocar nada, y los tres se fijan aquí:
//
//   1. RELACIÓN ANCHO/ALTO. El grosor lo decide `sampleAt` (weather_system.cpp) y el ancho lo decide
//      la escala del campo (`kFieldScale`, que consume el shader). Estaban en ficheros distintos y
//      nadie comparaba: 2857 m de ancho contra 440-980 m de alto = entre 6,5:1 y 2,9:1. Un cúmulo
//      así de aplastado se ve como una lámina POR GEOMETRÍA, se recorra con el raymarch que se
//      recorra. No hay forma de arreglarlo dentro del shader.
//
//   2. EL CAMPO ES 3D. La densidad tiene que depender de la altura por algo MÁS que el perfil
//      vertical; si sólo depende del perfil, toda nube es un prisma de sección constante — la misma
//      silueta de la base al techo. Se comprueba leyendo el shader: es el único lado del que la CPU
//      no puede llamar a la función.
//
//   3. LA COBERTURA MEDIANA DEL PLANETA TIENE QUE DAR NUBES VISIBLES. Medido: la mediana de
//      `cloudCover` sobre la esfera es 0,111 y el 60 % del planeta está por debajo de 0,20. Con la
//      densidad vieja (`campo − umbral`, que vale 0,05-0,21) eso daba opacidad 0,016 en el cénit:
//      el pase se ejecutaba y no se veía nada. Aquí se fija que la losa típica tenga espesor
//      suficiente para saturar Beer-Lambert con la extinción del motor.
//
// Todas las comprobaciones llevan CONTRAPRUEBA con los valores ANTIGUOS: si el criterio no
// distinguiera el antes del después, no estaría midiendo nada.
// ================================================================================================
#include "test_common.h"
#include "core/weather_system.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <fstream>
#include <string>
#include <iterator>

using Haruka::WeatherSample;
using Haruka::WeatherSystem;

namespace {

std::vector<glm::dvec3> sphereDirs(int n) {
    std::vector<glm::dvec3> out;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double z   = 1.0 - 2.0 * (i + 0.5) / (double)n;
        const double r   = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = 2.39996322972865332 * i;
        out.push_back(glm::dvec3(r * std::cos(phi), r * std::sin(phi), z));
    }
    return out;
}

/// El shader, como texto. Es la única forma de auditar el lado GLSL desde un test de CPU (el mismo
/// recurso que usa `weather_3d` para atar el perfil). La ruta sale de `__FILE__` y NO del directorio
/// de trabajo: esta suite la ejecutan dos árboles de build distintos.
std::string readCloudLib() {
    std::string self = __FILE__;
    const size_t cut = self.find_last_of("/\\");
    const std::string dir = (cut == std::string::npos) ? std::string(".") : self.substr(0, cut);
    const std::string path = dir + "/../assets/shaders/lib/cloud_volume.glsl";
    std::ifstream f(path);
    if (!f) { std::printf("    (no se pudo abrir %s)\n", path.c_str()); return {}; }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

void test_cloud_shape() {
    beginTest("cloud_shape");

    WeatherSystem W;
    W.configure(4242u);
    const auto dirs = sphereDirs(600);

    // --- 1. RELACIÓN ANCHO/ALTO, con contraprueba -----------------------------------------------
    //
    // El ancho es una constante del modelo; el alto sale del clima. Se recorren climas de buen
    // tiempo (sin precipitación: el cúmulo corriente, que es el que se ve el 99 % del tiempo) y se
    // exige que la nube no sea un panqueque. El límite es 2,5:1 y no 1:1 a propósito: un cúmulo
    // humilis SÍ es más ancho que alto. Lo que no puede ser es 6:1.
    const float widthM = WeatherSystem::cloudWidthM();
    {
        double worst = 0.0; float worstThick = 0.0f;
        double sumRatio = 0.0; int n = 0;
        for (int ti = 0; ti < 8; ++ti) {
            W.setTime(ti * 197.0);
            for (const auto& d : dirs) {
                const WeatherSample w = W.sampleAt(d, 20.0f, 0.7f);
                if (w.cloudCover <= 0.05f || w.precip > 0.0f) continue;   // buen tiempo
                const double ratio = widthM / (double)w.cloudThicknessM();
                if (ratio > worst) { worst = ratio; worstThick = w.cloudThicknessM(); }
                sumRatio += ratio; ++n;
            }
        }
        std::printf("    ancho tipico %.0f m · relacion ancho/alto: media %.2f:1 · PEOR %.2f:1"
                    " (espesor %.0f m, n=%d)\n",
                    widthM, n ? sumRatio / n : 0.0, worst, worstThick, n);
        CHECK(n > 100, "hay nubes de buen tiempo suficientes para medir");
        CHECK(worst < 2.5, "ni la nube mas fina es una lamina (ancho/alto < 2.5:1)");

        // CONTRAPRUEBA: con las cifras ANTERIORES (ancho 2857 m, cuerpo 260 + 900·cover) el mismo
        // criterio tiene que FALLAR. Sin esto, un 2.5 generoso pasaría con cualquier cosa.
        double worstOld = 0.0;
        for (int ti = 0; ti < 8; ++ti) {
            W.setTime(ti * 197.0);
            for (const auto& d : dirs) {
                const WeatherSample w = W.sampleAt(d, 20.0f, 0.7f);
                if (w.cloudCover <= 0.05f || w.precip > 0.0f) continue;
                const double oldThick = 260.0 + 900.0 * w.cloudCover;   // el cuerpo de antes
                worstOld = std::max(worstOld, (1.0 / 0.00035) / oldThick);
            }
        }
        std::printf("    CONTRAPRUEBA (ancho 2857 m + cuerpo viejo): peor relacion %.2f:1\n", worstOld);
        CHECK(worstOld > 2.5, "CONTRAPRUEBA: las cifras viejas SI dan una lamina y el criterio lo ve");
    }

    // --- 2. LA COBERTURA REAL DEL PLANETA DA NUBE VISIBLE, con contraprueba ---------------------
    //
    // No basta con que la nube sea gruesa donde el cielo está cerrado: el cielo típico NO está
    // cerrado. Se mide la mediana de la cobertura sobre la esfera y se exige que ESA losa sature.
    {
        std::vector<float> cover;
        std::vector<float> thick;
        for (int ti = 0; ti < 8; ++ti) {
            W.setTime(ti * 311.0);
            for (const auto& d : dirs) {
                const WeatherSample w = W.sampleAt(d, 18.0f, 0.65f);
                cover.push_back(w.cloudCover);
                if (w.cloudCover > 0.05f) thick.push_back(w.cloudThicknessM());
            }
        }
        std::sort(cover.begin(), cover.end());
        std::sort(thick.begin(), thick.end());
        const float medCover = cover[cover.size() / 2];
        const float medThick = thick.empty() ? 0.0f : thick[thick.size() / 2];

        // Profundidad óptica de un rayo VERTICAL por el núcleo de esa nube mediana: el núcleo tiene
        // fuerza 1 por construcción (`harukaCloudStrength` normaliza), el perfil promedia ~0.7 sobre
        // la altura, y la extinción del motor es 0.008 por metro.
        const float kExtinction = 0.008f;      // gemelo de kCloudExtinction en application_render.cpp
        const float opticalMed  = medThick * 0.7f * kExtinction;
        const float alphaMed    = 1.0f - std::exp(-opticalMed);
        std::printf("    cobertura mediana %.3f · espesor mediano %.0f m -> profundidad optica %.2f"
                    " -> opacidad %.3f\n", medCover, medThick, opticalMed, alphaMed);
        CHECK(medThick > 300.0f, "la nube del cielo TIPICO tiene cuerpo, no es un jiron");
        CHECK(alphaMed > 0.80f, "el nucleo de la nube tipica es OPACO (se ve, no se transparenta)");

        // CONTRAPRUEBA: la fórmula ANTERIOR. La densidad no estaba normalizada, así que valía lo que
        // el campo superara al umbral — medido sobre el campo real, 0.054 de media con esta
        // cobertura. Con la extinción de entonces (0.014) y el espesor de entonces sale un jirón.
        //
        // ⚠️ ESTA CUENTA ES UNA COTA SUPERIOR de lo que hacía la versión vieja, no su resultado: usa
        // el exceso MEDIO sobre el umbral como si fuera el del núcleo. Una sonda que marchaba el rayo
        // de verdad contra el campo daba **0,016 en el cénit** con cobertura 0,2, bastante por debajo
        // de lo que sale aquí. El criterio se escribe sobre la cota, que es lo reproducible desde un
        // test de CPU, y por eso se exige un FACTOR y no un valor absoluto: aunque se le conceda a la
        // fórmula vieja su mejor caso, la nueva tiene que ser varias veces más opaca.
        const float oldDens    = 0.054f;                       // medido: (campo − umbral) medio
        const float oldThick   = 260.0f + 900.0f * medCover;
        const float oldOptical = oldThick * 0.7f * oldDens * 0.014f;
        const float oldAlpha   = 1.0f - std::exp(-oldOptical);
        std::printf("    CONTRAPRUEBA (densidad sin normalizar, COTA SUPERIOR): optica %.4f"
                    " -> opacidad %.4f · la nueva es %.1fx mas opaca\n",
                    oldOptical, oldAlpha, oldAlpha > 0.0f ? alphaMed / oldAlpha : 0.0f);
        CHECK(oldAlpha < 0.25f,
              "CONTRAPRUEBA: la formula vieja daba un JIRON con la cobertura tipica");
        CHECK(alphaMed > oldAlpha * 4.0f,
              "CONTRAPRUEBA: la formula nueva es varias veces mas opaca con el MISMO cielo");
    }

    // --- 3. EL CAMPO DEL SHADER ES 3D, con contraprueba -----------------------------------------
    //
    // Lo único que impide volver al prisma extruido es que la densidad reciba la altura por una vía
    // distinta del perfil. Desde C++ no se puede llamar a la función, así que se audita el texto:
    // no es una demostración de que el resultado sea correcto, pero SÍ detecta el retroceso — que es
    // el fallo que de verdad ocurrió.
    {
        const std::string src = readCloudLib();
        CHECK(!src.empty(), "se ha encontrado lib/cloud_volume.glsl");
        if (!src.empty()) {
            const bool has3D     = src.find("harukaCloudVNoise3") != std::string::npos;
            const bool hasDens   = src.find("harukaCloudDensity") != std::string::npos;
            const bool hasRemap  = src.find("HARUKA_CLOUD_MINTOP") != std::string::npos;
            const bool hasNorm   = src.find("harukaCloudStrength") != std::string::npos;
            std::printf("    shader: ruido3D %d · densidad %d · remapeo de altura %d · normalizada %d\n",
                        (int)has3D, (int)hasDens, (int)hasRemap, (int)hasNorm);
            CHECK(has3D,    "el shader tiene ruido 3D (la silueta cambia con la altura)");
            CHECK(hasDens,  "el shader tiene una densidad volumetrica, no campo x perfil");
            CHECK(hasRemap, "el shader remapea la altura al techo LOCAL de cada nube");
            CHECK(hasNorm,  "la fuerza de la nube esta normalizada a [0,1]");

            // El perfil compartido con la CPU NO se puede haber tocado: el remapeo trabaja DENTRO de
            // esa envolvente, y si alguien cambiara el perfil para dar forma se rompería la paridad
            // que ata `cloudDensityAt` con el shader (banco `weather_3d`).
            CHECK(src.find("HARUKA_CLOUD_RISE") != std::string::npos &&
                  src.find("HARUKA_CLOUD_FALL") != std::string::npos,
                  "el perfil compartido con la CPU sigue existiendo (no se sustituyo por la forma nueva)");

            // CONTRAPRUEBA: el lector no da por bueno lo que no está.
            CHECK(src.find("harukaCloudVNoise4D") == std::string::npos,
                  "CONTRAPRUEBA: el lector NO encuentra un simbolo que no existe");
        }
    }

    // --- 3b. EL MUESTREO RESUELVE LA NUBE, con contraprueba -------------------------------------
    //
    // ⚠️ ESTE ES EL PUNTO QUE SE ESCAPÓ LA PRIMERA VEZ, y la lección va más allá de las nubes: el
    // apartado 1 mide la relación ancho/alto de la LOSA, o sea de la geometría IDEAL. Salió 2,31:1,
    // pasó en verde, y la nube se seguía viendo plana — porque lo que el ojo ve no es el campo, es
    // el MUESTREO del campo. Con 24 pasos repartidos por igual, un rayo que mira al horizonte
    // recorre 14 km dentro de la losa y da pasos de 587 m contra nubes de ~330 m: menos de una
    // muestra por nube, o sea que no queda relieve que promediar. El campo estaba bien; se estaba
    // dibujando mal. (Es el mismo error de método que con los peñones de la roca: el test tiene que
    // ir contra lo que se DIBUJA, no contra la superficie ideal.)
    //
    // El arreglo es un paso que CRECE con la distancia. Aquí se comprueba lo que de eso se puede
    // comprobar desde la CPU: que el primer paso resuelve la nube y que los 24 pasos alcanzan.
    {
        const std::string src = readCloudLib();   // el paso vive en cloud_vol.frag, no en la lib
        std::string frag;
        {
            std::string self = __FILE__;
            const size_t cut = self.find_last_of("/\\");
            const std::string dir = (cut == std::string::npos) ? std::string(".") : self.substr(0, cut);
            std::ifstream f(dir + "/../assets/shaders/cloud_vol.frag");
            if (f) frag.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        CHECK(!frag.empty(), "se ha encontrado cloud_vol.frag");
        if (!frag.empty()) {
            auto readConst = [&](const char* name, float& out) -> bool {
                const size_t at = frag.find(name);
                if (at == std::string::npos) return false;
                const size_t eq = frag.find('=', at);
                if (eq == std::string::npos) return false;
                try { out = std::stof(frag.substr(eq + 1, 32)); } catch (...) { return false; }
                return true;
            };
            float nearStep = -1.0f, growth = -1.0f;
            const bool ok = readConst("kNearStep", nearStep) && readConst("kGrowth", growth);
            CHECK(ok, "el shader declara kNearStep y kGrowth (paso que crece)");
            if (ok) {
                // ⚠️ EL NÚMERO DE PASOS SE LEE DEL MOTOR, no se copia aquí. La primera versión lo
                // tenía cableado a 24 y al subirlo a 64 este test falló por su propia copia
                // obsoleta, no por el código. Una constante duplicada en un test es una bomba de
                // relojería: o falla sin motivo, o —peor— pasa auditando un valor que ya no existe.
                int STEPS = 0;
                {
                    std::string self = __FILE__;
                    const size_t cut = self.find_last_of("/\\");
                    const std::string dir = (cut == std::string::npos) ? std::string(".")
                                                                        : self.substr(0, cut);
                    std::ifstream f(dir + "/../src/core/application_render.cpp");
                    std::string ar;
                    if (f) ar.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                    const size_t at = ar.find("kCloudSteps = ");
                    if (at != std::string::npos) {
                        try { STEPS = std::stoi(ar.substr(at + 14, 8)); } catch (...) { STEPS = 0; }
                    }
                }
                std::printf("    pasos del motor (leidos de application_render.cpp): %d\n", STEPS);
                CHECK(STEPS >= 8, "se ha podido leer kCloudSteps del motor");
                if (STEPS < 8) STEPS = 24;
                // Alcance: suma de la progresión geométrica de los pasos del motor.
                const double reach = nearStep * (std::pow((double)growth, STEPS) - 1.0) / (growth - 1.0);
                // Recorrido dentro de la losa mirando casi al horizonte, sobre la losa TÍPICA.
                // d = sqrt(2·R·espesor) es la aproximación esférica del tramo rasante.
                const float  R = 6371000.0f;
                const float  typThick = 500.0f + 1400.0f * 0.11f;      // cobertura mediana
                const double grazing  = std::sqrt(2.0 * R * typThick);
                // ⚠️ La contraprueba usa **24**, que es una constante HISTÓRICA (el número de pasos
                // que tenía el motor cuando se vio el fallo), no la actual. Es deliberado: lo que se
                // demuestra es que AQUEL reparto uniforme se saltaba nubes. Evaluarla con los pasos
                // de hoy la vuelve inútil — con 64 uniformes el caso rasante ya casi da la talla, y
                // la contraprueba dejaría de separar el antes del después sin que nada se hubiera
                // roto.
                const int    kStepsWhenBugFound = 24;
                const double uniform = grazing / kStepsWhenBugFound;
                // Ancho de nube: el rasgo del campo, y en la práctica el cuerpo con densidad útil es
                // bastante menor. Se usa el rasgo entero, que es el criterio más indulgente.
                const float widthM2 = WeatherSystem::cloudWidthM();
                std::printf("    paso inicial %.0f m (crece x%.2f, alcance %.1f km) · tramo rasante"
                            " %.1f km · nube %.0f m\n", nearStep, growth, reach / 1000.0,
                            grazing / 1000.0, widthM2);
                std::printf("    muestras por nube: paso nuevo %.1f · CONTRAPRUEBA uniforme de entonces %.2f\n",
                            widthM2 / nearStep, widthM2 / uniform);
                CHECK(nearStep < widthM2 * 0.5f,
                      "el primer paso resuelve la nube (al menos 2 muestras por nube)");
                CHECK(reach > grazing,
                      "los pasos del motor alcanzan a cruzar la losa mirando al horizonte");
                CHECK(uniform > widthM2,
                      "CONTRAPRUEBA: con los 24 pasos de entonces, el reparto UNIFORME se saltaba nubes enteras");
            }
        }
        (void)src;
    }

    // --- 4. NO SE HA ROTO LA LOSA ---------------------------------------------------------------
    //
    // El cuerpo subió de 260+900·cover a 500+1400·cover. Eso toca `sampleAt`, que es la función que
    // `weather_3d` audita por otro lado: se vuelve a exigir aquí lo esencial, porque un cambio de
    // cifras que dejara la tormenta por debajo del buen tiempo pasaría desapercibido si sólo lo
    // mirara el otro banco.
    {
        double dry = 0.0, wet = 0.0; int nD = 0, nW = 0; float topMax = 0.0f;
        int badRange = 0;
        for (int ti = 0; ti < 8; ++ti) {
            W.setTime(ti * 89.0);
            for (const auto& d : dirs) {
                const WeatherSample w = W.sampleAt(d, 22.0f, 0.8f);
                if (w.cloudTopM > 14000.0f || w.cloudBaseM < 200.0f) ++badRange;
                if (w.cloudCover <= 0.05f) continue;
                if (w.precip > 0.0f) { wet += w.cloudThicknessM(); ++nW;
                                       topMax = std::max(topMax, w.cloudTopM); }
                else                 { dry += w.cloudThicknessM(); ++nD; }
            }
        }
        const double aDry = nD ? dry / nD : 0.0, aWet = nW ? wet / nW : 0.0;
        std::printf("    grosor medio: seco %.0f m (n=%d) · tormenta %.0f m (n=%d) -> %.1fx"
                    " · techo max %.0f m\n", aDry, nD, aWet, nW, aDry > 1.0 ? aWet / aDry : 0.0, topMax);
        CHECK(badRange == 0, "base y techo siguen dentro de limites fisicos");
        CHECK(aWet > aDry * 2.0, "la tormenta sigue siendo mas del doble de gruesa que el buen tiempo");
        CHECK(topMax > 4000.0f, "el cumulonimbo sigue subiendo kilometros");
    }
}
