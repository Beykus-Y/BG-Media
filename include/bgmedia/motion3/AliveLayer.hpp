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

#include <cstdint>

namespace bgmedia::motion {

// Applies the layer to `pose` in place.
//   wallTime  seconds on a monotonic clock that keeps running while the
//             transport is paused. Non-finite values are read as 0.
//   energy    0..1 choreography energy (ClipPose::energy). 0 = full amplitude,
//             about 0.35 and above = the dancing level. Non-finite = 0.
//   seed      per-character phase and frequency jitter.
//   weight    0..1 overall strength. 0 leaves the pose untouched (identity).
// Offsets are composed like the other overlays: bone = delta * bone, with the
// delta authored about the clip-frame axes (+X left, +Y up, +Z forward).
void applyAlive(ClipPose& pose, double wallTime, float energy, std::uint32_t seed,
                float weight);

// Amplitude multiplier the energy input produces: 1 at idle, about 0.3 when
// dancing. Exposed for diagnostics and tests.
[[nodiscard]] float aliveEnergyScale(float energy);

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
