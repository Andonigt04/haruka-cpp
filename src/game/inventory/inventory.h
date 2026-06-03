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

namespace Haruka {

/** @brief Static definition of an item type. Registered once in ItemRegistry. */
struct ItemDef {
    std::string id;                 // unique key, e.g. "wood"
    std::string name;               // display name, e.g. "Wood"
    int   maxStack = 99;            // how many fit in one slot (1 = non-stackable)
    std::string iconPath;           // optional UI icon asset
    // Free-form tags/values the game interprets (tool, food value, damage…).
    std::unordered_map<std::string, float> stats;

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

/** @brief One slot: an item id and a count. Empty when id is "" or count <= 0. */
struct ItemStack {
    std::string id;
    int count = 0;

    bool empty() const { return id.empty() || count <= 0; }
    void clear() { id.clear(); count = 0; }
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

    /** @brief Adds `count` of `id`; returns the amount that did NOT fit. */
    int add(const std::string& id, int count);

    /** @brief Removes up to `count` of `id`; returns how many were actually removed. */
    int remove(const std::string& id, int count);

    /** @brief Total count of `id` across all slots. */
    int countOf(const std::string& id) const;

    /** @brief Moves/swaps/merges the stack in slot `from` onto slot `to`. */
    void moveSlot(int from, int to);

    /** @brief Empties every slot. */
    void clear();

    const std::vector<ItemStack>& slots() const { return m_slots; }

private:
    std::vector<ItemStack> m_slots;
};

} // namespace Haruka
