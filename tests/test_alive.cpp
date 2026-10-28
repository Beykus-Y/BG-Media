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
