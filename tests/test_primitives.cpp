// ================================================================================================
// PRIMITIVAS: que sus caras miren HACIA FUERA.
//
// Una cara bobinada al revés no da ningún error: con descarte de caras traseras simplemente NO SE
// DIBUJA, y el objeto aparece con un agujero por el que se le ve el interior. Es de los fallos más
// baratos de cometer (basta invertir dos índices) y de los más caros de encontrar mirando.
//
// Y no era hipotético: las dos tapas del CILINDRO estaban del revés. Nadie lo había notado porque
// hasta hoy ningún cilindro llegaba a dibujarse — las 14 columnas del juego (y las ruedas) se
// pintaban como cajas, porque nadie interpretaba la forma declarada de un item. Al conectar esa
// forma con su primitiva, el agujero salió a la primera.
//
// La comprobación es la misma para todas: para CADA triángulo, la normal geométrica (u x v de sus
// dos aristas) tiene que apuntar al mismo lado que las normales que el generador guardó en sus
// vértices. Si discrepan, el triángulo se dibuja del revés.
// ================================================================================================
#include "test_common.h"

#include "renderer/primitive_shapes.h"

#include <glm/glm.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// @return número de triángulos cuyo bobinado contradice a sus propias normales.
int trianglesFacingIn(const std::vector<glm::vec3>& v,
                      const std::vector<glm::vec3>& n,
                      const std::vector<unsigned int>& idx,
                      double* peorDot = nullptr) {
    int malos = 0;
    double peor = 1.0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= v.size() || b >= v.size() || c >= v.size()) continue;
        const glm::vec3 geo = glm::cross(v[b] - v[a], v[c] - v[a]);
        if (glm::length(geo) < 1e-9f) continue;          // degenerado: no dice nada
        // La normal "que dice el generador" es la media de las tres del triángulo.
        glm::vec3 dicha(0.0f);
        if (a < n.size()) dicha += n[a];
        if (b < n.size()) dicha += n[b];
        if (c < n.size()) dicha += n[c];
        if (glm::length(dicha) < 1e-9f) continue;
        const double d = glm::dot(glm::normalize(geo), glm::normalize(dicha));
        peor = std::min(peor, d);
        if (d < 0.0) ++malos;
    }
    if (peorDot) *peorDot = peor;
    return malos;
}

void comprueba(const char* nombre,
               const std::vector<glm::vec3>& v,
               const std::vector<glm::vec3>& n,
               const std::vector<unsigned int>& idx) {
    double peor = 1.0;
    const int malos = trianglesFacingIn(v, n, idx, &peor);
    std::printf("    %-10s %4zu triangulos · %d al reves · peor coseno %+.3f\n",
                nombre, idx.size() / 3, malos, peor);
    CHECK(!idx.empty(), (std::string(nombre) + ": genera geometria").c_str());
    CHECK(malos == 0, (std::string(nombre) + ": todas las caras miran HACIA FUERA").c_str());
}

} // namespace

void test_primitive_winding() {
    beginTest("primitivas: ninguna cara del reves");

    std::vector<glm::vec3> v, n;
    std::vector<unsigned int> i;

    Haruka::Renderer::PrimitiveShapes::createCylinder(1.0f, 3.0f, 24, v, n, i);
    comprueba("cilindro", v, n, i);

    // ⚠️ CONTRAPRUEBA. Si el test pasara con CUALQUIER malla no estaría midiendo nada: se invierte
    // el bobinado de todos los triángulos a mano y tiene que INTERCAMBIAR buenos y malos. (Decir
    // "entonces salen todas mal" sería falso si alguna ya lo estaba — y es justo lo que pasó: el
    // cilindro tenía 48 de 96 al revés cuando escribí esto.)
    {
        const int malosAntes = trianglesFacingIn(v, n, i);
        std::vector<unsigned int> alReves = i;
        for (size_t t = 0; t + 2 < alReves.size(); t += 3) std::swap(alReves[t + 1], alReves[t + 2]);
        const int malosDespues = trianglesFacingIn(v, n, alReves);
        const int total = (int)(alReves.size() / 3);
        std::printf("    contraprueba (bobinado invertido a mano): %d al reves -> %d de %d\n",
                    malosAntes, malosDespues, total);
        CHECK(malosDespues == total - malosAntes,
              "contraprueba: invertir el bobinado intercambia las caras buenas y las malas");
    }

    Haruka::Renderer::PrimitiveShapes::createCube(1.0f, v, n, i);
    comprueba("cubo", v, n, i);

    Haruka::Renderer::PrimitiveShapes::createSphere(1.0f, 24, 16, v, n, i);
    comprueba("esfera", v, n, i);

    Haruka::Renderer::PrimitiveShapes::createCapsule(0.5f, 2.0f, 24, 16, v, n, i);
    comprueba("capsula", v, n, i);
}
