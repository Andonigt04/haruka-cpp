/**
 * @file prop_collider.h
 * @brief COLLIDERS de los props del scatter, derivados del MISMO esqueleto que dibuja la malla.
 *
 * El problema que resuelve: los props del scatter global (`scatterPropsNear` →
 * `InstancedObjectRegistry`) se dibujaban pero no colisionaban ni se podían talar. El sistema que
 * hacía las dos cosas era el `ResourceSystem` del juego, y quedó apagado al pasar los árboles al
 * scatter del motor — con él se fueron `registerPropColliders()` y el camino de cosecha.
 *
 * ── POR QUÉ POR PARTES Y NO UNA CAJA ─────────────────────────────────────────────────────
 * Un árbol NO es una caja. El sistema viejo le ponía un cajón de tronco (0,45 × 1,4 × 0,45) y
 * ya: no había ramas con las que chocar, ni nada que romper por separado. Aquí el collider son
 * los SEGMENTOS del esqueleto (`TreeSkeleton`): el fuste y cada rama, cada uno con su identidad
 * (`partId`). Eso da tres cosas de una vez:
 *   · caminas BAJO las ramas y chocas con el tronco, porque la copa no aporta collider;
 *   · se puede romper UNA rama sin tocar el árbol (el `partId` es el bit de la máscara);
 *   · el día que las ramas se muevan por softbody, el rig ya está — es esta misma cadena de
 *     segmentos con radio en cada extremo.
 *
 * ── UNA SOLA FUENTE ───────────────────────────────────────────────────────────────────────
 * ⚠️ La clasificación del prototipo (¿árbol? ¿roca? ¿casa?) y los parámetros del árbol vivían
 * ESCRITOS A MANO dentro del bake de la malla (`application_render.cpp`: `6.5f, 0.35f` y unos
 * `find("rock")`). Derivar el collider aparte habría creado una segunda copia de la misma
 * decisión, y dos copias divergen — es literalmente el fallo que costó la paridad del terreno.
 * Aquí están una vez y las consume tanto el bake como el collider.
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
// `glm::rotation`/`mat3_cast` viven en una extensión que GLM marca como experimental y que aborta
// la compilación si no se declara. En el motor la activaba otra cabecera incluida antes; esta se
// usa también desde los tests, que no arrastran esa cadena.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/quaternion.hpp>

#include "tools/procgraph/tree_mesh.h"   // TreeSkeleton / treeSkeleton / TreePartKind

namespace Haruka { namespace Planet {

/** @brief Qué clase de prop es un prototipo. Se decide por el NOMBRE porque es lo que el scatter
 *  conoce (la capa declara `mesh: "tree"`); el bake ya lo hacía así y esta es ahora la única copia. */
enum class PropShapeKind { Tree, Rock, House };

inline PropShapeKind propShapeKind(const std::string& name) {
    if (name.find("rock")  != std::string::npos ||
        name.find("roca")  != std::string::npos) return PropShapeKind::Rock;
    if (name.find("house") != std::string::npos ||
        name.find("casa")  != std::string::npos) return PropShapeKind::House;
    return PropShapeKind::Tree;
}

/** @brief Parámetros con los que se hornea el árbol prototipo. Estaban sueltos en la llamada a
 *  `TreeMeshNode`; ahora se piden aquí para que el esqueleto del collider sea EL MISMO árbol. */
struct PropTreeParams {
    float height   = 6.5f;    ///< altura nominal (m)
    float trunkR   = 0.35f;   ///< radio del tronco en la base (m)
    float canopy   = 1.0f;    ///< escala de la copa
    int   segments = 8;       ///< lados del cono a detalle 1
};
inline PropTreeParams propTreeParams() { return PropTreeParams{}; }

/**
 * @brief Un collider en el marco LOCAL del prop (base en el origen, +Y arriba, SIN escalar).
 *
 * Es una caja orientada al segmento: `axis` es la dirección del segmento y `half` sus semiejes
 * en el marco (radio, media longitud, radio). Se aproxima el cono truncado por una caja porque
 * es lo que Jolt consume aquí (`StaticOBB`) y porque para un tronco la diferencia entre cono y
 * caja es de centímetros — muy por debajo de lo que el jugador nota al chocar.
 */
struct PropColliderPart {
    int       partId = -1;             ///< índice en `TreeSkeleton::parts` (-1 = pieza sin identidad)
    bool      breakable = false;       ///< ¿se puede romper a hachazos por separado?
    glm::vec3 center{0.0f};            ///< centro de la caja (local)
    glm::vec3 axis{0, 1, 0};           ///< eje del segmento (local, unitario)
    glm::vec3 half{0.0f};              ///< semiejes (radio, media longitud, radio)
    float     length = 0.0f;           ///< longitud del segmento (m) — decide si deja un palo
};

/**
 * @brief Colliders locales de un prototipo. Para un árbol salen del esqueleto (tronco + ramas);
 *        para roca/casa es UNA caja, porque no tienen partes que romper por separado.
 * @param protoName nombre del prototipo (el `mesh` de la capa).
 * @param meshSeed  semilla del bake — la misma que dibuja la malla, o el collider sería otro árbol.
 */
inline std::vector<PropColliderPart> propColliderParts(const std::string& protoName,
                                                       uint32_t meshSeed) {
    std::vector<PropColliderPart> out;
    const PropShapeKind kind = propShapeKind(protoName);

    if (kind == PropShapeKind::Tree) {
        const PropTreeParams tp = propTreeParams();
        const Haruka::Tools::ProcGraph::TreeSkeleton sk =
            Haruka::Tools::ProcGraph::treeSkeleton((int)meshSeed, tp.height, tp.trunkR);
        out.reserve(sk.parts.size());
        for (size_t i = 0; i < sk.parts.size(); ++i) {
            const auto& p = sk.parts[i];
            const float len = p.length();
            if (len < 1e-4f) continue;
            PropColliderPart c;
            c.partId    = (int)i;
            c.breakable = true;
            c.axis      = (p.b - p.a) / len;
            c.center    = 0.5f * (p.a + p.b);
            c.half      = glm::vec3(p.radiusMid(), 0.5f * len, p.radiusMid());
            c.length    = len;
            out.push_back(c);
        }
        return out;
    }

    // Roca / casa: una sola caja. Las medidas son las de su bake (radio 1 y 0,72 de achatado para
    // la roca; cubo de lado 1 para la casa), escaladas luego por la instancia.
    PropColliderPart c;
    c.partId    = 0;
    c.breakable = (kind == PropShapeKind::Rock);   // la roca se pica; la casa no se rompe a hachazos
    c.axis      = glm::vec3(0, 1, 0);
    if (kind == PropShapeKind::Rock) {
        c.half   = glm::vec3(1.0f, 0.72f, 1.0f);
        c.center = glm::vec3(0.0f, 0.72f, 0.0f);
    } else {
        c.half   = glm::vec3(1.0f, 1.0f, 1.0f);
        c.center = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    c.length = 2.0f * c.half.y;
    out.push_back(c);
    return out;
}

/**
 * @brief Base de rotación de una instancia: lleva el marco local (+Y arriba) a la vertical del
 *        planeta y le aplica el yaw.
 *
 * ⚠️ Es la MISMA receta que el render (`glm::rotation(up, dir)` y después el yaw sobre el eje
 * LOCAL +Y, no sobre `dir` — girar sobre `dir` tumbaba los árboles), pero en DOBLE. El render la
 * hace en float porque su matriz es relativa a la cámara. La diferencia entre ambas es de micras
 * sobre un árbol de 6,5 m: aquí el radio del planeta NO multiplica el error, al contrario que en
 * el terreno, donde 1 ulp de dirección son decímetros de superficie.
 */
inline glm::dmat3 propInstanceBasis(const glm::vec3& dir, float yaw) {
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 d = glm::normalize(glm::dvec3(dir));
    const glm::dmat3 toUp = glm::mat3_cast(glm::rotation(up, d));
    const glm::dmat3 spin = glm::mat3_cast(glm::angleAxis((double)yaw, up));
    return toUp * spin;   // primero el yaw en local, luego llevar +Y a la vertical
}

/** @brief Una caja de collider ya en coordenadas de MUNDO, lista para `PhysicsEngine::addPropOBB`. */
struct PropWorldOBB {
    glm::dvec3 center{0.0};
    glm::dvec3 halfExtents{0.0};
    glm::dmat3 rot{1.0};
    int        partId = -1;
    bool       breakable = false;
    double     length = 0.0;   ///< longitud del segmento en el MUNDO (ya escalada)
};

/**
 * @brief Coloca los colliders locales de un prototipo en el mundo, para una instancia concreta.
 *
 * @param parts     colliders locales (`propColliderParts`).
 * @param dir       dirección unitaria de la instancia sobre la esfera.
 * @param heightM   cota del suelo (m) en esa dirección.
 * @param scale     escala de la instancia.
 * @param yaw       giro de la instancia (rad).
 * @param planetC   centro del planeta (m).
 * @param planetR   radio del planeta (m).
 * @param breakMask máscara de partes ROTAS: las que tengan su bit puesto no generan collider.
 */
inline std::vector<PropWorldOBB> propWorldColliders(
        const std::vector<PropColliderPart>& parts,
        const glm::vec3& dir, float heightM, float scale, float yaw,
        const glm::dvec3& planetC, double planetR, uint32_t breakMask = 0u) {
    std::vector<PropWorldOBB> out;
    out.reserve(parts.size());

    const glm::dvec3 base = planetC + glm::dvec3(glm::normalize(dir)) * (planetR + (double)heightM);
    const glm::dmat3 B    = propInstanceBasis(dir, yaw);
    const double s        = (double)scale;

    for (const auto& p : parts) {
        if (p.partId >= 0 && p.partId < 32 && (breakMask & (1u << p.partId))) continue;  // rota

        // Marco de la caja: su eje Y sigue al segmento. Para el tronco eso es casi la vertical;
        // para una rama es su inclinación, que es lo que hace que puedas pasar por debajo.
        const glm::dvec3 ay = glm::normalize(glm::dvec3(p.axis));
        glm::dvec3 refv = (std::abs(ay.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 ax = glm::normalize(glm::cross(refv, ay));
        const glm::dvec3 az = glm::cross(ay, ax);

        PropWorldOBB o;
        o.rot         = B * glm::dmat3(ax, ay, az);          // columnas: ejes de la caja en local→mundo
        o.center      = base + B * (glm::dvec3(p.center) * s);
        o.halfExtents = glm::dvec3(p.half) * s;
        o.partId      = p.partId;
        o.breakable   = p.breakable;
        o.length      = (double)p.length * s;
        out.push_back(o);
    }
    return out;
}

}} // namespace Haruka::Planet
