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
    /// GRADOS que ESTA pieza va doblada a lo largo de su lado largo. 0 = recta.
    ///
    /// Son dos cosas distintas y por eso viven en dos sitios: el ITEM declara cuánto PUEDE doblarse
    /// su material (`ItemDef::arc`; una tabla de roble cede, una losa de pizarra no) y la PIEZA dice
    /// cuánto se dobla de hecho aquí. Quien coloca comprueba lo segundo contra lo primero: pedirle a
    /// la piedra la curva del roble no es un dato válido, es un error del fichero.
    ///
    /// El doblez es el NATURAL de una tabla: gira alrededor de su eje de ANCHO, así que la pieza se
    /// curva en el plano de su normal (positivo = se curva hacia su -Y local, que en un casco es
    /// hacia dentro, dejando la cara de fuera convexa). Doblar la tabla DE CANTO no se representa: no
    /// se hace. Lo que coloca la tabla en su sitio no es un ángulo ajustado, es su ANCLAJE a la
    /// cuaderna — ver `tools/gen_forro.py`.
    double      arc = 0.0;
};

struct Prefab {
    std::string name;
    std::vector<PrefabPiece> pieces;
    /// PUNTOS DE ANCLAJE del montaje, en el marco del prefabricado.
    ///
    /// Son los sitios donde una pieza acaba y empieza la siguiente — en un casco, dónde muere una
    /// tabla del forro y arranca la de proa. Existen como DATO porque si no existen no se pueden
    /// ver: se calculaban al generar el fichero y se perdían al escribirlo, así que las juntas sólo
    /// se podían medir con un script y nunca en pantalla, que es donde se notan.
    ///
    /// No son puertos: un puerto lo declara el ASSET y vale para todas sus copias; esto lo declara
    /// el MONTAJE y sólo vale para él. Un tablón no sabe dónde lo vas a cortar.
    std::vector<glm::dvec3> anchors;
    bool empty() const { return pieces.empty(); }
};

/** @brief Carga un prefabricado. false si no existe o no parsea. */
bool loadPrefab(const std::string& path, Prefab& out);
/** @brief Lo guarda: es lo que escribirá el editor y lo que exporta el comando de consola. */
bool savePrefab(const std::string& path, const Prefab& p);

} // namespace Haruka
