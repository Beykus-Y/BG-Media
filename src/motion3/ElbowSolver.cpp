#include "bgmedia/motion3/ElbowSolver.hpp"

#include "bgmedia/motion3/WristPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace bgmedia::motion {
namespace {

using animation::CollisionProxyInfo;
using math::Vec3;

constexpr float kEpsilon = 1.0e-5f;
// Below this fraction of the upper arm the swivel circle is treated as a point.
constexpr float kDegenerateRadius = 1.0e-3f;
// Below this length the pole has no usable component across the S-W axis.
constexpr float kCollinearPole = 1.0e-4f;

// Two-bone locus for one sample: the swivel circle around the shoulder-wrist axis.
struct Locus {
    Vec3 axis{};        // unit shoulder -> wrist
    Vec3 wrist{};       // wrist after clamping to the reach sphere
    float along = 0.0f; // distance from the shoulder to the circle centre along axis
    float radius = 0.0f;
    Vec3 e1{};          // circle basis; e1 points across the axis towards the pole
    Vec3 e2{};
    bool degenerate = false;
    bool clamped = false;
};

Vec3 safeNormalize(Vec3 v, Vec3 fallback) {
    const float length = v.length();
    return length > 1.0e-8f ? v / length : fallback;
}

Locus makeLocus(Vec3 shoulder, Vec3 wrist, Vec3 pole, float upper, float fore) {
    Locus locus;
    const Vec3 offset = wrist - shoulder;
    float distance = offset.length();
    if (distance < 1.0e-6f) {
        locus.axis = {0.0f, -1.0f, 0.0f};
        distance = std::abs(upper - fore);
        locus.degenerate = true;
    } else {
        locus.axis = offset / distance;
    }
    const float reachMax = upper + fore;
    const float reachMin = std::abs(upper - fore);
    if (distance > reachMax + kEpsilon) {
        distance = reachMax;
        locus.clamped = true;
    }
    if (distance < reachMin - kEpsilon) {
        distance = reachMin;
        locus.degenerate = true;
    }
    locus.wrist = shoulder + locus.axis * distance;
    if (distance > 1.0e-6f) {
        locus.along = (distance * distance + upper * upper - fore * fore) / (2.0f * distance);
    }
    const float radiusSquared = upper * upper - locus.along * locus.along;
    locus.radius = radiusSquared > 0.0f ? std::sqrt(radiusSquared) : 0.0f;
    if (locus.radius < kDegenerateRadius * upper) {
        locus.degenerate = true;
    }

    // The pole projected across the axis sets swivel 0. A collinear pole has no
    // preferred direction, so a helper axis is used and the sample is flagged.
    Vec3 across = pole - locus.axis * math::dot(pole, locus.axis);
    if (across.length() < kCollinearPole) {
        locus.degenerate = true;
        const Vec3 helper = std::abs(locus.axis.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f}
                                                          : Vec3{0.0f, 1.0f, 0.0f};
        across = helper - locus.axis * math::dot(helper, locus.axis);
    }
    locus.e1 = math::normalize(across);
    locus.e2 = math::cross(locus.axis, locus.e1);
    return locus;
}

// Elbow offset (elbow - shoulder) for a swivel angle on a locus. Length = upper arm.
Vec3 offsetFor(const Locus& locus, float swivel) {
    return locus.axis * locus.along +
           (locus.e1 * std::cos(swivel) + locus.e2 * std::sin(swivel)) * locus.radius;
}

// Smallest clearance of the upper arm and forearm from the torso proxy, minus the arm
// radius. The shoulder joint itself is excluded: it sits on the torso surface by design.
float armClearance(const CollisionProxyInfo& torso, Vec3 shoulder, Vec3 elbow, Vec3 wrist,
                   const ElbowSolveOptions& options) {
    float minimum = std::numeric_limits<float>::max();
    const int count = std::max(1, options.armSamples);
    for (int i = 1; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        minimum = std::min(minimum, torsoSignedDistance(torso, math::lerp(shoulder, elbow, t)) -
                                        options.armRadius);
        minimum = std::min(minimum, torsoSignedDistance(torso, math::lerp(elbow, wrist, t)) -
                                        options.armRadius);
    }
    return minimum;
}

float angleBetween(Vec3 a, Vec3 b) {
    const Vec3 na = safeNormalize(a, a);
    const Vec3 nb = safeNormalize(b, b);
    return std::acos(std::clamp(math::dot(na, nb), -1.0f, 1.0f));
}

} // namespace

math::Vec3 elbowPosition(Vec3 shoulder, Vec3 wrist, float swivel, Vec3 pole, float upperArm,
                         float forearm, bool* degenerate) {
    const Locus locus = makeLocus(shoulder, wrist, pole, upperArm, forearm);
    if (degenerate) {
        *degenerate = locus.degenerate;
    }
    return shoulder + offsetFor(locus, swivel);
}

math::Vec3 ElbowSwivelCurve::sample(double beat) const {
    if (elbowOffset.empty()) {
        return {};
    }
    if (beat <= beats.front()) {
        return elbowOffset.front();
    }
    if (beat >= beats.back()) {
        return elbowOffset.back();
    }
    const auto next = std::upper_bound(beats.begin(), beats.end(), beat);
    const std::size_t index = static_cast<std::size_t>(next - beats.begin());
    const Vec3& before = elbowOffset[index - 1];
    const Vec3& after = elbowOffset[index];
    const double span = beats[index] - beats[index - 1];
    if (span <= 0.0) {
        return after;
    }
    // Interpolate directions and keep the bone length: the arm never stretches.
    const float t = static_cast<float>((beat - beats[index - 1]) / span);
    const float length = before.length();
    const Vec3 direction =
        math::lerp(safeNormalize(before, before), safeNormalize(after, after), t);
    return safeNormalize(direction, before) * length;
}

ElbowSwivelCurve solveElbowSwivel(const CollisionProxyInfo& torso,
                                  const std::vector<ElbowSample>& samples,
                                  const ElbowSolveOptions& options, ElbowSolveReport* report) {
    ElbowSolveReport local;
    ElbowSolveReport& stats = report ? *report : local;
    stats = ElbowSolveReport{};

    ElbowSwivelCurve curve;
    const std::size_t count = samples.size();
    const std::size_t candidates = static_cast<std::size_t>(std::max(1, options.candidates));
    stats.samples = static_cast<int>(count);
    if (count == 0) {
        return curve;
    }
    const auto index = [candidates](std::size_t k, std::size_t i) {
        return k * candidates + i;
    };

    // Candidate elbow offsets, clearance and per-sample cost. Candidate i is the swivel
    // angle 2 pi i / N, so candidate 0 is the pole-only elbow.
    std::vector<Vec3> offsets(count * candidates);
    std::vector<float> clearance(count * candidates);
    std::vector<float> frame(count * candidates);
    for (std::size_t k = 0; k < count; ++k) {
        const ElbowSample& sample = samples[k];
        const Locus locus = makeLocus(sample.shoulder, sample.wrist, sample.pole,
                                      options.upperArm, options.forearm);
        if (locus.degenerate) {
            ++stats.degenerateSamples;
        }
        if (locus.clamped) {
            ++stats.clampedSamples;
        }
        for (std::size_t i = 0; i < candidates; ++i) {
            const float swivel = 2.0f * math::kPi * static_cast<float>(i) /
                                 static_cast<float>(candidates);
            const Vec3 offset = offsetFor(locus, swivel);
            const float gap = armClearance(torso, sample.shoulder, sample.shoulder + offset,
                                           locus.wrist, options);
            offsets[index(k, i)] = offset;
            clearance[index(k, i)] = gap;
            frame[index(k, i)] = options.poleWeight * (1.0f - std::cos(swivel)) +
                                 options.clearanceWeight * std::max(0.0f, options.margin - gap);
        }
    }

    // Viterbi over the candidates: minimise frame cost plus the change between
    // neighbouring samples. The result depends only on the samples, never on render
    // order, frame rate or seek.
    const float rightAngle = 0.5f * math::kPi;
    std::vector<float> cost(candidates);
    std::vector<float> nextCost(candidates);
    std::vector<std::size_t> back(count * candidates, 0);
    for (std::size_t i = 0; i < candidates; ++i) {
        cost[i] = frame[index(0, i)];
    }
    for (std::size_t k = 1; k < count; ++k) {
        for (std::size_t i = 0; i < candidates; ++i) {
            float best = std::numeric_limits<float>::max();
            std::size_t from = 0;
            for (std::size_t j = 0; j < candidates; ++j) {
                const float angle = angleBetween(offsets[index(k - 1, j)], offsets[index(k, i)]);
                const float transition = options.smoothWeight * angle +
                                         (angle > rightAngle ? options.flipWeight : 0.0f);
                const float total = cost[j] + transition;
                if (total < best) {
                    best = total;
                    from = j;
                }
            }
            nextCost[i] = best + frame[index(k, i)];
            back[index(k, i)] = from;
        }
        cost.swap(nextCost);
    }
    std::vector<std::size_t> chosen(count, 0);
    for (std::size_t i = 1; i < candidates; ++i) {
        if (cost[i] < cost[chosen[count - 1]]) {
            chosen[count - 1] = i;
        }
    }
    for (std::size_t k = count - 1; k > 0; --k) {
        chosen[k - 1] = back[index(k, chosen[k])];
    }

    // Report and curve.
    stats.minClearanceBefore = std::numeric_limits<float>::max();
    stats.minClearanceAfter = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < count; ++k) {
        stats.minClearanceBefore = std::min(stats.minClearanceBefore, clearance[index(k, 0)]);
        stats.minClearanceAfter =
            std::min(stats.minClearanceAfter, clearance[index(k, chosen[k])]);
    }
    curve.beats.reserve(count);
    curve.elbowOffset.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        curve.beats.push_back(samples[k].beat);
        curve.elbowOffset.push_back(offsets[index(k, chosen[k])]);
        if (k == 0) {
            continue;
        }
        const float angle = angleBetween(curve.elbowOffset[k - 1], curve.elbowOffset[k]);
        stats.maxSwivelStep = std::max(stats.maxSwivelStep, angle);
        if (angle > rightAngle) {
            ++stats.flips;
        }
        const double dt = samples[k].beat - samples[k - 1].beat;
        if (dt > 0.0) {
            stats.maxAngularSpeed =
                std::max(stats.maxAngularSpeed, static_cast<float>(angle / dt));
        }
    }
    return curve;
}

} // namespace bgmedia::motion
