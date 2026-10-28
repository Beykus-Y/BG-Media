// BG Motion 3.4 alive layer and idle crossfade tests. Everything here is pure:
// no model, no window, no clock. The desktop host only wires these together.
#include "framework.hpp"

#include "bgmedia/motion3/AliveLayer.hpp"
#include "bgmedia/motion3/DanceClip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

namespace motion = bgmedia::motion;
namespace math = bgmedia::math;
using motion::ClipPose;
using motion::HumanoidBone;

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

std::size_t idx(HumanoidBone bone) { return static_cast<std::size_t>(bone); }

// Rotation angle of a unit quaternion, in degrees. atan2 instead of acos: acos
// of a w that is within 1e-7 of 1 loses about 0.02 deg at these amplitudes.
double angleDeg(const math::Quat& q) {
    const double v = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
                               static_cast<double>(q.z) * q.z);
    return 2.0 * std::atan2(v, std::abs(static_cast<double>(q.w))) * kRadToDeg;
}

double angleOf(const ClipPose& pose, HumanoidBone bone) {
    return angleDeg(pose.bone[idx(bone)]);
}

bool quatIdentical(const math::Quat& a, const math::Quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

bool vecIdentical(const math::Vec3& a, const math::Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Exact comparison of every field the layer may touch (and the rest, for good measure).
bool poseIdentical(const ClipPose& a, const ClipPose& b) {
    for (std::size_t i = 0; i < a.bone.size(); ++i) {
        if (!quatIdentical(a.bone[i], b.bone[i])) {
            return false;
        }
    }
    if (!vecIdentical(a.hips, b.hips) || a.rootX != b.rootX || a.rootZ != b.rootZ ||
        a.rootYaw != b.rootYaw || a.energy != b.energy || a.accent != b.accent) {
        return false;
    }
    for (std::size_t f = 0; f < 2; ++f) {
        if (!vecIdentical(a.foot[f].position, b.foot[f].position) ||
            a.foot[f].yaw != b.foot[f].yaw || a.foot[f].contact != b.foot[f].contact ||
            a.hand[f] != b.hand[f]) {
            return false;
        }
    }
    return true;
}

bool quatFinite(const math::Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w);
}

// A pose with some non-trivial content, so "identity" checks are meaningful.
ClipPose sampleInput() {
    ClipPose pose = motion::neutralClipPose();
    pose.hips = {0.01f, 0.25f, -0.02f};
    pose.rootX = 0.05f;
    pose.rootZ = -0.03f;
    pose.rootYaw = 0.1f;
    pose.energy = 0.4f;
    pose.bone[idx(HumanoidBone::Head)] = math::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.3f);
    pose.bone[idx(HumanoidBone::Chest)] = math::fromAxisAngle({1.0f, 0.0f, 0.0f}, -0.2f);
    pose.bone[idx(HumanoidBone::LeftUpperArm)] =
        math::fromAxisAngle({0.0f, 0.0f, 1.0f}, 0.7f);
    return pose;
}

constexpr std::uint32_t kSeeds[4] = {1u, 42u, 0xdeadbeefu, 0u};

} // namespace

TEST_CASE(alive.weight_zero_is_identity) {
    const ClipPose input = sampleInput();
    for (const std::uint32_t seed : kSeeds) {
        ClipPose pose = input;
        motion::applyAlive(pose, 12.3, 0.2f, seed, 0.0f);
        CHECK(poseIdentical(pose, input));

        pose = input;
        motion::applyAlive(pose, 987.6, 0.0f, seed,
                           std::numeric_limits<float>::quiet_NaN());
        CHECK(poseIdentical(pose, input));

        pose = input;
        motion::applyAlive(pose, 987.6, 0.0f, seed, -1.0f);
        CHECK(poseIdentical(pose, input));
    }
}

TEST_CASE(alive.pose_changes_with_wall_time_at_constant_media_time) {
    // The clip is sampled at one media beat for both calls: only the wall clock differs.
    const ClipPose base = motion::neutralClipPose();
    ClipPose early = base;
    ClipPose later = base;
    motion::applyAlive(early, 1.0, 0.0f, 42u, 1.0f);
    motion::applyAlive(later, 2.6, 0.0f, 42u, 1.0f);
    CHECK(!poseIdentical(early, later));
    CHECK(!quatIdentical(early.bone[idx(HumanoidBone::Head)],
                         later.bone[idx(HumanoidBone::Head)]));
    // Over a few seconds the layer must actually move the body.
    double maxHead = 0.0;
    double maxHips = 0.0;
    for (double t = 0.0; t < 20.0; t += 0.05) {
        ClipPose pose = base;
        motion::applyAlive(pose, t, 0.0f, 42u, 1.0f);
        maxHead = std::max(maxHead, angleOf(pose, HumanoidBone::Head));
        maxHips = std::max(maxHips, static_cast<double>(std::abs(pose.hips.y)));
    }
    CHECK(maxHead > 0.5);
    CHECK(maxHips > 0.001);
}

TEST_CASE(alive.same_inputs_give_identical_output_and_no_state) {
    const ClipPose input = sampleInput();
    ClipPose first = input;
    ClipPose second = input;
    motion::applyAlive(first, 17.25, 0.6f, 9u, 0.8f);
    motion::applyAlive(second, 17.25, 0.6f, 9u, 0.8f);
    CHECK(poseIdentical(first, second));

    // Calls at other times in between must not change the answer: the layer is
    // a function of its arguments, not an integrator.
    ClipPose noise = input;
    motion::applyAlive(noise, 400.0, 0.1f, 9u, 1.0f);
    motion::applyAlive(noise, 3.0, 0.9f, 9u, 1.0f);
    ClipPose third = input;
    motion::applyAlive(third, 17.25, 0.6f, 9u, 0.8f);
    CHECK(poseIdentical(first, third));

    // Different seeds give different personalities.
    ClipPose other = input;
    motion::applyAlive(other, 17.25, 0.6f, 10u, 0.8f);
    CHECK(!poseIdentical(first, other));
}

TEST_CASE(alive.amplitudes_stay_within_their_limits) {
    // Worst case is idle energy with full weight.
    const double hipsYLimit = 0.3 / 160.0 * 1.001;
    const double hipsXLimit = 1.0 / 160.0 * 1.001;
    double maxHeadAngle = 0.0;
    double maxHipsX = 0.0;
    for (const std::uint32_t seed : kSeeds) {
        const ClipPose base = motion::neutralClipPose();
        for (double t = 0.0; t < 300.0; t += 0.05) {
            ClipPose pose = base;
            motion::applyAlive(pose, t, 0.0f, seed, 1.0f);
            const double head = angleOf(pose, HumanoidBone::Head);
            const double neck = angleOf(pose, HumanoidBone::Neck);
            maxHeadAngle = std::max(maxHeadAngle, head);
            CHECK(head <= 5.0);
            CHECK(neck <= 5.0);
            // Breathing: chest +-1.5 deg (upper chest), +-0.75 deg (chest), shoulders +-1 deg.
            CHECK(angleOf(pose, HumanoidBone::UpperChest) <= 1.5 + 1.0e-3);
            CHECK(angleOf(pose, HumanoidBone::Chest) <= 0.75 + 1.0e-3);
            CHECK(angleOf(pose, HumanoidBone::LeftShoulder) <= 1.0 + 1.0e-3);
            CHECK(angleOf(pose, HumanoidBone::RightShoulder) <= 1.0 + 1.0e-3);
            CHECK(std::abs(static_cast<double>(pose.hips.y)) <= hipsYLimit);
            CHECK(std::abs(static_cast<double>(pose.hips.x)) <= hipsXLimit);
            maxHipsX = std::max(maxHipsX, std::abs(static_cast<double>(pose.hips.x)));
        }
    }
    // The look-around is alive, not dead: it reaches a clearly visible angle.
    CHECK(maxHeadAngle > 1.5);
    CHECK(maxHipsX > 0.5 / 160.0);
}

TEST_CASE(alive.neck_takes_forty_percent_of_the_look) {
    // Over a sweep the neck's own look is a fixed share of the total, so the head
    // (60 %) must carry more angle than the neck at the peak of the look.
    const ClipPose base = motion::neutralClipPose();
    double maxNeck = 0.0;
    double maxHead = 0.0;
    for (double t = 0.0; t < 200.0; t += 0.05) {
        ClipPose pose = base;
        motion::applyAlive(pose, t, 0.0f, 5u, 1.0f);
        maxNeck = std::max(maxNeck, angleOf(pose, HumanoidBone::Neck));
        maxHead = std::max(maxHead, angleOf(pose, HumanoidBone::Head));
    }
    CHECK(maxNeck > 0.5);
    const double ratio = maxNeck / maxHead;
    CHECK(ratio > 0.5 && ratio < 0.8);
}

TEST_CASE(alive.energy_scales_the_amplitude_down_when_dancing) {
    CHECK_NEAR(motion::aliveEnergyScale(0.0f), 1.0, 1.0e-6);
    CHECK_NEAR(motion::aliveEnergyScale(std::numeric_limits<float>::quiet_NaN()), 1.0,
               1.0e-6);
    CHECK_NEAR(motion::aliveEnergyScale(0.95f), 0.3, 0.01);
    CHECK_NEAR(motion::aliveEnergyScale(0.62f), 0.3, 0.01);
    CHECK(motion::aliveEnergyScale(0.3f) > motion::aliveEnergyScale(0.6f));

    // Measured on the head: the dancing level is about 30 % of the idle level.
    const ClipPose base = motion::neutralClipPose();
    double idleMax = 0.0;
    double danceMax = 0.0;
    for (double t = 0.0; t < 200.0; t += 0.05) {
        ClipPose idle = base;
        ClipPose dancing = base;
        motion::applyAlive(idle, t, 0.0f, 3u, 1.0f);
        motion::applyAlive(dancing, t, 0.95f, 3u, 1.0f);
        idleMax = std::max(idleMax, angleOf(idle, HumanoidBone::Head));
        danceMax = std::max(danceMax, angleOf(dancing, HumanoidBone::Head));
    }
    CHECK(idleMax > 1.0);
    CHECK(danceMax <= 0.4 * idleMax);
    CHECK(danceMax >= 0.2 * idleMax);
}

TEST_CASE(alive.no_nan_for_huge_or_invalid_times) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double times[] = {1.0e300, -1.0e300, 1.0e18, -7.5, nan, inf, -inf, 1.7e308, 0.0};
    const float energies[] = {std::numeric_limits<float>::quiet_NaN(), -1.0f, 2.0f, 0.5f};
    for (const double t : times) {
        for (const float energy : energies) {
            for (const std::uint32_t seed : kSeeds) {
                ClipPose pose = sampleInput();
                motion::applyAlive(pose, t, energy, seed, 1.0f);
                for (const math::Quat& q : pose.bone) {
                    CHECK(quatFinite(q));
                    CHECK(std::abs(q.lengthSquared() - 1.0f) < 1.0e-4f);
                }
                CHECK(std::isfinite(pose.hips.x) && std::isfinite(pose.hips.y) &&
                      std::isfinite(pose.hips.z));
            }
        }
    }
}

TEST_CASE(alive.idle_crossfade_holds_then_eases_to_idle_and_back) {
    motion::IdleCrossfade fade;
    fade.update(true, 0.0);
    CHECK_NEAR(fade.mix(0.0), 0.0, 1.0e-6);

    // A short gap (below the hold) does not start a blend.
    fade.update(false, 10.0);
    fade.update(false, 10.0 + motion::IdleCrossfade::kHoldSeconds - 0.01);
    CHECK_NEAR(fade.mix(10.2), 0.0, 1.0e-6);
    fade.update(true, 10.3);
    CHECK_NEAR(fade.mix(10.6), 0.0, 1.0e-6);

    // A real pause: hold, then smoothstep to Idle over 0.6 s.
    const double stop = 20.0;
    const double start = stop + motion::IdleCrossfade::kHoldSeconds + 0.001;
    fade.update(false, stop);
    fade.update(false, start);
    CHECK_NEAR(fade.mix(stop + motion::IdleCrossfade::kHoldSeconds), 0.0, 1.0e-6);
    CHECK_NEAR(fade.mix(start + 0.3), 0.5, 1.0e-4);
    CHECK_NEAR(fade.mix(start + 0.6), 1.0, 1.0e-6);
    CHECK_NEAR(fade.mix(start + 5.0), 1.0, 1.0e-6);

    // Monotone while easing to Idle.
    float previous = -1.0f;
    for (double t = start; t <= start + 0.7; t += 0.01) {
        const float value = fade.mix(t);
        CHECK(value >= previous - 1.0e-6f);
        previous = value;
    }

    // Resume mid-transition: the crossfade starts from where it is, no jump.
    fade.update(true, 25.0);
    CHECK_NEAR(fade.mix(25.4), 0.0, 1.0e-6);
    fade.update(false, 30.0);
    fade.update(false, 30.5);
    const double resumeAt = 30.5 + 0.3;
    const float before = fade.mix(resumeAt);
    CHECK_NEAR(before, 0.5, 1.0e-4);
    fade.update(true, resumeAt);
    CHECK_NEAR(fade.mix(resumeAt), before, 1.0e-6);
    CHECK_NEAR(fade.mix(resumeAt + 0.2), before * 0.5, 0.02);
    CHECK_NEAR(fade.mix(resumeAt + 0.4), 0.0, 1.0e-6);
}

TEST_CASE(alive.idle_crossfade_is_driven_by_wall_time_not_update_rate) {
    // The same wall-clock pause at 60 Hz and 240 Hz gives the same blend. The state
    // is only observed once per frame, so the start and the end of the hold can each
    // lag by one 60 Hz frame: that is the tolerance below.
    const double pauseAt = 5.0;
    const double resumeAt = 5.9;
    const double probes[] = {4.9, 5.1, 5.3, 5.5, 5.7, 5.9, 6.0, 6.2};
    const auto sample = [&](double step) {
        motion::IdleCrossfade fade;
        std::vector<float> values;
        double t = 0.0;
        for (const double probe : probes) {
            while (t <= probe + 1.0e-9) {
                fade.update(t < pauseAt || t >= resumeAt, t);
                t += step;
            }
            values.push_back(fade.mix(probe));
        }
        return values;
    };
    const std::vector<float> coarse = sample(1.0 / 60.0);
    const std::vector<float> fine = sample(1.0 / 240.0);
    CHECK(coarse.size() == fine.size());
    for (std::size_t i = 0; i < coarse.size() && i < fine.size(); ++i) {
        CHECK_NEAR(coarse[i], fine[i], 0.1);
    }
    CHECK_NEAR(fine[3], 0.376, 0.02);  // 5.5 s: 0.25 s into the 0.6 s ease
}

// --- body / head weights (desktop: body fades out as the Idle clip takes over) ---

TEST_CASE(alive.default_weights_match_the_explicit_full_weights) {
    const ClipPose input = sampleInput();
    for (const std::uint32_t seed : kSeeds) {
        for (double t = 0.0; t < 30.0; t += 0.37) {
            ClipPose implicit = input;
            ClipPose explicitFull = input;
            motion::applyAlive(implicit, t, 0.6f, seed, 0.8f);
            motion::applyAlive(explicitFull, t, 0.6f, seed, 0.8f, 1.0f, 1.0f);
            CHECK(poseIdentical(implicit, explicitFull));
        }
    }
}

TEST_CASE(alive.body_weight_zero_stops_breathing_and_shift_but_keeps_the_look) {
    const ClipPose base = motion::neutralClipPose();
    double maxHead = 0.0;
    for (const std::uint32_t seed : kSeeds) {
        for (double t = 0.0; t < 60.0; t += 0.05) {
            ClipPose pose = base;
            motion::applyAlive(pose, t, 0.0f, seed, 1.0f, 0.0f, 1.0f);
            // Breathing and weight-shift bones and hips are exactly the input.
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::UpperChest)],
                                base.bone[idx(HumanoidBone::UpperChest)]));
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::Chest)],
                                base.bone[idx(HumanoidBone::Chest)]));
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::LeftShoulder)],
                                base.bone[idx(HumanoidBone::LeftShoulder)]));
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::RightShoulder)],
                                base.bone[idx(HumanoidBone::RightShoulder)]));
            CHECK(vecIdentical(pose.hips, base.hips));
            maxHead = std::max(maxHead, angleOf(pose, HumanoidBone::Head));
        }
    }
    // The head still looks around.
    CHECK(maxHead > 1.5);
}

TEST_CASE(alive.head_weight_zero_stops_the_look_but_keeps_breathing) {
    const ClipPose base = motion::neutralClipPose();
    double maxChest = 0.0;
    for (const std::uint32_t seed : kSeeds) {
        for (double t = 0.0; t < 60.0; t += 0.05) {
            ClipPose pose = base;
            motion::applyAlive(pose, t, 0.0f, seed, 1.0f, 1.0f, 0.0f);
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::Head)],
                                base.bone[idx(HumanoidBone::Head)]));
            CHECK(quatIdentical(pose.bone[idx(HumanoidBone::Neck)],
                                base.bone[idx(HumanoidBone::Neck)]));
            maxChest = std::max(maxChest, angleOf(pose, HumanoidBone::Chest));
        }
    }
    CHECK(maxChest > 0.3);
}

TEST_CASE(alive.body_weight_scales_the_breathing_linearly) {
    // Breathing and weight shift are linear in their amplitude, so half the body
    // weight is exactly half the chest tilt, shoulder lift and hip sway. The head is
    // untouched by the body weight.
    const ClipPose base = motion::neutralClipPose();
    for (double t = 0.0; t < 30.0; t += 0.05) {
        ClipPose full = base;
        ClipPose half = base;
        motion::applyAlive(full, t, 0.0f, 42u, 1.0f);
        motion::applyAlive(half, t, 0.0f, 42u, 1.0f, 0.5f, 1.0f);
        CHECK_NEAR(angleOf(half, HumanoidBone::Chest), 0.5 * angleOf(full, HumanoidBone::Chest),
                   1.0e-3);
        CHECK_NEAR(angleOf(half, HumanoidBone::LeftShoulder),
                   0.5 * angleOf(full, HumanoidBone::LeftShoulder), 1.0e-3);
        CHECK_NEAR(static_cast<double>(half.hips.y), 0.5 * static_cast<double>(full.hips.y),
                   1.0e-6);
        CHECK_NEAR(static_cast<double>(half.hips.x), 0.5 * static_cast<double>(full.hips.x),
                   1.0e-6);
        CHECK(quatIdentical(half.bone[idx(HumanoidBone::Head)], full.bone[idx(HumanoidBone::Head)]));
    }
}

TEST_CASE(alive.non_finite_body_and_head_weights_are_identity) {
    const ClipPose input = sampleInput();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    ClipPose pose = input;
    motion::applyAlive(pose, 3.3, 0.0f, 7u, 1.0f, nan, 1.0f);
    // Body off: breathing identical to the input, look still applied.
    CHECK(vecIdentical(pose.hips, input.hips));
    CHECK(quatIdentical(pose.bone[idx(HumanoidBone::Chest)], input.bone[idx(HumanoidBone::Chest)]));
    pose = input;
    motion::applyAlive(pose, 3.3, 0.0f, 7u, 1.0f, 1.0f, nan);
    CHECK(quatIdentical(pose.bone[idx(HumanoidBone::Head)], input.bone[idx(HumanoidBone::Head)]));
}

// --- dance -> Idle blend with the feet handed over one at a time ---

namespace {

// A dance pose that has travelled: root away from the origin, left foot planted
// forward, right foot in the air (swing) with a small toe roll.
ClipPose travelledDance() {
    ClipPose pose = sampleInput();
    pose.rootX = 0.4f;
    pose.rootZ = -0.3f;
    pose.rootYaw = 0.25f;
    pose.energy = 0.7f;
    pose.foot[0].position = {0.12f, 0.0f, 0.20f};
    pose.foot[0].yaw = 0.10f;
    pose.foot[0].toePitch = 0.40f;
    pose.foot[0].contact = 1.0f;
    pose.foot[0].phase = motion::FootPhase::Stance;
    pose.foot[1].position = {-0.10f, 0.0f, 0.05f};
    pose.foot[1].yaw = -0.05f;
    pose.foot[1].toePitch = -0.20f;
    pose.foot[1].contact = 0.0f;
    pose.foot[1].phase = motion::FootPhase::Swing;
    return pose;
}

// The Idle clip: standing at the origin, both feet planted.
ClipPose idleStance() {
    ClipPose pose = motion::neutralClipPose();
    pose.hips = {0.0f, 0.01f, 0.0f};
    pose.bone[idx(HumanoidBone::Chest)] = math::fromAxisAngle({1.0f, 0.0f, 0.0f}, 0.05f);
    pose.foot[0].position = {0.09f, 0.0f, 0.0f};
    pose.foot[0].contact = 1.0f;
    pose.foot[1].position = {-0.09f, 0.0f, 0.0f};
    pose.foot[1].contact = 1.0f;
    return pose;
}

float footDistanceXZ(const motion::Vec3& a, const motion::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

}  // namespace

TEST_CASE(alive.blend_to_idle_is_the_dance_at_zero_and_idle_feet_at_one) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    CHECK(poseIdentical(motion::blendToIdleKeepingFeet(dance, idle, 0.0f), dance));

    const ClipPose end = motion::blendToIdleKeepingFeet(dance, idle, 1.0f);
    // The dance root survives: the Idle origin does not pull a travelled dancer back.
    CHECK(end.rootX == dance.rootX);
    CHECK(end.rootZ == dance.rootZ);
    CHECK(end.rootYaw == dance.rootYaw);
    // The feet are the Idle feet, and the body is the Idle body.
    for (std::size_t f = 0; f < 2; ++f) {
        CHECK(vecIdentical(end.foot[f].position, idleFeet[f].position));
        CHECK(end.foot[f].contact == idle.foot[f].contact);
        CHECK(end.foot[f].toePitch == idle.foot[f].toePitch);
    }
    for (std::size_t b = 0; b < end.bone.size(); ++b) {
        CHECK(quatIdentical(end.bone[b], idle.bone[b]));
    }
    CHECK(vecIdentical(end.hips, idle.hips));
}

TEST_CASE(alive.blend_to_idle_keeps_one_foot_exactly_planted_at_every_mix) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    for (int i = 0; i <= 200; ++i) {
        const float m = static_cast<float>(i) / 200.0f;
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        bool planted = false;
        for (std::size_t f = 0; f < 2; ++f) {
            planted = planted || vecIdentical(out.foot[f].position, dance.foot[f].position) ||
                      vecIdentical(out.foot[f].position, idleFeet[f].position);
        }
        CHECK(planted);
        // The root never moves off the dance root.
        CHECK(out.rootX == dance.rootX);
        CHECK(out.rootZ == dance.rootZ);
        CHECK(out.rootYaw == dance.rootYaw);
    }
}

TEST_CASE(alive.blend_to_idle_hands_the_feet_over_one_at_a_time) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    // The swinging right foot (1) lands over the first half while the planted left foot
    // (0) holds; the left foot moves over the second half.
    for (int i = 0; i <= 50; ++i) {
        const float m = static_cast<float>(i) / 100.0f;  // 0 .. 0.5
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        CHECK(vecIdentical(out.foot[0].position, dance.foot[0].position));
    }
    for (int i = 50; i <= 100; ++i) {
        const float m = static_cast<float>(i) / 100.0f;  // 0.5 .. 1
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        CHECK(vecIdentical(out.foot[1].position, idleFeet[1].position));
    }
    // Mid-handover the moving foot is strictly between its endpoints.
    const ClipPose quarter = motion::blendToIdleKeepingFeet(dance, idle, 0.25f);
    const motion::Vec3 a = dance.foot[1].position;
    const motion::Vec3 b = idleFeet[1].position;
    const motion::Vec3 q = quarter.foot[1].position;
    CHECK(q.x > std::min(a.x, b.x) && q.x < std::max(a.x, b.x));
    CHECK(q.z > std::min(a.z, b.z) && q.z < std::max(a.z, b.z));
}

TEST_CASE(alive.blend_to_idle_foot_travel_is_monotone) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    for (std::size_t f = 0; f < 2; ++f) {
        float previousFromDance = -1.0f;
        float previousToIdle = 1.0e9f;
        for (int i = 0; i <= 200; ++i) {
            const float m = static_cast<float>(i) / 200.0f;
            const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
            const float fromDance = footDistanceXZ(out.foot[f].position, dance.foot[f].position);
            const float toIdle = footDistanceXZ(out.foot[f].position, idleFeet[f].position);
            CHECK(fromDance >= previousFromDance - 1.0e-6f);
            CHECK(toIdle <= previousToIdle + 1.0e-6f);
            previousFromDance = fromDance;
            previousToIdle = toIdle;
        }
    }
}

TEST_CASE(alive.blend_to_idle_lifts_a_moving_foot_by_the_capped_arch) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    // Planted foot 0 travels about 0.20 height units, so the cap (0.03) applies. It moves
    // second; at mix 0.75 its handover is at its peak (p = 0.5): 0.03 up, no contact.
    const ClipPose peak = motion::blendToIdleKeepingFeet(dance, idle, 0.75f);
    CHECK_NEAR(peak.foot[0].position.y, 0.03, 1.0e-5);
    CHECK_NEAR(peak.foot[0].contact, 0.0, 1.0e-5);

    // Over the whole blend the lift never exceeds the cap, and it is zero at the ends.
    float maxLift = 0.0f;
    for (int i = 0; i <= 500; ++i) {
        const float m = static_cast<float>(i) / 500.0f;
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        for (std::size_t f = 0; f < 2; ++f) {
            // Both endpoints have y = 0, so y itself is the lift.
            maxLift = std::max(maxLift, out.foot[f].position.y);
            CHECK(out.foot[f].contact >= -1.0e-6f);
            CHECK(out.foot[f].contact <= 1.0f + 1.0e-6f);
        }
    }
    CHECK(maxLift <= 0.03f + 1.0e-6f);
    CHECK(maxLift > 0.02f);
}

TEST_CASE(alive.blend_to_idle_does_not_lift_a_foot_that_barely_moves) {
    const ClipPose dance = travelledDance();
    ClipPose idle = idleStance();
    // Same root on both sides, so the Idle feet are used as they are.
    idle.rootX = dance.rootX;
    idle.rootZ = dance.rootZ;
    idle.rootYaw = dance.rootYaw;
    // Right foot: 0.2 cm of travel, under the 1 cm threshold, so no lift.
    idle.foot[1].position = {dance.foot[1].position.x + 0.002f, 0.0f,
                             dance.foot[1].position.z + 0.002f};
    for (int i = 50; i <= 100; ++i) {
        const float m = static_cast<float>(i) / 100.0f;
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        CHECK(std::abs(out.foot[1].position.y) < 1.0e-7f);
    }
}

TEST_CASE(alive.blend_to_idle_crossfades_the_body_smoothly) {
    const ClipPose dance = travelledDance();
    const ClipPose idle = idleStance();
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    const ClipPose half = motion::blendToIdleKeepingFeet(dance, idle, 0.5f);
    CHECK_NEAR(half.hips.x, 0.5 * (dance.hips.x + idle.hips.x), 1.0e-6);
    CHECK_NEAR(half.hips.y, 0.5 * (dance.hips.y + idle.hips.y), 1.0e-6);
    CHECK_NEAR(half.energy, 0.5 * (dance.energy + idle.energy), 1.0e-6);
    // Feet turn with the same handover: at mix 0.5 the landed foot 1 has its Idle yaw and
    // the still planted foot 0 its dance yaw.
    CHECK_NEAR(motion::blendToIdleKeepingFeet(dance, idle, 0.5f).foot[1].yaw, idleFeet[1].yaw,
               1.0e-6);
    CHECK_NEAR(motion::blendToIdleKeepingFeet(dance, idle, 0.5f).foot[0].yaw, dance.foot[0].yaw,
               1.0e-6);
    // Toe roll follows the same p as the position: at the peak it is half way.
    const ClipPose peak = motion::blendToIdleKeepingFeet(dance, idle, 0.75f);
    CHECK_NEAR(peak.foot[0].toePitch, 0.5 * (dance.foot[0].toePitch + idle.foot[0].toePitch),
               1.0e-5);
    // Contact is 0 at the peak of the handover of foot 0 (p = 0.5), and it never
    // exceeds the Idle contact it ends on.
    CHECK_NEAR(peak.foot[0].contact, 0.0, 1.0e-5);
    for (int i = 50; i <= 100; ++i) {
        const float m = static_cast<float>(i) / 100.0f;
        CHECK(motion::blendToIdleKeepingFeet(dance, idle, m).foot[0].contact <= 1.0f + 1.0e-6f);
    }
}

TEST_CASE(alive.idle_feet_follow_the_kept_dance_root) {
    const ClipPose idle = idleStance();
    // Same root: exactly the Idle feet.
    ClipPose sameRoot = travelledDance();
    sameRoot.rootX = idle.rootX;
    sameRoot.rootZ = idle.rootZ;
    sameRoot.rootYaw = idle.rootYaw;
    const auto same = motion::idleFeetAtDanceRoot(sameRoot, idle);
    for (std::size_t f = 0; f < 2; ++f) {
        CHECK(vecIdentical(same[f].position, idle.foot[f].position));
        CHECK(same[f].yaw == idle.foot[f].yaw);
    }

    // A dancer who travelled and turned half way round: the Idle stance is carried along.
    ClipPose dance = travelledDance();
    dance.rootX = 0.24f;
    dance.rootZ = 0.10f;
    dance.rootYaw = 0.5f;
    const auto moved = motion::idleFeetAtDanceRoot(dance, idle);
    // The stance stays centred on the dance root (ankle midpoint at the root) ...
    const double midX = 0.5 * (moved[0].position.x + moved[1].position.x);
    const double midZ = 0.5 * (moved[0].position.z + moved[1].position.z);
    CHECK_NEAR(midX, dance.rootX, 1.0e-5);
    CHECK_NEAR(midZ, dance.rootZ, 1.0e-5);
    // ... with the same width, turned with the body: after half a turn the left ankle
    // (+x side in Idle) is on the -x side of the root.
    const double ankle = 0.06;
    CHECK(ankle + moved[0].position.x - dance.rootX < 0.0);
    CHECK_NEAR((ankle + moved[0].position.x) - (-ankle + moved[1].position.x),
               -((ankle + idle.foot[0].position.x) - (-ankle + idle.foot[1].position.x)), 1.0e-5);
    // Heights and the feet's heading relative to the body are kept.
    for (std::size_t f = 0; f < 2; ++f) {
        CHECK(moved[f].position.y == idle.foot[f].position.y);
        CHECK_NEAR(moved[f].yaw - dance.rootYaw, idle.foot[f].yaw - idle.rootYaw, 1.0e-6);
    }
    // The blend ends on the carried stance, not on Idle's origin.
    const ClipPose end = motion::blendToIdleKeepingFeet(dance, idle, 1.0f);
    for (std::size_t f = 0; f < 2; ++f) {
        CHECK(vecIdentical(end.foot[f].position, moved[f].position));
    }
}

TEST_CASE(alive.blend_to_idle_lands_the_airborne_foot_first) {
    // Paused mid-step: the right foot is in the air, the left foot carries the body. The
    // left foot must not leave the floor until the right foot is down.
    const ClipPose idle = idleStance();
    ClipPose dance = idleStance();
    dance.foot[0].position = {0.10f, 0.0f, 0.05f};
    dance.foot[0].contact = 1.0f;
    dance.foot[1].position = {-0.12f, 0.047f, -0.04f};
    dance.foot[1].contact = 0.0f;
    dance.foot[1].phase = motion::FootPhase::Swing;
    const auto idleFeet = motion::idleFeetAtDanceRoot(dance, idle);
    for (int i = 0; i <= 50; ++i) {
        const float m = static_cast<float>(i) / 100.0f;
        const ClipPose out = motion::blendToIdleKeepingFeet(dance, idle, m);
        CHECK(vecIdentical(out.foot[0].position, dance.foot[0].position));
        CHECK(out.foot[0].contact == 1.0f);
        // The airborne foot only comes down: no extra step arch on top of its height.
        CHECK(out.foot[1].position.y <= dance.foot[1].position.y + 1.0e-6f);
    }
    const ClipPose landed = motion::blendToIdleKeepingFeet(dance, idle, 0.5f);
    CHECK(vecIdentical(landed.foot[1].position, idleFeet[1].position));
}
