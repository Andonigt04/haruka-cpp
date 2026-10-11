/**
 * @file VoxFineBricks.cpp
 * @brief The fine bricks around a focus point. See `VoxFineBricks.h`.
 */
#include "Haruka/World/Vox/VoxFineBricks.h"
#include "Haruka/Core/Jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace Haruka::World::Vox {

VoxFineBricks::~VoxFineBricks() { finishJobs(); }

void VoxFineBricks::finishJobs() {
    for (auto& kv : m_slots) if (kv.second.building) { kv.second.job.wait(); collect(kv.second); }
}

void VoxFineBricks::collect(Slot& s) {
    const Built r = s.job.get();
    s.building = false;
    s.chunkRev = r.chunkRev;
    s.b.walls = r.walls; s.b.solid = r.solid; s.b.ground = r.ground; s.b.air = r.air;
    s.b.revision = m_version.fetch_add(1, std::memory_order_relaxed) + 1;   // unique and growing
}

void VoxFineBricks::update(VoxWorld& world, const glm::dvec3& focus) {
    for (auto& kv : m_slots) kv.second.b.wanted = false;
    // What the jobs finished.
    for (auto& kv : m_slots)
        if (kv.second.building && kv.second.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) collect(kv.second);

    // WHICH bricks: cells of the loaded chunks within the radius (plus their half diagonal) that can
    // have walls of their own (`fineBrickMayHaveWalls`). Nearest first.
    struct Want { double dist; Key key; double halfDiag; bool crossesGround; };
    std::vector<Want> want;
    for (const VoxKey& k : world.loadedKeys()) {
        const VoxChunk* c = world.get(k);
        if (!c) continue;
        const glm::vec3 mpv = c->metresPerVoxel();
        const double halfDiag = 0.5 * glm::length(glm::dvec3(mpv));
        const glm::dvec3 cc = c->centerDir * (c->baseR + 0.5 * kVoxChunkM);
        const double chunkR = 0.5 * std::sqrt((double)c->sideU * c->sideU + (double)c->sideV * c->sideV + kVoxChunkM * kVoxChunkM);
        if (glm::length(cc - focus) > kRadiusM + chunkR) continue;
        const glm::dvec3 g = c->gridOf(focus);
        const int rx = (int)std::ceil((kRadiusM + 2.0 * halfDiag) / mpv.x), ry = (int)std::ceil((kRadiusM + 2.0 * halfDiag) / mpv.y),
                  rz = (int)std::ceil((kRadiusM + 2.0 * halfDiag) / mpv.z);
        const int last = VoxWorld::kFineCellsPerAxis - 1;
        const int x0 = std::clamp((int)std::floor(g.x) - rx, 0, last), x1 = std::clamp((int)std::floor(g.x) + rx, 0, last);
        const int y0 = std::clamp((int)std::floor(g.y) - ry, 0, last), y1 = std::clamp((int)std::floor(g.y) + ry, 0, last);
        const int z0 = std::clamp((int)std::floor(g.z) - rz, 0, last), z1 = std::clamp((int)std::floor(g.z) + rz, 0, last);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const glm::dvec3 centre = c->worldOfGrid(x + 0.5, y + 0.5, z + 0.5);
                    const double dist = glm::length(centre - focus);
                    if (dist > kRadiusM + halfDiag) continue;
                    if (!world.fineBrickMayHaveWalls(k, glm::ivec3(x, y, z))) continue;
                    // Does the GROUND cross this cell (at its centre column)? Radial range of the cell.
                    const glm::dvec3 dir = glm::normalize(centre);
                    const double groundR = world.planetRadius() + (double)world.elevAt(dir) + (double)world.surfaceDeltaM(dir);
                    const double rLo = glm::length(c->worldOfGrid(x + 0.5, y + 0.5, z)), rHi = glm::length(c->worldOfGrid(x + 0.5, y + 0.5, z + 1));
                    want.push_back({ dist, { k, glm::ivec3(x, y, z) }, halfDiag, groundR >= rLo && groundR <= rHi });
                }
    }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.dist < b.dist; });

    // BUILD in the JOB SYSTEM: a brick with a wall samples ~43 000 points of the analytic field.
    int inFlight = 0;
    for (const auto& kv : m_slots) inFlight += kv.second.building ? 1 : 0;
    for (const Want& w : want) {
        Slot& s = m_slots[w.key];
        s.b.key = w.key; s.b.wanted = true; s.b.dist = w.dist; s.b.halfDiag = w.halfDiag;
        const VoxChunk* c = world.get(w.key.key);
        const uint64_t rev = c ? c->revision : 0;
        if (s.building || (s.b.built() && s.chunkRev == rev) || inFlight >= kJobs) continue;
        VoxWorld* wp = &world;
        const VoxKey key = w.key.key;
        const glm::ivec3 cell = w.key.cell;
        const bool crosses = w.crossesGround;
        const glm::dvec3 centre = c ? c->worldOfGrid(cell.x + 0.5, cell.y + 0.5, cell.z + 0.5) : glm::dvec3(0.0);
        s.job = Core::Jobs::jobs().submit(Core::Jobs::Priority::Frame, [wp, key, cell, rev, crosses, centre]() {
            auto walls = std::make_shared<CaveMesh>();
            auto solid = std::make_shared<CaveMesh>();
            wp->buildFineMesh(key, cell, kS, *walls, 200000, crosses ? solid.get() : nullptr);
            Built r;
            r.chunkRev = rev;
            r.ground = crosses && !walls->empty() && !solid->empty();
            r.air = wp->fieldAt(centre) < 0.0f;
            r.walls = walls;
            if (r.ground) r.solid = solid;
            return r;
        });
        s.building = true;
        ++inFlight;
    }
    // Far bricks are dropped (one still building waits for its job first).
    bool dropped = false;
    for (auto it = m_slots.begin(); it != m_slots.end();) {
        if (it->second.b.wanted || it->second.building) { ++it; continue; }
        it = m_slots.erase(it); dropped = true;
    }
    if (dropped) m_version.fetch_add(1, std::memory_order_relaxed);
    // The radius within which everything wanted is built.
    double r = want.empty() ? 0.0 : kRadiusM;
    for (const auto& kv : m_slots)
        if (kv.second.b.wanted && !kv.second.b.built()) r = std::min(r, kv.second.b.dist - kv.second.b.halfDiag);
    m_builtRadius = std::max(r, 0.0);
    publishSnapshot(world);
}

void VoxFineBricks::forEach(const std::function<void(const Brick&)>& fn) const {
    for (const auto& kv : m_slots) fn(kv.second.b);
}

void VoxFineBricks::publishSnapshot(const VoxWorld& world) {
    auto snap = std::make_shared<Snapshot>();
    for (const auto& kv : m_slots) {
        const Brick& b = kv.second.b;
        if (!b.built()) continue;
        snap->covered.insert(b.key);
        if (b.ground) snap->ground.insert(b.key);
        if (!snap->frames.count(b.key.key))
            if (const VoxChunk* c = world.get(b.key.key)) {
                VoxChunk f;   // the FRAME only: what `gridOf` needs
                f.key = c->key; f.u0 = c->u0; f.u1 = c->u1; f.v0 = c->v0; f.v1 = c->v1; f.baseR = c->baseR;
                f.centerDir = c->centerDir; f.origin = c->origin; f.sideU = c->sideU; f.sideV = c->sideV;
                snap->frames.emplace(b.key.key, std::move(f));
            }
    }
    std::lock_guard<std::mutex> lk(m_snapMx);
    m_snap = std::move(snap);
}

bool VoxFineBricks::replacesGroundAt(const VoxWorld& world, const glm::dvec3& p) const {
    std::shared_ptr<const Snapshot> snap;
    { std::lock_guard<std::mutex> lk(m_snapMx); snap = m_snap; }
    if (!snap || snap->ground.empty()) return false;
    auto inGround = [&](const glm::dvec3& q) {
        const VoxKey k = world.keyAt(q);
        auto f = snap->frames.find(k);
        if (f == snap->frames.end()) return false;
        const glm::dvec3 g = f->second.gridOf(q);
        return snap->ground.count({ k, glm::ivec3((int)std::floor(g.x), (int)std::floor(g.y), (int)std::floor(g.z)) }) > 0;
    };
    if (!inGround(p)) return false;
    // `kGroundInsetM` inside: the four points that far around it (tangent plane) are in ground bricks too.
    const double len = glm::length(p);
    if (len < 1e-9) return false;
    const glm::dvec3 up = p / len;
    const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    return inGround(p + e1 * kGroundInsetM) && inGround(p - e1 * kGroundInsetM) &&
           inGround(p + e2 * kGroundInsetM) && inGround(p - e2 * kGroundInsetM);
}

bool VoxFineBricks::coversCell(const VoxKey& key, const glm::ivec3& cell) const {
    std::shared_ptr<const Snapshot> snap;
    { std::lock_guard<std::mutex> lk(m_snapMx); snap = m_snap; }
    return snap && snap->covered.count({ key, cell }) > 0;
}

} // namespace Haruka::World::Vox
