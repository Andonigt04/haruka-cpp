/**
 * @file inventory.h
 * @brief Optional, reusable inventory module (items, stacks, slots).
 *
 * Engine-side and game-agnostic: the game registers item definitions and owns
 * Inventory instances. No rendering or input here — a game/UI layer draws it.
 * Data-oriented and serialisable (for saves): an inventory is just slots, each a
 * stack of {item id, count}.
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <glm/glm.hpp>

namespace Haruka {

/** @brief Static definition of an item type. Registered once in ItemRegistry. */
struct ItemDef {
    std::string id;                 // unique key, e.g. "wood"
    std::string name;               // display name, e.g. "Wood"
    std::string type;               // categoría: herramienta/comida/metal/gema/estacion/...
    int   maxStack = 99;            // how many fit in one slot (1 = non-stackable)
    // Free-form tags/values the game interprets (tool, food value, damage, calidad…). SOLO NÚMEROS:
    // los valores no numéricos de "stats" se ignoran aquí y se leen en su campo propio (ver `affinity`).
    std::unordered_map<std::string, float> stats;
    // AFINIDAD por NOMBRE (clave de elements.json: "fire"/"wind"/"land"/"water"/"void"…), no por índice.
    // Antes era un entero dentro de `stats` y había DOS numeraciones incompatibles bajo el mismo nombre
    // (materials.json contaba fuego,viento,tierra,agua,vacío; elements.json fire,water,ice,wind,land…),
    // así que una gema podía "casar" con el elemento equivocado sin que nada lo cantara. Un string no
    // se desalinea en silencio. "any" = conducto neutro (cuarzo/cristal: vale para todo, más débil).
    std::string affinity;
    std::vector<std::string> tags;  // usos/etiquetas (construccion, polvora, magia…)
    // --- ASPECTO: un item se ve de UNA de estas dos formas ---------------------------------------
    // (a) `modelPath`: un .glb de verdad (herramientas, muebles, fixtures).
    // (b) `shape` + `size` + `material`: una FORMA PROCEDURAL de un material. Es lo que permite definir
    //     MILES de items sin arte: baldosa/columna/viga/bloque × piedra/madera/ladrillo/mármol… Mismo
    //     principio que los edificios (forma tejida de un material), reutilizando su lista de materiales.
    // Antes había cuatro campos (`sizeMax`, `bulk`, `variants`, `proc`) que NADIE leía: se parseaban y se
    // guardaban sin más. Los sustituye esto, que sí tiene consumidor.
    std::string modelPath;          // (a) modelo .glb — solo para lo que de verdad necesita malla a mano
    std::string shape;              // (b) forma; "" = sin forma. DOS familias:
                                    //   · PRIMITIVA (cosas rectas): block/tile/column/beam/slab/plank →
                                    //     una caja con la TEXTURA del material. Aquí caen piedra, tablón,
                                    //     baldosa, ladrillo… que hoy son .glb y NO deberían serlo.
                                    //   · PROCEDURAL (cosas orgánicas): log/branch/rock/ore → la malla se
                                    //     GENERA por semilla. En el mundo, la semilla es la de su fuente
                                    //     (variedad); el preview del inventario usa SEMILLA 1 = forma
                                    //     canónica, siempre igual.
    /// Los GRADOS que la pieza se ARQUEA a lo largo de su lado largo. 0 = recta, y es el defecto.
    ///
    /// Un casco no se forra con tablas rectas: se forra con tablas que ceden. Esto lo dice el ITEM,
    /// no quien lo coloca — dos tablones del mismo material se doblan lo mismo, y un tablón de
    /// piedra no se dobla nada. Es lo que separa "una caja fina puesta en diagonal" de una tabla.
    float       arc = 0.0f;
    /// A QUÉ EJE DE LA CAJA VA CADA EJE DE LA MALLA. `modelAxes[i]` = eje de la caja al que va el eje
    /// `i` del .glb. Una permutación de {0,1,2}; `{-1,-1,-1}` (el defecto) = emparejar POR RANGO.
    ///
    /// ⚠️ EL RANGO NO SIEMPRE PUEDE. `makePieceObject` empareja el eje más largo de la malla con el
    /// más largo de la caja, el más corto con el más corto. Eso acierta mientras la malla y la caja
    /// tengan la misma "forma", y no hay manera de que ponga el eje MÁS CORTO de la malla en el lado
    /// MÁS LARGO de la caja. Medido en un eslabón de oruga: `CaterpillarTrack.glb` es un anillo de
    /// 2,30 x 2,00 con el agujero atravesando su eje de 1,00, y el pasador de una oruga tiene que ir
    /// ATRAVESADO al vehículo, que es su lado ancho. Por rango es imposible, y la pieza salía girada
    /// con el agujero mirando al cielo.
    ///
    /// Sólo lo declaran las piezas donde el rango se equivoca; el resto no cambia ni una línea.
    glm::ivec3  modelAxes{-1, -1, -1};
    glm::vec3   size{0.5f};         // lo que define el item es su TAMAÑO/PROPORCIÓN (m), no la malla
    std::string material;           // id de material (stone, wood, brick…): su TEXTURA. PENDIENTE: hoy
                                    //     solo se aplica el COLOR del material.

    float stat(const std::string& k, float def = 0.0f) const {
        auto it = stats.find(k);
        return it != stats.end() ? it->second : def;
    }
};

/** @brief Global registry of item definitions (id → def). */
class ItemRegistry {
public:
    static ItemRegistry& get() { static ItemRegistry r; return r; }

    void registerItem(const ItemDef& def) { m_defs[def.id] = def; }
    const ItemDef* find(const std::string& id) const {
        auto it = m_defs.find(id);
        return it != m_defs.end() ? &it->second : nullptr;
    }
    bool exists(const std::string& id) const { return m_defs.count(id) != 0; }
    int  maxStack(const std::string& id) const {
        const ItemDef* d = find(id); return d ? d->maxStack : 99;
    }
    const std::unordered_map<std::string, ItemDef>& all() const { return m_defs; }

private:
    std::unordered_map<std::string, ItemDef> m_defs;
};

/** @brief One slot: item id + count + CALIDAD.
 *
 *  La calidad es un valor CONTINUO (el juego usa 1..10000): dos objetos del mismo material pueden
 *  diferir un 40 % y eso tiene que notarse. Antes era un entero de 0 a 4 y todo lo que cayera en el
 *  mismo cajón era literalmente el mismo item.
 *
 *  ⚠️ APILADO: si dos calidades apilan juntas NO lo decide el motor, que no sabe qué es "raro".
 *  Lo decide `setStackPolicy` (el juego instala su regla de bandas). Sin política, apila solo lo
 *  EXACTAMENTE igual — que es el comportamiento de siempre y el seguro. Al fundirse dos stacks la
 *  calidad resultante es la MEDIA PONDERADA por cantidad: si no, juntar 1 mena mala con 99 buenas
 *  ascendería o hundiría las 100 de golpe. */
struct ItemStack {
    std::string id;
    int count  = 0;
    int quality = 600;   // escala del juego (1..10000). 600 = común corriente

    bool empty() const { return id.empty() || count <= 0; }
    void clear() { id.clear(); count = 0; quality = 600; }
};

/**
 * @brief A fixed set of slots that hold ItemStacks, with stacking rules.
 *
 * add() fills existing stacks first (up to maxStack), then empty slots.
 * Returns the leftover that didn't fit (0 means everything fit).
 */
class Inventory {
public:
    explicit Inventory(int slotCount = 0) { resize(slotCount); }

    void resize(int slotCount) { m_slots.assign(std::max(0, slotCount), ItemStack{}); }
    int  size() const { return (int)m_slots.size(); }
    const ItemStack& slot(int i) const { return m_slots[i]; }
    ItemStack&       slot(int i)       { return m_slots[i]; }

    /** @brief Adds `count` de `id` con `quality`; devuelve lo que NO cupo. Apila según la política. */
    int add(const std::string& id, int count, int quality = 600);

    /** @brief Regla de APILADO por calidad. `nullptr` = solo apila lo exactamente igual.
     *  El juego instala aquí su criterio de bandas; el motor no sabe qué es "raro". */
    using StackPolicy = bool (*)(int qualityA, int qualityB);
    static void setStackPolicy(StackPolicy p);
    static bool sameStack(int qualityA, int qualityB);

    /** @brief Quita hasta `count` de `id` (cualquier calidad); devuelve lo retirado. */
    int remove(const std::string& id, int count);

    /** @brief Total de `id` en todas las calidades. */
    int countOf(const std::string& id) const;

    /** @brief Calidad media (ponderada por cantidad) de `id`; 600 si no hay ninguno. */
    float avgQuality(const std::string& id) const;

    /** @brief Moves/swaps/merges the stack in slot `from` onto slot `to`. */
    void moveSlot(int from, int to);

    /** @brief Empties every slot. */
    void clear();

    const std::vector<ItemStack>& slots() const { return m_slots; }

private:
    std::vector<ItemStack> m_slots;
};

} // namespace Haruka
