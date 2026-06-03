#include "inventory.h"
#include <algorithm>

namespace Haruka {

int Inventory::add(const std::string& id, int count) {
    if (id.empty() || count <= 0) return count;
    const int maxStack = ItemRegistry::get().maxStack(id);

    // 1) Top up existing stacks of the same item.
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.id == id && s.count < maxStack) {
            int space = maxStack - s.count;
            int put = std::min(space, count);
            s.count += put;
            count   -= put;
        }
    }
    // 2) Fill empty slots.
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.empty()) {
            int put = std::min(maxStack, count);
            s.id = id;
            s.count = put;
            count -= put;
        }
    }
    return count; // leftover that didn't fit
}

int Inventory::remove(const std::string& id, int count) {
    if (id.empty() || count <= 0) return 0;
    int removed = 0;
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.id == id && s.count > 0) {
            int take = std::min(s.count, count);
            s.count -= take;
            count   -= take;
            removed += take;
            if (s.count <= 0) s.clear();
        }
    }
    return removed;
}

int Inventory::countOf(const std::string& id) const {
    int total = 0;
    for (const auto& s : m_slots)
        if (s.id == id) total += s.count;
    return total;
}

void Inventory::moveSlot(int from, int to) {
    if (from < 0 || to < 0 || from >= size() || to >= size() || from == to) return;
    ItemStack& a = m_slots[from];
    ItemStack& b = m_slots[to];
    if (a.empty()) return;

    if (b.empty()) {              // move into empty
        b = a; a.clear(); return;
    }
    if (a.id == b.id) {           // merge same item up to maxStack
        int maxStack = ItemRegistry::get().maxStack(a.id);
        int space = maxStack - b.count;
        int move = std::min(space, a.count);
        b.count += move;
        a.count -= move;
        if (a.count <= 0) a.clear();
        return;
    }
    std::swap(a, b);              // different items → swap
}

void Inventory::clear() {
    for (auto& s : m_slots) s.clear();
}

} // namespace Haruka
