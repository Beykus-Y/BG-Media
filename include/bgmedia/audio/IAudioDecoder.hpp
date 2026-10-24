#pragma once

#include "bgmedia/audio/AudioData.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace bgmedia {

using DecodeProgressCallback =
    std::function<void(float fraction, std::string_view stage)>;

struct DecodeResult {
    bool ok = false;
    AudioData audio;
    std::string error;

    [[nodiscard]] static DecodeResult failure(std::string message) {
        DecodeResult r;
        r.error = std::move(message);
        return r;
    }

    [[nodiscard]] static DecodeResult success(AudioData data) {
        DecodeResult r;
        r.ok = true;
        r.audio = std::move(data);
        return r;
    }
};

// Decoders are platform/backend adapters. The core never links a decoder by
// default: WavDecoder is provided for dependency-free WAV input, while the
// miniaudio adapter adds MP3/FLAC. Android can plug MediaCodec/NNAudio behind
// the same interface without touching the analyzer.
class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool supports(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual DecodeResult decode(
        const std::filesystem::path& path,
        const DecodeProgressCallback& progress = {}) const = 0;
};

} // namespace bgmedia
