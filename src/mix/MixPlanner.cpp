#include "bgmedia/mix/MixPlanner.hpp"

#include "bgmedia/mix/Crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bgmedia::mix {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

double musicalEnd(const AnalysisResult& analysis) {
    double end = analysis.info.duration;
    for (const SilenceRegion& silence : analysis.silences) {
        if (silence.end >= analysis.info.duration - 0.35 &&
            silence.start >= analysis.info.duration - 8.0) {
            end = std::min(end, silence.start + 0.20);
        }
    }
    return end;
}

double sectionEnergyAt(const AnalysisResult& analysis, double time) {
    for (const Section& section : analysis.sections) {
        if (time >= section.start - 0.05 && time <= section.end + 0.05) {
            return section.energy;
        }
    }
    return 0.5;
}

bool sectionStartsNear(const AnalysisResult& analysis, double time,
                       double tolerance) {
    for (const Section& section : analysis.sections) {
        if (section.kind != SectionKind::Unknown &&
            std::abs(section.start - time) <= tolerance) {
            return true;
        }
    }
    return false;
}

double accentStrengthNear(const AnalysisResult& analysis, double time,
                          double tolerance) {
    double best = 0.0;
    for (const BassAccent& accent : analysis.bassAccents) {
        if (std::abs(accent.time - time) <= tolerance) {
            best = std::max(best, static_cast<double>(accent.strength) *
                                      accent.confidence);
        }
    }
    return best;
}

float phraseConfidenceAt(const AnalysisResult& analysis, double time,
                         int phraseBars) {
    if (analysis.beats.empty()) {
        return 0.0f;
    }
    const int index = indexNear(analysis.beats, time);
    if (index < 0) {
        return 0.0f;
    }
    const Beat& beat = analysis.beats[static_cast<std::size_t>(index)];
    if (std::abs(beat.time - time) > 0.08 || !beat.downbeat) {
        return 0.0f;
    }
    const int bars = std::max(2, phraseBars);
    const bool periodic = beat.barIndex % bars == 0;
    const double energy = sectionEnergyAt(analysis, beat.time);
    const bool novelty = sectionStartsNear(analysis, beat.time, 0.08);
    const double accent = accentStrengthNear(analysis, beat.time, 0.12);
    double score = 0.30 * static_cast<double>(beat.confidence) +
                   0.20 * static_cast<double>(beat.strength) +
                   0.20 * energy + 0.15 * accent;
    if (periodic) {
        score += 0.15;
    }
    if (novelty) {
        score += 0.10;
    }
    return static_cast<float>(std::clamp(score, 0.0, 1.0));
}

struct PhraseCue {
    double time = 0.0;
    bool downbeat = false;
    float confidence = 0.0f;
    bool phrase = false;
    bool found = false;
};

// Picks a phrase boundary near `target`. A boundary is a downbeat whose bar
// index is a multiple of the phrase length, so it works for 3/4 through 12/8
// without assuming a four-beat bar.
PhraseCue findPhraseCue(const AnalysisResult& analysis, double target,
                        int phraseBars, double tolerance) {
    PhraseCue result;
    if (analysis.beats.empty()) {
        return result;
    }
    const int bars = std::max(2, phraseBars);
    double bestDistance = tolerance;
    for (const Beat& beat : analysis.beats) {
        if (!beat.downbeat || beat.barIndex % bars != 0) {
            continue;
        }
        const double distance = std::abs(beat.time - target);
        if (distance > bestDistance) {
            continue;
        }
        const float confidence = phraseConfidenceAt(analysis, beat.time, bars);
        if (!result.found || distance < bestDistance ||
            confidence > result.confidence) {
            bestDistance = distance;
            result.time = beat.time;
            result.downbeat = true;
            result.confidence = confidence;
            result.phrase = true;
            result.found = true;
        }
    }
    if (result.found) {
        return result;
    }
    // No phrase boundary in range: report the nearest downbeat/beat instead so
    // the caller can explain the fallback instead of silently misaligning.
    bool down = false;
    double error = 0.0;
    double time = 0.0;
    if (findGridPoint(analysis, target, true, &time, &down, &error)) {
        result.time = time;
        result.downbeat = down;
        result.confidence = phraseConfidenceAt(analysis, time, bars);
        result.phrase = false;
        result.found = true;
    }
    return result;
}

struct TempoMatch {
    bool compatible = false;
    bool matched = false;  // tempo ratios differ from 1.0
    double targetBpm = 0.0;
    double ratioA = 1.0;
    double ratioB = 1.0;
    double deltaPercent = 0.0;
    std::string reason;
};

TempoMatch matchTempo(const AnalysisResult& b, double localBpmA,
                      const PlannerConfig& config, bool allowOctave) {
    TempoMatch match;
    if (localBpmA <= 0.0 || b.bpm <= 0.0) {
        match.reason = "tempo is unknown";
        return match;
    }
    match.targetBpm = b.bpm;
    match.ratioA = b.bpm / localBpmA;
    match.ratioB = 1.0;
    match.deltaPercent = std::abs(match.ratioA - 1.0) * 100.0;
    const double limit = std::max(0.05, config.maxTempoDeltaPercent);

    auto consider = [&](double candidate) {
        if (candidate <= 0.0) {
            return;
        }
        const double ratioA = candidate / localBpmA;
        const double ratioB = candidate / b.bpm;
        if (ratioA < 0.5 || ratioA > 2.0 || ratioB < 0.75 || ratioB > 1.33) {
            return;
        }
        const double delta = std::abs(ratioA - 1.0) * 100.0;
        if (delta > limit) {
            return;
        }
        if (!match.compatible || delta < match.deltaPercent) {
            match.targetBpm = candidate;
            match.ratioA = ratioA;
            match.ratioB = ratioB;
            match.deltaPercent = delta;
            match.compatible = true;
        }
    };

    consider(b.bpm);
    if (allowOctave && b.octaveAmbiguous) {
        for (double candidate : b.bpmCandidates) {
            consider(candidate);
        }
    }
    match.matched = std::abs(match.ratioA - 1.0) > 1e-4 ||
                    std::abs(match.ratioB - 1.0) > 1e-4;
    if (!match.compatible) {
        match.reason = format(
            "BPM difference %.1f%% exceeds the configured %.1f%% limit",
            match.deltaPercent, limit);
    }
    return match;
}

double shapeValue(TempoCurveKind kind, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (kind) {
    case TempoCurveKind::Linear:
        return t;
    case TempoCurveKind::Smooth:
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    case TempoCurveKind::SCurve:
        return 0.5 - 0.5 * std::cos(kPi * t);
    }
    return t;
}

int rampBarsFor(const PlannerConfig& config, double deltaPercent) {
    if (config.tempoRampBars >= 1.0) {
        return std::clamp(static_cast<int>(std::llround(config.tempoRampBars)),
                          2, 32);
    }
    int bars = deltaPercent <= 3.0 ? 4 : (deltaPercent <= 6.0 ? 8 : 16);
    if (config.aggressiveness == Aggressiveness::Gentle) {
        bars += 4;
    } else if (config.aggressiveness == Aggressiveness::Energetic) {
        bars -= 2;
    }
    return std::clamp(bars, 2, 32);
}

void appendBassSwap(MixPlan& plan, double duration) {
    if (!plan.bassSwap || duration <= 0.0) {
        return;
    }
    constexpr int kPoints = 9;
    plan.eqCurveOut.clear();
    plan.eqCurveIn.clear();
    plan.eqCurveOut.reserve(kPoints);
    plan.eqCurveIn.reserve(kPoints);
    for (int i = 0; i < kPoints; ++i) {
        const double t = static_cast<double>(i) / (kPoints - 1);
        const double time = duration * t;
        // Outgoing bass leaves over the first 40% of the fade; the incoming
        // bass enters from 25% to 70%. The overlap is short but never sums to
        // two full bass lines.
        const double outAmount = smoothStep(std::clamp(t / 0.40, 0.0, 1.0));
        const double inAmount =
            smoothStep(std::clamp((t - 0.25) / 0.45, 0.0, 1.0));
        EqGainPoint out;
        out.time = time;
        out.low = static_cast<float>(1.0 - outAmount);
        EqGainPoint in;
        in.time = time;
        in.low = static_cast<float>(inAmount);
        plan.eqCurveOut.push_back(out);
        plan.eqCurveIn.push_back(in);
    }
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
    if (config_.aggressiveness == Aggressiveness::Gentle) {
        duration = std::min(config_.maxCrossfadeSeconds, duration * 1.25);
    } else if (config_.aggressiveness == Aggressiveness::Energetic) {
        duration *= 0.75;
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
    plan.pitchLock = config_.pitchLock;

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
    const bool lowConfidence = plan.confidence < config_.minConfidence;
    const bool grids = !a.beats.empty() && !b.beats.empty();
    const int bpbA = a.beatsPerBar >= 2 ? a.beatsPerBar : 4;
    const int bpbB = b.beatsPerBar >= 2 ? b.beatsPerBar : 4;
    const int phraseBars = std::clamp(config_.phraseBars, 2, 16);

    double duration = request.requestedDuration > 0.0
                          ? request.requestedDuration
                          : suggestDuration(a, b);

    // ------------------------------------------------------------------
    // Adaptive type selection. The requested Smart Crossfade is the
    // "let the planner decide" mode; explicit types keep their identity.
    // ------------------------------------------------------------------
    TransitionType type = request.type;
    bool adaptiveChoice = false;
    if (type == TransitionType::SmartCrossfade && config_.adaptive) {
        adaptiveChoice = true;
        if (lowConfidence || !grids) {
            type = TransitionType::Crossfade;
            plan.fallbackReason =
                lowConfidence
                    ? format("analysis confidence %.2f is below the %.2f "
                             "threshold; using a safe equal-power crossfade",
                             plan.confidence, config_.minConfidence)
                    : "beat grid is missing; using a safe equal-power crossfade";
        } else if (!a.tempoStable || !b.tempoStable) {
            type = TransitionType::Crossfade;
            plan.fallbackReason =
                "tempo is not stable enough for beatmatching; using a safe "
                "equal-power crossfade";
        } else {
            const double localBpm =
                bpmAt(a, request.currentPositionA >= 0.0
                              ? request.currentPositionA
                              : std::max(0.0, a.info.duration - duration),
                      a.bpm);
            const TempoMatch match =
                matchTempo(b, localBpm > 0.0 ? localBpm : 120.0, config_, true);
            if (!match.compatible) {
                type = TransitionType::SmartCrossfade;
                plan.fallbackReason =
                    "adaptive: " + match.reason + "; using EQ bass-swap crossfade";
                plan.reasons.push_back(plan.fallbackReason);
            } else if (match.matched && config_.rampEnabled()) {
                type = TransitionType::TempoRamp;
                plan.reasons.push_back(format(
                    "Adaptive: compatible tempo (%.1f%% delta) -> pre-transition "
                    "tempo ramp",
                    match.deltaPercent));
            } else if (config_.preferPhraseBoundaries) {
                type = TransitionType::PhraseAware;
                plan.reasons.push_back(
                    "Adaptive: compatible tempo -> phrase-boundary mix");
            } else {
                type = TransitionType::Beatmatched;
                plan.reasons.push_back(
                    "Adaptive: compatible tempo -> beatmatched mix");
            }
        }
    }

    const bool unimplemented =
        transitionStatus(type) == ImplementationStatus::NotImplemented;
    if (unimplemented) {
        plan.fallbackReason = std::string(transitionTypeName(type)) +
                              " is not implemented in BG Mix 0.2";
        type = TransitionType::Crossfade;
        plan.type = type;
    }

    // ------------------------------------------------------------------
    // Musical window in the outgoing track.
    // ------------------------------------------------------------------
    double localBpmA = bpmAt(a, request.currentPositionA >= 0.0
                                   ? request.currentPositionA
                                   : std::max(0.0, a.info.duration - duration),
                             a.bpm);
    if (localBpmA <= 0.0) {
        localBpmA = a.bpm > 0.0 ? a.bpm : 120.0;
    }
    if (request.requestedDuration <= 0.0 &&
        plan.confidence >= config_.minConfidence) {
        const double barSeconds =
            static_cast<double>(bpbA) * 60.0 / localBpmA;
        int bars = static_cast<int>(std::llround(duration / barSeconds));
        bars = std::clamp(bars, 1, 32);
        if (downbeats && bars % 2 != 0) {
            ++bars;
        }
        duration = bars * barSeconds;
    }
    duration = clampDuration(duration, config_);

    TempoMatch tempo = matchTempo(b, localBpmA, config_, true);
    const bool tempoRequested =
        type == TransitionType::TempoRamp ||
        type == TransitionType::Beatmatched ||
        type == TransitionType::PhraseAware;
    bool ramp = false;
    int rampBars = 0;
    double rampSeconds = 0.0;
    double rampMediaSeconds = 0.0;
    double ratioA = 1.0;

    if (tempoRequested && tempo.compatible && tempo.matched &&
        config_.rampEnabled() && duration >= 2.0 &&
        plan.confidence >= config_.minConfidence && a.tempoStable &&
        b.tempoStable && !a.hasTempoChange()) {
        rampBars = rampBarsFor(config_, tempo.deltaPercent);
        rampMediaSeconds = static_cast<double>(rampBars * bpbA) * 60.0 /
                           localBpmA;
        ratioA = tempo.ratioA;
        const double meanRate = 0.5 * (1.0 + ratioA);
        rampSeconds = rampMediaSeconds / meanRate;
        ramp = true;
    }

    double overallStartA = 0.0;
    double crossfadeStartA = 0.0;
    double endA = 0.0;

    if (request.currentPositionA >= 0.0) {
        overallStartA = std::min(request.currentPositionA, a.info.duration);
        // Snap the ramp start to the outgoing grid when it is close. Snapping
        // backwards is allowed because the engine seeks the deck to the plan.
        if (grids && plan.confidence >= config_.minConfidence) {
            double aligned = 0.0;
            double error = 0.0;
            if (findGridPoint(a, overallStartA, downbeats, &aligned, nullptr,
                              &error) &&
                std::abs(error) <= 0.25 * 1000.0) {
                overallStartA = aligned;
            }
        }
        crossfadeStartA = overallStartA + (ramp ? rampMediaSeconds : 0.0);
        endA = crossfadeStartA + duration;
        if (ramp && endA > a.info.duration + 1e-6) {
            // The full ramp plus the fade does not fit after the current
            // position. Shorten the ramp in whole bars instead of collapsing
            // the crossfade to a few milliseconds.
            const double barMedia =
                static_cast<double>(bpbA) * 60.0 / localBpmA;
            const double room =
                a.info.duration - duration - overallStartA;
            const int fitBars =
                room > 0.0 ? static_cast<int>(std::floor(room / barMedia)) : 0;
            if (fitBars >= 2) {
                rampBars = std::min(rampBars, fitBars);
                rampMediaSeconds =
                    static_cast<double>(rampBars * bpbA) * 60.0 / localBpmA;
                rampSeconds = rampMediaSeconds / (0.5 * (1.0 + ratioA));
                crossfadeStartA = overallStartA + rampMediaSeconds;
                endA = std::min(a.info.duration, crossfadeStartA + duration);
                plan.reasons.push_back(format(
                    "Tempo ramp shortened to %.0f bars so the %.1f s crossfade "
                    "still fits before the end of the outgoing track",
                    static_cast<double>(rampBars), duration));
            } else {
                ramp = false;
                rampBars = 0;
                rampSeconds = 0.0;
                rampMediaSeconds = 0.0;
                ratioA = 1.0;
                crossfadeStartA = overallStartA;
                endA = std::min(a.info.duration, crossfadeStartA + duration);
                plan.reasons.push_back(
                    "Tempo ramp disabled: the crossfade must start now to fit "
                    "the remaining outgoing audio");
            }
        }
    } else {
        endA = musicalEnd(a);
        crossfadeStartA = std::max(0.0, endA - duration);
        if (grids && plan.confidence >= config_.minConfidence) {
            double aligned = 0.0;
            double error = 0.0;
            if (findGridPoint(a, crossfadeStartA, downbeats, &aligned, nullptr,
                              &error) &&
                std::abs(error) <= 0.5 * 1000.0) {
                crossfadeStartA = aligned;
            }
        }
        endA = std::min(a.info.duration, crossfadeStartA + duration);
        overallStartA = std::max(0.0, crossfadeStartA -
                                          (ramp ? rampMediaSeconds : 0.0));
        if (ramp && crossfadeStartA - overallStartA <
                        rampMediaSeconds - 1e-6) {
            // Not enough outgoing material for the full ramp: shorten it in
            // whole bars until it fits, or drop the ramp with a reason.
            const double available = crossfadeStartA;
            const double barMedia =
                static_cast<double>(bpbA) * 60.0 / localBpmA;
            const int fitBars = static_cast<int>(std::floor(available / barMedia));
            if (fitBars >= 2) {
                rampBars = std::min(rampBars, fitBars);
                rampMediaSeconds = static_cast<double>(rampBars * bpbA) * 60.0 /
                                   localBpmA;
                const double meanRate = 0.5 * (1.0 + ratioA);
                rampSeconds = rampMediaSeconds / meanRate;
                overallStartA = crossfadeStartA - rampMediaSeconds;
                plan.reasons.push_back(format(
                    "Tempo ramp shortened to %.0f bars: the outgoing track has "
                    "only %.1f s before the crossfade",
                    static_cast<double>(rampBars), available));
            } else {
                ramp = false;
                rampBars = 0;
                rampSeconds = 0.0;
                rampMediaSeconds = 0.0;
                ratioA = 1.0;
                overallStartA = crossfadeStartA;
                plan.reasons.push_back(
                    "Tempo ramp disabled: not enough outgoing audio before the "
                    "crossfade");
            }
        }
    }

    if (endA <= crossfadeStartA + 0.05) {
        plan.valid = false;
        plan.fallbackReason =
            "transition window does not fit the loaded outgoing track";
        return plan;
    }
    duration = endA - crossfadeStartA;
    plan.localBpmA = localBpmA;
    plan.duration = duration;
    plan.transitionStartA = crossfadeStartA;
    plan.transitionEndA = endA;

    // ------------------------------------------------------------------
    // Type-specific fallbacks that depend on the musical window.
    // ------------------------------------------------------------------
    if (type == TransitionType::TempoRamp && !ramp) {
        std::string reason;
        if (!config_.rampEnabled()) {
            reason = "Tempo Ramp is disabled in preferences";
        } else if (lowConfidence) {
            reason = format("tempo confidence %.2f is too low for a tempo ramp",
                            plan.confidence);
        } else if (!a.tempoStable || a.hasTempoChange()) {
            reason = "outgoing tempo is not stable enough for a tempo ramp";
        } else if (!tempo.compatible) {
            reason = tempo.reason;
        } else if (!tempo.matched) {
            reason = "tracks already share the same tempo";
        } else {
            reason = "transition is too short for a musical tempo ramp";
        }
        type = TransitionType::Crossfade;
        plan.fallbackReason = reason;
    }
    if (type == TransitionType::Beatmatched &&
        (!tempo.compatible || lowConfidence || !grids)) {
        type = TransitionType::Crossfade;
        plan.fallbackReason =
            lowConfidence || !grids
                ? "beat grid is missing or low-confidence; using a plain "
                  "crossfade"
                : "tempo is not compatible for beatmatching; using a plain "
                  "crossfade";
    }
    if (type == TransitionType::Beatmatched && tempo.matched &&
        !config_.rampEnabled()) {
        type = TransitionType::Crossfade;
        plan.fallbackReason =
            "beatmatching needs tempo sync but the tempo ramp is off; using a "
            "plain crossfade";
    }
    if (type == TransitionType::PhraseAware && (!grids || lowConfidence)) {
        type = TransitionType::Crossfade;
        plan.fallbackReason =
            "phrase alignment needs a confident beat grid; using a plain "
            "crossfade";
    }

    // ------------------------------------------------------------------
    // Entry point in the incoming track.
    // ------------------------------------------------------------------
    const double intro = sectionCue(b, SectionKind::Intro,
                                    b.beats.empty() ? 0.2 : b.beats.front().time);
    double cue = std::clamp(intro, 0.0, std::max(0.0, b.info.duration - 0.5));
    double startB = cue;
    bool bDownbeat = false;
    double bErrorMs = 0.0;
    bool bPhrase = false;
    float bPhraseConfidence = 0.0f;

    if (grids && plan.confidence >= config_.minConfidence) {
        const double barSecondsB = static_cast<double>(bpbB) * 60.0 /
                                   std::max(1.0, b.bpm);
        if (type == TransitionType::PhraseAware) {
            const PhraseCue phraseCue =
                findPhraseCue(b, cue, phraseBars, std::max(1.0, barSecondsB * 2.0));
            if (phraseCue.found) {
                startB = phraseCue.time;
                bDownbeat = phraseCue.downbeat;
                bPhrase = phraseCue.phrase;
                bPhraseConfidence = phraseCue.confidence;
            }
        }
        if (!bPhrase) {
            double aligned = 0.0;
            if (findGridPoint(b, cue, downbeats, &aligned, &bDownbeat,
                              &bErrorMs)) {
                if (aligned >= 0.0 && aligned < b.info.duration - 0.2) {
                    startB = aligned;
                }
            }
        }
        if (startB > cue + 0.01) {
            plan.reasons.push_back(format(
                bDownbeat ? "B entry: downbeat at %.3f s (%.1f ms from the "
                            "intro cue)"
                          : "B entry: beat at %.3f s (%.1f ms from the intro "
                            "cue)",
                startB, std::abs(bErrorMs)));
            plan.beatAligned = true;
            plan.downbeatAligned = bDownbeat;
            plan.alignmentErrorMs = std::max(plan.alignmentErrorMs,
                                             std::abs(bErrorMs));
        }
        if (bPhrase) {
            plan.reasons.push_back(format(
                "Phrase entry: bar-aligned boundary at %.3f s (confidence "
                "%.2f)",
                startB, bPhraseConfidence));
            plan.beatAligned = true;
            plan.downbeatAligned = true;
            plan.phraseBoundary = true;
            plan.phraseConfidence = bPhraseConfidence;
        }
    }
    startB = std::clamp(startB, 0.0, std::max(0.0, b.info.duration - 0.25));

    // ------------------------------------------------------------------
    // Outgoing cue alignment (kept after the type decision so a fallback
    // crossfade still starts on a musical boundary when possible).
    // ------------------------------------------------------------------
    if (grids && plan.confidence >= config_.minConfidence) {
        const double beatSecondsA = 60.0 / std::max(1.0, localBpmA);
        const double tolerance = std::max(0.35, 0.5 * beatSecondsA);
        double aligned = 0.0;
        double error = 0.0;
        bool snapped = false;
        bool snappedDownbeat = false;
        // Prefer a downbeat, but only when it is genuinely close. At slow
        // tempos the nearest downbeat can be most of a bar away; in that case
        // align to the beat instead of delaying the mix by half a bar.
        if (downbeats &&
            findGridPoint(a, crossfadeStartA, true, &aligned, nullptr, &error) &&
            std::abs(aligned - crossfadeStartA) <= tolerance) {
            snapped = true;
            snappedDownbeat = true;
        } else if (findGridPoint(a, crossfadeStartA, false, &aligned, nullptr,
                                 &error) &&
                   std::abs(aligned - crossfadeStartA) <= tolerance) {
            snapped = true;
            snappedDownbeat = false;
        }
        if (snapped &&
            overallStartA + (aligned - crossfadeStartA) >= 0.0) {
            const double shift = aligned - crossfadeStartA;
            crossfadeStartA = aligned;
            endA = std::min(a.info.duration, crossfadeStartA + duration);
            overallStartA += shift;
            duration = endA - crossfadeStartA;
            plan.transitionStartA = crossfadeStartA;
            plan.transitionEndA = endA;
            plan.duration = duration;
            plan.reasons.push_back(format(
                snappedDownbeat
                    ? "A grid: transition starts on a downbeat at %.3f s (%.1f "
                      "ms from the requested point)"
                    : "A grid: transition starts on a beat at %.3f s (%.1f ms "
                      "from the requested point)",
                crossfadeStartA, std::abs(error)));
            plan.beatAligned = true;
            plan.downbeatAligned =
                plan.downbeatAligned || snappedDownbeat;
            plan.alignmentErrorMs = std::max(plan.alignmentErrorMs,
                                             std::abs(error));
        }
    }

    // ------------------------------------------------------------------
    // Fit the fade to the available incoming audio.
    // ------------------------------------------------------------------
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
    plan.phraseBars = phraseBars;
    plan.beatPhaseErrorMs =
        plan.beatAligned ? 0.0 : (b.bpm > 0.0 ? 30000.0 / b.bpm : 0.0);

    // ------------------------------------------------------------------
    // Direct cut.
    // ------------------------------------------------------------------
    if (type == TransitionType::DirectCut) {
        plan.type = TransitionType::DirectCut;
        plan.duration = 0.0;
        plan.preTransitionSeconds = 0.0;
        plan.tempoRampActive = false;
        // An executed cut happens at the current position. A preview reports
        // the end of the planned musical window, so automatic scheduling cuts
        // there (before trailing silence) rather than at a mid-window point.
        const double cutPoint =
            request.currentPositionA >= 0.0 ? overallStartA : endA;
        plan.transitionStartA = cutPoint;
        plan.transitionEndA = cutPoint;
        plan.tempoRampStartA = cutPoint;
        plan.startB = startB;
        plan.endB = std::min(b.info.duration, startB + 0.01);
        plan.reasons.push_back(
            "Direct cut: no crossfade, incoming deck starts on its marker");
        plan.valid = crossfadeStartA >= 0.0 && startB < b.info.duration;
        if (!plan.fallbackReason.empty()) {
            plan.reasons.push_back("Fallback: " + plan.fallbackReason);
        }
        return plan;
    }

    plan.type = type;
    plan.tempoRampActive = ramp;
    plan.tempoRampPercent = tempo.deltaPercent;
    plan.tempoRatioAStart = 1.0;
    plan.tempoRatioAEnd = ratioA;
    plan.tempoRatioB = tempo.ratioB;
    plan.preTransitionSeconds = ramp ? rampSeconds : 0.0;
    plan.tempoRampStartA = ramp ? overallStartA : crossfadeStartA;
    plan.targetBpm = tempo.compatible ? tempo.targetBpm : b.bpm;

    // ------------------------------------------------------------------
    // Tempo automation: ramp before the overlay, then hold the matched rate.
    // Curve times are relative to the crossfade start, so pre-roll points are
    // negative. The integral of the ramp equals the outgoing media consumed.
    // ------------------------------------------------------------------
    if (ramp) {
        constexpr int kRampPoints = 65;
        plan.tempoCurve.reserve(kRampPoints + 2);
        for (int i = 0; i < kRampPoints; ++i) {
            const double t = static_cast<double>(i) / (kRampPoints - 1);
            const double rate =
                1.0 + (ratioA - 1.0) * shapeValue(config_.tempoCurveKind, t);
            const double time = -rampSeconds + rampSeconds * t;
            plan.tempoCurve.push_back(
                {time, rate, localBpmA * rate});
        }
        plan.tempoCurve.push_back({0.0, ratioA, tempo.targetBpm});
        if (plan.duration > 1e-6) {
            plan.tempoCurve.push_back(
                {plan.duration, ratioA, tempo.targetBpm});
        }
        plan.reasons.push_back(format(
            "Tempo ramp: %.1f -> %.1f BPM over %.1f bars (%.1f s) before the "
            "crossfade",
            localBpmA, tempo.targetBpm, static_cast<double>(rampBars),
            rampSeconds));
        const char* shapeName =
            config_.tempoCurveKind == TempoCurveKind::Linear
                ? "linear"
                : (config_.tempoCurveKind == TempoCurveKind::Smooth
                       ? "smooth"
                       : "S-curve");
        plan.reasons.push_back(
            std::string("Rate curve ") + shapeName +
            format(": starts at 1.000, ends at %.4f, no boundary step",
                   ratioA));
        if (std::abs(tempo.ratioB - 1.0) > 1e-4) {
            plan.reasons.push_back(format(
                "Incoming deck tempo ratio %.3f to reach %.1f BPM",
                tempo.ratioB, tempo.targetBpm, 0.0));
        }
    } else {
        plan.tempoCurve.push_back({0.0, 1.0, localBpmA});
        if (plan.duration > 0.0) {
            plan.tempoCurve.push_back({plan.duration, 1.0, localBpmA});
        }
    }
    if (plan.pitchLock) {
        plan.reasons.push_back(
            "Pitch lock: WSOLA time-stretch keeps both decks at their recorded "
            "pitch");
    } else if (ramp || std::abs(tempo.ratioB - 1.0) > 1e-4) {
        plan.pitchShiftWarning = true;
        plan.reasons.push_back(
            "Pitch lock off: resampling will shift pitch during the tempo "
            "change");
    }

    // ------------------------------------------------------------------
    // Bass swap / EQ automation.
    // ------------------------------------------------------------------
    plan.bassSwap = config_.bassSwap && plan.type != TransitionType::DirectCut &&
                    plan.duration >= 2.0;
    if (plan.bassSwap) {
        appendBassSwap(plan, plan.duration);
        plan.reasons.push_back(
            "Bass swap: outgoing low band leaves before the incoming low band "
            "enters");
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

    if (adaptiveChoice && plan.fallbackReason.empty()) {
        plan.reasons.push_back(
            "Adaptive: chose " + std::string(transitionTypeName(plan.type)));
    }
    if (lowConfidence) {
        plan.reasons.push_back(format(
            "Analysis confidence %.2f is low; the planner chose conservative "
            "values",
            plan.confidence));
    }
    if (plan.fallbackReason.empty()) {
        plan.reasons.push_back(format("Plan confidence %.2f", plan.confidence));
    } else {
        plan.reasons.push_back("Fallback: " + plan.fallbackReason);
    }
    plan.valid = plan.duration >= 0.0 && plan.duration <= a.info.duration &&
                 plan.startB < b.info.duration &&
                 plan.transitionStartA <= a.info.duration &&
                 plan.overallStartA() >= -1e-6;
    if (!plan.valid) {
        plan.fallbackReason = "transition window does not fit the loaded tracks";
    }
    return plan;
}

} // namespace bgmedia::mix
