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
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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
        /// Nodos TOCADOS este frame (hojas pedidas + toda su cadena de ancestros). Es exactamente lo
        /// que el pool tiene que sostener a la vez: si `capacity` no llega, hay nodos que NO CABEN y
        /// la caida por ancestro se dispara sin que ninguna politica de desalojo pueda evitarlo.
        size_t live = 0;
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

    /// Política de cadena: encolar también los ancestros no residentes de lo que se pide, y
    /// generarlos ANTES que sus hijos. Encendida por defecto; apagarla reproduce el comportamiento
    /// anterior al 2026-08-25 y existe para que el test pueda medir el A/B en vez de afirmarlo.
    void setChainAncestors(bool on) { m_chainAncestors = on; }
    bool chainAncestors() const { return m_chainAncestors; }

    void beginFrame() {
        ++m_frame;
        m_pending.clear();
        m_pendingSet.clear();
        m_stats.hitsExact = m_stats.hitsAncestor = m_stats.misses = 0;
        m_stats.live = 0;
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
     *
     * ── ⚠️ Y POR QUÉ SE PIDE TAMBIÉN LA CADENA DE ANCESTROS ────────────────────────────────────────
     *
     * El selector solo pide **HOJAS**. Un nodo interior quedaba residente solo por accidente —porque
     * fue hoja en algún frame anterior— así que la caída por ancestro podía saltarse varios niveles de
     * golpe: medido, hasta **4 niveles**, o sea 1,372 m de escalón donde el geomorph solo sabe cerrar
     * uno (apunta al PADRE). Eso son los pinchos por caída profunda.
     *
     * Aquí cada eslabón que falta de la cadena raíz→n se encola también. El conjunto residente pasa a
     * ser un SUBÁRBOL CONEXO desde la raíz, y entonces el ancestro más profundo de cualquier hoja
     * pedida es su padre — que es exactamente la hipótesis con la que el geomorph está escrito.
     *
     * Y los ancestros hay que TOCARLOS además de tenerlos: un padre al que nadie pide se vuelve el
     * menos recientemente usado y el LRU se lo lleva, rompiendo la cadena justo donde importa.
     */
    Resolved request(const NodeId& n) {
        auto it = m_index.find(nodeKey(n));
        if (it != m_index.end()) {
            Slot& s = m_slots[(size_t)it->second];
            if (s.lastUsed != m_frame) ++m_stats.live;
            s.lastUsed = m_frame;
            touchAncestors(n);
            ++m_stats.hitsExact;
            return Resolved{ it->second, true, n };
        }
        // No está: encolar (con presupuesto) y buscar ancestro.
        ++m_stats.misses;
        ++m_stats.live;               // no esta, pero hace falta: cuenta para la capacidad
        // ⚠️ SE ENCOLAN TODOS, Y EL PRESUPUESTO SE APLICA DESPUES DE ORDENAR. Antes se cortaba aquí,
        // así que los que se generaban eran los que el recorrido visitó primero — un orden espacial
        // arbitrario, no el de lo que se nota. Ver `prioritisePending`.
        enqueue(n);
        NodeId a = n;
        while (a.level > 0) {
            a.level--; a.i /= 2; a.j /= 2;
            auto ia = m_index.find(nodeKey(a));
            if (ia != m_index.end()) {
                if (m_slots[(size_t)ia->second].lastUsed != m_frame) ++m_stats.live;
                m_slots[(size_t)ia->second].lastUsed = m_frame;
                touchAncestors(a);
                ++m_stats.hitsAncestor;
                return Resolved{ ia->second, false, a };
            }
            if (m_chainAncestors) enqueue(a);   // el eslabón que falta: sin él la caída salta niveles
        }
        return Resolved{ -1, false, n };
    }

    /** @brief Nodos a generar este frame (ya acotados por `genPerFrame`). */
    /**
     * @brief Ordena la cola de generación por CERCANÍA y la recorta al presupuesto.
     *
     * ⚠️ ES LO QUE HACE QUE GIRAR NO SE VEA BASTO. Medido: girar 90° pide **935 nodos nuevos de
     * 3 616 (26 %)**, y con 170 por frame eso son ~6 frames. Durante esos frames los que faltan se
     * dibujan por ANCESTRO, o sea más gruesos — que es la "disparidad al mover la cámara".
     *
     * No se puede evitar la espera sin más VRAM, pero SÍ se puede elegir qué se resuelve primero. Lo
     * que se nota es lo cercano: un nodo a 200 m ocupa media pantalla, uno a 100 km son cuatro
     * píxeles. Antes el orden era el del recorrido del quadtree — espacial y arbitrario.
     */
    /**
     * ⚠️ Y EL PADRE ENTRA ANTES QUE EL HIJO, aunque el hijo esté más cerca. La distancia sigue
     * mandando en QUÉ zona se resuelve primero, pero dentro de una zona el orden es de grueso a fino:
     * generar una hoja cuyo padre no está residente no arregla nada —la hoja de al lado seguirá
     * cayendo varios niveles— mientras que el padre cierra el salto para sus cuatro hijos a la vez.
     */
    void prioritisePending(const glm::dvec3& camPos, const glm::dvec3& planetCenter,
                           double planetRadiusM) {
        if (m_pending.empty()) return;
        std::sort(m_pending.begin(), m_pending.end(),
                  [&](const NodeId& a, const NodeId& b) {
                      const glm::dvec3 pa = planetCenter +
                          nodeTexelDir(a, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2) * planetRadiusM;
                      const glm::dvec3 pb = planetCenter +
                          nodeTexelDir(b, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2) * planetRadiusM;
                      // Cuadrado a mano: `glm::length2` vive en una extension experimental
                      // de GLM y no vale la pena arrastrarla por una comparacion.
                      const glm::dvec3 da = pa - camPos, db = pb - camPos;
                      return glm::dot(da, da) < glm::dot(db, db);
                  });
        if (!m_chainAncestors) {                       // comportamiento anterior al 2026-08-25
            if (m_pending.size() > m_genPerFrame) m_pending.resize(m_genPerFrame);
            return;
        }
        // Recorrido por cercanía, pero emitiendo la cadena raíz→nodo de lo que aún no es residente,
        // del más grueso al más fino. Así el presupuesto nunca produce un hijo huérfano.
        // ── LA DISTANCIA MANDA SOBRE EL PRESUPUESTO ─────────────────────────────────────────────
        //
        // ⚠️ LA FIDELIDAD NO PUEDE DEPENDER DE UNA TASA DE STREAMING. El presupuesto es transitorio:
        // limita cuantos nodos se hornean por frame. Que un nodo A TUS PIES se dibuje con el mapa de
        // un ancestro —un nivel mas basto, hasta 1,372 m de escalon contra la colision— porque ese
        // frame se agoto el cupo no es un compromiso razonable, es un fallo. Lejos, basto un par de
        // frames no se nota; cerca, si.
        //
        // Asi que lo CERCANO se genera aunque el cupo se haya acabado, y el cupo pasa a gobernar solo
        // lo lejano. El coste extra esta acotado por AREA, no por la vista entera: dentro de este
        // radio caben pocos nodos, y ademas solo se pagan cuando de verdad faltan (al entrar en una
        // zona nueva), no en cada frame.
        const double kAlwaysM = 512.0;   // radio dentro del cual la distancia gana al presupuesto
        auto distOf = [&](const NodeId& n) {
            const glm::dvec3 p = planetCenter + nodeTexelDir(n, TERRAIN_NODE_CELLS / 2,
                                                             TERRAIN_NODE_CELLS / 2) * planetRadiusM;
            return glm::length(p - camPos);
        };
        std::vector<NodeId> out; out.reserve(std::min(m_pending.size(), m_genPerFrame));
        std::unordered_set<uint64_t> taken;
        std::vector<NodeId> chain;
        for (const NodeId& n : m_pending) {
            // `continue`, no `break`: la cola va ordenada por cercania, pero la cadena de ancestros
            // puede colar nodos mas lejanos por delante. Cortar en seco dejaria fuera a un vecino
            // cercano que venia detras.
            if (out.size() >= m_genPerFrame && distOf(n) > kAlwaysM) continue;
            chain.clear();
            for (NodeId a = n;; ) {
                const uint64_t k = nodeKey(a);
                if (m_index.find(k) == m_index.end() && taken.find(k) == taken.end())
                    chain.push_back(a);
                if (a.level == 0) break;
                a.level--; a.i /= 2; a.j /= 2;
            }
            for (auto ic = chain.rbegin(); ic != chain.rend(); ++ic) {   // grueso -> fino
                // ⚠️ EL CORTE INTERIOR SI ES DURO, y el override de distancia NO se aplica aqui.
                // Se probo dejarlo abierto para los nodos cercanos y es PEOR: cada nodo cercano puede
                // emitir su cadena entera de ancestros, `out` crece sin tope y el generador —que si
                // corta en el presupuesto— se queda con los primeros N, que pasan a ser ancestros
                // gruesos en vez de las hojas finas que hacen falta. El presupuesto se gasta en lo
                // que no se ve.
                if (out.size() >= m_genPerFrame) break;
                if (taken.insert(nodeKey(*ic)).second) out.push_back(*ic);
            }
        }
        m_pending.swap(out);
    }

    const std::vector<NodeId>& takePending() const { return m_pending; }
    /// Nodos que se pueden generar por frame. Lo aplican `prioritisePending` y el generador.
    size_t genPerFrame() const { return m_genPerFrame; }

    /**
     * @brief Cambia el presupuesto de generacion de este frame. Ver `TerrainNodeRenderer::draw`.
     *
     * ⚠️ EL PRESUPUESTO ES UN INTERCAMBIO, Y EL COSTE SOLO SE PAGA CUANDO HAY COLA. Medido en el
     * propio motor: 0,0555 ms por nodo, o sea 85 nodos = 4,7 ms de frame y 170 = 9,4 ms. Se eligio
     * 85 para no pagar los 9,4 SIEMPRE — correcto, porque en reposo no hay nada que generar y ese
     * gasto seria puro desperdicio.
     *
     * Pero la factura de esa eleccion se paga al GIRAR: girar 90 grados pide 935 nodos nuevos, que a
     * 85 por frame son 11 frames dibujando por ANCESTRO (a 170 serian 6). Y medido en partida, el
     * 98 % de esas caidas son de dos niveles o mas —1 183 de 1 203— con la mas honda en SIETE, que es
     * un escalon de mas de un metro contra el vecino.
     *
     * La salida es que el numero no sea fijo: en reposo la cola esta vacia y da igual cuanto valga,
     * y en la rafaga es cuando compensa gastar. Asi el coste alto dura lo que dura la rafaga.
     */
    void setGenPerFrame(size_t n) { m_genPerFrame = n; }

    /** @brief Cuantos nodos esperan generacion. Es la senal para dosificar el presupuesto. */
    size_t pendingCount() const { return m_pending.size(); }

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
            // ⚠️ SE PROBO A PROTEGER AQUI A LOS NODOS CON HIJOS RESIDENTES —para que el LRU no pudiera
            // romper la cadena raiz->hoja— Y ES UN NO-OP: `touchAncestors` marca toda la cadena con
            // `lastUsed = m_frame` y la linea de abajo ya rechaza cualquier hueco usado este frame,
            // asi que el padre que hace falta es inmune de serie. La regla saltaba 146-205 huecos en
            // el barrido, ninguno era el elegido, y el resultado salia identico al ultimo digito.
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
        // ⚠️ Y SUS ANCESTROS TAMBIEN CUENTAN COMO USADOS. `touchAncestors` corta en cuanto encuentra
        // un eslabon ya marcado con este frame —da por hecho que entonces los de arriba tambien lo
        // estan—, y esa suposicion la rompia justo esta linea de arriba: un nodo recien publicado
        // queda con `lastUsed = m_frame` SIN que nadie haya tocado a su padre. La siguiente hoja que
        // sube por la cadena se para aqui, el padre se queda con la marca vieja, y el LRU se lo lleva.
        //
        // Se midio en el juego contando los desalojos que se llevaban un nodo CON hijos residentes:
        // **358-1303 por ventana de 120 frames**. ⚠️ El banco headless daba CERO —regenera todo cada
        // frame y el orden nunca lo destapa—, asi que la conclusion salio del juego, no del test.
        touchAncestors(n);
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

    /**
     * @brief Hueco de un nodo si esta residente, o -1. NO toca el LRU.
     *
     * Lo usa el renderer para que el morph lea el mapa del PADRE de su propio hueco en vez de una
     * copia guardada dentro del hueco del hijo. Sin `touch` a proposito: quien mantiene viva la cadena
     * es `request` (via `touchAncestors`), y contar tambien esta consulta falsearia el LRU.
     */
    int slotOf(const NodeId& n) const {
        const auto it = m_index.find(nodeKey(n));
        return (it == m_index.end()) ? -1 : it->second;
    }
    size_t capacity()      const { return m_capacity; }
    Stats  stats()         const { Stats s = m_stats; s.resident = m_index.size(); s.capacity = m_capacity; return s; }

private:
    /// Encola sin duplicar: un ancestro lo piden hasta 4^k descendientes y sin esto se comería el
    /// presupuesto consigo mismo.
    void enqueue(const NodeId& n) {
        if (m_pendingSet.insert(nodeKey(n)).second) m_pending.push_back(n);
    }

    /// Marca la cadena de ancestros como usada ESTE frame, para que el LRU no la desaloje: nadie los
    /// pide directamente (el selector solo pide hojas) y sin esto el padre es siempre el candidato
    /// más viejo. Para en cuanto encuentra uno ya tocado: ese ya subió el resto de la cadena.
    void touchAncestors(NodeId a) {
        while (a.level > 0) {
            a.level--; a.i /= 2; a.j /= 2;
            auto ia = m_index.find(nodeKey(a));
            if (ia == m_index.end()) continue;
            Slot& s = m_slots[(size_t)ia->second];
            if (s.lastUsed == m_frame) break;
            s.lastUsed = m_frame; ++m_stats.live;
        }
    }

    struct Slot {
        bool      used = false, pinned = false;
        NodeId    node;
        NodeRange range;
        uint64_t  lastUsed = 0;
    };
    size_t   m_capacity;
    size_t   m_genPerFrame;   // no const: lo dosifica el renderer (ver setGenPerFrame)
    bool     m_chainAncestors = true;
    uint64_t m_frame = 0;
    std::vector<Slot> m_slots;
    std::vector<int>  m_free;
    std::unordered_map<uint64_t, int> m_index;
    std::vector<NodeId> m_pending;
    std::unordered_set<uint64_t> m_pendingSet;   ///< dedup de `m_pending` dentro del frame
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
                                int outCoarser[4],
                                NodeId* outNb = nullptr) {
    for (int e = 0; e < 4; ++e) outCoarser[e] = 0;
    // ⚠️ El vecino por defecto es UNO MISMO, no un nodo inválido: quien lo consuma le va a preguntar
    // el stride, y un `NodeId{}` sin rellenar es la cara 0 nivel 0 — el nodo más grueso del planeta,
    // que pediría coser contra una zancada gigante en las aristas donde NO hay vecino dibujado.
    if (outNb) for (int e = 0; e < 4; ++e) outNb[e] = n;
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
                if (outNb) outNb[e] = nb;
                break;
            }
            if (nb.level == 0) break;
            nb.level--; nb.i /= 2; nb.j /= 2;
        }
    }
}

/**
 * @brief Cuántos niveles tiene que BAJAR `n` para que ningún vecino dibujado le saque más de uno.
 *
 * ── LA CONDICIÓN 2:1, Y POR QUÉ NO BASTA CON QUE EL SELECTOR LA CUMPLA ──────────────────────────
 *
 * El cosido coloca la arista sobre la recta del vecino grueso, pero el geomorph que cierra el
 * escalón de RELIEVE apunta a la altura del **padre** — y eso solo vale si el vecino ESTÁ al nivel
 * del padre, o sea un nivel de diferencia. Con dos o más, el padre no es el vecino y queda un
 * escalón: medido, **0,339 m a dos niveles y 1,372 m a cuatro**. Eso se ve como pinchos.
 *
 * El árbol que devuelve `nodeSelectVisible` ya es 2:1 (medido: 0 saltos de más de uno a cualquier
 * presupuesto). El que se DIBUJA no, porque un nodo sin hueco en el pool cae a un ancestro y puede
 * caer varios niveles de golpe. Por eso la condición hay que reimponerla sobre el conjunto dibujado.
 *
 * Devuelve 0 si no hay nada que hacer. Es la ÚNICA definición de la regla: la usan el renderer (que
 * además tiene que pedirle el ancestro al pool) y el test que la verifica.
 */
inline int nodeBalanceDrop(const NodeId& n,
                           const std::unordered_map<uint64_t, uint32_t>& drawnLevel) {
    if (n.level == 0) return 0;
    int c[4]; NodeId nb[4];
    nodeNeighbourLevels(n, drawnLevel, c, nb);
    int drop = 0;
    for (int e = 0; e < 4; ++e) if (c[e] > 1) drop = std::max(drop, c[e] - 1);
    return drop;
}

/**
 * @brief Aristas de `n` cuyo vecino DIBUJADO es MÁS FINO. Bits: 1=izq 2=der 4=abajo 8=arriba.
 *
 * ⚠️ HACE FALTA PORQUE `nodeNeighbourLevels` NO PUEDE CONTESTAR ESTO. Aquella sube por los ancestros
 * hasta encontrar uno dibujado, y nunca baja: por construcción devuelve siempre un vecino de nivel
 * MENOR O IGUAL. Su `if (outCoarser[e] < 0) outCoarser[e] = 0` sugiere que el caso existía, pero no
 * puede darse — así que preguntar «¿es más fino?» por ahí devuelve siempre «no», en silencio.
 *
 * Me costó una medida: una máscara construida con `nb[e].level > n.level` salió SIEMPRE 0 y el
 * arreglo del escalón no movió un decimal. El síntoma de una consulta imposible es un cero perfecto.
 *
 * Aquí se mira hacia ABAJO: si alguno de los cuatro hijos del vecino de mi nivel está dibujado,
 * entonces esa arista linda con terreno más fino. Basta con un nivel: el selector garantiza 2:1.
 */
inline int nodeNeighbourFinerMask(const NodeId& n,
                                  const std::unordered_map<uint64_t, uint32_t>& drawnLevel) {
    int mask = 0;
    const int64_t lim = (int64_t)1 << n.level;
    const int dx[4] = { -1, +1,  0,  0 };
    const int dy[4] = {  0,  0, -1, +1 };
    for (int e = 0; e < 4; ++e) {
        const int64_t ni = (int64_t)n.i + dx[e], nj = (int64_t)n.j + dy[e];
        if (ni < 0 || nj < 0 || ni >= lim || nj >= lim) continue;   // el cruce de cara, aparte
        const NodeId nbSame{ n.face, n.level, (uint32_t)ni, (uint32_t)nj };
        if (drawnLevel.find(nodeKey(nbSame)) != drawnLevel.end()) continue;   // mismo nivel
        for (uint32_t c = 0; c < 4 && !(mask & (1 << e)); ++c) {
            const NodeId child{ n.face, n.level + 1,
                                nbSame.i * 2 + (c & 1u), nbSame.j * 2 + (c >> 1) };
            if (drawnLevel.find(nodeKey(child)) != drawnLevel.end()) mask |= (1 << e);
        }
    }
    return mask;
}

/** @brief Índice `clave -> nivel` del conjunto que se va a dibujar. Lo consume `nodeNeighbourLevels`. */
inline std::unordered_map<uint64_t, uint32_t> nodeDrawnIndex(const std::vector<NodeId>& drawn) {
    std::unordered_map<uint64_t, uint32_t> m;
    m.reserve(drawn.size() * 2);
    for (const NodeId& n : drawn) m[nodeKey(n)] = n.level;
    return m;
}

}} // namespace Haruka::Terrain
