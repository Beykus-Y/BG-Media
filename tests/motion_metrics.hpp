#pragma once

// Measurement helpers shared by the BG Motion 3.0 unit tests and the
// bgmedia_motion_qa command line tool. They never judge; they measure, and the
// callers decide what is acceptable.

#include "bgmedia/animation/HumanoidRetargeter.hpp"
#include "bgmedia/motion3/DanceDirector.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bgmedia::qa {

using motion::ClipPose;
using motion::HumanoidClip;
using vrm::HumanoidBone;

inline double angleBetween(const math::Quat& a, const math::Quat& b) {
    const float d = std::abs(math::dot(a.normalized(), b.normalized()));
    return 2.0 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 180.0 / 3.14159265358979;
}

// Angular velocity vector (deg/s) taking a to b over dt, world axes.
inline math::Vec3 angularVelocity(const math::Quat& a, const math::Quat& b, double dt) {
    math::Quat delta = (b * a.conjugate()).normalized();
    if (delta.w < 0.0f) {
        delta = {-delta.x, -delta.y, -delta.z, -delta.w};
    }
    const float s = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    const float angle = 2.0f * std::atan2(s, delta.w);
    if (s < 1e-9f) {
        return {};
    }
    const float scale = static_cast<float>(angle * 180.0 / 3.14159265358979 / dt) / s;
    return {delta.x * scale, delta.y * scale, delta.z * scale};
}

struct ClipReport {
    std::string id;
    double loopRotationDeg = 0.0;       // pose difference first vs last key
    double loopPositionHeight = 0.0;
    double loopVelocityJumpDegPerSec = 0.0;  // angular speed discontinuity at the seam
    double maxAngularSpeed = 0.0;       // deg/s, any bone, at the evaluation BPM
    HumanoidBone fastestBone = HumanoidBone::Hips;
    double maxAngularAcceleration = 0.0;  // deg/s^2
    double maxQuatNormError = 0.0;
    double maxHipsSpeed = 0.0;          // height units / s
    double maxFootSpeed = 0.0;          // height units / s
    double maxRootExcursion = 0.0;      // height units from the origin
    double maxRootYawDeg = 0.0;
    double slideMax = 0.0;              // metres a planted ankle moved (retargeted)
    double slideBeat = 0.0;
    int slideFoot = 0;
    double slideHeightFraction = 0.0;
    int stanceRuns = 0;
    double footErrorMax = 0.0;          // metres, IK reach deficit
    double swingDistanceMax = 0.0;      // height units: longest foot swing
    int swings = 0;                     // foot swings longer than 6% of height
    double footPathLength = 0.0;        // sum of both feet, height units
    double hipsDropMax = 0.0;           // metres of automatic reach drop
};

inline ClipReport analyseClip(const HumanoidClip& clip, const vrm::VrmModel* model,
                              const animation::HumanoidRetargeter* rig, double bpm,
                              double sampleHz = 240.0) {
    ClipReport report;
    report.id = clip.id;
    const double seconds = clip.beats * 60.0 / bpm;
    const double dt = 1.0 / sampleHz;
    const int count = static_cast<int>(std::floor(seconds * sampleHz));
    const double beatsPerSecond = bpm / 60.0;

    {
        const ClipPose first = motion::sampleClip(clip, 0.0);
        const ClipPose last = motion::sampleClip(clip, clip.beats - 1e-6);
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            report.loopRotationDeg =
                std::max<double>(report.loopRotationDeg, angleBetween(first.bone[b], last.bone[b]));
        }
        const auto dist = [](math::Vec3 a, math::Vec3 b) { return (a - b).length(); };
        report.loopPositionHeight = std::max<double>(
            {dist(first.hips, last.hips),
             std::hypot(first.rootX - last.rootX, first.rootZ - last.rootZ),
             dist(first.foot[0].position, last.foot[0].position),
             dist(first.foot[1].position, last.foot[1].position)});
        // Velocity continuity across the seam.
        const double eps = 1.0 / 480.0 * beatsPerSecond;
        const ClipPose a0 = motion::sampleClip(clip, clip.beats - 2 * eps);
        const ClipPose a1 = motion::sampleClip(clip, clip.beats - eps);
        const ClipPose b0 = motion::sampleClip(clip, eps);
        const ClipPose b1 = motion::sampleClip(clip, 2 * eps);
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const math::Vec3 before = angularVelocity(a0.bone[b], a1.bone[b], 1.0 / 480.0);
            const math::Vec3 after = angularVelocity(b0.bone[b], b1.bone[b], 1.0 / 480.0);
            report.loopVelocityJumpDegPerSec =
                std::max<double>(report.loopVelocityJumpDegPerSec, (after - before).length());
        }
    }

    std::vector<ClipPose> poses;
    poses.reserve(static_cast<std::size_t>(count) + 1);
    for (int i = 0; i <= count; ++i) {
        poses.push_back(motion::sampleClip(clip, i * dt * beatsPerSecond));
    }
    std::vector<std::vector<math::Vec3>> velocity(
        vrm::kHumanoidBoneCount, std::vector<math::Vec3>(poses.size()));
    for (std::size_t i = 0; i < poses.size(); ++i) {
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const float n = std::sqrt(poses[i].bone[b].lengthSquared());
            report.maxQuatNormError = std::max<double>(report.maxQuatNormError, std::abs(n - 1.0));
        }
        report.maxRootExcursion = std::max<double>(
            report.maxRootExcursion, std::hypot(poses[i].rootX, poses[i].rootZ));
        report.maxRootYawDeg = std::max<double>(report.maxRootYawDeg, std::abs(poses[i].rootYaw) * 360.0);
        if (i == 0) {
            continue;
        }
        const ClipPose& p = poses[i - 1];
        const ClipPose& q = poses[i];
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            velocity[b][i] = angularVelocity(p.bone[b], q.bone[b], dt);
            const double speed = velocity[b][i].length();
            if (speed > report.maxAngularSpeed) {
                report.maxAngularSpeed = speed;
                report.fastestBone = static_cast<HumanoidBone>(b);
            }
            if (i > 1) {
                report.maxAngularAcceleration = std::max(
                    report.maxAngularAcceleration, (velocity[b][i] - velocity[b][i - 1]).length() / dt);
            }
        }
        report.maxHipsSpeed = std::max(report.maxHipsSpeed, (q.hips - p.hips).length() / dt);
        for (int f = 0; f < 2; ++f) {
            report.maxFootSpeed = std::max(
                report.maxFootSpeed,
                (q.foot[static_cast<std::size_t>(f)].position - p.foot[static_cast<std::size_t>(f)].position).length() / dt);
        }
    }

    // Foot swings (stage space, authored data).
    for (int f = 0; f < 2; ++f) {
        double path = 0.0;
        double current = 0.0;
        bool inSwing = false;
        for (std::size_t i = 1; i < poses.size(); ++i) {
            const auto& a = poses[i - 1].foot[static_cast<std::size_t>(f)];
            const auto& b = poses[i].foot[static_cast<std::size_t>(f)];
            const double step = std::hypot(b.position.x - a.position.x, b.position.z - a.position.z);
            path += step;
            const bool swinging = b.phase == motion::FootPhase::Swing;
            if (swinging) {
                current += step;
                inSwing = true;
            } else if (inSwing) {
                report.swingDistanceMax = std::max(report.swingDistanceMax, current);
                if (current > 0.06) {
                    ++report.swings;
                }
                current = 0.0;
                inSwing = false;
            }
        }
        report.footPathLength += path;
    }

    // Retargeted measurements.
    if (model && rig && rig->valid()) {
        const float height = model->height;
        for (int f = 0; f < 2; ++f) {
            const int node = model->nodeFor(f == 0 ? HumanoidBone::LeftFoot : HumanoidBone::RightFoot);
            bool inRun = false;
            math::Vec3 anchor{};
            for (std::size_t i = 0; i < poses.size(); ++i) {
                const auto& foot = poses[i].foot[static_cast<std::size_t>(f)];
                const bool planted =
                    (foot.phase == motion::FootPhase::Stance || foot.phase == motion::FootPhase::Pivot) &&
                    foot.contact > 0.999f;
                const animation::RetargetedPose r = rig->applyClip(poses[i], i * dt);
                report.footErrorMax = std::max<double>(report.footErrorMax, r.stats.maxFootError);
                report.hipsDropMax = std::max<double>(report.hipsDropMax, r.stats.reachHipDrop);
                if (!planted) {
                    inRun = false;
                    continue;
                }
                const math::Vec3 p = animation::HumanoidRetargeter::stagePosition(r, node);
                if (!inRun) {
                    inRun = true;
                    anchor = p;
                    ++report.stanceRuns;
                } else {
                    const double moved = (p - anchor).length();
                    if (moved > report.slideMax) {
                        report.slideMax = moved;
                        report.slideBeat = i * dt * beatsPerSecond;
                        report.slideFoot = f;
                    }
                }
            }
        }
        report.slideHeightFraction = report.slideMax / std::max(0.1f, height);
    }
    return report;
}

// Largest frame-to-frame change across a window of a director's output.
struct JumpReport {
    double maxAngularSpeed = 0.0;   // deg/s
    double maxAngularAcceleration = 0.0;
    double maxHipsStep = 0.0;       // height units per frame
    double maxRootStep = 0.0;
    double maxFootStep = 0.0;
    int frames = 0;
    bool finite = true;
};

inline JumpReport measureWindow(const motion::DanceDirector& director, double t0, double t1,
                                double hz) {
    JumpReport report;
    const double dt = 1.0 / hz;
    ClipPose previous = director.sample(t0).pose;
    std::vector<math::Vec3> lastVelocity(vrm::kHumanoidBoneCount);
    bool haveVelocity = false;
    for (double t = t0 + dt; t <= t1; t += dt) {
        const ClipPose current = director.sample(t).pose;
        ++report.frames;
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const math::Quat q = current.bone[b];
            if (!std::isfinite(q.x + q.y + q.z + q.w)) {
                report.finite = false;
            }
            const math::Vec3 v = angularVelocity(previous.bone[b], current.bone[b], dt);
            report.maxAngularSpeed = std::max<double>(report.maxAngularSpeed, v.length());
            if (haveVelocity) {
                report.maxAngularAcceleration = std::max<double>(
                    report.maxAngularAcceleration, (v - lastVelocity[b]).length() / dt);
            }
            lastVelocity[b] = v;
        }
        haveVelocity = true;
        report.maxHipsStep = std::max<double>(report.maxHipsStep, (current.hips - previous.hips).length());
        report.maxRootStep = std::max<double>(
            report.maxRootStep, std::hypot(current.rootX - previous.rootX, current.rootZ - previous.rootZ));
        for (int f = 0; f < 2; ++f) {
            report.maxFootStep = std::max<double>(
                report.maxFootStep,
                (current.foot[static_cast<std::size_t>(f)].position -
                 previous.foot[static_cast<std::size_t>(f)].position).length());
        }
        previous = current;
    }
    return report;
}

} // namespace bgmedia::qa
