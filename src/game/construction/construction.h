/**
 * @file construction.h
 * @brief Núcleo del SISTEMA DE CONSTRUCCIÓN (F1) — data model + validación de colocación. GL-free y
 *        DETERMINISTA, en su PROPIA lib `HarukaConstruction` (game/construction, FUERA de físicas: es
 *        lógica de validación, no física) → el cliente predice y el DGS valida con el MISMO código.
 *        Ver docs/guides/PLAN_CONSTRUCCION.md. F1 = piezas primitivas (caja/OBB); Jolt entra en F2-F3.
 *        Depende solo de glm + `IWorldProvider` (interfaz GL-free de consulta del mundo/terreno).
 *
 * La colocación es FREE-FORM (cualquier posición/orientación) pero VALIDADA: no penetra el terreno, no
 * solapa otra pieza, y está anclada (toca el suelo o una pieza). Sin GL, sin RNG, sin hilos → el resultado
 * es idéntico en cliente y servidor (requisito anti-cheat).
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <cstdint>

#include "physics/world_provider.h"

namespace Haruka { namespace Construction {

/** @brief Tipo de pieza del catálogo. F1: la colisión es una CAJA (OBB) de semiejes `halfExtents`.
 *  En F5+ vendrá del descompositor de .glb (convex hull + puntos de anclaje). */
// Máscara de SOPORTES válidos sobre los que una pieza puede apoyarse (regla "sobre qué").
inline constexpr uint8_t kSupportTerrain = 0x1;   // puede posarse en el TERRENO
inline constexpr uint8_t kSupportPiece   = 0x2;   // puede apilarse sobre otra PIEZA

struct PieceType {
    uint16_t   id = 0;
    glm::dvec3 halfExtents{0.5, 0.5, 0.5};   // m (caja de colisión)
    float      mass = 10.0f;                  // kg (para la física de F2-F3)
    float      breakThreshold = 1000.0f;      // fuerza que rompe un ancla (∞ = zona protegida)
    float      buildCost = 1.0f;
    // Reglas de colocación ("sobre qué / dónde"). El cliente las llena desde el item; el DGS valida igual.
    uint8_t    supports  = kSupportTerrain | kSupportPiece;  // sobre qué puede apoyar (default: ambos)
    bool       needsFlat = false;             // exige terreno LLANO bajo la huella (cimentación)
};

/** @brief Una pieza COLOCADA en el mundo. Transform en double (escala planetaria, sin jitter). */
struct Piece {
    uint64_t   id = 0;
    uint16_t   typeId = 0;
    glm::dvec3 position{0.0};                 // mundo (m)
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
    uint32_t   structureId = 0;               // grupo anclado (F2; en F1 = 1 por pieza)
    enum class State : uint8_t { Anchored, Free, Broken } state = State::Anchored;
};

/** @brief Resultado de validar una colocación. El cliente lo usa para el feedback (verde/rojo + motivo). */
enum class Placement : uint8_t {
    Ok = 0,
    UnknownType,        // typeId no está en el catálogo
    OverlapsPiece,      // solapa otra pieza ya colocada
    NotAnchored,        // no toca el suelo ni una pieza → no se sostiene
    WrongSupport,       // hay apoyo, pero no del TIPO que la pieza admite (p.ej. cimentación exige SUELO)
    TooSteep,           // exige suelo llano y el terreno bajo la huella está inclinado
    // (No hay rechazo por penetrar el terreno: el relieve se moldea con la herramienta de terreno.)
};

/** @brief Estado autoritativo de construcción: catálogo + piezas colocadas. Serializable (persistencia
 *  DGS / replicación). En F1 la búsqueda de solape es fuerza bruta; el índice espacial es optimización. */
class ConstructionState {
public:
    void setCatalog(std::vector<PieceType> catalog) { m_catalog = std::move(catalog); }
    const PieceType* type(uint16_t id) const;

    /** @brief Valida una colocación FREE-FORM. DETERMINISTA (resultado independiente del orden de
     *  iteración): no penetra terreno, no solapa, y está anclada. Cliente y DGS la llaman igual. */
    Placement validate(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient,
                       const Physics::IWorldProvider& world) const;

    /** @brief Inserta una pieza SIN validar y sin consultar el mundo. Para INCORPORAR estado que otro
     *  nodo ya validó (traspaso de una región entre servidores): revalidar allí rechazaría piezas
     *  legítimas cuyo apoyo se quedó en el nodo vecino, justo al borde de la región. Devuelve su id. */
    uint64_t addRaw(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient);

    /** @brief Añade la pieza SIN revalidar (el llamador ya validó). Devuelve su id. F2: al colocarla se
     *  tejen las FIJACIONES (aristas del grafo de anclaje) con toda pieza que roza (a ≤ kAnchorGap), se
     *  marca si toca el TERRENO, y se re-etiquetan las estructuras (componentes conexas). DETERMINISTA. */
    uint64_t add(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient,
                 const Physics::IWorldProvider& world);

    // ---- Prefabs: colocación por LOTE (un prefab = varias piezas ya posicionadas, colocadas de una) ----
    /** @brief Una pieza pendiente de un lote: tipo + transform del CENTRO de su OBB (en mundo). */
    struct Placed { uint16_t typeId = 0; glm::dvec3 pos{0.0}; glm::dquat orient{1.0, 0.0, 0.0, 0.0}; };
    /** @brief Valida un LOTE (prefab) como conjunto: ninguna pieza solapa una existente NI otra del lote, y
     *  el conjunto está ANCLADO (alguna toca el suelo o una pieza existente; el resto se sostiene por el
     *  grafo interno). Devuelve Ok o el primer motivo de fallo. DETERMINISTA. */
    Placement validateBatch(const std::vector<Placed>& batch, const Physics::IWorldProvider& world) const;
    /** @brief Coloca el LOTE de una vez (el llamador ya validó): añade todas y teje sus fijaciones (entre
     *  ellas y con lo existente). Devuelve los ids nuevos, en el orden del lote. */
    std::vector<uint64_t> addBatch(const std::vector<Placed>& batch, const Physics::IWorldProvider& world);

    // ---- F2: grafo de anclaje (fijaciones = aristas; soporte = alcanzar el suelo por el grafo) ----
    /** @brief Fijaciones de una pieza (ids de las piezas a las que está clavada/atornillada). */
    const std::vector<uint64_t>& fasteners(uint64_t pieceId) const;
    /** @brief ¿La pieza toca el TERRENO directamente? (raíz del soporte). */
    bool grounded(uint64_t pieceId) const { return m_grounded.count(pieceId) != 0; }
    // ── MECANISMOS, MASA y DISTANCIA ────────────────────────────────────────────────────────────
    // Lo que un VEHÍCULO necesita por encima de un edificio. No hace falta un tipo "vehículo": un
    // vehículo es una estructura NO anclada con un propulsor, y eso lo decide el proyecto. Aquí solo
    // vive lo que es estructura, que es lo mismo para una casa y para un acorazado terrestre.

    /** @brief Qué clase de fijación une dos piezas. */
    enum class JointKind : uint8_t {
        Rigid,      ///< clavo/tornillo: FUNDE. Las dos piezas son el mismo cuerpo mientras aguante
        Mechanism   ///< raíl (bisagra/deslizadera): NO funde, porque la hoja debe poder moverse
    };

    /** @brief Convierte en MECANISMO la fijación entre dos piezas ya unidas (o la crea).
     *
     *  ⚠️ Es el único concepto nuevo que pide un vehículo. Sin esto, una puerta atornillada al casco
     *  sería parte RÍGIDA de él: la componente conexa se la tragaría y no podría girar. Un mecanismo
     *  es una arista que CORTA la isla rígida pero SIGUE en el grafo de montaje (la puerta sigue
     *  formando parte del vehículo, y sigue sosteniéndose por él).
     *  Devuelve false si alguna de las dos piezas no existe. */
    bool setJointKind(uint64_t a, uint64_t b, JointKind kind);
    /** @brief Clase de la fijación entre dos piezas (Rigid si están unidas y nadie dijo otra cosa). */
    JointKind jointKind(uint64_t a, uint64_t b) const;

    /** @brief La ISLA RÍGIDA de una pieza: lo que se mueve como UN cuerpo.
     *
     *  Es `structureComponent` pero SIN cruzar mecanismos. La distinción es la que hace viable una
     *  fortaleza: 20 000 piezas son 1 cuerpo de física, y solo sus puertas y torretas son cuerpos
     *  aparte. */
    std::vector<uint64_t> rigidIsland(uint64_t pieceId) const;

    /** @brief Masa y centro de masas de una isla rígida. */
    struct MassProps { double massKg = 0.0; glm::dvec3 centerOfMass{0.0}; };
    /** @brief Masa total y centro de masas de la isla rígida de `pieceId`, en MUNDO.
     *
     *  La masa sale del BUILD, no se elige: si le atornillas blindaje, acelera menos. Y un centro de
     *  masas alto es lo que hace VOLCAR a una fortaleza en una ladera, sin programar el vuelco. */
    MassProps islandMass(uint64_t pieceId) const;

    /** @brief Distancia EN PIEZAS desde `from`, por el grafo de montaje. `maxDepth` < 0 = sin tope.
     *
     *  ⚠️ Esta SÍ cruza mecanismos: un condensador colgado de una bisagra sigue estando al lado de la
     *  cámara. La adyacencia es de MONTAJE; la rigidez es otra pregunta. Y es distancia por el GRAFO,
     *  no euclídea: dos piezas pegadas pero unidas dando la vuelta al casco están lejos, que es justo
     *  lo que se quiere para decidir si un condensador "oye" a la cámara. */
    std::unordered_map<uint64_t, int> distancesFrom(uint64_t from, int maxDepth = -1) const;
    /** @brief Distancia en piezas entre dos, o -1 si no están conectadas. */
    int pieceDistance(uint64_t from, uint64_t to) const;

    /** @brief ¿La pieza se SOSTIENE? = conectada por el grafo a alguna pieza grounded. F3 corta aristas
     *  y re-pregunta esto: lo que deja de estar soportado, cae. DETERMINISTA (indep. del orden). */
    bool isSupported(uint64_t pieceId) const;
    /** @brief Componente conexa (estructura) que contiene la pieza, ids ORDENADOS (determinista). */
    std::vector<uint64_t> structureComponent(uint64_t pieceId) const;
    /** @brief Nº total de fijaciones (aristas no dirigidas). Para tests/estadística. */
    std::size_t fastenerCount() const;

    // ---- F3: rotura estructural (quitar pieza → recomputar soporte → colapso en cascada) ----
    /** @brief Rompe/quita una pieza y hace COLAPSAR lo que se queda sin soporte (ya no alcanza el suelo por
     *  el grafo). Devuelve los ids de las piezas que CAYERON en consecuencia (ordenados; NO incluye la rota
     *  ni las protegidas). No-op si `pieceId` está protegida. DETERMINISTA. */
    std::vector<uint64_t> breakPiece(uint64_t pieceId);
    /** @brief Marca una pieza como ZONA PROTEGIDA: irrompible y ancla del mundo (soporte raíz, nunca cae). */
    void setProtected(uint64_t pieceId, bool prot);
    bool isProtected(uint64_t pieceId) const { return m_protected.count(pieceId) != 0; }

    /** @brief Quita la pieza y todas sus aristas (también en sus vecinos). PÚBLICO porque el traspaso de
     *  una región entre nodos del DGS necesita SOLTAR lo que ya sirve otro nodo. */
    void erasePiece(uint64_t pieceId);

    const std::unordered_map<uint64_t, Piece>& pieces() const { return m_pieces; }
    std::size_t size() const { return m_pieces.size(); }

    // Tolerancias (m). Públicas para que el test y el cliente compartan los mismos umbrales.
    static constexpr double kAnchorGap = 0.30;  // hueco máximo para considerarse "apoyado/tocando"
    static constexpr double kFlatTol   = 0.30;  // desnivel máx del terreno bajo la huella para ser "llano"

private:
    void relabelStructures();          // recomputa Piece::structureId como componentes conexas (determinista)


    std::vector<PieceType>              m_catalog;
    std::unordered_map<uint64_t, Piece> m_pieces;
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_adj;  // fijaciones (adyacencia simétrica)
    // Aristas que son MECANISMO (no funden). Se guarda con la clave ordenada (min,max) para que la
    // arista sea UNA sola cosa: guardarla dos veces invita a que las dos copias discrepen.
    std::set<std::pair<uint64_t, uint64_t>> m_mechanisms;
    std::unordered_set<uint64_t>        m_grounded;             // piezas que tocan el terreno
    std::unordered_set<uint64_t>        m_protected;            // zonas protegidas (ancla irrompible)
    uint64_t                            m_nextId = 1;
};

// ================================================================================================
// FIXTURES (puerta/ventana/marco): la REGLA de dónde van en un muro. GL-free y DETERMINISTA (como el
// resto del core → el cliente predice y el DGS valida con el MISMO código). El CAPARAZÓN (muros/tejado)
// es procedural del material; estas fixtures son los MODELOS (.glb) que van en los HUECOS. El core
// calcula los huecos y su transform; el cliente solo instancia el .glb en ese transform.
// ================================================================================================
enum class FixtureKind : uint8_t { Door, Window };

/** @brief Un HUECO en un muro, en coordenadas del PLANO del muro: `center.x` a lo ancho (0 = centro del
 *  muro), `center.y` = altura desde la BASE del muro; `halfExtents` = medio (ancho, alto) del hueco. */
struct Opening {
    FixtureKind kind = FixtureKind::Window;
    glm::dvec2  center{0.0, 0.0};
    glm::dvec2  halfExtents{0.5, 0.5};
};

/** @brief Calcula los HUECOS de un muro de `wallW`×`wallH` (m): si `isFrontGround`, una PUERTA centrada
 *  y apoyada en la base (ancho `doorW`, alto `doorH`); y VENTANAS (ancho `winW`, alto `winH`) a la altura
 *  de alféizar `sill`, repartidas con el RITMO `baySpacing`, evitando solaparse con la puerta. DETERMINISTA
 *  (independiente del orden). Devuelve los huecos en coordenadas del plano del muro (ver `Opening`). */
std::vector<Opening> wallOpenings(double wallW, double wallH, bool isFrontGround,
                                  double doorW, double doorH, double winW, double winH,
                                  double sill, double baySpacing);

/** @brief ¿Cabe un fixture de semiejes 2D `fixtureHalf` (ancho, alto) en el hueco `op`? (con tolerancia). */
bool fixtureFits(const Opening& op, const glm::dvec2& fixtureHalf);

// ================================================================================================
// LAYOUT de edificio: la ESTRUCTURA en datos (SIN render, en CPU) — plantas, tamaño, muros con sus
// huecos. Determinista y testeable. Después, el cliente COLOCA las PIEZAS del material (tablones,
// ladrillos…) siguiendo este layout, y pone las fixtures (puerta/ventana) en los huecos como objetos
// normales (que ya se mueven). El layout NO conoce material, .glb ni escala: solo geometría/estructura.
// ================================================================================================
/** @brief Un muro del layout: tramo `a`→`b` en PLANTA (x,z locales, edificio centrado en el origen), con
 *  base a `baseY` y altura `height`; `openings` en coords del plano del muro (ver `wallOpenings`). */
struct WallRun {
    glm::dvec2 a{0.0, 0.0}, b{0.0, 0.0};   // extremos en planta (x,z)
    double     baseY = 0.0, height = 3.0, thickness = 0.25;
    bool       isFrontGround = false;      // el frontal de la planta baja (lleva la puerta)
    std::vector<Opening> openings;
};
struct BuildingLayout {
    int    floors = 1;
    double footprintX = 6.0, footprintZ = 6.0, floorHeight = 3.0;
    std::vector<WallRun> walls;   // 4 por planta (frente/atrás/izq/der), con sus huecos
};
/** @brief Genera el LAYOUT (sin render): `floors` plantas de `W`×`D`, 4 muros por planta con sus huecos
 *  (puerta en el frontal de la planta baja + ventanas en el ritmo). DETERMINISTA. El frente = z=+D/2. */
BuildingLayout buildingLayout(int floors, double W, double D, double wallT, double floorH,
                              double doorW, double doorH, double winW, double winH,
                              double sill, double baySpacing);

/** @brief Un trozo MACIZO de muro, en el plano del muro (mismas coords que `Opening`). */
struct WallSolid { glm::dvec2 center{0.0, 0.0}, halfExtents{0.0, 0.0}; };

/** @brief Descompone un muro en sus trozos MACIZOS — para COLISIÓN, no para dibujar.
 *
 *  Un edificio tejido son miles de piezas; registrar una caja por pieza es inviable. Pero la colisión no
 *  necesita las piezas: necesita saber por dónde NO se puede pasar, y eso está en el LAYOUT. De un muro
 *  salen 1..N cajas en vez de cientos.
 *
 *  Solo se descuentan las PUERTAS. Las ventanas no: el muro bajo el alféizar y el dintel siguen siendo
 *  macizos y el hueco queda demasiado alto para cruzarlo andando, así que descontarlas multiplicaría las
 *  cajas sin cambiar por dónde se camina. Sobre cada puerta se emite su DINTEL. */
std::vector<WallSolid> wallSolids(double wallW, double wallH, const std::vector<Opening>& openings);

}} // namespace Haruka::Construction
