#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bgmedia {

// Decoded, interleaved float PCM. This is the only audio representation the
// analysis layer accepts, which keeps decoding completely separate from DSP.
struct AudioData {
    std::vector<float> samples; // interleaved, nominally [-1, 1]
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;

    [[nodiscard]] bool valid() const noexcept {
        return sampleRate > 0 && channels > 0 && !samples.empty() &&
               samples.size() % channels == 0;
    }

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channels > 0 ? samples.size() / channels : 0;
    }

    [[nodiscard]] double duration() const noexcept {
        return sampleRate > 0 ? static_cast<double>(frameCount()) / sampleRate : 0.0;
    }

    // Deterministic downmix with NaN/Inf protection. Multi-channel material is
    // averaged, which is what the analysis stage needs; playback keeps its own
    // original stream.
    [[nodiscard]] std::vector<float> toMono() const;
};

// Mono PCM prepared for analysis, plus a content fingerprint used as part of
// the cache key. The fingerprint is over the mono samples, not the file path,
// so moving or renaming a file still hits the cache.
struct PcmBuffer {
    std::vector<float> samples;
    std::uint32_t sampleRate = 0;
    std::uint32_t sourceChannels = 1;
    std::uint64_t contentHash = 0;

    [[nodiscard]] bool valid() const noexcept {
        return sampleRate > 0 && !samples.empty();
    }

    [[nodiscard]] std::size_t frameCount() const noexcept { return samples.size(); }

    [[nodiscard]] double duration() const noexcept {
        return sampleRate > 0 ? static_cast<double>(samples.size()) / sampleRate : 0.0;
    }

    // Builds a sanitised mono buffer and computes the content hash.
    [[nodiscard]] static PcmBuffer fromAudio(const AudioData& audio, std::string* error = nullptr);
};

// FNV-1a over little-endian 32-bit float bit patterns. Stable across cache
// loads on the platforms BG Media targets.
[[nodiscard]] std::uint64_t contentHashOf(std::span<const float> samples) noexcept;

} // namespace bgmedia
