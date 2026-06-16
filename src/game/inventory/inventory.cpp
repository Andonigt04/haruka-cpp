#include "inventory.h"
#include <algorithm>

namespace Haruka {

int Inventory::add(const std::string& id, int count, int pureza) {
    if (id.empty() || count <= 0) return count;
    if (pureza < 0) pureza = 0; if (pureza > 4) pureza = 4;
    const int maxStack = ItemRegistry::get().maxStack(id);

    // 1) Rellena stacks existentes del MISMO item Y pureza.
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.id == id && s.pureza == pureza && s.count < maxStack) {
            int space = maxStack - s.count;
            int put = std::min(space, count);
            s.count += put;
            count   -= put;
        }
    }
    // 2) Huecos vacíos.
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.empty()) {
            int put = std::min(maxStack, count);
            s.id = id; s.count = put; s.pureza = pureza;
            count -= put;
        }
    }
    return count; // leftover that didn't fit
}

float Inventory::avgPureza(const std::string& id) const {
    long sum = 0, n = 0;
    for (const auto& s : m_slots)
        if (s.id == id && s.count > 0) { sum += (long)s.pureza * s.count; n += s.count; }
    return n ? (float)sum / (float)n : 1.0f;
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
    if (a.id == b.id && a.pureza == b.pureza) { // merge same item+pureza up to maxStack
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
