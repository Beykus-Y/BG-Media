// bgmedia_vox_enhance: offline A/B tool for BG Vox Enhance.
//
//   bgmedia_vox_enhance <in.wav> <out_prefix> [--mode natural|strong|all]
//   bgmedia_vox_enhance --synthetic <out_prefix>
//
// Reads 16-bit PCM WAV (mono or stereo), writes <prefix>.original.wav,
// <prefix>.natural.wav, <prefix>.strong.wav and prints one JSON line per run.
// Self-contained: depends only on bgmedia_vox.

#include "bgmedia/vox/VoxEnhance.hpp"
#include "support/VoxSynthetic.hpp"
#include "support/WavFile.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bgmedia::vox;
using bgmedia::toolsupport::readWav;
using bgmedia::toolsupport::WavFile;
using bgmedia::toolsupport::writeWav;

namespace {

std::string jsonReport(const char* label, const VoxEnhanceReport& r, std::uint32_t sr) {
    const double seconds = static_cast<double>(r.frames) / static_cast<double>(sr);
    const double rtf = r.processingMs > 0.0 ? seconds * 1000.0 / r.processingMs : 0.0;
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << "{\"mode\":\"" << label << "\",\"status\":\"" << voxStatusName(r.status)
      << "\",\"detail\":\"" << r.detail << "\",\"frames\":" << r.frames
      << ",\"input_peak_dbfs\":" << r.inputPeakDbfs << ",\"output_peak_dbfs\":" << r.outputPeakDbfs
      << ",\"input_speech_rms_dbfs\":" << r.inputSpeechRmsDbfs
      << ",\"output_speech_rms_dbfs\":" << r.outputSpeechRmsDbfs
      << ",\"noise_floor_dbfs\":" << r.estimatedNoiseFloorDbfs
      << ",\"makeup_gain_db\":" << r.appliedMakeupGainDb << ",\"processing_ms\":" << r.processingMs
      << ",\"realtime_factor\":" << rtf << "}";
    return o.str();
}

VoxEnhanceResult run(const std::vector<float>& pcm, std::uint32_t sr, std::uint32_t ch,
                     VoxEnhanceMode mode) {
    VoxEnhanceConfig cfg;
    cfg.mode = mode;
    return enhanceVoice(pcm.data(), pcm.size() / ch, VoxPcmFormat{sr, ch}, cfg);
}

std::vector<std::int16_t> quantise(const std::vector<float>& pcm) { return toInt16(pcm); }

int processFile(const std::string& in, const std::string& prefix, const std::string& modeArg) {
    WavFile wav;
    std::string error;
    if (!readWav(in, wav, error)) {
        std::cerr << "error: " << error << "\n";
        return 2;
    }
    const std::size_t frames = wav.samples.size() / wav.channels;
    std::vector<float> pcm(wav.samples.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<float>(wav.samples[i]) / 32768.0f;
    }
    (void)frames;
    if (!writeWav(prefix + ".original.wav", wav.sampleRate, wav.channels, wav.samples)) {
        std::cerr << "error: cannot write " << prefix << ".original.wav\n";
        return 2;
    }
    const bool doNatural = modeArg == "natural" || modeArg == "all";
    const bool doStrong = modeArg == "strong" || modeArg == "all";
    if (doNatural) {
        auto r = run(pcm, wav.sampleRate, wav.channels, VoxEnhanceMode::Natural);
        writeWav(prefix + ".natural.wav", wav.sampleRate, wav.channels, quantise(r.pcm));
        std::cout << jsonReport("natural", r.report, wav.sampleRate) << "\n";
    }
    if (doStrong) {
        auto r = run(pcm, wav.sampleRate, wav.channels, VoxEnhanceMode::StrongExperimental);
        writeWav(prefix + ".strong.wav", wav.sampleRate, wav.channels, quantise(r.pcm));
        std::cout << jsonReport("strong", r.report, wav.sampleRate) << "\n";
    }
    return 0;
}

int syntheticRun(const std::string& prefix) {
    using namespace bgmedia::voxsynth;
    const std::uint32_t sr = 16000;
    const double seconds = 20.0;
    const NoiseKind kinds[] = {NoiseKind::White, NoiseKind::Pink, NoiseKind::FanHum, NoiseKind::Clicks};
    const double snrs[] = {5.0, 15.0};
    // pauseSNR: level inside words minus level inside pauses (dB), see
    // tools/support/VoxSynthetic.hpp. corr: zero-lag correlation with the clean
    // speech (reduced by the high-pass phase shift, informative only).
    std::cout << std::left << std::setw(9) << "noise" << std::setw(7) << "mixSNR" << std::setw(9)
              << "in" << std::setw(9) << "natural" << std::setw(9) << "strong" << std::setw(10)
              << "corr.nat" << std::setw(10) << "corr.str" << std::setw(8) << "xRT.nat"
              << std::setw(8) << "xRT.str" << "peak nat/str\n";
    for (NoiseKind kind : kinds) {
        for (double snr : snrs) {
            Scenario sc = makeScenario(kind, snr, sr, seconds, 1234);
            const std::string name = prefix + "." + noiseName(kind) + "_" + std::to_string(static_cast<int>(snr)) + "dB";
            writeWav(name + ".original.wav", sr, 1, quantise(sc.noisy));
            auto nat = run(sc.noisy, sr, 1, VoxEnhanceMode::Natural);
            auto str = run(sc.noisy, sr, 1, VoxEnhanceMode::StrongExperimental);
            writeWav(name + ".natural.wav", sr, 1, quantise(nat.pcm));
            writeWav(name + ".strong.wav", sr, 1, quantise(str.pcm));
            std::cout << std::fixed << std::setprecision(1) << std::left << std::setw(9)
                      << noiseName(kind) << std::setw(7) << snr << std::setw(9)
                      << pauseSnrDb(sc, sc.noisy) << std::setw(9) << pauseSnrDb(sc, nat.pcm)
                      << std::setw(9) << pauseSnrDb(sc, str.pcm) << std::setprecision(3)
                      << std::setw(10) << correlation(sc.clean, nat.pcm) << std::setw(10)
                      << correlation(sc.clean, str.pcm) << std::setprecision(0) << std::setw(8)
                      << seconds * 1000.0 / nat.report.processingMs << std::setw(8)
                      << seconds * 1000.0 / str.report.processingMs << std::setprecision(1)
                      << nat.report.outputPeakDbfs << "/" << str.report.outputPeakDbfs << "\n";
            std::cout << "  " << jsonReport("natural", nat.report, sr) << "\n";
            std::cout << "  " << jsonReport("strong", str.report, sr) << "\n";
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--synthetic") {
        return syntheticRun(argv[2]);
    }
    if (argc < 3) {
        std::cerr << "usage: bgmedia_vox_enhance <in.wav> <out_prefix> [--mode natural|strong|all]\n"
                     "       bgmedia_vox_enhance --synthetic <out_prefix>\n";
        return 1;
    }
    std::string mode = "all";
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        }
    }
    if (mode != "natural" && mode != "strong" && mode != "all") {
        std::cerr << "unknown mode: " << mode << "\n";
        return 1;
    }
    return processFile(argv[1], argv[2], mode);
}
