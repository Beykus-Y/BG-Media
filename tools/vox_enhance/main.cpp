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

namespace {

struct Wav {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<std::int16_t> samples;  // interleaved
};

std::uint32_t rd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t rd16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

bool readWav(const std::string& path, Wav& wav, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    std::size_t pos = 12;
    bool haveFmt = false;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    while (pos + 8 <= d.size()) {
        const std::uint32_t size = rd32(&d[pos + 4]);
        const std::size_t body = pos + 8;
        if (std::memcmp(&d[pos], "fmt ", 4) == 0 && body + 16 <= d.size()) {
            format = rd16(&d[body]);
            wav.channels = rd16(&d[body + 2]);
            wav.sampleRate = rd32(&d[body + 4]);
            bits = rd16(&d[body + 14]);
            haveFmt = true;
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            if (!haveFmt || format != 1 || bits != 16 || wav.channels < 1 || wav.channels > 2) {
                error = "only PCM16 mono/stereo WAV is supported";
                return false;
            }
            const std::size_t avail = std::min<std::size_t>(size, d.size() - body);
            wav.samples.resize(avail / 2);
            for (std::size_t i = 0; i < wav.samples.size(); ++i) {
                wav.samples[i] = static_cast<std::int16_t>(rd16(&d[body + i * 2]));
            }
            return true;
        }
        pos = body + size + (size & 1u);
    }
    error = "no data chunk";
    return false;
}

void wr32(std::ofstream& f, std::uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}
void wr16(std::ofstream& f, std::uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}

bool writeWav(const std::string& path, std::uint32_t sr, std::uint32_t ch,
              const std::vector<std::int16_t>& s) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(s.size() * 2);
    f.write("RIFF", 4);
    wr32(f, 36 + bytes);
    f.write("WAVEfmt ", 8);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, static_cast<std::uint16_t>(ch));
    wr32(f, sr);
    wr32(f, sr * ch * 2);
    wr16(f, static_cast<std::uint16_t>(ch * 2));
    wr16(f, 16);
    f.write("data", 4);
    wr32(f, bytes);
    for (std::int16_t v : s) {
        wr16(f, static_cast<std::uint16_t>(v));
    }
    return static_cast<bool>(f);
}

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
    Wav wav;
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
