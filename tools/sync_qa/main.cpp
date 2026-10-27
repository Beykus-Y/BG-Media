// bgmedia_sync_qa: measurable BG Motion 3.1 musical-sync report.
//
// The tool is deliberately headless. It builds deterministic synthetic tracks
// with ground truth, runs BG Beat, builds the MusicalTimeline, and measures:
//   * beat precision/recall/timing error, octave errors;
//   * meter/downbeat accuracy and confidence;
//   * predicted visual accent approach time (not display latency);
//   * phase speed / acceleration / jerk, foot slide, NaN;
//   * analysis, timeline and schedule-sampling cost.
//
// "Emulated 3.0" uses the old flat BeatClock schedule (no warp, no downbeat
// preference). "3.1" uses the new timeline, trusted-downbeat alignment and the
// Soft/Expressive warp. The two paths are labelled explicitly; the tool never
// claims that a predicted pose time is a measured monitor latency.

#include "../../tests/motion_metrics.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace bgmedia;
using Clock = std::chrono::steady_clock;

struct Case {
    std::string name;
    test::SyntheticTrack track;
    double truthBpm = 0.0;
    int truthMeter = 4;
    std::string expectedMeter;
};

struct BeatScore {
    int matched = 0;
    int detected = 0;
    int reference = 0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    double medianAbsMs = 0.0;
    double p95AbsMs = 0.0;
    double meanSignedMs = 0.0;
    int missed = 0;
    int extra = 0;
};

BeatScore scoreBeats(const std::vector<double>& truth,
                     const std::vector<Beat>& detected, double tolerance) {
    BeatScore score;
    std::vector<double> det;
    for (const Beat& beat : detected) {
        if (std::isfinite(beat.time)) {
            det.push_back(beat.time);
        }
    }
    std::vector<double> ref;
    for (double t : truth) {
        if (std::isfinite(t)) {
            ref.push_back(t);
        }
    }
    std::sort(det.begin(), det.end());
    std::sort(ref.begin(), ref.end());
    if (ref.empty()) {
        score.detected = static_cast<int>(det.size());
        score.extra = score.detected;
        return score;
    }
    const double from = ref.front() - tolerance;
    const double to = ref.back() + tolerance;
    std::vector<double> detIn;
    for (double t : det) {
        if (t >= from && t <= to) {
            detIn.push_back(t);
        }
    }
    std::vector<double> refIn;
    for (double t : ref) {
        if (t >= ref.front() && t <= ref.back()) {
            refIn.push_back(t);
        }
    }
    std::vector<bool> used(detIn.size(), false);
    std::vector<double> errors;
    for (double expected : refIn) {
        int best = -1;
        double bestError = tolerance + 1e-12;
        for (std::size_t i = 0; i < detIn.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const double error = std::abs(detIn[i] - expected);
            if (error <= bestError) {
                bestError = error;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++score.matched;
            errors.push_back(detIn[static_cast<std::size_t>(best)] - expected);
        }
    }
    score.detected = static_cast<int>(det.size());
    score.reference = static_cast<int>(ref.size());
    score.missed = score.reference - score.matched;
    score.extra = score.detected - score.matched;
    score.precision = score.detected > 0 ? static_cast<double>(score.matched) / score.detected : 0.0;
    score.recall = score.reference > 0 ? static_cast<double>(score.matched) / score.reference : 0.0;
    score.f1 = score.precision + score.recall > 0.0
                   ? 2.0 * score.precision * score.recall / (score.precision + score.recall)
                   : 0.0;
    if (!errors.empty()) {
        std::vector<double> abs;
        double sum = 0.0;
        for (double e : errors) {
            sum += e;
            abs.push_back(std::abs(e) * 1000.0);
        }
        score.meanSignedMs = sum / static_cast<double>(errors.size()) * 1000.0;
        std::sort(abs.begin(), abs.end());
        score.medianAbsMs = abs[abs.size() / 2];
        score.p95AbsMs = abs[static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(abs.size())) - 1.0)];
    }
    return score;
}

BeatScore scoreDownbeats(const std::vector<double>& truth,
                         const std::vector<Beat>& detected, double tolerance) {
    std::vector<Beat> downbeats;
    for (const Beat& beat : detected) {
        if (beat.downbeat) {
            downbeats.push_back(beat);
        }
    }
    return scoreBeats(truth, downbeats, tolerance);
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double angularDistance(const motion::ClipPose& a, const motion::ClipPose& b,
                       bool upperOnly) {
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        const auto bone = static_cast<vrm::HumanoidBone>(i);
        if (upperOnly) {
            switch (bone) {
            case vrm::HumanoidBone::Hips:
            case vrm::HumanoidBone::LeftUpperLeg:
            case vrm::HumanoidBone::LeftLowerLeg:
            case vrm::HumanoidBone::LeftFoot:
            case vrm::HumanoidBone::LeftToes:
            case vrm::HumanoidBone::RightUpperLeg:
            case vrm::HumanoidBone::RightLowerLeg:
            case vrm::HumanoidBone::RightFoot:
            case vrm::HumanoidBone::RightToes:
                continue;
            default:
                break;
            }
        }
        const float d = std::abs(math::dot(a.bone[i].normalized(), b.bone[i].normalized()));
        sum += 2.0 * std::acos(std::clamp(d, 0.0f, 1.0f)) * 180.0 / 3.14159265358979;
        ++count;
    }
    return count ? sum / static_cast<double>(count) : 0.0;
}

struct AnimationMetrics {
    double minSpeed = 0.0;
    double maxSpeed = 0.0;
    double maxAccel = 0.0;
    double maxJerk = 0.0;
    double slideM = 0.0;
    double footErrorM = 0.0;
    int nonFinite = 0;
};

AnimationMetrics measureAnimation(const motion::DanceDirector& director,
                                  const vrm::VrmModel* model,
                                  const animation::HumanoidRetargeter* rig,
                                  double from, double to) {
    AnimationMetrics metrics;
    const double dt = 1.0 / 240.0;
    double previousBeat = director.sample(from).clipBeat;
    double previousSpeed = 0.0;
    double previousAccel = 0.0;
    bool haveSpeed = false;
    bool haveAccel = false;
    const int nodes[2] = {model && model->nodeFor(vrm::HumanoidBone::LeftFoot) >= 0
                              ? model->nodeFor(vrm::HumanoidBone::LeftFoot)
                              : -1,
                          model && model->nodeFor(vrm::HumanoidBone::RightFoot) >= 0
                              ? model->nodeFor(vrm::HumanoidBone::RightFoot)
                              : -1};
    bool inRun[2] = {false, false};
    math::Vec3 anchor[2];
    bool first = true;
    for (double t = from + dt; t <= to; t += dt) {
        const motion::DanceFrame frame = director.sample(t);
        if (!std::isfinite(frame.clipBeat) || !std::isfinite(frame.pose.hips.y)) {
            ++metrics.nonFinite;
        }
        const double speed = (frame.clipBeat - previousBeat) / dt;
        if (first) {
            metrics.minSpeed = speed;
            first = false;
        }
        metrics.minSpeed = std::min(metrics.minSpeed, speed);
        metrics.maxSpeed = std::max(metrics.maxSpeed, speed);
        const double accel = (speed - previousSpeed) / dt;
        if (haveSpeed) {
            metrics.maxAccel = std::max(metrics.maxAccel, std::abs(accel));
        }
        if (haveAccel) {
            metrics.maxJerk = std::max(metrics.maxJerk, std::abs(accel - previousAccel) / dt);
        }
        previousBeat = frame.clipBeat;
        previousSpeed = speed;
        previousAccel = accel;
        haveSpeed = true;
        haveAccel = true;
        if (model && rig && rig->valid()) {
            const animation::RetargetedPose pose = rig->applyClip(frame.pose, t);
            metrics.footErrorM = std::max<double>(metrics.footErrorM, pose.stats.maxFootError);
            for (int f = 0; f < 2; ++f) {
                const auto& foot = frame.pose.foot[static_cast<std::size_t>(f)];
                const bool planted =
                    (foot.phase == motion::FootPhase::Stance ||
                     foot.phase == motion::FootPhase::Pivot) &&
                    foot.contact > 0.999f;
                if (!planted) {
                    inRun[f] = false;
                    continue;
                }
                const math::Vec3 p =
                    animation::HumanoidRetargeter::stagePosition(pose, nodes[f]);
                if (!inRun[f]) {
                    inRun[f] = true;
                    anchor[f] = p;
                } else {
                    metrics.slideM = std::max<double>(metrics.slideM, (p - anchor[f]).length());
                }
            }
        }
    }
    return metrics;
}

struct AccentSync {
    double medianAbsMs = 0.0;
    double p95AbsMs = 0.0;
    int samples = 0;
};

AccentSync measureAccentSync(motion::DanceDirector& director,
                             const motion::MusicalTimeline& timeline,
                             double from, double to, int maxEvents) {
    AccentSync result;
    std::vector<double> errors;
    for (const motion::AccentEvent& event : timeline.accents()) {
        if (!event.downbeat || event.time < from || event.time > to) {
            continue;
        }
        const double target = event.time;
        const motion::ClipPose reference = director.sample(target - 0.35).pose;
        const motion::ClipPose targetPose = director.sample(target).pose;
        const double span = angularDistance(reference, targetPose, true);
        if (span < 0.5) {
            continue;
        }
        double crossing = target;
        bool found = false;
        for (double t = target - 0.25; t <= target + 0.10; t += 1.0 / 240.0) {
            const motion::ClipPose pose = director.sample(t).pose;
            const double progress = 1.0 - angularDistance(pose, targetPose, true) / span;
            if (progress >= 0.90) {
                crossing = t;
                found = true;
                break;
            }
        }
        if (!found) {
            crossing = target;
        }
        errors.push_back((crossing - target) * 1000.0);
        if (static_cast<int>(errors.size()) >= maxEvents) {
            break;
        }
    }
    result.samples = static_cast<int>(errors.size());
    if (!errors.empty()) {
        std::vector<double> absolute;
        for (double e : errors) {
            absolute.push_back(std::abs(e));
        }
        std::sort(absolute.begin(), absolute.end());
        result.medianAbsMs = absolute[absolute.size() / 2];
        result.p95AbsMs = absolute[static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(absolute.size())) - 1.0)];
    }
    return result;
}

std::vector<Case> makeCases() {
    std::vector<Case> cases;
    const auto addClick = [&](const std::string& name, double bpm, int meter,
                              const std::string& label, double duration = 24.0) {
        test::TrackOptions options;
        options.bpm = bpm;
        options.duration = duration;
        options.beatsPerBar = meter;
        Case item;
        item.name = name;
        item.track = test::makeClickTrack(options);
        item.truthBpm = bpm;
        item.truthMeter = meter;
        item.expectedMeter = label;
        cases.push_back(std::move(item));
    };
    for (double bpm : {60.0, 80.0, 90.0, 100.0, 120.0, 125.0, 128.0, 150.0, 180.0}) {
        addClick(std::to_string(static_cast<int>(bpm)) + " BPM 4/4", bpm, 4, "4/4");
    }
    addClick("100 BPM 3/4", 100.0, 3, "3/4");

    const auto addTrack = [&](const std::string& name, test::SyntheticTrack track,
                              double bpm, int meter, const std::string& label) {
        Case item;
        item.name = name;
        item.track = std::move(track);
        item.truthBpm = bpm;
        item.truthMeter = meter;
        item.expectedMeter = label;
        cases.push_back(std::move(item));
    };
    addTrack("6/8 (2 pulses, triplets)", test::makeCompoundTrack(120.0, 2, 24.0, 3),
             120.0, 2, "6/8");
    addTrack("12/8 (4 pulses, triplets)", test::makeCompoundTrack(90.0, 4, 24.0, 3),
             90.0, 4, "12/8");
    addTrack("half-time 80", test::makeHalfTimeTrack(80.0, 24.0), 80.0, 4, "4/4");
    addTrack("dense hats 160 (double-time layer)", test::makeDenseHatTrack(160.0, 20.0),
             160.0, 4, "4/4");
    addTrack("strong hats / weak kick 128", test::makeStrongHatWeakKickTrack(128.0, 20.0),
             128.0, 4, "4/4");
    addTrack("offbeat accents 100", test::makeOffbeatTrack(100.0, 20.0), 100.0, 4, "4/4");
    addTrack("syncopated bass 100", test::makeSyncopatedBassTrack(100.0, 20.0), 100.0, 4,
             "4/4");
    addTrack("shifted accents 100", test::makeShiftedAccentTrack(100.0, 20.0), 100.0, 4,
             "4/4");
    addTrack("silence gap / resume", test::makeSilenceTrack(), 92.0, 4, "4/4");
    addTrack("tempo ramp 80->120", test::makeTempoRampTrack(80.0, 120.0, 24.0), 0.0, 4,
             "4/4");
    addTrack("sudden tempo 92->123", test::makeTempoChangeTrack(92.0, 123.0, 12.0, 24.0),
             0.0, 4, "4/4");
    return cases;
}

struct CaseReport {
    std::string name;
    double truthBpm = 0.0;
    double detectedBpm = 0.0;
    float bpmConfidence = 0.0f;
    BeatScore beats;
    BeatScore downbeats;
    int detectedMeter = 0;
    bool meterCorrect = false;
    float downbeatConfidence = 0.0f;
    std::string timelineMeter;
    int subdivision = 1;
    bool compound = false;
    bool downbeatTrusted = false;
    int accentCount = 0;
    int offbeatAccentCount = 0;
    int bassAccentCount = 0;
    double danceRate = 1.0;
    double danceEffectiveBpm = 0.0;
    bool danceSpeedLimited = false;
    double analysisMs = 0.0;
    double timelineMs = 0.0;
    AccentSync baselineAccent;
    AccentSync softAccent;
    AccentSync expressiveAccent;
    AnimationMetrics baseline;
    AnimationMetrics expressive;
    double sampleUsOff = 0.0;
    double sampleUsExpressive = 0.0;
};

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path dances = "assets/dances";
    std::filesystem::path modelPath = "models/Model1.vrm";
    std::filesystem::path jsonPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dances" && i + 1 < argc) {
            dances = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            modelPath = argv[++i];
        } else if (arg == "--json" && i + 1 < argc) {
            jsonPath = argv[++i];
        }
    }

    motion::DanceLibrary library;
    library.loadDirectory(dances);
    std::printf("BG Motion 3.1 sync QA\n%zu clips from %s\n", library.clips().size(),
                dances.string().c_str());
    for (const motion::ClipDiagnostic& d : library.diagnostics()) {
        std::printf("%s %s: %s\n",
                    d.severity == motion::ClipDiagnostic::Severity::Error ? "ERROR" : "warn",
                    d.file.c_str(), d.message.c_str());
    }

    std::unique_ptr<vrm::LoadResult> loaded;
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    if (std::filesystem::exists(modelPath)) {
        loaded = std::make_unique<vrm::LoadResult>(vrm::VrmLoader::load(modelPath));
        if (loaded->ok()) {
            rig = std::make_unique<animation::HumanoidRetargeter>(*loaded->model);
            std::printf("model: %s (%.2f m)\n", modelPath.string().c_str(),
                        loaded->model->height);
        }
    }
    if (!rig) {
        std::printf("model not loaded: foot-slide metrics will be omitted\n");
    }
    const vrm::VrmModel* model = loaded && loaded->ok() ? loaded->model.get() : nullptr;

    std::vector<Case> cases = makeCases();
    std::vector<CaseReport> reports;
    reports.reserve(cases.size());

    for (const Case& item : cases) {
        CaseReport report;
        report.name = item.name;
        report.truthBpm = item.truthBpm;
        std::string error;
        PcmBuffer pcm = PcmBuffer::fromAudio(item.track.audio, &error);
        if (!pcm.valid()) {
            std::printf("%-34s PCM error: %s\n", item.name.c_str(), error.c_str());
            continue;
        }
        Analyzer analyzer;
        const AnalysisReport analysisReport = analyzer.analyze(pcm);
        if (!analysisReport.ok()) {
            std::printf("%-34s analysis error: %s\n", item.name.c_str(),
                        analysisReport.message.c_str());
            continue;
        }
        const AnalysisResult& analysis = analysisReport.result;
        report.detectedBpm = analysis.bpm;
        report.bpmConfidence = analysis.bpmConfidence;
        report.detectedMeter = analysis.beatsPerBar;
        report.meterCorrect = analysis.beatsPerBar == item.truthMeter;
        report.downbeatConfidence = analysis.downbeatConfidence;
        report.analysisMs = analysis.analysisSeconds * 1000.0;
        if (item.name.find("ramp") != std::string::npos ||
            item.name.find("sudden") != std::string::npos ||
            item.name.find("silence") != std::string::npos) {
            std::printf("  %s: stable=%d segments=%zu transitions=%zu bpm=%.2f\n",
                        item.name.c_str(), analysis.tempoStable ? 1 : 0,
                        analysis.tempoSegments.size(), analysis.tempoTransitions.size(),
                        analysis.bpm);
            for (const TempoSegment& segment : analysis.tempoSegments) {
                std::printf("    seg %.2f..%.2f %.2f\n", segment.start, segment.end,
                            segment.bpm);
            }
        }

        const double tolerance = std::max(0.07, 0.03 * 60.0 / std::max(40.0, item.truthBpm));
        report.beats = scoreBeats(item.track.beatTimes, analysis.beats, tolerance);
        report.downbeats = scoreDownbeats(item.track.downbeatTimes, analysis.beats, tolerance);

        const auto timelineStart = Clock::now();
        const motion::MusicalTimeline timeline =
            motion::MusicalTimeline::fromAnalysis(analysis);
        report.timelineMs =
            std::chrono::duration<double, std::milli>(Clock::now() - timelineStart).count();
        report.timelineMeter = timeline.meterLabel();
        report.subdivision = timeline.subdivision();
        report.compound = timeline.compound();
        report.downbeatTrusted = timeline.downbeatTrusted();
        report.accentCount = static_cast<int>(timeline.accents().size());
        for (const motion::AccentEvent& accent : timeline.accents()) {
            if (accent.kind == motion::AccentKind::Offbeat ||
                accent.kind == motion::AccentKind::Subdivision) {
                ++report.offbeatAccentCount;
            }
            if (accent.bass) {
                ++report.bassAccentCount;
            }
        }
        if (const motion::HumanoidClip* energetic =
                library.forStyle(motion::DanceStyle::Energetic)) {
            report.danceRate = motion::DanceDirector::chooseRate(
                *energetic, analysis.bpm, &report.danceSpeedLimited);
            report.danceEffectiveBpm = analysis.bpm * report.danceRate;
        }

        // Schedule metrics: same clip and grid; only alignment/warp differ.
        motion::DanceDirector baseline;
        baseline.setLibrary(&library);
        baseline.setBeatClock(motion::BeatClock::fromAnalysis(analysis));
        baseline.setExpression(motion::MotionExpression::Off);
        baseline.reset(0.0);
        baseline.select(motion::DanceStyle::Groove, 0.0, false, true);

        motion::DanceDirector soft;
        soft.setLibrary(&library);
        soft.setMusicalTimeline(timeline);
        soft.setExpression(motion::MotionExpression::Soft);
        soft.reset(0.0);
        soft.select(motion::DanceStyle::Groove, 0.0, false, true);

        motion::DanceDirector expressive;
        expressive.setLibrary(&library);
        expressive.setMusicalTimeline(timeline);
        expressive.setExpression(motion::MotionExpression::Expressive);
        expressive.reset(0.0);
        expressive.select(motion::DanceStyle::Groove, 0.0, false, true);

        const double measureFrom = 1.0;
        const double measureTo = std::min(analysis.info.duration - 0.5,
                                          measureFrom + 16.0 * 60.0 /
                                                           std::max(60.0, analysis.bpm));
        if (measureTo > measureFrom + 1.0) {
            report.baselineAccent =
                measureAccentSync(baseline, timeline, measureFrom, measureTo, 10);
            report.softAccent =
                measureAccentSync(soft, timeline, measureFrom, measureTo, 10);
            report.expressiveAccent =
                measureAccentSync(expressive, timeline, measureFrom, measureTo, 10);
            report.baseline = measureAnimation(baseline, model, rig.get(), measureFrom,
                                               measureTo);
            report.expressive = measureAnimation(expressive, model, rig.get(), measureFrom,
                                                 measureTo);
        }

        // Sampling cost, including the second clip sample used by the warp.
        const auto sampleStart = Clock::now();
        double sink = 0.0;
        for (int i = 0; i < 6000; ++i) {
            sink += baseline.sample(measureFrom + i * 0.001).clipBeat;
        }
        const auto sampleMid = Clock::now();
        for (int i = 0; i < 6000; ++i) {
            sink += expressive.sample(measureFrom + i * 0.001).clipBeat;
        }
        const auto sampleEnd = Clock::now();
        report.sampleUsOff =
            std::chrono::duration<double, std::micro>(sampleMid - sampleStart).count() / 6000.0;
        report.sampleUsExpressive =
            std::chrono::duration<double, std::micro>(sampleEnd - sampleMid).count() / 6000.0;
        (void)sink;
        reports.push_back(std::move(report));
    }

    std::printf("\n%-32s %8s %8s %6s %6s %7s %7s %7s %6s %6s %5s %8s %5s %8s\n",
                "case", "bpm", "detect", "conf", "P", "R", "med ms", "p95 ms",
                "downF1", "meter", "sub", "label", "rate", "effBpm");
    for (const CaseReport& r : reports) {
        std::printf("%-32s %8.1f %8.2f %6.2f %6.2f %6.2f %7.2f %7.2f %6.2f %6d %5d %8s %5.1f %8.1f\n",
                    r.name.c_str(), r.truthBpm, r.detectedBpm, r.bpmConfidence,
                    r.beats.precision, r.beats.recall, r.beats.medianAbsMs,
                    r.beats.p95AbsMs, r.downbeats.f1, r.detectedMeter, r.subdivision,
                    r.timelineMeter.c_str(), r.danceRate, r.danceEffectiveBpm);
    }

    std::printf("\n%-32s %10s %10s %10s | %10s %10s | %8s %8s | %8s %8s\n",
                "case", "baseAccMs", "softAccMs", "exprAccMs", "baseVel", "exprVel",
                "baseJerk", "exprJerk", "baseSlide", "exprSlide");
    for (const CaseReport& r : reports) {
        std::printf("%-32s %10.1f %10.1f %10.1f | %10.2f %10.2f | %8.0f %8.0f | %8.2f %8.2f\n",
                    r.name.c_str(), r.baselineAccent.medianAbsMs, r.softAccent.medianAbsMs,
                    r.expressiveAccent.medianAbsMs, r.baseline.maxSpeed,
                    r.expressive.maxSpeed, r.baseline.maxJerk, r.expressive.maxJerk,
                    r.baseline.slideM * 1000.0, r.expressive.slideM * 1000.0);
    }

    double sumAnalysis = 0.0;
    double sumTimeline = 0.0;
    double sumBeatF1 = 0.0;
    double sumDownF1 = 0.0;
    int meterCorrect = 0;
    int trusted = 0;
    int nan = 0;
    double worstSlide = 0.0;
    double sumBaseAcc = 0.0;
    double sumExprAcc = 0.0;
    int accSamples = 0;
    for (const CaseReport& r : reports) {
        sumAnalysis += r.analysisMs;
        sumTimeline += r.timelineMs;
        sumBeatF1 += r.beats.f1;
        sumDownF1 += r.downbeats.f1;
        meterCorrect += r.meterCorrect ? 1 : 0;
        trusted += r.downbeatTrusted ? 1 : 0;
        nan += r.expressive.nonFinite;
        worstSlide = std::max(worstSlide, r.expressive.slideM);
        sumBaseAcc += r.baselineAccent.medianAbsMs;
        sumExprAcc += r.expressiveAccent.medianAbsMs;
        accSamples += r.expressiveAccent.samples;
    }
    const double n = std::max(1.0, static_cast<double>(reports.size()));
    std::printf("\nsummary: %zu cases\n", reports.size());
    std::printf("  beat F1 %.4f  median %.2f ms  p95 %.2f ms\n", sumBeatF1 / n,
                median([&] {
                    std::vector<double> v;
                    for (const auto& r : reports) v.push_back(r.beats.medianAbsMs);
                    return v;
                }()),
                median([&] {
                    std::vector<double> v;
                    for (const auto& r : reports) v.push_back(r.beats.p95AbsMs);
                    return v;
                }()));
    std::printf("  downbeat F1 %.4f  meter %d/%zu  trusted %d/%zu\n", sumDownF1 / n,
                meterCorrect, reports.size(), trusted, reports.size());
    std::printf("  predicted accent approach: base %.1f ms -> expressive %.1f ms (%d samples)\n",
                sumBaseAcc / std::max(1, static_cast<int>(reports.size())),
                sumExprAcc / std::max(1, static_cast<int>(reports.size())), accSamples);
    std::printf("  worst foot slide %.2f mm  non-finite %d  analysis %.1f ms  timeline %.2f ms\n",
                worstSlide * 1000.0, nan, sumAnalysis / n, sumTimeline / n);

    if (!jsonPath.empty()) {
        std::error_code ec;
        if (!jsonPath.parent_path().empty()) {
            std::filesystem::create_directories(jsonPath.parent_path(), ec);
        }
        std::ofstream out(jsonPath, std::ios::binary | std::ios::trunc);
        if (out) {
            out << "{\n  \"cases\": [\n";
            for (std::size_t i = 0; i < reports.size(); ++i) {
                const CaseReport& r = reports[i];
                out << "    {\"name\":\"" << r.name << "\",\"truthBpm\":" << r.truthBpm
                    << ",\"detectedBpm\":" << r.detectedBpm
                    << ",\"bpmConfidence\":" << r.bpmConfidence
                    << ",\"beatF1\":" << r.beats.f1
                    << ",\"medianMs\":" << r.beats.medianAbsMs
                    << ",\"p95Ms\":" << r.beats.p95AbsMs
                    << ",\"precision\":" << r.beats.precision
                    << ",\"recall\":" << r.beats.recall
                    << ",\"downbeatF1\":" << r.downbeats.f1
                    << ",\"downbeatConfidence\":" << r.downbeatConfidence
                    << ",\"meterCorrect\":" << (r.meterCorrect ? "true" : "false")
                    << ",\"timelineMeter\":\"" << r.timelineMeter << "\""
                    << ",\"subdivision\":" << r.subdivision
                    << ",\"compound\":" << (r.compound ? "true" : "false")
                    << ",\"downbeatTrusted\":" << (r.downbeatTrusted ? "true" : "false")
                    << ",\"accentCount\":" << r.accentCount
                    << ",\"offbeatAccentCount\":" << r.offbeatAccentCount
                    << ",\"bassAccentCount\":" << r.bassAccentCount
                    << ",\"danceRate\":" << r.danceRate
                    << ",\"danceEffectiveBpm\":" << r.danceEffectiveBpm
                    << ",\"danceSpeedLimited\":" << (r.danceSpeedLimited ? "true" : "false")
                    << ",\"baseAccentMs\":" << r.baselineAccent.medianAbsMs
                    << ",\"softAccentMs\":" << r.softAccent.medianAbsMs
                    << ",\"exprAccentMs\":" << r.expressiveAccent.medianAbsMs
                    << ",\"baseSlideMm\":" << r.baseline.slideM * 1000.0
                    << ",\"exprSlideMm\":" << r.expressive.slideM * 1000.0
                    << ",\"footErrorMm\":" << r.expressive.footErrorM * 1000.0
                    << ",\"nonFinite\":" << r.expressive.nonFinite
                    << ",\"analysisMs\":" << r.analysisMs
                    << ",\"timelineMs\":" << r.timelineMs
                    << ",\"sampleUsOff\":" << r.sampleUsOff
                    << ",\"sampleUsExpr\":" << r.sampleUsExpressive << "}";
                out << (i + 1 < reports.size() ? ",\n" : "\n");
            }
            out << "  ]\n}\n";
        }
    }
    return 0;
}
