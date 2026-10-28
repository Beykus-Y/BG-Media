#pragma once

// BG Motion 3.4 alive layer: the small, always-on life a standing person has
// and a sampled clip does not: breathing, a slow weight shift and head
// look-around. It is applied on top of any ClipPose (authored dance, Idle or
// the paused hold) by the host, after the DancePlayer and before retargeting.
//
// The layer is a pure function of wall-clock time. It keeps no state and no
// integrator, so it is frame-rate independent and identical for identical
// inputs. Its amplitude falls as the choreography gets louder, so it does not
// fight the steps: full strength at idle, about 30 % when dancing.

#include "bgmedia/motion3/DanceClip.hpp"

#include <array>
#include <cstdint>

namespace bgmedia::motion {

// Applies the layer to `pose` in place.
//   wallTime  seconds on a monotonic clock that keeps running while the
//             transport is paused. Non-finite values are read as 0.
//   energy    0..1 choreography energy (ClipPose::energy). 0 = full amplitude,
//             about 0.35 and above = the dancing level. Non-finite = 0.
//   seed      per-character phase and frequency jitter.
//   weight    0..1 overall strength. 0 leaves the pose untouched (identity).
//   bodyWeight 0..1 extra scale on breathing and the weight shift (chest,
//             shoulders, hips). The host lowers it as the Idle clip takes over,
//             because Idle already breathes and shifts its weight.
//   headWeight 0..1 extra scale on the head and neck look-around.
// The two extra weights multiply `weight`; the defaults keep the full layer.
// Offsets are composed like the other overlays: bone = delta * bone, with the
// delta authored about the clip-frame axes (+X left, +Y up, +Z forward).
void applyAlive(ClipPose& pose, double wallTime, float energy, std::uint32_t seed,
                float weight, float bodyWeight = 1.0f, float headWeight = 1.0f);

// Amplitude multiplier the energy input produces: 1 at idle, about 0.3 when
// dancing. Exposed for diagnostics and tests.
[[nodiscard]] float aliveEnergyScale(float energy);

// Blend from a dance pose towards the Idle pose for a stopped transport.
//   mix 0 returns `dance` exactly, mix 1 takes the Idle body but keeps the dance
//   root (rootX, rootZ, rootYaw), so a dancer who has travelled stays where they are.
// Feet are handed over one at a time, each with a smootherstep: the foot with less
// contact in the dance pose (airborne) lands over mix 0..0.5, then the supporting foot
// moves over 0.5..1 (ties: left first). At every mix at least one foot sits
// exactly on one of its endpoints (the Idle end re-anchored by idleFeetAtDanceRoot),
// so one foot is always on an endpoint. A moving planted foot
// lifts by min(0.03, 0.5 * travel) model heights, sine-shaped; the lift fades in
// smoothly with the foot's dance contact (0.25..0.75) and its travel (0.5..1.5 cm). Its contact is min(lerp(dance, idle, p), 1 - sin(pi p)): it never rises
// above either end, and it drops while the foot moves. Yaw and toe roll follow the
// same p. Bones, hips and hands use the plain crossfade with `mix`.
// `firstFoot` (0 or 1) fixes which foot moves first; any other value picks it from
// `dance` with airborneFootFirst. Latch it once per transition when `dance` keeps moving.
[[nodiscard]] ClipPose blendToIdleKeepingFeet(const ClipPose& dance, const ClipPose& idle,
                                              float mix, int firstFoot = -1);

// The foot that lands first in blendToIdleKeepingFeet: the one with less contact
// (airborne), ties left (0).
[[nodiscard]] int airborneFootFirst(const ClipPose& dance);

// The Idle feet moved from Idle's root to the dance root: each foot keeps its offset
// from the root in the root's frame (the retargeter reads foot targets as absolute
// stage positions), and its yaw keeps its angle to the root heading. With equal roots
// this is exactly idle.foot. blendToIdleKeepingFeet hands the feet over to these.
[[nodiscard]] std::array<ClipPose::Foot, 2> idleFeetAtDanceRoot(const ClipPose& dance,
                                                               const ClipPose& idle);

// Wall-clock crossfade between the dance and the Idle clip, used when the
// transport has stopped (paused, ended or stopped). It holds the dance for a
// short grace period, then eases to Idle; on resume it eases back to the dance.
// Pure state machine: the host calls update() once per frame and reads mix().
class IdleCrossfade {
public:
    static constexpr double kHoldSeconds = 0.25;      // paused this long before Idle starts
    static constexpr double kToIdleSeconds = 0.6;     // dance -> Idle
    static constexpr double kToDanceSeconds = 0.4;    // Idle -> dance (resume)

    // `running` is the transport state of this frame; `wallNow` is monotonic seconds.
    void update(bool running, double wallNow);

    // 0 = dance, 1 = Idle. Smoothstep over the current transition, wall-time driven.
    [[nodiscard]] float mix(double wallNow) const;

private:
    bool running_ = true;
    double stoppedAt_ = 0.0;
    float from_ = 0.0f;
    float to_ = 0.0f;
    double start_ = 0.0;
    double duration_ = 0.0;
};

} // namespace bgmedia::motion
