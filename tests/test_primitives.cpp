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

// ── LA CAPSULA DEL PERSONAJE, DE PIE ──────────────────────────────────────────────────────────
// Lo que se dibuja de otro jugador es la primitiva CHARACTER (0,35 x 1,9, base en el origen) con
// `getCharacterTransform`: de pie sobre la vertical del planeta, el pie en `position`. Se mide en
// CPU: se transforman los vertices y se proyectan sobre la vertical local (extension 0..1,9) y sobre
// el plano tangente (radio 0,35). CONTRAPRUEBA: la transformacion GENERICA (Euler del mundo) a esa
// misma latitud deja la capsula tumbada y medio enterrada — que es lo que se veia.
#include "core/application_internal.h"
#include "core/scene/scene_manager.h"
#include "tools/object_types.h"
#include <vector>

void test_character_capsule_standing() {
    beginTest("character_capsule_standing");
    std::vector<glm::vec3> verts, normals; std::vector<unsigned int> idx;
    Haruka::Renderer::PrimitiveShapes::createCapsule(0.35f, 1.9f, 24, 12, verts, normals, idx);
    for (auto& v : verts) v.y += 0.95f;   // lo mismo que hace getPrimitiveMesh(CHARACTER)

    // Un jugador a 31° de latitud, 131° de longitud (donde arranca Survival), sobre una Tierra en el origen.
    const double R = 6371000.0, lat = glm::radians(31.15586), lon = glm::radians(131.65449);
    const glm::dvec3 up(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
    Haruka::SceneObject obj;
    obj.position   = Haruka::WorldPos(up * R);      // el PIE
    obj.rotation   = glm::dvec3(0.0, 37.0, 0.0);    // yaw cualquiera
    obj.objectType = Haruka::ObjectType::CHARACTER;
    const Haruka::WorldPos cam(up * (R + 10.0));    // camara a 10 m: el marco es camara-relativo

    auto measure = [&](const glm::mat4& m, double& minUp, double& maxUp, double& maxRadial) {
        minUp = 1e9; maxUp = -1e9; maxRadial = 0.0;
        const glm::dvec3 footRel = glm::dvec3(obj.position - cam);   // el pie, relativo a la camara
        for (const auto& v : verts) {
            const glm::dvec3 p = glm::dvec3(m * glm::vec4(v, 1.0f)) - footRel;   // relativo al pie
            const double h = glm::dot(p, up);
            const glm::dvec3 t = p - up * h;
            minUp = std::min(minUp, h); maxUp = std::max(maxUp, h); maxRadial = std::max(maxRadial, glm::length(t));
        }
    };
    double lo, hi, rad;
    measure(AppInternal::getCharacterTransform(obj, cam, up), lo, hi, rad);
    char msg[200];
    std::snprintf(msg, sizeof msg, "de pie: del pie %.3f al %.3f m sobre la vertical local, radio %.3f (esperado 0 / 1,9 / 0,35)", lo, hi, rad);
    std::printf("    %s\n", msg);
    CHECK(std::fabs(lo) < 0.02 && std::fabs(hi - 1.9) < 0.02 && std::fabs(rad - 0.35) < 0.02, msg);

    // Contraprueba: la transformacion generica (Euler del MUNDO) con la primitiva CAPSULE de antes
    // (0,5 x 1,5 centrada): tumbada 59° y con la mitad bajo el pie.
    std::vector<glm::vec3> old, on; std::vector<unsigned int> oi;
    Haruka::Renderer::PrimitiveShapes::createCapsule(0.5f, 1.5f, 24, 12, old, on, oi);
    verts.swap(old);
    measure(AppInternal::getTransformMatrix(obj, cam), lo, hi, rad);
    std::snprintf(msg, sizeof msg, "contraprueba (antes): del %.2f al %.2f m sobre la vertical, radio tangente %.2f: enterrada y tumbada", lo, hi, rad);
    CHECK(lo < -0.5 && hi < 1.2 && rad > 0.6, msg);   // de pie el radio seria 0,5: 0,71 es la inclinacion
}
