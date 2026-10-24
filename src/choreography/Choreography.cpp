#include "bgmedia/choreography/Choreography.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace bgmedia {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;

inline float clampValue(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

inline double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

inline double smoothStep01(double value) {
    const double x = clamp01(value);
    return x * x * (3.0 - 2.0 * x);
}

inline float wrapTurn(float turns) {
    float value = std::fmod(turns + 0.5f, 1.0f);
    if (value < 0.0f) {
        value += 1.0f;
    }
    return value - 0.5f;
}

inline float turnDelta(float from, float to) {
    return wrapTurn(to - from);
}

inline float applyEasing(Easing easing, float t) {
    t = clampValue(t, 0.0f, 1.0f);
    switch (easing) {
    case Easing::Linear:
    case Easing::Cubic:
        return t;
    case Easing::EaseIn:
        return t * t;
    case Easing::EaseOut: {
        const float inv = 1.0f - t;
        return 1.0f - inv * inv;
    }
    case Easing::EaseInOut:
        return t < 0.5f ? 2.0f * t * t
                        : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    case Easing::SmoothStep:
        return t * t * (3.0f - 2.0f * t);
    }
    return t;
}

#define BGM_POSE_SCALARS(X)                                                       \
    X(bob) X(lean) X(armLeft) X(armRight) X(legLeft) X(legRight) X(energy)       \
        X(headNod) X(headTilt) X(headTurn) X(chestTwist) X(hipShift) X(kneeLeft) \
            X(kneeRight) X(elbowLeft) X(elbowRight) X(footLeft) X(footRight)

CharacterPose clampPose(CharacterPose pose) {
#define BGM_CLAMP_FIELD(field) pose.field = clampValue(pose.field, -1.0f, 1.0f);
    BGM_POSE_SCALARS(BGM_CLAMP_FIELD)
#undef BGM_CLAMP_FIELD
    pose.energy = clampValue(pose.energy, 0.0f, 1.0f);
    pose.phase = clampValue(pose.phase, 0.0f, 1.0f);
    pose.blend = clampValue(pose.blend, 0.0f, 1.0f);
    pose.impact = clampValue(pose.impact, 0.0f, 1.0f);
    pose.rotation = wrapTurn(pose.rotation);
    return pose;
}

CharacterPose idlePose() {
    CharacterPose pose;
    pose.move = MoveKind::Idle;
    return pose;
}

CharacterPose freezePose() {
    CharacterPose pose;
    pose.move = MoveKind::Freeze;
    return pose;
}

CharacterPose blendPoses(const CharacterPose& a, const CharacterPose& b, float u) {
    u = clampValue(u, 0.0f, 1.0f);
    CharacterPose out;
#define BGM_LERP_FIELD(field) out.field = a.field + (b.field - a.field) * u;
    BGM_POSE_SCALARS(BGM_LERP_FIELD)
#undef BGM_LERP_FIELD
    out.rotation = wrapTurn(a.rotation + turnDelta(a.rotation, b.rotation) * u);
    out.phase = a.phase + (b.phase - a.phase) * u;
    out.blend = a.blend + (b.blend - a.blend) * u;
    out.impact = a.impact + (b.impact - a.impact) * u;
    out.move = u < 0.5f ? a.move : b.move;
    out.accented = u < 0.5f ? a.accented : b.accented;
    return clampPose(out);
}

float cueIntensity(const Beat& beat) {
    float intensity = 0.35f + 0.55f * clampValue(beat.strength, 0.0f, 1.0f);
    if (beat.downbeat) {
        intensity += 0.08f;
    }
    intensity *= 0.75f + 0.25f * clampValue(beat.confidence, 0.0f, 1.0f);
    return clampValue(intensity, 0.1f, 1.0f);
}

struct BeatLocation {
    int index = -1;
    double start = 0.0;
    double end = 0.0;
    double phase = 0.0;
    double duration = 0.0;
};

struct AccentWindow {
    double preparation = 0.0;
    double hold = 0.0;
    double release = 0.0;
    double end = 0.0;
};

struct BuildContext {
    const AnalysisResult& analysis;
    ChoreographyStyle style;
    int beatsPerBar = 4;
    double duration = 0.0;
    std::vector<float> bassStrength;
    std::vector<double> barStart;
    std::vector<double> barEnd;
    std::vector<int> barFirstBeat;

    BuildContext(const AnalysisResult& source, ChoreographyStyle styleValue)
        : analysis(source), style(styleValue) {
        beatsPerBar = std::max(2, analysis.beatsPerBar);
        duration = analysis.info.duration;
        if (!analysis.beats.empty()) {
            duration = std::max(duration, analysis.beats.back().time + 0.1);
        }
        if (!std::isfinite(duration) || duration <= 0.0) {
            duration = 1.0;
        }

        bassStrength.assign(analysis.beats.size(), 0.0f);
        for (const BassAccent& accent : analysis.bassAccents) {
            // nearestBeat is the analyzer's deliberate low-frequency event
            // classification. Do not re-snap an offbeat bass attack onto the
            // nearest beat here: at fast tempos the old 0.16 s fallback put a
            // BassDip on nearly every beat. The local interval bound keeps a
            // genuinely beat-aligned accent while rejecting a subdivision.
            int index = accent.nearestBeat;
            if (index < 0 || static_cast<std::size_t>(index) >= analysis.beats.size() ||
                accent.confidence < 0.20f) {
                continue;
            }
            const double interval = beatInterval(index);
            const double tolerance = std::min(0.08, 0.25 * interval);
            if (std::abs(analysis.beats[static_cast<std::size_t>(index)].time -
                         accent.time) <= tolerance) {
                bassStrength[static_cast<std::size_t>(index)] =
                    std::max(bassStrength[static_cast<std::size_t>(index)],
                             clampValue(accent.strength, 0.0f, 1.5f));
            }
        }

        int maxBar = -1;
        for (const Beat& beat : analysis.beats) {
            maxBar = std::max(maxBar, beat.barIndex);
        }
        if (maxBar < 0) {
            maxBar = 0;
        }
        barStart.assign(static_cast<std::size_t>(maxBar + 2),
                        std::numeric_limits<double>::infinity());
        barEnd.assign(static_cast<std::size_t>(maxBar + 2), -1.0);
        barFirstBeat.assign(static_cast<std::size_t>(maxBar + 2), -1);
        for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
            const Beat& beat = analysis.beats[i];
            const int bar = std::max(0, beat.barIndex);
            if (beat.time < barStart[static_cast<std::size_t>(bar)]) {
                barStart[static_cast<std::size_t>(bar)] = beat.time;
                barFirstBeat[static_cast<std::size_t>(bar)] = static_cast<int>(i);
            }
            barEnd[static_cast<std::size_t>(bar)] =
                std::max(barEnd[static_cast<std::size_t>(bar)],
                         beat.time + beatInterval(static_cast<int>(i)));
        }
        for (int bar = 0; bar <= maxBar; ++bar) {
            if (!std::isfinite(barStart[static_cast<std::size_t>(bar)])) {
                barStart[static_cast<std::size_t>(bar)] = 0.0;
            }
            if (barEnd[static_cast<std::size_t>(bar)] < 0.0) {
                barEnd[static_cast<std::size_t>(bar)] = duration;
            }
        }
    }

    [[nodiscard]] double beatInterval(int index) const {
        if (analysis.beats.empty() || index < 0 ||
            static_cast<std::size_t>(index) >= analysis.beats.size()) {
            return 0.5;
        }
        const double start = analysis.beats[static_cast<std::size_t>(index)].time;
        double end = duration;
        if (static_cast<std::size_t>(index + 1) < analysis.beats.size()) {
            end = analysis.beats[static_cast<std::size_t>(index + 1)].time;
        }
        return std::max(0.05, end - start);
    }

    [[nodiscard]] double tempoAmplitude(double interval) const {
        if (!style.reduceAmplitudeAtFastTempo) {
            return 1.0;
        }
        // 120 BPM = 1.0. At 163 BPM the amplitude is reduced to about 0.74;
        // it never collapses completely.
        return std::clamp(interval / 0.5, 0.52, 1.0);
    }

    [[nodiscard]] BeatLocation locate(double time) const {
        BeatLocation location;
        if (analysis.beats.empty()) {
            return location;
        }
        if (time <= analysis.beats.front().time) {
            location.index = 0;
            location.start = analysis.beats.front().time;
            location.end = location.start + beatInterval(0);
            location.duration = std::max(0.05, location.end - location.start);
            location.phase = 0.0;
            return location;
        }

        const auto it = std::upper_bound(
            analysis.beats.begin(), analysis.beats.end(), time,
            [](double value, const Beat& beat) { return value < beat.time; });
        location.index = static_cast<int>(it - analysis.beats.begin()) - 1;
        location.start =
            analysis.beats[static_cast<std::size_t>(location.index)].time;
        location.end = location.index + 1 < static_cast<int>(analysis.beats.size())
                           ? analysis.beats[static_cast<std::size_t>(location.index + 1)].time
                           : duration;
        location.end = std::max(location.start + 0.05, location.end);
        location.duration = location.end - location.start;
        location.phase = clamp01((time - location.start) / location.duration);
        return location;
    }

    [[nodiscard]] float bassAt(std::size_t index) const {
        return index < bassStrength.size() ? bassStrength[index] : 0.0f;
    }

    [[nodiscard]] MoveKind accentMoveAt(std::size_t index) const {
        if (index >= analysis.beats.size()) {
            return MoveKind::Idle;
        }
        if (bassAt(index) > 0.05f) {
            return MoveKind::BassDip;
        }
        if (analysis.beats[index].downbeat) {
            return MoveKind::AccentPose;
        }
        return MoveKind::Idle;
    }

    [[nodiscard]] float accentStrengthAt(std::size_t index) const {
        if (index >= analysis.beats.size()) {
            return 0.0f;
        }
        const Beat& beat = analysis.beats[index];
        float strength = 0.45f + 0.45f * clampValue(beat.strength, 0.0f, 1.0f);
        if (bassAt(index) > 0.05f) {
            strength = std::max(strength,
                                0.55f + 0.45f * clampValue(bassAt(index), 0.0f, 1.0f));
        }
        return clampValue(strength, 0.25f, 1.0f);
    }

    [[nodiscard]] AccentWindow accentWindow(std::size_t index,
                                            MoveKind move) const {
        const double interval = beatInterval(static_cast<int>(index));
        const double anticipationFraction =
            std::clamp(static_cast<double>(style.anticipation), 0.15, 0.75);
        AccentWindow window;
        // Proportional to the local beat: preparation and release slow down
        // with the music and compress at fast tempos. Caps are in seconds, not
        // frames, so rendering FPS never changes the animation.
        window.preparation =
            std::min(std::clamp(interval * anticipationFraction, 0.075, 0.30),
                     interval * 0.60);
        window.hold = std::min(std::clamp(interval * 0.09, 0.015, 0.045),
                               interval * 0.14);
        // Never let one accent's release overlap the next beat's preparation:
        // overlapping windows could otherwise force an instantaneous target
        // switch at very fast tempos. The remaining time budget is used for
        // the release, and amplitude is reduced elsewhere.
        const double remaining = interval - window.preparation - window.hold;
        window.release = std::min(
            std::clamp(interval * style.accentRelease, 0.08, 0.36),
            std::max(0.025, remaining));
        window.end = window.hold + window.release;
        (void)move;
        return window;
    }

    [[nodiscard]] double accentWeight(double time, std::size_t index,
                                      const AccentWindow& window) const {
        const double accentTime = analysis.beats[index].time;
        const double relative = time - accentTime;
        if (relative < -window.preparation || relative > window.end) {
            return 0.0;
        }
        if (relative < 0.0) {
            return smoothStep01((relative + window.preparation) /
                                std::max(0.001, window.preparation));
        }
        if (relative <= window.hold) {
            return 1.0;
        }
        return 1.0 - smoothStep01((relative - window.hold) /
                                  std::max(0.001, window.release));
    }

    [[nodiscard]] double silenceWeight(double time) const {
        double outside = 1.0;
        for (const SilenceRegion& region : analysis.silences) {
            if (region.end <= region.start) {
                continue;
            }
            const double fade = std::min(0.18, std::max(0.04, (region.end - region.start) * 0.08));
            if (time >= region.start && time <= region.end) {
                return 0.0;
            }
            if (time >= region.start - fade && time < region.start) {
                const double u = (time - (region.start - fade)) / fade;
                outside = std::min(outside, 1.0 - smoothStep01(u));
            } else if (time > region.end && time <= region.end + fade) {
                const double u = (time - region.end) / fade;
                outside = std::min(outside, smoothStep01(u));
            }
        }
        return clamp01(outside);
    }
};

MoveKind movementForBar(int barIndex) {
    switch ((barIndex % 4 + 4) % 4) {
    case 0:
        return MoveKind::ArmWave;
    case 1:
        return MoveKind::Sway;
    case 2:
        return MoveKind::Bounce;
    default:
        return MoveKind::HeadNod;
    }
}

float movementIntensity(const AnalysisResult& analysis, int beatIndex,
                        const ChoreographyStyle& style, double beatDuration) {
    if (beatIndex < 0 || static_cast<std::size_t>(beatIndex) >= analysis.beats.size()) {
        return 0.0f;
    }
    const Beat& beat = analysis.beats[static_cast<std::size_t>(beatIndex)];
    const double tempo = style.reduceAmplitudeAtFastTempo
                             ? std::clamp(beatDuration / 0.5, 0.52, 1.0)
                             : 1.0;
    return clampValue(cueIntensity(beat) * style.intensity *
                          static_cast<float>(tempo),
                      0.0f, 1.0f);
}

CharacterPose movementPose(MoveKind move, const BuildContext& context,
                           const BeatLocation& location, double time,
                           float intensityOverride = -1.0f) {
    (void)time;
    const double phase = location.phase;
    const double globalBeat = static_cast<double>(location.index) + phase;
    const float amp = intensityOverride >= 0.0f
                          ? intensityOverride
                          : movementIntensity(context.analysis, location.index,
                                              context.style, location.duration);
    CharacterPose pose;
    pose.move = move;
    pose.phase = static_cast<float>(phase);
    pose.blend = 1.0f;

    const double twoPi = 2.0 * kPi;
    const double cycle = std::cos(twoPi * phase);
    const double cycleSin = std::sin(twoPi * phase);
    const double halfCycle = std::sin(kPi * phase);

    switch (move) {
    case MoveKind::Pulse:
        pose.bob = static_cast<float>(-0.35 * cycle * amp);
        pose.armLeft = static_cast<float>(-0.2 * halfCycle * amp);
        pose.armRight = static_cast<float>(0.2 * halfCycle * amp);
        pose.energy = static_cast<float>(0.48 + 0.35 * std::abs(cycle)) * amp;
        break;
    case MoveKind::Bounce:
        // Lowest at the beat, highest halfway to the next beat. Knees absorb
        // the vertical travel so the rendered feet can stay planted.
        pose.bob = static_cast<float>(-0.72 * cycle * amp);
        pose.lean = static_cast<float>(0.11 * cycleSin * amp);
        pose.armLeft = static_cast<float>(-0.23 - 0.18 * cycle) * amp;
        pose.armRight = static_cast<float>(0.23 + 0.18 * cycle) * amp;
        pose.kneeLeft = static_cast<float>(0.34 + 0.32 * cycle) * amp;
        pose.kneeRight = static_cast<float>(0.34 + 0.32 * cycle) * amp;
        pose.headNod = static_cast<float>(0.16 * cycle) * amp;
        pose.chestTwist = static_cast<float>(0.06 * cycleSin) * amp;
        pose.energy = static_cast<float>(0.55 + 0.35 * std::abs(cycle)) * amp;
        break;
    case MoveKind::Sway: {
        const double sway = std::sin(kPi * globalBeat);
        pose.lean = static_cast<float>(0.72 * sway) * amp;
        pose.hipShift = static_cast<float>(-0.58 * sway) * amp;
        pose.chestTwist = static_cast<float>(0.24 * sway) * amp;
        pose.armLeft = static_cast<float>(-0.28 - 0.45 * sway) * amp;
        pose.armRight = static_cast<float>(0.28 - 0.45 * sway) * amp;
        pose.bob = static_cast<float>(-0.16 * std::abs(sway)) * amp;
        pose.headTilt = static_cast<float>(-0.28 * sway) * amp;
        pose.headTurn = static_cast<float>(0.12 * sway) * amp;
        pose.kneeLeft = static_cast<float>(0.20 + 0.18 * std::max(0.0, sway)) * amp;
        pose.kneeRight = static_cast<float>(0.20 + 0.18 * std::max(0.0, -sway)) * amp;
        pose.energy = static_cast<float>(0.48 + 0.30 * std::abs(sway)) * amp;
        break;
    }
    case MoveKind::ArmWave: {
        // A two-beat travelling wave. Using the global beat phase (rather than
        // a per-beat parity flip) keeps the arm endpoints identical on both
        // sides of every beat and, crucially, when the next bar starts with
        // ArmWave after a bar-boundary cross-fade.
        const double wave = std::cos(kPi * globalBeat);
        const double sweep = std::sin(kPi * globalBeat);
        pose.armLeft = static_cast<float>(-0.42 - 0.40 * wave) * amp;
        pose.armRight = static_cast<float>(0.42 - 0.40 * wave) * amp;
        pose.elbowLeft = static_cast<float>(0.30 + 0.35 * wave) * amp;
        pose.elbowRight = static_cast<float>(0.30 - 0.35 * wave) * amp;
        pose.lean = static_cast<float>(0.22 * sweep) * amp;
        pose.bob = static_cast<float>(-0.16 * std::abs(sweep)) * amp;
        pose.headNod = static_cast<float>(0.13 * sweep) * amp;
        pose.headTurn = static_cast<float>(0.18 * sweep) * amp;
        pose.chestTwist = static_cast<float>(0.18 * sweep) * amp;
        pose.energy = static_cast<float>(0.55 + 0.30 * std::abs(wave)) * amp;
        break;
    }
    case MoveKind::HeadNod:
        pose.headNod = static_cast<float>(-0.72 * cycle) * amp;
        pose.headTilt = static_cast<float>(0.10 * cycleSin) * amp;
        pose.headTurn = static_cast<float>(0.10 * cycleSin) * amp;
        pose.bob = static_cast<float>(-0.13 * cycle) * amp;
        pose.lean = static_cast<float>(0.10 * cycleSin) * amp;
        pose.armLeft = static_cast<float>(-0.16 - 0.10 * cycle) * amp;
        pose.armRight = static_cast<float>(0.16 + 0.10 * cycle) * amp;
        pose.elbowLeft = static_cast<float>(0.18 * amp);
        pose.elbowRight = static_cast<float>(0.18 * amp);
        pose.energy = static_cast<float>(0.45 + 0.26 * std::abs(cycle)) * amp;
        break;
    case MoveKind::Spin:
        pose.rotation = static_cast<float>(0.45 * std::sin(kPi * phase) * amp);
        pose.lean = static_cast<float>(0.22 * cycleSin) * amp;
        pose.armLeft = static_cast<float>(-0.62 * amp);
        pose.armRight = static_cast<float>(0.62 * amp);
        pose.kneeLeft = static_cast<float>(0.24 * amp);
        pose.kneeRight = static_cast<float>(0.24 * amp);
        pose.energy = static_cast<float>(0.62 * amp);
        break;
    case MoveKind::Pose:
        pose.armLeft = static_cast<float>(-0.82 * amp);
        pose.armRight = static_cast<float>(0.82 * amp);
        pose.elbowLeft = static_cast<float>(0.22 * amp);
        pose.elbowRight = static_cast<float>(0.22 * amp);
        pose.lean = static_cast<float>(0.32 * cycle) * amp;
        pose.bob = static_cast<float>(-0.28 * cycle) * amp;
        pose.headNod = static_cast<float>(0.18 * cycleSin) * amp;
        pose.energy = static_cast<float>(0.70 * amp);
        break;
    case MoveKind::BassDip:
    case MoveKind::AccentPose:
        // These are overlays; the fallback below is used only if a caller
        // asks for them as a base movement.
        pose = idlePose();
        pose.move = move;
        break;
    case MoveKind::Freeze:
    case MoveKind::Idle:
    default:
        return idlePose();
    }
    return clampPose(pose);
}

CharacterPose accentPose(MoveKind move, float strength, double beatDuration,
                         const ChoreographyStyle& style) {
    if (move != MoveKind::BassDip && move != MoveKind::AccentPose) {
        return idlePose();
    }
    const double tempo = style.reduceAmplitudeAtFastTempo
                             ? std::clamp(beatDuration / 0.5, 0.52, 1.0)
                             : 1.0;
    const float amp = clampValue(strength * style.intensity * static_cast<float>(tempo),
                                 0.15f, 1.0f);
    CharacterPose pose;
    pose.move = move;
    pose.accented = true;
    pose.impact = 1.0f;
    if (move == MoveKind::BassDip) {
        pose.bob = -0.82f * amp;
        pose.lean = 0.10f * amp;
        pose.kneeLeft = 0.72f * amp;
        pose.kneeRight = 0.66f * amp;
        pose.legLeft = 0.12f * amp;
        pose.legRight = -0.12f * amp;
        pose.hipShift = 0.15f * amp;
        pose.armLeft = -0.42f * amp;
        pose.armRight = 0.42f * amp;
        pose.elbowLeft = 0.30f * amp;
        pose.elbowRight = 0.30f * amp;
        pose.headNod = -0.42f * amp;
        pose.chestTwist = 0.12f * amp;
        pose.energy = 0.95f * amp;
    } else {
        pose.bob = -0.30f * amp;
        pose.lean = 0.16f * amp;
        pose.armLeft = -0.92f * amp;
        pose.armRight = 0.92f * amp;
        pose.elbowLeft = 0.26f * amp;
        pose.elbowRight = 0.26f * amp;
        pose.headNod = 0.36f * amp;
        pose.headTilt = 0.12f * amp;
        pose.chestTwist = 0.20f * amp;
        pose.hipShift = 0.10f * amp;
        pose.kneeLeft = 0.18f * amp;
        pose.kneeRight = 0.18f * amp;
        pose.energy = 0.90f * amp;
    }
    return clampPose(pose);
}

CharacterPose basePoseAt(const BuildContext& context, double time,
                         const BeatLocation& location) {
    if (context.analysis.beats.empty() || time < context.analysis.beats.front().time) {
        return idlePose();
    }
    const int bar = std::max(
        0, context.analysis.beats[static_cast<std::size_t>(location.index)].barIndex);
    const MoveKind currentMove = movementForBar(bar);
    CharacterPose pose;

    // Cross-fade into the next bar before its boundary. At the boundary the
    // new movement is fully active, so the accent pose there is authoritative.
    const int nextBar = bar + 1;
    const double nextBarStart =
        nextBar < static_cast<int>(context.barStart.size())
            ? context.barStart[static_cast<std::size_t>(nextBar)]
            : std::numeric_limits<double>::infinity();
    double blendWindow = 0.0;
    if (nextBar < static_cast<int>(context.barFirstBeat.size())) {
        const int nextFirstBeat = context.barFirstBeat[static_cast<std::size_t>(nextBar)];
        if (nextFirstBeat >= 0) {
            blendWindow = context.beatInterval(nextFirstBeat) *
                          std::clamp(static_cast<double>(context.style.movementBlend),
                                     0.0, 0.75);
        }
    }
    if (std::isfinite(nextBarStart) && blendWindow > 0.001 &&
        time < nextBarStart && time >= nextBarStart - blendWindow) {
        const double u = smoothStep01((time - (nextBarStart - blendWindow)) /
                                      std::max(0.001, blendWindow));
        const MoveKind nextMove = movementForBar(nextBar);
        const int nextFirstBeat = context.barFirstBeat[static_cast<std::size_t>(nextBar)];
        const float nextIntensity =
            movementIntensity(context.analysis, nextFirstBeat, context.style,
                              context.beatInterval(nextFirstBeat));
        const CharacterPose previousPose =
            movementPose(currentMove, context, location, time);
        const CharacterPose nextPose =
            movementPose(nextMove, context, location, time, nextIntensity);
        pose = blendPoses(previousPose, nextPose, static_cast<float>(u));
        pose.blend = static_cast<float>(u);
        pose.move = u < 0.5 ? currentMove : nextMove;
    } else {
        pose = movementPose(currentMove, context, location, time);
        pose.blend = 1.0f;
        pose.move = currentMove;
    }
    return pose;
}

CharacterPose evaluatedPose(const BuildContext& context, double time) {
    if (context.analysis.beats.empty()) {
        return idlePose();
    }
    if (time < context.analysis.beats.front().time - 0.001) {
        return idlePose();
    }

    const BeatLocation location = context.locate(time);
    CharacterPose pose = basePoseAt(context, time, location);

    // Accent overlays. We evaluate the current beat and its immediate
    // neighbours, then choose the strongest active window. At the exact beat
    // time the current window has weight 1, so the stored AccentTarget is
    // reached exactly.
    double bestWeight = 0.0;
    CharacterPose bestTarget;
    MoveKind bestMove = MoveKind::Idle;
    for (int offset = -1; offset <= 1; ++offset) {
        const int index = location.index + offset;
        if (index < 0 || static_cast<std::size_t>(index) >= context.analysis.beats.size()) {
            continue;
        }
        const MoveKind move = context.accentMoveAt(static_cast<std::size_t>(index));
        if (move == MoveKind::Idle) {
            continue;
        }
        const AccentWindow window =
            context.accentWindow(static_cast<std::size_t>(index), move);
        const double weight =
            context.accentWeight(time, static_cast<std::size_t>(index), window);
        if (weight > bestWeight) {
            bestWeight = weight;
            bestMove = move;
            bestTarget = accentPose(
                move, context.accentStrengthAt(static_cast<std::size_t>(index)),
                context.beatInterval(index), context.style);
        }
    }
    if (bestWeight > 0.0) {
        pose = blendPoses(pose, bestTarget, static_cast<float>(bestWeight));
        pose.impact = static_cast<float>(bestWeight);
        pose.accented = bestWeight > 0.2;
        if (bestWeight > 0.25) {
            pose.move = bestMove;
        }
    }

    // Silence fades to a real freeze over a short edge; inside a region there
    // are no fabricated beats or accents.
    const double outside = context.silenceWeight(time);
    if (outside < 1.0) {
        pose = blendPoses(pose, freezePose(), static_cast<float>(1.0 - outside));
        pose.energy *= static_cast<float>(outside);
        pose.impact *= static_cast<float>(outside);
        pose.accented = pose.accented && outside > 0.5;
    }
    pose.phase = static_cast<float>(location.phase);
    if (outside <= 0.001) {
        pose = freezePose();
    }
    return clampPose(pose);
}

CharacterPose interpolateLegacyCue(const DanceCue& cue, double mediaSeconds) {
    CharacterPose pose;
    pose.move = cue.move;
    pose.accented = cue.bassAccent;
    pose.energy = cue.intensity;
    const double phase = std::clamp(
        (mediaSeconds - cue.time) / std::max(1e-6, cue.duration), 0.0, 1.0);
    const double sine = std::sin(phase * kPi);
    const double pulse = std::sin(phase * kPi * 2.0);
    const float intensity = cue.intensity;

    switch (cue.move) {
    case MoveKind::Pulse:
        pose.bob = static_cast<float>(-0.45 * sine * intensity);
        pose.armLeft = static_cast<float>(0.2 * pulse * intensity);
        pose.armRight = static_cast<float>(-0.2 * pulse * intensity);
        break;
    case MoveKind::Bounce:
        pose.bob = static_cast<float>(-0.9 * sine * intensity);
        pose.lean = static_cast<float>(0.18 * pulse * intensity);
        pose.armLeft = static_cast<float>(-0.25 * sine * intensity);
        pose.armRight = static_cast<float>(-0.25 * sine * intensity);
        pose.legLeft = static_cast<float>(0.3 * sine * intensity);
        pose.legRight = static_cast<float>(-0.3 * sine * intensity);
        break;
    case MoveKind::Sway:
        pose.lean = static_cast<float>(0.85 * pulse * intensity);
        pose.armLeft = static_cast<float>(-0.65 * sine * intensity);
        pose.armRight = static_cast<float>(0.7 * sine * intensity);
        pose.bob = static_cast<float>(-0.25 * sine * intensity);
        pose.rotation = static_cast<float>(0.22 * pulse * intensity);
        break;
    case MoveKind::ArmWave:
        pose.armLeft = static_cast<float>((-0.9 + 0.5 * sine) * intensity);
        pose.armRight = static_cast<float>((0.55 - 0.9 * sine) * intensity);
        pose.lean = static_cast<float>(0.22 * pulse * intensity);
        pose.bob = static_cast<float>(-0.3 * sine * intensity);
        break;
    case MoveKind::Spin:
        pose.rotation = static_cast<float>(std::sin(phase * kPi * 2.0) * intensity);
        pose.lean = static_cast<float>(0.28 * pulse * intensity);
        pose.armLeft = static_cast<float>(-0.7 * intensity);
        pose.armRight = static_cast<float>(0.7 * intensity);
        pose.bob = static_cast<float>(-0.2 * sine * intensity);
        break;
    case MoveKind::Pose:
        pose.bob = static_cast<float>(-0.55 * sine * intensity);
        pose.lean = static_cast<float>(0.3 * sine * intensity);
        pose.armLeft = -0.82f * intensity;
        pose.armRight = 0.82f * intensity;
        pose.legLeft = static_cast<float>(0.18 * pulse * intensity);
        pose.legRight = static_cast<float>(-0.18 * pulse * intensity);
        break;
    case MoveKind::HeadNod:
        pose.headNod = static_cast<float>(-0.75 * std::cos(phase * kPi * 2.0) *
                                          intensity);
        pose.headTilt = static_cast<float>(0.1 * pulse * intensity);
        pose.bob = -0.08f * intensity;
        break;
    case MoveKind::BassDip:
        pose.bob = static_cast<float>(-0.75 * (1.0 - phase) * intensity);
        pose.kneeLeft = 0.65f * intensity;
        pose.kneeRight = 0.65f * intensity;
        pose.armLeft = -0.35f * intensity;
        pose.armRight = 0.35f * intensity;
        pose.headNod = -0.4f * intensity;
        break;
    case MoveKind::AccentPose:
        pose.bob = static_cast<float>(-0.30 * (1.0 - phase) * intensity);
        pose.armLeft = -0.9f * intensity;
        pose.armRight = 0.9f * intensity;
        pose.headNod = 0.35f * intensity;
        pose.chestTwist = 0.2f * intensity;
        break;
    case MoveKind::Freeze:
    case MoveKind::Idle:
    default:
        pose = idlePose();
        pose.move = cue.move;
        pose.energy = 0.0f;
        break;
    }
    return clampPose(pose);
}

CharacterPose cubicInterpolate(const MotionKeyframe& previous,
                               const MotionKeyframe& a,
                               const MotionKeyframe& b,
                               const MotionKeyframe& next, float t) {
    const double h = std::max(0.0001, b.time - a.time);
    const double dtPrev = a.time - previous.time;
    const double dtNext = next.time - b.time;
    const double u = clamp01(t);

    auto hermiteScalar = [&](float p0, float p1, float previousValue,
                             float nextValue) {
        double v0 = 0.0;
        double v1 = 0.0;
        if (dtPrev > 1e-7) {
            v0 = (static_cast<double>(p1) - previousValue) /
                 std::max(1e-7, b.time - previous.time);
        } else {
            v0 = (static_cast<double>(p1) - p0) / h;
        }
        if (dtNext > 1e-7) {
            v1 = (static_cast<double>(nextValue) - p0) /
                 std::max(1e-7, next.time - a.time);
        } else {
            v1 = (static_cast<double>(p1) - p0) / h;
        }
        const double m0 = v0 * h;
        const double m1 = v1 * h;
        const double u2 = u * u;
        const double u3 = u2 * u;
        const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
        const double h10 = u3 - 2.0 * u2 + u;
        const double h01 = -2.0 * u3 + 3.0 * u2;
        const double h11 = u3 - u2;
        return static_cast<float>(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1);
    };

    CharacterPose out;
#define BGM_HERMITE_FIELD(field)                                                  \
    out.field = hermiteScalar(a.pose.field, b.pose.field, previous.pose.field,    \
                              next.pose.field);
    BGM_POSE_SCALARS(BGM_HERMITE_FIELD)
#undef BGM_HERMITE_FIELD

    const float previousTurn =
        a.pose.rotation + turnDelta(a.pose.rotation, previous.pose.rotation);
    const float endTurn = a.pose.rotation + turnDelta(a.pose.rotation, b.pose.rotation);
    const float nextTurn = endTurn + turnDelta(b.pose.rotation, next.pose.rotation);
    const double v0 = dtPrev > 1e-7
                          ? (endTurn - previousTurn) / std::max(1e-7, b.time - previous.time)
                          : (endTurn - a.pose.rotation) / h;
    const double v1 = dtNext > 1e-7
                          ? (nextTurn - a.pose.rotation) / std::max(1e-7, next.time - a.time)
                          : (endTurn - a.pose.rotation) / h;
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
    const double h10 = u3 - 2.0 * u2 + u;
    const double h01 = -2.0 * u3 + 3.0 * u2;
    const double h11 = u3 - u2;
    out.rotation = wrapTurn(static_cast<float>(
        h00 * a.pose.rotation + h10 * (v0 * h) + h01 * endTurn + h11 * (v1 * h)));

    out.phase = a.pose.phase + (b.pose.phase - a.pose.phase) * t;
    out.blend = a.pose.blend + (b.pose.blend - a.pose.blend) * t;
    out.impact = a.pose.impact + (b.pose.impact - a.pose.impact) * t;
    out.move = t < 0.5f ? a.pose.move : b.pose.move;
    out.accented = t < 0.5f ? a.pose.accented : b.pose.accented;
    return clampPose(out);
}

} // namespace

Choreography ChoreographyBuilder::build(const AnalysisResult& analysis,
                                        ChoreographyStyle style) const {
    Choreography choreography;
    choreography.beatsPerBar = std::max(2, analysis.beatsPerBar);
    choreography.analyzerVersion = analysis.info.analyzerVersion;
    choreography.duration = analysis.info.duration;
    choreography.bpm = analysis.bpm;
    choreography.smoothed = true;
    if (analysis.beats.empty()) {
        return choreography;
    }

    BuildContext context(analysis, style);
    choreography.duration = context.duration;

    // Legacy cues. They are still useful in the timeline and are the source for
    // the stepped comparison renderer.
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const Beat& beat = analysis.beats[i];
        const double next = i + 1 < analysis.beats.size()
                                ? analysis.beats[i + 1].time
                                : context.duration;
        const double interval = std::max(0.05, next - beat.time);
        const MoveKind accentMove = context.accentMoveAt(i);
        DanceCue cue;
        cue.time = beat.time;
        cue.duration = interval;
        cue.move = accentMove != MoveKind::Idle
                       ? accentMove
                       : movementForBar(std::max(0, beat.barIndex));
        cue.intensity = movementIntensity(analysis, static_cast<int>(i), style, interval);
        if (context.bassAt(i) > 0.05f) {
            cue.intensity = clampValue(cue.intensity + style.bassInfluence, 0.0f, 1.0f);
        }
        cue.beatIndex = static_cast<int>(i);
        cue.barIndex = beat.barIndex;
        cue.bassAccent = context.bassAt(i) > 0.05f;
        cue.targetTime = beat.time;
        const double previous = i > 0 ? analysis.beats[i - 1].time : 0.0;
        const AccentWindow window = context.accentWindow(i, cue.move);
        cue.anticipationTime =
            std::max(previous, beat.time - std::min(window.preparation, interval * 0.8));
        cue.easing = Easing::Cubic;

        bool silent = false;
        for (const SilenceRegion& region : analysis.silences) {
            if (beat.time >= region.start && beat.time <= region.end) {
                silent = true;
                break;
            }
        }
        if (!silent) {
            choreography.cues.push_back(cue);
        }
    }

    // Explicit freeze cues in long silences.
    for (const SilenceRegion& silence : analysis.silences) {
        if (silence.end - silence.start < 0.5) {
            continue;
        }
        DanceCue freeze;
        freeze.time = silence.start;
        freeze.duration = silence.end - silence.start;
        freeze.move = MoveKind::Freeze;
        freeze.intensity = 0.0f;
        freeze.barIndex = -1;
        freeze.beatIndex = -1;
        freeze.targetTime = silence.start;
        freeze.anticipationTime = silence.start;
        choreography.cues.push_back(freeze);
    }

    std::sort(choreography.cues.begin(), choreography.cues.end(),
              [](const DanceCue& a, const DanceCue& b) {
                  if (a.time == b.time) {
                      return static_cast<int>(a.move) < static_cast<int>(b.move);
                  }
                  return a.time < b.time;
              });
    for (std::size_t i = 0; i + 1 < choreography.cues.size(); ++i) {
        const double next = choreography.cues[i + 1].time;
        if (choreography.cues[i].time + choreography.cues[i].duration > next) {
            choreography.cues[i].duration =
                std::max(0.02, next - choreography.cues[i].time);
        }
    }

    // Clips are compact authoring/debug records. Bar clips describe the base
    // movement; accent clips describe the overlays that hit musical events.
    const int maxBar = static_cast<int>(context.barStart.size()) - 1;
    for (int bar = 0; bar <= maxBar; ++bar) {
        const int firstBeat = context.barFirstBeat[static_cast<std::size_t>(bar)];
        if (firstBeat < 0) {
            continue;
        }
        MotionClip clip;
        clip.move = movementForBar(bar);
        clip.start = context.barStart[static_cast<std::size_t>(bar)];
        clip.end = std::min(context.duration,
                            context.barEnd[static_cast<std::size_t>(bar)]);
        const double blendWindow = context.beatInterval(firstBeat) *
                                   std::clamp(static_cast<double>(style.movementBlend),
                                              0.0, 0.75);
        clip.anticipationStart = std::max(0.0, clip.start - blendWindow);
        clip.intensity = movementIntensity(analysis, firstBeat, style,
                                           context.beatInterval(firstBeat));
        clip.barIndex = bar;
        choreography.clips.push_back(clip);
    }
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const MoveKind move = context.accentMoveAt(i);
        if (move == MoveKind::Idle) {
            continue;
        }
        const AccentWindow window = context.accentWindow(i, move);
        MotionClip clip;
        clip.move = move;
        clip.accentTime = analysis.beats[i].time;
        clip.start = std::max(0.0, clip.accentTime - window.preparation);
        clip.end = std::min(context.duration, clip.accentTime + window.end);
        clip.anticipationStart = clip.start;
        clip.intensity = context.accentStrengthAt(i);
        clip.beatIndex = static_cast<int>(i);
        clip.barIndex = analysis.beats[i].barIndex;
        clip.bassAccent = context.bassAt(i) > 0.05f;
        choreography.clips.push_back(clip);
    }
    std::sort(choreography.clips.begin(), choreography.clips.end(),
              [](const MotionClip& a, const MotionClip& b) {
                  if (a.start == b.start) {
                      return a.accentTime < b.accentTime;
                  }
                  return a.start < b.start;
              });

    // Build the sample times that carry all meaningful motion: keyframes on a
    // sub-beat grid, exact beats, bar-transition edges, accent windows and
    // silence boundaries. The sampler never depends on this density: it
    // interpolates between keys at arbitrary media times.
    std::vector<double> times;
    auto addTime = [&](double value) {
        if (!std::isfinite(value)) {
            return;
        }
        times.push_back(std::clamp(value, 0.0, context.duration));
    };

    addTime(0.0);
    addTime(context.duration);
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const double start = analysis.beats[i].time;
        const double interval = context.beatInterval(static_cast<int>(i));
        addTime(start);
        for (int step = 1; step < 8; ++step) {
            addTime(start + interval * (static_cast<double>(step) / 8.0));
        }

        const MoveKind move = context.accentMoveAt(i);
        if (move != MoveKind::Idle) {
            const AccentWindow window = context.accentWindow(i, move);
            addTime(start - window.preparation);
            addTime(start - window.preparation * 0.5);
            addTime(start);
            addTime(start + window.hold);
            addTime(start + window.hold + window.release * 0.5);
            addTime(start + window.hold + window.release);
        }
    }
    for (int bar = 1; bar <= maxBar; ++bar) {
        const int firstBeat = context.barFirstBeat[static_cast<std::size_t>(bar)];
        if (firstBeat < 0) {
            continue;
        }
        const double start = context.barStart[static_cast<std::size_t>(bar)];
        const double blendWindow = context.beatInterval(firstBeat) *
                                   std::clamp(static_cast<double>(style.movementBlend),
                                              0.0, 0.75);
        addTime(start - blendWindow);
        addTime(start - blendWindow * 0.5);
        addTime(start);
    }
    for (const SilenceRegion& silence : analysis.silences) {
        const double fade = std::min(0.18, std::max(0.04, (silence.end - silence.start) * 0.08));
        addTime(silence.start - fade);
        addTime(silence.start);
        addTime(silence.end);
        addTime(silence.end + fade);
    }

    std::sort(times.begin(), times.end());
    // Keep this tolerance much smaller than any musical interval: an accidental
    // merge of a beat keyframe with a nearby transition key would weaken the
    // exact-accent guarantee. Float noise from phase interpolation is ~1e-15.
    times.erase(std::unique(times.begin(), times.end(),
                            [](double a, double b) { return std::abs(a - b) < 1e-9; }),
                times.end());

    choreography.keyframes.reserve(times.size());
    for (double time : times) {
        MotionKeyframe key;
        key.time = time;
        key.pose = evaluatedPose(context, time);
        key.easingToNext = Easing::Cubic;
        key.accent = key.pose.impact > 0.5f || key.pose.accented;
        key.beatIndex = context.locate(time).index;
        choreography.keyframes.push_back(key);
    }

    // Accent targets are computed at the exact beat timestamps. They are also
    // keyframes, so sampling at that timestamp returns this pose exactly.
    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const Beat& beat = analysis.beats[i];
        bool silent = false;
        for (const SilenceRegion& region : analysis.silences) {
            if (beat.time >= region.start && beat.time <= region.end) {
                silent = true;
                break;
            }
        }
        if (silent) {
            continue;
        }
        AccentTarget target;
        target.time = beat.time;
        target.pose = evaluatedPose(context, beat.time);
        target.move = target.pose.move;
        target.beatIndex = static_cast<int>(i);
        target.barIndex = beat.barIndex;
        target.bassAccent = context.bassAt(i) > 0.05f;
        target.downbeat = beat.downbeat;
        target.strength = context.accentStrengthAt(i);
        choreography.accents.push_back(target);
    }

    return choreography;
}

const DanceCue* ChoreographySampler::activeCue(const Choreography& choreography,
                                               double mediaSeconds) const {
    if (choreography.cues.empty()) {
        return nullptr;
    }
    const auto it = std::upper_bound(
        choreography.cues.begin(), choreography.cues.end(), mediaSeconds,
        [](double time, const DanceCue& cue) { return time < cue.time; });
    if (it == choreography.cues.begin()) {
        return nullptr;
    }
    const DanceCue& cue = *(it - 1);
    if (mediaSeconds <= cue.time + cue.duration + 1e-9) {
        return &cue;
    }
    return nullptr;
}

MotionSample ChoreographySampler::sampleWithDebug(const Choreography& choreography,
                                                  double mediaSeconds) const {
    MotionSample sample;
    if (choreography.keyframes.empty()) {
        if (!choreography.cues.empty()) {
            sample.pose = sampleStepped(choreography, mediaSeconds);
            sample.move = sample.pose.move;
            sample.accent = sample.pose.accented;
        } else {
            sample.pose = idlePose();
        }
        return sample;
    }

    const auto& keys = choreography.keyframes;
    if (mediaSeconds <= keys.front().time + kTiny) {
        sample.keyframeIndex = 0;
        sample.nextKeyframeIndex = keys.size() > 1 ? 1 : 0;
        sample.keyframeTime = keys.front().time;
        sample.nextKeyframeTime = sample.nextKeyframeIndex == 0
                                      ? keys.front().time
                                      : keys[1].time;
        sample.pose = keys.front().pose;
        sample.move = sample.pose.move;
        sample.accent = sample.pose.accented;
        sample.movementBlend = sample.pose.blend;
        return sample;
    }
    if (mediaSeconds >= keys.back().time - kTiny) {
        sample.keyframeIndex = static_cast<int>(keys.size()) - 1;
        sample.nextKeyframeIndex = sample.keyframeIndex;
        sample.keyframeTime = keys.back().time;
        sample.nextKeyframeTime = keys.back().time;
        sample.pose = keys.back().pose;
        sample.move = sample.pose.move;
        sample.accent = sample.pose.accented;
        sample.movementBlend = sample.pose.blend;
        return sample;
    }

    const auto it = std::upper_bound(
        keys.begin(), keys.end(), mediaSeconds,
        [](double value, const MotionKeyframe& key) { return value < key.time; });
    const int index = static_cast<int>(it - keys.begin()) - 1;
    sample.keyframeIndex = index;
    sample.nextKeyframeIndex = std::min(index + 1, static_cast<int>(keys.size()) - 1);
    sample.keyframeTime = keys[static_cast<std::size_t>(index)].time;
    sample.nextKeyframeTime = keys[static_cast<std::size_t>(sample.nextKeyframeIndex)].time;
    const double duration = std::max(1e-9, sample.nextKeyframeTime - sample.keyframeTime);
    const double local = (mediaSeconds - sample.keyframeTime) / duration;
    sample.segmentT = static_cast<float>(clamp01(local));
    sample.easing = keys[static_cast<std::size_t>(index)].easingToNext;

    const MotionKeyframe& a = keys[static_cast<std::size_t>(index)];
    const MotionKeyframe& b = keys[static_cast<std::size_t>(sample.nextKeyframeIndex)];

    if (std::abs(mediaSeconds - a.time) < 1e-6) {
        sample.pose = a.pose;
    } else if (sample.easing == Easing::Cubic) {
        const MotionKeyframe& previous =
            index > 0 ? keys[static_cast<std::size_t>(index - 1)] : a;
        const MotionKeyframe& next =
            sample.nextKeyframeIndex + 1 < static_cast<int>(keys.size())
                ? keys[static_cast<std::size_t>(sample.nextKeyframeIndex + 1)]
                : b;
        sample.pose = cubicInterpolate(previous, a, b, next, sample.segmentT);
    } else {
        const float u = applyEasing(sample.easing, sample.segmentT);
        sample.pose = blendPoses(a.pose, b.pose, u);
    }

    sample.move = sample.pose.move;
    sample.accent = sample.pose.accented;
    sample.movementBlend = sample.pose.blend;
    return sample;
}

CharacterPose ChoreographySampler::sample(const Choreography& choreography,
                                          double mediaSeconds) const {
    return sampleWithDebug(choreography, mediaSeconds).pose;
}

CharacterPose ChoreographySampler::sampleStepped(const Choreography& choreography,
                                                 double mediaSeconds) const {
    const DanceCue* cue = activeCue(choreography, mediaSeconds);
    if (!cue) {
        return idlePose();
    }
    return interpolateLegacyCue(*cue, mediaSeconds);
}

} // namespace bgmedia
