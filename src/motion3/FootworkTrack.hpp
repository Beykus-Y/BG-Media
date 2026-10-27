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
        Vec3 next = pack(x, z, yaw);
        segments_.push_back({"arc", begin, end, current_, next, lift, toe});
        current_ = next;
        time_ = end;
    }

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
        return {s.from.x + (s.to.x - s.from.x) * smooth, lift,
                s.from.z + (s.to.z - s.from.z) * smooth,
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
    };

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
