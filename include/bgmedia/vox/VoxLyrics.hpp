#pragma once

// BG Vox Lyrics: lyric lines with time stamps and PROVENANCE for a song the
// host already has as PCM. The library never fetches lyrics, never knows a
// music service, Telegram or any provider API, never stores anything and has
// no global state. Lyrics TEXT comes from the host (a BeyGram adapter that
// asks the music source for legally available lyrics/sync); this library
// parses it, aligns it to the audio and labels how every line got its text and
// its times.
//
// Honesty rules baked into the types:
//  * VoxLyrics::isOriginalText is true ONLY when the text came from the host
//    as provider text (ProviderSynced / ProviderUnsynced / aligned provider
//    text). Text produced by ASR (AsrHypothesis) is never original.
//  * Times produced here (AlignedToAudio) are estimates, not the provider's
//    sync; every line carries a confidence (0..1, < 0 unknown) and an
//    `interpolated` flag when its times were not measured.
//  * Singing is much harder than speech for ASR and for any heuristic: there
//    is no accuracy promise for sung audio, polyphony or dense mixes.
//  * hostMayCache / providerTag are set BY THE HOST from the provider's terms.
//    The library only carries them; it caches and stores nothing.
//
// All entry points are noexcept (exceptions are mapped to a status), re-entrant
// and take the PCM as borrowed buffers (same limits as BG Vox Enhance).

#include "bgmedia/vox/VoxEnhance.hpp"
#include "bgmedia/vox/VoxTranscribe.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bgmedia::vox {

enum class VoxLyricsStatus {
    Ok = 0,
    Cancelled = 1,
    InvalidInput = 2,       // null pointer, zero frames, NaN/Inf samples, bad options
    UnsupportedFormat = 3,  // sample rate / channels outside the P1 limits
    TooLong = 4,            // audio or text over the limits
    ParseError = 5,         // not LRC / nothing usable in the text
    NoLyrics = 6,           // no text and no ASR result to build lines from
    NoVocals = 7,           // no vocal regions found, nothing to align to
    AlignmentFailed = 8,    // text and audio could not be matched (no anchors)
    NoBackend = 9,          // ASR requested but no backend / separator failed to load
    BackendError = 10,      // ASR backend or separator reported a failure
    OutOfMemory = 11,
    InternalError = 12,
};

[[nodiscard]] const char* voxLyricsStatusName(VoxLyricsStatus status) noexcept;

// How a line got its text and its times, from most to least trustworthy.
enum class VoxLyricsSource {
    ProviderSynced = 0,    // text and times from the provider (host-supplied LRC)
    ProviderUnsynced = 1,  // provider text, no times
    AlignedToAudio = 2,    // provider text, times estimated here from the audio
    AsrHypothesis = 3,     // text recognised by ASR: a machine guess, not the lyrics
};

[[nodiscard]] const char* voxLyricsSourceName(VoxLyricsSource source) noexcept;

struct VoxLyricLine {
    double startSec = -1.0;  // < 0 unknown
    double endSec = -1.0;    // < 0 unknown
    std::string text;        // UTF-8; empty for stanza breaks and instrumental gaps
    float confidence = -1.0f;  // 0..1, < 0 unknown. Provider times: 1.
    VoxLyricsSource source = VoxLyricsSource::ProviderUnsynced;
    bool instrumental = false;  // explicit instrumental part (text empty)
    std::vector<VoxWord> words;  // optional word times (enhanced LRC or alignment)
    bool stanzaBreak = false;    // blank line between stanzas in the host's text
    bool interpolated = false;   // times guessed from neighbours / vocal regions

    [[nodiscard]] bool hasTimes() const noexcept { return startSec >= 0.0; }
    // True for unknown confidence and for confidence below `threshold`.
    [[nodiscard]] bool isLowConfidence(float threshold = 0.5f) const noexcept {
        return confidence < 0.0f || confidence < threshold;
    }
};

struct VoxLyrics {
    std::vector<VoxLyricLine> lines;
    // Weakest provenance of the content: ProviderSynced only if every line is.
    VoxLyricsSource source = VoxLyricsSource::ProviderUnsynced;
    // Opaque host label of where the text came from ("" if not given). Never
    // interpreted by the library.
    std::string providerTag;
    // Set by the HOST from the provider's terms. The library never caches.
    bool hostMayCache = false;
    // True only when the text is the provider's text (Provider*, or provider
    // text with aligned times). False for AsrHypothesis.
    bool isOriginalText = false;
    VoxLyricsStatus status = VoxLyricsStatus::InternalError;
    std::string detail;

    [[nodiscard]] bool isSynced() const noexcept;
};

// ---------------------------------------------------------------- parsing --

// LRC: [mm:ss.xx] / [mm:ss.xxx] / [mm:ss], several stamps per line, [offset:+-ms]
// (positive = lyrics earlier: effective = stamp - offset, as most players do),
// metadata tags ([ti:], [ar:], ...) ignored, enhanced word tags <mm:ss.xx>
// turned into VoxLyricLine::words. Lines are sorted by start; endSec = next
// line start (last line: unknown). Invalid UTF-8 is replaced by U+FFFD. Lines
// without a stamp are ignored and counted in `detail`. Garbage or text with no
// stamp at all gives ParseError. Never throws. Result: source ProviderSynced,
// isOriginalText true (the HOST vouches that the text is provider text).
[[nodiscard]] VoxLyrics parseLrc(std::string_view text) noexcept;

// Plain text: one line per text line, blank lines kept as empty lines with
// stanzaBreak = true (consecutive blanks collapse to one, leading/trailing
// blanks dropped). No times. source ProviderUnsynced, isOriginalText true.
[[nodiscard]] VoxLyrics parsePlainLyrics(std::string_view text) noexcept;

// LRC for every line with known times. Instrumental lines are written as "♪",
// stanza breaks as empty text. Word times are written as <mm:ss.xx> tags when
// includeWords. Non-provider sources get "[x-bg-source:...]" / "[x-bg-original:0]"
// metadata so that the file keeps its provenance label (parseLrc ignores them).
[[nodiscard]] std::string toLrc(const VoxLyrics& lyrics, bool includeWords = false);

// ------------------------------------------------- vocal activity (music) --

struct VoxVocalRegion {
    double startSec = 0.0;
    double endSec = 0.0;
    float confidence = 0.0f;  // 0..1, mean frame score of the region
};

// HEURISTIC detector for a lead voice in MUSIC (not the speech VAD of P1/P2).
// Per 32 ms frame: share of energy in 200..4000 Hz, harmonicity (normalised
// autocorrelation peak of the band-limited frame at 80..1000 Hz lags) and a
// level gate; the score is smoothed, thresholded with hysteresis, short gaps
// are merged and short regions dropped. Works on a clear lead vocal over a
// sparse bed; it has NO separation model and is unreliable on dense mixes,
// harmonic instruments in the vocal band (piano, guitar, strings, pads), choirs
// and rap/whisper. See docs/BG_VOX_LYRICS.md for what was and was not measured.
struct VoxVocalDetectOptions {
    double onThreshold = 0.50;
    double offThreshold = 0.35;
    double minRegionSeconds = 0.6;
    double mergeGapSeconds = 0.5;
    // Frames more than this many dB below the loud (95th percentile) frames
    // cannot be vocal.
    double levelGateDb = 35.0;
    double maxDurationSeconds = 1800.0;
};

struct VoxVocalRegions {
    std::vector<VoxVocalRegion> regions;
    double audioSeconds = 0.0;
    VoxLyricsStatus status = VoxLyricsStatus::InternalError;
    std::string detail;
    // Sum of region lengths / audioSeconds.
    [[nodiscard]] double vocalFraction() const noexcept;
};

[[nodiscard]] VoxVocalRegions detectVocalRegions(
    const float* interleaved, std::size_t frames, VoxPcmFormat format,
    const VoxVocalDetectOptions& options = {}, const VoxCancelToken* cancel = nullptr,
    const VoxProgressFn& progress = {}) noexcept;

[[nodiscard]] VoxVocalRegions detectVocalRegions(
    const std::int16_t* interleaved, std::size_t frames, VoxPcmFormat format,
    const VoxVocalDetectOptions& options = {}, const VoxCancelToken* cancel = nullptr,
    const VoxProgressFn& progress = {}) noexcept;

// --------------------------------------------------------- separation hook --

// Optional vocal/accompaniment separation, a HOOK ONLY: no implementation is
// shipped. Candidates (Demucs, Spleeter, UVR/MDX-Net) and their licences and
// weight are discussed in docs/BG_VOX_LYRICS.md. The host (or an adapter)
// implements it; buildLyrics() then runs vocal detection and ASR on the
// separated vocal stem.
class IVoxVocalSeparator {
public:
    virtual ~IVoxVocalSeparator() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Mono float PCM at `sampleRate` in, vocal stem out (same rate and length;
    // the frontend rejects other lengths). Return Ok, Cancelled or an error
    // status with a message in `error`. Exceptions are tolerated.
    virtual VoxLyricsStatus separate(const float* mono, std::size_t frames,
                                     std::uint32_t sampleRate, const VoxCancelToken* cancel,
                                     const VoxProgressFn* progress,
                                     std::vector<float>& vocalsMono, std::string& error) = 0;
};

// -------------------------------------------------------------- alignment --

struct VoxAlignOptions {
    // Lines with a confidence below this are "low confidence" (host UI should
    // not show them as synced). Also the per-line cut used for the summary.
    float lowConfidenceThreshold = 0.5f;
    // A line with a matched-token fraction below this is not trusted as an
    // anchor: its times are interpolated from the neighbours.
    float minLineMatch = 0.34f;
    // Alignment matrix cap (lyric tokens x ASR tokens).
    std::size_t maxMatrixCells = 16u * 1000u * 1000u;
};

// Align host-supplied unsynced text to an ASR transcript: tokens are
// lower-cased and stripped of punctuation (Latin and Cyrillic aware, ё = е),
// matched with a global/semi-global dynamic programme with fuzzy token
// similarity, and line times are taken from the matched ASR words (or from
// segments split evenly by characters when no words exist). Unmatched lines are
// interpolated between anchors and flagged (interpolated, low confidence).
// Result: source AlignedToAudio, isOriginalText = unsynced.isOriginalText,
// providerTag / hostMayCache copied. No anchor at all: AlignmentFailed and the
// lines come back unsynced.
[[nodiscard]] VoxLyrics alignLyricsToTranscript(const VoxLyrics& unsynced,
                                                const VoxTranscript& asr,
                                                const VoxAlignOptions& options = {},
                                                const VoxCancelToken* cancel = nullptr) noexcept;

// Fallback without ASR: spreads the lines over the vocal regions in proportion
// to their vowel count (ru/en; character-based when there are no vowels).
// Confidence is at most 0.3 and every line is `interpolated`.
[[nodiscard]] VoxLyrics alignLyricsToVocalRegions(const VoxLyrics& unsynced,
                                                  const std::vector<VoxVocalRegion>& regions,
                                                  const VoxAlignOptions& options = {}) noexcept;

// No text at all: one AsrHypothesis line per transcript segment. isOriginalText
// is false and every line keeps the ASR confidence. NoLyrics when empty.
[[nodiscard]] VoxLyrics lyricsFromTranscript(const VoxTranscript& asr) noexcept;

// Adds explicit instrumental lines (instrumental = true, empty text) for gaps
// longer than minGapSeconds between vocal regions, including the intro and the
// outro up to audioSeconds, unless a timed lyric line starts inside the gap.
// Keeps the lines sorted by start; lines without times stay at the end.
void markInstrumentalGaps(VoxLyrics& lyrics, const std::vector<VoxVocalRegion>& regions,
                          double audioSeconds, double minGapSeconds) noexcept;

// --------------------------------------------------------------- pipeline --

// What the host knows about the lyrics text. Leave `text` empty when there is
// none. Set `synced` when `text` is LRC.
struct VoxProvidedLyrics {
    std::string text;
    bool synced = false;
    std::string providerTag;
    bool hostMayCache = false;
};

struct VoxBuildOptions {
    VoxVocalDetectOptions vocal;
    VoxAlignOptions align;
    VoxTranscribeOptions asr;           // wordTimestamps is forced on for alignment
    double instrumentalGapSeconds = 8.0;
    bool detectVocals = true;           // needed for the fallback and for gap lines
};

// Pipeline. `interleaved` may be null only when provided.synced and the LRC
// parses (the text is returned as it is, nothing else runs).
//   provider synced            -> parseLrc, as is
//   provider unsynced + backend -> [separator] -> vocal regions -> transcribe()
//                                  -> alignLyricsToTranscript; falls back to
//                                  alignLyricsToVocalRegions when ASR or the
//                                  alignment fails (detail says so)
//   provider unsynced, no backend -> vocal regions -> alignLyricsToVocalRegions
//   no text + backend          -> lyricsFromTranscript (AsrHypothesis)
//   no text, no backend        -> NoLyrics
// Instrumental gap lines are added for aligned / hypothesis results.
[[nodiscard]] VoxLyrics buildLyrics(
    const VoxProvidedLyrics& provided, IVoxAsrBackend* backend, IVoxVocalSeparator* separator,
    const float* interleaved, std::size_t frames, VoxPcmFormat format,
    const VoxBuildOptions& options = {}, const VoxCancelToken* cancel = nullptr,
    const VoxProgressFn& progress = {}) noexcept;

} // namespace bgmedia::vox
