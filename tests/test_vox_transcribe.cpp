#include "framework.hpp"

#include "bgmedia/vox/VoxTranscribe.hpp"
#include "bgmedia/vox/vox_transcribe_capi.h"
#include "support/VoxSynthetic.hpp"
#include "vox/VoxResample.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <vector>

namespace {

using namespace bgmedia::vox;
using namespace bgmedia::voxsynth;


// Deterministic stand-in for an ASR engine. Records every chunk it is given.
class FakeBackend final : public IVoxAsrBackend {
public:
    enum class Mode { Simple, Probe, Tiling, Custom };
    // Mode::Custom: fills `out` for call `index` of a chunk lasting `dur` seconds.
    std::function<void(int index, double dur, std::vector<VoxSegment>& out)> custom;
    bool translation = false;
    Mode mode = Mode::Simple;
    std::uint32_t rate = 16000;
    std::vector<float> confidences = {0.9f};  // cycled per segment
    std::string language;                     // reported per segment
    int failAtCall = -1;                      // 0-based call index
    VoxTranscribeStatus failStatus = VoxTranscribeStatus::BackendError;
    int throwAtCall = -1;
    bool throwBadAlloc = false;
    int cancelAtCall = -1;
    VoxCancelToken* tokenToCancel = nullptr;

    struct Call {
        std::size_t frames = 0;
        std::string optionsLanguage;
        std::vector<float> samples;
    };
    std::vector<Call> calls;

    const char* name() const noexcept override { return "fake"; }
    std::uint32_t expectedSampleRate() const noexcept override { return rate; }
    VoxAsrBackendInfo info() const override {
        VoxAsrBackendInfo i;
        i.modelName = "fake-model";
        i.approxRamBytes = 1234;
        i.modelFileBytes = 99;
        i.multilingual = true;
        i.supportsTranslation = translation;
        return i;
    }

    VoxTranscribeStatus transcribeChunk(const float* mono, std::size_t frames,
                                        const VoxTranscribeOptions& options,
                                        const VoxCancelToken* cancel,
                                        const VoxProgressFn* chunkProgress,
                                        std::vector<VoxSegment>& out,
                                        std::string& error) override {
        (void)cancel;
        const int index = static_cast<int>(calls.size());
        Call c;
        c.frames = frames;
        c.optionsLanguage = options.language;
        c.samples.assign(mono, mono + frames);
        calls.push_back(std::move(c));
        if (chunkProgress != nullptr && *chunkProgress) {
            (*chunkProgress)(0.5f);
        }
        if (index == throwAtCall) {
            if (throwBadAlloc) {
                throw std::bad_alloc();
            }
            throw std::runtime_error("fake exploded");
        }
        if (index == failAtCall) {
            error = "boom";
            return failStatus;
        }
        const double dur = static_cast<double>(frames) / rate;
        const std::string k = std::to_string(index);
        auto add = [&](double a, double b, const std::string& text) {
            VoxSegment s;
            s.startSec = a;
            s.endSec = b;
            s.text = text;
            s.confidence = confidences[out.size() % confidences.size()];
            s.language = language;
            out.push_back(std::move(s));
        };
        if (mode == Mode::Simple) {
            add(1.0, 2.0, " word" + k + " ");
        } else if (mode == Mode::Probe) {
            add(0.1, 0.3, "a" + k);
            add(5.0, 6.0, "b" + k);
            add(dur - 0.3, dur - 0.1, "c" + k);
        } else if (mode == Mode::Custom) {
            custom(index, dur, out);
        } else {
            add(0.0, dur + 50.0, "huge" + k);   // runs past the chunk and the recording
            add(-3.0, 0.5, "neg" + k);          // starts before the chunk
        }
        if (index == cancelAtCall && tokenToCancel != nullptr) {
            tokenToCancel->cancel();
        }
        return VoxTranscribeStatus::Ok;
    }
};

std::vector<float> sine(double hz, std::uint32_t sr, double seconds, double amp = 0.5) {
    std::vector<float> x(static_cast<std::size_t>(seconds * sr));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
    }
    return x;
}

double rmsMiddle(const std::vector<float>& x) {
    const std::size_t a = x.size() / 4;
    const std::size_t b = x.size() - a;
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) {
        s += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(s / static_cast<double>(b - a));
}

double toneGainDb(double hz, std::uint32_t inRate, std::uint32_t outRate) {
    const auto in = sine(hz, inRate, 1.0);
    std::vector<float> out;
    const bool ok = detail::resampleMono(in.data(), in.size(), inRate, outRate, out,
                                         detail::KeepGoing{});
    CHECK(ok);
    return 20.0 * std::log10((rmsMiddle(out) + 1e-12) / rmsMiddle(in));
}

VoxTranscript run(FakeBackend& fake, const std::vector<float>& pcm, std::uint32_t sr = 16000,
                  std::uint32_t ch = 1, VoxTranscribeOptions opts = {},
                  const VoxCancelToken* cancel = nullptr, const VoxProgressFn& pr = {}) {
    return transcribe(&fake, pcm.data(), pcm.size() / ch, VoxPcmFormat{sr, ch}, opts, cancel, pr);
}

std::vector<float> speechPcm(double seconds, std::uint64_t seed, SpeechSignal* keep = nullptr) {
    SpeechSignal s = makeSpeech(16000, seconds, seed);
    std::vector<float> out = s.samples;
    if (keep != nullptr) {
        *keep = std::move(s);
    }
    return out;
}

// Loud wideband signal with no pause: 3 loud frames, 1 quieter frame, repeated.
std::vector<float> noPausePcm(double seconds) {
    Rng rng(5);
    std::vector<float> x(static_cast<std::size_t>(seconds * 16000));
    for (std::size_t i = 0; i < x.size(); ++i) {
        const bool quiet = (i / 160) % 4 == 3;
        x[i] = static_cast<float>(rng.gauss() * (quiet ? 0.02 : 0.1));
    }
    return x;
}

std::vector<std::size_t> boundaries(const FakeBackend& f) {
    std::vector<std::size_t> b;
    std::size_t acc = 0;
    for (const auto& c : f.calls) {
        acc += c.frames;
        b.push_back(acc);
    }
    return b;
}

} // namespace

// ---------------------------------------------------------------- resampler

TEST_CASE(vox_transcribe.resample_passband_and_length) {
    CHECK_NEAR(toneGainDb(1000.0, 48000, 16000), 0.0, 0.5);
    CHECK_NEAR(toneGainDb(3000.0, 44100, 16000), 0.0, 0.5);
    CHECK_NEAR(toneGainDb(6000.0, 48000, 16000), 0.0, 1.0);   // below 0.9 * Nyquist
    CHECK_NEAR(toneGainDb(1000.0, 8000, 16000), 0.0, 0.5);    // up-sampling
    CHECK_NEAR(toneGainDb(1000.0, 16000, 16000), 0.0, 1e-6);  // identity

    const std::vector<float> in(48000, 0.5f);
    std::vector<float> out;
    CHECK(detail::resampleMono(in.data(), in.size(), 48000, 16000, out, {}));
    CHECK(out.size() == 16000);
    CHECK_NEAR(out[8000], 0.5, 1e-4);  // DC gain is 1
    CHECK(detail::resampleMono(in.data(), in.size(), 44100, 16000, out, {}));
    CHECK(out.size() == 48000ull * 16000 / 44100);
    CHECK(detail::resampleMono(in.data(), 7, 48000, 16000, out, {}));
    CHECK(out.size() == 2);
    CHECK(detail::resampleMono(in.data(), 1, 48000, 16000, out, {}));
    CHECK(out.size() == 1);
}

TEST_CASE(vox_transcribe.resample_rejects_aliases) {
    // 9 and 10 kHz are above the 8 kHz output Nyquist: unfiltered decimation
    // would fold them to 7 and 6 kHz at full level.
    CHECK(toneGainDb(9000.0, 48000, 16000) < -40.0);
    CHECK(toneGainDb(10000.0, 48000, 16000) < -40.0);
    CHECK(toneGainDb(12000.0, 48000, 16000) < -40.0);
    CHECK(toneGainDb(14000.0, 44100, 16000) < -40.0);
    // 8 kHz-source up-sampling: the 7 kHz image of a 1 kHz tone must be removed.
    const auto in = sine(1000.0, 8000, 1.0);
    std::vector<float> out;
    CHECK(detail::resampleMono(in.data(), in.size(), 8000, 16000, out, {}));
    // Project onto 7 kHz.
    double re = 0.0, im = 0.0;
    for (std::size_t i = out.size() / 4; i < out.size() * 3 / 4; ++i) {
        re += out[i] * std::cos(2.0 * kPi * 7000.0 * static_cast<double>(i) / 16000.0);
        im += out[i] * std::sin(2.0 * kPi * 7000.0 * static_cast<double>(i) / 16000.0);
    }
    const double n = static_cast<double>(out.size() / 2);
    const double imageAmp = 2.0 * std::sqrt(re * re + im * im) / n;
    CHECK(20.0 * std::log10(imageAmp / 0.5 + 1e-12) < -40.0);
}

TEST_CASE(vox_transcribe.resample_cancel) {
    const std::vector<float> in(48000 * 10, 0.1f);
    std::vector<float> out;
    CHECK(!detail::resampleMono(in.data(), in.size(), 48000, 16000, out,
                                [] { return false; }));
}

// ----------------------------------------------------------------- frontend

TEST_CASE(vox_transcribe.status_names) {
    CHECK(std::strcmp(voxTranscribeStatusName(VoxTranscribeStatus::Ok), "Ok") == 0);
    CHECK(std::strcmp(voxTranscribeStatusName(VoxTranscribeStatus::ModelUnavailable),
                      "ModelUnavailable") == 0);
    CHECK(std::strcmp(bgm_vox_transcribe_status_name(BGM_VOX_TR_NO_BACKEND), "NoBackend") == 0);
    CHECK(std::strcmp(bgm_vox_transcribe_status_name(99), "Unknown") == 0);
}

TEST_CASE(vox_transcribe.input_validation) {
    FakeBackend fake;
    const auto pcm = speechPcm(3.0, 1);
    auto t = transcribe(nullptr, pcm.data(), pcm.size(), {16000, 1}, {});
    CHECK(t.status == VoxTranscribeStatus::NoBackend);
    CHECK(t.machineGenerated);

    t = run(fake, pcm, 7999, 1);
    CHECK(t.status == VoxTranscribeStatus::UnsupportedFormat);
    t = run(fake, pcm, 48001, 1);
    CHECK(t.status == VoxTranscribeStatus::UnsupportedFormat);
    t = run(fake, pcm, 16000, 3);
    CHECK(t.status == VoxTranscribeStatus::UnsupportedFormat);
    t = transcribe(&fake, static_cast<const float*>(nullptr), 100, {16000, 1}, {});
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);
    t = transcribe(&fake, pcm.data(), 0, {16000, 1}, {});
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);
    t = transcribe(&fake, static_cast<const std::int16_t*>(nullptr), 100, {16000, 1}, {});
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);

    auto bad = pcm;
    bad[100] = std::numeric_limits<float>::quiet_NaN();
    t = run(fake, bad);
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);
    bad[100] = std::numeric_limits<float>::infinity();
    t = run(fake, bad);
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);

    VoxTranscribeOptions o;
    o.maxDurationSeconds = 1.0;
    t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::TooLong);
    CHECK(fake.calls.empty());
    CHECK_NEAR(t.audioSeconds, 3.0, 1e-9);

    VoxCancelToken token;
    token.cancel();
    t = run(fake, pcm, 16000, 1, {}, &token);
    CHECK(t.status == VoxTranscribeStatus::Cancelled);
    CHECK(fake.calls.empty());

    fake.rate = 100;
    t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::BackendError);
}

TEST_CASE(vox_transcribe.silence_does_not_call_backend) {
    FakeBackend fake;
    const std::vector<float> zeros(16000 * 5, 0.0f);
    auto t = run(fake, zeros);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(t.segments.empty());
    CHECK(t.fullText().empty());
    CHECK(fake.calls.empty());
    CHECK(t.machineGenerated);

    Rng rng(3);
    std::vector<float> hiss(16000 * 5);
    for (float& v : hiss) {
        v = static_cast<float>(rng.gauss() * 1e-4);
    }
    t = run(fake, hiss);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.empty());
}

TEST_CASE(vox_transcribe.simple_run_metadata) {
    FakeBackend fake;
    const auto pcm = speechPcm(8.0, 2);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() == 1);
    CHECK(fake.calls[0].frames == pcm.size());
    CHECK(t.segments.size() == 1);
    CHECK(t.segments[0].text == "word0");  // trimmed
    CHECK_NEAR(t.segments[0].startSec, 1.0, 1e-9);
    CHECK(t.backendName == "fake");
    CHECK(t.modelName == "fake-model");
    CHECK(t.machineGenerated);
    CHECK(t.processingMs >= 0.0);
}

TEST_CASE(vox_transcribe.chunks_split_at_pauses_not_mid_word) {
    FakeBackend fake;
    SpeechSignal sp;
    const auto pcm = speechPcm(95.0, 7, &sp);
    VoxTranscribeOptions o;
    o.chunkSeconds = 30.0;
    auto t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() >= 4);
    const auto b = boundaries(fake);
    CHECK(b.back() == pcm.size());
    std::size_t prev = 0;
    for (std::size_t i = 0; i + 1 < b.size(); ++i) {
        CHECK(sp.speechMask[b[i]] == 0);        // cut is outside every word
        CHECK(sp.speechMask[b[i] - 1] == 0);
        CHECK(b[i] - prev <= 30u * 16000);       // never longer than the target
        CHECK(b[i] - prev >= 14u * 16000);       // and cut in the second half of the window
        prev = b[i];
    }
    // Chunks do not overlap when a pause was found: the audio is a partition.
    std::size_t sum = 0;
    for (const auto& c : fake.calls) {
        sum += c.frames;
    }
    CHECK(sum == pcm.size());
}

TEST_CASE(vox_transcribe.chunking_after_resampling_stereo) {
    // 48 kHz stereo input: the backend still sees 16 kHz mono and the total
    // duration is preserved.
    FakeBackend fake;
    const auto mono16 = speechPcm(40.0, 11);
    std::vector<float> st(mono16.size() * 3 * 2);
    for (std::size_t i = 0; i < mono16.size() * 3; ++i) {
        const float v = mono16[i / 3];
        st[i * 2] = v;
        st[i * 2 + 1] = v;
    }
    auto t = run(fake, st, 48000, 2);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK_NEAR(t.audioSeconds, 40.0, 1e-9);
    std::size_t sum = 0;
    for (const auto& c : fake.calls) {
        sum += c.frames;
    }
    CHECK(sum == mono16.size());
    CHECK(fake.calls.size() >= 2);
}

TEST_CASE(vox_transcribe.timestamps_offset_to_recording_timeline) {
    FakeBackend fake;
    const auto pcm = speechPcm(70.0, 4);
    VoxTranscribeOptions o;
    o.chunkSeconds = 20.0;
    auto t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() >= 3);
    CHECK(t.segments.size() == fake.calls.size());
    const auto b = boundaries(fake);
    std::size_t begin = 0;
    for (std::size_t i = 0; i < t.segments.size(); ++i) {
        CHECK_NEAR(t.segments[i].startSec, static_cast<double>(begin) / 16000.0 + 1.0, 1e-9);
        CHECK_NEAR(t.segments[i].endSec,
                   std::min(static_cast<double>(begin) / 16000.0 + 2.0,
                            static_cast<double>(b[i]) / 16000.0),
                   1e-9);
        begin = b[i];
    }
    for (std::size_t i = 1; i < t.segments.size(); ++i) {
        CHECK(t.segments[i].startSec >= t.segments[i - 1].endSec);
    }
}

TEST_CASE(vox_transcribe.timestamps_clamped) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Tiling;
    const auto pcm = speechPcm(6.0, 5);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(t.segments.size() == 2);
    // "neg" starts at -3 s: clamped to 0; sorted before "huge" (which starts at 0, stable).
    for (const auto& s : t.segments) {
        CHECK(s.startSec >= 0.0);
        CHECK(s.endSec <= 6.0 + 1e-9);
        CHECK(s.endSec >= s.startSec);
    }
    CHECK_NEAR(t.segments[0].startSec, 0.0, 1e-9);
}

TEST_CASE(vox_transcribe.hard_cut_overlap_is_deduplicated) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Probe;
    const auto pcm = noPausePcm(70.0);
    VoxTranscribeOptions o;
    o.chunkSeconds = 30.0;
    auto t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() == 3);
    std::size_t sum = 0;
    for (const auto& c : fake.calls) {
        sum += c.frames;
    }
    CHECK(sum == pcm.size() + 2u * 8000u);  // two 0.5 s overlaps
    // Per chunk the fake emitted a(0.1-0.3), b(5-6), c(end-0.3..end-0.1). The
    // a/c segments that sit inside an overlap are dropped on one side.
    std::vector<std::string> texts;
    for (const auto& s : t.segments) {
        texts.push_back(s.text);
    }
    const std::vector<std::string> expected = {"a0", "b0", "b1", "b2", "c2"};
    CHECK(texts == expected);
    for (std::size_t i = 1; i < t.segments.size(); ++i) {
        CHECK(t.segments[i].startSec >= t.segments[i - 1].startSec);
    }
}

TEST_CASE(vox_transcribe.silent_chunks_are_skipped) {
    FakeBackend fake;
    std::vector<float> pcm = speechPcm(5.0, 8);
    pcm.insert(pcm.end(), 16000 * 70, 0.0f);
    const auto tail = speechPcm(5.0, 9);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    VoxTranscribeOptions o;
    o.chunkSeconds = 30.0;
    auto t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() == 2);
    CHECK(t.segments.size() == 2);
}

TEST_CASE(vox_transcribe.cancel_midway_keeps_finished_chunks) {
    FakeBackend fake;
    VoxCancelToken token;
    fake.tokenToCancel = &token;
    fake.cancelAtCall = 1;
    const auto pcm = speechPcm(95.0, 7);
    auto t = run(fake, pcm, 16000, 1, {}, &token);
    CHECK(t.status == VoxTranscribeStatus::Cancelled);
    CHECK(fake.calls.size() == 2);
    CHECK(t.segments.size() == 2);
    CHECK(t.machineGenerated);
}

TEST_CASE(vox_transcribe.backend_cancel_status_propagates) {
    FakeBackend fake;
    fake.failAtCall = 1;
    fake.failStatus = VoxTranscribeStatus::Cancelled;
    const auto pcm = speechPcm(95.0, 7);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Cancelled);
    CHECK(t.segments.size() == 1);
}

TEST_CASE(vox_transcribe.backend_error_propagates) {
    FakeBackend fake;
    fake.failAtCall = 1;
    const auto pcm = speechPcm(95.0, 7);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::BackendError);
    CHECK(t.detail == "boom");
    CHECK(fake.calls.size() == 2);  // stopped at the failure
    CHECK(t.segments.size() == 1);  // what was finished is kept, status says it is partial

    FakeBackend missing;
    missing.failAtCall = 0;
    missing.failStatus = VoxTranscribeStatus::ModelUnavailable;
    t = run(missing, speechPcm(4.0, 3));
    CHECK(t.status == VoxTranscribeStatus::ModelUnavailable);
    CHECK(t.segments.empty());
}

TEST_CASE(vox_transcribe.backend_exceptions_are_caught) {
    FakeBackend fake;
    fake.throwAtCall = 0;
    auto t = run(fake, speechPcm(4.0, 3));
    CHECK(t.status == VoxTranscribeStatus::InternalError);
    CHECK(t.detail.find("fake exploded") != std::string::npos);

    FakeBackend oom;
    oom.throwAtCall = 0;
    oom.throwBadAlloc = true;
    t = run(oom, speechPcm(4.0, 3));
    CHECK(t.status == VoxTranscribeStatus::OutOfMemory);
}

TEST_CASE(vox_transcribe.int16_matches_float) {
    const auto pcmF = speechPcm(6.0, 12);
    std::vector<std::int16_t> pcmI(pcmF.size());
    std::vector<float> back(pcmF.size());
    for (std::size_t i = 0; i < pcmF.size(); ++i) {
        pcmI[i] = static_cast<std::int16_t>(std::lround(pcmF[i] * 32768.0f));
        back[i] = static_cast<float>(pcmI[i]) / 32768.0f;
    }
    FakeBackend a;
    FakeBackend b;
    const auto ta = transcribe(&a, back.data(), back.size(), {16000, 1}, {});
    const auto tb = transcribe(&b, pcmI.data(), pcmI.size(), {16000, 1}, {});
    CHECK(ta.status == VoxTranscribeStatus::Ok);
    CHECK(tb.status == VoxTranscribeStatus::Ok);
    CHECK(a.calls.size() == b.calls.size());
    CHECK(a.calls[0].samples == b.calls[0].samples);
    CHECK(ta.fullText() == tb.fullText());
    CHECK(ta.segments.size() == tb.segments.size());
}

TEST_CASE(vox_transcribe.low_confidence_flag) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Probe;
    fake.confidences = {0.9f, 0.3f, -1.0f};
    const auto pcm = speechPcm(10.0, 13);
    auto t = run(fake, pcm);
    CHECK(t.segments.size() == 3);
    CHECK(!t.segments[0].lowConfidence);
    CHECK(t.segments[1].lowConfidence);
    CHECK(t.segments[2].lowConfidence);          // unknown is never presented as reliable
    CHECK(t.segments[2].confidence < 0.0f);
    CHECK_NEAR(t.segments[0].confidence, 0.9, 1e-6);

    FakeBackend fake2;
    fake2.mode = FakeBackend::Mode::Probe;
    fake2.confidences = {0.9f, 0.3f, 0.45f};
    VoxTranscribeOptions o;
    o.lowConfidenceThreshold = 0.4f;
    t = run(fake2, pcm, 16000, 1, o);
    CHECK(t.segments[1].lowConfidence);  // 0.3 < 0.4
    CHECK(!t.segments[2].lowConfidence);           // 0.45 >= 0.4
}

TEST_CASE(vox_transcribe.full_text_joins_trimmed_segments) {
    VoxTranscript t;
    CHECK(t.fullText().empty());
    VoxSegment a;
    a.text = "  hello ";
    VoxSegment b;
    b.text = "   ";
    VoxSegment c;
    c.text = "world\n";
    t.segments = {a, b, c};
    CHECK(t.fullText() == "hello world");
    CHECK(t.machineGenerated);
}

TEST_CASE(vox_transcribe.language_detected_once_then_locked) {
    FakeBackend fake;
    fake.language = "ru";
    const auto pcm = speechPcm(95.0, 7);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(fake.calls.size() >= 2);
    CHECK(fake.calls[0].optionsLanguage.empty());
    CHECK(fake.calls[1].optionsLanguage == "ru");
    CHECK(t.detectedLanguage == "ru");

    FakeBackend forced;
    VoxTranscribeOptions o;
    o.language = "en";
    t = run(forced, speechPcm(5.0, 3), 16000, 1, o);
    CHECK(forced.calls[0].optionsLanguage == "en");
    CHECK(t.detectedLanguage == "en");
}

TEST_CASE(vox_transcribe.progress_is_monotonic_and_finishes) {
    FakeBackend fake;
    std::vector<float> seen;
    const auto pcm = speechPcm(95.0, 7);
    auto t = run(fake, pcm, 16000, 1, {}, nullptr, [&](float f) { seen.push_back(f); });
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(seen.size() >= 3);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i] >= seen[i - 1]);
    }
    CHECK(seen.front() == 0.0f);
    CHECK(seen.back() == 1.0f);
}

TEST_CASE(vox_transcribe.capi_without_backend) {
    char err[64];
    bgm_vox_asr* asr = nullptr;
    const int st = bgm_vox_whisper_create("/definitely/not/here.bin", 0, 0, &asr, err, sizeof err);
    CHECK(asr == nullptr);
    CHECK(st == BGM_VOX_TR_NO_BACKEND || st == BGM_VOX_TR_MODEL_UNAVAILABLE);
    CHECK(std::strlen(err) > 0);

    const std::int16_t pcm[100] = {};
    bgm_vox_transcript* r = nullptr;
    const int st2 = bgm_vox_transcribe_s16(nullptr, pcm, 100, 16000, 1, "", 0, 0.0, 0.0, nullptr,
                                           nullptr, nullptr, &r);
    CHECK(st2 == BGM_VOX_TR_NO_BACKEND);
    CHECK(r != nullptr);
    CHECK(bgm_vox_transcript_status(r) == BGM_VOX_TR_NO_BACKEND);
    CHECK(bgm_vox_transcript_segment_count(r) == 0);
    CHECK(bgm_vox_transcript_machine_generated(r) == 1);
    CHECK(bgm_vox_transcript_segment_low_confidence(r, 5) == 1);
    CHECK(std::strlen(bgm_vox_transcript_segment_text(r, 5)) == 0);
    bgm_vox_transcript_free(r);
    bgm_vox_transcript_free(nullptr);
    bgm_vox_asr_free(nullptr);
    CHECK(bgm_vox_asr_info(nullptr, nullptr, nullptr) != 0);
}

TEST_CASE(vox_transcribe.padding_hallucinations_are_dropped) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Custom;
    fake.custom = [](int, double dur, std::vector<VoxSegment>& out) {
        VoxSegment real;
        real.startSec = 1.0;
        real.endSec = 2.0;
        real.text = "real";
        VoxWord w1;
        w1.text = "real";
        w1.startSec = 1.0;
        w1.endSec = 2.0;
        VoxWord w2;  // word that starts in the padding
        w2.text = "ghost";
        w2.startSec = dur + 0.2;
        w2.endSec = dur + 0.5;
        real.words = {w1, w2};
        out.push_back(real);
        VoxSegment ghost;  // starts exactly at / after the real end
        ghost.startSec = dur;
        ghost.endSec = dur + 0.4;
        ghost.text = "Thanks for watching";
        out.push_back(ghost);
        VoxSegment later = ghost;
        later.startSec = dur + 0.3;
        later.endSec = dur + 0.9;
        out.push_back(later);
    };
    const auto pcm = speechPcm(2.5, 21);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(t.segments.size() == 1);
    CHECK(t.segments[0].text == "real");
    CHECK(t.segments[0].words.size() == 1);
}

TEST_CASE(vox_transcribe.translation_requires_backend_support) {
    const auto pcm = speechPcm(2.5, 22);
    VoxTranscribeOptions o;
    o.translateToEnglish = true;
    FakeBackend no;
    auto t = run(no, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::InvalidInput);
    CHECK(no.calls.empty());
    FakeBackend yes;
    yes.translation = true;
    t = run(yes, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
}

TEST_CASE(vox_transcribe.overlap_tail_kept_when_new_chunk_has_nothing_past_handover) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Custom;
    fake.custom = [](int index, double dur, std::vector<VoxSegment>& out) {
        auto add = [&](double a, double b, const std::string& text) {
            VoxSegment s;
            s.startSec = a;
            s.endSec = b;
            s.text = text + std::to_string(index);
            s.confidence = 0.9f;
            out.push_back(std::move(s));
        };
        if (index == 0) {
            add(0.1, 0.3, "a");
            add(dur - 0.3, dur - 0.1, "tail");
        } else if (index == 1) {
            add(0.0, 0.1, "dup");  // entirely inside the overlap, before the handover
        } else {
            add(5.0, 6.0, "b");
        }
    };
    const auto pcm = noPausePcm(70.0);
    VoxTranscribeOptions o;
    o.chunkSeconds = 30.0;
    auto t = run(fake, pcm, 16000, 1, o);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    std::vector<std::string> texts;
    for (const auto& s : t.segments) {
        texts.push_back(s.text);
    }
    const std::vector<std::string> expected = {"a0", "tail0", "b2"};
    CHECK(texts == expected);
}

TEST_CASE(vox_transcribe.invalid_utf8_is_replaced) {
    FakeBackend fake;
    fake.mode = FakeBackend::Mode::Custom;
    fake.custom = [](int, double, std::vector<VoxSegment>& out) {
        VoxSegment a;
        a.startSec = 0.5;
        a.endSec = 1.0;
        a.text = std::string("ok \xD0\xBF\xD1\x80 caf\xC3");       // truncated 2-byte tail
        VoxWord w;
        w.text = "x\xE2\x82";                                          // truncated 3-byte
        w.startSec = 0.5;
        w.endSec = 0.8;
        a.words = {w};
        out.push_back(a);
        VoxSegment b;
        b.startSec = 1.0;
        b.endSec = 1.5;
        b.text = std::string("\xA9 \xC0\xAF \xED\xA0\x80 fin");   // stray cont., overlong, surrogate
        out.push_back(b);
    };
    const auto pcm = speechPcm(2.5, 23);
    auto t = run(fake, pcm);
    CHECK(t.status == VoxTranscribeStatus::Ok);
    CHECK(t.segments.size() == 2);
    CHECK(t.segments[0].text == "ok \xD0\xBF\xD1\x80 caf\xEF\xBF\xBD");
    CHECK(t.segments[0].words.size() == 1);
    CHECK(t.segments[0].words[0].text == "x\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(t.segments[1].text == "\xEF\xBF\xBD \xEF\xBF\xBD\xEF\xBF\xBD \xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD fin");
}
