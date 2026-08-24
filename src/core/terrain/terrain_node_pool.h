/**
 * @file terrain_node_pool.h
 * @brief POOL DE NODOS del quadtree (v5, fase F2): residencia, LRU y presupuesto de generación.
 *
 * ── POR QUÉ LA POLÍTICA VIVE SEPARADA DE LA GPU ─────────────────────────────────────────────────
 *
 * Aquí no hay una sola llamada al RHI. El pool decide QUÉ nodos están residentes, en qué hueco, cuál
 * se desaloja y cuántos se generan este frame; la subida es un callback que el llamador implementa.
 *
 * No es purismo: es que la política es donde están los bugs que no se ven. Un desalojo que tira un
 * nodo visible produce parpadeo, uno que no acota la generación produce un tirón al descender, y una
 * caché sin fallback deja agujeros en el suelo mientras carga. Nada de eso se depura mirando la
 * pantalla — se depura con un test que mueve la cámara mil veces y cuenta. Con la política aquí, ese
 * test corre headless y en milisegundos.
 *
 * ── LA PROPIEDAD QUE JUSTIFICA TODO EL v5 ───────────────────────────────────────────────────────
 *
 * ⚠️ Los nodos están FIJOS AL MUNDO (índices enteros, sin ancla). Por eso andar no invalida el pool:
 * los nodos que siguen a la vista siguen siendo los MISMOS objetos. Compárese con lo que hay hoy —
 * al derivar 48 m, el clipmap reutiliza **8 nodos de 257 049** (0 %), porque su marco tangente se
 * re-ancla y todos los nodos se mueven con él. `terrain_node_pool_reuse` mide esa diferencia.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include "terrain_node.h"

namespace Haruka { namespace Terrain {

/**
 * @brief ¿Son válidos los índices de este nodo? En el nivel L el rango es [0, 2^L).
 *
 * ⚠️ Existe porque un índice fuera de rango NO falla de forma visible: al subir hacia la raíz
 * aterriza en un nodo que no existe, el fallback por ancestro no encuentra nada y el pool devuelve
 * -1 — que se lee como "el pool está roto" cuando lo que está mal es la petición. Pasó al escribir
 * el test (j=5678 en el nivel 12, cuyo tope es 4095).
 */
inline bool nodeIndicesValid(const NodeId& n) {
    if (n.level > TERRAIN_NODE_MAX_LEVEL) return false;
    const uint64_t lim = 1ull << n.level;
    return n.i < lim && n.j < lim;
}

/// Clave entera de un nodo. Cabe en 64 bits: 3 de cara, 5 de nivel, 2×26 de índice.
inline uint64_t nodeKey(const NodeId& n) {
    return ((uint64_t)n.face << 58) | ((uint64_t)n.level << 52)
         | ((uint64_t)n.i << 26) | (uint64_t)n.j;
}

/**
 * @brief Pool de nodos residentes con desalojo LRU y presupuesto de generación por frame.
 *
 * Uso por frame:
 *   1. `beginFrame()`
 *   2. por cada nodo del selector: `request(n)` → devuelve el hueco si está residente, o encola su
 *      generación y devuelve el del ANCESTRO más profundo que sí lo esté (ver `Resolved`).
 *   3. `takePending()` → los nodos a generar este frame, ya acotados al presupuesto.
 *   4. tras generarlos: `publish(n, slot, range)`.
 */
class TerrainNodePool {
public:
    struct Resolved {
        int  slot      = -1;     ///< hueco en el pool, o -1 si no hay ni ancestro (no debería pasar)
        bool exact     = false;  ///< true = es el nodo pedido; false = un ancestro (se ve más basto)
        NodeId node;             ///< qué nodo hay de verdad en ese hueco
    };

    struct Stats {
        size_t resident = 0, capacity = 0;
        size_t hitsExact = 0, hitsAncestor = 0, misses = 0;
        size_t generated = 0, evicted = 0;
    };

    /**
     * @param capacity huecos del pool. A 65 KB por nodo (129² × 4 B), 1 024 huecos son 65 MB — el
     *        orden que midió `terrain_node_range` para lo visible a pie (918 nodos, 58 MB).
     * @param genPerFrame tope de nodos a generar por frame. Medido: 0,046 ms/nodo en el caso fino,
     *        así que 43 nodos son ~2 ms. Es un PRESUPUESTO, no un límite del hardware.
     */
    explicit TerrainNodePool(size_t capacity = 1024, size_t genPerFrame = 43)
        : m_capacity(capacity), m_genPerFrame(genPerFrame) {
        m_slots.resize(capacity);
        for (size_t i = 0; i < capacity; ++i) m_free.push_back((int)i);
    }

    void beginFrame() {
        ++m_frame;
        m_pending.clear();
        m_stats.hitsExact = m_stats.hitsAncestor = m_stats.misses = 0;
    }

    /**
     * @brief Pide un nodo. Si no está, lo encola y devuelve el ancestro residente más profundo.
     *
     * ⚠️ EL FALLBACK POR ANCESTRO NO ES UN ADORNO: es lo que impide AGUJEROS EN EL SUELO mientras el
     * pool carga. Sin él, un nodo pedido y no residente no tiene nada que dibujar, y al descender
     * —cuando más nodos nuevos se piden de golpe— el terreno se llenaría de huecos. Con él, lo peor
     * que pasa es que esa zona se vea más basta un par de frames, que es exactamente lo que un LOD
     * debe hacer bajo presión.
     *
     * La raíz de cada cara se marca residente al publicarla y no se desaloja nunca (`pin`), así que
     * siempre hay un ancestro que devolver.
     */
    Resolved request(const NodeId& n) {
        auto it = m_index.find(nodeKey(n));
        if (it != m_index.end()) {
            Slot& s = m_slots[(size_t)it->second];
            s.lastUsed = m_frame;
            ++m_stats.hitsExact;
            return Resolved{ it->second, true, n };
        }
        // No está: encolar (con presupuesto) y buscar ancestro.
        ++m_stats.misses;
        if (m_pending.size() < m_genPerFrame) m_pending.push_back(n);
        NodeId a = n;
        while (a.level > 0) {
            a.level--; a.i /= 2; a.j /= 2;
            auto ia = m_index.find(nodeKey(a));
            if (ia != m_index.end()) {
                m_slots[(size_t)ia->second].lastUsed = m_frame;
                ++m_stats.hitsAncestor;
                return Resolved{ ia->second, false, a };
            }
        }
        return Resolved{ -1, false, n };
    }

    /** @brief Nodos a generar este frame (ya acotados por `genPerFrame`). */
    const std::vector<NodeId>& takePending() const { return m_pending; }

    /**
     * @brief Publica un nodo ya generado. Devuelve el hueco asignado, o -1 si no cabe.
     *
     * ⚠️ NO DESALOJA NADA QUE SE HAYA USADO ESTE FRAME. Si el único candidato a desalojar está en el
     * conjunto visible, se rechaza la publicación en vez de tirarlo: desalojar algo que se va a pedir
     * otra vez dentro de un frame es la definición de trashing, y en pantalla se ve como parpadeo.
     * Rechazar solo significa "este nodo entra más tarde", y el fallback por ancestro lo cubre.
     */
    int publish(const NodeId& n, const NodeRange& range, bool pin = false) {
        auto it = m_index.find(nodeKey(n));
        if (it != m_index.end()) return it->second;          // ya estaba

        int slot = -1;
        if (!m_free.empty()) { slot = m_free.back(); m_free.pop_back(); }
        else {
            // LRU: el menos recientemente usado, y NUNCA uno usado este frame ni fijado.
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < m_slots.size(); ++i) {
                const Slot& s = m_slots[i];
                if (!s.used || s.pinned || s.lastUsed == m_frame) continue;
                if (s.lastUsed < oldest) { oldest = s.lastUsed; slot = (int)i; }
            }
            if (slot < 0) return -1;                         // todo en uso: entra más tarde
            m_index.erase(nodeKey(m_slots[(size_t)slot].node));
            ++m_stats.evicted;
        }
        Slot& s = m_slots[(size_t)slot];
        s.used = true; s.pinned = pin; s.node = n; s.range = range; s.lastUsed = m_frame;
        m_index[nodeKey(n)] = slot;
        ++m_stats.generated;
        return slot;
    }

    /** @brief Rango de elevación conocido de un nodo, o inválido. Alimenta el recorte de frustum. */
    NodeRange rangeOf(const NodeId& n) const {
        auto it = m_index.find(nodeKey(n));
        if (it != m_index.end()) return m_slots[(size_t)it->second].range;
        // Herencia: el área del hijo está contenida en la del padre, así que su rango también
        // (medido: los hijos se salen 0,1 % de la amplitud del padre — ver `terrain_node_range`).
        NodeId a = n;
        while (a.level > 0) {
            a.level--; a.i /= 2; a.j /= 2;
            auto ia = m_index.find(nodeKey(a));
            if (ia != m_index.end()) return m_slots[(size_t)ia->second].range;
        }
        return NodeRange{};
    }

    /// Adaptador para `nodeSelectVisible`: `rangeFn = &TerrainNodePool::rangeFnAdapter`.
    static NodeRange rangeFnAdapter(const NodeId& n, void* user) {
        return static_cast<const TerrainNodePool*>(user)->rangeOf(n);
    }

    /** @brief Marca un nodo ya residente como INDESALOJABLE. Para las raíces de cara: son la
     *  garantía de que el fallback por ancestro siempre tiene algo que devolver. */
    bool pin(const NodeId& n) {
        auto it = m_index.find(nodeKey(n));
        if (it == m_index.end()) return false;
        m_slots[(size_t)it->second].pinned = true;
        return true;
    }

    /** @brief Qué nodo ocupa un hueco. Lo necesita quien auditea el pool contra una referencia. */
    bool nodeAtSlot(int slot, NodeId& out) const {
        if (slot < 0 || (size_t)slot >= m_slots.size() || !m_slots[(size_t)slot].used) return false;
        out = m_slots[(size_t)slot].node;
        return true;
    }

    bool   isResident(const NodeId& n) const { return m_index.count(nodeKey(n)) != 0; }
    size_t residentCount() const { return m_index.size(); }
    size_t capacity()      const { return m_capacity; }
    Stats  stats()         const { Stats s = m_stats; s.resident = m_index.size(); s.capacity = m_capacity; return s; }

private:
    struct Slot {
        bool      used = false, pinned = false;
        NodeId    node;
        NodeRange range;
        uint64_t  lastUsed = 0;
    };
    size_t   m_capacity, m_genPerFrame;
    uint64_t m_frame = 0;
    std::vector<Slot> m_slots;
    std::vector<int>  m_free;
    std::unordered_map<uint64_t, int> m_index;
    std::vector<NodeId> m_pending;
    Stats m_stats;
};

/**
 * @brief Nivel del vecino de cada arista, para alimentar el cosido (`nodeStitch`).
 *
 * Devuelve en `outCoarser[4]` cuántos NIVELES más grueso es el vecino de cada arista
 * (0 = mismo nivel o más fino → no hay nada que coser). Aristas: 0=izq, 1=der, 2=abajo, 3=arriba.
 *
 * El vecino se busca en el conjunto SELECCIONADO: si no está a su nivel, se sube por sus ancestros
 * hasta encontrar el que sí se va a dibujar. Eso es exactamente lo que el cosido necesita, porque lo
 * que abre la grieta no es qué nodos existen sino cuáles se DIBUJAN.
 *
 * ⚠️ LIMITACIÓN DECLARADA: solo resuelve vecinos DENTRO DE LA MISMA CARA del cubo. En el borde de una
 * cara el vecino está en otra, con su propia orientación (hay que rotar y a veces intercambiar los
 * ejes), y esa tabla de adyacencia no está escrita. Ahí se devuelve 0 — o sea, no se cose — y puede
 * quedar una grieta en las doce aristas del cubo. Es un caso REAL y visible; se deja anotado en vez
 * de disimulado, y es lo siguiente que hay que cerrar de F3.
 */
inline void nodeNeighbourLevels(const NodeId& n,
                                const std::unordered_map<uint64_t, uint32_t>& drawnLevel,
                                int outCoarser[4]) {
    for (int e = 0; e < 4; ++e) outCoarser[e] = 0;
    const uint64_t lim = 1ull << n.level;
    const int dx[4] = { -1, +1,  0,  0 };
    const int dy[4] = {  0,  0, -1, +1 };
    for (int e = 0; e < 4; ++e) {
        const int64_t ni = (int64_t)n.i + dx[e], nj = (int64_t)n.j + dy[e];
        NodeId nb;
        if (ni < 0 || nj < 0 || ni >= (int64_t)lim || nj >= (int64_t)lim) {
            // ⚠️ CRUZAR DE CARA NO ES UN CASO RARO: son las DOCE ARISTAS DEL CUBO.
            //
            // Aquí había un `continue`, así que a lo largo de todas ellas nadie cosía: el vecino
            // quedaba como "mismo nivel" y las T-junctions abrían grieta. `nodeNeighbourAcrossFace`
            // saca la cara y los índices del vecino de la GEOMETRÍA (ida y vuelta por
            // `cubeFaceToDir`/`dirToCubeFaceClosed`), así que trae su rotación y su reflejo sin que
            // haya una tabla de 24 casos que mantener.
            //
            // El cosido del shader no necesita la orientación: interpola sobre MI arista con MI (u,v)
            // y una zancada que sale de la DIFERENCIA DE NIVEL, y eso es invariante a que el vecino
            // tenga la arista girada o del revés. Lo único que hace falta de él es el nivel.
            if (!nodeNeighbourAcrossFace(n, e, nb)) continue;
        } else {
            nb = NodeId{ n.face, n.level, (uint32_t)ni, (uint32_t)nj };
        }
        // Bajar por ancestros hasta el que de verdad se dibuja.
        for (;;) {
            auto it = drawnLevel.find(nodeKey(nb));
            if (it != drawnLevel.end()) {
                outCoarser[e] = (int)n.level - (int)nb.level;
                if (outCoarser[e] < 0) outCoarser[e] = 0;   // el vecino es MÁS fino: cose él, no yo
                break;
            }
            if (nb.level == 0) break;
            nb.level--; nb.i /= 2; nb.j /= 2;
        }
    }
}

/** @brief Índice `clave -> nivel` del conjunto que se va a dibujar. Lo consume `nodeNeighbourLevels`. */
inline std::unordered_map<uint64_t, uint32_t> nodeDrawnIndex(const std::vector<NodeId>& drawn) {
    std::unordered_map<uint64_t, uint32_t> m;
    m.reserve(drawn.size() * 2);
    for (const NodeId& n : drawn) m[nodeKey(n)] = n.level;
    return m;
}

}} // namespace Haruka::Terrain
