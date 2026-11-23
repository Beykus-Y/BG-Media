#include "bgmedia/vox/VoxTranscribe.hpp"

#include "VoxDsp.hpp"
#include "VoxResample.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace bgmedia::vox {

const char* voxTranscribeStatusName(VoxTranscribeStatus status) noexcept {
    switch (status) {
    case VoxTranscribeStatus::Ok: return "Ok";
    case VoxTranscribeStatus::Cancelled: return "Cancelled";
    case VoxTranscribeStatus::InvalidInput: return "InvalidInput";
    case VoxTranscribeStatus::UnsupportedFormat: return "UnsupportedFormat";
    case VoxTranscribeStatus::TooLong: return "TooLong";
    case VoxTranscribeStatus::NoBackend: return "NoBackend";
    case VoxTranscribeStatus::ModelUnavailable: return "ModelUnavailable";
    case VoxTranscribeStatus::BackendError: return "BackendError";
    case VoxTranscribeStatus::OutOfMemory: return "OutOfMemory";
    case VoxTranscribeStatus::InternalError: return "InternalError";
    }
    return "Unknown";
}

namespace {

using namespace detail;

std::string trimmed(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Replaces invalid UTF-8 (stray continuation bytes, truncated or overlong
// sequences, surrogates, > U+10FFFF) with U+FFFD. whisper may split a multibyte
// character across two segments, so segment text is not always valid UTF-8.
std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto cont = [&](std::size_t k) { return k < n && (p[k] & 0xC0u) == 0x80u; };
    while (i < n) {
        const unsigned char c = p[i];
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
            const unsigned cp = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
            if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                len = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            const unsigned cp = ((c & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) |
                                ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                len = 4;
            }
        }
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s, i, len);
            i += len;
        }
    }
    return out;
}

// Assigns a status detail without ever throwing (callers are noexcept).
void setDetail(VoxTranscript& res, const char* text) noexcept {
    try {
        res.detail = text;
    } catch (...) {
        res.detail.clear();
    }
}

struct Exit {
    VoxTranscribeStatus status;
    const char* detail;
};

class Progress {
public:
    explicit Progress(const VoxProgressFn& fn) : fn_(fn) {}
    void report(double fraction) {
        if (!fn_) {
            return;
        }
        const double f = std::min(1.0, std::max(0.0, fraction));
        if (f >= 1.0 ? !done_ : f - last_ >= 0.05) {
            last_ = f;
            done_ = f >= 1.0;
            fn_(static_cast<float>(f));
        }
    }

private:
    const VoxProgressFn& fn_;
    double last_ = -1.0;
    bool done_ = false;
};

struct Chunk {
    std::size_t begin = 0;
    std::size_t end = 0;
    // Segments of this chunk whose midpoint is before `handover` belong to the
    // previous chunk (overlap de-duplication); equal to `begin` without overlap.
    std::size_t handover = 0;
};

constexpr std::size_t kMinActiveFrames = 5;  // 50 ms of speech activity per chunk
constexpr double kMinPauseSeconds = 0.08;
constexpr double kMaxOverlapSeconds = 0.5;

// Plans the chunk list over a mono signal at `rate`. `active` holds one VAD
// flag per 10 ms frame (no hangover) and `frameDb` the frame levels.
std::vector<Chunk> planChunks(std::size_t total, std::uint32_t rate, double chunkSeconds,
                              const std::vector<float>& frameDb,
                              const std::vector<std::uint8_t>& active) {
    std::vector<Chunk> chunks;
    const std::size_t frameLen = vadFrameLength(rate);
    const std::size_t maxFrames = std::max<std::size_t>(
        frameLen * 10, static_cast<std::size_t>(chunkSeconds * static_cast<double>(rate)));
    const std::size_t overlap = std::min(
        static_cast<std::size_t>(kMaxOverlapSeconds * static_cast<double>(rate)), maxFrames / 4);
    const std::size_t minPause =
        std::max<std::size_t>(1, static_cast<std::size_t>(kMinPauseSeconds * 100.0));  // in VAD frames

    std::size_t pos = 0;    // first frame not yet covered by an earlier chunk
    std::size_t carry = 0;  // overlap in front of `pos` after a hard cut
    while (pos < total) {
        Chunk c;
        c.begin = pos - carry;
        c.handover = c.begin + carry / 2;
        if (total - c.begin <= maxFrames) {
            c.end = total;
            chunks.push_back(c);
            break;
        }
        const std::size_t limit = c.begin + maxFrames;
        // Look for the quietest pause (run of inactive frames) in the second half.
        const std::size_t f0 = (c.begin + maxFrames / 2 + frameLen - 1) / frameLen;
        const std::size_t f1 = std::min(frameDb.size(), limit / frameLen);
        std::size_t cut = 0;
        double bestDb = 1e9;
        std::size_t bestLen = 0;
        for (std::size_t f = f0; f < f1;) {
            if (active[f] != 0) {
                ++f;
                continue;
            }
            std::size_t g = f;
            double sum = 0.0;
            while (g < f1 && active[g] == 0) {
                sum += static_cast<double>(frameDb[g]);
                ++g;
            }
            const std::size_t len = g - f;
            if (len >= minPause) {
                const double mean = sum / static_cast<double>(len);
                if (mean < bestDb - 0.5 || (mean < bestDb + 0.5 && len >= bestLen)) {
                    bestDb = std::min(bestDb, mean);
                    bestLen = len;
                    cut = (f + len / 2) * frameLen;
                }
            }
            f = g;
        }
        if (cut > pos && cut < total) {
            c.end = cut;
            carry = 0;
        } else {
            c.end = limit;
            carry = overlap;
        }
        chunks.push_back(c);
        pos = c.end;
    }
    return chunks;
}

void sanitizeSegments(std::vector<VoxSegment>& segs, double chunkStartSec, double chunkDurSec,
                      double audioSec, const std::string& lang, float lowThreshold) {
    auto fixConf = [](float c) { return std::isfinite(c) && c >= 0.0f ? std::min(c, 1.0f) : -1.0f; };
    auto fixTime = [&](double t) {
        if (!std::isfinite(t)) {
            t = 0.0;
        }
        t = std::min(std::max(t, 0.0), chunkDurSec);
        return std::min(t + chunkStartSec, audioSec);
    };
    // Whisper pads short chunks with silence and may hallucinate text there; such
    // segments start at or after the real end of the chunk and are dropped
    // (before clamping would turn them into zero-length segments at the end).
    constexpr double kEndEps = 0.01;
    const double cutoff = chunkDurSec - kEndEps;
    auto pastEnd = [&](double t) { return std::isfinite(t) && t >= cutoff; };
    segs.erase(std::remove_if(segs.begin(), segs.end(),
                              [&](const VoxSegment& s) { return pastEnd(s.startSec); }),
               segs.end());
    for (VoxSegment& s : segs) {
        s.text = trimmed(sanitizeUtf8(s.text));
        const double a = fixTime(s.startSec);
        const double b = fixTime(s.endSec);
        s.startSec = std::min(a, b);
        s.endSec = std::max(a, b);
        s.confidence = fixConf(s.confidence);
        s.lowConfidence = s.confidence < 0.0f || s.confidence < lowThreshold;
        if (s.language.empty()) {
            s.language = lang;
        }
        std::vector<VoxWord> words;
        words.reserve(s.words.size());
        for (VoxWord& w : s.words) {
            w.text = trimmed(sanitizeUtf8(w.text));
            if (w.text.empty() || pastEnd(w.startSec)) {
                continue;
            }
            const double wa = fixTime(w.startSec);
            const double wb = fixTime(w.endSec);
            w.startSec = std::min(wa, wb);
            w.endSec = std::max(wa, wb);
            w.confidence = fixConf(w.confidence);
            words.push_back(std::move(w));
        }
        s.words = std::move(words);
    }
    segs.erase(std::remove_if(segs.begin(), segs.end(),
                              [](const VoxSegment& s) { return s.text.empty(); }),
               segs.end());
    std::stable_sort(segs.begin(), segs.end(), [](const VoxSegment& a, const VoxSegment& b) {
        return a.startSec < b.startSec;
    });
}

inline bool sampleOk(float v) { return std::isfinite(v); }
inline bool sampleOk(std::int16_t) { return true; }
inline float sampleToFloat(float v) { return v; }
inline float sampleToFloat(std::int16_t v) { return static_cast<float>(v) / 32768.0f; }

template <typename Sample>
void run(IVoxAsrBackend* backend, const Sample* in, std::size_t frames, VoxPcmFormat format,
         const VoxTranscribeOptions& options, const VoxCancelToken* cancel,
         const VoxProgressFn& progressFn, VoxTranscript& res) {
    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };
    KeepGoing keep = [&]() { return !cancelled(); };

    if (backend == nullptr) {
        throw Exit{VoxTranscribeStatus::NoBackend, "no ASR backend"};
    }
    if (!isSupportedFormat(format)) {
        throw Exit{VoxTranscribeStatus::UnsupportedFormat, "unsupported sample rate or channel count"};
    }
    if (in == nullptr || frames == 0) {
        throw Exit{VoxTranscribeStatus::InvalidInput, "empty or null input"};
    }
    const std::size_t channels = format.channels;
    if (frames > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float)) / 2) {
        throw Exit{VoxTranscribeStatus::TooLong, "input too large"};
    }
    res.audioSeconds = static_cast<double>(frames) / static_cast<double>(format.sampleRate);
    if (!(options.maxDurationSeconds > 0.0) || !std::isfinite(options.maxDurationSeconds)) {
        throw Exit{VoxTranscribeStatus::InvalidInput, "maxDurationSeconds must be positive"};
    }
    if (res.audioSeconds > options.maxDurationSeconds) {
        throw Exit{VoxTranscribeStatus::TooLong, "input longer than maxDurationSeconds"};
    }
    if (cancelled()) {
        throw Exit{VoxTranscribeStatus::Cancelled, "cancelled before start"};
    }
    const std::uint32_t rate = backend->expectedSampleRate();
    if (rate < kVoxMinSampleRate || rate > kVoxMaxSampleRate) {
        throw Exit{VoxTranscribeStatus::BackendError, "backend sample rate outside 8000..48000"};
    }
    res.backendName = backend->name() != nullptr ? backend->name() : "";
    const VoxAsrBackendInfo backendInfo = backend->info();
    res.modelName = backendInfo.modelName;
    if (options.translateToEnglish && !backendInfo.supportsTranslation) {
        throw Exit{VoxTranscribeStatus::InvalidInput, "backend does not support translation"};
    }

    // ---- downmix to mono straight from the input type (finite check on the way);
    // no full-length float copy of the interleaved input is ever made ----
    std::vector<float> mono(frames);
    const std::size_t total = frames * channels;
    for (std::size_t i = 0, o = 0; i < total; i += channels, ++o) {
        float acc = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            const Sample raw = in[i + c];
            if (!sampleOk(raw)) {
                throw Exit{VoxTranscribeStatus::InvalidInput, "NaN or Inf in input"};
            }
            acc += sampleToFloat(raw);
        }
        mono[o] = channels == 2 ? acc * 0.5f : acc;
    }

    // ---- band-limited resampling to the backend rate ----
    std::vector<float> pcm;
    if (!resampleMono(mono.data(), frames, format.sampleRate, rate, pcm, keep)) {
        throw Exit{VoxTranscribeStatus::Cancelled, "cancelled"};
    }
    mono.clear();
    mono.shrink_to_fit();
    const std::size_t total16 = pcm.size();
    const double audioSec = res.audioSeconds;

    Progress progress(progressFn);
    progress.report(0.0);

    // ---- VAD for silence rejection and pause search ----
    const std::size_t frameLen = vadFrameLength(rate);
    std::vector<float> frameDb;
    frameMeanSquareDb(pcm.data(), total16, 1, frameLen, frameDb);
    VadParams vp;
    vp.hangoverMs = 0.0;
    vp.minRangeDb = 3.0;
    VadResult vad;
    runVad(frameDb, vp, vad);
    if (!vad.hasSpeech) {
        // Continuous speech with little level variation: retry with a small margin
        // so that it is not mistaken for stationary noise.
        vp.marginDb = 3.0;
        runVad(frameDb, vp, vad);
    }
    if (cancelled()) {
        throw Exit{VoxTranscribeStatus::Cancelled, "cancelled"};
    }
    if (!vad.hasSpeech) {
        res.status = VoxTranscribeStatus::Ok;
        res.detail = "no speech detected; backend not called";
        res.detectedLanguage = options.language;
        progress.report(1.0);
        return;
    }

    const double chunkSeconds = std::min(600.0, std::max(2.0, std::isfinite(options.chunkSeconds)
                                                                 ? options.chunkSeconds : 30.0));
    const std::vector<Chunk> chunks =
        planChunks(total16, rate, chunkSeconds, frameDb, vad.active);

    VoxTranscribeOptions chunkOpts = options;
    bool languageLocked = !options.language.empty();
    res.detectedLanguage = options.language;
    std::vector<VoxSegment> chunkSegs;
    std::string error;
    const double rateD = static_cast<double>(rate);

    for (const Chunk& c : chunks) {
        if (cancelled()) {
            res.status = VoxTranscribeStatus::Cancelled;
            res.detail = "cancelled";
            return;
        }
        // Skip chunks without speech activity (no hallucinated text from silence).
        std::size_t activeInChunk = 0;
        const std::size_t f0 = c.begin / frameLen;
        const std::size_t f1 = std::min(vad.active.size(), (c.end + frameLen - 1) / frameLen);
        for (std::size_t f = f0; f < f1; ++f) {
            activeInChunk += vad.active[f];
        }
        if (activeInChunk < kMinActiveFrames) {
            progress.report(static_cast<double>(c.end) / static_cast<double>(total16));
            continue;
        }

        chunkSegs.clear();
        error.clear();
        const double chunkStart = static_cast<double>(c.begin) / rateD;
        const double chunkDur = static_cast<double>(c.end - c.begin) / rateD;
        VoxProgressFn inner = [&](float fraction) {
            progress.report((static_cast<double>(c.begin) +
                             static_cast<double>(fraction) * static_cast<double>(c.end - c.begin)) /
                            static_cast<double>(total16));
        };
        VoxTranscribeStatus st;
        try {
            st = backend->transcribeChunk(pcm.data() + c.begin, c.end - c.begin, chunkOpts,
                                          cancel, &inner, chunkSegs, error);
        } catch (const std::bad_alloc&) {
            res.status = VoxTranscribeStatus::OutOfMemory;
            res.detail = "out of memory in backend";
            return;
        } catch (const std::exception& e) {
            res.status = VoxTranscribeStatus::InternalError;
            res.detail = std::string("backend threw: ") + e.what();
            return;
        } catch (...) {
            res.status = VoxTranscribeStatus::InternalError;
            res.detail = "backend threw an unknown exception";
            return;
        }
        if (st != VoxTranscribeStatus::Ok) {
            res.status = st;
            res.detail = !error.empty() ? error
                         : st == VoxTranscribeStatus::Cancelled ? "cancelled" : "backend failed";
            return;
        }

        sanitizeSegments(chunkSegs, chunkStart, chunkDur, audioSec, chunkOpts.language,
                         options.lowConfidenceThreshold);

        if (!languageLocked) {
            for (const VoxSegment& s : chunkSegs) {
                if (!s.language.empty()) {
                    chunkOpts.language = s.language;
                    res.detectedLanguage = s.language;
                    languageLocked = true;
                    break;
                }
            }
        }

        // Overlap de-duplication around the handover point. The previous chunk's
        // tail is replaced only when this chunk produced something past the
        // handover; otherwise the old tail is kept (never lose text).
        const double handover = static_cast<double>(c.handover) / rateD;
        const bool overlapped = c.handover > c.begin;
        auto midpoint = [](const VoxSegment& s) { return 0.5 * (s.startSec + s.endSec); };
        const bool hasNew = std::any_of(chunkSegs.begin(), chunkSegs.end(),
                                        [&](const VoxSegment& s) { return midpoint(s) >= handover; });
        if (overlapped && hasNew) {
            while (!res.segments.empty() && midpoint(res.segments.back()) >= handover) {
                res.segments.pop_back();
            }
        }
        for (VoxSegment& s : chunkSegs) {
            // Segments before the handover duplicate the previous chunk's tail,
            // which is kept either way.
            if (overlapped && midpoint(s) < handover) {
                continue;
            }
            res.segments.push_back(std::move(s));
        }
        progress.report(static_cast<double>(c.end) / static_cast<double>(total16));
    }
    res.status = VoxTranscribeStatus::Ok;
    progress.report(1.0);
}

} // namespace

std::string VoxTranscript::fullText() const {
    std::string out;
    for (const VoxSegment& s : segments) {
        const std::string t = trimmed(s.text);
        if (t.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += t;
    }
    return out;
}

namespace {

template <typename Sample>
VoxTranscript transcribeImpl(IVoxAsrBackend* backend, const Sample* interleaved, std::size_t frames,
                             VoxPcmFormat format, const VoxTranscribeOptions& options,
                             const VoxCancelToken* cancel, const VoxProgressFn& progress) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    VoxTranscript res;
    try {
        run(backend, interleaved, frames, format, options, cancel, progress, res);
    } catch (const Exit& e) {
        res.status = e.status;
        setDetail(res, e.detail);
    } catch (const std::bad_alloc&) {
        res.status = VoxTranscribeStatus::OutOfMemory;
        setDetail(res, "out of memory");
    } catch (const std::exception& e) {
        res.status = VoxTranscribeStatus::InternalError;
        setDetail(res, e.what());
    } catch (...) {
        res.status = VoxTranscribeStatus::InternalError;
        setDetail(res, "unknown exception");
    }
    res.processingMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

} // namespace

VoxTranscript transcribe(IVoxAsrBackend* backend, const float* interleaved, std::size_t frames,
                         VoxPcmFormat format, const VoxTranscribeOptions& options,
                         const VoxCancelToken* cancel, const VoxProgressFn& progress) noexcept {
    return transcribeImpl(backend, interleaved, frames, format, options, cancel, progress);
}

VoxTranscript transcribe(IVoxAsrBackend* backend, const std::int16_t* interleaved,
                         std::size_t frames, VoxPcmFormat format,
                         const VoxTranscribeOptions& options, const VoxCancelToken* cancel,
                         const VoxProgressFn& progress) noexcept {
    return transcribeImpl(backend, interleaved, frames, format, options, cancel, progress);
}

} // namespace bgmedia::vox
