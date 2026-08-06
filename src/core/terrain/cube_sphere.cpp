#include "core/terrain/cube_sphere.h"
#include <cmath>

namespace Haruka {

    glm::dvec3 cubeFaceToDir(PlanetFace face, double lx, double ly) {
        glm::dvec3 p;
        switch (face) {
            case PlanetFace::FRONT:  p = {  1.0,  ly, -lx }; break;
            case PlanetFace::BACK:   p = { -1.0,  ly,  lx }; break;
            case PlanetFace::TOP:    p = {  lx,  1.0, -ly }; break;
            case PlanetFace::BOTTOM: p = {  lx, -1.0,  ly }; break;
            case PlanetFace::RIGHT:  p = {  lx,   ly,  1.0 }; break;
            default:                 p = { -lx,   ly, -1.0 }; break;   // LEFT
        }
        const double x2 = p.x * p.x, y2 = p.y * p.y, z2 = p.z * p.z;
        return glm::dvec3(p.x * std::sqrt(1.0 - y2 / 2.0 - z2 / 2.0 + y2 * z2 / 3.0),
                          p.y * std::sqrt(1.0 - z2 / 2.0 - x2 / 2.0 + z2 * x2 / 3.0),
                          p.z * std::sqrt(1.0 - x2 / 2.0 - y2 / 2.0 + x2 * y2 / 3.0));
    }

    // Inversa en forma cerrada. La derivación entera está en cube_sphere.h; aquí solo el cálculo.
    // ⚠️ Gemelo GLSL: assets/shaders/lib/cube_face.glsl. Los dos a la vez.
    void dirToCubeFaceClosed(const glm::dvec3& dirIn, PlanetFace& outFace, double& lx, double& ly) {
        const glm::dvec3 d = glm::normalize(dirIn);
        const double ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);

        // CARA: la componente dominante. El spherify no saca el punto de su cara, así que la
        // dirección y su punto de cubo comparten cara, y la dominante lo sigue siendo.
        // (sj, sk) = las dos componentes NO dominantes, en el orden en que `cubeFaceToDir` las
        // coloca al construir el punto del cubo.
        int dom; double sj, sk;
        if (ax >= ay && ax >= az) { dom = 0; sj = d.y; sk = d.z; }
        else if (ay >= az)        { dom = 1; sj = d.x; sk = d.z; }
        else                      { dom = 2; sj = d.x; sk = d.y; }

        const double j2 = sj * sj, k2 = sk * sk;
        const double T  = j2 + k2;
        const double D  = 2.0 * (j2 - k2);                       // = A - B
        const double Q  = std::fmax(9.0 - 12.0 * T + D * D, 0.0);
        // S = 3 - sqrt(Q), pero por la CONJUGADA: cerca del centro de la cara T→0 y sqrt(Q)→3, y
        // la resta directa se come todos los dígitos significativos.
        const double S  = (12.0 * T - D * D) / (3.0 + std::sqrt(Q));

        const double a = std::copysign(std::sqrt(std::fmax((S + D) * 0.5, 0.0)), sj);
        const double b = std::copysign(std::sqrt(std::fmax((S - D) * 0.5, 0.0)), sk);

        // De (a,b) —las dos coordenadas del punto del CUBO— a (lx,ly), deshaciendo exactamente la
        // colocación que hace `cubeFaceToDir` en cada cara.
        switch (dom) {
            case 0:
                if (d.x > 0) { outFace = PlanetFace::FRONT;  ly =  a; lx = -b; }   // p = ( 1, ly,-lx)
                else         { outFace = PlanetFace::BACK;   ly =  a; lx =  b; }   // p = (-1, ly, lx)
                break;
            case 1:
                if (d.y > 0) { outFace = PlanetFace::TOP;    lx =  a; ly = -b; }   // p = (lx, 1,-ly)
                else         { outFace = PlanetFace::BOTTOM; lx =  a; ly =  b; }   // p = (lx,-1, ly)
                break;
            default:
                if (d.z > 0) { outFace = PlanetFace::RIGHT;  lx =  a; ly =  b; }   // p = (lx,ly, 1)
                else         { outFace = PlanetFace::LEFT;   lx = -a; ly =  b; }   // p = (-lx,ly,-1)
                break;
        }
    }

    void dirToCubeFace(const glm::dvec3& dirIn, PlanetFace& outFace, double& lx, double& ly) {
        const glm::dvec3 d = glm::normalize(dirIn);

        // SEMILLA: la forma cerrada, que ya es exacta. Antes era la proyección gnómica (dividir por
        // la componente dominante), que se desvía ~1% de la cara —decenas de km en la Tierra— y por
        // eso hacía falta el Newton de abajo. Se conserva porque cuesta nada y sirve de red.
        dirToCubeFaceClosed(d, outFace, lx, ly);

        // NEWTON con jacobiano numérico: refina (lx,ly) hasta que cubeFaceToDir(lx,ly) == dir.
        const double h = 1e-6;
        for (int it = 0; it < 4; ++it) {
            const glm::dvec3 f0 = cubeFaceToDir(outFace, lx, ly);
            const glm::dvec3 e  = d - f0;
            if (glm::dot(e, e) < 1e-24) break;                 // ya está
            const glm::dvec3 dfx = (cubeFaceToDir(outFace, lx + h, ly) - f0) / h;
            const glm::dvec3 dfy = (cubeFaceToDir(outFace, lx, ly + h) - f0) / h;

            // Mínimos cuadrados 2x2 (3 ecuaciones, 2 incógnitas): J^T J · s = J^T e
            const double a = glm::dot(dfx, dfx), b = glm::dot(dfx, dfy), c = glm::dot(dfy, dfy);
            const double u = glm::dot(dfx, e),   v = glm::dot(dfy, e);
            const double det = a * c - b * b;
            if (std::abs(det) < 1e-18) break;
            lx += ( c * u - b * v) / det;
            ly += (-b * u + a * v) / det;
            lx = glm::clamp(lx, -1.0, 1.0);
            ly = glm::clamp(ly, -1.0, 1.0);
        }
    }

} // namespace Haruka
