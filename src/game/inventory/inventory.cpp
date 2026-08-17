#include "inventory.h"
#include <algorithm>

namespace Haruka {

// Sin política instalada apila solo lo EXACTAMENTE igual: es lo que hacía el sistema de siempre y
// es la opción segura. El juego instala su criterio de bandas en el arranque.
static Inventory::StackPolicy g_stackPolicy = nullptr;
void Inventory::setStackPolicy(StackPolicy p) { g_stackPolicy = p; }
bool Inventory::sameStack(int a, int b) { return g_stackPolicy ? g_stackPolicy(a, b) : a == b; }

int Inventory::add(const std::string& id, int count, int quality) {
    if (id.empty() || count <= 0) return count;
    if (quality < 1) quality = 1; if (quality > 10000) quality = 10000;
    const int maxStack = ItemRegistry::get().maxStack(id);

    // 1) Rellena stacks existentes del MISMO item y calidad COMPATIBLE (ver sameStack).
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.id == id && sameStack(s.quality, quality) && s.count < maxStack) {
            int space = maxStack - s.count;
            int put = std::min(space, count);
            // MEDIA PONDERADA: juntar 1 mena mala con 99 buenas no puede hundir las 100. La calidad
            // del stack es la de su contenido, no la del último que entró.
            s.quality = (int)(((long)s.quality * s.count + (long)quality * put) / (s.count + put));
            s.count += put;
            count   -= put;
        }
    }
    // 2) Huecos vacíos.
    for (auto& s : m_slots) {
        if (count <= 0) break;
        if (s.empty()) {
            int put = std::min(maxStack, count);
            s.id = id; s.count = put; s.quality = quality;
            count -= put;
        }
    }
    return count; // leftover that didn't fit
}

float Inventory::avgQuality(const std::string& id) const {
    long sum = 0, n = 0;
    for (const auto& s : m_slots)
        if (s.id == id && s.count > 0) { sum += (long)s.quality * s.count; n += s.count; }
    return n ? (float)sum / (float)n : 600.0f;
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
    if (a.id == b.id && sameStack(a.quality, b.quality)) { // funde si la politica lo permite
        int maxStack = ItemRegistry::get().maxStack(a.id);
        int space = maxStack - b.count;
        int move = std::min(space, a.count);
        b.quality = (int)(((long)b.quality * b.count + (long)a.quality * move) / (b.count + move));
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
