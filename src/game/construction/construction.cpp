/**
 * @file construction.cpp
 * @brief F1 del sistema de construcción: validación de colocación free-form (terreno + solape + anclaje).
 *        Geometría pura en double (OBB-OBB SAT + muestreo del terreno vía IWorldProvider). Sin GL/Jolt/RNG.
 */
#include "game/construction/construction.h"

#include <cmath>
#include <array>
#include <algorithm>

namespace Haruka { namespace Construction {

namespace {

// OBB en double: centro, semiejes, y los 3 ejes locales (columnas de R = mat3 de la orientación).
struct OBB { glm::dvec3 c; glm::dvec3 h; glm::dvec3 ax[3]; };

OBB makeOBB(const glm::dvec3& pos, const glm::dquat& q, const glm::dvec3& half) {
    const glm::dmat3 R = glm::mat3_cast(q);
    OBB o; o.c = pos; o.h = half;
    o.ax[0] = R[0]; o.ax[1] = R[1]; o.ax[2] = R[2];   // glm es column-major: R[i] = eje i
    return o;
}

// SAT OBB-OBB (Ericson, Real-Time Collision Detection). `expand` infla B (para el test de anclaje: un
// pequeño hueco cuenta como "tocando"). Devuelve true si se solapan. Determinista (sin orden dependiente).
bool obbOverlap(const OBB& a, const OBB& b, double expand) {
    double R[3][3], AbsR[3][3];
    const double EPS = 1e-9;   // evita divisiones por ~0 con ejes casi paralelos
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) { R[i][j] = glm::dot(a.ax[i], b.ax[j]); AbsR[i][j] = std::abs(R[i][j]) + EPS; }

    glm::dvec3 tw = b.c - a.c;
    const double t[3] = { glm::dot(tw, a.ax[0]), glm::dot(tw, a.ax[1]), glm::dot(tw, a.ax[2]) };
    const double ah[3] = { a.h.x, a.h.y, a.h.z };
    const double bh[3] = { b.h.x + expand, b.h.y + expand, b.h.z + expand };

    // Ejes de A (L = A0,A1,A2)
    for (int i = 0; i < 3; ++i) {
        const double ra = ah[i];
        const double rb = bh[0]*AbsR[i][0] + bh[1]*AbsR[i][1] + bh[2]*AbsR[i][2];
        if (std::abs(t[i]) > ra + rb) return false;
    }
    // Ejes de B (L = B0,B1,B2)
    for (int j = 0; j < 3; ++j) {
        const double ra = ah[0]*AbsR[0][j] + ah[1]*AbsR[1][j] + ah[2]*AbsR[2][j];
        const double rb = bh[j];
        const double tj = t[0]*R[0][j] + t[1]*R[1][j] + t[2]*R[2][j];
        if (std::abs(tj) > ra + rb) return false;
    }
    // Ejes cruzados Ai × Bj (9)
    // A0×B0
    { double ra=ah[1]*AbsR[2][0]+ah[2]*AbsR[1][0], rb=bh[1]*AbsR[0][2]+bh[2]*AbsR[0][1];
      if (std::abs(t[2]*R[1][0]-t[1]*R[2][0]) > ra+rb) return false; }
    // A0×B1
    { double ra=ah[1]*AbsR[2][1]+ah[2]*AbsR[1][1], rb=bh[0]*AbsR[0][2]+bh[2]*AbsR[0][0];
      if (std::abs(t[2]*R[1][1]-t[1]*R[2][1]) > ra+rb) return false; }
    // A0×B2
    { double ra=ah[1]*AbsR[2][2]+ah[2]*AbsR[1][2], rb=bh[0]*AbsR[0][1]+bh[1]*AbsR[0][0];
      if (std::abs(t[2]*R[1][2]-t[1]*R[2][2]) > ra+rb) return false; }
    // A1×B0
    { double ra=ah[0]*AbsR[2][0]+ah[2]*AbsR[0][0], rb=bh[1]*AbsR[1][2]+bh[2]*AbsR[1][1];
      if (std::abs(t[0]*R[2][0]-t[2]*R[0][0]) > ra+rb) return false; }
    // A1×B1
    { double ra=ah[0]*AbsR[2][1]+ah[2]*AbsR[0][1], rb=bh[0]*AbsR[1][2]+bh[2]*AbsR[1][0];
      if (std::abs(t[0]*R[2][1]-t[2]*R[0][1]) > ra+rb) return false; }
    // A1×B2
    { double ra=ah[0]*AbsR[2][2]+ah[2]*AbsR[0][2], rb=bh[0]*AbsR[1][1]+bh[1]*AbsR[1][0];
      if (std::abs(t[0]*R[2][2]-t[2]*R[0][2]) > ra+rb) return false; }
    // A2×B0
    { double ra=ah[0]*AbsR[1][0]+ah[1]*AbsR[0][0], rb=bh[1]*AbsR[2][2]+bh[2]*AbsR[2][1];
      if (std::abs(t[1]*R[0][0]-t[0]*R[1][0]) > ra+rb) return false; }
    // A2×B1
    { double ra=ah[0]*AbsR[1][1]+ah[1]*AbsR[0][1], rb=bh[0]*AbsR[2][2]+bh[2]*AbsR[2][0];
      if (std::abs(t[1]*R[0][1]-t[0]*R[1][1]) > ra+rb) return false; }
    // A2×B2
    { double ra=ah[0]*AbsR[1][2]+ah[1]*AbsR[0][2], rb=bh[0]*AbsR[2][1]+bh[1]*AbsR[2][0];
      if (std::abs(t[1]*R[0][2]-t[0]*R[1][2]) > ra+rb) return false; }

    return true;   // ningún eje separador → solapan
}

// Contacto con el terreno de la OBB. `maxPen` = mayor (alturaTerreno − alturaEsquina) sobre las 8 esquinas
// (>0 = esquina BAJO el terreno; ≥ −kAnchorGap = apoya). `bottomSpread` = desnivel del terreno bajo la CARA
// INFERIOR (max−min de la penetración en sus 4 esquinas) → mide si el suelo bajo la huella está LLANO.
// Altura RADIAL sobre la esfera. Sin planeta activo → maxPen = −1e300 (no toca).
struct TerrainContact { double maxPen = -1e300; double bottomSpread = 0.0; };
TerrainContact terrainContact(const OBB& box, const Physics::IWorldProvider& world) {
    TerrainContact tc;
    if (!world.hasActivePlanet()) return tc;
    const glm::dvec3 pc = world.activePlanetCenter();
    const double     R  = world.activePlanetRadius();
    double botMin = 1e300, botMax = -1e300;
    for (int s = 0; s < 8; ++s) {
        const glm::dvec3 corner = box.c
            + box.ax[0] * ((s & 1) ? box.h.x : -box.h.x)
            + box.ax[1] * ((s & 2) ? box.h.y : -box.h.y)
            + box.ax[2] * ((s & 4) ? box.h.z : -box.h.z);
        const double alt = glm::length(corner - pc) - R;   // altura de la esquina sobre la esfera ref
        const double pen = world.terrainHeightAt(corner) - alt;
        if (pen > tc.maxPen) tc.maxPen = pen;
        if (!(s & 2)) { if (pen < botMin) botMin = pen; if (pen > botMax) botMax = pen; }   // cara inferior (−y)
    }
    tc.bottomSpread = botMax - botMin;
    return tc;
}

} // namespace

const PieceType* ConstructionState::type(uint16_t id) const {
    for (const auto& t : m_catalog) if (t.id == id) return &t;
    return nullptr;
}

Placement ConstructionState::validate(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient,
                                      const Physics::IWorldProvider& world) const {
    const PieceType* pt = type(typeId);
    if (!pt) return Placement::UnknownType;

    const OBB box = makeOBB(pos, glm::normalize(orient), pt->halfExtents);

    // --- (1) Anclaje al SUELO: la pieza apoya si su esquina más baja está a ≤ kAnchorGap sobre el terreno
    //         (altura RADIAL sobre la esfera). NO se rechaza por hundirse en el terreno: el relieve se
    //         moldea con la HERRAMIENTA de terreno (DeformationField: nivelar/subir/bajar/roller, el mismo
    //         que la tecla G), no rechazando la pieza. maxPen>0 = enterrada (sigue anclada). ---
    const TerrainContact tc = terrainContact(box, world);   // maxPen (apoyo) + desnivel bajo la huella
    const bool onTerrain = (tc.maxPen >= -kAnchorGap);

    // --- (2) Solape con piezas ya colocadas (rechazo) y (3) anclaje a una pieza (a menos de kAnchorGap). ---
    bool onPiece = false;
    for (const auto& kv : m_pieces) {
        const PieceType* ot = type(kv.second.typeId);
        if (!ot) continue;
        const OBB other = makeOBB(kv.second.position, kv.second.orientation, ot->halfExtents);
        if (obbOverlap(box, other, 0.0)) return Placement::OverlapsPiece;      // solape real → ilegal
        if (obbOverlap(box, other, kAnchorGap)) onPiece = true;               // roza → cuenta como anclaje
    }

    // --- (4) Reglas de colocación ("sobre qué"): debe apoyar en un soporte del TIPO que la pieza admite
    //         (bitmask `supports`), y si exige suelo LLANO (`needsFlat`, p.ej. cimentación) el terreno bajo
    //         la huella no puede estar inclinado. ---
    if (!onTerrain && !onPiece) return Placement::NotAnchored;                 // nada debajo
    const bool okTerrain = onTerrain && (pt->supports & kSupportTerrain);
    const bool okPiece   = onPiece   && (pt->supports & kSupportPiece);
    if (!okTerrain && !okPiece) return Placement::WrongSupport;               // hay apoyo, pero no del tipo válido
    if (pt->needsFlat && okTerrain && tc.bottomSpread > kFlatTol) return Placement::TooSteep;
    return Placement::Ok;
}

uint64_t ConstructionState::addRaw(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient) {
    Piece p;
    p.id          = m_nextId++;
    p.typeId      = typeId;
    p.position    = pos;
    p.orientation = glm::normalize(orient);
    p.state       = Piece::State::Anchored;   // viene ya aceptada por el nodo de origen
    m_pieces[p.id] = p;
    return p.id;
}

uint64_t ConstructionState::add(uint16_t typeId, const glm::dvec3& pos, const glm::dquat& orient,
                                const Physics::IWorldProvider& world) {
    Piece p;
    p.id = m_nextId++;
    p.typeId = typeId;
    p.position = pos;
    p.orientation = glm::normalize(orient);
    p.state = Piece::State::Anchored;

    // F2: al colocar, marca si toca el suelo y teje las FIJACIONES (aristas) con las piezas que roza.
    if (const PieceType* pt = type(typeId)) {
        const OBB box = makeOBB(p.position, p.orientation, pt->halfExtents);
        if (terrainContact(box, world).maxPen >= -kAnchorGap) m_grounded.insert(p.id);   // apoya en el suelo
        for (const auto& kv : m_pieces) {
            const PieceType* ot = type(kv.second.typeId);
            if (!ot) continue;
            const OBB other = makeOBB(kv.second.position, kv.second.orientation, ot->halfExtents);
            if (obbOverlap(box, other, kAnchorGap)) {   // roza otra pieza → clavo/tornillo (arista simétrica)
                m_adj[p.id].push_back(kv.first);
                m_adj[kv.first].push_back(p.id);
            }
        }
    }
    m_pieces.emplace(p.id, p);
    relabelStructures();
    return p.id;
}

Placement ConstructionState::validateBatch(const std::vector<Placed>& batch,
                                           const Physics::IWorldProvider& world) const {
    std::vector<OBB> boxes; boxes.reserve(batch.size());
    for (const auto& b : batch) {
        const PieceType* pt = type(b.typeId);
        if (!pt) return Placement::UnknownType;
        boxes.push_back(makeOBB(b.pos, glm::normalize(b.orient), pt->halfExtents));
    }
    bool anyAnchored = false;   // basta con que UNA parte apoye (el resto cuelga del grafo interno)
    for (std::size_t i = 0; i < batch.size(); ++i) {
        for (const auto& kv : m_pieces) {                                    // vs piezas EXISTENTES
            const PieceType* ot = type(kv.second.typeId); if (!ot) continue;
            const OBB other = makeOBB(kv.second.position, kv.second.orientation, ot->halfExtents);
            if (obbOverlap(boxes[i], other, 0.0)) return Placement::OverlapsPiece;
            if (!anyAnchored && obbOverlap(boxes[i], other, kAnchorGap)) anyAnchored = true;
        }
        for (std::size_t j = 0; j < i; ++j)                                  // vs otras partes del lote
            if (obbOverlap(boxes[i], boxes[j], 0.0)) return Placement::OverlapsPiece;
        if (!anyAnchored && terrainContact(boxes[i], world).maxPen >= -kAnchorGap) anyAnchored = true;
    }
    if (!anyAnchored) return Placement::NotAnchored;
    return Placement::Ok;
}

std::vector<uint64_t> ConstructionState::addBatch(const std::vector<Placed>& batch,
                                                  const Physics::IWorldProvider& world) {
    std::vector<uint64_t> ids; ids.reserve(batch.size());
    // add() ya teje por proximidad las fijaciones con lo existente Y con las partes ya añadidas del lote.
    for (const auto& b : batch) ids.push_back(add(b.typeId, b.pos, b.orient, world));
    return ids;
}

const std::vector<uint64_t>& ConstructionState::fasteners(uint64_t pieceId) const {
    static const std::vector<uint64_t> kEmpty;
    auto it = m_adj.find(pieceId);
    return it == m_adj.end() ? kEmpty : it->second;
}

bool ConstructionState::isSupported(uint64_t pieceId) const {
    if (!m_pieces.count(pieceId)) return false;
    // Raíces de soporte: tocar el TERRENO o ser una ZONA PROTEGIDA (ancla del mundo).
    if (m_grounded.count(pieceId) || m_protected.count(pieceId)) return true;
    // BFS por el grafo de fijaciones; soportada si alcanza una raíz. Indep. del orden.
    std::unordered_set<uint64_t> seen{pieceId};
    std::vector<uint64_t> stack{pieceId};
    while (!stack.empty()) {
        const uint64_t cur = stack.back(); stack.pop_back();
        auto ai = m_adj.find(cur);
        if (ai == m_adj.end()) continue;
        for (uint64_t nb : ai->second) {
            if (seen.count(nb)) continue;
            if (m_grounded.count(nb) || m_protected.count(nb)) return true;
            seen.insert(nb); stack.push_back(nb);
        }
    }
    return false;
}

std::vector<uint64_t> ConstructionState::structureComponent(uint64_t pieceId) const {
    std::vector<uint64_t> out;
    if (!m_pieces.count(pieceId)) return out;
    std::unordered_set<uint64_t> seen{pieceId};
    std::vector<uint64_t> stack{pieceId};
    while (!stack.empty()) {
        const uint64_t cur = stack.back(); stack.pop_back();
        out.push_back(cur);
        auto ai = m_adj.find(cur);
        if (ai == m_adj.end()) continue;
        for (uint64_t nb : ai->second)
            if (!seen.count(nb) && m_pieces.count(nb)) { seen.insert(nb); stack.push_back(nb); }
    }
    std::sort(out.begin(), out.end());   // determinista
    return out;
}

std::size_t ConstructionState::fastenerCount() const {
    std::size_t deg = 0;
    for (const auto& kv : m_adj) deg += kv.second.size();
    return deg / 2;   // cada arista se cuenta en sus dos extremos
}

// ── MECANISMOS, MASA y DISTANCIA (lo que un vehiculo pide de mas que un edificio) ───────────────

namespace { inline std::pair<uint64_t,uint64_t> edgeKey(uint64_t a, uint64_t b) {
    return a < b ? std::make_pair(a,b) : std::make_pair(b,a);
} }

bool ConstructionState::setJointKind(uint64_t a, uint64_t b, JointKind kind) {
    if (!m_pieces.count(a) || !m_pieces.count(b)) return false;
    // Si aun no estaban unidas, unirlas: montar un mecanismo ENTRE dos piezas implica que se tocan.
    auto& va = m_adj[a];
    if (std::find(va.begin(), va.end(), b) == va.end()) {
        va.push_back(b);
        m_adj[b].push_back(a);
    }
    if (kind == JointKind::Mechanism) m_mechanisms.insert(edgeKey(a, b));
    else                              m_mechanisms.erase(edgeKey(a, b));
    relabelStructures();
    return true;
}

ConstructionState::JointKind ConstructionState::jointKind(uint64_t a, uint64_t b) const {
    return m_mechanisms.count(edgeKey(a, b)) ? JointKind::Mechanism : JointKind::Rigid;
}

std::vector<uint64_t> ConstructionState::rigidIsland(uint64_t pieceId) const {
    std::vector<uint64_t> out;
    if (!m_pieces.count(pieceId)) return out;
    std::unordered_set<uint64_t> seen{pieceId};
    std::vector<uint64_t> stack{pieceId};
    while (!stack.empty()) {
        const uint64_t cur = stack.back(); stack.pop_back();
        out.push_back(cur);
        auto ai = m_adj.find(cur);
        if (ai == m_adj.end()) continue;
        for (uint64_t nb : ai->second) {
            // La UNICA diferencia con `structureComponent`: un mecanismo no propaga la isla.
            if (m_mechanisms.count(edgeKey(cur, nb))) continue;
            if (!seen.count(nb) && m_pieces.count(nb)) { seen.insert(nb); stack.push_back(nb); }
        }
    }
    std::sort(out.begin(), out.end());   // determinista
    return out;
}

ConstructionState::MassProps ConstructionState::islandMass(uint64_t pieceId) const {
    MassProps mp;
    glm::dvec3 acc(0.0);
    for (uint64_t id : rigidIsland(pieceId)) {
        auto it = m_pieces.find(id); if (it == m_pieces.end()) continue;
        const PieceType* pt = type(it->second.typeId); if (!pt) continue;
        const double m = (double)pt->mass;
        acc += it->second.position * m;
        mp.massKg += m;
    }
    if (mp.massKg > 0.0) mp.centerOfMass = acc / mp.massKg;
    return mp;
}

std::unordered_map<uint64_t, int> ConstructionState::distancesFrom(uint64_t from, int maxDepth) const {
    std::unordered_map<uint64_t, int> dist;
    if (!m_pieces.count(from)) return dist;
    dist[from] = 0;
    std::vector<uint64_t> frontier{from};
    int depth = 0;
    while (!frontier.empty() && (maxDepth < 0 || depth < maxDepth)) {
        ++depth;
        std::vector<uint64_t> next;
        for (uint64_t cur : frontier) {
            auto ai = m_adj.find(cur);
            if (ai == m_adj.end()) continue;
            // ⚠️ Aqui NO se filtran mecanismos: la adyacencia es de MONTAJE. Un condensador colgado
            // de una bisagra sigue estando al lado de la camara.
            for (uint64_t nb : ai->second)
                if (m_pieces.count(nb) && !dist.count(nb)) { dist[nb] = depth; next.push_back(nb); }
        }
        // Orden estable: `m_adj` guarda vectores, pero el frontier se arma desde varios; ordenar
        // deja el recorrido identico entre ejecuciones (el DGS valida con el MISMO codigo).
        std::sort(next.begin(), next.end());
        frontier.swap(next);
    }
    return dist;
}

int ConstructionState::pieceDistance(uint64_t from, uint64_t to) const {
    const auto d = distancesFrom(from);
    auto it = d.find(to);
    return it == d.end() ? -1 : it->second;
}

void ConstructionState::relabelStructures() {
    // Componentes conexas del grafo de fijaciones. Etiqueta DETERMINISTA: recorre las piezas en orden
    // ascendente de id; cada componente nueva recibe la siguiente etiqueta (BFS sobre m_adj).
    std::vector<uint64_t> ids;
    ids.reserve(m_pieces.size());
    for (const auto& kv : m_pieces) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    std::unordered_set<uint64_t> seen;
    uint32_t label = 0;
    for (uint64_t start : ids) {
        if (seen.count(start)) continue;
        ++label;
        std::vector<uint64_t> stack{start};
        seen.insert(start);
        while (!stack.empty()) {
            const uint64_t cur = stack.back(); stack.pop_back();
            auto it = m_pieces.find(cur);
            if (it != m_pieces.end()) it->second.structureId = label;
            auto ai = m_adj.find(cur);
            if (ai == m_adj.end()) continue;
            for (uint64_t nb : ai->second)
                if (!seen.count(nb) && m_pieces.count(nb)) { seen.insert(nb); stack.push_back(nb); }
        }
    }
}

void ConstructionState::erasePiece(uint64_t id) {
    auto ai = m_adj.find(id);
    if (ai != m_adj.end()) {
        // Las aristas de MECANISMO de esta pieza se van con ella: si no, quedarian entradas colgando
        // que ya no corresponden a ninguna arista y la isla rigida se calcularia sobre basura.
        for (uint64_t nb : ai->second) m_mechanisms.erase(edgeKey(id, nb));
        for (uint64_t nb : ai->second) {                 // borra la arista recíproca en cada vecino
            auto ni = m_adj.find(nb);
            if (ni == m_adj.end()) continue;
            auto& v = ni->second;
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
        }
        m_adj.erase(ai);
    }
    m_pieces.erase(id);
    m_grounded.erase(id);
    m_protected.erase(id);
}

void ConstructionState::setProtected(uint64_t pieceId, bool prot) {
    if (!m_pieces.count(pieceId)) return;
    if (prot) m_protected.insert(pieceId); else m_protected.erase(pieceId);
}

std::vector<uint64_t> ConstructionState::breakPiece(uint64_t pieceId) {
    std::vector<uint64_t> collapsed;
    if (!m_pieces.count(pieceId) || m_protected.count(pieceId)) return collapsed;  // no existe / irrompible
    erasePiece(pieceId);

    // Una sola pasada capta TODA la cascada: si el único camino a una raíz pasaba por una pieza sin soporte,
    // esa pieza queda también sin soporte en el MISMO cálculo (isSupported se evalúa sobre el grafo ya sin la
    // rota). Las protegidas nunca salen (isSupported las trata como raíz).
    for (const auto& kv : m_pieces)
        if (!isSupported(kv.first)) collapsed.push_back(kv.first);
    for (uint64_t c : collapsed) erasePiece(c);           // quita las que cayeron

    std::sort(collapsed.begin(), collapsed.end());        // determinista
    relabelStructures();
    return collapsed;
}

// ---- FIXTURES: la regla de dónde van puerta/ventanas en un muro (GL-free, determinista) ----
std::vector<Opening> wallOpenings(double wallW, double wallH, bool isFrontGround,
                                  double doorW, double doorH, double winW, double winH,
                                  double sill, double baySpacing) {
    std::vector<Opening> out;
    const bool hasDoor = isFrontGround && doorW > 0.0 && doorH > 0.0 && doorH < wallH;
    if (hasDoor) {   // PUERTA centrada y apoyada en la base del muro
        Opening d; d.kind = FixtureKind::Door;
        d.center      = glm::dvec2(0.0, doorH * 0.5);
        d.halfExtents = glm::dvec2(doorW * 0.5, doorH * 0.5);
        out.push_back(d);
    }
    // VENTANAS en el ritmo `baySpacing`, a la altura de alféizar, evitando la puerta.
    if (winW > 0.0 && winH > 0.0 && baySpacing > 1e-3 && sill + winH < wallH) {
        const int n = (int)std::floor((wallW - 1.0) / baySpacing);   // deja margen en los extremos
        for (int i = 0; i < n; ++i) {
            const double x = -wallW * 0.5 + (double(i) + 0.5) * (wallW / double(n));
            if (hasDoor && std::abs(x) < doorW * 0.5 + winW * 0.5 + 0.15) continue; // no pisar la puerta
            Opening w; w.kind = FixtureKind::Window;
            w.center      = glm::dvec2(x, sill + winH * 0.5);
            w.halfExtents = glm::dvec2(winW * 0.5, winH * 0.5);
            out.push_back(w);
        }
    }
    return out;
}

bool fixtureFits(const Opening& op, const glm::dvec2& fixtureHalf) {
    const double tol = 0.02;   // el fixture cabe si no es MÁS ancho/alto que el hueco (con holgura)
    return fixtureHalf.x <= op.halfExtents.x + tol && fixtureHalf.y <= op.halfExtents.y + tol;
}

BuildingLayout buildingLayout(int floors, double W, double D, double wallT, double floorH,
                              double doorW, double doorH, double winW, double winH,
                              double sill, double baySpacing) {
    BuildingLayout L;
    L.floors      = std::max(1, floors);
    L.footprintX  = W;
    L.footprintZ  = D;
    L.floorHeight = floorH;
    const double hx = W * 0.5, hz = D * 0.5;
    // Los 4 muros en planta (extremos a→b, y su ancho): frente z=+hz, atrás z=-hz, izq x=-hx, der x=+hx.
    struct Side { glm::dvec2 a, b; bool isFront; };
    const Side sides[4] = {
        { { -hx,  hz }, {  hx,  hz }, true  },   // FRENTE (+z): lleva la puerta en planta baja
        { {  hx, -hz }, { -hx, -hz }, false },   // ATRÁS  (-z)
        { { -hx, -hz }, { -hx,  hz }, false },   // IZQUIERDA (-x)
        { {  hx,  hz }, {  hx, -hz }, false },   // DERECHA  (+x)
    };
    for (int f = 0; f < L.floors; ++f) {
        const double baseY = double(f) * floorH;
        for (const Side& s : sides) {
            WallRun w;
            w.a = s.a; w.b = s.b;
            w.baseY = baseY; w.height = floorH; w.thickness = wallT;
            w.isFrontGround = s.isFront && f == 0;
            const double wallW = glm::length(s.b - s.a);
            w.openings = wallOpenings(wallW, floorH, w.isFrontGround,
                                      doorW, doorH, winW, winH, sill, baySpacing);
            L.walls.push_back(std::move(w));
        }
    }
    return L;
}

std::vector<WallSolid> wallSolids(double wallW, double wallH, const std::vector<Opening>& openings) {
    std::vector<WallSolid> out;
    if (wallW <= 1e-6 || wallH <= 1e-6) return out;
    const double hW = wallW * 0.5;

    // Solo las PUERTAS abren paso. Se ordenan por x para barrer el muro de un extremo al otro.
    struct Door { double x0, x1, top; };
    std::vector<Door> doors;
    for (const Opening& op : openings)
        if (op.kind == FixtureKind::Door)
            doors.push_back({ op.center.x - op.halfExtents.x,
                              op.center.x + op.halfExtents.x,
                              op.center.y + op.halfExtents.y });
    std::sort(doors.begin(), doors.end(), [](const Door& a, const Door& b){ return a.x0 < b.x0; });

    auto emit = [&](double x0, double x1, double y0, double y1) {
        if (x1 - x0 < 1e-6 || y1 - y0 < 0.05) return;      // trozos residuales: no valen como colisión
        WallSolid s;
        s.center      = glm::dvec2((x0 + x1) * 0.5, (y0 + y1) * 0.5);
        s.halfExtents = glm::dvec2((x1 - x0) * 0.5, (y1 - y0) * 0.5);
        out.push_back(s);
    };

    double cursor = -hW;
    for (const Door& d : doors) {
        const double x0 = std::max(d.x0, -hW), x1 = std::min(d.x1, hW);
        if (x1 <= cursor) continue;                         // puerta ya cubierta por otra
        emit(cursor, x0, 0.0, wallH);                       // macizo ANTES de la puerta
        emit(x0, x1, std::min(d.top, wallH), wallH);        // DINTEL sobre la puerta
        cursor = x1;
    }
    emit(cursor, hW, 0.0, wallH);                           // macizo hasta el extremo
    return out;
}

}} // namespace Haruka::Construction
