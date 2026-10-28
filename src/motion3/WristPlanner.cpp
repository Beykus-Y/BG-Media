#include "bgmedia/motion3/WristPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bgmedia::motion {
namespace {

using animation::CollisionProxyInfo;
using math::Vec3;

// Returned for an invalid torso: far enough that nothing is ever in contact.
constexpr float kNoTorsoDistance = 1.0e6f;
// Tolerance for float comparisons at the margin boundary.
constexpr float kEpsilon = 1.0e-5f;
// Candidate detour directions, in a fixed order: the order is the tie-breaker, which
// keeps the planner deterministic. Unnormalised; normalised when used.
constexpr Vec3 kDetourDirections[] = {
    {0.0f, 0.0f, 1.0f},    // front
    {1.0f, 0.0f, 1.0f},    // front, +X side
    {-1.0f, 0.0f, 1.0f},   // front, -X side
    {1.0f, 0.0f, 0.0f},    // +X side
    {-1.0f, 0.0f, 0.0f},   // -X side
    {0.0f, 1.0f, 0.0f},    // up
    {0.0f, -1.0f, 0.0f},   // down
    {0.0f, 1.0f, 1.0f},    // front, up
    {0.0f, -1.0f, 1.0f},   // front, down
    {0.0f, 0.0f, -1.0f},   // back
    {1.0f, 0.0f, -1.0f},   // back, +X side
    {-1.0f, 0.0f, -1.0f},  // back, -X side
};
constexpr int kDetourOffsetSteps = 3;  // waypoint at 1x, 2x, 3x detourOffset

// Smallest clearance along the straight segment a -> b, sampled at `step`.
float segmentClearance(const CollisionProxyInfo& torso, Vec3 a, Vec3 b, float step,
                       float handRadius) {
    const float length = (b - a).length();
    const int count = std::max(1, static_cast<int>(std::ceil(length / step)));
    float minimum = kNoTorsoDistance;
    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        minimum = std::min(minimum,
                           torsoSignedDistance(torso, math::lerp(a, b, t)) - handRadius);
    }
    return minimum;
}

// Every sampled point of a -> b stays within reach of the shoulder, which moves linearly
// from shoulderA to shoulderB along the segment.
bool segmentReachable(Vec3 shoulderA, Vec3 shoulderB, float reach, Vec3 a, Vec3 b,
                      float step) {
    const float length = (b - a).length();
    const int count = std::max(1, static_cast<int>(std::ceil(length / step)));
    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const Vec3 shoulder = math::lerp(shoulderA, shoulderB, t);
        if ((math::lerp(a, b, t) - shoulder).length() > reach + kEpsilon) {
            return false;
        }
    }
    return true;
}

} // namespace

animation::CollisionProxyInfo syntheticTorsoProxy() {
    animation::CollisionProxyInfo torso;
    torso.valid = true;
    torso.fromMesh = false;
    // Waist to shoulders. Wider at the chest, deeper at the sternum.
    const float width[animation::kProxyBands] = {0.070f, 0.085f, 0.090f, 0.090f, 0.080f};
    const float front[animation::kProxyBands] = {0.050f, 0.065f, 0.070f, 0.070f, 0.065f};
    for (int band = 0; band < animation::kProxyBands; ++band) {
        torso.bandWidth[band] = width[band];
        torso.bandFront[band] = front[band];
        torso.bandBack[band] = front[band];
    }
    torso.torsoHalfWidth = 0.090f;
    torso.torsoHalfFront = 0.070f;
    torso.torsoHalfBack = 0.070f;
    torso.shapeExponent = 2.0f;
    return torso;
}

float torsoSignedDistance(const animation::CollisionProxyInfo& torso, Vec3 point) {
    if (!torso.valid) {
        return kNoTorsoDistance;
    }
    // Continuous band coordinate, clamped to the proxy's height range.
    const float span = kTorsoTopY - kTorsoBottomY;
    const float band = std::clamp((point.y - kTorsoBottomY) / span, 0.0f, 1.0f) *
                       static_cast<float>(animation::kProxyBands - 1);
    const int lower = std::min(static_cast<int>(band), animation::kProxyBands - 2);
    const float fraction = band - static_cast<float>(lower);
    const auto interpolate = [lower, fraction](const float* values) {
        return values[lower] + (values[lower + 1] - values[lower]) * fraction;
    };
    const float halfWidth = std::max(interpolate(torso.bandWidth), 1.0e-4f);
    const float halfDepth =
        std::max(point.z >= 0.0f ? interpolate(torso.bandFront) : interpolate(torso.bandBack),
                 1.0e-4f);

    // Vertical excess beyond the torso's height range.
    const float excess =
        std::max({kTorsoBottomY - point.y, point.y - kTorsoTopY, 0.0f});

    const float exponent = torso.shapeExponent > 0.0f ? torso.shapeExponent : 2.0f;
    const float radial =
        std::pow(std::pow(std::abs(point.x) / halfWidth, exponent) +
                     std::pow(std::abs(point.z) / halfDepth, exponent),
                 1.0f / exponent);
    // Scale by the smaller half axis, so the value is a distance estimate, not a ratio.
    const float horizontal = (radial - 1.0f) * std::min(halfWidth, halfDepth);
    if (excess > 0.0f) {
        return std::hypot(std::max(horizontal, 0.0f), excess);
    }
    return horizontal;
}

float polylineClearance(const animation::CollisionProxyInfo& torso,
                        const std::vector<Vec3>& points, float step, float handRadius) {
    if (points.empty()) {
        return kNoTorsoDistance;
    }
    if (points.size() == 1) {
        return torsoSignedDistance(torso, points.front()) - handRadius;
    }
    float minimum = kNoTorsoDistance;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        minimum = std::min(minimum, segmentClearance(torso, points[i], points[i + 1], step,
                                                     handRadius));
    }
    return minimum;
}

WristSegmentPlan planWristSegment(const animation::CollisionProxyInfo& torso, Vec3 shoulderStart,
                                  Vec3 shoulderEnd, Vec3 start, Vec3 end,
                                  const WristPlanOptions& options, int previousDirection) {
    WristSegmentPlan plan;
    const float reach = options.reachLimit;
    const float step = options.sampleStep;
    const float radius = options.handRadius;
    const float required = options.margin - kEpsilon;

    const auto legOk = [&](Vec3 a, Vec3 b, Vec3 shoulderA, Vec3 shoulderB) {
        return segmentClearance(torso, a, b, step, radius) >= required &&
               segmentReachable(shoulderA, shoulderB, reach, a, b, step);
    };

    // A start pose that already penetrates cannot be planned away: report it and keep
    // the straight segment so the conflict is visible to the caller.
    if (torsoSignedDistance(torso, start) - radius < 0.0f) {
        plan.conflict = true;
        plan.points = {start, end};
        plan.minClearance = segmentClearance(torso, start, end, step, radius);
        return plan;
    }

    if (legOk(start, end, shoulderStart, shoulderEnd)) {
        plan.points = {start, end};
        plan.minClearance = segmentClearance(torso, start, end, step, radius);
        return plan;
    }

    // Detour search: one waypoint, candidates in a fixed order, lowest cost wins.
    const Vec3 middle = (start + end) * 0.5f;
    const Vec3 middleShoulder = math::lerp(shoulderStart, shoulderEnd, 0.5f);
    float bestCost = 0.0f;
    bool found = false;
    Vec3 bestWaypoint{};
    int bestDirection = -1;
    const int directionCount = static_cast<int>(sizeof(kDetourDirections) / sizeof(Vec3));
    for (int level = 1; level <= kDetourOffsetSteps; ++level) {
        const float offset = options.detourOffset * static_cast<float>(level);
        for (int d = 0; d < directionCount; ++d) {
            const Vec3 waypoint =
                middle + math::normalize(kDetourDirections[d]) * offset;
            if (!legOk(start, waypoint, shoulderStart, middleShoulder) ||
                !legOk(waypoint, end, middleShoulder, shoulderEnd)) {
                continue;
            }
            const float length = (waypoint - start).length() + (end - waypoint).length();
            float cost = length + options.detourCostPerMeter * offset;
            if (previousDirection >= 0 && d != previousDirection) {
                cost += options.directionHysteresis;
            }
            if (!found || cost < bestCost) {
                found = true;
                bestCost = cost;
                bestWaypoint = waypoint;
                bestDirection = d;
            }
        }
    }
    if (found) {
        plan.points = {start, bestWaypoint, end};
        plan.detoured = true;
        plan.directionIndex = bestDirection;
        plan.minClearance = std::min(segmentClearance(torso, start, bestWaypoint, step, radius),
                                     segmentClearance(torso, bestWaypoint, end, step, radius));
        return plan;
    }

    // No clear detour: stop at the last clear point along the straight segment.
    float lower = 0.0f;  // start is clear (checked above)
    float upper = 1.0f;  // the full segment is not
    for (int iteration = 0; iteration < 16; ++iteration) {
        const float mid = 0.5f * (lower + upper);
        if (legOk(start, math::lerp(start, end, mid), shoulderStart,
                  math::lerp(shoulderStart, shoulderEnd, mid))) {
            lower = mid;
        } else {
            upper = mid;
        }
    }
    const Vec3 stop = math::lerp(start, end, lower);
    plan.attenuated = true;
    plan.points = {start, stop};
    plan.minClearance = segmentClearance(torso, start, stop, step, radius);
    return plan;
}

math::Vec3 WristPath::sample(double beat) const {
    if (keys.empty()) {
        return {};
    }
    if (beat <= keys.front().beat) {
        return keys.front().wrist;
    }
    if (beat >= keys.back().beat) {
        return keys.back().wrist;
    }
    // First key strictly after the beat; the previous key starts the span.
    const auto next = std::upper_bound(
        keys.begin(), keys.end(), beat,
        [](double value, const WristKey& key) { return value < key.beat; });
    const WristKey& after = *next;
    const WristKey& before = *(next - 1);
    const double span = after.beat - before.beat;
    if (span <= 0.0) {
        return after.wrist;
    }
    const float t = static_cast<float>((beat - before.beat) / span);
    return math::lerp(before.wrist, after.wrist, t);
}

WristPath planWristPath(const animation::CollisionProxyInfo& torso,
                        const std::vector<WristKey>& keys, const WristPlanOptions& options,
                        WristPlanReport* report) {
    WristPlanReport local;
    WristPlanReport& stats = report ? *report : local;
    stats = WristPlanReport{};

    WristPath path;
    if (keys.empty()) {
        return path;
    }

    // Reach first: a key outside the sphere is moved onto it, so no planned point is
    // unreachable. Authored clips never leave the sphere, so this is a no-op for them.
    std::vector<WristKey> targets = keys;
    for (WristKey& key : targets) {
        const Vec3 offset = key.wrist - key.shoulder;
        const float distance = offset.length();
        if (distance > options.reachLimit + kEpsilon) {
            key.wrist = key.shoulder + offset * (options.reachLimit / distance);
            ++stats.reachClamped;
        }
    }

    std::vector<Vec3> directPoints;
    directPoints.reserve(targets.size());
    for (const WristKey& key : targets) {
        directPoints.push_back(key.wrist);
    }
    stats.directClearance =
        polylineClearance(torso, directPoints, options.sampleStep, options.handRadius);

    path.keys.push_back(targets.front());
    Vec3 current = targets.front().wrist;
    int previousDirection = -1;
    std::vector<Vec3> plannedPoints = {current};
    for (std::size_t i = 0; i + 1 < targets.size(); ++i) {
        const WristKey& from = targets[i];
        const WristKey& to = targets[i + 1];
        const WristSegmentPlan plan = planWristSegment(torso, from.shoulder, to.shoulder,
                                                       current, to.wrist, options,
                                                       previousDirection);
        ++stats.segments;
        if (plan.conflict) {
            ++stats.conflicts;
        }
        if (plan.attenuated) {
            ++stats.attenuated;
        }
        if (plan.detoured) {
            ++stats.detoured;
            const Vec3 middle = (current + to.wrist) * 0.5f;
            stats.maxDetourOffset =
                std::max(stats.maxDetourOffset, (plan.points[1] - middle).length());
        }
        previousDirection = plan.directionIndex;

        // Waypoint beats: split the segment's beat span by arc length.
        double totalLength = 0.0;
        for (std::size_t p = 0; p + 1 < plan.points.size(); ++p) {
            totalLength += (plan.points[p + 1] - plan.points[p]).length();
        }
        double walked = 0.0;
        for (std::size_t p = 1; p < plan.points.size(); ++p) {
            walked += (plan.points[p] - plan.points[p - 1]).length();
            const bool last = p + 1 == plan.points.size();
            const double beat =
                last ? to.beat
                     : from.beat + (to.beat - from.beat) *
                                       (totalLength > 0.0 ? walked / totalLength : 0.0);
            path.keys.push_back({beat, plan.points[p]});
            plannedPoints.push_back(plan.points[p]);
        }
        current = plan.points.back();
    }

    stats.plannedClearance =
        polylineClearance(torso, plannedPoints, options.sampleStep, options.handRadius);
    return path;
}

} // namespace bgmedia::motion
