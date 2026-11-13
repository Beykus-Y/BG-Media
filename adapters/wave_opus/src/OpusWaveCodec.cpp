#include "bgmedia/wave/OpusWaveCodec.hpp"

#include <opus/opus.h>

#include <algorithm>

namespace bgmedia::wave {

struct OpusWaveCodec::Impl {
    OpusEncoder* encoder = nullptr;
    OpusDecoder* decoder = nullptr;

    ~Impl() {
        if (encoder) {
            opus_encoder_destroy(encoder);
        }
        if (decoder) {
            opus_decoder_destroy(decoder);
        }
    }
};

OpusWaveCodec::OpusWaveCodec(std::uint32_t bitrateBps)
    : impl_(std::make_unique<Impl>()),
      bitrateBps_(std::clamp(bitrateBps, kWaveMinBitrateBps, kWaveMaxBitrateBps)) {
    int encErr = OPUS_OK;
    impl_->encoder = opus_encoder_create(
        static_cast<opus_int32>(kWaveSampleRate),
        static_cast<int>(kWaveChannels),
        OPUS_APPLICATION_AUDIO,
        &encErr);
    if (impl_->encoder && encErr == OPUS_OK) {
        opus_encoder_ctl(impl_->encoder,
                         OPUS_SET_BITRATE(static_cast<opus_int32>(bitrateBps_)));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(8));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(2));
    }

    int decErr = OPUS_OK;
    impl_->decoder = opus_decoder_create(
        static_cast<opus_int32>(kWaveSampleRate),
        static_cast<int>(kWaveChannels),
        &decErr);
    if (decErr != OPUS_OK && impl_->decoder) {
        opus_decoder_destroy(impl_->decoder);
        impl_->decoder = nullptr;
    }
}

OpusWaveCodec::~OpusWaveCodec() = default;

bool OpusWaveCodec::isReady() const noexcept {
    return impl_ && impl_->encoder != nullptr && impl_->decoder != nullptr;
}

bool OpusWaveCodec::encode(std::span<const float> interleavedPcm,
                           std::vector<std::uint8_t>& outPayload) {
    if (!isReady() ||
        interleavedPcm.size() != kWaveFrameSamples * kWaveChannels) {
        return false;
    }
    outPayload.resize(1500);
    const int encodedBytes = opus_encode_float(
        impl_->encoder,
        interleavedPcm.data(),
        static_cast<int>(kWaveFrameSamples),
        outPayload.data(),
        static_cast<opus_int32>(outPayload.size()));
    if (encodedBytes < 0) {
        outPayload.clear();
        return false;
    }
    outPayload.resize(static_cast<std::size_t>(encodedBytes));
    return true;
}

bool OpusWaveCodec::decode(std::span<const std::uint8_t> payload,
                           std::span<float> outInterleavedPcm) {
    if (!isReady() || payload.empty() ||
        outInterleavedPcm.size() != kWaveFrameSamples * kWaveChannels) {
        return false;
    }
    const int decoded = opus_decode_float(
        impl_->decoder,
        payload.data(),
        static_cast<opus_int32>(payload.size()),
        outInterleavedPcm.data(),
        static_cast<int>(kWaveFrameSamples),
        0);
    return decoded == static_cast<int>(kWaveFrameSamples);
}

bool OpusWaveCodec::decodePlc(std::span<float> outInterleavedPcm) {
    if (!isReady() ||
        outInterleavedPcm.size() != kWaveFrameSamples * kWaveChannels) {
        return false;
    }
    const int decoded = opus_decode_float(
        impl_->decoder,
        nullptr,
        0,
        outInterleavedPcm.data(),
        static_cast<int>(kWaveFrameSamples),
        0);
    return decoded == static_cast<int>(kWaveFrameSamples);
}

void OpusWaveCodec::setBitrate(std::uint32_t bps) {
    bitrateBps_ = std::clamp(bps, kWaveMinBitrateBps, kWaveMaxBitrateBps);
    if (impl_ && impl_->encoder) {
        opus_encoder_ctl(impl_->encoder,
                         OPUS_SET_BITRATE(static_cast<opus_int32>(bitrateBps_)));
    }
}

void OpusWaveCodec::setPacketLossPercent(int lossPercent) {
    if (impl_ && impl_->encoder) {
        const int clamped = std::clamp(lossPercent, 0, 100);
        opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(clamped));
    }
}

void OpusWaveCodec::reset() {
    if (impl_ && impl_->encoder) {
        opus_encoder_ctl(impl_->encoder, OPUS_RESET_STATE);
    }
    if (impl_ && impl_->decoder) {
        opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
    }
}

} // namespace bgmedia::wave
