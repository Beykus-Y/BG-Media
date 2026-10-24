#include "bgmedia/audio/AudioData.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace bgmedia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void hashByte(std::uint64_t& h, std::uint8_t byte) noexcept {
    h ^= byte;
    h *= kFnvPrime;
}

inline void hashU32(std::uint64_t& h, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        hashByte(h, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

inline void hashU64(std::uint64_t& h, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        hashByte(h, static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

} // namespace

std::vector<float> AudioData::toMono() const {
    if (!valid()) {
        return {};
    }

    const std::size_t frames = frameCount();
    std::vector<float> mono(frames, 0.0f);
    const float invChannels = 1.0f / static_cast<float>(channels);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float value = samples[frame * channels + ch];
            if (!std::isfinite(value)) {
                value = 0.0f;
            }
            sum += value;
        }
        mono[frame] = sum * invChannels;
    }
    return mono;
}

std::uint64_t contentHashOf(std::span<const float> samples) noexcept {
    std::uint64_t h = kFnvOffset;
    for (const float sample : samples) {
        std::uint32_t bits = 0;
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::memcpy(&bits, &sample, sizeof(bits));
        hashU32(h, bits);
    }
    return h;
}

PcmBuffer PcmBuffer::fromAudio(const AudioData& audio, std::string* error) {
    PcmBuffer out;
    if (!audio.valid()) {
        if (error) {
            *error = "decoded audio is empty or has invalid channel/sample-rate data";
        }
        return out;
    }

    out.samples = audio.toMono();
    if (out.samples.empty()) {
        if (error) {
            *error = "downmix produced no samples";
        }
        return out;
    }

    out.sampleRate = audio.sampleRate;
    out.sourceChannels = audio.channels;

    // Mix sample rate and frame count into the content fingerprint. The cache
    // stores both, but including them in the key avoids a cross-sample-rate
    // collision path entirely.
    std::uint64_t h = contentHashOf(out.samples);
    hashU32(h, out.sampleRate);
    hashU32(h, out.sourceChannels);
    hashU64(h, static_cast<std::uint64_t>(out.samples.size()));
    out.contentHash = h;

    return out;
}

} // namespace bgmedia
