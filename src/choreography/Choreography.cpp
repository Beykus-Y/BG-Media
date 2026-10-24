#include "bgmedia/choreography/Choreography.hpp"

#include "analysis/Dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace bgmedia {
namespace {

MoveKind cycleMove(std::size_t barIndex) {
    switch (barIndex % 4) {
    case 0:
        return MoveKind::ArmWave;
    case 1:
        return MoveKind::Sway;
    case 2:
        return MoveKind::Bounce;
    default:
        return MoveKind::Spin;
    }
}

float cueIntensity(const Beat& beat, const AnalysisResult& analysis) {
    float intensity = 0.35f + 0.55f * std::clamp(beat.strength, 0.0f, 1.0f);
    if (beat.downbeat) {
        intensity += 0.08f;
    }
    intensity *= 0.75f + 0.25f * std::clamp(beat.confidence, 0.0f, 1.0f);
    (void)analysis;
    return std::clamp(intensity, 0.1f, 1.0f);
}

DanceCue makeCueFromBeat(const Beat& beat, double duration, MoveKind move,
                         float intensity, bool accent, int beatIndex) {
    DanceCue cue;
    cue.time = beat.time;
    cue.duration = std::max(0.02, duration);
    cue.move = move;
    cue.intensity = intensity;
    cue.beatIndex = beatIndex;
    cue.barIndex = beat.barIndex;
    cue.bassAccent = accent;
    return cue;
}

} // namespace

Choreography ChoreographyBuilder::build(const AnalysisResult& analysis,
                                        ChoreographyStyle style) const {
    Choreography choreography;
    choreography.beatsPerBar = std::max(2, analysis.beatsPerBar);
    choreography.analyzerVersion = analysis.info.analyzerVersion;

    if (analysis.beats.empty()) {
        return choreography;
    }

    const int beatsPerBar = choreography.beatsPerBar;
    std::vector<int> accentBeats(analysis.beats.size(), 0);
    for (const BassAccent& accent : analysis.bassAccents) {
        if (accent.nearestBeat >= 0 &&
            static_cast<std::size_t>(accent.nearestBeat) < analysis.beats.size()) {
            accentBeats[static_cast<std::size_t>(accent.nearestBeat)] = 1;
        }
    }

    // Silence gaps become freezes rather than fabricated dance beats.
    auto silenceAt = [&](double time) {
        for (const SilenceRegion& silence : analysis.silences) {
            if (time >= silence.start && time <= silence.end) {
                return true;
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
        const Beat& beat = analysis.beats[i];
        if (silenceAt(beat.time)) {
            continue;
        }

        const double nextBeatTime =
            i + 1 < analysis.beats.size() ? analysis.beats[i + 1].time
                                          : analysis.info.duration;
        const double beatDuration = std::max(0.05, nextBeatTime - beat.time);

        MoveKind move = cycleMove(static_cast<std::size_t>(std::max(0, beat.barIndex)));
        if (beat.downbeat) {
            move = MoveKind::Pose;
        } else if (beatsPerBar == 4 && beat.beatInBar == 2) {
            move = MoveKind::Bounce;
        }

        float intensity = cueIntensity(beat, analysis);
        if (accentBeats[i] != 0) {
            intensity = std::clamp(intensity + style.bassInfluence, 0.0f, 1.0f);
        }
        intensity = std::clamp(intensity * std::clamp(style.intensity, 0.0f, 2.0f),
                               0.05f, 1.0f);

        choreography.cues.push_back(
            makeCueFromBeat(beat, beatDuration, move, intensity,
                            accentBeats[i] != 0, static_cast<int>(i)));
    }

    // Add explicit freezes at the start of long silence regions so a renderer
    // can settle instead of holding the last dance frame.
    for (const SilenceRegion& silence : analysis.silences) {
        if (silence.end - silence.start < 0.5) {
            continue;
        }
        DanceCue freeze;
        freeze.time = silence.start;
        freeze.duration = silence.end - silence.start;
        freeze.move = MoveKind::Freeze;
        freeze.intensity = 0.0f;
        freeze.bassAccent = false;
        freeze.barIndex = -1;
        freeze.beatIndex = -1;
        choreography.cues.push_back(freeze);
    }

    std::sort(choreography.cues.begin(), choreography.cues.end(),
              [](const DanceCue& a, const DanceCue& b) {
                  if (a.time == b.time) {
                      return a.move < b.move;
                  }
                  return a.time < b.time;
              });

    // Trim overlaps caused by freezes inserted into a beat cue.
    for (std::size_t i = 0; i + 1 < choreography.cues.size(); ++i) {
        const double next = choreography.cues[i + 1].time;
        if (choreography.cues[i].time + choreography.cues[i].duration > next) {
            choreography.cues[i].duration = std::max(0.02, next - choreography.cues[i].time);
        }
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

CharacterPose ChoreographySampler::sample(const Choreography& choreography,
                                          double mediaSeconds) const {
    CharacterPose pose;
    const DanceCue* cue = activeCue(choreography, mediaSeconds);
    if (!cue) {
        return pose;
    }

    pose.move = cue->move;
    pose.accented = cue->bassAccent;
    pose.energy = cue->intensity;
    const double phase = std::clamp(
        (mediaSeconds - cue->time) / std::max(1e-6, cue->duration), 0.0, 1.0);
    const double sine = std::sin(phase * dsp::kPi);
    const double pulse = std::sin(phase * dsp::kPi * 2.0);
    const float intensity = cue->intensity;

    switch (cue->move) {
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
        pose.rotation = static_cast<float>(std::sin(phase * dsp::kPi * 2.0) * intensity);
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
    case MoveKind::Freeze:
    case MoveKind::Idle:
    default:
        pose.bob = 0.0f;
        pose.energy = 0.0f;
        break;
    }
    return pose;
}

} // namespace bgmedia
