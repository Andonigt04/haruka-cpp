/**
 * @file cinematic_sequencer.h
 * @brief Runtime camera cinematics: plays a data-driven `.cine` timeline that
 *        drives a Camera (position + look-at + FOV) with smooth Catmull-Rom
 *        interpolation, blend-in from the gameplay camera, and a letterbox amount.
 *
 * Authoring is intuitive: each keyframe is "camera AT `pos` LOOKING AT `look`,
 * FOV `fov`, at time `t` (seconds)". Orientation is derived each frame from
 * pos→look (matches Camera's -Z forward), so there is no quaternion authoring.
 *
 * Engine-side and reusable: any game drives it (Survival plays one via the
 * `cine` console command). Editor/timeline authoring in the `haruka` IDE is a
 * future step — this is the runtime player.
 */
#ifndef HARUKA_CINEMATIC_SEQUENCER_H
#define HARUKA_CINEMATIC_SEQUENCER_H

#include <string>
#include <vector>
#include <unordered_map>

#include "tools/math_types.h"

namespace Haruka { namespace Core { class Camera; } } using Haruka::Core::Camera;

namespace Haruka {

class CinematicSequencer {
public:
    /** @brief Loads a `.cine` JSON. Returns false if missing/invalid. */
    bool load(const std::string& path);

    /** @brief Starts playback from the beginning (no-op if no keyframes). */
    void play();
    /** @brief Stops playback (camera control returns to the game). */
    void stop();
    /** @brief True while playing (including the blend-in). */
    bool active() const { return m_playing; }

    /**
     * @brief Advances the timeline by `dt` and writes the interpolated camera.
     * @param gameplayCam camera to blend FROM during blend-in (live game camera).
     * @return active() after advancing (false → caller keeps its own camera).
     */
    bool update(double dt, Camera& out, const Camera& gameplayCam);

    /** @brief Letterbox bar amount 0..1 (eased in/out) — 0 when inactive. */
    float letterbox() const;

    /** @brief Binds a named anchor to a world position. A keyframe's `anchor`/`lookAnchor`
     *  makes its `pos`/`look` OFFSETS from that anchor (resolved to world space at play()).
     *  Set anchors (e.g. "sun", "planet", "orbit", "player") BEFORE play(). */
    void setAnchor(const std::string& name, const glm::dvec3& worldPos) { m_anchors[name] = worldPos; }

private:
    struct Key {
        double t = 0.0; glm::dvec3 pos{0.0}; glm::dvec3 look{0.0, 0.0, -1.0}; float fov = 45.0f;
        std::string anchor;      // world anchor for pos ("" = relative-to-player or absolute)
        std::string lookAnchor;  // world anchor for look ("" = same as `anchor`)
        glm::dquat orient{1.0, 0.0, 0.0, 0.0}; // resolved orientation (from pos→look), for SLERP
    };

    // Orientation is SLERP'd between keys (not the look POINT) to avoid the camera swinging
    // past / "bouncing" when targets reverse or the interpolated point passes near the camera.
    void sample(double time, glm::dvec3& pos, glm::dquat& orient, float& fov) const;
    void buildResolved();                                 // bake anchors/relative → world keys + orient
    glm::dvec3 anchorBase(const std::string& name) const; // anchor pos, else start (relative) / 0

    std::vector<Key> m_keys;       // raw, as loaded
    std::vector<Key> m_resolved;   // world-space keys (anchors + relative baked in)
    std::unordered_map<std::string, glm::dvec3> m_anchors;
    double m_time     = 0.0;
    double m_duration = 0.0;
    bool   m_playing  = false;
    bool   m_letterbox = true;
    // Relative mode: keyframe pos/look are OFFSETS added to the gameplay camera
    // position captured at play() — lets one `.cine` work anywhere on a planet.
    bool   m_relative = false;
    // Loop: restart at the end (demo-reel / showcase background). Keeps letterbox up.
    bool   m_loop = false;
    float  m_blendIn  = 0.5f;
    float  m_blendOut = 0.5f;

    // Gameplay camera snapshot captured at play() for the blend-in.
    glm::dvec3 m_startPos{0.0};
    glm::dquat m_startOrient{1.0, 0.0, 0.0, 0.0};
    float      m_startFov = 45.0f;
    bool       m_haveStart = false;
};

} // namespace Haruka

#endif // HARUKA_CINEMATIC_SEQUENCER_H
