#include "bgmedia/audio/WavDecoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace bgmedia {
namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
constexpr std::uint32_t kMaxChannels = 32;

std::uint16_t readU16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t readU32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readU64(const std::uint8_t* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

struct FmtInfo {
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    bool floatData = false;
};

bool parseFmt(const std::vector<std::uint8_t>& chunk, FmtInfo& out, std::string& error) {
    if (chunk.size() < 16) {
        error = "WAV fmt chunk is shorter than 16 bytes";
        return false;
    }

    out.format = readU16(chunk.data());
    out.channels = readU16(chunk.data() + 2);
    out.sampleRate = readU32(chunk.data() + 4);
    out.blockAlign = readU16(chunk.data() + 12);
    out.bitsPerSample = readU16(chunk.data() + 14);

    if (out.format == kFormatExtensible) {
        if (chunk.size() < 40) {
            error = "WAV extensible fmt chunk is truncated";
            return false;
        }
        const std::uint16_t subFormat = readU16(chunk.data() + 24);
        if (subFormat != kFormatPcm && subFormat != kFormatFloat) {
            error = "unsupported WAV extensible sub-format";
            return false;
        }
        out.format = subFormat;
    }

    if (out.format != kFormatPcm && out.format != kFormatFloat) {
        error = "unsupported WAV encoding (only PCM and IEEE float are supported)";
        return false;
    }
    if (out.channels == 0 || out.channels > kMaxChannels) {
        error = "unsupported WAV channel count";
        return false;
    }
    if (out.sampleRate < 1000 || out.sampleRate > 768000) {
        error = "unsupported WAV sample rate";
        return false;
    }
    if (out.bitsPerSample == 0 || out.bitsPerSample > 64) {
        error = "unsupported WAV bit depth";
        return false;
    }

    if (out.format == kFormatFloat) {
        if (out.bitsPerSample != 32 && out.bitsPerSample != 64) {
            error = "IEEE float WAV must be 32 or 64 bits";
            return false;
        }
        out.floatData = true;
    } else {
        switch (out.bitsPerSample) {
        case 8:
        case 16:
        case 24:
        case 32:
            break;
        default:
            error = "unsupported PCM WAV bit depth (8/16/24/32 only)";
            return false;
        }
    }

    const std::uint32_t expectedAlign =
        static_cast<std::uint32_t>(out.channels) * (out.bitsPerSample / 8u);
    if (out.blockAlign == 0) {
        out.blockAlign = static_cast<std::uint16_t>(expectedAlign);
    }
    if (out.blockAlign < expectedAlign) {
        error = "WAV block alignment is smaller than one sample frame";
        return false;
    }

    return true;
}

float decodeSample(const std::uint8_t* p, const FmtInfo& fmt) noexcept {
    if (fmt.floatData) {
        if (fmt.bitsPerSample == 32) {
            const std::uint32_t bits = readU32(p);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        const std::uint64_t bits = readU64(p);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<float>(value);
    }

    switch (fmt.bitsPerSample) {
    case 8:
        return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
    case 16: {
        const auto raw = static_cast<std::int16_t>(readU16(p));
        return static_cast<float>(raw) / 32768.0f;
    }
    case 24: {
        std::int32_t raw = static_cast<std::int32_t>(p[0]) |
                           (static_cast<std::int32_t>(p[1]) << 8) |
                           (static_cast<std::int32_t>(p[2]) << 16);
        if (raw & 0x00800000) {
            raw |= ~0x00FFFFFF;
        }
        return static_cast<float>(raw) / 8388608.0f;
    }
    case 32: {
        const auto raw = static_cast<std::int32_t>(readU32(p));
        return static_cast<float>(static_cast<double>(raw) / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

} // namespace

bool WavDecoder::supports(const std::filesystem::path& path) const {
    return lowerExtension(path) == ".wav";
}

DecodeResult WavDecoder::decode(const std::filesystem::path& path,
                                const DecodeProgressCallback& progress) const {
    if (progress) {
        progress(0.0f, "opening WAV");
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return DecodeResult::failure("cannot open WAV file: " + path.string());
    }

    const std::streamoff fileSize = file.tellg();
    if (fileSize < 44) {
        return DecodeResult::failure("file is too small to be a WAV container");
    }
    file.seekg(0, std::ios::beg);

    std::array<std::uint8_t, 12> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
        return DecodeResult::failure("failed to read RIFF header");
    }
    if (std::memcmp(header.data(), "RIFF", 4) != 0 ||
        std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
        return DecodeResult::failure("not a RIFF/WAVE file");
    }

    FmtInfo fmt;
    bool haveFmt = false;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataSize = 0;

    while (file.tellg() >= 0 &&
           file.tellg() + static_cast<std::streamoff>(8) <= fileSize) {
        std::array<std::uint8_t, 8> chunkHeader{};
        if (!file.read(reinterpret_cast<char*>(chunkHeader.data()), chunkHeader.size())) {
            break;
        }
        const std::uint32_t chunkSize = readU32(chunkHeader.data() + 4);
        const std::streamoff chunkDataPos = file.tellg();
        if (chunkDataPos < 0 || chunkSize > static_cast<std::uint64_t>(fileSize - chunkDataPos)) {
            return DecodeResult::failure("WAV chunk extends beyond the end of the file");
        }

        if (std::memcmp(chunkHeader.data(), "fmt ", 4) == 0) {
            if (chunkSize < 16 || chunkSize > 4096) {
                return DecodeResult::failure("invalid WAV fmt chunk size");
            }
            std::vector<std::uint8_t> chunk(chunkSize);
            if (!file.read(reinterpret_cast<char*>(chunk.data()), chunkSize)) {
                return DecodeResult::failure("failed to read WAV fmt chunk");
            }
            std::string parseError;
            if (!parseFmt(chunk, fmt, parseError)) {
                return DecodeResult::failure(parseError);
            }
            haveFmt = true;
        } else if (std::memcmp(chunkHeader.data(), "data", 4) == 0) {
            dataOffset = static_cast<std::uint64_t>(chunkDataPos);
            dataSize = chunkSize;
            // Continue scanning for fmt if it comes after data; most files put
            // fmt first, but supporting both costs nothing.
        }

        std::uint64_t next = static_cast<std::uint64_t>(chunkDataPos) + chunkSize;
        if (chunkSize & 1u) {
            ++next; // RIFF chunks are word aligned.
        }
        if (next > static_cast<std::uint64_t>(fileSize)) {
            break;
        }
        file.seekg(static_cast<std::streamoff>(next), std::ios::beg);
    }

    if (!haveFmt) {
        return DecodeResult::failure("WAV file has no fmt chunk");
    }
    if (dataOffset == 0 || dataSize == 0) {
        return DecodeResult::failure("WAV file has no audio data");
    }

    const std::uint64_t frameCount = dataSize / fmt.blockAlign;
    if (frameCount == 0) {
        return DecodeResult::failure("WAV data chunk contains no complete frames");
    }
    const std::uint64_t sampleCount = frameCount * fmt.channels;
    if (sampleCount > 0xFFFFFFFFULL) {
        return DecodeResult::failure("WAV file is too large for this decoder");
    }

    std::vector<float> samples(static_cast<std::size_t>(sampleCount));
    file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);

    constexpr std::size_t kBlockFrames = 8192;
    std::vector<std::uint8_t> block(kBlockFrames * fmt.blockAlign);
    std::size_t outIndex = 0;
    std::uint64_t framesLeft = frameCount;

    while (framesLeft > 0) {
        const std::size_t framesThisBlock = static_cast<std::size_t>(
            std::min<std::uint64_t>(framesLeft, kBlockFrames));
        const std::size_t bytesThisBlock = framesThisBlock * fmt.blockAlign;
        if (!file.read(reinterpret_cast<char*>(block.data()),
                       static_cast<std::streamsize>(bytesThisBlock))) {
            return DecodeResult::failure("unexpected end of WAV data");
        }

        const std::size_t bytesPerSample = fmt.bitsPerSample / 8;
        for (std::size_t frame = 0; frame < framesThisBlock; ++frame) {
            const std::uint8_t* framePtr = block.data() + frame * fmt.blockAlign;
            for (std::uint16_t ch = 0; ch < fmt.channels; ++ch) {
                samples[outIndex++] = decodeSample(framePtr + ch * bytesPerSample, fmt);
            }
        }

        framesLeft -= framesThisBlock;
        if (progress) {
            progress(static_cast<float>(frameCount - framesLeft) /
                         static_cast<float>(frameCount),
                     "decoding WAV");
        }
    }

    if (progress) {
        progress(1.0f, "WAV decoded");
    }

    AudioData audio;
    audio.samples = std::move(samples);
    audio.channels = fmt.channels;
    audio.sampleRate = fmt.sampleRate;
    return DecodeResult::success(std::move(audio));
}

} // namespace bgmedia
