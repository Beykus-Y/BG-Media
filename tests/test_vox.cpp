#include "framework.hpp"

#include "bgmedia/vox/VoxEnhance.hpp"
#include "bgmedia/vox/vox_capi.h"
#include "support/VoxSynthetic.hpp"
#include "vox/VoxDenoiser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using namespace bgmedia::vox;
using namespace bgmedia::voxsynth;

VoxEnhanceConfig cfgFor(VoxEnhanceMode mode) {
    VoxEnhanceConfig c;
    c.mode = mode;
    return c;
}

VoxEnhanceResult run(const std::vector<float>& pcm, std::uint32_t sr, std::uint32_t ch,
                     VoxEnhanceMode mode) {
    return enhanceVoice(pcm.data(), pcm.size() / ch, VoxPcmFormat{sr, ch}, cfgFor(mode));
}

bool allFinite(const std::vector<float>& x) {
    for (float v : x) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

double peakOf(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) {
        p = std::max(p, static_cast<double>(std::abs(v)));
    }
    return p;
}

bool bitEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

constexpr double kCeilingTolerance = 0.9013;  // -0.9 dBFS

std::vector<std::int16_t> int16Ramp(std::size_t n) {
    std::vector<std::int16_t> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<std::int16_t>(static_cast<int>((i * 2654435761u) % 65536u) - 32768);
    }
    x[0] = -32768;
    x[1] = 32767;
    return x;
}

} // namespace

TEST_CASE(vox.off_bit_exact) {
    const auto speech = makeScenario(NoiseKind::White, 10.0, 16000, 2.0, 3);
    for (auto mode : {VoxEnhanceMode::Off}) {
        const auto r = run(speech.noisy, 16000, 1, mode);
        CHECK(r.report.status == VoxStatus::Bypassed);
        CHECK(r.usedOriginal());
        CHECK(bitEqual(r.pcm, speech.noisy));
    }
    // int16 path: exact conversion and exact round trip.
    const auto ints = int16Ramp(2 * 5000);
    const auto r = enhanceVoice(ints.data(), 5000, VoxPcmFormat{44100, 2}, cfgFor(VoxEnhanceMode::Off));
    CHECK(r.report.status == VoxStatus::Bypassed);
    CHECK(r.pcm.size() == ints.size());
    for (std::size_t i = 0; i < ints.size(); ++i) {
        CHECK(r.pcm[i] == static_cast<float>(ints[i]) / 32768.0f);
    }
    CHECK(toInt16(r.pcm) == ints);
    // C ABI.
    std::vector<std::int16_t> out(ints.size(), 7);
    const int status = bgm_vox_enhance_s16(ints.data(), 5000, 44100, 2, BGM_VOX_MODE_OFF, out.data(), nullptr);
    CHECK(status == BGM_VOX_BYPASSED);
    CHECK(out == ints);
    CHECK(std::string(bgm_vox_status_name(status)) == "Bypassed");
}

TEST_CASE(vox.stft_identity_unity_gain) {
    struct Case {
        std::uint32_t sr;
        std::size_t channels;
        std::size_t frames;
    };
    const Case cases[] = {{16000, 1, 12345}, {48000, 2, 30001}, {8000, 1, 4096}, {44100, 1, 1000},
                          {16000, 1, 7},     {16000, 2, 1},     {32000, 1, 2048}};
    for (const Case& c : cases) {
        Rng rng(c.frames + c.sr);
        std::vector<float> x(c.frames * c.channels);
        for (float& v : x) {
            v = static_cast<float>(0.5 * rng.range(-1.0, 1.0));
        }
        const std::vector<float> ref = x;
        bgmedia::vox::detail::DenoiseParams dp;
        dp.unityGain = true;
        bgmedia::vox::detail::SpectralDenoiser den(c.sr, c.channels, dp);
        CHECK(den.process(x.data(), c.channels, c.frames, {}));
        double maxErr = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            maxErr = std::max(maxErr, static_cast<double>(std::abs(x[i] - ref[i])));
        }
        CHECK(maxErr < 1e-4);
    }
    CHECK(bgmedia::vox::detail::chooseFftSize(8000) == 256);
    CHECK(bgmedia::vox::detail::chooseFftSize(16000) == 512);
    CHECK(bgmedia::vox::detail::chooseFftSize(32000) == 1024);
    CHECK(bgmedia::vox::detail::chooseFftSize(44100) == 1024);
    CHECK(bgmedia::vox::detail::chooseFftSize(48000) == 1024);
}

TEST_CASE(vox.noise_reduction_improves_snr) {
    for (NoiseKind kind : {NoiseKind::White, NoiseKind::Pink, NoiseKind::FanHum}) {
        const auto sc = makeScenario(kind, 5.0, 16000, 5.0, 11);
        const double in = pauseSnrDb(sc, sc.noisy);
        const auto nat = run(sc.noisy, 16000, 1, VoxEnhanceMode::Natural);
        const auto str = run(sc.noisy, 16000, 1, VoxEnhanceMode::StrongExperimental);
        CHECK(nat.report.status == VoxStatus::Processed);
        CHECK(str.report.status == VoxStatus::Processed);
        const double natGain = pauseSnrDb(sc, nat.pcm) - in;
        const double strGain = pauseSnrDb(sc, str.pcm) - in;
        CHECK(natGain >= 4.0);
        CHECK(strGain > natGain);
    }
}

TEST_CASE(vox.speech_preserved_and_aligned) {
    for (NoiseKind kind : {NoiseKind::White, NoiseKind::Pink}) {
        const auto sc = makeScenario(kind, 15.0, 16000, 5.0, 23);
        for (auto mode : {VoxEnhanceMode::Natural, VoxEnhanceMode::StrongExperimental}) {
            const auto r = run(sc.noisy, 16000, 1, mode);
            CHECK(r.report.status == VoxStatus::Processed);
            CHECK(r.pcm.size() == sc.noisy.size());
            CHECK(correlation(sc.clean, r.pcm) > 0.9);
            const int lag = bestLag(sc.clean, r.pcm, 64);
            CHECK(lag >= -1 && lag <= 1);
        }
    }
}

TEST_CASE(vox.no_clipping_on_loud_input) {
    auto sc = makeScenario(NoiseKind::White, 15.0, 16000, 4.0, 5);
    for (float& v : sc.noisy) {
        v = std::max(-1.0f, std::min(1.0f, v * 9.0f));  // loud, partly hard clipped
    }
    CHECK(peakOf(sc.noisy) >= 0.999);
    for (auto mode : {VoxEnhanceMode::Natural, VoxEnhanceMode::StrongExperimental}) {
        const auto r = run(sc.noisy, 16000, 1, mode);
        CHECK(r.report.status == VoxStatus::Processed);
        CHECK(allFinite(r.pcm));
        CHECK(peakOf(r.pcm) <= kCeilingTolerance);
        CHECK(r.report.outputPeakDbfs <= -0.9);
    }
    // Full-scale square wave burst + silence.
    std::vector<float> sq(16000, 0.0f);
    for (std::size_t i = 4000; i < 8000; ++i) {
        sq[i] = ((i / 40) % 2 == 0) ? 1.0f : -1.0f;
    }
    const auto r = run(sq, 16000, 1, VoxEnhanceMode::StrongExperimental);
    CHECK(allFinite(r.pcm));
    CHECK(peakOf(r.pcm) <= kCeilingTolerance);
}

TEST_CASE(vox.silence_is_not_amplified) {
    const double rmsIn = std::pow(10.0, -70.0 / 20.0);
    auto noise = makeNoise(NoiseKind::White, 16000, 16000 * 4, 9);
    scaleToRms(noise, rmsIn);
    for (auto mode : {VoxEnhanceMode::Natural, VoxEnhanceMode::StrongExperimental}) {
        const auto r = run(noise, 16000, 1, mode);
        CHECK(r.report.status == VoxStatus::Processed);
        CHECK(r.report.appliedMakeupGainDb == 0.0);
        const double outDb = 20.0 * std::log10(rmsOf(r.pcm) + 1e-12);
        const double inDb = 20.0 * std::log10(rmsOf(noise) + 1e-12);
        CHECK(outDb <= inDb + 1.0);
    }
    const std::vector<float> zeros(16000, 0.0f);
    const auto z = run(zeros, 16000, 1, VoxEnhanceMode::Natural);
    CHECK(allFinite(z.pcm));
    CHECK(peakOf(z.pcm) == 0.0);
}

TEST_CASE(vox.quiet_speech_is_raised_within_limits) {
    auto sc = makeScenario(NoiseKind::White, 20.0, 16000, 5.0, 31);
    const float g = static_cast<float>(std::pow(10.0, -16.0 / 20.0));  // speech ~ -40 dBFS
    for (float& v : sc.noisy) {
        v *= g;
    }
    for (auto mode : {VoxEnhanceMode::Natural, VoxEnhanceMode::StrongExperimental}) {
        const auto r = run(sc.noisy, 16000, 1, mode);
        const auto p = paramsForMode(mode);
        CHECK(r.report.status == VoxStatus::Processed);
        CHECK(r.report.appliedMakeupGainDb > 6.0);
        CHECK(r.report.appliedMakeupGainDb <= p.maxMakeupDb + 1e-9);
        CHECK(r.report.outputSpeechRmsDbfs > r.report.inputSpeechRmsDbfs + 4.0);
        CHECK(r.report.outputSpeechRmsDbfs <= r.report.inputSpeechRmsDbfs + p.maxMakeupDb + 0.5);
    }
}

TEST_CASE(vox.fail_open_statuses) {
    const auto sc = makeScenario(NoiseKind::White, 10.0, 16000, 2.0, 2);
    const std::size_t frames = sc.noisy.size();
    const auto nat = cfgFor(VoxEnhanceMode::Natural);

    for (std::uint32_t rate : {96000u, 4000u, 0u}) {
        const auto r = enhanceVoice(sc.noisy.data(), frames, VoxPcmFormat{rate, 1}, nat);
        CHECK(r.report.status == VoxStatus::FallbackUnsupportedFormat);
        CHECK(r.usedOriginal());
        CHECK(bitEqual(r.pcm, sc.noisy));
    }
    {
        std::vector<float> three(frames * 3 / 3 * 1, 0.1f);
        three.resize(frames / 3 * 3, 0.1f);
        const auto r = enhanceVoice(three.data(), three.size() / 3, VoxPcmFormat{16000, 3}, nat);
        CHECK(r.report.status == VoxStatus::FallbackUnsupportedFormat);
        CHECK(bitEqual(r.pcm, three));
    }
    {
        const auto r = enhanceVoice(sc.noisy.data(), 0, VoxPcmFormat{16000, 1}, nat);
        CHECK(r.report.status == VoxStatus::FallbackInvalidInput);
        CHECK(r.pcm.empty());
    }
    {
        const auto r = enhanceVoice(static_cast<const float*>(nullptr), 1000, VoxPcmFormat{16000, 1}, nat);
        CHECK(r.report.status == VoxStatus::FallbackInvalidInput);
        CHECK(r.pcm.empty());
    }
    for (float bad : {std::nanf(""), INFINITY, -INFINITY}) {
        std::vector<float> x = sc.noisy;
        x[frames / 2] = bad;
        const auto r = run(x, 16000, 1, VoxEnhanceMode::StrongExperimental);
        CHECK(r.report.status == VoxStatus::FallbackInvalidInput);
        CHECK(bitEqual(r.pcm, x));
    }
    {
        VoxEnhanceConfig c = nat;
        c.maxDurationSeconds = 1.0;  // input is 2 s
        const auto r = enhanceVoice(sc.noisy.data(), frames, VoxPcmFormat{16000, 1}, c);
        CHECK(r.report.status == VoxStatus::FallbackTooLong);
        CHECK(bitEqual(r.pcm, sc.noisy));
    }
    {
        VoxCancelToken token;
        token.cancel();
        const auto r = enhanceVoice(sc.noisy.data(), frames, VoxPcmFormat{16000, 1}, nat, &token);
        CHECK(r.report.status == VoxStatus::Cancelled);
        CHECK(bitEqual(r.pcm, sc.noisy));
    }
    // C ABI fallbacks copy the input.
    const auto ints = int16Ramp(8000);
    std::vector<std::int16_t> out(ints.size(), 0);
    CHECK(bgm_vox_enhance_s16(ints.data(), 8000, 96000, 1, BGM_VOX_MODE_NATURAL, out.data(), nullptr) ==
          BGM_VOX_FALLBACK_UNSUPPORTED_FORMAT);
    CHECK(out == ints);
    volatile int flag = 1;
    std::fill(out.begin(), out.end(), static_cast<std::int16_t>(0));
    CHECK(bgm_vox_enhance_s16(ints.data(), 8000, 16000, 1, BGM_VOX_MODE_NATURAL, out.data(), &flag) ==
          BGM_VOX_CANCELLED);
    CHECK(out == ints);
    CHECK(bgm_vox_enhance_s16(nullptr, 8000, 16000, 1, BGM_VOX_MODE_NATURAL, out.data(), nullptr) ==
          BGM_VOX_FALLBACK_INVALID_INPUT);
    CHECK(std::string(bgm_vox_status_name(BGM_VOX_CANCELLED)) == "Cancelled");
    CHECK(std::string(bgm_vox_status_name(999)) == "Unknown");
}

TEST_CASE(vox.c_abi_processes) {
    const auto sc = makeScenario(NoiseKind::White, 10.0, 16000, 3.0, 6);
    std::vector<std::int16_t> in(sc.noisy.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<std::int16_t>(std::lrint(sc.noisy[i] * 32767.0f));
    }
    std::vector<std::int16_t> out(in.size(), 0);
    CHECK(bgm_vox_enhance_s16(in.data(), in.size(), 16000, 1, BGM_VOX_MODE_NATURAL, out.data(), nullptr) ==
          BGM_VOX_PROCESSED);
    CHECK(out != in);
    int peak = 0;
    for (std::int16_t v : out) {
        peak = std::max(peak, std::abs(static_cast<int>(v)));
    }
    CHECK(peak <= 29500);  // -0.9 dBFS
    CHECK(std::string(bgm_vox_status_name(BGM_VOX_PROCESSED)) == "Processed");
}

TEST_CASE(vox.sample_rates_and_stereo) {
    for (std::uint32_t sr : {8000u, 16000u, 44100u, 48000u}) {
        const auto sc = makeScenario(NoiseKind::Pink, 10.0, sr, 3.0, 41);
        const auto r = run(sc.noisy, sr, 1, VoxEnhanceMode::Natural);
        CHECK(r.report.status == VoxStatus::Processed);
        CHECK(r.pcm.size() == sc.noisy.size());
        CHECK(allFinite(r.pcm));
        CHECK(peakOf(r.pcm) <= kCeilingTolerance);
        CHECK(pauseSnrDb(sc, r.pcm) > pauseSnrDb(sc, sc.noisy) + 3.0);
    }
    // Stereo: different noise per channel, same speech. Gains are linked.
    const auto a = makeScenario(NoiseKind::White, 10.0, 44100, 3.0, 51);
    const auto b = makeScenario(NoiseKind::White, 10.0, 44100, 3.0, 52);
    std::vector<float> st(a.noisy.size() * 2);
    for (std::size_t i = 0; i < a.noisy.size(); ++i) {
        st[2 * i] = a.noisy[i];
        st[2 * i + 1] = 0.5f * b.noisy[i];
    }
    const auto r = run(st, 44100, 2, VoxEnhanceMode::StrongExperimental);
    CHECK(r.report.status == VoxStatus::Processed);
    CHECK(r.pcm.size() == st.size());
    CHECK(r.report.frames == a.noisy.size());
    CHECK(allFinite(r.pcm));
    CHECK(peakOf(r.pcm) <= kCeilingTolerance);
    // Identical channels stay identical (linked processing).
    std::vector<float> dual(a.noisy.size() * 2);
    for (std::size_t i = 0; i < a.noisy.size(); ++i) {
        dual[2 * i] = dual[2 * i + 1] = a.noisy[i];
    }
    const auto d = run(dual, 44100, 2, VoxEnhanceMode::Natural);
    CHECK(d.report.status == VoxStatus::Processed);
    for (std::size_t i = 0; i < a.noisy.size(); ++i) {
        CHECK(d.pcm[2 * i] == d.pcm[2 * i + 1]);
    }
    // int16 overload.
    const auto ints = int16Ramp(2 * 3000);
    const auto ri = enhanceVoice(ints.data(), 3000, VoxPcmFormat{48000, 2}, cfgFor(VoxEnhanceMode::Natural));
    CHECK(ri.pcm.size() == ints.size());
}

TEST_CASE(vox.very_short_inputs_do_not_break) {
    for (std::size_t frames : {1u, 2u, 17u, 159u, 160u, 161u, 1000u}) {
        std::vector<float> x(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            x[i] = 0.3f * std::sin(0.05f * static_cast<float>(i));
        }
        const auto r = run(x, 16000, 1, VoxEnhanceMode::Natural);
        CHECK(r.pcm.size() == frames);
        CHECK(allFinite(r.pcm));
        CHECK(peakOf(r.pcm) <= kCeilingTolerance || r.usedOriginal());
    }
}

TEST_CASE(vox.deterministic) {
    const auto sc = makeScenario(NoiseKind::Clicks, 8.0, 16000, 4.0, 77);
    for (auto mode : {VoxEnhanceMode::Natural, VoxEnhanceMode::StrongExperimental}) {
        const auto a = run(sc.noisy, 16000, 1, mode);
        const auto b = run(sc.noisy, 16000, 1, mode);
        CHECK(bitEqual(a.pcm, b.pcm));
    }
}

TEST_CASE(vox.threads_match_single_thread) {
    const auto s1 = makeScenario(NoiseKind::White, 5.0, 16000, 3.0, 1);
    const auto s2 = makeScenario(NoiseKind::Pink, 10.0, 16000, 3.0, 2);
    const auto s3 = makeScenario(NoiseKind::FanHum, 8.0, 16000, 3.0, 3);
    const auto s4 = makeScenario(NoiseKind::Clicks, 12.0, 16000, 3.0, 4);
    const std::vector<float>* inputs[4] = {&s1.noisy, &s2.noisy, &s3.noisy, &s4.noisy};
    std::vector<float> expected[4];
    for (int i = 0; i < 4; ++i) {
        expected[i] = run(*inputs[i], 16000, 1, VoxEnhanceMode::Natural).pcm;
    }
    std::vector<float> got[4];
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i]() {
            got[i] = run(*inputs[i], 16000, 1, VoxEnhanceMode::Natural).pcm;
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(bitEqual(expected[i], got[i]));
    }
}

TEST_CASE(vox.progress_and_cancel_midway) {
    const auto sc = makeScenario(NoiseKind::White, 8.0, 16000, 6.0, 8);
    std::vector<float> fractions;
    const auto r = enhanceVoice(sc.noisy.data(), sc.noisy.size(), VoxPcmFormat{16000, 1},
                                cfgFor(VoxEnhanceMode::Natural), nullptr,
                                [&](float f) { fractions.push_back(f); });
    CHECK(r.report.status == VoxStatus::Processed);
    CHECK(fractions.size() >= 4);
    CHECK(fractions.size() <= 40);
    CHECK(fractions.back() == 1.0f);
    for (std::size_t i = 1; i < fractions.size(); ++i) {
        CHECK(fractions[i] >= fractions[i - 1]);
    }
    VoxCancelToken token;
    const auto c = enhanceVoice(sc.noisy.data(), sc.noisy.size(), VoxPcmFormat{16000, 1},
                                cfgFor(VoxEnhanceMode::Natural), &token,
                                [&](float f) {
                                    if (f > 0.3f) {
                                        token.cancel();
                                    }
                                });
    CHECK(c.report.status == VoxStatus::Cancelled);
    CHECK(bitEqual(c.pcm, sc.noisy));
}

TEST_CASE(vox.report_fields_are_consistent) {
    const auto sc = makeScenario(NoiseKind::White, 10.0, 16000, 5.0, 12);
    const auto r = run(sc.noisy, 16000, 1, VoxEnhanceMode::Natural);
    CHECK(r.report.status == VoxStatus::Processed);
    CHECK(r.report.frames == sc.noisy.size());
    CHECK(r.report.processingMs > 0.0);
    CHECK(r.report.inputSpeechRmsDbfs > -60.0);
    CHECK(r.report.estimatedNoiseFloorDbfs < r.report.inputSpeechRmsDbfs);
    CHECK(r.report.appliedMakeupGainDb >= -6.0 && r.report.appliedMakeupGainDb <= 12.0);
    CHECK(std::string(voxStatusName(VoxStatus::FallbackQualityGuard)) == "FallbackQualityGuard");
}
