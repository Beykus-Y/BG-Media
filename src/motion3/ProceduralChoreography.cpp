#include "bgmedia/motion3/ProceduralChoreography.hpp"

#include "FootworkTrack.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia::motion {
namespace {

using Clock = std::chrono::steady_clock;
using math::Quat;
using math::Vec3;
using vrm::HumanoidBone;

constexpr float kDeg = 3.14159265358979323846f / 180.0f;
constexpr float kRefHeightCm = 160.0f;
constexpr float kFootHalfCm = 7.0f;
constexpr float kTiny = 1.0e-6f;

float clamp01(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

float smootherstep(float t) {
    t = clamp01(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

Quat quatAxis(Vec3 axis, float degrees) {
    return math::fromAxisAngle(axis, degrees * kDeg);
}

Quat qmul(Quat a, Quat b) { return (a * b).normalized(); }

Quat qchain(std::initializer_list<Quat> quats) {
    Quat out = Quat::identity();
    for (Quat q : quats) {
        out = qmul(out, q);
    }
    return out;
}

Quat qFromTo(Vec3 from, Vec3 to) {
    return math::fromTo(from, to);
}

Quat qNorm(Quat q) { return q.normalized(); }

// Wrist roll (degrees) as a continuous function of the arm plane.
// magnitude: 9 deg for a low arm, 22 deg for a raised arm, ramped through the
// 62..96 deg elevation band; direction: signed azimuth, zero at 45 deg.
// Continuous in both arguments, so neighbouring keys cannot step the hand.
float wristRollDeg(float elevDeg, float azimDeg) {
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    const float magnitude = 9.0f + 13.0f * smooth((elevDeg - 62.0f) / 34.0f);
    const float direction = std::tanh((azimDeg - 45.0f) / 7.0f);
    return magnitude * direction;
}

std::uint32_t hashSeed(std::uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return seed ? seed : 1u;
}

struct Rng {
    std::uint32_t state = 1u;
    explicit Rng(std::uint32_t seed) : state(hashSeed(seed)) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    float unit() { return static_cast<float>(next() >> 8) / 16777215.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    int pick(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    }
    bool chance(float p) { return unit() < p; }
};

// --- shape-preserving periodic scalar curve ---------------------------------

class Curve {
public:
    struct Key {
        float time = 0.0f;
        float value = 0.0f;
        bool hit = false;
        float tension = 1.0f;
    };

    Curve() = default;
    Curve(float period, float defaultValue)
        : period_(std::max(0.001f, period)), defaultValue_(defaultValue) {}

    void key(float time, float value, bool hit = false, float tension = 1.0f) {
        if (!std::isfinite(time) || !std::isfinite(value)) {
            return;
        }
        time = std::clamp(time, 0.0f, period_);
        if (time >= period_ - 1.0e-6f) {
            time = 0.0f;
        }
        for (Key& existing : keys_) {
            if (std::abs(existing.time - time) < 1.0e-6f) {
                existing = {time, value, hit, tension};
                built_ = false;
                return;
            }
        }
        keys_.push_back({time, value, hit, tension});
        built_ = false;
    }

    [[nodiscard]] float at(float time) const {
        if (keys_.empty()) {
            return defaultValue_;
        }
        if (keys_.size() == 1) {
            return keys_.front().value;
        }
        build();
        time = std::fmod(time, period_);
        if (time < 0.0f) {
            time += period_;
        }
        auto it = std::lower_bound(times_.begin(), times_.end(), time);
        std::size_t hi = static_cast<std::size_t>(it - times_.begin());
        if (hi == 0) {
            hi = 1;
        }
        if (hi >= times_.size()) {
            hi = times_.size() - 1;
        }
        const std::size_t lo = hi - 1;
        const float h = times_[hi] - times_[lo];
        if (!(h > kTiny)) {
            return values_[lo];
        }
        const float u = (time - times_[lo]) / h;
        const float u2 = u * u;
        const float u3 = u2 * u;
        return (2.0f * u3 - 3.0f * u2 + 1.0f) * values_[lo] +
               (u3 - 2.0f * u2 + u) * h * slopes_[lo] +
               (-2.0f * u3 + 3.0f * u2) * values_[hi] +
               (u3 - u2) * h * slopes_[hi];
    }

private:
    void build() const {
        if (built_) {
            return;
        }
        std::vector<Key> sorted = keys_;
        std::sort(sorted.begin(), sorted.end(), [](const Key& a, const Key& b) {
            return a.time < b.time;
        });
        std::vector<Key> extended;
        extended.reserve(sorted.size() * 3);
        for (int repeat = -1; repeat <= 1; ++repeat) {
            for (const Key& key : sorted) {
                Key copy = key;
                copy.time += static_cast<float>(repeat) * period_;
                extended.push_back(copy);
            }
        }
        const std::size_t count = extended.size();
        std::vector<float> h(count > 0 ? count - 1 : 0);
        std::vector<float> d(count > 0 ? count - 1 : 0);
        std::vector<float> m(count, 0.0f);
        for (std::size_t i = 0; i + 1 < count; ++i) {
            h[i] = std::max(1.0e-5f, extended[i + 1].time - extended[i].time);
            d[i] = (extended[i + 1].value - extended[i].value) / h[i];
        }
        for (std::size_t i = 1; i + 1 < count; ++i) {
            if (extended[i].hit) {
                m[i] = 0.0f;
                continue;
            }
            if (d[i - 1] * d[i] <= 0.0f) {
                m[i] = 0.0f;
            } else {
                const float w1 = 2.0f * h[i] + h[i - 1];
                const float w2 = h[i] + 2.0f * h[i - 1];
                m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
                m[i] *= extended[i].tension;
            }
        }
        if (count >= 2) {
            m.front() = d.front();
            m.back() = d.back();
        }
        times_.resize(count);
        values_.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            times_[i] = extended[i].time;
            values_[i] = extended[i].value;
        }
        slopes_ = std::move(m);
        built_ = true;
    }

    float period_ = 1.0f;
    float defaultValue_ = 0.0f;
    std::vector<Key> keys_;
    mutable std::vector<float> times_;
    mutable std::vector<float> values_;
    mutable std::vector<float> slopes_;
    mutable bool built_ = false;
};

// --- footwork ---------------------------------------------------------------

// (see FootworkTrack.hpp)
using detail::Footwork;

// --- body controls ----------------------------------------------------------

enum class Ctrl : std::uint8_t {
    HipsX, HipsY, HipsZ,
    PelvisYaw, PelvisRoll, PelvisPitch,
    SpineBend, SpineSide, SpineTwist,
    WaveLower, WaveMid, WaveUpper,
    HeadNod, HeadTilt, HeadTurn,
    ShLLift, ShLFwd, ShRLift, ShRFwd,
    RootX, RootZ, RootYaw,
    HandL, HandR,
    ArmLElev, ArmLAzim, ArmLTwist, ArmLElbow, ArmLFtwist, ArmLWrist,
    ArmRElev, ArmRAzim, ArmRTwist, ArmRElbow, ArmRFtwist, ArmRWrist,
    Count
};

constexpr int kCtrlCount = static_cast<int>(Ctrl::Count);

float defaultControl(Ctrl control) {
    switch (control) {
    case Ctrl::ArmLElev:
    case Ctrl::ArmRElev: return 12.0f;
    case Ctrl::ArmLAzim:
    case Ctrl::ArmRAzim: return 42.0f;
    case Ctrl::ArmLTwist:
    case Ctrl::ArmRTwist: return 12.0f;
    case Ctrl::ArmLElbow:
    case Ctrl::ArmRElbow: return 20.0f;
    case Ctrl::HandL:
    case Ctrl::HandR: return 0.30f;
    default: return 0.0f;
    }
}

struct Controls {
    std::array<float, kCtrlCount> value{};
    explicit Controls() {
        for (int i = 0; i < kCtrlCount; ++i) {
            value[static_cast<std::size_t>(i)] = defaultControl(static_cast<Ctrl>(i));
        }
    }
    float& operator[](Ctrl c) { return value[static_cast<std::size_t>(c)]; }
    float operator[](Ctrl c) const { return value[static_cast<std::size_t>(c)]; }
};

struct ArmQuats {
    Quat upper;
    Quat lower;
    Quat hand;
};

ArmQuats armQuats(int side, const Controls& c) {
    const bool left = side == 0;
    const float elev = c[left ? Ctrl::ArmLElev : Ctrl::ArmRElev] * kDeg;
    const float azim = c[left ? Ctrl::ArmLAzim : Ctrl::ArmRAzim] * kDeg;
    const float outward = std::cos(azim) * std::sin(elev);
    const float forward = std::sin(azim) * std::sin(elev);
    const float down = -std::cos(elev);
    Vec3 desired = left ? Vec3{outward, down, forward} : Vec3{-outward, down, forward};
    const Vec3 bind = left ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{-1.0f, 0.0f, 0.0f};
    const Quat swing = qFromTo(bind, math::normalize(desired));
    const Quat twist = quatAxis({1.0f, 0.0f, 0.0f},
                                -c[left ? Ctrl::ArmLTwist : Ctrl::ArmRTwist]);
    const Quat upper = qmul(swing, twist);
    const float hinge = left ? -1.0f : 1.0f;
    const Quat lower = qmul(quatAxis({0.0f, 1.0f, 0.0f},
                                     hinge * c[left ? Ctrl::ArmLElbow : Ctrl::ArmRElbow]),
                            quatAxis({1.0f, 0.0f, 0.0f},
                                     -c[left ? Ctrl::ArmLFtwist : Ctrl::ArmRFtwist]));
    const float wrist = c[left ? Ctrl::ArmLWrist : Ctrl::ArmRWrist];
    const Quat hand = quatAxis({0.0f, 0.0f, 1.0f}, left ? -wrist : wrist);
    return {upper, lower, hand};
}

std::array<Quat, vrm::kHumanoidBoneCount> controlsToBones(const Controls& c) {
    std::array<Quat, vrm::kHumanoidBoneCount> bones{};
    bones.fill(Quat::identity());
    const auto put = [&](HumanoidBone bone, Quat q) {
        bones[static_cast<std::size_t>(bone)] = qNorm(q);
    };
    put(HumanoidBone::Hips,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::PelvisYaw]),
                quatAxis({0.0f, 0.0f, 1.0f}, c[Ctrl::PelvisRoll]),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::PelvisPitch])}));
    const auto spine = [&](Ctrl bend, Ctrl side, Ctrl twist, float wave) {
        return qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[twist] * 0.34f),
                       quatAxis({0.0f, 0.0f, 1.0f}, -c[side] * 0.34f),
                       quatAxis({1.0f, 0.0f, 0.0f}, c[bend] * 0.34f + wave)});
    };
    put(HumanoidBone::Spine,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::SpineTwist] * 0.30f),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::SpineSide] * 0.30f),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::SpineBend] * 0.30f + c[Ctrl::WaveLower])}));
    put(HumanoidBone::Chest,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::SpineTwist] * 0.34f),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::SpineSide] * 0.34f),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::SpineBend] * 0.34f + c[Ctrl::WaveMid])}));
    put(HumanoidBone::UpperChest,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::SpineTwist] * 0.36f),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::SpineSide] * 0.36f),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::SpineBend] * 0.36f + c[Ctrl::WaveUpper])}));
    put(HumanoidBone::Neck,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::HeadTurn] * 0.35f),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::HeadTilt] * 0.35f),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::HeadNod] * 0.35f)}));
    put(HumanoidBone::Head,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::HeadTurn] * 0.65f),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::HeadTilt] * 0.65f),
                quatAxis({1.0f, 0.0f, 0.0f}, c[Ctrl::HeadNod] * 0.65f)}));
    put(HumanoidBone::LeftShoulder,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, -c[Ctrl::ShLFwd]),
                quatAxis({0.0f, 0.0f, 1.0f}, c[Ctrl::ShLLift])}));
    put(HumanoidBone::RightShoulder,
        qchain({quatAxis({0.0f, 1.0f, 0.0f}, c[Ctrl::ShRFwd]),
                quatAxis({0.0f, 0.0f, 1.0f}, -c[Ctrl::ShRLift])}));
    for (int side = 0; side < 2; ++side) {
        const ArmQuats arm = armQuats(side, c);
        put(side == 0 ? HumanoidBone::LeftUpperArm : HumanoidBone::RightUpperArm,
            arm.upper);
        put(side == 0 ? HumanoidBone::LeftLowerArm : HumanoidBone::RightLowerArm,
            arm.lower);
        put(side == 0 ? HumanoidBone::LeftHand : HumanoidBone::RightHand, arm.hand);
    }
    (void)spine;
    return bones;
}

struct Turn {
    float begin = 0.0f;
    float end = 0.0f;
    Vec3 pivot{};
};

// --- choreographer ----------------------------------------------------------

class Choreographer {
public:
    explicit Choreographer(const ChoreographyRequest& request)
        : request_(request),
          period_(static_cast<float>(std::max(4, request.bars) *
                                     std::max(2, request.beatsPerBar))),
          rng_(static_cast<std::uint32_t>(request.seed)),
          curves_{},
          left_(period_, 0),
          right_(period_, 1) {
        for (int i = 0; i < kCtrlCount; ++i) {
            curves_[static_cast<std::size_t>(i)] =
                Curve(period_, defaultControl(static_cast<Ctrl>(i)));
        }
        left_.setStart(0.0f, 0.0f, 0.0f);
        right_.setStart(0.0f, 0.0f, 0.0f);
        // Each foot plans its steps against the other leg.
        left_.setOther(&right_);
        right_.setOther(&left_);
    }

    void pose(float beat, bool hit, float tension,
              std::initializer_list<std::pair<Ctrl, float>> values) {
        for (const auto& [control, value] : values) {
            curves_[static_cast<std::size_t>(control)].key(beat, value, hit, tension);
        }
    }

    void accent(float beat, float strength, const char* kind) {
        AccentMark mark;
        mark.beat = beat;
        mark.strength = strength;
        mark.confidence = 1.0f;
        mark.kind = kind;
        accents_.push_back(mark);
    }

    void turnAbout(float begin, float end, Vec3 pivot) {
        turns_.push_back({begin, end, pivot});
    }

    GeneratedDance build();

private:
    void authorStructure();
    void authorFootwork();
    void authorBody();
    void authorArms();
    void authorAccents();
    // Strength of the nearest authored accent (0..1); a plain beat gets 0.55.
    [[nodiscard]] float accentStrengthAt(float beat) const;
    void armKey(float beat, int side, float elev, float azim, float twist,
                float elbow, float wrist = 0.0f, bool hit = false);
    void bothArms(float beat, std::array<float, 4> leftArm,
                  std::array<float, 4> rightArm, bool hit = false);
    void addSideTouch(float begin, float direction, float size);
    void addCrossStep(float begin, float direction, float size);
    void addPivotTurn(float begin, float direction);
    void addHeelToe(float begin);

    ChoreographyRequest request_;
    float period_ = 32.0f;
    Rng rng_;
    std::array<Curve, kCtrlCount> curves_;
    Footwork left_;
    Footwork right_;
    std::vector<AccentMark> accents_;
    std::vector<Turn> turns_;
    std::vector<std::string> structure_;
    float turnDirection_ = 1.0f;
};

void Choreographer::armKey(float beat, int side, float elev, float azim, float twist,
                           float elbow, float wrist, bool hit) {
    if (side == 0) {
        pose(beat, hit, 1.0f, {{Ctrl::ArmLElev, elev}, {Ctrl::ArmLAzim, azim},
                              {Ctrl::ArmLTwist, twist}, {Ctrl::ArmLElbow, elbow},
                              {Ctrl::ArmLWrist, wrist}});
    } else {
        pose(beat, hit, 1.0f, {{Ctrl::ArmRElev, elev}, {Ctrl::ArmRAzim, azim},
                              {Ctrl::ArmRTwist, twist}, {Ctrl::ArmRElbow, elbow},
                              {Ctrl::ArmRWrist, wrist}});
    }
}

void Choreographer::bothArms(float beat, std::array<float, 4> leftArm,
                             std::array<float, 4> rightArm, bool hit) {
    // Wrist orientation follows the arm plane: a high arm turns the palm
    // slightly down/in, a low arm keeps it neutral. This is authored, not
    // random, and gives the hand layer a real base to build gestures on.
    //
    // Both dependencies are continuous on purpose. The old form
    // `elev > 80 ? 22 : 9` and `azim > 45 ? 1 : -1` was a branch on the pose
    // values, so a key that crossed either threshold baked a 13 deg step into
    // the hand track; played back that is a 600 deg/s wrist snap (the
    // `neonPulse` anomaly: 14.1 rad/s at 60 FPS on Model1). A ramp plus a
    // signed azimuth keeps the same neutral/palm-down intent without a jump.
    const float wristL = wristRollDeg(leftArm[0], leftArm[1]);
    const float wristR = wristRollDeg(rightArm[0], rightArm[1]);
    armKey(beat, 0, leftArm[0], leftArm[1], leftArm[2], leftArm[3], wristL, hit);
    armKey(beat, 1, rightArm[0], rightArm[1], rightArm[2], rightArm[3], wristR, hit);
}

void Choreographer::addSideTouch(float begin, float direction, float size) {
    const float yaw = 12.0f * direction;
    // The lead foot is the one on the side of travel (left foot when travelling to the
    // character's left), the trailing foot closes beside it. On the way back the TRAILING
    // foot returns first: if the outer (lead) foot went home first it would have to
    // cross the inner one, which is still standing out, and a leg would pass through
    // the other.
    Footwork& lead = direction > 0.0f ? left_ : right_;
    Footwork& trail = direction > 0.0f ? right_ : left_;
    lead.step(begin + 0.6f, begin + 1.4f, size * direction, 0.0f, yaw, 9.0f, 0.15f);
    trail.step(begin + 2.2f, begin + 3.0f, size * direction, 0.0f, yaw, 8.0f, 0.1f);
    trail.step(begin + 4.4f, begin + 5.2f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f);
    lead.step(begin + 6.0f, begin + 6.8f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f);
    left_.hold(begin + 8.0f);
    right_.hold(begin + 8.0f);
}

void Choreographer::addCrossStep(float begin, float direction, float size) {
    left_.step(begin + 0.7f, begin + 1.6f, size * direction, size * 0.55f,
               -18.0f * direction, 12.0f, 0.2f);
    right_.step(begin + 2.0f, begin + 2.9f, size * 0.35f * direction, 0.0f,
                -6.0f * direction, 10.0f, 0.1f);
    left_.step(begin + 4.2f, begin + 5.1f, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f);
    right_.step(begin + 5.4f, begin + 6.3f, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f);
    left_.hold(begin + 8.0f);
    right_.hold(begin + 8.0f);
}

void Choreographer::addPivotTurn(float begin, float direction) {
    left_.pivot(begin + 0.5f, begin + 1.6f, 22.0f * direction);
    right_.pivot(begin + 0.5f, begin + 1.6f, 22.0f * direction);
    left_.pivot(begin + 2.5f, begin + 3.4f, 0.0f);
    right_.pivot(begin + 2.5f, begin + 3.4f, 0.0f);
    left_.hold(begin + 4.0f);
    right_.hold(begin + 4.0f);
}

void Choreographer::addHeelToe(float begin) {
    left_.step(begin + 0.8f, begin + 1.8f, 0.0f, 15.0f, 0.0f, 8.0f, 0.85f);
    left_.step(begin + 3.0f, begin + 4.0f, 0.0f, 0.0f, 0.0f, 8.0f, 0.0f);
    right_.step(begin + 4.8f, begin + 5.8f, 0.0f, 13.0f, 0.0f, 7.0f, -0.65f);
    right_.step(begin + 6.2f, begin + 7.2f, 0.0f, 0.0f, 0.0f, 7.0f, 0.0f);
    left_.hold(begin + 8.0f);
    right_.hold(begin + 8.0f);
}

void Choreographer::authorFootwork() {
    const float bar = static_cast<float>(std::max(2, request_.beatsPerBar));
    const float size = (request_.profile == "street" ? 26.0f : 22.0f) +
                       rng_.range(-2.0f, 3.0f);
    left_.hold(bar);
    right_.hold(bar);
    structure_.push_back("Intro: weight shift, no travel");
    const int bars = std::max(4, request_.bars);
    if (bars >= 4) {
        addSideTouch(bar, rng_.chance(0.5f) ? 1.0f : -1.0f, size);
        structure_.push_back("Phrase A: side-touch with weight transfer");
    }
    if (bars >= 6) {
        addCrossStep(bar * 3.0f, rng_.chance(0.5f) ? -1.0f : 1.0f, size * 0.9f);
        structure_.push_back("Phrase B: cross-step and close");
    }
    if (bars >= 8) {
        turnDirection_ = rng_.chance(0.5f) ? 1.0f : -1.0f;
        addPivotTurn(bar * 5.0f, turnDirection_);
        structure_.push_back("Phrase C: pivot turn in place, both feet loaded");
        addHeelToe(bar * 6.0f);
        structure_.push_back("Phrase D: heel and toe taps, forward/back weight");
        left_.hold(period_);
        right_.hold(period_);
    } else {
        left_.hold(period_);
        right_.hold(period_);
    }
    left_.close();
    right_.close();
}

void Choreographer::authorBody() {
    const float bar = static_cast<float>(std::max(2, request_.beatsPerBar));
    const int beats = static_cast<int>(period_);
    const float intensity = std::clamp(request_.intensity, 0.2f, 1.0f);
    const bool street = request_.profile == "street";
    const bool fluid = request_.profile == "fluid";
    const float dip = (street ? 15.0f : fluid ? 8.0f : 11.0f) * (0.75f + 0.5f * intensity);
    for (int beat = 0; beat < beats; ++beat) {
        const float b = static_cast<float>(beat);
        const int section = static_cast<int>(b / (bar * 2.0f));
        const float sectionEnergy = (section == 0) ? 0.7f : (section == 3 ? 1.0f : 0.9f);
        // Every beat used to drop by exactly the same amount at exactly the same
        // time, which is what makes a loop read as a metronome. The accent map
        // decides how deep this beat is and how long it takes to come back up:
        // a strong beat lands lower and releases later, a weak beat is shallow
        // and recovers sooner. The down key never moves off the beat, so the
        // music stays in time; only the depth and the release change.
        const float hit = accentStrengthAt(b);
        const float depth = dip * sectionEnergy * (0.74f + 0.34f * hit);
        const float release = 0.52f + 0.16f * hit;
        pose(b, false, 1.0f,
             {{Ctrl::HipsY, -depth},
              {Ctrl::SpineBend, (street ? 9.0f : 5.0f) * sectionEnergy * (0.85f + 0.2f * hit)}});
        // The spine answers after the pelvis instead of stopping with it.
        pose(b + 0.09f, false, 1.0f,
             {{Ctrl::SpineBend, (street ? 8.4f : 4.6f) * sectionEnergy * (0.85f + 0.2f * hit)}});
        pose(b + release, false, 1.0f,
             {{Ctrl::HipsY, -depth * 0.40f},
              {Ctrl::SpineBend, (street ? 3.6f : 1.8f) * sectionEnergy}});
        // Body wave: each vertebra group lags the previous one.
        pose(b, false, 1.0f, {{Ctrl::WaveLower, -4.5f * intensity}});
        pose(b + 0.5f, false, 1.0f, {{Ctrl::WaveLower, 6.0f * intensity}});
        pose(b + 0.18f, false, 1.0f, {{Ctrl::WaveMid, -3.8f * intensity}});
        pose(b + 0.68f, false, 1.0f, {{Ctrl::WaveMid, 5.0f * intensity}});
        pose(b + 0.36f, false, 1.0f, {{Ctrl::WaveUpper, -3.0f * intensity}});
        pose(b + 0.86f, false, 1.0f, {{Ctrl::WaveUpper, 4.5f * intensity}});
        // Head: a relaxed nod on the beat, a small delayed answer off-beat.
        pose(b + 0.06f, false, 1.0f, {{Ctrl::HeadNod, 7.0f + 3.0f * intensity}});
        pose(b + 0.58f, false, 1.0f, {{Ctrl::HeadNod, -2.0f}});
    }
    // Pelvis / chest opposition, lateral ribcage isolation and shoulder answer.
    for (int phrase = 0; phrase < beats / 2; ++phrase) {
        const float b = static_cast<float>(phrase) * 2.0f;
        const float sign = (phrase % 2 == 0) ? 1.0f : -1.0f;
        pose(b, false, 1.0f,
             {{Ctrl::PelvisYaw, 15.0f * sign * intensity},
              {Ctrl::SpineTwist, -24.0f * sign * intensity},
              {Ctrl::HipsX, 2.5f * sign * intensity}});
        pose(b + 0.75f, false, 1.0f,
             {{Ctrl::SpineSide, 7.0f * sign * intensity},
              {Ctrl::PelvisRoll, 3.0f * sign * intensity}});
        pose(b + 1.25f, false, 1.0f,
             {{Ctrl::SpineSide, -4.0f * sign * intensity}});
        const bool leadLeft = sign > 0.0f;
        pose(b + 0.15f, false, 1.0f,
             {{Ctrl::ShLLift, leadLeft ? 14.0f : -5.0f},
              {Ctrl::ShRLift, leadLeft ? -5.0f : 14.0f},
              {Ctrl::ShLFwd, leadLeft ? 8.0f : -3.0f},
              {Ctrl::ShRFwd, leadLeft ? -3.0f : 8.0f}});
        pose(b + 1.35f, false, 1.0f,
             {{Ctrl::ShLLift, leadLeft ? -4.0f : 8.0f},
              {Ctrl::ShRLift, leadLeft ? 8.0f : -4.0f}});
    }
    // Head looks toward the leading side, then settles through the phrase.
    for (int phrase = 0; request_.headLookAtPhrases && phrase < beats / 4; ++phrase) {
        const float b = static_cast<float>(phrase) * 4.0f;
        const float sign = (phrase % 2 == 0) ? 1.0f : -1.0f;
        pose(b, false, 1.0f,
             {{Ctrl::HeadTurn, 17.0f * sign * intensity},
              {Ctrl::HeadTilt, -8.0f * sign * intensity}});
        pose(b + 2.0f, false, 1.0f,
             {{Ctrl::HeadTurn, -8.0f * sign * intensity},
              {Ctrl::HeadTilt, 6.0f * sign * intensity}});
    }
    // The root turn happens on a fully planted pivot; both feet stay loaded.
    if (request_.bars >= 8) {
        const float turnAt = bar * 5.0f;
        const float direction = turnDirection_;
        pose(turnAt, false, 1.0f, {{Ctrl::RootYaw, 0.0f}});
        pose(turnAt + 1.6f, true, 1.0f, {{Ctrl::RootYaw, 22.0f * direction}});
        pose(turnAt + 2.5f, false, 1.0f, {{Ctrl::RootYaw, 22.0f * direction}});
        pose(turnAt + 3.4f, true, 1.0f, {{Ctrl::RootYaw, 0.0f}});
        turnAbout(turnAt + 0.5f, turnAt + 3.4f, {0.0f, 0.0f, 0.0f});
    }
}

void Choreographer::authorArms() {
    const float bar = static_cast<float>(std::max(2, request_.beatsPerBar));
    const bool street = request_.profile == "street";
    const bool fluid = request_.profile == "fluid";
    const float baseElev = street ? 32.0f : fluid ? 42.0f : 40.0f;
    const float jitterL = rng_.range(-5.0f, 5.0f);
    const float jitterR = rng_.range(-5.0f, 5.0f);

    // Arm presets are planes, not amplitude scalars: one is high, one folded,
    // one diagonal, one low/back. A phrase selects a pair and swaps sides on
    // the next beat, so the arms travel instead of holding a T-pose.
    const std::array<std::array<float, 4>, 7> presets = {{
        // Neutral is a relaxed arm: low, elbows bent, hands in front of the
        // hips. It used to be `baseElev + 34` (about 16 deg below horizontal),
        // which is where the "arms held out like a scarecrow" look of the
        // procedural clips came from: every return pose and most motif blocks
        // used it, so the arms spent the dance in one horizontal plane.
        {24.0f + jitterL * 0.5f, 30.0f, 10.0f, 46.0f},        // neutral / low
        {124.0f + jitterR, 22.0f, 5.0f, 22.0f},               // overhead
        {36.0f + jitterL, 72.0f, 4.0f, 102.0f},               // folded across chest
        {26.0f + jitterR, -12.0f, 12.0f, 54.0f},              // low and back
        {94.0f, 48.0f, 8.0f, 40.0f},                          // diagonal up-forward
        {58.0f, 78.0f, 0.0f, 118.0f},                         // sweep across the body
        {108.0f + jitterL, 8.0f, 10.0f, 16.0f},               // point/reach
    }};
    const auto at = [&](float beat, std::array<float, 4> left,
                        std::array<float, 4> right, bool hit) {
        if (beat < period_ - 1.0e-4f) {
            bothArms(beat, left, right, hit);
        }
    };

    // Intro: open, fold one arm, low sweep, then into the first motif. The
    // "open again" key used to sit at bar - 0.25, i.e. a quarter of a beat before
    // the motif: with a low neutral the arm had to cover a large arc and stop in
    // 0.14 s, which is where neonPulse's 114 m/s^2 hand deceleration came from.
    // The beat-3 pose now goes straight into the motif, so the arrival has a full
    // beat and no pose is lost.
    at(0.0f, presets[0], presets[0], true);
    at(bar * 0.5f, presets[2], presets[0], false);
    at(bar * 0.75f, presets[3], presets[4], false);

    // Development: a two-beat motif that swaps sides on the second beat. The
    // motif rotates every two bars so the four phrases are not the same.
    int motifSeed = (std::abs(request_.seed) / 3) % 4;
    for (int block = static_cast<int>(bar); block + 2 <= static_cast<int>(period_);
         block += 2) {
        const int motif = (motifSeed + block / std::max(2, static_cast<int>(bar))) % 4;
        const int leftIndex = ((motif * 2) % 7 + 7) % 7;
        const int rightIndex = ((motif * 2 + 3) % 7 + 7) % 7;
        const bool boundary = (block % std::max(2, static_cast<int>(bar))) == 0;
        const float swing = fluid ? 0.5f : 0.0f;
        at(static_cast<float>(block), presets[static_cast<std::size_t>(leftIndex)],
           presets[static_cast<std::size_t>(rightIndex)], boundary);
        at(static_cast<float>(block) + 1.0f + swing,
           presets[static_cast<std::size_t>(rightIndex)],
           presets[static_cast<std::size_t>(leftIndex)], false);
        if (fluid) {
            at(static_cast<float>(block) + 1.5f, presets[4], presets[0], false);
        }
    }
    // Build: push the arms higher for the final section, then resolve.
    if (period_ >= bar * 6.0f) {
        at(bar * 5.0f, presets[1], presets[3], true);
        at(bar * 5.5f, presets[3], presets[1], false);
        at(bar * 6.0f, presets[4], presets[4], true);
        at(bar * 6.5f, presets[2], presets[6], false);
        at(period_ - bar * 0.5f, presets[0], presets[0], true);
    }
    at(period_ - 0.5f, presets[0], presets[0], false);

    // Hand openness follows the phrase energy. The detailed hand pose baked per
    // key adds independent finger ripple on top of this scalar.
    for (int section = 0; section < std::max(1, request_.bars / 2); ++section) {
        const float beat = static_cast<float>(section) * bar * 2.0f;
        pose(beat, false, 1.0f, {{Ctrl::HandL, 0.22f}, {Ctrl::HandR, 0.22f}});
    }
    for (int beat = 0; beat < static_cast<int>(period_); beat += 2) {
        const bool leftFirst = (beat / 2) % 2 == 0;
        pose(static_cast<float>(beat) + 0.25f, false, 1.0f,
             {{Ctrl::HandL, leftFirst ? 0.18f : 0.42f},
              {Ctrl::HandR, leftFirst ? 0.42f : 0.18f}});
        pose(static_cast<float>(beat) + 1.25f, false, 1.0f,
             {{Ctrl::HandL, leftFirst ? 0.36f : 0.24f},
              {Ctrl::HandR, leftFirst ? 0.24f : 0.36f}});
    }
    pose(period_ - 1.0f, false, 1.0f, {{Ctrl::HandL, 0.30f}, {Ctrl::HandR, 0.30f}});
}

void Choreographer::authorAccents() {
    const float bar = static_cast<float>(std::max(2, request_.beatsPerBar));
    const int bars = std::max(4, request_.bars);
    // Only marks inside the clip. Short requests (4 or 6 bars) used to receive the
    // fixed phrase/turn/heel/toe marks up to beat 28, i.e. beyond the clip's
    // period; a reader then computed a negative circular distance and ordinary
    // beats inherited strong accents.
    const auto add = [&](float at, float strength, const char* kind) {
        if (at >= 0.0f && at < period_) {
            accent(at, strength, kind);
        }
    };
    for (int b = 0; b < bars; ++b) {
        const float at = static_cast<float>(b) * bar;
        add(at, b == 0 ? 1.0f : 0.82f, b % 2 == 0 ? "downbeat" : "bar");
        if (b % 2 == 0) {
            add(at + bar * 0.5f, 0.55f, "offbeat");
        }
    }
    add(bar * 1.0f, 0.9f, "phrase");
    add(bar * 3.0f, 1.0f, "phrase hit");
    add(bar * 5.0f, 1.0f, "turn accent");
    add(bar * 6.0f, 0.85f, "heel");
    add(bar * 7.0f, 0.9f, "toe");
}

float Choreographer::accentStrengthAt(float beat) const {
    // Nearest authored accent within half a beat; a plain beat moves less (0.55).
    // Several marks may share one beat (a bar accent plus a phrase hit, a turn or
    // a heel/toe mark). They must not be decided by array order: the strongest of
    // the nearest marks wins, otherwise the authored phrase emphasis never reaches
    // the body track.
    constexpr float kTie = 1.0e-4f;
    float nearest = 1.0e9f;
    float strength = 0.55f;
    for (const AccentMark& mark : accents_) {
        // Marks beyond the loop have no circular distance. `beat == period_` is the
        // legal final hit (it aliases beat 0), so only strictly beyond is rejected.
        if (!(mark.beat >= 0.0f) || mark.beat > period_ + 1.0e-4f) {
            continue;
        }
        const float raw = std::abs(mark.beat - beat);
        const float distance = std::min(raw, std::max(0.0f, period_ - raw));
        if (distance > 0.5f) {
            continue;
        }
        if (distance < nearest - kTie) {
            nearest = distance;
            strength = mark.strength;
        } else if (distance <= nearest + kTie) {
            strength = std::max(strength, mark.strength);
        }
    }
    return nearest <= 0.5f ? std::clamp(strength, 0.0f, 1.0f) : 0.55f;
}

void Choreographer::authorStructure() {
    structure_.clear();
    structure_.push_back("Section: intro groove");
    structure_.push_back("Section: development (side-touch, cross-step)");
    structure_.push_back("Section: build (pivot, arms)");
    structure_.push_back("Section: resolve (heel/toe, settle)");
}

GeneratedDance Choreographer::build() {
    const auto started = Clock::now();
    authorStructure();
    authorFootwork();
    // Accents first: the body shaping reads their strength per beat.
    authorAccents();
    authorBody();
    authorArms();

    const int keyCount = static_cast<int>(std::lround(period_ * request_.keysPerBeat)) + 1;
    const int cycles = keyCount - 1;
    // Per-foot load, smoothed as in the authored tools. This is the weight
    // transfer signal: the hips follow the loaded foot, never a sine wave.
    std::array<std::vector<float>, 2> loads;
    const float sigma = 0.22f;
    const int radius = static_cast<int>(sigma * 3.0f * static_cast<float>(request_.keysPerBeat)) + 1;
    std::vector<float> kernel(static_cast<std::size_t>(radius * 2 + 1));
    float kernelSum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float beats = static_cast<float>(i) / static_cast<float>(request_.keysPerBeat);
        kernel[static_cast<std::size_t>(i + radius)] = std::exp(-0.5f * (beats / sigma) * (beats / sigma));
        kernelSum += kernel[static_cast<std::size_t>(i + radius)];
    }
    for (float& value : kernel) {
        value /= kernelSum;
    }
    for (int side = 0; side < 2; ++side) {
        const Footwork& foot = side == 0 ? left_ : right_;
        std::vector<float> swing(static_cast<std::size_t>(cycles), 0.0f);
        for (int i = 0; i < cycles; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(request_.keysPerBeat);
            swing[static_cast<std::size_t>(i)] =
                std::strcmp(foot.at(t).kind, "swing") == 0 ? 1.0f : 0.0f;
        }
        loads[static_cast<std::size_t>(side)].resize(static_cast<std::size_t>(cycles));
        for (int i = 0; i < cycles; ++i) {
            float acc = 0.0f;
            for (int j = -radius; j <= radius; ++j) {
                const int index = ((i + j) % cycles + cycles) % cycles;
                acc += kernel[static_cast<std::size_t>(j + radius)] *
                       swing[static_cast<std::size_t>(index)];
            }
            loads[static_cast<std::size_t>(side)][static_cast<std::size_t>(i)] = 1.0f - acc;
        }
    }

    HumanoidClip clip;
    clip.id = request_.id;
    clip.name = request_.name;
    clip.style = request_.style;
    clip.author = "BG Media procedural choreography (BG Motion 3.2)";
    clip.origin = "generated: motif -> phrase -> section composition, seed " +
                  std::to_string(request_.seed) + ", profile " + request_.profile;
    clip.description = "Procedurally generated full dance. Not motion capture; footwork, "
                       "weight transfer and arm paths are authored by rules.";
    clip.bpm = request_.bpm;
    clip.bpmMin = request_.bpm * 0.72;
    clip.bpmMax = request_.bpm * 1.32;
    clip.beats = period_;
    clip.beatsPerBar = std::max(2, request_.beatsPerBar);
    clip.keysPerBeat = std::max(2, request_.keysPerBeat);
    clip.loop = request_.loop;
    clip.seamless = request_.loop;
    clip.tempoSync = true;
    clip.energy = std::clamp(request_.intensity, 0.0f, 1.0f);
    clip.keyCount = keyCount;
    clip.accents = accents_;
    left_.contacts(clip.contacts);
    right_.contacts(clip.contacts);
    std::sort(clip.contacts.begin(), clip.contacts.end(),
              [](const ContactSegment& a, const ContactSegment& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.foot < b.foot;
              });

    std::array<std::vector<Quat>, vrm::kHumanoidBoneCount> boneTracks;
    std::array<bool, vrm::kHumanoidBoneCount> boneUsed{};
    std::vector<Vec3> hipsTrack;
    std::vector<Vec3> rootTrack;
    std::array<std::vector<Vec3>, 2> footPosition;
    std::array<std::vector<float>, 2> footYaw;
    std::array<std::vector<float>, 2> footToe;
    std::array<std::vector<float>, 2> handScalar;
    std::array<std::vector<HandPose>, 2> handPoses;
    hipsTrack.reserve(static_cast<std::size_t>(keyCount));
    rootTrack.reserve(static_cast<std::size_t>(keyCount));
    for (auto& track : footPosition) track.reserve(static_cast<std::size_t>(keyCount));
    for (auto& track : footYaw) track.reserve(static_cast<std::size_t>(keyCount));
    for (auto& track : footToe) track.reserve(static_cast<std::size_t>(keyCount));
    for (auto& track : handScalar) track.reserve(static_cast<std::size_t>(keyCount));
    for (auto& track : handPoses) track.reserve(static_cast<std::size_t>(keyCount));
    std::array<Quat, vrm::kHumanoidBoneCount> previousBones{};
    previousBones.fill(Quat::identity());

    for (int i = 0; i < keyCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(request_.keysPerBeat);
        Controls c;
        for (int ci = 0; ci < kCtrlCount; ++ci) {
            c.value[static_cast<std::size_t>(ci)] =
                curves_[static_cast<std::size_t>(ci)].at(t);
        }
        const float li = loads[0][static_cast<std::size_t>(i % cycles)];
        const float ri = loads[1][static_cast<std::size_t>(i % cycles)];
        const float roll = (request_.profile == "fluid" ? 6.0f : 9.0f) * (li - ri);
        c[Ctrl::PelvisRoll] += roll;
        c[Ctrl::SpineSide] -= 0.9f * roll;

        // Root turn around the declared pivot point.
        float rootX = c[Ctrl::RootX];
        float rootZ = c[Ctrl::RootZ];
        for (const Turn& turn : turns_) {
            if (t < turn.begin) {
                continue;
            }
            const float end = std::min(t, turn.end);
            const float delta =
                curves_[static_cast<std::size_t>(Ctrl::RootYaw)].at(end) -
                curves_[static_cast<std::size_t>(Ctrl::RootYaw)].at(turn.begin);
            const float angle = delta * kDeg;
            const float dx = rootX - turn.pivot.x;
            const float dz = rootZ - turn.pivot.y;
            rootX = turn.pivot.x + dx * std::cos(angle) + dz * std::sin(angle);
            rootZ = turn.pivot.y - dx * std::sin(angle) + dz * std::cos(angle);
        }

        const Footwork::Sample fl = left_.at(t);
        const Footwork::Sample fr = right_.at(t);
        float comX = 0.5f * (fl.x + fr.x);
        float comZ = 0.5f * (fl.z + fr.z);
        if (li + ri > 0.05f) {
            comX = (li * (kFootHalfCm + fl.x) + ri * (-kFootHalfCm + fr.x)) / (li + ri);
            comZ = (li * fl.z + ri * fr.z) / (li + ri);
        }
        const float balanceX = request_.profile == "fluid" ? 0.72f : 0.84f;
        const float balanceZ = 0.30f;
        const float hipsX = c[Ctrl::HipsX] + comX * balanceX;
        const float hipsY = c[Ctrl::HipsY];
        const float hipsZ = c[Ctrl::HipsZ] + comZ * balanceZ;
        // The root follows the travelled centre of mass a little, so a side
        // step moves the whole character instead of stretching both legs while
        // the root remains nailed to the stage. Feet keep their stage targets.
        const float rootTravelX = rootX + comX * 0.24f;
        const float rootTravelZ = rootZ + comZ * 0.18f;

        const auto bones = controlsToBones(c);
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const HumanoidBone bone = static_cast<HumanoidBone>(b);
            if (bone == HumanoidBone::LeftUpperLeg || bone == HumanoidBone::LeftLowerLeg ||
                bone == HumanoidBone::LeftFoot || bone == HumanoidBone::LeftToes ||
                bone == HumanoidBone::RightUpperLeg || bone == HumanoidBone::RightLowerLeg ||
                bone == HumanoidBone::RightFoot || bone == HumanoidBone::RightToes) {
                continue;
            }
            Quat q = bones[b];
            if (math::dot(q, previousBones[b]) < 0.0f) {
                q = {-q.x, -q.y, -q.z, -q.w};
            }
            previousBones[b] = q;
            if (!boneUsed[b] && std::abs(q.w) < 0.999999f) {
                boneUsed[b] = true;
            }
            boneTracks[b].push_back(q);
        }
        hipsTrack.push_back({hipsX / kRefHeightCm, hipsY / kRefHeightCm,
                             hipsZ / kRefHeightCm});
        rootTrack.push_back({rootTravelX / kRefHeightCm, rootTravelZ / kRefHeightCm,
                             c[Ctrl::RootYaw] / 360.0f});
        footPosition[0].push_back({fl.x / kRefHeightCm, fl.lift / kRefHeightCm,
                                   fl.z / kRefHeightCm});
        footPosition[1].push_back({fr.x / kRefHeightCm, fr.lift / kRefHeightCm,
                                   fr.z / kRefHeightCm});
        footYaw[0].push_back(fl.yaw / 360.0f);
        footYaw[1].push_back(fr.yaw / 360.0f);
        footToe[0].push_back(std::clamp(fl.toe, -1.0f, 1.0f));
        footToe[1].push_back(std::clamp(fr.toe, -1.0f, 1.0f));
        const float handL = clamp01(c[Ctrl::HandL]);
        const float handR = clamp01(c[Ctrl::HandR]);
        handScalar[0].push_back(handL);
        handScalar[1].push_back(handR);
        for (int side = 0; side < 2; ++side) {
            const float base = side == 0 ? handL : handR;
            HandPose pose = relaxedHandPose(base);
            // Subtle independent finger life even without an overlay. The
            // amplitude is small enough not to read as random noise.
            for (int finger = 0; finger < 5; ++finger) {
                const float phase = t * 2.0f + static_cast<float>(finger) * 0.13f +
                                    static_cast<float>(side) * 0.5f;
                const float ripple = 0.035f * std::sin(phase * 6.2831853f) *
                                     (0.4f + 0.6f * request_.intensity);
                for (int joint = 0; joint < 3; ++joint) {
                    pose.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] =
                        clamp01(pose.curl[static_cast<std::size_t>(finger)]
                                       [static_cast<std::size_t>(joint)] + ripple);
                }
                if (finger > 0) {
                    pose.spread[static_cast<std::size_t>(finger)] =
                        0.10f * std::sin(phase * 3.14159265f);
                }
            }
            clampHandPose(pose);
            handPoses[static_cast<std::size_t>(side)].push_back(pose);
        }
    }

    // Force an exact loop seam. Curves and feet are already periodic; this
    // removes any floating-point difference in the final key.
    if (clip.loop && keyCount > 1) {
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            if (!boneTracks[b].empty()) {
                boneTracks[b].back() = boneTracks[b].front();
            }
        }
        hipsTrack.back() = hipsTrack.front();
        rootTrack.back() = rootTrack.front();
        for (int side = 0; side < 2; ++side) {
            footPosition[static_cast<std::size_t>(side)].back() =
                footPosition[static_cast<std::size_t>(side)].front();
            footYaw[static_cast<std::size_t>(side)].back() =
                footYaw[static_cast<std::size_t>(side)].front();
            footToe[static_cast<std::size_t>(side)].back() =
                footToe[static_cast<std::size_t>(side)].front();
            handPoses[static_cast<std::size_t>(side)].back() =
                handPoses[static_cast<std::size_t>(side)].front();
            handScalar[static_cast<std::size_t>(side)].back() =
                handScalar[static_cast<std::size_t>(side)].front();
        }
    }

    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        if (boneUsed[b]) {
            clip.bones[b] = std::move(boneTracks[b]);
        }
    }
    clip.hips = std::move(hipsTrack);
    clip.root = std::move(rootTrack);
    for (int side = 0; side < 2; ++side) {
        clip.feet[static_cast<std::size_t>(side)].position =
            std::move(footPosition[static_cast<std::size_t>(side)]);
        clip.feet[static_cast<std::size_t>(side)].yaw =
            std::move(footYaw[static_cast<std::size_t>(side)]);
        clip.feet[static_cast<std::size_t>(side)].toe =
            std::move(footToe[static_cast<std::size_t>(side)]);
        clip.hands[static_cast<std::size_t>(side)] =
            std::move(handScalar[static_cast<std::size_t>(side)]);
        clip.handPoses[static_cast<std::size_t>(side)] =
            std::move(handPoses[static_cast<std::size_t>(side)]);
    }
    // Entry points: bar starts and half-bars. Exits: the same points so a
    // manual switch can land musically without cutting a step.
    for (int b = 0; b < static_cast<int>(period_); b += std::max(1, clip.beatsPerBar)) {
        clip.entryBeats.push_back(static_cast<float>(b));
        clip.exitBeats.push_back(static_cast<float>(b));
    }

    GeneratedDance dance;
    dance.clip = std::move(clip);
    if (request_.includeOverlays) {
        const float density = std::clamp(request_.intensity * 0.9f, 0.2f, 1.1f);
        dance.overlays = planPrimitiveOverlays(dance.clip,
                                               static_cast<std::uint32_t>(request_.seed + 17),
                                               density, true);
    }
    dance.structure = structure_;

    // --- stats -------------------------------------------------------------
    auto& stats = dance.stats;
    stats.beats = static_cast<int>(period_);
    stats.phrases = std::max(1, static_cast<int>(period_ / 8.0f));
    stats.sections = 4;
    stats.accents = static_cast<int>(dance.clip.accents.size());
    stats.overlayCues = static_cast<int>(dance.overlays.size());
    for (const ContactSegment& segment : dance.clip.contacts) {
        if (segment.pivot) {
            ++stats.pivots;
        } else {
            ++stats.steps;
        }
    }
    for (const auto& track : dance.clip.bones) {
        stats.bonesUsed += track.empty() ? 0 : 1;
    }
    stats.peakHipDrop = 0.0f;
    for (Vec3 hips : dance.clip.hips) {
        stats.peakHipDrop = std::min(stats.peakHipDrop, hips.y);
    }
    // Sample at 240 Hz to measure speed / acceleration / seam without claiming
    // physical validation from one number.
    const double seconds = period_ * 60.0 / std::max(20.0, request_.bpm);
    const int samples = static_cast<int>(std::max(2.0, seconds * 240.0));
    std::vector<ClipPose> poses;
    poses.reserve(static_cast<std::size_t>(samples) + 1);
    const double beatsPerSample = static_cast<double>(period_) / samples;
    for (int i = 0; i <= samples; ++i) {
        poses.push_back(sampleClip(dance.clip, i * beatsPerSample));
    }
    std::array<Quat, vrm::kHumanoidBoneCount> last{};
    std::array<Quat, vrm::kHumanoidBoneCount> lastLast{};
    last.fill(Quat::identity());
    lastLast.fill(Quat::identity());
    std::array<float, vrm::kHumanoidBoneCount> lastSpeed{};
    const double dt = seconds / samples;
    for (std::size_t i = 0; i < poses.size(); ++i) {
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const Quat q = poses[i].bone[b].normalized();
            if (!std::isfinite(q.w) || !std::isfinite(q.x) || !std::isfinite(q.y) ||
                !std::isfinite(q.z)) {
                stats.finite = false;
            }
            if (i >= 2) {
                const Quat d1 = (q * last[b].conjugate()).normalized();
                const Quat d0 = (last[b] * lastLast[b].conjugate()).normalized();
                const float angle1 = 2.0f * std::acos(std::clamp(std::abs(d1.w), 0.0f, 1.0f));
                const float angle0 = 2.0f * std::acos(std::clamp(std::abs(d0.w), 0.0f, 1.0f));
                const float speed = angle1 * 57.29578f / static_cast<float>(dt);
                const float accel =
                    std::abs(speed - lastSpeed[b]) / static_cast<float>(dt);
                stats.maxAngularSpeedDegPerSec =
                    std::max(stats.maxAngularSpeedDegPerSec, speed);
                stats.maxAngularAccelerationDegPerSec2 =
                    std::max(stats.maxAngularAccelerationDegPerSec2, accel);
                lastSpeed[b] = speed;
                (void)angle0;
            }
            lastLast[b] = last[b];
            last[b] = q;
        }
    }
    const ClipPose first = sampleClip(dance.clip, 0.0);
    const ClipPose nearEnd = sampleClip(dance.clip, period_ - 1.0e-4);
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const float dot = std::abs(math::dot(first.bone[b], nearEnd.bone[b]));
        stats.loopSeamDeg = std::max(
            stats.loopSeamDeg, 2.0f * std::acos(std::clamp(dot, 0.0f, 1.0f)) * 57.29578f);
    }
    // Planted foot slide measured on the authored stage tracks.
    for (const ContactSegment& segment : dance.clip.contacts) {
        const auto& track = dance.clip.feet[static_cast<std::size_t>(segment.foot)].position;
        if (track.size() != static_cast<std::size_t>(dance.clip.keyCount)) {
            continue;
        }
        const int k0 = std::clamp(static_cast<int>(std::ceil(segment.start * dance.clip.keysPerBeat)),
                                  0, dance.clip.keyCount - 1);
        const int k1 = std::clamp(static_cast<int>(std::floor(segment.end * dance.clip.keysPerBeat)),
                                  0, dance.clip.keyCount - 1);
        for (int k = k0; k <= k1; ++k) {
            const Vec3 d = track[static_cast<std::size_t>(k)] -
                           track[static_cast<std::size_t>(k0)];
            stats.footSlideHeightUnits =
                std::max(stats.footSlideHeightUnits, d.length());
        }
    }
    stats.generationMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return dance;
}

} // namespace

GeneratedDance ProceduralChoreography::generate(const ChoreographyRequest& request) {
    Choreographer choreographer(request);
    return choreographer.build();
}

std::vector<GeneratedDance> ProceduralChoreography::showcase() {
    std::vector<GeneratedDance> dances;
    ChoreographyRequest neon;
    neon.id = "neonPulse";
    neon.name = "Neon Pulse";
    neon.style = DanceStyle::Custom;
    neon.profile = "neon";
    neon.bpm = 104.0;
    neon.bars = 8;
    neon.seed = 7;
    neon.intensity = 0.78f;
    dances.push_back(generate(neon));

    ChoreographyRequest street;
    street.id = "streetCurrent";
    street.name = "Street Current";
    street.style = DanceStyle::Custom;
    street.profile = "street";
    street.bpm = 96.0;
    street.bars = 8;
    street.seed = 23;
    street.intensity = 0.68f;
    dances.push_back(generate(street));

    ChoreographyRequest fluid;
    fluid.id = "fluidSignal";
    fluid.name = "Fluid Signal";
    fluid.style = DanceStyle::Custom;
    fluid.profile = "fluid";
    fluid.bpm = 112.0;
    fluid.bars = 8;
    fluid.seed = 41;
    fluid.intensity = 0.62f;
    dances.push_back(generate(fluid));
    return dances;
}

bool ProceduralChoreography::save(const GeneratedDance& dance,
                                  const std::filesystem::path& path,
                                  std::string* error) {
    const std::string json = clipToJson(dance.clip);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    file << json << '\n';
    if (!file) {
        if (error) *error = "failed to write " + path.string();
        return false;
    }
    return true;
}

} // namespace bgmedia::motion
