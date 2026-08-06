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
    // Free-form tags/values the game interprets (tool, food value, damage, pureza…).
    std::unordered_map<std::string, float> stats;
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

/** @brief One slot: item id + count + pureza (0=Impuro..4=Único, universal). Los
 *  stacks separan por id Y pureza (distinta calidad = stack aparte). */
struct ItemStack {
    std::string id;
    int count  = 0;
    int pureza = 1;   // 0=Impuro 1=Común 2=No común 3=Raro 4=Único

    bool empty() const { return id.empty() || count <= 0; }
    void clear() { id.clear(); count = 0; pureza = 1; }
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

    /** @brief Adds `count` de `id` con `pureza`; devuelve lo que NO cupo. Apila por id+pureza. */
    int add(const std::string& id, int count, int pureza = 1);

    /** @brief Quita hasta `count` de `id` (cualquier pureza); devuelve lo retirado. */
    int remove(const std::string& id, int count);

    /** @brief Total de `id` en todas las purezas. */
    int countOf(const std::string& id) const;

    /** @brief Pureza media (ponderada por cantidad) de `id`; 1 (Común) si no hay. */
    float avgPureza(const std::string& id) const;

    /** @brief Moves/swaps/merges the stack in slot `from` onto slot `to`. */
    void moveSlot(int from, int to);

    /** @brief Empties every slot. */
    void clear();

    const std::vector<ItemStack>& slots() const { return m_slots; }

private:
    std::vector<ItemStack> m_slots;
};

} // namespace Haruka
