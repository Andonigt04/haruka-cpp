#pragma once
/**
 * @file Prefab.h
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
 *
 * O MONTADO EN BLENDER (assets/data/prefabs/<nombre>.glb / .gltf). Es la MISMA lista de referencias
 * con pose, no una malla: del fichero sólo se lee el trozo JSON (nodos y poses); la geometría ni se
 * abre. Cada nodo con malla es una pieza:
 *   · item  = propiedad personalizada `item` del objeto, o el NOMBRE DE LA MALLA ("brick", y
 *             "brick.001" cuenta como "brick"), o el nombre del objeto. Con duplicados ENLAZADOS
 *             (Alt+D) todas las piezas comparten una malla y el .glb la guarda una sola vez.
 *   · pose  = la del nodo compuesta con la de sus padres (un vacío "muro_norte" agrupa y mueve).
 *             Es el CENTRO de la caja del item, como en el JSON: la malla que se dibuja en Blender
 *             tiene que ser esa caja centrada en su origen (ver Survival/Tools/blender_prefab_palette.py).
 *   · `joint` = "mechanism" y `arc` = grados, también como propiedades personalizadas.
 *   · Un vacío con la propiedad `anchor` (o llamado "anclaje...") es un punto de anclaje.
 * ⚠️ Una pieza ESCALADA o espejada se descarta con aviso: el tamaño lo declara el item, no el
 * montaje — escalar una plancha en Blender no la haría más grande en el juego, solo mentiría.
 * Hay que exportar con "Include > Custom Properties" para que lleguen `item`/`joint`/`arc`.
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

/**
 * @brief LO QUE LE HA PASADO A UNA PIEZA DE **ESTE** MONTAJE. El prefabricado no se entera.
 *
 * ⚠️ ES LA DIFERENCIA ENTRE UN PREFABRICADO Y UNA PLANTILLA MUERTA. Si a un barco se le parte una
 * tabla, no ha cambiado "el barco": ha cambiado ESE barco. Las dos salidas obvias son malas:
 *
 *   · tocar el `.json` del montaje → se parte la tabla en TODOS los barcos, incluidos los que aún
 *     no existen;
 *   · guardar las 870 piezas en la escena → se acabó el prefabricado, y la copia queda congelada
 *     sin enterarse de que el montaje cambió.
 *
 * Así que la escena guarda la referencia **y lo que se desvía de ella**: qué pieza y cómo. Tres
 * tablas rotas son tres líneas, no ochocientas setenta.
 *
 * `index` es la POSICIÓN en `Prefab::pieces`. Es frágil a propósito y hay que saberlo: reordenar las
 * piezas de un montaje ya colocado descoloca sus cambios. Un id por pieza lo arreglaría y no está
 * hecho — el formato del fichero no lo tiene, e inventarlo aquí sería que el editor escribiera algo
 * que el resto del motor no sabe leer.
 */
struct PrefabEdit {
    int  index = -1;          ///< pieza del montaje
    bool removed = false;     ///< ya no está: rota, arrancada, quemada
    bool hasPos = false;      ///< ¿la pose está tocada? (un `optional` no cabe en el JSON plano)
    bool hasRot = false;
    glm::dvec3  pos{0.0};
    glm::dquat  rot{1.0, 0.0, 0.0, 0.0};
    std::string item;         ///< "" = el que diga el montaje; si no, la pieza se ha SUSTITUIDO
};

/**
 * @brief Aplica los cambios de un conjunto concreto sobre la copia de su montaje.
 *
 * Vive aquí y no en cada anfitrión porque "qué significa un cambio" tiene que ser UNA respuesta: si
 * el juego y el editor la interpretaran por su cuenta, el mismo barco roto se vería de dos formas
 * distintas — que es el error que este motor ya ha pagado con las aguas y con las miniaturas.
 *
 * Las piezas marcadas `removed` se BORRAN de la copia, así que los índices posteriores se mueven:
 * por eso se aplica todo sobre `pieces` ANTES de quitar nada.
 */
void applyPrefabEdits(Prefab& pf, const std::vector<PrefabEdit>& edits);

/** @brief Carga un prefabricado (.json, .glb o .gltf, por la extensión). false si no existe o no
 *  parsea. */
bool loadPrefab(const std::string& path, Prefab& out);
/** @brief El fichero de `<dir>/<nombre>`: .json, si no .glb, si no .gltf. "" si no hay ninguno.
 *  Si hay un .json Y un .glb gana el .json —es el que escribe el editor— y se avisa: un .glb
 *  reexportado desde Blender quedaría tapado sin que nadie lo viera. */
std::string findPrefabFile(const std::string& dir, const std::string& name);
/** @brief Lo guarda: es lo que escribirá el editor y lo que exporta el comando de consola. */
bool savePrefab(const            std::string& path, const Prefab& p);

} // namespace Haruka
