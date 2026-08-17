#pragma once
/**
 * @file prefab.h
 * @brief PREFABRICADOS: conjuntos de objetos ya montados, en DATO y no en código.
 *
 * Un prefabricado es una lista de piezas con su pose. Nada más. Puede ser un vehículo, una casa, un
 * puesto de herrero o un tramo de muralla — el prefabricado no sabe cuál de esas cosas es, y no debe
 * saberlo: **un vehículo es una estructura no anclada**, así que lo que distingue a uno de una casa
 * se decide al COLOCARLO, no al guardarlo.
 *
 * Por qué dato y no código: montar un crawler en C++ significaba que mover una plancha pedía
 * recompilar y que solo quien toca el código podía hacerlo. Como dato lo edita cualquiera, el editor
 * puede escribirlo y el DGS puede validarlo.
 *
 * ⚠️ Las piezas siguen siendo OBJETOS DISTINTOS. El prefabricado no funde nada en una sola malla:
 * cada pieza conserva su item, su collider y su isla rígida, y eso es lo que permite que una puerta
 * gire, que una suspensión ceda y que un impacto arranque un trozo. Fundirlo todo sería más barato
 * y no serviría para nada de eso.
 *
 * Formato (assets/data/prefabs/<nombre>.json):
 *   { "name": "...",
 *     "pieces": [ { "item": "armor_plate",
 *                   "pos":  [x,y,z],        // metros, marco del prefabricado
 *                   "rot":  [w,x,y,z],      // opcional; identidad si falta
 *                   "joint": "rigid|mechanism" } ] }
 */
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Haruka {

/** @brief Una pieza del prefabricado, en su marco LOCAL. */
struct PrefabPiece {
    std::string id;                        ///< itemId
    glm::dvec3  pos{0.0};
    glm::dquat  rot{1.0, 0.0, 0.0, 0.0};
    /// Su unión NO funde islas rígidas (es un mecanismo: bisagra, deslizadera). Por defecto rígida.
    bool        mechanism = false;
};

struct Prefab {
    std::string name;
    std::vector<PrefabPiece> pieces;
    bool empty() const { return pieces.empty(); }
};

/** @brief Carga un prefabricado. false si no existe o no parsea. */
bool loadPrefab(const std::string& path, Prefab& out);
/** @brief Lo guarda: es lo que escribirá el editor y lo que exporta el comando de consola. */
bool savePrefab(const std::string& path, const Prefab& p);

} // namespace Haruka
