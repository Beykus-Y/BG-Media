#pragma once

#include "bgmedia/wave/WaveTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bgmedia::wave {

// Portable audio codec boundary for BG Wave. Implementations may wrap Opus
// (in an optional adapter) or a dependency-free reference PCM codec for tests.
class IWaveAudioCodec {
public:
    virtual ~IWaveAudioCodec() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sampleRate() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t channels() const noexcept = 0;
    [[nodiscard]] virtual std::size_t frameSamples() const noexcept = 0;

    // Encodes one frame (`frameSamples() * channels()` interleaved float32
    // samples) into `outPayload`.
    [[nodiscard]] virtual bool encode(
        std::span<const float> interleavedPcm,
        std::vector<std::uint8_t>& outPayload) = 0;

    // Decodes `payload` into `outInterleavedPcm` (`frameSamples() * channels()`
    // floats).
    [[nodiscard]] virtual bool decode(
        std::span<const std::uint8_t> payload,
        std::span<float> outInterleavedPcm) = 0;

    // Generates one frame of packet-loss concealment audio.
    [[nodiscard]] virtual bool decodePlc(
        std::span<float> outInterleavedPcm) = 0;

    virtual void setBitrate(std::uint32_t bps) = 0;
    [[nodiscard]] virtual std::uint32_t bitrate() const noexcept = 0;
    virtual void setPacketLossPercent(int lossPercent) = 0;
    virtual void reset() = 0;
};

// Dependency-free 16-bit little-endian PCM reference codec for core-only builds,
// loopback verification, and deterministic unit tests.
class Pcm16WaveCodec final : public IWaveAudioCodec {
public:
    explicit Pcm16WaveCodec(std::uint32_t sampleRate = kWaveSampleRate,
                            std::uint32_t channels = kWaveChannels,
                            std::size_t frameSamples = kWaveFrameSamples);

    [[nodiscard]] std::string_view name() const noexcept override {
        return "pcm16-reference";
    }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept override {
        return sampleRate_;
    }
    [[nodiscard]] std::uint32_t channels() const noexcept override {
        return channels_;
    }
    [[nodiscard]] std::size_t frameSamples() const noexcept override {
        return frameSamples_;
    }

    [[nodiscard]] bool encode(std::span<const float> interleavedPcm,
                              std::vector<std::uint8_t>& outPayload) override;
    [[nodiscard]] bool decode(std::span<const std::uint8_t> payload,
                              std::span<float> outInterleavedPcm) override;
    [[nodiscard]] bool decodePlc(std::span<float> outInterleavedPcm) override;

    void setBitrate(std::uint32_t bps) override { bitrate_ = bps; }
    [[nodiscard]] std::uint32_t bitrate() const noexcept override {
        return bitrate_;
    }
    void setPacketLossPercent(int /*lossPercent*/) override {}
    void reset() override;

private:
    std::uint32_t sampleRate_ = kWaveSampleRate;
    std::uint32_t channels_ = kWaveChannels;
    std::size_t frameSamples_ = kWaveFrameSamples;
    std::uint32_t bitrate_ = kWaveDefaultBitrateBps;
    std::vector<float> lastDecoded_;
    int plcStreak_ = 0;
};

} // namespace bgmedia::wave
