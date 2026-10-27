#pragma once

// Foot trajectory authoring used by the procedural choreography generator. Kept
// in its own private header so the loop-closing rules can be unit tested.

#include "bgmedia/motion3/DanceClip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bgmedia::motion::detail {

using math::Vec3;

inline constexpr float kFootDeg = 3.14159265358979323846f / 180.0f;
inline constexpr float kFootHalfCm = 7.0f;

inline float footClamp01(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}
inline float footSmoothstep(float t) {
    t = footClamp01(t);
    return t * t * (3.0f - 2.0f * t);
}
inline float footSmootherstep(float t) {
    t = footClamp01(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ---------------------------------------------------------------------------
// Reference lower-body proxy (centimetres at the generator's 160 cm reference height,
// clip frame: +x = the character's left, +y up, +z forward). It lets the footwork
// planner reject steps in which one leg would pass through the other. It is a
// planning aid in reference units, not the runtime collision test (that one is
// calibrated per model in HumanoidRetargeter).
// ---------------------------------------------------------------------------
struct LegProxy {
    float hipHalf = 8.5f;   // hip joint to the pelvis centre
    float hipY = 88.0f;     // hip height
    float ankleY = 7.0f;    // ankle height above the floor
    float thighR = 7.0f;    // thigh radius at the hip
    float kneeR = 5.4f;
    float ankleR = 4.4f;
    float footLen = 13.0f;  // ankle to toe
    float footR = 3.6f;
};

struct LegPose {
    float x = 0.0f, lift = 0.0f, z = 0.0f;  // Footwork::Sample coordinates (offset from the bind ankle)
};

namespace legproxy_detail {

struct Cap {
    Vec3 a, b;
    float ra, rb;
};

inline float segmentSamples(const Cap& p, const Cap& q, int n, float fromP, float fromQ) {
    float best = 1.0e9f;
    for (int i = 0; i <= n; ++i) {
        const float tp = fromP + (1.0f - fromP) * static_cast<float>(i) / static_cast<float>(n);
        const Vec3 pp = p.a + (p.b - p.a) * tp;
        const float rp = p.ra + (p.rb - p.ra) * tp;
        for (int j = 0; j <= n; ++j) {
            const float tq = fromQ + (1.0f - fromQ) * static_cast<float>(j) / static_cast<float>(n);
            const Vec3 qq = q.a + (q.b - q.a) * tq;
            const float rq = q.ra + (q.rb - q.ra) * tq;
            best = std::min(best, (pp - qq).length() - rp - rq);
        }
    }
    return best;
}

}  // namespace legproxy_detail

// Smallest gap in cm between the two legs (thigh, shin and foot capsules of each side
// against all of the other side; the thighs are compared away from the crotch, where
// they touch by design). Negative = one leg is inside the other.
inline float legClearanceCm(const LegPose& left, const LegPose& right, const LegProxy& body = {}) {
    using legproxy_detail::Cap;
    const auto build = [&](const LegPose& pose, float side, Cap out[3]) {
        const Vec3 hip{side * body.hipHalf, body.hipY, 0.0f};
        const Vec3 ankle{side * kFootHalfCm + pose.x, body.ankleY + pose.lift, pose.z};
        // The knee sits half way and a little forward (the IK bends it forward).
        const Vec3 knee = (hip + ankle) * 0.5f + Vec3{0.0f, 0.0f, 3.0f};
        out[0] = {hip, knee, body.thighR, body.kneeR};
        out[1] = {knee, ankle, body.kneeR, body.ankleR};
        out[2] = {ankle, ankle + Vec3{0.0f, -1.5f, body.footLen}, body.footR, body.footR * 0.8f};
    };
    Cap l[3], r[3];
    build(left, 1.0f, l);
    build(right, -1.0f, r);
    float best = 1.0e9f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            // thigh-thigh only away from the crotch
            const float fromL = (i == 0 && j == 0) ? 0.45f : 0.0f;
            best = std::min(best, legproxy_detail::segmentSamples(l[i], r[j], 6, fromL, fromL));
        }
    }
    return best;
}

class Footwork {
public:
    struct Sample {
        float x = 0.0f;
        float lift = 0.0f;
        float z = 0.0f;
        float yaw = 0.0f;
        float toe = 0.0f;
        const char* kind = "hold";
    };

    Footwork(float period, int side) : period_(period), side_(side) {}

    // The other foot of the same dancer. With it, step() plans against the other leg.
    void setOther(const Footwork* other) { other_ = other; }
    // Minimum clearance (cm) a planned step must keep from the other leg.
    static constexpr float kMinLegGapCm = 1.0f;

    void setStart(float x, float z, float yaw) {
        start_ = pack(x, z, yaw);
        current_ = start_;
    }

    void hold(float until) {
        if (until > time_ + 1.0e-6f) {
            segments_.push_back({"hold", time_, until, current_, current_, 0.0f, 0.0f});
            time_ = until;
        }
    }

    void step(float begin, float end, float x, float z, float yaw,
              float lift = 6.0f, float toe = 0.0f) {
        hold(begin);
        if (!(end > begin + 1.0e-5f)) {
            return;
        }
        float bump = 0.0f;
        planClearance(begin, end, x, z, lift, bump);
        Vec3 next = pack(x, z, yaw);
        segments_.push_back({"arc", begin, end, current_, next, lift, toe, bump});
        current_ = next;
        time_ = end;
    }

    // Planned-step statistics (how many steps had to be adjusted), for tests/diagnostics.
    [[nodiscard]] int adjustedSteps() const { return adjusted_; }

    void pivot(float begin, float end, float yaw) {
        hold(begin);
        if (!(end > begin + 1.0e-5f)) {
            return;
        }
        Vec3 next = pack(current_.x, current_.z, yaw);
        segments_.push_back({"pivot", begin, end, current_, next, 0.0f, 0.0f});
        current_ = next;
        time_ = end;
    }

    void orbit(float begin, float end, Vec3 center, float angle, float lift) {
        hold(begin);
        if (!(end > begin + 1.0e-5f)) {
            return;
        }
        const float half = side_ == 0 ? kFootHalfCm : -kFootHalfCm;
        const float a = angle * kFootDeg;
        const float dx = half + current_.x - center.x;
        const float dz = current_.z - center.z;
        const float nx = center.x + dx * std::cos(a) + dz * std::sin(a);
        const float nz = center.z - dx * std::sin(a) + dz * std::cos(a);
        Vec3 next = pack(nx - half, nz, current_.y + angle);
        segments_.push_back({"orbit", begin, end, current_, next, lift, 0.0f});
        orbitCenter_ = center;
        orbitAngle_ = angle;
        current_ = next;
        time_ = end;
    }

    // Closes the loop: the foot must be back at its start pose at `period_`. If the
    // authored pattern ended elsewhere, a settling arc is scheduled in the time that
    // is still *free* (after the last authored segment), never over a hold: `at()`
    // takes the first matching segment, so an overlapping correction would be
    // shadowed and the loop would jump at the seam.
    void close() {
        const bool mismatch = std::abs(current_.x - start_.x) > 1.0e-4f ||
                              std::abs(current_.z - start_.z) > 1.0e-4f ||
                              std::abs(current_.y - start_.y) > 1.0e-4f;
        if (!mismatch) {
            hold(period_);
            return;
        }
        // Prefer the last beat; start earlier only if the pattern already used it,
        // and never before the end of the last authored segment.
        constexpr float kMinSettle = 0.05f;
        float begin = std::max(time_, period_ - 1.0f);
        begin = std::min(begin, period_ - kMinSettle);
        if (begin < time_ - 1.0e-6f) {
            // The authored pattern ran into the settle window: shorten its tail so
            // the segments stay disjoint (the foot is re-routed from where the
            // truncated segment actually ends).
            truncateAfter(begin);
        }
        hold(begin);
        segments_.push_back({"arc", begin, period_, current_, start_, 6.0f, 0.0f});
        current_ = start_;
        time_ = period_;
    }

    // Test hook / invariant check: segments must not overlap.
    [[nodiscard]] bool segmentsAreDisjoint() const {
        for (std::size_t i = 1; i < segments_.size(); ++i) {
            if (segments_[i].begin < segments_[i - 1].end - 1.0e-5f) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] std::size_t segmentCount() const { return segments_.size(); }

    [[nodiscard]] Sample at(float time) const {
        if (segments_.empty()) {
            return {start_.x, 0.0f, start_.z, start_.y, 0.0f, "hold"};
        }
        time = std::fmod(time, period_);
        if (time < 0.0f) {
            time += period_;
        }
        const Segment* active = &segments_.back();
        for (const Segment& segment : segments_) {
            if (time >= segment.begin - 1.0e-6f && time <= segment.end + 1.0e-6f) {
                active = &segment;
                break;
            }
        }
        const Segment& s = *active;
        if (std::strcmp(s.kind, "hold") == 0) {
            return {s.from.x, 0.0f, s.from.z, s.from.y, 0.0f, "hold"};
        }
        const float u = std::clamp((time - s.begin) / std::max(1.0e-5f, s.end - s.begin),
                                   0.0f, 1.0f);
        if (std::strcmp(s.kind, "pivot") == 0) {
            const float smooth = footSmoothstep(u);
            return {s.from.x, 0.0f, s.from.z,
                    s.from.y + (s.to.y - s.from.y) * smooth, 0.0f, "pivot"};
        }
        const float smooth = footSmootherstep(u);
        if (std::strcmp(s.kind, "orbit") == 0) {
            const float half = side_ == 0 ? kFootHalfCm : -kFootHalfCm;
            const float a = orbitAngle_ * kFootDeg * smooth;
            const float dx = half + s.from.x - orbitCenter_.x;
            const float dz = s.from.z - orbitCenter_.z;
            const float nx = orbitCenter_.x + dx * std::cos(a) + dz * std::sin(a);
            const float nz = orbitCenter_.z - dx * std::sin(a) + dz * std::cos(a);
            return {nx - half, s.lift * std::sin(3.14159265f * u), nz,
                    s.from.y + orbitAngle_ * smooth, 0.0f, "swing"};
        }
        const float lift = s.lift * std::pow(std::max(0.0f, std::sin(3.14159265f * u)), 0.85f);
        const float toe = s.toe * std::sin(3.14159265f * u);
        // `bump` bends the path sideways (forward/back) so a swing foot passes around the
        // standing leg instead of through it; it is zero at both ends.
        return {s.from.x + (s.to.x - s.from.x) * smooth, lift,
                s.from.z + (s.to.z - s.from.z) * smooth + s.bump * std::sin(3.14159265f * u),
                s.from.y + (s.to.y - s.from.y) * smooth, toe, "swing"};
    }

    void contacts(std::vector<ContactSegment>& out) const {
        struct Raw {
            float start = 0.0f;
            float end = 0.0f;
            bool pivot = false;
        };
        std::vector<Raw> merged;
        for (const Segment& segment : segments_) {
            const bool planted = std::strcmp(segment.kind, "hold") == 0 ||
                                 std::strcmp(segment.kind, "pivot") == 0;
            if (!planted) {
                continue;
            }
            const bool pivot = std::strcmp(segment.kind, "pivot") == 0;
            if (!merged.empty() && std::abs(merged.back().end - segment.begin) < 1.0e-5f &&
                merged.back().pivot == pivot) {
                merged.back().end = segment.end;
            } else {
                merged.push_back({segment.begin, segment.end, pivot});
            }
        }
        for (const Raw& raw : merged) {
            ContactSegment segment;
            segment.foot = side_;
            segment.start = raw.start;
            segment.end = std::min(raw.end, period_);
            segment.pivot = raw.pivot;
            if (segment.end > segment.start + 1.0e-4f) {
                out.push_back(segment);
            }
        }
    }

private:
    // Segment endpoints are stored as Vec3{x, yaw, z}: .y is the yaw (degrees) and
    // .z the forward offset, which is what at() and orbit() read. Every writer
    // goes through pack() so the argument order (x, z, yaw) cannot be mixed up
    // with the storage order again.
    static Vec3 pack(float x, float z, float yaw) { return {x, yaw, z}; }

    // Drops/clips segments after `time`, leaving current_ where the clipped
    // segment would be at `time` (linear in its own parameter).
    void truncateAfter(float time) {
        while (!segments_.empty() && segments_.back().begin >= time - 1.0e-6f) {
            current_ = segments_.back().from;
            segments_.pop_back();
        }
        if (!segments_.empty() && segments_.back().end > time) {
            Segment& last = segments_.back();
            const float u = (time - last.begin) / std::max(1.0e-5f, last.end - last.begin);
            const Vec3 at = {last.from.x + (last.to.x - last.from.x) * u,
                             last.from.y + (last.to.y - last.from.y) * u,
                             last.from.z + (last.to.z - last.from.z) * u};
            last.end = time;
            last.to = at;
            current_ = at;
        }
        time_ = time;
    }

    struct Segment {
        const char* kind = "hold";
        float begin = 0.0f;
        float end = 0.0f;
        Vec3 from{};
        Vec3 to{};
        float lift = 0.0f;
        float toe = 0.0f;
        float bump = 0.0f;  // sideways bend of the swing path (cm)
    };

    // Smallest clearance to the other leg over a candidate swing (cm).
    float swingClearance(float begin, float end, float x, float z, float lift, float bump) const {
        if (!other_) {
            return 1.0e9f;
        }
        Segment candidate{"arc", begin, end, current_, pack(x, z, 0.0f), lift, 0.0f, bump};
        float best = 1.0e9f;
        for (int i = 1; i <= 10; ++i) {
            const float u = static_cast<float>(i) / 10.0f;
            const float t = begin + (end - begin) * u;
            const float smooth = footSmootherstep(u);
            const float lf = lift * std::pow(std::max(0.0f, std::sin(3.14159265f * u)), 0.85f);
            LegPose mine{candidate.from.x + (candidate.to.x - candidate.from.x) * smooth, lf,
                         candidate.from.z + (candidate.to.z - candidate.from.z) * smooth +
                             bump * std::sin(3.14159265f * u)};
            const Sample o = other_->at(t);
            const LegPose theirs{o.x, o.lift, o.z};
            best = std::min(best, side_ == 0 ? legClearanceCm(mine, theirs) : legClearanceCm(theirs, mine));
        }
        return best;
    }

    // The authored target (x, z) is a request. When it would drive this leg through the
    // other one, find the smallest change that does not: shorten the lateral reach, shift
    // the landing forward/back, and bend the swing path around the standing leg.
    void planClearance(float begin, float end, float& x, float& z, float lift, float& bump) {
        if (!other_ || swingClearance(begin, end, x, z, lift, 0.0f) >= kMinLegGapCm) {
            return;
        }
        static constexpr float kShrink[] = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f};
        static constexpr float kShift[] = {0.0f, 4.0f, -4.0f, 8.0f, -8.0f, 12.0f, -12.0f, 16.0f, -16.0f, 20.0f, -20.0f, 26.0f, -26.0f, 34.0f, -34.0f};
        static constexpr float kBump[] = {0.0f, 5.0f, -5.0f, 10.0f, -10.0f, 15.0f, -15.0f};
        float bestCost = 1.0e9f, bestClear = -1.0e9f;
        float bx = x, bz = z, bb = 0.0f;
        for (float shrink : kShrink) {
            const float cx = x > 0.0f ? std::max(0.0f, x - shrink) : std::min(0.0f, x + shrink);
            for (float shift : kShift) {
                for (float b : kBump) {
                    const float cz = z + shift;
                    const float clear = swingClearance(begin, end, cx, cz, lift, b);
                    // Prefer the smallest change to the authored step; a shift towards the
                    // back or a bump behind the standing leg costs slightly more than a
                    // forward one (crossing in front is the natural cross-step).
                    const float cost = 1.3f * shrink + std::abs(shift) + 0.8f * std::abs(b) +
                                       (shift < 0.0f ? 0.5f : 0.0f) + (b < 0.0f ? 0.4f : 0.0f);
                    if (clear >= kMinLegGapCm) {
                        if (cost < bestCost) {
                            bestCost = cost;
                            bx = cx;
                            bz = cz;
                            bb = b;
                            bestClear = clear;
                        }
                    } else if (bestCost > 1.0e8f && clear > bestClear) {
                        bestClear = clear;  // remember the least bad option if nothing is feasible
                        bx = cx;
                        bz = cz;
                        bb = b;
                    }
                }
            }
        }
        x = bx;
        z = bz;
        bump = bb;
        ++adjusted_;
    }

    const Footwork* other_ = nullptr;
    int adjusted_ = 0;

    float period_ = 1.0f;
    int side_ = 0;
    Vec3 start_{};
    Vec3 current_{};
    float time_ = 0.0f;
    std::vector<Segment> segments_;
    Vec3 orbitCenter_{};
    float orbitAngle_ = 0.0f;
};

} // namespace bgmedia::motion::detail
