#include "bgmedia/analysis/Analyzer.hpp"

#include "analysis/BeatTracker.hpp"
#include "analysis/Cancelled.hpp"
#include "analysis/Dsp.hpp"
#include "analysis/EventDetection.hpp"
#include "analysis/SpectralAnalysis.hpp"
#include "analysis/TempoEstimator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace bgmedia {
namespace {

void report(const AnalysisProgressCallback& progress, float fraction,
            const char* stage) {
    if (progress) {
        progress(std::clamp(fraction, 0.0f, 1.0f), stage);
    }
}

} // namespace

int resolveStftSize(int requested, std::uint32_t sampleRate) noexcept {
    if (requested > 0) {
        return std::clamp(dsp::nextPowerOfTwo(requested), 128, 16384);
    }
    if (sampleRate >= 32000) {
        return 2048;
    }
    if (sampleRate >= 16000) {
        return 1024;
    }
    return 512;
}

Analyzer::Analyzer(AnalysisConfig config) : config_(std::move(config)) {
    if (config_.minBpm < 20.0) {
        config_.minBpm = 20.0;
    }
    if (config_.maxBpm > 400.0) {
        config_.maxBpm = 400.0;
    }
    if (config_.maxBpm <= config_.minBpm) {
        config_.maxBpm = config_.minBpm + 10.0;
    }
    if (config_.minSilenceSeconds < 0.1) {
        config_.minSilenceSeconds = 0.1;
    }
    if (config_.tempoWindowSeconds < 2.0) {
        config_.tempoWindowSeconds = 2.0;
    }
    if (config_.tempoHopSeconds < 0.25) {
        config_.tempoHopSeconds = 0.25;
    }
}

AnalysisReport Analyzer::analyze(const PcmBuffer& pcm,
                                 const AnalysisProgressCallback& progress,
                                 const AnalysisCancelCallback& cancel) const {
    const auto started = std::chrono::steady_clock::now();
    AnalysisReport reportOut;
    if (!pcm.valid()) {
        reportOut.status = AnalysisStatus::InvalidInput;
        reportOut.message = "invalid or empty PCM buffer";
        return reportOut;
    }

    auto cancelled = [&]() {
        return cancel && cancel();
    };

    try {
        report(progress, 0.0f, "preparing analysis");
        if (cancelled()) {
            reportOut.status = AnalysisStatus::Cancelled;
            reportOut.message = "analysis cancelled";
            return reportOut;
        }

        AnalysisResult result;
        result.info.schemaVersion = kAnalysisSchemaVersion;
        result.info.analyzerVersion = kAnalyzerVersion;
        result.info.contentHash = pcm.contentHash;
        result.info.configHash = config_.fingerprint();
        result.info.duration = pcm.duration();
        result.info.sampleRate = pcm.sampleRate;
        result.info.sourceChannels = pcm.sourceChannels;

        SpectralFeatures features = computeSpectralFeatures(
            pcm, config_,
            [&](float fraction, const std::string& stage) {
                report(progress, 0.04f + 0.54f * fraction, stage.c_str());
            },
            cancel);
        if (cancelled()) {
            throw AnalysisCancelled{};
        }

        report(progress, 0.60f, "detecting silence");
        SilenceAnalysis silence = detectSilence(features, config_, pcm.duration());
        result.silences = silence.regions;
        result.activeRatio = silence.activeRatio;

        for (std::size_t i = 0; i < features.env.size(); ++i) {
            if (i < silence.active.size() && silence.active[i] == 0) {
                features.env[i] = 0.0f;
                features.sharpEnv[i] = 0.0f;
                features.lowEnv[i] = 0.0f;
                features.lowRatio[i] = 0.0f;
                features.lowEnergyRatio[i] = 0.0f;
                features.flux[i] = 0.0f;
                features.lowFlux[i] = 0.0f;
            }
        }

        // Tempo evidence is deliberately low-frequency weighted. Hi-hats and
        // other subdivisions live above 180 Hz and can otherwise pull the
        // octave estimate to double time even though the kick pattern is 92
        // BPM. Weak broadband onsets are still retained through the 0.35*env
        // floor, so bass-light material remains analysable.
        std::vector<float> tempoEnvelope(features.env.size(), 0.0f);
        for (std::size_t i = 0; i < features.env.size(); ++i) {
            tempoEnvelope[i] = std::max(features.lowEnv[i], 0.35f * features.env[i]);
        }

        report(progress, 0.66f, "estimating tempo");
        TempoEstimate globalTempo = estimateTempo(tempoEnvelope, features.framesPerSecond,
                                                  config_, silence.activeRatio);
        if (cancelled()) {
            throw AnalysisCancelled{};
        }

        report(progress, 0.74f, "analysing tempo changes");
        TempoStructure structure = estimateTempoStructure(
            tempoEnvelope, silence.active, features.framesPerSecond, pcm.duration(),
            config_, globalTempo);
        if (cancelled()) {
            throw AnalysisCancelled{};
        }

        result.bpm = structure.bpm;
        result.bpmConfidence = structure.confidence;
        result.tempoStable = structure.stable;
        result.octaveAmbiguous = structure.octaveAmbiguous;
        result.tempoSegments = structure.segments;
        result.tempoTransitions = structure.transitions;
        result.bpmCandidates = structure.candidates;
        result.bpmCandidateScores = structure.candidateScores;

        if (structure.bpm > 0.0) {
            report(progress, 0.82f, "tracking beats");
            BeatTrackingResult beatResult = trackBeats(
                features, silence.active, result.silences, structure, pcm.duration(),
                config_);
            if (cancelled()) {
                throw AnalysisCancelled{};
            }
            result.beats = std::move(beatResult.beats);
            result.onsets = std::move(beatResult.onsets);
            result.beatsPerBar = beatResult.beatsPerBar;
            result.meterConfidence = beatResult.meterConfidence;

            report(progress, 0.91f, "classifying bass accents");
            result.bassAccents = detectBassAccents(features, result.silences,
                                                   result.beats, pcm.duration(), config_);
        } else {
            // No reliable pulse (silence, extremely short or arrhythmic
            // material): still expose onsets for the timeline, but never invent
            // a BPM or beat grid.
            result.onsets = detectOnsets(features, result.silences, pcm.duration(), config_);
            result.beatsPerBar = config_.beatsPerBarHint;
            result.meterConfidence = 0.0f;
            result.tempoStable = false;
        }

        if (cancelled()) {
            throw AnalysisCancelled{};
        }

        report(progress, 0.95f, "finding sections");
        result.sections = detectSections(features, pcm.duration(), config_);

        result.waveform = std::move(features.waveform);
        result.waveformPeaksPerSecond = features.waveformPeaksPerSecond;
        result.spectrogram = std::move(features.spectrogram);

        const auto finished = std::chrono::steady_clock::now();
        result.analysisSeconds =
            std::chrono::duration<double>(finished - started).count();

        report(progress, 1.0f, "analysis complete");
        reportOut.status = AnalysisStatus::Ok;
        reportOut.result = std::move(result);
        return reportOut;
    } catch (const AnalysisCancelled&) {
        reportOut.status = AnalysisStatus::Cancelled;
        reportOut.message = "analysis cancelled";
        return reportOut;
    } catch (const std::exception& ex) {
        reportOut.status = AnalysisStatus::Error;
        reportOut.message = ex.what();
        return reportOut;
    } catch (...) {
        reportOut.status = AnalysisStatus::Error;
        reportOut.message = "unknown analysis error";
        return reportOut;
    }
}

} // namespace bgmedia
