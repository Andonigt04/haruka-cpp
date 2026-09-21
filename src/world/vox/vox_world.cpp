/**
 * @file vox_world.cpp
 * @brief El campo volumétrico del mundo por chunks. Ver `vox_world.h`.
 */
#include "world/vox/vox_world.h"
#include "world/terrain/bake_util.h"

#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace Haruka {

using BakeUtil::dirToFaceUV;
using BakeUtil::faceUVToDir;

// ── VoxChunk ────────────────────────────────────────────────────────────────────────────────────

glm::dvec3 VoxChunk::worldOfGrid(double x, double y, double z) const {
    const double k = 1.0 / (double)(kVoxN - 1);
    const double u = u0 + (u1 - u0) * (x * k), v = v0 + (v1 - v0) * (y * k);
    return faceUVToDir(key.face, u, v) * (baseR + z * k * kVoxChunkM);
}

glm::dvec3 VoxChunk::gridOf(const glm::dvec3& p) const {
    const double r = glm::length(p);
    int face; double u, v;
    dirToFaceUV(p / std::max(r, 1e-9), face, u, v);
    // Si el punto cae en otra cara del cubo (sólo pasa en el delantal de un chunk pegado a una
    // arista), se proyecta a ESTA cara: la misma dirección tiene (u,v) fuera de [-1,1] aquí.
    if (face != key.face) {
        const glm::dvec3 d = p / std::max(r, 1e-9);
        double a = 0, b = 0, c = 1;
        switch (key.face) {
            case 0: a = d.y; b = d.z; c =  d.x; break;  case 1: a = d.y; b = d.z; c = -d.x; break;
            case 2: a = d.x; b = d.z; c =  d.y; break;  case 3: a = d.x; b = d.z; c = -d.y; break;
            case 4: a = d.x; b = d.y; c =  d.z; break;  default: a = d.x; b = d.y; c = -d.z; break;
        }
        u = a / std::max(c, 1e-9); v = b / std::max(c, 1e-9);
    }
    const double k = (double)(kVoxN - 1);
    return glm::dvec3((u - u0) / (u1 - u0) * k, (v - v0) / (v1 - v0) * k, (r - baseR) / kVoxChunkM * k);
}

glm::vec3 VoxChunk::metresPerVoxel() const {
    return glm::vec3(sideU / (float)(kVoxN - 1), sideV / (float)(kVoxN - 1), (float)kVoxChunkM / (float)(kVoxN - 1));
}

// ── VoxWorld ────────────────────────────────────────────────────────────────────────────────────

void VoxWorld::configure(uint32_t seed, double planetRadiusM, const std::string& bakesDir,
                         const std::string& saveDir, VoxElevFn elev) {
    m_seed = seed; m_radius = planetRadiusM; m_bakes = bakesDir; m_save = saveDir; m_elev = std::move(elev);
    m_chunks.clear(); m_strokes.clear(); m_caves.clear(); m_caveDefs.clear(); m_placedBoxes.clear();
    m_islands.clear(); m_islandDefs.clear(); m_placedIslandBoxes.clear();
}

void VoxWorld::setSaveDir(const std::string& dir) {
    if (dir == m_save) return;
    m_save = dir;
    if (!configured()) return;
    // Los trazos de la partida anterior (si la hubo) se van: se tiran los chunks y se recargan con
    // los de ésta. Las cuevas e islas son de la ESCENA, no de la partida: se quedan.
    std::unique_lock<std::shared_mutex> lk(m_mx);
    m_chunks.clear(); m_strokes.clear();
    ++m_version;
}

void VoxWorld::setDesignerDir(const std::string& dir) {
    if (dir == m_designerDir) return;
    m_designerDir = dir;
    if (!configured()) return;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    m_chunks.clear(); m_strokes.clear();
    ++m_version;
}

// ── Cuevas e islas: objetos de la escena ────────────────────────────────────────────────────────

static glm::dvec3 latLonDir(double latDeg, double lonDeg) {
    const double la = glm::radians(latDeg), lo = glm::radians(lonDeg);
    return glm::dvec3(std::cos(la) * std::cos(lo), std::sin(la), std::cos(la) * std::sin(lo));
}

static float elevThunk(const glm::dvec3& d, void* u) {
    const VoxElevFn* f = (const VoxElevFn*)u;
    return (f && *f) ? (*f)(d) : 0.0f;
}

CaveBox VoxWorld::boxFor(const CaveDef& d) const {
    return caveBoxFromDef(d, m_radius, elevThunk, (void*)&m_elev);
}
IslandBox VoxWorld::boxFor(const IslandDef& d) const {
    return islandBoxFromDef(d, m_radius, elevThunk, (void*)&m_elev);
}
const CaveDef* VoxWorld::caveDefById(uint32_t id) const {
    for (const CaveDef& d : m_caveDefs) if (caveIdFromName(d.name) == id) return &d;
    return nullptr;
}
const IslandDef* VoxWorld::islandDefById(uint32_t id) const {
    for (const IslandDef& d : m_islandDefs) if (caveIdFromName(d.name) == id) return &d;
    return nullptr;
}

void VoxWorld::dropChunksNearNoLock(const glm::dvec3& dir, double radiusM) {
    // ⚠️ SE REHORNEAN EN SITIO, NO SE BORRAN. Borrarlos hacía que el render soltara sus mallas y
    // que el recorte del terreno (que exige "mallado alguna vez") se apagara hasta que el chunk
    // nuevo se mallaba de nuevo: al colocar, mover o rehornear una cueva, el suelo original volvía
    // a taparla un momento ("la malla original fantasma"). Rehorneando en sitio la malla vieja se
    // sigue dibujando hasta que llega la nueva, y el recorte no se corta.
    for (auto& kv : m_chunks) {
        const double ang = std::acos(std::clamp(glm::dot(kv.second.centerDir, dir), -1.0, 1.0));
        if (ang * m_radius < radiusM) rebakeNoLock(kv.second, kv.first);
    }
    ++m_version;
}

void VoxWorld::setCaves(const std::vector<CaveDef>& defs) {
    if (!configured()) return;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    for (const CaveBox& b : m_placedBoxes) dropChunksNearNoLock(b.centerDir, (double)b.radiusM + 450.0);
    m_caveDefs.clear(); m_placedBoxes.clear(); m_caves.clear();
    for (const CaveDef& d : defs) {
        if (caveDefById(caveIdFromName(d.name))) continue;   // nombres repetidos: se queda el primero
        m_caveDefs.push_back(d);
        m_placedBoxes.push_back(boxFor(d));
        dropChunksNearNoLock(m_placedBoxes.back().centerDir, (double)m_placedBoxes.back().radiusM + 450.0);
    }
    ++m_version;
}

void VoxWorld::setIslands(const std::vector<IslandDef>& defs) {
    if (!configured()) return;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    for (const IslandBox& b : m_placedIslandBoxes) dropChunksNearNoLock(b.centerDir, (double)b.radiusM + 450.0);
    m_islandDefs.clear(); m_placedIslandBoxes.clear(); m_islands.clear();
    for (const IslandDef& d : defs) {
        if (islandDefById(caveIdFromName(d.name))) continue;
        m_islandDefs.push_back(d);
        m_placedIslandBoxes.push_back(boxFor(d));
        dropChunksNearNoLock(m_placedIslandBoxes.back().centerDir, (double)m_placedIslandBoxes.back().radiusM + 450.0);
    }
    ++m_version;
}

bool VoxWorld::placeCave(const CaveDef& def) {
    if (!configured() || def.name.empty()) return false;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    if (caveDefById(caveIdFromName(def.name))) return false;
    m_caveDefs.push_back(def);
    m_placedBoxes.push_back(boxFor(def));
    dropChunksNearNoLock(m_placedBoxes.back().centerDir, (double)m_placedBoxes.back().radiusM + 450.0);
    return true;
}

bool VoxWorld::placeIsland(const IslandDef& def) {
    if (!configured() || def.name.empty()) return false;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    if (islandDefById(caveIdFromName(def.name))) return false;
    m_islandDefs.push_back(def);
    m_placedIslandBoxes.push_back(boxFor(def));
    dropChunksNearNoLock(m_placedIslandBoxes.back().centerDir, (double)m_placedIslandBoxes.back().radiusM + 450.0);
    return true;
}

static std::string draftName(const char* kind, const glm::dvec3& d) {
    char name[96];
    std::snprintf(name, sizeof(name), "%s_borrador_%.4f_%.4f", kind,
                  glm::degrees(std::asin(std::clamp(d.y, -1.0, 1.0))), glm::degrees(std::atan2(d.z, d.x)));
    return name;
}

bool VoxWorld::placeCave(const glm::dvec3& dir) {
    if (!configured()) return false;
    // El borrador: la caja que propondría la siembra para esta celda (forma de la semilla del
    // mundo), como definición SIN mapas (se hornean al cargar y no se guardan).
    CaveBox box;
    caveBoxForCell(m_seed, glm::normalize(dir), m_radius, box, elevThunk, (void*)&m_elev);
    CaveDef def;
    def.name = draftName("cueva", box.centerDir);
    def.centerDir = box.centerDir; def.radiusM = box.radiusM; def.depthM = box.depthM;
    def.mouth = box.mouth;
    // La MISMA forma que daría la siembra: el horneado mezcla `seed ^ (id · 2654435761)`, y el id
    // del borrador es el hash del nombre, no la celda: se compensa aquí.
    def.draftSeed = (box.seed ^ (box.cell * 2654435761u)) ^ (caveIdFromName(def.name) * 2654435761u);
    return placeCave(def);
}

bool VoxWorld::placeIsland(const glm::dvec3& dir) {
    if (!configured()) return false;
    IslandBox box;
    islandBoxForCell(m_seed, glm::normalize(dir), m_radius, box, elevThunk, (void*)&m_elev);
    IslandDef def;
    def.name = draftName("isla", box.centerDir);
    def.centerDir = box.centerDir; def.radiusM = box.radiusM; def.topM = box.topM;
    def.rootM = box.rootM; def.altitudeM = box.altitudeM;
    def.draftSeed = (box.seed ^ (box.cell * 2654435761u)) ^ (caveIdFromName(def.name) * 2654435761u);
    return placeIsland(def);
}

bool VoxWorld::removeCave(const std::string& name) {
    if (!configured()) return false;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    for (size_t i = 0; i < m_caveDefs.size(); ++i)
        if (m_caveDefs[i].name == name) {
            const CaveBox b = m_placedBoxes[i];
            m_caves.erase(b.cell);
            m_caveDefs.erase(m_caveDefs.begin() + (long)i); m_placedBoxes.erase(m_placedBoxes.begin() + (long)i);
            dropChunksNearNoLock(b.centerDir, (double)b.radiusM + 450.0);
            return true;
        }
    return false;
}

bool VoxWorld::removeIsland(const std::string& name) {
    if (!configured()) return false;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    for (size_t i = 0; i < m_islandDefs.size(); ++i)
        if (m_islandDefs[i].name == name) {
            const IslandBox b = m_placedIslandBoxes[i];
            m_islands.erase(b.cell);
            m_islandDefs.erase(m_islandDefs.begin() + (long)i); m_placedIslandBoxes.erase(m_placedIslandBoxes.begin() + (long)i);
            dropChunksNearNoLock(b.centerDir, (double)b.radiusM + 450.0);
            return true;
        }
    return false;
}

bool VoxWorld::removeCave(const glm::dvec3& dir) {
    std::string name;
    {
        std::shared_lock<std::shared_mutex> lk(m_mx);
        const glm::dvec3 d = glm::normalize(dir);
        // La más cercana, si está a menos de su radio o dentro de la celda de siembra (un borrador
        // colocado "en `dir`" tiene el centro en la celda, hasta a 7 km del punto pedido).
        double best = 1e300;
        for (size_t i = 0; i < m_placedBoxes.size(); ++i) {
            const double dist = std::acos(std::clamp(glm::dot(m_placedBoxes[i].centerDir, d), -1.0, 1.0)) * m_radius;
            if (dist < best && dist <= std::max((double)m_placedBoxes[i].radiusM, kCaveCellRad * m_radius)) { best = dist; name = m_caveDefs[i].name; }
        }
    }
    return !name.empty() && removeCave(name);
}

bool VoxWorld::removeIsland(const glm::dvec3& dir) {
    std::string name;
    {
        std::shared_lock<std::shared_mutex> lk(m_mx);
        const glm::dvec3 d = glm::normalize(dir);
        double best = 1e300;
        for (size_t i = 0; i < m_placedIslandBoxes.size(); ++i) {
            const double dist = std::acos(std::clamp(glm::dot(m_placedIslandBoxes[i].centerDir, d), -1.0, 1.0)) * m_radius;
            if (dist < best && dist <= std::max((double)m_placedIslandBoxes[i].radiusM, kIslandCellRad * m_radius)) { best = dist; name = m_islandDefs[i].name; }
        }
    }
    return !name.empty() && removeIsland(name);
}

std::vector<CaveBox> VoxWorld::placedCaveBoxes() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return m_placedBoxes;
}

std::vector<glm::dvec3> VoxWorld::placedCaveMouths() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    std::vector<glm::dvec3> out;
    for (const CaveBox& b : m_placedBoxes) out.push_back(caveEntranceDir(b));
    return out;
}

std::vector<IslandBox> VoxWorld::placedIslandBoxes() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return m_placedIslandBoxes;
}

const IslandSystem* VoxWorld::islandFor(const IslandBox& box) {
    auto it = m_islands.find(box.cell);
    if (it != m_islands.end()) return &it->second;
    // Sin hilo: cargar/hornear tres mapas de 256² son milisegundos.
    IslandSystem& sys = m_islands[box.cell];
    const IslandDef* def = islandDefById(box.cell);
    const bool ok = def ? islandLoadBox(*def, box, sys, kIslandMapRes) : islandLoadOrBake(box, sys, m_bakes);
    if (!ok) { m_islands.erase(box.cell); return nullptr; }
    return &sys;
}

float VoxWorld::floorDepthM(const glm::dvec3& dir, float maxM) const {
    if (!configured()) return 0.0f;
    const glm::dvec3 d = glm::normalize(dir);
    const double top = m_radius + (m_elev ? (double)m_elev(d) : 0.0);
    std::shared_lock<std::shared_mutex> lk(m_mx);
    // Se baja por la columna de metro en metro mientras el campo diga aire. El primer metro va a
    // -3 como `surfaceCut` (a 0 exacto la trilineal está en el borde del chunk).
    if (!findNoLock(keyAt(d * (top - 0.75)))) return 0.0f;
    auto vol = [&](const glm::dvec3& p) { return std::max(caveDensityNoLock(p), addedDensityNoLock(p)); };
    if (vol(d * (top - 0.75)) >= 0.0f) return 0.0f;
    float depth = 1.0f;
    while (depth < maxM) {
        const glm::dvec3 p = d * (top - (double)depth);
        if (!findNoLock(keyAt(p)) || vol(p) >= 0.0f) break;
        depth += 1.0f;
    }
    return depth;
}

static int chunksPerFaceFor(double R) {
    return std::max(1, (int)std::lround((3.14159265358979 * 0.5) / (kVoxChunkM / R)));
}
int VoxWorld::chunksPerFace() const { return chunksPerFaceFor(m_radius); }

VoxKey VoxWorld::keyAt(const glm::dvec3& p) const {
    VoxKey k;
    const double r = glm::length(p);
    if (r < 1e-9) return k;
    const glm::dvec3 d = p / r;
    const int n = chunksPerFaceFor(m_radius);
    double u, v;
    dirToFaceUV(d, k.face, u, v);
    k.i = std::clamp((int)std::floor((u + 1.0) * 0.5 * n), 0, n - 1);
    k.j = std::clamp((int)std::floor((v + 1.0) * 0.5 * n), 0, n - 1);
    k.k = (int)std::floor((r - m_radius) / kVoxChunkM);
    return k;
}

double VoxWorld::chunkSideAt(const glm::dvec3& dir) const {
    VoxChunk c;
    frameFor(keyAt(glm::normalize(dir) * m_radius), c);
    return (double)std::min(c.sideU, c.sideV);
}

glm::dvec3 VoxWorld::chunkCenterDir(const VoxKey& key) const {
    VoxChunk c;
    frameFor(key, c);
    return c.centerDir;
}

std::vector<VoxKey> VoxWorld::columnsNear(const glm::dvec3& dir, double radiusM) const {
    std::vector<VoxKey> out;
    if (!configured() || radiusM < 0.0) return out;
    const glm::dvec3 up = glm::normalize(dir);
    const glm::dvec3 ref = (std::fabs(up.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    const int n = chunksPerFaceFor(m_radius);
    // POR ÍNDICE: la caja (u,v) del disco en la cara del centro, y de ella toda celda cuyo centro
    // quede a menos de radio + media diagonal. Un barrido a pasos de metros no sirve: la celda es
    // un paralelogramo que se aprieta y se sesga hacia las esquinas del cubo, y cualquier paso fijo
    // acaba saltándose alguna (medido: paso = lado/2 dejaba 5 de 16 a 35°N 45°E).
    int face; double uc, vc;
    dirToFaceUV(up, face, uc, vc);
    double umin = uc, umax = uc, vmin = vc, vmax = vc;
    bool otraCara = false;
    for (int t = 0; t < 64; ++t) {
        const double a = 6.283185307179586 * (double)t / 64.0;
        const glm::dvec3 d = glm::normalize(up + (e1 * std::cos(a) + e2 * std::sin(a)) * (radiusM / m_radius));
        int f; double u, v;
        dirToFaceUV(d, f, u, v);
        if (f != face) {
            // La muestra cae en otra cara: se PROYECTA sobre ésta (u,v fuera de [-1,1]) y se
            // sujeta al borde, para que la caja llegue hasta la arista y no se quede corta.
            otraCara = true;
            double aa = 0, bb = 0, cc = 1;
            switch (face) {
                case 0: aa = d.y; bb = d.z; cc =  d.x; break;  case 1: aa = d.y; bb = d.z; cc = -d.x; break;
                case 2: aa = d.x; bb = d.z; cc =  d.y; break;  case 3: aa = d.x; bb = d.z; cc = -d.y; break;
                case 4: aa = d.x; bb = d.y; cc =  d.z; break;  default: aa = d.x; bb = d.y; cc = -d.z; break;
            }
            u = std::clamp(aa / std::max(cc, 1e-9), -1.0, 1.0); v = std::clamp(bb / std::max(cc, 1e-9), -1.0, 1.0);
        }
        umin = std::min(umin, u); umax = std::max(umax, u); vmin = std::min(vmin, v); vmax = std::max(vmax, v);
    }
    const int imin = std::clamp((int)std::floor((umin + 1.0) * 0.5 * n), 0, n - 1), imax = std::clamp((int)std::floor((umax + 1.0) * 0.5 * n), 0, n - 1);
    const int jmin = std::clamp((int)std::floor((vmin + 1.0) * 0.5 * n), 0, n - 1), jmax = std::clamp((int)std::floor((vmax + 1.0) * 0.5 * n), 0, n - 1);
    for (int j = jmin; j <= jmax; ++j)
        for (int i = imin; i <= imax; ++i) {
            VoxChunk c;
            VoxKey k; k.face = face; k.i = i; k.j = j; k.k = 0;
            frameFor(k, c);
            const double dist = std::acos(std::clamp(glm::dot(c.centerDir, up), -1.0, 1.0)) * m_radius;
            const double diag = 0.5 * std::sqrt((double)c.sideU * c.sideU + (double)c.sideV * c.sideV);
            if (dist <= radiusM + diag) out.push_back(k);
        }
    // Si el disco cruza a otra cara del cubo, esa parte se recoge por muestreo fino (raro: seis
    // aristas en todo el planeta).
    if (otraCara) {
        const double step = std::max(0.25 * chunkSideAt(up), 4.0);
        const int m = (int)std::ceil(radiusM / step);
        for (int j = -m; j <= m; ++j)
            for (int i = -m; i <= m; ++i) {
                const double ox = i * step, oy = j * step;
                if (ox * ox + oy * oy > radiusM * radiusM) continue;
                VoxKey k = keyAt(glm::normalize(up + (e1 * ox + e2 * oy) / m_radius) * m_radius);
                k.k = 0;
                if (k.face != face && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
            }
    }
    return out;
}

void VoxWorld::frameFor(const VoxKey& key, VoxChunk& c) const {
    c.key = key;
    const int n = chunksPerFaceFor(m_radius);
    c.u0 = -1.0 + 2.0 * (double)key.i / (double)n;  c.u1 = -1.0 + 2.0 * (double)(key.i + 1) / (double)n;
    c.v0 = -1.0 + 2.0 * (double)key.j / (double)n;  c.v1 = -1.0 + 2.0 * (double)(key.j + 1) / (double)n;
    c.baseR = m_radius + (double)key.k * kVoxChunkM;
    c.centerDir = faceUVToDir(key.face, 0.5 * (c.u0 + c.u1), 0.5 * (c.v0 + c.v1));
    c.origin = c.centerDir * c.baseR;
    // Metros reales de la celda: la retícula (u,v) no es uniforme (1,4x del centro de la cara a la
    // arista). Se miden sobre la esfera, no se suponen.
    const double vm = 0.5 * (c.v0 + c.v1), um = 0.5 * (c.u0 + c.u1);
    c.sideU = (float)(std::acos(std::clamp(glm::dot(faceUVToDir(key.face, c.u0, vm), faceUVToDir(key.face, c.u1, vm)), -1.0, 1.0)) * c.baseR);
    c.sideV = (float)(std::acos(std::clamp(glm::dot(faceUVToDir(key.face, um, c.v0), faceUVToDir(key.face, um, c.v1)), -1.0, 1.0)) * c.baseR);
}

const VoxChunk* VoxWorld::findNoLock(const VoxKey& key) const {
    auto it = m_chunks.find(key);
    return it == m_chunks.end() ? nullptr : &it->second;
}
VoxChunk* VoxWorld::get(const VoxKey& key) {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return const_cast<VoxChunk*>(findNoLock(key));
}
const VoxChunk* VoxWorld::get(const VoxKey& key) const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return findNoLock(key);
}

const CaveSystem* VoxWorld::caveFor(const CaveBox& box) {
    auto it = m_caves.find(box.cell);
    if (it != m_caves.end()) return &it->second;
    // De la escena (o borrador colocado): sus mapas; sembrada automática: la caché por semilla.
    const CaveDef* defp = caveDefById(box.cell);
    const CaveDef def = defp ? *defp : CaveDef{};
    const bool fromDef = defp != nullptr;
    auto load = [](const CaveDef& d, bool fd, const CaveBox& b, const std::string& bakes) {
        CaveSystem sys;
        const bool ok = fd ? caveLoadBox(d, b, sys, bakes) : caveLoadOrBake(b, sys, bakes);
        if (!ok) sys = CaveSystem{};
        return sys;
    };
    if (m_syncBake) {
        CaveSystem& sys = m_caves[box.cell];
        sys = load(def, fromDef, box, m_bakes);
        if (!sys.valid()) { m_caves.erase(box.cell); return nullptr; }
        return &sys;
    }
    // En hilo. La carga es pura sobre sus argumentos (lee/escribe SUS ficheros) y no toca nada de
    // este objeto: se puede lanzar sin cerrojo. El chunk que lo pidió se queda provisional
    // (`pendingCave`) hasta `pollCaveJobs`.
    if (!m_caveJobs.count(box.cell)) {
        const std::string bakes = m_bakes;
        m_caveJobs[box.cell] = std::async(std::launch::async, [def, fromDef, box, bakes, load] {
            return load(def, fromDef, box, bakes);
        });
    }
    return nullptr;
}

int VoxWorld::pollCaveJobs() {
    std::unique_lock<std::shared_mutex> lk(m_mx);
    int llegaron = 0;
    for (auto it = m_caveJobs.begin(); it != m_caveJobs.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        CaveSystem sys = it->second.get();
        if (sys.valid()) m_caves[it->first] = std::move(sys);
        it = m_caveJobs.erase(it);
        ++llegaron;
    }
    if (llegaron > 0) {
        // Los chunks provisionales se tiran: `ensure` los rehace con la cueva ya en memoria (los
        // editados no se pierden: se rehornean en sitio y sus trazos siguen en `m_strokes`).
        // En sitio también (ver `dropChunksNearNoLock`): así la malla provisional (roca) no
        // desaparece antes de que exista la de la cueva.
        for (auto& kv : m_chunks) if (kv.second.pendingCave) rebakeNoLock(kv.second, kv.first);
        ++m_version;
        HARUKA_LOGI("Vox", "%d cueva(s) horneadas en hilo recogidas: los chunks provisionales se rehacen", llegaron);
    }
    return llegaron;
}

void VoxWorld::waitCaveJobs() {
    for (;;) {
        { std::shared_lock<std::shared_mutex> lk(m_mx); if (m_caveJobs.empty()) return; for (auto& kv : m_caveJobs) kv.second.wait(); }
        pollCaveJobs();
    }
}

bool VoxWorld::cavesPending() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return !m_caveJobs.empty();
}

void VoxWorld::bake(VoxChunk& c) {
    c.d.clear();   // roca maciza: sin reservar hasta que una cueva o un trazo escriban algo
    // Las cuevas sembradas que pueden cruzar este chunk: cajas de hasta 400 m de radio, así que se
    // buscan hasta lado + 450 m del centro. Las que no lo tocan devuelven "roca" y el `min` las
    // ignora — no hace falta recortar por caja a mano.
    CaveBox boxes[16];
    struct Ctx { const VoxElevFn* f; } ctx{ &m_elev };
    auto elevFn = [](const glm::dvec3& d, void* u) -> float {
        const Ctx* cx = (const Ctx*)u; return (cx->f && *cx->f) ? (*cx->f)(d) : 0.0f;
    };
    int nb = caveBoxesNear(m_seed, c.centerDir, (double)std::max(c.sideU, c.sideV) + 450.0, m_radius, boxes, 16,
                           elevFn, &ctx);
    // Y las colocadas a mano que lo cruzan (el caso normal hoy: la siembra automática está a 0).
    for (const CaveBox& b : m_placedBoxes) {
        if (nb >= 16) break;
        const double ang = std::acos(std::clamp(glm::dot(b.centerDir, c.centerDir), -1.0, 1.0)) * m_radius;
        if (ang < (double)std::max(c.sideU, c.sideV) + 450.0) boxes[nb++] = b;
    }
    std::vector<const CaveSystem*> caves;
    c.pendingCave = false;
    for (int i = 0; i < nb; ++i) {
        if (const CaveSystem* s = caveFor(boxes[i])) caves.push_back(s);
        else if (m_caveJobs.count(boxes[i].cell)) c.pendingCave = true;   // llegará: este chunk es provisional
    }
    m_lastBakeCaves = (int)caves.size();

    // ── EL CANAL AÑADIDO: las islas que cruzan este chunk ─────────────────────────────────────
    // Cajas de hasta 350 m de medio lado, ±(raíz+cima+24) m de alto en torno a su base.
    c.a.clear(); c.islandCells.clear();
    for (const IslandBox& b : m_placedIslandBoxes) {
        const double ang = std::acos(std::clamp(glm::dot(b.centerDir, c.centerDir), -1.0, 1.0)) * m_radius;
        if (ang >= (double)std::max(c.sideU, c.sideV) + (double)b.radiusM + 100.0) continue;
        const double lo = b.baseRM - (double)b.rootM - (double)kIslandRangeM, hi = b.baseRM + (double)b.topM + (double)kIslandRangeM;
        if (c.baseR + kVoxChunkM < lo || c.baseR > hi) continue;
        if (islandFor(b)) c.islandCells.push_back(b.cell);
    }
    if (!c.islandCells.empty()) {
        c.a.assign((size_t)kVoxN * kVoxN * kVoxN, -kVoxRockM);
        bool any = false;
        for (int z = 0; z < kVoxN; ++z)
            for (int y = 0; y < kVoxN; ++y)
                for (int x = 0; x < kVoxN; ++x) {
                    const float v = bakedAddedNoLock(c, c.worldOfGrid(x, y, z));
                    c.a[(size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x] = v;
                    any = any || v > -kVoxRockM;
                }
        if (!any) c.a.clear();   // la caja la rozaba pero la isla no llega: sin memoria
    }

    if (caves.empty()) return;   // todo roca: el caso normal, y gratis

    // EL POZO HASTA EL SUELO DE VERDAD. La caja de la cueva tiene el techo plano a la cota de su
    // centro; el terreno, no. Aquí el pozo se prolonga desde el techo de la caja hasta la cota del
    // terreno en cada columna, como el trazo que es: sin esto la boca queda enterrada bajo la ladera
    // (o colgando en el aire donde el terreno baja).
    c.caveCells.clear(); c.shafts.clear();
    for (int i = 0; i < nb; ++i) {
        if (!m_caves.count(boxes[i].cell)) continue;
        c.caveCells.push_back(boxes[i].cell);
        const glm::dvec3 md = caveEntranceDir(boxes[i]);
        c.shafts.push_back({ md, (double)caveEntranceRadiusM(boxes[i]), boxes[i].surfaceRM });
    }

    c.reserveCut();
    bool any = false;
    for (int z = 0; z < kVoxN; ++z)
        for (int y = 0; y < kVoxN; ++y)
            for (int x = 0; x < kVoxN; ++x) {
                const float v = bakedFieldNoLock(c, c.worldOfGrid(x, y, z));
                c.at(x, y, z) = v;
                any = any || v < kVoxRockM;
            }
    // La caja rozaba el chunk pero ninguna galería llega: roca maciza, sin reservar (medido en el
    // juego con una cueva de 360 m: 532 de 576 chunks "con contenido", 66 MB, con el aire en ~40).
    if (!any) c.d.clear();
}

float VoxWorld::bakedFieldNoLock(const VoxChunk& c, const glm::dvec3& p) const {
    float d = kVoxRockM;
    const double r = glm::length(p);
    if (r < 1e-9) return d;
    const glm::dvec3 dir = p / r;
    // La cueva DRAPEADA sobre el terreno: profundidad desde la cota local, no desde un techo plano.
    const double ground = m_radius + (m_elev ? (double)m_elev(dir) : 0.0);
    static const bool s_flatTop = [] { const char* e = std::getenv("HARUKA_VOX_FLATTOP"); return e && e[0] == '1'; }();   // A/B: techo plano
    for (uint32_t cell : c.caveCells) {
        auto it = m_caves.find(cell);
        if (it != m_caves.end())
            d = std::min(d, s_flatTop ? caveDensityAt(it->second, p) : caveDensityAtDraped(it->second, p, ground));
    }
    // El pozo: con la cueva drapeada ya nace en el suelo local; el cilindro sólo asegura que la
    // boca esté abierta hasta un poco por encima de la cota (y los primeros metros de techo).
    for (const VoxChunk::Shaft& sh : c.shafts) {
        const double lat = std::acos(std::clamp(glm::dot(dir, sh.dir), -1.0, 1.0)) * m_radius;
        if (lat > sh.rM + 8.0) continue;
        if (r < ground - 60.0 || r > ground + 6.0) continue;
        d = std::min(d, (float)(lat - sh.rM));
    }
    return std::clamp(d, -kVoxRockM, kVoxRockM);
}

float VoxWorld::bakedAddedNoLock(const VoxChunk& c, const glm::dvec3& p) const {
    float a = -kVoxRockM;
    for (uint32_t cell : c.islandCells) {
        auto it = m_islands.find(cell);
        if (it != m_islands.end()) a = std::max(a, islandDensityAt(it->second, p));
    }
    return std::clamp(a, -kVoxRockM, kVoxRockM);
}

std::string VoxWorld::strokePathIn(const std::string& base, const VoxKey& key) const {
    if (base.empty()) return {};
    char name[96];
    std::snprintf(name, sizeof(name), "vox/%d_%d_%d_%d.txt", key.face, key.i, key.j, key.k);
    const std::string dir = (base.back() == '/' || base.back() == '\\') ? base : base + "/";
    return dir + name;
}
std::string VoxWorld::strokePath(const VoxKey& key) const { return strokePathIn(m_save, key); }

void VoxWorld::saveChunkStrokes(const VoxKey& key) const {
    auto it = m_strokes.find(key);
    if (it == m_strokes.end()) return;
    std::vector<CaveStroke> mundo, partida;
    for (const Stroke& s : it->second) (s.designer ? mundo : partida).push_back(caveStrokeFrom(s));
    // Cada lista a su carpeta; la lista vacía BORRA el fichero (deshacer el último trazo de un
    // chunk tiene que dejar el disco como estaba).
    auto put = [&](const std::string& path, const std::vector<CaveStroke>& v) {
        if (path.empty()) return;
        if (v.empty()) { std::error_code ec; std::filesystem::remove(path, ec); }
        else caveSaveStrokes(path, v);
    };
    if (m_designerMode) put(strokePathIn(m_designerDir, key), mundo);
    put(strokePathIn(m_save, key), partida);
}

void VoxWorld::markNeighboursDirty(const VoxKey& key) {
    // Los vecinos por POSICIÓN, no por aritmética de índices: en el borde de una cara del cubo
    // `i+1` no existe, pero el punto "un chunk más allá" sí. Se pide el punto a medio vóxel más allá
    // de cada cara/arista/esquina de la rejilla.
    VoxChunk* c = const_cast<VoxChunk*>(findNoLock(key));
    if (!c) return;
    const double N = (double)(kVoxN - 1);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy && !dz) continue;
                const glm::dvec3 p = c->worldOfGrid(0.5 * N + dx * (0.5 * N + 0.5),
                                                    0.5 * N + dy * (0.5 * N + 0.5),
                                                    0.5 * N + dz * (0.5 * N + 0.5));
                if (VoxChunk* nb = const_cast<VoxChunk*>(findNoLock(keyAt(p)))) nb->dirty = true;
            }
}

VoxChunk& VoxWorld::ensure(const VoxKey& key) {
    std::unique_lock<std::shared_mutex> lk(m_mx);
    return ensureNoLock(key);
}

VoxChunk& VoxWorld::ensureNoLock(const VoxKey& key) {
    if (VoxChunk* c = const_cast<VoxChunk*>(findNoLock(key))) return *c;
    VoxChunk& c = m_chunks[key];
    frameFor(key, c);
    const auto t0 = std::chrono::high_resolution_clock::now();
    bake(c);
    // Los trazos, ENCIMA del horneado y en este orden: los del MUNDO (editor) y luego los de la
    // PARTIDA (jugador). Son lo que alguien hizo y no se recalculan.
    // ⚠️ Se REEMPLAZA la lista en memoria, no se añade: tras descargar y recargar el mismo chunk
    // los trazos seguían en `m_strokes` y se duplicaban en cada vuelta (y en el fichero al
    // guardar). El fichero es la verdad al cargar.
    // Si la sesión ya tiene la lista de este chunk (se descargó y se vuelve a cargar), esa es la
    // verdad: conserva el orden y los números de secuencia del deshacer. Si no, de los ficheros.
    auto itS = m_strokes.find(key);
    if (itS == m_strokes.end()) {
        auto& lst = m_strokes[key];
        for (int pass = 0; pass < 2; ++pass) {
            const std::string path = strokePathIn(pass == 0 ? m_designerDir : m_save, key);
            std::vector<CaveStroke> saved;
            if (path.empty() || !caveLoadStrokes(path, saved)) continue;
            // `centerLocal` de un trazo de chunk = coordenadas de REJILLA (x,y,z fraccionales):
            // es lo que hace que el trazo caiga en el mismo vóxel al recargar, sin pasar por metros.
            for (const CaveStroke& s : saved) lst.push_back(strokeFrom(s, pass == 0));
        }
    }
    for (const Stroke& st : m_strokes[key]) applyLocal(c, st);
    c.edited = !m_strokes[key].empty();
    const size_t savedCount = m_strokes[key].size();
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    if (m_lastBakeCaves > 0 || c.edited)
        HARUKA_LOGDIAG("Vox", "chunk (%d,%d,%d,%d) en %.1f ms: %d cuevas · %zu trazos",
                    key.face, key.i, key.j, key.k, ms, m_lastBakeCaves, savedCount);
    c.dirty = true;
    markNeighboursDirty(key);
    ++m_version;
    return c;
}

float VoxWorld::terrainDensity(const glm::dvec3& p) const {
    const double r = glm::length(p);
    if (r < 1e-9) return kVoxRockM;
    const glm::dvec3 dir = p / r;
    const double ground = m_radius + (m_elev ? (double)m_elev(dir) : 0.0);
    return (float)std::clamp(ground - r, -(double)kVoxRockM, (double)kVoxRockM);
}

float VoxWorld::density(const glm::dvec3& p) const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return std::max(std::min(caveDensityNoLock(p), terrainDensity(p)), addedDensityNoLock(p));
}

float VoxWorld::caveDensity(const glm::dvec3& p) const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return caveDensityNoLock(p);
}

float VoxWorld::addedDensity(const glm::dvec3& p) const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    return addedDensityNoLock(p);
}

float VoxWorld::caveDensityNoLock(const glm::dvec3& p) const {
    const VoxChunk* c = findNoLock(keyAt(p));
    if (!c || c->d.empty()) return kVoxRockM;
    return sampleChannel(*c, c->d, c->gridOf(p));
}

float VoxWorld::addedDensityNoLock(const glm::dvec3& p) const {
    const VoxChunk* c = findNoLock(keyAt(p));
    if (!c || c->a.empty()) return -kVoxRockM;
    return sampleChannel(*c, c->a, c->gridOf(p));
}

float VoxWorld::sampleChannel(const VoxChunk&, const std::vector<float>& ch, const glm::dvec3& g) {
    const float fx = std::clamp((float)g.x, 0.0f, (float)(kVoxN - 1));
    const float fy = std::clamp((float)g.y, 0.0f, (float)(kVoxN - 1));
    const float fz = std::clamp((float)g.z, 0.0f, (float)(kVoxN - 1));
    const int x0 = std::min((int)fx, kVoxN - 2), y0 = std::min((int)fy, kVoxN - 2), z0 = std::min((int)fz, kVoxN - 2);
    const float ax = fx - (float)x0, ay = fy - (float)y0, az = fz - (float)z0;
    auto D = [&](int x, int y, int z) { return ch[(size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x]; };
    const float c00 = D(x0,y0,z0) + (D(x0+1,y0,z0) - D(x0,y0,z0)) * ax;
    const float c10 = D(x0,y0+1,z0) + (D(x0+1,y0+1,z0) - D(x0,y0+1,z0)) * ax;
    const float c01 = D(x0,y0,z0+1) + (D(x0+1,y0,z0+1) - D(x0,y0,z0+1)) * ax;
    const float c11 = D(x0,y0+1,z0+1) + (D(x0+1,y0+1,z0+1) - D(x0,y0+1,z0+1)) * ax;
    const float c0 = c00 + (c10 - c00) * ay, c1 = c01 + (c11 - c01) * ay;
    return c0 + (c1 - c0) * az;
}

glm::vec3 VoxWorld::gradient(const glm::dvec3& p, float hM) const {
    const double r = glm::length(p);
    if (r < 1e-9) return glm::vec3(0.0f);
    const glm::dvec3 up = p / r;
    const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    const glm::vec3 g((density(p + e1 * (double)hM) - density(p - e1 * (double)hM)) / (2.0f * hM),
                      (density(p + e2 * (double)hM) - density(p - e2 * (double)hM)) / (2.0f * hM),
                      (density(p + up * (double)hM) - density(p - up * (double)hM)) / (2.0f * hM));
    return glm::vec3(e1) * g.x + glm::vec3(e2) * g.y + glm::vec3(up) * g.z;
}

VoxWorld::Stroke VoxWorld::strokeFrom(const VoxChunk& c, const glm::dvec3& worldPos, const Brush& b, bool designer, uint64_t seq) {
    Stroke st;
    st.local = glm::vec3(c.gridOf(worldPos));
    st.r = b.radiusM; st.remove = (b.op == Brush::Remove); st.designer = designer;
    st.shape = (uint8_t)b.shape; st.halfM = b.halfM; st.seq = seq;
    st.op = (uint8_t)b.op; st.amount = b.amountM; st.hardness = b.hardness;
    st.localB = (b.shape == Brush::Capsule) ? glm::vec3(c.gridOf(b.capsuleEnd)) : st.local;
    st.falloff = (uint8_t)b.falloff; st.pattern = (uint8_t)b.pattern; st.sides = b.sides; st.patternM = b.patternM;
    return st;
}
VoxWorld::Stroke VoxWorld::strokeFrom(const CaveStroke& s, bool designer) {
    Stroke st;
    st.local = s.centerLocal; st.r = s.radiusM; st.remove = s.remove; st.designer = designer;
    st.shape = s.shape; st.halfM = s.halfM; st.seq = 0;
    st.op = s.op; st.amount = s.amountM; st.hardness = s.hardness; st.localB = s.endLocal;
    st.falloff = s.falloff; st.pattern = s.pattern; st.sides = s.sides; st.patternM = s.patternM;
    return st;
}
CaveStroke VoxWorld::caveStrokeFrom(const Stroke& s) {
    CaveStroke c;
    c.centerLocal = s.local; c.radiusM = s.r; c.remove = s.remove; c.shape = s.shape; c.halfM = s.halfM;
    c.op = s.op; c.amountM = s.amount; c.hardness = s.hardness; c.endLocal = s.localB;
    c.falloff = s.falloff; c.pattern = s.pattern; c.sides = s.sides; c.patternM = s.patternM;
    return c;
}

int VoxWorld::applyLocal(VoxChunk& c, const Stroke& st) {
    // `st.local` = centro en coordenadas de REJILLA. La distancia se mide en el MUNDO (metros de
    // verdad, por la posición de cada vóxel), no en vóxeles: el vóxel no es cúbico ni igual en dos
    // chunks.
    const glm::vec3 g = st.local;
    const glm::vec3 mpv = c.metresPerVoxel();
    const Brush::Shape shape = (Brush::Shape)st.shape;
    const Brush::Op op = (Brush::Op)st.op;
    const glm::dvec3 centerW = c.worldOfGrid(g.x, g.y, g.z);
    const glm::dvec3 endW = (shape == Brush::Capsule) ? c.worldOfGrid(st.localB.x, st.localB.y, st.localB.z) : centerW;
    // Alcance en metros alrededor del centro (la cápsula, alrededor de su segmento).
    float reach = std::max(st.r, st.halfM) * (shape == Brush::Box ? 1.42f : 1.0f);
    if (op == Brush::Raise || op == Brush::Lower || op == Brush::Flatten || op == Brush::Sculpt) reach = std::max(reach, st.r + std::fabs(st.amount) + 2.0f);
    const glm::vec3 gB = (shape == Brush::Capsule) ? st.localB : g;
    const int kx = (int)std::ceil(reach / mpv.x) + 1, ky = (int)std::ceil(reach / mpv.y) + 1, kz = (int)std::ceil(reach / mpv.z) + 1;
    const int x0 = std::max(0, (int)std::floor(std::min(g.x, gB.x)) - kx), x1 = std::min(kVoxN - 1, (int)std::ceil(std::max(g.x, gB.x)) + kx);
    const int y0 = std::max(0, (int)std::floor(std::min(g.y, gB.y)) - ky), y1 = std::min(kVoxN - 1, (int)std::ceil(std::max(g.y, gB.y)) + ky);
    const int z0 = std::max(0, (int)std::floor(std::min(g.z, gB.z)) - kz), z1 = std::min(kVoxN - 1, (int)std::ceil(std::max(g.z, gB.z)) + kz);
    // Marco local de la forma: vertical = radial en el centro, tangentes deterministas (las mismas
    // que en `gradient`/`stroke`): el mismo trazo al recargar cae exactamente igual.
    const glm::dvec3 up = glm::normalize(centerW);
    const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    auto sdf = [&](const glm::dvec3& p) -> float {   // < 0 dentro de la forma
        const glm::dvec3 rel = p - centerW;
        switch (shape) {
            case Brush::Cylinder: {
                const double h = glm::dot(rel, up);
                const double lat = glm::length(rel - up * h);
                return (float)std::max(lat - (double)st.r, std::fabs(h) - (double)st.halfM);
            }
            case Brush::Box: {
                const double h = glm::dot(rel, up), x = glm::dot(rel, e1), y = glm::dot(rel, e2);
                return (float)std::max({ std::fabs(x) - (double)st.r, std::fabs(y) - (double)st.r, std::fabs(h) - (double)st.halfM });
            }
            case Brush::Capsule: {
                // Distancia al SEGMENTO centro→extremo: un túnel entre dos puntos del arrastre.
                const glm::dvec3 ab = endW - centerW;
                const double l2 = glm::dot(ab, ab);
                const double t = l2 > 1e-9 ? std::clamp(glm::dot(rel, ab) / l2, 0.0, 1.0) : 0.0;
                return (float)(glm::length(rel - ab * t) - (double)st.r);
            }
            default: return (float)(glm::length(rel) - (double)st.r);
        }
    };
    // El plano de Allanar/Subir/Bajar: la esfera de radio |centro| (+ cantidad). `plano(p)` > 0 =
    // por debajo (roca). Es radial, no un plano tangente: en 60 m de radio la diferencia es de cm y
    // así "a la misma cota" significa lo mismo que para el terreno.
    const double planeR = glm::length(centerW) + (op == Brush::Raise ? (double)st.amount : op == Brush::Lower ? -(double)st.amount : 0.0);
    auto plane = [&](const glm::dvec3& p) -> float { return (float)(planeR - glm::length(p)); };
    // Peso de la huella para los pinceles "de suelo": 1 en el centro, cae a 0 en el borde según la
    // dureza (borde blando = la rampa empieza antes).
    const float edgeM = std::max((1.0f - st.hardness) * st.r, 0.5f);
    auto weight = [&](float sd) -> float { return std::clamp(-sd / edgeM, 0.0f, 1.0f); };

    // Rellenar escribe también en el canal AÑADIDO (es lo que levanta roca donde el terreno dice
    // aire); picar lo baja también (quita lo construido y lo de la isla). Se reserva `a` sólo al
    // primer relleno; `d` sólo al primer picado (rellenar sobre roca maciza no cambia `d`).
    // ESCULPIR: la superficie se desplaza ±amount a lo largo de su normal → F' = F + δ (mover el
    // conjunto de nivel de una distancia con signo). δ = amount · caída(r/R) · patrón(x,y). El campo
    // completo F incluye el terreno (que el chunk no guarda), así que se escribe explícito en los
    // dos canales: d = a = F + δ, atenuado por el peso de la huella.
    auto falloffOf = [&](float x) -> float {   // x = distancia normalizada 0 (centro) .. 1 (borde)
        x = std::clamp(x, 0.0f, 1.0f);
        switch ((Brush::Falloff)st.falloff) {
            case Brush::FSphere:    return std::sqrt(std::max(0.0f, 1.0f - x * x));
            case Brush::FRoot:      return std::sqrt(1.0f - x);
            case Brush::FSharp:     return (1.0f - x) * (1.0f - x);
            case Brush::FLinear:    return 1.0f - x;
            case Brush::FConstant:  return 1.0f;
            case Brush::FInvSquare: return 1.0f / (1.0f + 8.0f * x * x) - 1.0f / 9.0f;
            default: { const float t = 1.0f - x; return t * t * (3.0f - 2.0f * t); }   // smoothstep
        }
    };
    auto patternOf = [&](double x, double y) -> float {   // x, y tangentes en metros desde el centro
        const double k = 6.283185307179586 / std::max((double)st.patternM, 0.5);
        switch ((Brush::Pattern)st.pattern) {
            case Brush::PWaves:   return (float)std::sin(x * k);
            case Brush::PRings:   return (float)std::cos(std::sqrt(x * x + y * y) * k);
            case Brush::PHex: {   // celdas hexagonales: 1 en el centro de cada celda, −1 en las aristas
                const double s3 = 1.7320508;
                const double a = std::sin(x * k), b = std::sin((x * 0.5 + y * s3 * 0.5) * k), c2 = std::sin((x * 0.5 - y * s3 * 0.5) * k);
                return (float)std::clamp((a + b + c2) * 0.6667, -1.0, 1.0);
            }
            case Brush::PChecker: return (std::fmod(std::floor(x * k / 6.2831853) + std::floor(y * k / 6.2831853), 2.0) == 0.0) ? 1.0f : -1.0f;
            case Brush::PNoise:   return BakeUtil::fbm2((float)(x * k * 0.16), (float)(y * k * 0.16), 7u, 3) * 2.0f - 1.0f;
            default: return 1.0f;
        }
    };
    // Huella poligonal (lados ≥ 3): distancia con signo a un polígono regular en el plano tangente.
    auto footprint = [&](double x, double y) -> float {
        if (st.sides < 3) return (float)(std::sqrt(x * x + y * y) - (double)st.r);
        const double ang = std::atan2(y, x), sector = 6.283185307179586 / (double)st.sides;
        const double a = std::fmod(std::fabs(ang) + sector * 0.5, sector) - sector * 0.5;   // ángulo dentro del sector
        const double apothem = (double)st.r * std::cos(sector * 0.5);
        return (float)(std::sqrt(x * x + y * y) * std::cos(a) - apothem);
    };
    const bool needCut = (op != Brush::Add), needAdded = (op != Brush::Remove);
    if (op == Brush::Smooth) { if (c.d.empty() && c.a.empty()) return 0; }
    else { if (needCut) c.reserveCut(); if (needAdded) c.reserveAdded(); }

    // Suavizar: media 3x3x3 por canal, calculada ENTERA antes de escribir (si no, cada vóxel vería
    // a sus vecinos ya suavizados y el resultado dependería del orden). Los nodos del borde del
    // chunk no se tocan: así los dos chunks que comparten una cara siguen teniendo el mismo valor
    // ahí (costura exacta) — a cambio, una línea de un nodo sin suavizar en cada cara de chunk.
    std::vector<float> newD, newA;
    if (op == Brush::Smooth) {
        auto smoothChannel = [&](const std::vector<float>& ch, std::vector<float>& outv) {
            if (ch.empty()) return;
            outv = ch;
            for (int z = std::max(z0, 1); z <= std::min(z1, kVoxN - 2); ++z)
                for (int y = std::max(y0, 1); y <= std::min(y1, kVoxN - 2); ++y)
                    for (int x = std::max(x0, 1); x <= std::min(x1, kVoxN - 2); ++x) {
                        const float w = weight(sdf(c.worldOfGrid(x, y, z))) * std::clamp(st.amount, 0.0f, 1.0f);
                        if (w <= 0.0f) continue;
                        float sum = 0.0f;
                        for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                            sum += ch[(size_t)(z + dz) * kVoxN * kVoxN + (size_t)(y + dy) * kVoxN + (size_t)(x + dx)];
                        const size_t idx = (size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x;
                        outv[idx] = ch[idx] + (sum / 27.0f - ch[idx]) * w;
                    }
        };
        smoothChannel(c.d, newD); smoothChannel(c.a, newA);
    }

    int cambiados = 0;
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::dvec3 p = c.worldOfGrid(x, y, z);
                const float sd = sdf(p);
                const size_t idx = (size_t)z * kVoxN * kVoxN + (size_t)y * kVoxN + x;
                // "Cambia de medio" = alguno de los dos canales cruza el cero (el chunk no sabe del
                // terreno, así que no puede juzgar el campo completo; para el conteo basta).
                const float dAntes = c.cutAt(x, y, z), aAntes = c.addedAt(x, y, z);
                float* d = c.d.empty() ? nullptr : &c.d[idx];
                float* a = c.a.empty() ? nullptr : &c.a[idx];
                switch (op) {
                    case Brush::Remove:   // abrir = unir aire (min con la distancia)
                        if (d) *d = std::min(*d, sd);
                        if (a) *a = std::min(*a, sd);
                        break;
                    case Brush::Add:      // rellenar = unir roca (max con −distancia)
                        if (d) *d = std::max(*d, -sd);
                        if (a) *a = std::max(*a, -sd);
                        break;
                    case Brush::Raise: case Brush::Lower: case Brush::Flatten: {
                        // Dentro de la huella (peso w): roca bajo el plano (Subir/Allanar) y aire
                        // sobre él (Bajar/Allanar). La huella es la forma SIN su alto: se usa la
                        // distancia lateral (cilindro infinito) para que el plano mande en vertical.
                        const glm::dvec3 rel = p - centerW;
                        const double h = glm::dot(rel, up);
                        float lat;
                        if (shape == Brush::Box) { lat = (float)std::max(std::fabs(glm::dot(rel, e1)) - st.r, std::fabs(glm::dot(rel, e2)) - st.r); }
                        else lat = (float)(glm::length(rel - up * h) - (double)st.r);
                        const float w = weight(lat);
                        if (w <= 0.0f) break;
                        const float pl = std::clamp(plane(p), -kVoxRockM, kVoxRockM);
                        if (op != Brush::Lower) {   // roca bajo el plano: como Add con el semiespacio, atenuado por w
                            const float target = pl;
                            if (d) *d = std::max(*d, *d + (target - *d) * w);
                            if (a) *a = std::max(*a, *a + (target - *a) * w);
                        }
                        if (op != Brush::Raise) {   // aire sobre el plano: como Remove con el semiespacio
                            const float target = pl;
                            if (d) *d = std::min(*d, *d + (target - *d) * w);
                            if (a) *a = std::min(*a, *a + (target - *a) * w);
                        }
                        break;
                    }
                    case Brush::Smooth:
                        if (d && !newD.empty()) *d = newD[idx];
                        if (a && !newA.empty()) *a = newA[idx];
                        break;
                    case Brush::Sculpt: {
                        const glm::dvec3 rel = p - centerW;
                        const double h = glm::dot(rel, up), x = glm::dot(rel, e1), y = glm::dot(rel, e2);
                        const float fp = footprint(x, y);                   // < 0 dentro de la huella
                        if (fp >= 0.0f || std::fabs(h) > (double)st.r + std::fabs(st.amount) + 8.0) break;
                        const float xn = 1.0f + fp / std::max(st.r, 0.5f);   // 0 centro .. 1 borde
                        const float w = falloffOf(xn) * (st.hardness < 1.0f ? weight(fp) : 1.0f);
                        if (w <= 0.0f) break;
                        const float delta = st.amount * w * patternOf(x, y);
                        // F actual con el terreno; F' explícito en los dos canales.
                        const glm::dvec3 dir = p / std::max(glm::length(p), 1e-9);
                        const float t = (float)std::clamp((m_radius + (m_elev ? (double)m_elev(dir) : 0.0)) - glm::length(p), -(double)kVoxRockM, (double)kVoxRockM);
                        const float F = std::max(std::min(d ? *d : kVoxRockM, t), a ? *a : -kVoxRockM);
                        // Sólo cerca de la superficie: lejos el campo está saturado y no hay nada que desplazar.
                        if (std::fabs(F) >= kVoxRockM - 0.01f) break;
                        const float Fn = std::clamp(F + delta, -kVoxRockM, kVoxRockM);
                        if (d) *d = Fn;
                        if (a) *a = Fn;
                        break;
                    }
                }
                if (d) *d = std::clamp(*d, -kVoxRockM, kVoxRockM);
                if (a) *a = std::clamp(*a, -kVoxRockM, kVoxRockM);
                if ((c.cutAt(x, y, z) < 0.0f) != (dAntes < 0.0f) || (c.addedAt(x, y, z) < 0.0f) != (aAntes < 0.0f)) ++cambiados;
            }
    return cambiados;
}

int VoxWorld::stroke(const glm::dvec3& worldPos, const Brush& brush) {
    if (!configured() || brush.radiusM <= 0.0f) return 0;
    // Todos los chunks que la forma puede tocar, por POSICIÓN (funciona a través de las caras del
    // cubo): los 27 puntos a ±alcance alrededor del centro (y del otro extremo, en la cápsula)
    // caen en todos los chunks que cruza mientras el alcance < lado del chunk (128 m).
    const double r = glm::length(worldPos);
    if (r < 1e-9) return 0;
    double reach = (double)std::max(brush.radiusM, brush.halfM) * (brush.shape == Brush::Box ? 1.42 : 1.0);
    if (brush.op == Brush::Raise || brush.op == Brush::Lower || brush.op == Brush::Flatten || brush.op == Brush::Sculpt) reach = std::max(reach, (double)brush.radiusM + std::fabs((double)brush.amountM) + 2.0);
    std::vector<VoxKey> keys;
    auto addAround = [&](const glm::dvec3& centre) {
        const glm::dvec3 up = glm::normalize(centre);
        const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const glm::dvec3 p = centre + (e1 * (double)dx + e2 * (double)dy + up * (double)dz) * reach;
                    const VoxKey k = keyAt(p);
                    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
                }
    };
    addAround(worldPos);
    if (brush.shape == Brush::Capsule) {
        // Y a lo largo del segmento, cada medio chunk.
        const glm::dvec3 ab = brush.capsuleEnd - worldPos;
        const int n = std::max(1, (int)std::ceil(glm::length(ab) / (0.5 * kVoxChunkM)));
        for (int i = 1; i <= n; ++i) addAround(worldPos + ab * ((double)i / (double)n));
    }
    int total = 0;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    const uint64_t seq = ++m_seq;
    for (const VoxKey& k : keys) {
        VoxChunk& c = ensureNoLock(k);
        const Stroke st = strokeFrom(c, worldPos, brush, m_designerMode, seq);
        const int n = applyLocal(c, st);
        // Se registra aunque no cambie ningún vóxel: el signo del campo puede no cambiar y aun así
        // haberse movido la pared unos centímetros — y eso también hay que reproducirlo al cargar.
        m_strokes[k].push_back(st);
        c.edited = true; c.dirty = true;
        markNeighboursDirty(k);
        total += n;
        ++m_version;
    }
    m_history.push_back({ worldPos, brush, brush.op == Brush::Remove, m_designerMode, seq, keys });
    m_redo.clear();
    return total;
}

void VoxWorld::rebakeNoLock(VoxChunk& c, const VoxKey& key) {
    // Horneado + lo que queda de trazos, en orden. Es lo que hace `ensure` al cargar, sin tocar
    // el fichero (la lista en memoria es la verdad de la sesión).
    bake(c);
    for (const Stroke& st : m_strokes[key]) applyLocal(c, st);
    c.edited = !m_strokes[key].empty();
    c.dirty = true;
    markNeighboursDirty(key);
    ++m_version;
}

bool VoxWorld::undo() {
    std::unique_lock<std::shared_mutex> lk(m_mx);
    if (m_history.empty()) return false;
    HistoryEntry h = m_history.back();
    m_history.pop_back();
    for (const VoxKey& k : h.keys) {
        auto it = m_strokes.find(k);
        if (it == m_strokes.end()) continue;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Stroke& s) { return s.seq == h.seq; }), v.end());
        if (VoxChunk* c = const_cast<VoxChunk*>(findNoLock(k))) rebakeNoLock(*c, k);
        else saveChunkStrokes(k);   // descargado: su fichero aún tenía el trazo
    }
    m_redo.push_back(h);
    return true;
}

bool VoxWorld::redo() {
    HistoryEntry h;
    std::vector<HistoryEntry> resto;
    {
        std::unique_lock<std::shared_mutex> lk(m_mx);
        if (m_redo.empty()) return false;
        h = m_redo.back();
        m_redo.pop_back();
        resto = m_redo;   // `stroke` vacía la pila de rehacer; lo que quedaba sigue valiendo (va en orden)
    }
    const bool dm = m_designerMode;
    m_designerMode = h.designer;
    stroke(h.worldPos, h.brush);
    m_designerMode = dm;
    std::unique_lock<std::shared_mutex> lk(m_mx);
    m_redo = resto;
    return true;
}

bool VoxWorld::raycast(const glm::dvec3& o, const glm::dvec3& dirIn, double maxM, glm::dvec3& outHit) const {
    if (!configured() || maxM <= 0.0) return false;
    const glm::dvec3 dir = glm::normalize(dirIn);
    if (density(o) >= 0.0f) return false;
    // Desde lejos (la cámara del editor a cientos de km) no se marcha el vacío: se salta a la
    // entrada en la esfera de "todo lo que puede haber" (R + 12 km: montañas + islas) y se marcha
    // desde ahí. Sin esto, 100 km a pasos de 2 m eran 50 000 muestras por frame.
    double tPrev = 0.0;
    {
        const double Rmax = m_radius + 12000.0;
        const double b = glm::dot(o, dir), c = glm::dot(o, o) - Rmax * Rmax;
        if (c > 0.0) {   // fuera de la esfera
            const double disc = b * b - c;
            if (disc <= 0.0) return false;
            const double tEnter = -b - std::sqrt(disc);
            if (tEnter < 0.0 || tEnter > maxM) return false;
            tPrev = tEnter;
        }
    }
    // Paso ADAPTATIVO: el campo es una distancia con signo (aprox.), así que cuanto más aire hay,
    // más se puede avanzar sin cruzar nada; junto a la superficie el paso baja a 25 cm. Con un paso
    // fijo de 2 m un rayo rasante se saltaba la cresta de una loma (menos de 2 m de roca a lo largo
    // del rayo) y pegaba en la ladera de detrás: "no prioriza la parte más cercana". El terreno
    // aporta la distancia VERTICAL, no la euclídea: se avanza la mitad, que cubre pendientes de
    // hasta 60°. Luego bisección a 1 cm.
    double t = tPrev;
    float dPrev = density(o + dir * t);
    for (int guard = 0; guard < 400000 && t < maxM; ++guard) {
        const double step = std::clamp(0.5 * (double)(-dPrev), 0.25, 8.0);
        const double tc = std::min(t + step, maxM);
        const float dc = density(o + dir * tc);
        if (dc >= 0.0f) {
            double a = t, b = tc;
            for (int i = 0; i < 12; ++i) {
                const double m = 0.5 * (a + b);
                (density(o + dir * m) >= 0.0f ? b : a) = m;
            }
            outHit = o + dir * b;
            return true;
        }
        t = tc; dPrev = dc;
    }
    return false;
}

float VoxWorld::surfaceCut(const glm::dvec3& dir) const {
    if (!configured()) return 0.0f;
    const glm::dvec3 d = glm::normalize(dir);
    const double elev = m_elev ? (double)m_elev(d) : 0.0;
    // ⚠️ A 0,75 m, no a 3: el recorte del suelo tiene que empezar donde la PARED de la cueva llega
    // a la superficie, y la pared llega hasta donde hay aire justo bajo el suelo. A 3 m el terreno
    // se recortaba antes de que la pared existiera y quedaba una cornisa sin dibujar en el borde.
    const glm::dvec3 p = d * (m_radius + elev - 0.75);
    std::shared_lock<std::shared_mutex> lk(m_mx);
    if (!findNoLock(keyAt(p))) return 0.0f;
    return std::clamp(-std::max(caveDensityNoLock(p), addedDensityNoLock(p)) / 2.0f, 0.0f, 1.0f);
}

void VoxWorld::buildMesh(const VoxKey& key, CaveMesh& out, size_t maxTris) {
    out.positions.clear(); out.normals.clear(); out.indices.clear();
    std::unique_lock<std::shared_mutex> lk(m_mx);
    VoxChunk* c = const_cast<VoxChunk*>(findNoLock(key));
    if (!c) return;
    // Roca maciza sin reservar: su única isosuperficie sería la del terreno, que no se emite. Nada
    // que mallar (y nada que muestrear: 34³ evaluaciones ahorradas por chunk).
    if (c->d.empty() && c->a.empty()) { c->dirty = false; return; }
    // Rejilla con DELANTAL: un nodo más allá de cada cara, muestreado del vecino con `density`.
    // Es lo que hace que la malla de dos chunks case en la costura: los dos ven el mismo campo a
    // ambos lados del plano. Si el vecino no está cargado, `density` dice roca y aquí aparece una
    // pared — por eso `ensure` marca `dirty` a los vecinos: en cuanto carga, esta malla se rehace.
    // ⚠️ SÓLO LA PARTE QUE ES DE LA CUEVA. El campo completo es `min(terreno, cueva)`, y su
    // isosuperficie incluye la SUPERFICIE DEL TERRENO entera — que ya la dibuja el pase de nodos, y
    // mejor. Aquí se cose un quad sólo cuando la arista que cruza el cero la cruza por la CUEVA (en
    // sus dos extremos `cueva <= terreno`): así salen las paredes, el pozo y el labio de la boca,
    // y nada del suelo. La cota del terreno se muestrea UNA vez por columna (34² llamadas, no 34³).
    const int n = kVoxN + 2;
    std::vector<float> f((size_t)n * n * n);
    std::vector<uint8_t> own((size_t)n * n * n, 0);
    std::vector<double> ground((size_t)n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const glm::dvec3 p = c->worldOfGrid(x - 1, y - 1, 0);
            const glm::dvec3 dir = glm::normalize(p);
            ground[(size_t)y * n + x] = m_radius + (m_elev ? (double)m_elev(dir) : 0.0);
        }
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const bool interior = x >= 1 && y >= 1 && z >= 1 && x <= kVoxN && y <= kVoxN && z <= kVoxN;
                const glm::dvec3 p = c->worldOfGrid(x - 1, y - 1, z - 1);
                // ⚠️ DELANTAL SIN VECINO CARGADO: no se lee "roca" (eso cosía una pared plana que
                // partía la cueva en dos en el borde del streaming) sino el campo horneado con las
                // cuevas de ESTE chunk. Lo que no puede saber: trazos del jugador en ese vecino
                // (están en su fichero) y cuevas que crucen el vecino pero no éste — raro, y el
                // vecino se remalla al cargar.
                static const bool s_apronRock = [] { const char* e = std::getenv("HARUKA_VOX_APRONROCK"); return e && e[0] == '1'; }();   // A/B: el delantal viejo
                float dc, da;
                if (interior) { dc = c->cutAt(x - 1, y - 1, z - 1); da = c->addedAt(x - 1, y - 1, z - 1); }
                else if (findNoLock(keyAt(p))) { dc = caveDensityNoLock(p); da = addedDensityNoLock(p); }
                else { dc = s_apronRock ? kVoxRockM : bakedFieldNoLock(*c, p); da = bakedAddedNoLock(*c, p); }
                const float dt = (float)std::clamp(ground[(size_t)y * n + x] - glm::length(p),
                                                   -(double)kVoxRockM, (double)kVoxRockM);
                const float base = std::min(dc, dt);
                f[(size_t)z * n * n + (size_t)y * n + x] = std::max(base, da);
                // bit 0: el aire es de la cueva · bit 1: bajo el terreno o roca añadida (extremo de
                // roca válido) · bit 2: la roca la pone el canal AÑADIDO (isla, construido).
                const bool added = da >= 0.0f && da > base;
                own[(size_t)z * n * n + (size_t)y * n + x] = (uint8_t)(((dc <= dt) ? 1u : 0u)
                                                                       | ((dt >= -0.5f || added) ? 2u : 0u)
                                                                       | (added ? 4u : 0u));
            }
    out.origin = c->origin;
    const VoxChunk& cc = *c;
    harukaSurfaceNets(n, f.data(), [&](float x, float y, float z) {
        return glm::vec3(cc.worldOfGrid(x - 1.0, y - 1.0, z - 1.0) - cc.origin);
    }, out, maxTris, own.data());
    // ── EL FALDÓN: la pared llega hasta el suelo ────────────────────────────────────────────────
    // La celda donde la roca pasa a aire DE SUPERFICIE es una costura del terreno y no se emite (si
    // se emitiera, dibujaría el suelo dos veces). Así la pared se paraba media celda por debajo del
    // suelo y entre ella y el recorte del terreno quedaba una franja vacía ("la costura al terreno
    // no está"). Los vértices de las celdas que cruzan la superficie se PROYECTAN radialmente a la
    // cota exacta del terreno: la pared toca el suelo donde el suelo se corta.
    for (size_t i = 0; i < out.positions.size(); ++i) {
        const glm::dvec3 pw = cc.origin + glm::dvec3(out.positions[i]);
        const glm::dvec3 g = cc.gridOf(pw);
        const int gx = (int)std::floor(g.x), gy = (int)std::floor(g.y), gz = (int)std::floor(g.z);
        bool arriba = false, abajo = false, cueva = false;
        for (int dz = 0; dz <= 1; ++dz)
            for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx) {
                    const int ax = gx + dx + 1, ay = gy + dy + 1, az = gz + dz + 1;   // índices del delantal
                    if (ax < 0 || ay < 0 || az < 0 || ax >= n || ay >= n || az >= n) continue;
                    const double dt = ground[(size_t)ay * n + ax] - glm::length(cc.worldOfGrid(ax - 1, ay - 1, az - 1));
                    (dt < 0.0 ? arriba : abajo) = true;
                    cueva = cueva || (own[(size_t)az * n * n + (size_t)ay * n + ax] & 1u) != 0u;
                }
        // Sólo las celdas donde una CUEVA cruza el suelo: un parapeto construido sobre el suelo
        // también cruza la superficie, y proyectarlo lo aplastaría contra ella.
        if (!(arriba && abajo && cueva)) continue;
        const glm::dvec3 dir = glm::normalize(pw);
        const double groundR = m_radius + (m_elev ? (double)m_elev(dir) : 0.0);
        out.positions[i] = glm::vec3(dir * groundR - cc.origin);
    }

    // ⚠️ LAS NORMALES, DEL CAMPO Y NO DE LA REJILLA. Surface nets saca la normal de diferencias
    // entre nodos de la rejilla con los índices SUJETADOS al borde: en la última celda de un chunk
    // el gradiente se evalúa un nodo hacia dentro, y el chunk vecino —que reconstruye esa misma
    // celda desde su delantal— lo evalúa hacia SU dentro. Misma posición, distinta normal: una
    // línea de sombreado en cada costura (Andoni: "la costura no encaja"). El gradiente del campo
    // en la posición del vértice es el mismo desde los dos lados por construcción.
    // `HARUKA_VOX_GRIDNORMALS=1`: deja las normales de rejilla (A/B de la costura). Medido en
    // `test_vox_world`: peor desviación entre las normales de un vértice compartido 30,47° con la
    // rejilla, 0,02° con el campo.
    static const bool s_gridNormals = [] { const char* e = std::getenv("HARUKA_VOX_GRIDNORMALS"); return e && e[0] == '1'; }();
    for (size_t i = 0; !s_gridNormals && i < out.positions.size(); ++i) {
        const glm::dvec3 p = cc.origin + glm::dvec3(out.positions[i]);
        const glm::dvec3 up = glm::normalize(p);
        const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
        const double h = 1.0;
        auto F = [&](const glm::dvec3& q) {
            const float dcq = caveDensityNoLock(q);
            const double gq = m_radius + (m_elev ? (double)m_elev(glm::normalize(q)) : 0.0);
            const float base = std::min(dcq, (float)std::clamp(gq - glm::length(q), -(double)kVoxRockM, (double)kVoxRockM));
            return std::max(base, addedDensityNoLock(q));
        };
        const glm::vec3 g((F(p + e1 * h) - F(p - e1 * h)) / (2.0f * (float)h),
                          (F(p + e2 * h) - F(p - e2 * h)) / (2.0f * (float)h),
                          (F(p + up * h) - F(p - up * h)) / (2.0f * (float)h));
        const glm::vec3 gw = glm::vec3(e1) * g.x + glm::vec3(e2) * g.y + glm::vec3(up) * g.z;
        const float gl = glm::length(gw);
        if (gl > 1e-6f) out.normals[i] = gw / gl;
    }
    c->dirty = false;
}

void VoxWorld::unloadFar(const glm::dvec3& nearDir, double keepRadiusM) {
    std::unique_lock<std::shared_mutex> lk(m_mx);
    const glm::dvec3 nd = glm::normalize(nearDir);
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        const double ang = std::acos(std::clamp(glm::dot(it->second.centerDir, nd), -1.0, 1.0));
        if (ang * m_radius <= keepRadiusM) { ++it; continue; }
        if (it->second.edited) saveChunkStrokes(it->first);
        it = m_chunks.erase(it);
        ++m_version;
    }
}

bool VoxWorld::saveEdits() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    if (m_save.empty() && !(m_designerMode && !m_designerDir.empty())) return false;
    for (const auto& kv : m_strokes) saveChunkStrokes(kv.first);
    return true;
}

float VoxWorld::seamMismatch(int* outFaces) const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    float worst = 0.0f; int faces = 0;
    const int N = kVoxN - 1;
    for (const auto& [ka, a] : m_chunks) {
        if (a.d.empty() && a.a.empty()) continue;   // roca maciza sin reservar: nada que comparar desde aquí
        // Vecinos en +u, +v y +r por POSICIÓN (funciona a través de las aristas del cubo).
        const glm::dvec3 probes[3] = { a.worldOfGrid(N + 1.0, N * 0.5, N * 0.5),
                                       a.worldOfGrid(N * 0.5, N + 1.0, N * 0.5),
                                       a.worldOfGrid(N * 0.5, N * 0.5, N + 1.0) };
        for (int axis = 0; axis < 3; ++axis) {
            const VoxChunk* b = findNoLock(keyAt(probes[axis]));
            if (!b || b == &a) continue;
            ++faces;
            for (int i = 0; i <= N; i += 3)
                for (int j = 0; j <= N; j += 3) {
                    const double x = (axis == 0) ? N : (axis == 1 ? i : i);
                    const double y = (axis == 0) ? i : (axis == 1 ? N : j);
                    const double z = (axis == 0) ? j : (axis == 1 ? j : N);
                    const glm::dvec3 pw = a.worldOfGrid(x, y, z);
                    const glm::dvec3 gb = b->gridOf(pw);
                    const int bx = (int)std::lround(gb.x), by = (int)std::lround(gb.y), bz = (int)std::lround(gb.z);
                    if (bx < 0 || by < 0 || bz < 0 || bx > N || by > N || bz > N) continue;
                    const float va = a.cutAt((int)x, (int)y, (int)z), vb = b->cutAt(bx, by, bz);
                    const float dd = std::max(std::fabs(va - vb), std::fabs(a.addedAt((int)x, (int)y, (int)z) - b->addedAt(bx, by, bz)));
                    if (dd > worst) {
                        worst = dd;
                        // `HARUKA_VOX_SEAMLOG=1`: quién es el peor par (para saber si es un chunk
                        // provisional, uno sin reservar, o un campo de verdad distinto).
                        static const bool s_log = [] { const char* e = std::getenv("HARUKA_VOX_SEAMLOG"); return e && e[0] == '1'; }();
                        if (s_log && dd > 0.01f)
                            HARUKA_LOGI("Vox", "costura %.3f m: (%d,%d,%d,%d)[%d,%d,%d]=%.2f (d %s, pend %d, edit %d) vs (%d,%d,%d,%d)[%d,%d,%d]=%.2f (d %s, pend %d, edit %d)",
                                        dd, ka.face, ka.i, ka.j, ka.k, (int)x, (int)y, (int)z, va, a.d.empty() ? "vacio" : "lleno", a.pendingCave ? 1 : 0, a.edited ? 1 : 0,
                                        b->key.face, b->key.i, b->key.j, b->key.k, bx, by, bz, vb, b->d.empty() ? "vacio" : "lleno", b->pendingCave ? 1 : 0, b->edited ? 1 : 0);
                    }
                }
        }
    }
    if (outFaces) *outFaces = faces;
    return worst;
}

size_t VoxWorld::voxelBytes() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    size_t b = 0;
    for (const auto& kv : m_chunks) b += (kv.second.d.size() + kv.second.a.size()) * sizeof(float);
    return b;
}

size_t VoxWorld::loadedWithContent() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    size_t n = 0;
    for (const auto& kv : m_chunks) if (!kv.second.d.empty() || !kv.second.a.empty()) ++n;
    return n;
}

std::vector<VoxKey> VoxWorld::loadedKeys() const {
    std::shared_lock<std::shared_mutex> lk(m_mx);
    std::vector<VoxKey> k;
    k.reserve(m_chunks.size());
    for (const auto& kv : m_chunks) k.push_back(kv.first);
    return k;
}

} // namespace Haruka
