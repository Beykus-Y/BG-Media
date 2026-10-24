#pragma once

#include "bgmedia/version.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia {

struct WaveformPeak {
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

// Compact, cache-friendly spectrogram. Values are 0..255 mapped from dB
// relative to the loudest analysed band; it is a display/overview artifact,
// not a replacement for the raw STFT.
struct Spectrogram {
    int bands = 0;
    double framesPerSecond = 0.0;
    std::uint32_t frameCount = 0;
    std::vector<std::uint8_t> magnitudes; // frameCount * bands, row-major

    [[nodiscard]] bool valid() const noexcept {
        return bands > 0 && frameCount > 0 &&
               magnitudes.size() == static_cast<std::size_t>(bands) * frameCount;
    }
};

struct Onset {
    double time = 0.0;
    float strength = 0.0f;      // normalised 0..1
    float lowBandRatio = 0.0f;  // share of spectral flux below ~180 Hz
};

// A musical beat in source-media seconds. extrapolated beats are grid
// continuations through regions where the tracker had no strong evidence;
// they are included so choreography can keep a stable pulse, but renderers
// should show them with lower confidence.
struct Beat {
    double time = 0.0;
    float strength = 0.0f;
    float confidence = 0.0f;
    int beatInBar = 0;          // 0 is the downbeat
    int barIndex = 0;
    bool downbeat = false;
    bool extrapolated = false;
};

struct TempoSegment {
    double start = 0.0;
    double end = 0.0;
    double bpm = 0.0;
    float confidence = 0.0f;
};

// A region where the tempo is changing or the evidence conflicts. It is
// intentionally separate from TempoSegment: BG Media never presents a guessed
// transition as a measured stable tempo.
struct TempoTransition {
    double start = 0.0;
    double end = 0.0;
    double fromBpm = 0.0;
    double toBpm = 0.0;
    float confidence = 0.0f;
};

struct BassAccent {
    double time = 0.0;
    float strength = 0.0f;      // normalised low-frequency transient
    float lowBandRatio = 0.0f;  // share of linear energy below ~180 Hz at the accent
    float confidence = 0.0f;
    int nearestBeat = -1;       // index into AnalysisResult::beats
    double offsetFromBeat = 0.0;
};

struct SilenceRegion {
    double start = 0.0;
    double end = 0.0;
    float confidence = 0.0f;
};

enum class SectionKind {
    Unknown,
    Intro,
    Build,
    Main,
    Breakdown,
    Outro
};

struct Section {
    double start = 0.0;
    double end = 0.0;
    SectionKind kind = SectionKind::Unknown;
    float energy = 0.0f;        // 0..1, relative to track percentile
    float confidence = 0.0f;    // deliberately conservative
};

struct AnalysisInfo {
    std::uint32_t schemaVersion = kAnalysisSchemaVersion;
    std::uint32_t analyzerVersion = kAnalyzerVersion;
    std::uint64_t contentHash = 0;
    std::uint64_t configHash = 0;
    double duration = 0.0;
    std::uint32_t sampleRate = 0;
    std::uint32_t sourceChannels = 0;
};

struct AnalysisResult {
    AnalysisInfo info;

    // Dominant tempo. bpmCandidates contains the strongest alternatives,
    // including octave-related ones when the signal is ambiguous.
    double bpm = 0.0;
    float bpmConfidence = 0.0f;
    bool tempoStable = true;
    bool octaveAmbiguous = false;
    std::vector<double> bpmCandidates;
    std::vector<float> bpmCandidateScores;

    int beatsPerBar = 4;
    float meterConfidence = 0.0f;

    std::vector<TempoSegment> tempoSegments;
    std::vector<TempoTransition> tempoTransitions;

    std::vector<Beat> beats;
    std::vector<Onset> onsets;
    std::vector<BassAccent> bassAccents;
    std::vector<SilenceRegion> silences;
    std::vector<Section> sections;

    std::vector<WaveformPeak> waveform;
    float waveformPeaksPerSecond = 0.0f;
    Spectrogram spectrogram;

    double analysisSeconds = 0.0;
    double activeRatio = 0.0; // non-silent share of the track

    [[nodiscard]] bool empty() const noexcept { return info.duration <= 0.0; }

    // Stable segments count; useful for the UI's honesty label.
    [[nodiscard]] bool hasTempoChange() const noexcept {
        return tempoSegments.size() > 1 || !tempoTransitions.empty();
    }
};

enum class AnalysisStatus {
    Ok,
    Cancelled,
    InvalidInput,
    Error
};

struct AnalysisReport {
    AnalysisStatus status = AnalysisStatus::Error;
    AnalysisResult result;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return status == AnalysisStatus::Ok; }
};

} // namespace bgmedia
