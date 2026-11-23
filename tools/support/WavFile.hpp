#pragma once

// Minimal 16-bit PCM WAV reader/writer shared by the BG Vox tools. Header-only,
// no dependencies beyond the STL. Not a general WAV library: PCM16, mono or
// stereo, one data chunk.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace bgmedia::toolsupport {

struct WavFile {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<std::int16_t> samples;  // interleaved
};

inline std::uint32_t wavRd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint16_t wavRd16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline bool readWav(const std::string& path, WavFile& wav, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 ||
        std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    std::size_t pos = 12;
    bool haveFmt = false;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    while (pos + 8 <= d.size()) {
        const std::uint32_t size = wavRd32(&d[pos + 4]);
        const std::size_t body = pos + 8;
        if (std::memcmp(&d[pos], "fmt ", 4) == 0 && body + 16 <= d.size()) {
            format = wavRd16(&d[body]);
            wav.channels = wavRd16(&d[body + 2]);
            wav.sampleRate = wavRd32(&d[body + 4]);
            bits = wavRd16(&d[body + 14]);
            haveFmt = true;
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            if (!haveFmt || format != 1 || bits != 16 || wav.channels < 1 || wav.channels > 2) {
                error = "only PCM16 mono/stereo WAV is supported";
                return false;
            }
            const std::size_t avail = std::min<std::size_t>(size, d.size() - body);
            wav.samples.resize(avail / 2);
            for (std::size_t i = 0; i < wav.samples.size(); ++i) {
                wav.samples[i] = static_cast<std::int16_t>(wavRd16(&d[body + i * 2]));
            }
            return true;
        }
        pos = body + size + (size & 1u);
    }
    error = "no data chunk";
    return false;
}

inline void wavWr32(std::ofstream& f, std::uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}

inline void wavWr16(std::ofstream& f, std::uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}

inline bool writeWav(const std::string& path, std::uint32_t sampleRate, std::uint32_t channels,
                     const std::vector<std::int16_t>& samples) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 2);
    f.write("RIFF", 4);
    wavWr32(f, 36 + bytes);
    f.write("WAVEfmt ", 8);
    wavWr32(f, 16);
    wavWr16(f, 1);
    wavWr16(f, static_cast<std::uint16_t>(channels));
    wavWr32(f, sampleRate);
    wavWr32(f, sampleRate * channels * 2);
    wavWr16(f, static_cast<std::uint16_t>(channels * 2));
    wavWr16(f, 16);
    f.write("data", 4);
    wavWr32(f, bytes);
    for (std::int16_t v : samples) {
        wavWr16(f, static_cast<std::uint16_t>(v));
    }
    return static_cast<bool>(f);
}

} // namespace bgmedia::toolsupport
