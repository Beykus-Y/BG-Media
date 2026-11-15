#include "bgmedia/wave/WaveCodec.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::wave {

Pcm16WaveCodec::Pcm16WaveCodec(std::uint32_t sampleRate,
                               std::uint32_t channels,
                               std::size_t frameSamples)
    : sampleRate_(sampleRate ? sampleRate : kWaveSampleRate),
      channels_(channels ? channels : kWaveChannels),
      frameSamples_(frameSamples ? frameSamples : kWaveFrameSamples),
      lastDecoded_(frameSamples_ * channels_, 0.0f) {}

bool Pcm16WaveCodec::encode(std::span<const float> interleavedPcm,
                            std::vector<std::uint8_t>& outPayload) {
    const std::size_t expectedSamples = frameSamples_ * channels_;
    if (interleavedPcm.size() != expectedSamples) {
        return false;
    }
    outPayload.resize(expectedSamples * 2);
    for (std::size_t i = 0; i < expectedSamples; ++i) {
        float s = interleavedPcm[i];
        if (!std::isfinite(s)) {
            s = 0.0f;
        }
        s = std::clamp(s, -1.0f, 1.0f);
        const auto q = static_cast<std::int16_t>(
            std::lrintf(s * 32767.0f));
        const auto u = static_cast<std::uint16_t>(q);
        outPayload[i * 2] = static_cast<std::uint8_t>(u & 0xFFu);
        outPayload[i * 2 + 1] = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    }
    return true;
}

bool Pcm16WaveCodec::decode(std::span<const std::uint8_t> payload,
                            std::span<float> outInterleavedPcm) {
    const std::size_t expectedSamples = frameSamples_ * channels_;
    if (payload.size() != expectedSamples * 2 ||
        outInterleavedPcm.size() != expectedSamples) {
        return false;
    }
    for (std::size_t i = 0; i < expectedSamples; ++i) {
        const std::uint16_t u =
            static_cast<std::uint16_t>(payload[i * 2]) |
            (static_cast<std::uint16_t>(payload[i * 2 + 1]) << 8);
        const auto q = static_cast<std::int16_t>(u);
        const float s = static_cast<float>(q) / 32767.0f;
        outInterleavedPcm[i] = s;
        lastDecoded_[i] = s;
    }
    plcStreak_ = 0;
    return true;
}

bool Pcm16WaveCodec::decodePlc(std::span<float> outInterleavedPcm) {
    const std::size_t expectedSamples = frameSamples_ * channels_;
    if (outInterleavedPcm.size() != expectedSamples) {
        return false;
    }
    if (plcStreak_ < 64) {
        ++plcStreak_;
    }
    if (plcStreak_ > 6) {
        std::fill(outInterleavedPcm.begin(), outInterleavedPcm.end(), 0.0f);
    } else {
        const float atten = std::pow(0.5f, static_cast<float>(plcStreak_));
        for (std::size_t i = 0; i < expectedSamples; ++i) {
            outInterleavedPcm[i] = lastDecoded_[i] * atten;
        }
    }
    return true;
}

void Pcm16WaveCodec::reset() {
    std::fill(lastDecoded_.begin(), lastDecoded_.end(), 0.0f);
    plcStreak_ = 0;
}

} // namespace bgmedia::wave
