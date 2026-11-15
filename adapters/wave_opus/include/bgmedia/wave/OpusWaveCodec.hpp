#pragma once

#include "bgmedia/wave/WaveCodec.hpp"

#include <atomic>
#include <memory>

namespace bgmedia::wave {

// Optional libopus codec adapter (48 kHz stereo, 20 ms frames, 128-160 kbit/s
// default profile, in-band FEC and PLC enabled). Built only when
// BGMEDIA_BUILD_WAVE_OPUS=ON.
class OpusWaveCodec final : public IWaveAudioCodec {
public:
    explicit OpusWaveCodec(std::uint32_t bitrateBps = kWaveDefaultBitrateBps);
    ~OpusWaveCodec() override;

    OpusWaveCodec(const OpusWaveCodec&) = delete;
    OpusWaveCodec& operator=(const OpusWaveCodec&) = delete;

    [[nodiscard]] bool isReady() const noexcept;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "opus-48k-stereo";
    }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept override {
        return kWaveSampleRate;
    }
    [[nodiscard]] std::uint32_t channels() const noexcept override {
        return kWaveChannels;
    }
    [[nodiscard]] std::size_t frameSamples() const noexcept override {
        return kWaveFrameSamples;
    }

    [[nodiscard]] bool encode(std::span<const float> interleavedPcm,
                              std::vector<std::uint8_t>& outPayload) override;
    [[nodiscard]] bool decode(std::span<const std::uint8_t> payload,
                              std::span<float> outInterleavedPcm) override;
    [[nodiscard]] bool decodePlc(std::span<float> outInterleavedPcm) override;
    // Decodes an in-band FEC frame from `nextPayload` when the preceding
    // packet was lost.
    [[nodiscard]] bool decodeFec(std::span<const std::uint8_t> nextPayload,
                                 std::span<float> outInterleavedPcm);

    void setBitrate(std::uint32_t bps) override;
    [[nodiscard]] std::uint32_t bitrate() const noexcept override {
        return bitrateBps_.load(std::memory_order_acquire);
    }
    void setPacketLossPercent(int lossPercent) override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<std::uint32_t> bitrateBps_{kWaveDefaultBitrateBps};
};

} // namespace bgmedia::wave
