#pragma once
/**
 * @file VoxFineBricks.h
 * @brief THE FINE BRICKS around a focus point: which 4 m cells get a decimetre mesh, built in the
 *        job system, for the renderer AND the physics.
 *
 * A brick is one coarse cell of a chunk subdivided `kS` times (`VoxWorld::buildFineMesh`). It lives
 * in the world module and not in the renderer because the physics needs the same bricks and a
 * headless run (tests, a server) has no RHI: the renderer uploads `walls`, the physics collides with
 * `solid` (or `walls`), and both read the bricks from here.
 *
 * Two meshes per brick:
 *   · `walls`: the brick's OWN walls (what the coarse chunk mesh would draw, finer). Drawn.
 *   · `solid`: every surface in the brick, the ground included. Only for bricks that REPLACE the
 *     collision heightfield (`ground`): a 30 cm pick cannot be opened in a heightfield of 0.5 m
 *     samples (a NaN removes every triangle touching it, a square metre), so inside such a brick the
 *     ground the physics stands on is the brick's own.
 *
 * Thread safety: `update` and `forEach` from one thread (the render thread). `replacesGroundAt` and
 * `coversCell` from ANY thread (the collision rings are built in jobs): they read an immutable
 * snapshot published at the end of `update`.
 */
#include "Haruka/World/Vox/VoxWorld.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Haruka::World::Vox {

class VoxFineBricks {
public:
    static constexpr int    kS       = 32;     ///< subdivisions per 4 m cell (~12.5 cm)
    static constexpr double kRadiusM = 12.0;   ///< bricks within this distance of the focus
    static constexpr int    kJobs    = 4;      ///< bricks built at once (~4 ms each, measured)
    /// The heightfield is replaced only this far INSIDE a ground brick (one collision sample, 0.5 m,
    /// plus margin): the triangles a NaN removes reach one sample around it, and they must stay
    /// inside the brick's own ground or a gap opens between the two. What is left is a ~0.1 m band
    /// along the brick's edge where both grounds overlap. No ground-only NEIGHBOURS are needed to
    /// open a hole near an edge: measured in `test_physics_fine_pick` (a pit 0.35 m from an edge,
    /// one in the middle, one on a corner) the same results with and without them, with half the
    /// bricks — a hole close enough to the edge to fall in that band reaches the neighbour, which
    /// then has walls of its own.
    static constexpr double kGroundInsetM = 0.6;

    struct Key {
        VoxKey key; glm::ivec3 cell{0};
        bool operator==(const Key& o) const { return key == o.key && cell == o.cell; }
    };
    struct KeyHash {
        size_t operator()(const Key& b) const {
            return VoxKeyHash{}(b.key) ^ ((size_t)((uint32_t)b.cell.x * 73856093u ^ (uint32_t)b.cell.y * 19349663u ^ (uint32_t)b.cell.z * 83492791u) << 1);
        }
    };
    struct Brick {
        Key      key;
        double   dist = 0.0;              ///< from the focus, last update
        double   halfDiag = 4.0;          ///< half the brick's diagonal (m)
        uint64_t revision = 0;            ///< bumps every time its meshes change; 0 = never built
        std::shared_ptr<const CaveMesh> walls;   ///< its own walls (drawn)
        std::shared_ptr<const CaveMesh> solid;   ///< every surface, ground included (only if `ground`)
        bool     ground = false;          ///< replaces the collision heightfield in its footprint
        bool     air = false;             ///< air at its centre (a wide pit: no wall, but cut ground)
        bool     wanted = false;          ///< chosen in the last update
        bool     built() const { return revision > 0; }
    };

    ~VoxFineBricks();

    /** @brief Chooses the bricks around `focusRelPlanet` (relative to the planet centre), starts the
     *  jobs for those that are new or stale, collects the finished ones, drops the far ones and
     *  publishes the snapshot for other threads. */
    void update(VoxWorld& world, const glm::dvec3& focusRelPlanet);
    /** @brief Waits for every job in flight and collects it (tests, shutdown). */
    void finishJobs();

    void forEach(const std::function<void(const Brick&)>& fn) const;
    size_t size() const { return m_slots.size(); }
    /** @brief Radius around the focus within which every wanted brick is built (0 = none). */
    double builtRadius() const { return m_builtRadius; }
    /** @brief Bumps whenever a brick's meshes change or a brick is dropped. */
    uint64_t version() const { return m_version.load(std::memory_order_relaxed); }

    /** @brief ANY THREAD. Does a fine brick replace the collision ground at `pRel` (relative to the
     *  planet centre)? True only `kGroundInsetM` inside a ground brick (checked at the point and at
     *  four points that far around it). */
    bool replacesGroundAt(const VoxWorld& world, const glm::dvec3& pRel) const;
    /** @brief ANY THREAD. Is this cell covered by a built brick (its coarse triangles are replaced)? */
    bool coversCell(const VoxKey& key, const glm::ivec3& cell) const;

private:
    struct Built { std::shared_ptr<const CaveMesh> walls, solid; bool ground = false, air = false; uint64_t chunkRev = 0; };
    struct Slot {
        Brick b;
        uint64_t chunkRev = ~0ull;        ///< `VoxChunk::revision` it was built from
        std::future<Built> job;
        bool building = false;
    };
    std::unordered_map<Key, Slot, KeyHash> m_slots;
    double m_builtRadius = 0.0;
    std::atomic<uint64_t> m_version{1};

    struct Snapshot {
        std::unordered_map<VoxKey, VoxChunk, VoxKeyHash> frames;   ///< chunk FRAMES only (no voxels): gridOf
        std::unordered_set<Key, KeyHash> ground, covered;
    };
    mutable std::mutex m_snapMx;
    std::shared_ptr<const Snapshot> m_snap;
    void publishSnapshot(const VoxWorld& world);
    void collect(Slot& s);
};

} // namespace Haruka::World::Vox
