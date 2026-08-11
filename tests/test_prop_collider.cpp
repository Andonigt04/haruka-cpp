// ================================================================================================
// COLLIDERS de los props del scatter (core/planet/prop_collider.h).
//
// Qué se perdió y por qué existe este banco
// -----------------------------------------
// Los árboles del mundo pasaron al scatter GLOBAL del motor y con ello se apagó el `ResourceSystem`
// del juego, que era el único que registraba colliders y permitía talar. Durante ese tiempo los
// props se DIBUJABAN y no se podían tocar: se atravesaban. Los colliders vuelven derivados del
// ESQUELETO del árbol (`treeSkeleton`), no de una caja envolvente.
//
// Lo que este banco fija
// ----------------------
//  1. UNA SOLA FUENTE: los colliders de un árbol son EXACTAMENTE sus partes de esqueleto. Si el
//     collider se derivara aparte, chocarías con un árbol distinto del que ves.
//  2. SE CAMINA BAJO LAS RAMAS. Es la propiedad que motivó hacerlo por partes en vez de con una
//     caja. CON CONTRAPRUEBA: la caja envolvente —lo que haría una implementación ingenua— SÍ
//     bloquea ese hueco, y el test lo comprueba, así que el punto 2 no puede pasar por casualidad.
//  3. El collider NO depende del LOD. El detalle cambia triángulos, no dónde está el tronco: si el
//     esqueleto siguiera al detalle, chocarías con cosas distintas según lo lejos que mirases.
//  4. Romper una parte le quita SU collider y solo el suyo.
//  5. La instancia se coloca de pie sobre la esfera (el eje del tronco sigue a la vertical local),
//     que es el fallo que ya ocurrió una vez con el yaw y dejó los árboles tumbados.
// ================================================================================================
#include "test_common.h"
#include "core/planet/prop_collider.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Haruka::Planet;
using Haruka::Tools::ProcGraph::TreeSkeleton;
using Haruka::Tools::ProcGraph::TreePartKind;
using Haruka::Tools::ProcGraph::treeSkeleton;

namespace {

/// ¿El punto `p` (local, sin escalar) cae dentro de la caja de la parte?
bool insidePart(const PropColliderPart& c, const glm::vec3& p) {
    const glm::vec3 ay = glm::normalize(c.axis);
    glm::vec3 refv = (std::abs(ay.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 ax = glm::normalize(glm::cross(refv, ay));
    const glm::vec3 az = glm::cross(ay, ax);
    const glm::vec3 d  = p - c.center;
    const glm::vec3 l(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, az));
    return std::abs(l.x) <= c.half.x && std::abs(l.y) <= c.half.y && std::abs(l.z) <= c.half.z;
}

bool insideAny(const std::vector<PropColliderPart>& parts, const glm::vec3& p) {
    for (const auto& c : parts) if (insidePart(c, p)) return true;
    return false;
}

} // namespace

void test_prop_collider() {
    beginTest("prop_collider");

    const PropTreeParams tp = propTreeParams();
    const uint32_t kSeed = 12345u;

    // --- 1. Los colliders SON las partes del esqueleto ------------------------------------------
    const TreeSkeleton sk = treeSkeleton((int)kSeed, tp.height, tp.trunkR);
    const std::vector<PropColliderPart> parts = propColliderParts("tree", kSeed);

    CHECK(sk.parts.size() >= 3, "el arbol tiene tronco y al menos 2 ramas");
    CHECK(parts.size() == sk.parts.size(), "un collider por parte del esqueleto");

    bool axesMatch = true, lensMatch = true, idsMatch = true;
    for (size_t i = 0; i < parts.size() && i < sk.parts.size(); ++i) {
        const auto& sp = sk.parts[i];
        const auto& cp = parts[i];
        if (cp.partId != (int)i) idsMatch = false;
        // El centro del collider es el punto medio del segmento y su media longitud, la mitad.
        if (glm::length(cp.center - 0.5f * (sp.a + sp.b)) > 1e-5f) axesMatch = false;
        if (std::abs(cp.half.y - 0.5f * sp.length()) > 1e-5f) lensMatch = false;
    }
    CHECK(idsMatch,  "el partId es el indice en el esqueleto (contrato con la mascara)");
    CHECK(axesMatch, "el collider se centra en el segmento del esqueleto");
    CHECK(lensMatch, "la media longitud del collider es la del segmento");

    // --- 2. SE CAMINA BAJO LAS RAMAS, con contraprueba ------------------------------------------
    //
    // Se busca un punto a la altura del pecho (1,6 m) que esté FUERA del tronco y de toda rama,
    // pero DENTRO de la caja envolvente del árbol entero. Ese punto es "el aire bajo la copa": el
    // jugador tiene que poder estar ahí. La contraprueba es que la caja envolvente —lo que haría un
    // collider ingenuo de una sola pieza— lo bloquea.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const auto& sp : sk.parts) {
        lo = glm::min(lo, glm::min(sp.a, sp.b) - glm::vec3(sp.radiusMid()));
        hi = glm::max(hi, glm::max(sp.a, sp.b) + glm::vec3(sp.radiusMid()));
    }
    // La copa es más ancha que el esqueleto: la envolvente REAL del árbol dibujado la incluye.
    const float canopyR = tp.trunkR * (3.2f + 1.6f * tp.canopy);
    lo.x = std::min(lo.x, -canopyR); lo.z = std::min(lo.z, -canopyR);
    hi.x = std::max(hi.x,  canopyR); hi.z = std::max(hi.z,  canopyR);

    int freePoints = 0, blockedByBox = 0, probed = 0;
    const float chestY = 1.6f;
    for (int i = 0; i < 64; ++i) {
        const float a = 6.2831853f * (float)i / 64.0f;
        const glm::vec3 p(std::cos(a) * canopyR * 0.8f, chestY, std::sin(a) * canopyR * 0.8f);
        ++probed;
        if (!insideAny(parts, p)) ++freePoints;
        const bool inBox = p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y &&
                           p.z >= lo.z && p.z <= hi.z;
        if (inBox) ++blockedByBox;
    }
    printf("    bajo la copa a %.1f m: %d de %d puntos LIBRES con colliders por parte · "
           "%d BLOQUEADOS por la caja envolvente\n", chestY, freePoints, probed, blockedByBox);
    CHECK(freePoints == probed, "por partes: se pasa bajo la copa en todo el perimetro");
    CHECK(blockedByBox == probed, "CONTRAPRUEBA: la caja envolvente bloquea esos mismos puntos");

    // El tronco SÍ bloquea: si no, el árbol no colisionaría con nada y el punto 2 sería trivial.
    CHECK(insideAny(parts, glm::vec3(0.0f, chestY, 0.0f)),
          "el tronco si bloquea (el test anterior no es trivial)");

    // --- 2b. NINGÚN TRIÁNGULO CRUZA DOS PARTES --------------------------------------------------
    //
    // De esto depende que romper una rama se pueda hacer en el vertex shader colapsando sus
    // vértices a un punto: si un triángulo tuviera vértices de dos partes, colapsar solo algunos lo
    // dejaría ESTIRADO entre el punto y los vértices vivos, que es justo la clase de artefacto que
    // produjo las bandas negras del terreno. Aquí se comprueba sobre la malla real, en varios
    // niveles de detalle (el LOD cambia qué se emite, no a qué parte pertenece).
    {
        int mixedTris = 0, checkedTris = 0;
        for (float det : { 1.0f, 0.60f, 0.45f }) {
            Haruka::Tools::ProcGraph::Graph g;
            const int node = g.emplaceNode<Haruka::Tools::ProcGraph::TreeMeshNode>(
                (int)kSeed, tp.height, tp.trunkR, tp.canopy, tp.segments, det);
            g.compile();
            Haruka::Tools::ProcGraph::TreeMeshData m;
            if (!Haruka::Tools::ProcGraph::bakeTreeMesh(g, node, m)) continue;
            if (m.partId.size() != m.positions.size()) { mixedTris = -1; break; }
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                ++checkedTris;
                const unsigned char p0 = m.partId[m.indices[t]];
                if (m.partId[m.indices[t + 1]] != p0 || m.partId[m.indices[t + 2]] != p0)
                    ++mixedTris;
            }
        }
        printf("    triangulos revisados: %d · con vertices de DOS partes: %d\n",
               checkedTris, mixedTris);
        CHECK(checkedTris > 300, "se han revisado triangulos de verdad");
        CHECK(mixedTris == 0, "ningun triangulo cruza dos partes (el colapso no deja estirados)");
    }

    // --- 3. El collider no depende del LOD -------------------------------------------------------
    // `treeSkeleton` no toma `detail` por construcción; lo que se comprueba es que las ramas están
    // SIEMPRE, incluso a detalles donde la malla ya no las dibuja (detalle < 0.5).
    int nBranch = 0;
    for (const auto& sp : sk.parts) if (sp.kind == TreePartKind::Branch) ++nBranch;
    CHECK(nBranch >= 2, "las ramas estan en el esqueleto aunque el LOD bajo no las dibuje");

    // --- 4. Romper una parte quita SU collider y solo el suyo -----------------------------------
    const glm::dvec3 planetC(0.0);
    const double     planetR = 6371000.0;
    const glm::vec3  dir = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
    const auto whole  = propWorldColliders(parts, dir, 0.0f, 1.0f, 0.0f, planetC, planetR, 0u);
    const auto broken = propWorldColliders(parts, dir, 0.0f, 1.0f, 0.0f, planetC, planetR, 1u << 1);
    CHECK(whole.size() == parts.size(), "arbol entero: todas las cajas");
    CHECK(broken.size() == parts.size() - 1, "rama rota: una caja menos");
    bool goneIsTheRightOne = true;
    for (const auto& b : broken) if (b.partId == 1) goneIsTheRightOne = false;
    CHECK(goneIsTheRightOne, "la caja que falta es la de la parte rota, no otra");

    // --- 5. La instancia se planta DE PIE sobre la esfera ----------------------------------------
    // El eje Y de la caja del tronco tiene que seguir a la vertical local (`dir`). Es exactamente el
    // fallo que ya ocurrió al aplicar el yaw sobre el eje equivocado y dejó los árboles tumbados.
    const glm::dvec3 trunkAxis = glm::normalize(glm::dvec3(whole[0].rot[1]));
    const double dotUp = glm::dot(trunkAxis, glm::dvec3(dir));
    printf("    eje del tronco vs vertical local: dot = %.6f (inclinacion %.2f deg)\n",
           dotUp, std::acos(std::min(1.0, dotUp)) * 180.0 / 3.14159265358979);
    CHECK(dotUp > 0.99, "el tronco se planta de pie sobre la vertical local");

    // Y el yaw NO puede tumbarlo: girar sobre la vertical deja el eje donde estaba.
    const auto spun = propWorldColliders(parts, dir, 0.0f, 1.0f, 2.1f, planetC, planetR, 0u);
    const double dotSpun = glm::dot(glm::normalize(glm::dvec3(spun[0].rot[1])), glm::dvec3(dir));
    CHECK(dotSpun > 0.99, "el yaw gira el arbol pero no lo tumba");

    // --- 6. La escala escala ---------------------------------------------------------------------
    const auto big = propWorldColliders(parts, dir, 0.0f, 2.0f, 0.0f, planetC, planetR, 0u);
    CHECK(std::abs(big[0].halfExtents.y - whole[0].halfExtents.y * 2.0) < 1e-6,
          "la escala de la instancia escala el collider");
    CHECK(std::abs(big[0].length - whole[0].length * 2.0) < 1e-6,
          "y la longitud del segmento (la que decide si deja un palo)");
}
