#include "bgmedia/mix/MixPlanner.hpp"

#include "bgmedia/mix/Crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bgmedia::mix {
namespace {

double clampDuration(double value, const PlannerConfig& config) {
    if (config.maxCrossfadeSeconds <= 0.0) {
        return std::max(0.0, value);
    }
    return std::clamp(value, std::max(0.0, config.minCrossfadeSeconds),
                      config.maxCrossfadeSeconds);
}

std::string format(const char* pattern, double a, double b = 0.0,
                   double c = 0.0) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, a, b, c);
    return buffer;
}

std::string format(const char* pattern, double a, double b, double c,
                   double d) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, a, b, c, d);
    return buffer;
}

double bpmAt(const AnalysisResult& analysis, double time, double fallback) {
    double best = fallback;
    for (const TempoSegment& segment : analysis.tempoSegments) {
        if (time >= segment.start - 0.25 && time <= segment.end + 0.25) {
            return segment.bpm > 0.0 ? segment.bpm : fallback;
        }
    }
    if (!analysis.tempoSegments.empty()) {
        double distance = std::numeric_limits<double>::max();
        for (const TempoSegment& segment : analysis.tempoSegments) {
            const double d = std::min(std::abs(time - segment.start),
                                      std::abs(time - segment.end));
            if (d < distance) {
                distance = d;
                best = segment.bpm > 0.0 ? segment.bpm : fallback;
            }
        }
    }
    return best > 0.0 ? best : fallback;
}

bool downbeatsUsable(const AnalysisResult& analysis, const PlannerConfig& config,
                     int* beatsPerBar) {
    const bool meter = analysis.meterConfidence >= 0.45f;
    const bool down = analysis.downbeatConfidence >= 0.45f;
    const int bpb = analysis.beatsPerBar >= 2 ? analysis.beatsPerBar : 4;
    if (beatsPerBar) {
        *beatsPerBar = bpb;
    }
    return config.trustDownbeats && meter && down;
}

int indexNear(const std::vector<Beat>& beats, double time) {
    if (beats.empty()) {
        return -1;
    }
    const auto it = std::lower_bound(
        beats.begin(), beats.end(), time,
        [](const Beat& beat, double value) { return beat.time < value; });
    int right = static_cast<int>(it - beats.begin());
    int left = right - 1;
    if (right >= static_cast<int>(beats.size())) {
        right = static_cast<int>(beats.size()) - 1;
    }
    if (left < 0) {
        return right;
    }
    return std::abs(beats[static_cast<std::size_t>(left)].time - time) <=
                   std::abs(beats[static_cast<std::size_t>(right)].time - time)
               ? left
               : right;
}

bool findGridPoint(const AnalysisResult& analysis, double target,
                   bool preferDownbeat, double* time, bool* downbeat,
                   double* errorMs) {
    if (analysis.beats.empty()) {
        return false;
    }
    const int near = indexNear(analysis.beats, target);
    if (near < 0) {
        return false;
    }
    int chosen = near;
    if (preferDownbeat) {
        int best = -1;
        double bestDistance = std::numeric_limits<double>::max();
        const int start = std::max(0, near - 8);
        const int end = std::min(static_cast<int>(analysis.beats.size()) - 1,
                                 near + 8);
        for (int i = start; i <= end; ++i) {
            const Beat& beat = analysis.beats[static_cast<std::size_t>(i)];
            if (!beat.downbeat) {
                continue;
            }
            const double d = std::abs(beat.time - target);
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        if (best >= 0) {
            chosen = best;
        }
    }
    const Beat& beat = analysis.beats[static_cast<std::size_t>(chosen)];
    if (time) {
        *time = beat.time;
    }
    if (downbeat) {
        *downbeat = beat.downbeat;
    }
    if (errorMs) {
        *errorMs = (beat.time - target) * 1000.0;
    }
    return true;
}

float compositeConfidence(const AnalysisResult& a, const AnalysisResult& b,
                          bool useDownbeats) {
    if (a.empty() || b.empty()) {
        return 0.0f;
    }
    const float bpm = std::min(a.bpmConfidence, b.bpmConfidence);
    const float phase = std::min(a.beatPhaseConfidence, b.beatPhaseConfidence);
    const float meter = std::min(a.meterConfidence, b.meterConfidence);
    const float downbeat =
        std::min(a.downbeatConfidence, b.downbeatConfidence);
    const float active =
        static_cast<float>(std::min(a.activeRatio, b.activeRatio));
    float value = 0.42f * bpm + 0.25f * phase + 0.18f * meter +
                  0.10f * (useDownbeats ? downbeat : bpm) + 0.05f * active;
    if (!a.tempoStable || !b.tempoStable) {
        value *= 0.72f;
    }
    if (a.octaveAmbiguous || b.octaveAmbiguous) {
        value *= 0.80f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

double sectionCue(const AnalysisResult& analysis, SectionKind kind,
                  double fallback) {
    for (const Section& section : analysis.sections) {
        if (section.kind == kind) {
            return section.start;
        }
    }
    return fallback;
}

double outroStart(const AnalysisResult& analysis) {
    for (auto it = analysis.sections.rbegin(); it != analysis.sections.rend();
         ++it) {
        if (it->kind == SectionKind::Outro) {
            return it->start;
        }
    }
    return analysis.info.duration;
}

} // namespace

MixPlanner::MixPlanner(PlannerConfig config) : config_(config) {}

double MixPlanner::nearestBeat(const AnalysisResult& analysis, double time,
                               bool* downbeat, double* errorMs) {
    double beatTime = 0.0;
    bool isDown = false;
    double error = 0.0;
    if (!findGridPoint(analysis, time, false, &beatTime, &isDown, &error)) {
        return -1.0;
    }
    if (downbeat) {
        *downbeat = isDown;
    }
    if (errorMs) {
        *errorMs = error;
    }
    return beatTime;
}

double MixPlanner::suggestDuration(const AnalysisResult& a,
                                   const AnalysisResult& b) const {
    double duration = config_.defaultCrossfadeSeconds;
    const double outro = outroStart(a);
    if (outro < a.info.duration - 0.25) {
        const double length = a.info.duration - outro;
        if (length >= 1.5) {
            duration = length;
            if (config_.preferPhraseBoundaries) {
                // Outro lengths are often a bar shorter than the musical fade.
                duration *= 1.15;
            }
        }
    }
    if (!b.sections.empty() && b.info.duration < 12.0) {
        duration = std::min(duration, std::max(2.0, b.info.duration * 0.35));
    }
    return clampDuration(duration, config_);
}

MixPlan MixPlanner::plan(const AnalysisResult& a, const AnalysisResult& b,
                         const PlanRequest& request) const {
    MixPlan plan;
    plan.requestedType = request.type;
    plan.type = request.type;
    plan.from = DeckId::A;
    plan.to = DeckId::B;
    plan.bpmA = a.bpm;
    plan.bpmB = b.bpm;
    plan.targetBpm = b.bpm;

    if (request.type == TransitionType::Off) {
        plan.valid = false;
        plan.fallbackReason = "automatic mixing is off";
        return plan;
    }
    if (a.empty() || b.empty()) {
        plan.valid = false;
        plan.fallbackReason = "one of the tracks has no usable analysis";
        return plan;
    }

    const bool downbeats = downbeatsUsable(a, config_, nullptr) &&
                           downbeatsUsable(b, config_, nullptr);
    plan.confidence = compositeConfidence(a, b, downbeats);

    double duration = request.requestedDuration > 0.0
                          ? request.requestedDuration
                          : suggestDuration(a, b);

    // Snap an automatic length to whole bars so the outgoing beat phase stays
    // musically stable. Manual lengths are honoured exactly.
    double localBpmA = bpmAt(a, request.currentPositionA > 0.0
                                   ? request.currentPositionA
                                   : std::max(0.0, a.info.duration - duration),
                             a.bpm);
    if (localBpmA <= 0.0) {
        localBpmA = a.bpm > 0.0 ? a.bpm : 120.0;
    }
    const int bpb = a.beatsPerBar >= 2 ? a.beatsPerBar : 4;
    if (request.requestedDuration <= 0.0 && plan.confidence >= config_.minConfidence) {
        const double barSeconds = static_cast<double>(bpb) * 60.0 / localBpmA;
        int bars = static_cast<int>(std::llround(duration / barSeconds));
        bars = std::clamp(bars, 1, 32);
        if (downbeats && bars % 2 != 0) {
            ++bars;  // keep the mid-point of the crossfade on a downbeat
        }
        duration = bars * barSeconds;
    }
    duration = clampDuration(duration, config_);

    // ------------------------------------------------------------------
    // Transition window in the outgoing track.
    // ------------------------------------------------------------------
    double startA = 0.0;
    double endA = 0.0;
    if (request.currentPositionA >= 0.0) {
        startA = std::min(request.currentPositionA, a.info.duration);
        endA = std::min(a.info.duration, startA + duration);
        duration = std::max(0.0, endA - startA);
    } else {
        endA = a.info.duration;
        // Prefer the end of the musical material rather than trailing silence.
        for (const SilenceRegion& silence : a.silences) {
            if (silence.end >= a.info.duration - 0.35 &&
                silence.start >= a.info.duration - 8.0) {
                endA = std::min(endA, silence.start + 0.20);
            }
        }
        startA = std::max(0.0, endA - duration);
    }

    if (plan.confidence >= config_.minConfidence && !a.beats.empty()) {
        double aligned = 0.0;
        double error = 0.0;
        if (findGridPoint(a, startA, downbeats, &aligned, nullptr, &error) &&
            aligned >= startA - 0.10 && aligned <= startA + 0.80) {
            startA = aligned;
            endA = std::min(a.info.duration, startA + duration);
            if (endA - startA > 0.25) {
                plan.reasons.push_back(format(
                    downbeats
                        ? "A grid: transition starts on a downbeat at %.3f s (%.1f ms from the requested point)"
                        : "A grid: transition starts on a beat at %.3f s (%.1f ms from the requested point)",
                    startA, std::abs(error)));
                plan.beatAligned = true;
                plan.downbeatAligned = downbeats;
                plan.alignmentErrorMs = std::abs(error);
            }
        }
    }
    plan.localBpmA = localBpmA;
    plan.duration = endA - startA;
    plan.transitionStartA = startA;
    plan.transitionEndA = endA;

    // ------------------------------------------------------------------
    // Entry point in the incoming track.
    // ------------------------------------------------------------------
    const double intro = sectionCue(b, SectionKind::Intro,
                                    b.beats.empty() ? 0.2 : b.beats.front().time);
    double cue = std::clamp(intro, 0.0, std::max(0.0, b.info.duration - 0.5));
    double startB = cue;
    bool bDownbeat = false;
    double bErrorMs = 0.0;
    if (plan.confidence >= config_.minConfidence && !b.beats.empty()) {
        double aligned = 0.0;
        if (findGridPoint(b, cue, downbeats, &aligned, &bDownbeat, &bErrorMs)) {
            if (aligned >= 0.0 && aligned < b.info.duration - 0.2) {
                startB = aligned;
                plan.reasons.push_back(format(
                    bDownbeat
                        ? "B entry: downbeat at %.3f s (%.1f ms from the intro cue)"
                        : "B entry: beat at %.3f s (%.1f ms from the intro cue)",
                    startB, std::abs(bErrorMs)));
                plan.beatAligned = true;
                plan.downbeatAligned = plan.downbeatAligned || bDownbeat;
                plan.alignmentErrorMs = std::max(plan.alignmentErrorMs,
                                                 std::abs(bErrorMs));
            }
        }
    }
    startB = std::clamp(startB, 0.0, std::max(0.0, b.info.duration - 0.25));

    // A transition cannot outlive either prepared track. A short incoming
    // edit must shorten the fade instead of leaving silence halfway through.
    const double availableB = b.info.duration - startB;
    const double availableA = plan.transitionEndA - plan.transitionStartA;
    const double fitted =
        std::min(plan.duration, std::min(availableA, availableB));
    if (fitted < plan.duration - 1e-6) {
        plan.duration = std::max(0.0, fitted);
        plan.transitionEndA = plan.transitionStartA + plan.duration;
        plan.reasons.push_back(format(
            "Shortened to %.2f s: the shortest remaining audio window is %.2f s",
            plan.duration, fitted));
    }
    plan.startB = startB;
    plan.endB = std::min(b.info.duration, startB + plan.duration);
    plan.handoverProgress = 0.5;

    // ------------------------------------------------------------------
    // Type-specific decisions.
    // ------------------------------------------------------------------
    const bool lowConfidence = plan.confidence < config_.minConfidence;
    const bool unimplemented =
        transitionStatus(request.type) == ImplementationStatus::NotImplemented;
    bool ramp = false;
    double ratio = 1.0;
    double rampSeconds = 0.0;

    auto fallback = [&](const std::string& reason) {
        plan.type = TransitionType::Crossfade;
        plan.fallbackReason = reason;
        plan.tempoCurve.clear();
        plan.experimental = false;
        plan.pitchShiftWarning = false;
        plan.targetBpm = b.bpm;
        plan.tempoRampPercent = 0.0;
    };

    if (unimplemented) {
        fallback(std::string(transitionTypeName(request.type)) +
                 " is not implemented in BG Mix 0.1");
    } else if (request.type == TransitionType::DirectCut) {
        plan.type = TransitionType::DirectCut;
        plan.duration = 0.0;
        // A cut has no fade window. An executed cut happens at the current
        // position; a preview reports the end of the planned musical window,
        // so automatic scheduling cuts there (before trailing silence) rather
        // than at the physical file end.
        const double cutPoint = request.currentPositionA >= 0.0 ? startA : endA;
        plan.transitionStartA = cutPoint;
        plan.transitionEndA = cutPoint;
        plan.reasons.push_back("Direct cut: no crossfade, incoming deck starts on its marker");
    } else if (request.type == TransitionType::SmartCrossfade) {
        if (lowConfidence) {
            fallback(format(
                "analysis confidence %.2f is below the %.2f threshold; using a safe equal-power crossfade",
                plan.confidence, config_.minConfidence));
        } else {
            plan.type = TransitionType::SmartCrossfade;
            plan.reasons.push_back(format(
                "Smart length: %.2f s from BG Beat sections/confidence", plan.duration));
        }
    } else if (request.type == TransitionType::Beatmatched) {
        if (lowConfidence || a.beats.empty() || b.beats.empty()) {
            fallback("beat grid is missing or low-confidence; using a plain crossfade");
        } else {
            plan.type = TransitionType::Beatmatched;
            plan.reasons.push_back("Beatmatched: both decks start on their local beat grid");
        }
    } else if (request.type == TransitionType::TempoRamp) {
        const double deltaPercent = std::abs(b.bpm - localBpmA) /
                                    std::max(1.0, localBpmA) * 100.0;
        rampSeconds = static_cast<double>(bpb) * 60.0 / localBpmA *
                      std::max(1.0, config_.tempoRampBars);
        rampSeconds = std::min(rampSeconds, plan.duration);
        ratio = b.bpm > 0.0 ? b.bpm / std::max(1.0, localBpmA) : 1.0;
        // The rate curve holds a small plateau above the requested delta and
        // settles back to the target at the end. That plateau is not cosmetic:
        // it makes the time-integral of the curve equal the target ratio, so
        // the outgoing deck arrives at B's tempo with the same beat phase.
        constexpr double kRampEdge = 0.12;
        const double plateau =
            1.0 + (kRampEdge * 0.5) / (1.0 - 1.5 * kRampEdge);
        const double peakPercent =
            std::abs(ratio - 1.0) * plateau * 100.0;
        if (!config_.allowTempoRamp) {
            fallback("Tempo Ramp is disabled in preferences");
        } else if (lowConfidence) {
            fallback(format("tempo confidence %.2f is too low for a tempo ramp", plan.confidence));
        } else if (!a.tempoStable || a.hasTempoChange()) {
            fallback("outgoing tempo is not stable enough for a tempo ramp");
        } else if (deltaPercent < 0.05) {
            fallback("tracks already share the same tempo");
        } else if (deltaPercent > config_.maxTempoDeltaPercent) {
            fallback(format("BPM difference %.1f%% exceeds the configured %.1f%% limit",
                            deltaPercent, config_.maxTempoDeltaPercent));
        } else if (plan.duration < 2.0) {
            fallback("transition is too short for a musical tempo ramp");
        } else {
            ramp = true;
            plan.type = TransitionType::TempoRamp;
            plan.experimental = true;
            plan.pitchShiftWarning = true;
            plan.tempoRampPercent = deltaPercent;
            plan.targetBpm = b.bpm;
            plan.reasons.push_back(format(
                "Tempo Ramp: %.1f -> %.1f BPM over %.1f s (%.0f%%)",
                localBpmA, b.bpm, rampSeconds, deltaPercent));
            plan.reasons.push_back(format(
                "Phase-preserving rate curve: mean speed matches the target; "
                "transient peak %.1f%%",
                peakPercent));
            plan.reasons.push_back(
                "Experimental: linear resampling changes pitch (no WSOLA yet)");
        }
    } else {
        plan.type = TransitionType::Crossfade;
        plan.reasons.push_back(format("Equal-power crossfade over %.2f s", plan.duration));
    }

    // ------------------------------------------------------------------
    // Tempo automation.
    // ------------------------------------------------------------------
    if (ramp) {
        constexpr int kPoints = 33;
        constexpr double kEdge = 0.12;
        const double plateau = 1.0 + (kEdge * 0.5) / (1.0 - 1.5 * kEdge);
        auto shape = [&](double t) {
            // g(0)=0, g(1)=1, zero slope at both ends, and integral(g)=1 over
            // [0,1]. The integral is what keeps the outgoing deck in musical
            // phase: a plain ramp from 1 to the target would only advance half
            // the beats the incoming track expects.
            t = std::clamp(t, 0.0, 1.0);
            if (t < kEdge) {
                return smoothStep(t / kEdge);
            }
            if (t > 1.0 - kEdge) {
                return 1.0 + (plateau - 1.0) *
                                 (1.0 - smoothStep((t - (1.0 - kEdge)) / kEdge));
            }
            return plateau;
        };
        plan.tempoCurve.reserve(kPoints + 1);
        for (int i = 0; i < kPoints; ++i) {
            const double t = static_cast<double>(i) / (kPoints - 1);
            const double rate = 1.0 + (ratio - 1.0) * shape(t);
            const double time = rampSeconds * t;
            plan.tempoCurve.push_back(
                {time, rate, localBpmA * rate});
        }
        if (rampSeconds < plan.duration) {
            plan.tempoCurve.push_back(
                {plan.duration, ratio, localBpmA * ratio});
        }
    } else {
        plan.tempoCurve.push_back({0.0, 1.0, localBpmA});
        if (plan.duration > 0.0) {
            plan.tempoCurve.push_back({plan.duration, 1.0, localBpmA});
        }
    }

    // Equal-power preview curves. The engine computes the live gains and adds
    // correlation-aware headroom and the output limiter.
    constexpr int kGainPoints = 17;
    plan.gainCurveOut.reserve(kGainPoints);
    plan.gainCurveIn.reserve(kGainPoints);
    for (int i = 0; i < kGainPoints; ++i) {
        const double t = static_cast<double>(i) / (kGainPoints - 1);
        const double time = plan.duration * t;
        plan.gainCurveOut.push_back({time, equalPowerOut(t)});
        plan.gainCurveIn.push_back({time, equalPowerIn(t)});
    }

    if (lowConfidence) {
        plan.reasons.push_back(format(
            "Analysis confidence %.2f is low; the planner chose conservative values",
            plan.confidence));
    }
    if (plan.fallbackReason.empty()) {
        plan.reasons.push_back(format("Plan confidence %.2f", plan.confidence));
    } else {
        plan.reasons.push_back("Fallback: " + plan.fallbackReason);
    }
    plan.valid = plan.duration >= 0.0 && plan.duration <= a.info.duration &&
                 plan.startB < b.info.duration;
    if (!plan.valid) {
        plan.fallbackReason = "transition window does not fit the loaded tracks";
    }
    return plan;
}

} // namespace bgmedia::mix
