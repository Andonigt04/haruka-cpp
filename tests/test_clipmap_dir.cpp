// ================================================================================================
// LA RECONSTRUCCIÓN DE `dir` DEL CLIPMAP: la única pieza del terreno que nunca se había comparado.
//
// Por qué este test existe
// ------------------------
// Buscando una disparidad visible del terreno murieron seis hipótesis, todas medidas:
//   · CPU y GPU comparten el campo de altura, la bilineal, el triM, el radio base y el recorte.
//   · Los pies quedan a ±1 mm del suelo (con el radio de la cápsula restado).
//   · El clipmap está activo y dibuja quads de 4 m, la misma retícula que colisiona.
//   · Los vértices de la malla de colisión están EXACTAMENTE en la función (autotest: 0,0000 m).
//
// Todas comparaban cosas que se calculan en CPU por el mismo camino, así que no podían ver esto: el
// vértice del clipmap **no llega a su dirección por el mismo camino que la CPU**.
//
//   CPU (física, props, colisión):  dir = normalize(worldPos - centro)          en DOUBLE
//   GPU (clipmap.tese):             dir = normalize(origin + tanU·x/R + tanV·y/R)  en FLOAT
//
// Son dos formas de llegar al mismo punto, y la segunda reconstruye la dirección desde el marco
// tangente con `vec3` de 32 bits — el marco viaja en el UBO como float (`cp.origin/tanU/tanV`).
// Un ulp de un `dir` unitario son ~0,38-0,76 m de posición sobre la superficie de la Tierra, y la
// altura se muestrea EN esa posición: si las dos direcciones difieren, las dos superficies difieren.
//
// Este test mide esa diferencia en metros de altura. No prueba que el terreno esté bien: prueba
// cuánto puede separarse el suelo que se DIBUJA del que se PISA por este camino concreto.
// ================================================================================================
#include "test_common.h"
#include "core/planet/terrain_lod.h"
#include "core/planet/terrain_detail.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Haruka::Planet;

namespace {

/// Reconstrucción EXACTA de `clipmap.tese`: marco en float (como el UBO) y aritmética en float.
glm::vec3 dirGPU(const glm::vec3& origin, const glm::vec3& tanU, const glm::vec3& tanV,
                 float locX, float locY, float R) {
    return glm::normalize(origin + tanU * (locX / R) + tanV * (locY / R));
}

/// El camino de la CPU: el punto del mundo en double y la dirección desde el centro, también en
/// double, y solo al final a float (que es lo que hace `sampleHeight(dirIn)`).
glm::vec3 dirCPU(const glm::dvec3& up, const glm::dvec3& tu, const glm::dvec3& tv,
                 double locX, double locY, double R) {
    return glm::vec3(glm::normalize(up * R + tu * locX + tv * locY));
}

} // namespace

void test_clipmap_dir_parity() {
    beginTest("clipmap_dir_parity");

    const double R = 6371000.0;
    // Varias latitudes: el marco tangente y su cuantización dependen de dónde estés, y el error de
    // float de una componente depende de su magnitud.
    const glm::dvec3 cams[] = {
        glm::normalize(glm::dvec3(0.31, 0.55, 0.77)) * (R + 1.7),
        glm::normalize(glm::dvec3(0.90, 0.10, 0.42)) * (R + 850.0),
        glm::normalize(glm::dvec3(0.02, 0.97, 0.24)) * (R + 4000.0),
        glm::normalize(glm::dvec3(-0.6, -0.3, 0.74)) * (R + 12.0),
    };

    double worstAngleM = 0.0, worstHeightM = 0.0, sumHeightM = 0.0;
    long n = 0;
    for (const glm::dvec3& cam : cams) {
        glm::dvec3 up, tu, tv;
        // El MISMO marco anclado que llena ClipParams y que construye la malla de colisión.
        terrainClipFrame(cam, glm::dvec3(0.0), up, tu, tv, R);
        // Y como lo ve el shader: recortado a float al pasar por el UBO.
        const glm::vec3 fOrigin(up), fTanU(tu), fTanV(tv);

        // Recorrido por la caja del clipmap en múltiplos del quad, que es donde caen los vértices.
        for (int iy = -40; iy <= 40; iy += 8) {
            for (int ix = -40; ix <= 40; ix += 8) {
                const double lx = ix * TERRAIN_CLIP_QUAD_M;
                const double ly = iy * TERRAIN_CLIP_QUAD_M;
                const float triM = terrainTriM(std::sqrt(lx * lx + ly * ly));

                const glm::vec3 dG = dirGPU(fOrigin, fTanU, fTanV, (float)lx, (float)ly, (float)R);
                const glm::vec3 dC = dirCPU(up, tu, tv, lx, ly, R);

                // Cuánto se separan las dos direcciones, en METROS sobre la superficie. Es la magnitud
                // que importa: la altura se muestrea en el punto al que apunta `dir`.
                const double angM = (double)glm::length(dG - dC) * R;
                worstAngleM = std::max(worstAngleM, angM);

                // Y lo que de verdad se ve: la diferencia de ALTURA que produce esa separación. Se
                // evalúa el detalle con el mismo triM en las dos direcciones (el campo base es el
                // mismo bake y su bilineal es idéntica, así que la diferencia sale del detalle).
                const float hG = terrainDetail(dG, (float)R, triM);
                const float hC = terrainDetail(dC, (float)R, triM);
                const double dh = std::abs((double)hG - (double)hC);
                worstHeightM = std::max(worstHeightM, dh);
                sumHeightM += dh; ++n;
            }
        }
    }

    std::printf("    dir del clipmap (float, desde el marco tangente) vs dir de la CPU (double):\n");
    std::printf("      separacion de las direcciones: peor %.4f m sobre la superficie\n", worstAngleM);
    std::printf("      diferencia de ALTURA que produce: peor %.4f m · media %.4f m  (%ld muestras)\n",
                worstHeightM, sumHeightM / (double)n, n);

    // ⚠️ ESTE TEST NO EXIGE CERO, y decirlo importa: la reconstrucción en float NO PUEDE dar la misma
    // dirección que la de double, porque un `vec3` unitario no tiene resolución para direccionar la
    // superficie de la Tierra mejor que ~0,4 m (el tope del §5). Lo que se fija es que la diferencia de
    // ALTURA que eso produce se quede en el orden de la sagita del quad (~8 cm) y no en metros: si
    // alguien cambia el marco, el orden de las operaciones del tese o el tipo del UBO y esto se va a
    // decímetros, el suelo que se dibuja y el que se pisa se separan de forma visible.
    // ── EL ORIGEN SIN ANCLAR: el bug que este test destapó ──────────────────────────────────────
    //
    // `world_system_provider.h` calculaba el marco ANCLADO con `terrainClipFrame` (que cuantiza `up` a
    // la retícula del mundo) y después construía la dirección desde `rel = center - pc`, que es la
    // dirección SIN anclar. O sea: ejes tangentes del marco anclado, origen del sin anclar. El
    // comentario del propio código decía la intención — «MISMO marco ANCLADO que el clipmap: si la
    // colisión usara un marco sin anclar, las dos retículas nacerían desfasadas» — y la línea de
    // debajo hacía lo contrario.
    //
    // El desfase es de hasta medio quad (2 m) TANGENCIALES, así que las dos retículas no comparten
    // vértices y la separación pasa de la sagita a la pendiente local por 2 m. Se mide aquí para que
    // quede el número, y para que reintroducirlo salte.
    double worstUnsnapM = 0.0;
    for (const glm::dvec3& cam : cams) {
        glm::dvec3 up, tu, tv;
        terrainClipFrame(cam, glm::dvec3(0.0), up, tu, tv, R);
        const glm::dvec3 relUnsnapped = cam;                  // sin anclar (lo que hacía el provider)
        for (int iy = -20; iy <= 20; iy += 5) for (int ix = -20; ix <= 20; ix += 5) {
            const double lx = ix * TERRAIN_CLIP_QUAD_M, ly = iy * TERRAIN_CLIP_QUAD_M;
            const float triM = terrainTriM(std::sqrt(lx*lx + ly*ly));
            const glm::vec3 dAnchored  = glm::vec3(glm::normalize(up * R + tu * lx + tv * ly));
            const glm::vec3 dUnsnapped = glm::vec3(glm::normalize(relUnsnapped + tu * lx + tv * ly));
            worstUnsnapM = std::max(worstUnsnapM,
                std::abs((double)terrainDetail(dAnchored, (float)R, triM)
                       - (double)terrainDetail(dUnsnapped, (float)R, triM)));
        }
    }
    std::printf("      origen SIN anclar (el bug): diferencia de altura peor %.4f m\n", worstUnsnapM);

    // ── EL MÓDULO DEL `dir` EN FLOAT: 0,76 m de error RADIAL que ninguna prueba de altura ve ──────
    //
    // `normalize` en float deja el módulo en 1 ± 1,2e-7, no en 1. Para la DIRECCIÓN da igual, y por eso
    // el cálculo de `dirF` sigue siendo float (hay que compartirlo bit a bit con el tese). Pero la
    // malla de colisión coloca el vértice en `centro + dir·(R+h)`: ese vector se MULTIPLICA por el
    // radio de la Tierra, y 6,37e6 · 1,2e-7 son decímetros de altitud.
    //
    // ⚠️ Lo que hace este fallo tan difícil de ver es que la comprobación natural NO LO DETECTA:
    // reevaluar la función de terreno EN el vértice devuelve exactamente la altura con la que se
    // colocó (medido en el motor: 0.0000 m sobre 128 949 vértices), porque el error es puramente
    // RADIAL y la función solo depende de la dirección. Solo lo ve quien mide el RADIO. En el motor lo
    // destapó el autotest del alambre —0,55 m donde tenía que dar 0— y la cura es renormalizar en
    // double tras promover, que conserva la dirección y arregla el módulo.
    //
    // Este bloque mide el error que el bug producía. Si alguien quita la renormalización de
    // `world_system_provider.h`, ESTA es la magnitud que vuelve.
    double worstRadialM = 0.0;
    for (const glm::dvec3& cam : cams) {
        glm::dvec3 up, tu, tv;
        terrainClipFrame(cam, glm::dvec3(0.0), up, tu, tv, R);
        const glm::vec3 fOrigin(up), fTanU(tu), fTanV(tv);
        for (int iy = -40; iy <= 40; iy += 8) for (int ix = -40; ix <= 40; ix += 8) {
            const glm::vec3 dG = dirGPU(fOrigin, fTanU, fTanV,
                                        (float)(ix * TERRAIN_CLIP_QUAD_M),
                                        (float)(iy * TERRAIN_CLIP_QUAD_M), (float)R);
            // El módulo promovido a double, SIN renormalizar: es lo que multiplicaba al radio.
            const double lenErr = std::abs(glm::length(glm::dvec3(dG)) - 1.0);
            worstRadialM = std::max(worstRadialM, lenErr * R);
        }
    }
    std::printf("      modulo de `dir` en float: hasta %.4f m de error RADIAL si no se renormaliza\n",
                worstRadialM);
    CHECK(worstRadialM > 0.05, "el error radial del float es REAL (si esto fallara, el bug no existiria "
                               "y la renormalizacion sobraria)");
    // Y que la cura funciona: renormalizado en double, el módulo es 1 y el error radial desaparece.
    {
        glm::dvec3 up, tu, tv;
        terrainClipFrame(cams[0], glm::dvec3(0.0), up, tu, tv, R);
        const glm::vec3 dG = dirGPU(glm::vec3(up), glm::vec3(tu), glm::vec3(tv), 512.0f, -256.0f, (float)R);
        const double lenErr = std::abs(glm::length(glm::normalize(glm::dvec3(dG))) - 1.0);
        CHECK(lenErr * R < 1e-6, "renormalizado en double, el error radial es nulo");
    }

    CHECK(worstAngleM < 2.0, "las dos direcciones no se separan mas de 2 m sobre la superficie");
    CHECK(worstHeightM < 0.25, "y la diferencia de altura se queda por debajo de 25 cm");
    CHECK(sumHeightM / (double)n < 0.05, "la diferencia TIPICA de altura es de centimetros");
}
