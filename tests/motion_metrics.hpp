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
    // atan2 of the relative rotation: acos(|dot|) turns float rounding of two identical
    // rotations into ~0.04 deg, which drowns sub-millidegree checks.
    const math::Quat r = b.normalized() * a.normalized().conjugate();
    const double s = std::sqrt(double(r.x) * r.x + double(r.y) * r.y + double(r.z) * r.z);
    return 2.0 * std::atan2(s, std::abs(double(r.w))) * 180.0 / 3.14159265358979;
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

// Uniform Catmull-Rom segment between p1 and p2, u in [0, 1].
inline math::Vec3 catmullRom(math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3, float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) * 0.5f;
}

// One ankle sample on a media-time axis. `planted` and `targetY` come from the
// contact schedule of the same frame; `ankle` is stage space, in metres.
struct FootSample {
    double t = 0.0;
    math::Vec3 ankle;
    float targetY = 0.0f;   // foot target height, model-height units
    bool planted = false;
};

// Ground drag: horizontal ankle travel while the foot is not planted, its target
// is below `liftLimit` (model-height units) and it moves faster than `speedLimit`
// (m/s). The integral is taken on a fixed reference grid, so the answer does not
// depend on the sampling rate of the caller. The speed gate uses a +-2 grid-step
// central difference (about 8 ms), which keeps the gate from flickering on
// per-frame noise.
struct GroundDrag {
    double distance = 0.0;  // metres
    double seconds = 0.0;   // time spent dragging
};

inline GroundDrag measureGroundDrag(const std::vector<FootSample>& samples, float liftLimit,
                                    double speedLimit, double referenceHz = 240.0) {
    GroundDrag out;
    if (samples.size() < 2) {
        return out;
    }
    const double t0 = samples.front().t;
    const int count = static_cast<int>(std::floor((samples.back().t - t0) * referenceHz + 1e-9)) + 1;
    std::vector<math::Vec3> ankle(static_cast<std::size_t>(count));
    std::vector<float> targetY(static_cast<std::size_t>(count));
    std::vector<char> planted(static_cast<std::size_t>(count));
    std::size_t k = 0;
    for (int i = 0; i < count; ++i) {
        const double t = t0 + i / referenceHz;
        while (k + 1 < samples.size() && samples[k + 1].t <= t) {
            ++k;
        }
        const FootSample& a = samples[k];
        const auto idx = static_cast<std::size_t>(i);
        planted[idx] = a.planted;
        if (k + 1 < samples.size() && samples[k + 1].t > a.t) {
            // Catmull-Rom through the neighbours: a straight line would turn every
            // sample into a constant-speed chord, which biases the speed gate at low rates.
            const FootSample& b = samples[k + 1];
            const float w = static_cast<float>(std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0));
            const math::Vec3& before = samples[k > 0 ? k - 1 : k].ankle;
            const math::Vec3& after = samples[std::min(k + 2, samples.size() - 1)].ankle;
            ankle[idx] = catmullRom(before, a.ankle, b.ankle, after, w);
            targetY[idx] = a.targetY + (b.targetY - a.targetY) * w;
        } else {
            ankle[idx] = a.ankle;
            targetY[idx] = a.targetY;
        }
    }
    const double dt = 1.0 / referenceHz;
    for (int i = 1; i < count; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (planted[idx] || targetY[idx] >= liftLimit) {
            continue;
        }
        const int j0 = std::max(0, i - 2);
        const int j1 = std::min(count - 1, i + 2);
        const math::Vec3 window = ankle[static_cast<std::size_t>(j1)] - ankle[static_cast<std::size_t>(j0)];
        const double speed = std::hypot(window.x, window.z) / ((j1 - j0) * dt);
        if (speed <= speedLimit) {
            continue;
        }
        const math::Vec3 step = ankle[idx] - ankle[idx - 1];
        out.distance += std::hypot(step.x, step.z);
        out.seconds += dt;
    }
    return out;
}

// The per-sample formula the QA reports used before the reference-grid metric.
// It gates each sample on its own chord speed, so its result depends on the
// sampling rate. Kept only to show that difference in the matrix output.
inline GroundDrag measureGroundDragPerSample(const std::vector<FootSample>& samples, float liftLimit,
                                             double speedLimit) {
    GroundDrag out;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const double dt = samples[i].t - samples[i - 1].t;
        if (dt <= 0.0 || samples[i].planted || samples[i].targetY >= liftLimit) {
            continue;
        }
        const math::Vec3 step = samples[i].ankle - samples[i - 1].ankle;
        const double horizontal = std::hypot(step.x, step.z);
        if (horizontal / dt > speedLimit) {
            out.distance += horizontal;
            out.seconds += dt;
        }
    }
    return out;
}

} // namespace bgmedia::qa
