#pragma once

// BG Vox Transcribe: offline speech-to-text for PCM that the host application
// has already captured or decoded. The library never records, downloads
// models, talks to a network or knows any messenger API; the host owns the
// audio, the model files, the consent UI and what is done with the text.
//
// Contract in one paragraph: transcribe() is noexcept, re-entrant and has no
// global state. It validates the PCM (same limits as BG Vox Enhance), mixes it
// down to mono, resamples it with a band-limited (windowed-sinc) filter to the
// rate the backend wants, splits long audio at the quietest pause so words are
// not cut, calls the backend once per chunk, shifts the timestamps back to the
// recording timeline and returns ONE VoxTranscript. The text is always a
// MACHINE transcript: VoxTranscript::machineGenerated is true, every segment
// carries a confidence (0..1, negative = unknown) and a lowConfidence flag,
// and the host must never present it as a reliable copy of what was said.
//
// The ASR engine itself is an optional adapter behind IVoxAsrBackend
// (see VoxWhisper.hpp). This header has no heavy dependency.

#include "bgmedia/vox/VoxEnhance.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::vox {

enum class VoxTranscribeStatus {
    Ok = 0,
    Cancelled = 1,
    InvalidInput = 2,        // null pointer, zero frames, NaN/Inf samples, bad options
    UnsupportedFormat = 3,   // sample rate / channel count outside the P1 limits
    TooLong = 4,             // longer than options.maxDurationSeconds
    NoBackend = 5,           // null backend, or the adapter is not built in
    ModelUnavailable = 6,    // model file missing / unreadable / unsupported
    BackendError = 7,        // the backend reported a failure
    OutOfMemory = 8,
    InternalError = 9,       // unexpected exception
};

[[nodiscard]] const char* voxTranscribeStatusName(VoxTranscribeStatus status) noexcept;

struct VoxTranscribeOptions {
    // BCP-47-ish language tag ("ru", "en", "pt-BR": only the primary subtag is
    // used by the backends). Empty = auto-detect on the first chunk with speech;
    // the detected language is then fixed for the remaining chunks so a long
    // message does not flip between languages.
    std::string language;
    // Ask the backend to translate to English instead of transcribing. Only
    // honoured when the backend reports supportsTranslation.
    bool translateToEnglish = false;
    // Fill VoxSegment::words (only if the backend supports it; costs CPU).
    bool wordTimestamps = false;
    // Longer input is rejected with TooLong. Bounds memory and run time.
    double maxDurationSeconds = 1800.0;
    // CPU threads for the backend; 0 = backend default.
    int threads = 0;
    // Target chunk length (clamped to 2..600 s). Audio longer than this is cut
    // at the quietest pause in the last half of the window; only when no pause
    // exists the cut is hard, with a short overlap that is de-duplicated.
    double chunkSeconds = 30.0;
    // Optional text that biases the decoder (names, jargon). Backend-specific.
    std::string initialPrompt;
    // Segments with a known confidence below this, and segments with unknown
    // confidence, get lowConfidence = true.
    float lowConfidenceThreshold = 0.5f;
};

struct VoxWord {
    double startSec = 0.0;
    double endSec = 0.0;
    std::string text;
    float confidence = -1.0f;  // 0..1, < 0 unknown
};

struct VoxSegment {
    double startSec = 0.0;     // on the recording timeline
    double endSec = 0.0;
    std::string text;          // UTF-8
    float confidence = -1.0f;  // 0..1, < 0 unknown (mean token probability for whisper)
    std::string language;      // language the segment was decoded in, may be empty
    std::vector<VoxWord> words;
    bool lowConfidence = true; // set by the frontend, see lowConfidenceThreshold
};

struct VoxTranscript {
    // Completed chunks only. When status != Ok this is what was finished
    // before the stop and may be shorter than the recording.
    std::vector<VoxSegment> segments;
    // Language of the text: the forced options.language or the auto-detected one.
    std::string detectedLanguage;
    // Always true. The text is produced by a model and may be wrong; the host
    // must label it as such and must not treat it as the original.
    bool machineGenerated = true;
    std::string backendName;
    std::string modelName;
    VoxTranscribeStatus status = VoxTranscribeStatus::InternalError;
    std::string detail;
    double audioSeconds = 0.0;
    double processingMs = 0.0;

    // Segment texts trimmed and joined with single spaces.
    [[nodiscard]] std::string fullText() const;
};

struct VoxAsrBackendInfo {
    std::string modelName;
    // Rough resident memory with the model loaded and one chunk in flight. An
    // ESTIMATE for the host's "will this fit" check, not a measurement.
    std::uint64_t approxRamBytes = 0;
    std::uint64_t modelFileBytes = 0;
    bool multilingual = false;
    // Explicit language list; may be empty when multilingual is true ("all the
    // languages of the model") or when the model is single-language and the
    // list is simply not provided.
    std::vector<std::string> languages;
    bool supportsTranslation = false;
    bool supportsWordTimestamps = false;
};

// An ASR engine. The frontend calls it from the thread that called
// transcribe(), one chunk at a time. An instance is NOT required to be
// thread-safe: the host either serialises calls or creates one instance per
// thread. Loading a model is the host's job (create the backend once, reuse
// it for many transcribe() calls, destroy it to free the model memory).
class IVoxAsrBackend {
public:
    virtual ~IVoxAsrBackend() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Sample rate of the mono float PCM transcribeChunk() expects (16000 for
    // whisper). Must be in [kVoxMinSampleRate, kVoxMaxSampleRate].
    [[nodiscard]] virtual std::uint32_t expectedSampleRate() const noexcept = 0;
    [[nodiscard]] virtual VoxAsrBackendInfo info() const = 0;

    // Transcribe one chunk of mono PCM at expectedSampleRate(), values in
    // [-1, 1]. Append segments to `out` with times RELATIVE TO THE CHUNK START
    // (the frontend shifts and clamps them). Words are optional and, if given,
    // also chunk-relative. Return Ok, Cancelled (when cancel->isCancelled()
    // became true) or an error status with a human-readable `error`. Should
    // poll `cancel` while working and may call `chunkProgress` (fraction 0..1
    // of this chunk) from the calling thread; both pointers may be null.
    // Exceptions are tolerated (the frontend maps them to InternalError).
    virtual VoxTranscribeStatus transcribeChunk(
        const float* mono, std::size_t frames, const VoxTranscribeOptions& options,
        const VoxCancelToken* cancel, const VoxProgressFn* chunkProgress,
        std::vector<VoxSegment>& out, std::string& error) = 0;
};

// Frontend. `interleaved` holds frames * format.channels samples (float in
// [-1, 1], or int16 converted as x / 32768). The caller owns the buffer for
// the duration of the call. Fail-safe: never throws; every problem is a
// status in the result. Progress is reported from the calling thread about
// every 5 percent and once at 1.0.
[[nodiscard]] VoxTranscript transcribe(
    IVoxAsrBackend* backend, const float* interleaved, std::size_t frames,
    VoxPcmFormat format, const VoxTranscribeOptions& options,
    const VoxCancelToken* cancel = nullptr, const VoxProgressFn& progress = {}) noexcept;

[[nodiscard]] VoxTranscript transcribe(
    IVoxAsrBackend* backend, const std::int16_t* interleaved, std::size_t frames,
    VoxPcmFormat format, const VoxTranscribeOptions& options,
    const VoxCancelToken* cancel = nullptr, const VoxProgressFn& progress = {}) noexcept;

} // namespace bgmedia::vox
