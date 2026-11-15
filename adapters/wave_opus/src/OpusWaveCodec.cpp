#include "bgmedia/wave/OpusWaveCodec.hpp"

#if __has_include(<opus/opus.h>)
#include <opus/opus.h>
#else
#include <opus.h>
#endif

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>

namespace bgmedia::wave {

namespace {

inline constexpr std::size_t kMaxOpusPayloadBytes = 4096u;

} // namespace

struct OpusWaveCodec::Impl {
    OpusEncoder* encoder = nullptr;
    OpusDecoder* decoder = nullptr;
    std::mutex encoderMutex;
    std::mutex decoderMutex;

    ~Impl() {
        if (encoder) {
            opus_encoder_destroy(encoder);
        }
        if (decoder) {
            opus_decoder_destroy(decoder);
        }
    }

    bool decodeFrame(const std::uint8_t* payloadData,
                     opus_int32 payloadBytes,
                     int decodeFecFlag,
                     std::span<float> outInterleavedPcm) {
        if (!decoder ||
            outInterleavedPcm.size() != kWaveFrameSamples * kWaveChannels) {
            return false;
        }
        std::lock_guard<std::mutex> lock(decoderMutex);
        const int decoded = opus_decode_float(
            decoder,
            payloadData,
            payloadBytes,
            outInterleavedPcm.data(),
            static_cast<int>(kWaveFrameSamples),
            decodeFecFlag);
        if (decoded <= 0 || decoded > static_cast<int>(kWaveFrameSamples)) {
            return false;
        }
        const std::size_t decodedSamples =
            static_cast<std::size_t>(decoded) * kWaveChannels;
        if (decodedSamples < outInterleavedPcm.size()) {
            std::fill(outInterleavedPcm.begin() +
                          static_cast<std::ptrdiff_t>(decodedSamples),
                      outInterleavedPcm.end(), 0.0f);
        }
        return true;
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
        const auto initialBitrate =
            static_cast<opus_int32>(bitrateBps_.load(std::memory_order_acquire));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(initialBitrate));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(8));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(2));
    } else if (impl_->encoder) {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
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
    std::lock_guard<std::mutex> lock(impl_->encoderMutex);
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
        payload.size() > kMaxOpusPayloadBytes ||
        payload.size() >
            static_cast<std::size_t>(std::numeric_limits<opus_int32>::max())) {
        return false;
    }
    return impl_->decodeFrame(payload.data(),
                              static_cast<opus_int32>(payload.size()), 0,
                              outInterleavedPcm);
}

bool OpusWaveCodec::decodePlc(std::span<float> outInterleavedPcm) {
    if (!isReady()) {
        return false;
    }
    return impl_->decodeFrame(nullptr, 0, 0, outInterleavedPcm);
}

bool OpusWaveCodec::decodeFec(std::span<const std::uint8_t> nextPayload,
                              std::span<float> outInterleavedPcm) {
    if (!isReady() || nextPayload.empty() ||
        nextPayload.size() > kMaxOpusPayloadBytes ||
        nextPayload.size() >
            static_cast<std::size_t>(std::numeric_limits<opus_int32>::max())) {
        return false;
    }
    return impl_->decodeFrame(nextPayload.data(),
                              static_cast<opus_int32>(nextPayload.size()), 1,
                              outInterleavedPcm);
}

void OpusWaveCodec::setBitrate(std::uint32_t bps) {
    const std::uint32_t clamped =
        std::clamp(bps, kWaveMinBitrateBps, kWaveMaxBitrateBps);
    bitrateBps_.store(clamped, std::memory_order_release);
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->encoderMutex);
    if (impl_->encoder) {
        opus_encoder_ctl(impl_->encoder,
                         OPUS_SET_BITRATE(static_cast<opus_int32>(clamped)));
    }
}

void OpusWaveCodec::setPacketLossPercent(int lossPercent) {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->encoderMutex);
    if (impl_->encoder) {
        const int clamped = std::clamp(lossPercent, 0, 100);
        opus_encoder_ctl(impl_->encoder, OPUS_SET_PACKET_LOSS_PERC(clamped));
    }
}

void OpusWaveCodec::reset() {
    if (!impl_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->encoderMutex);
        if (impl_->encoder) {
            opus_encoder_ctl(impl_->encoder, OPUS_RESET_STATE);
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->decoderMutex);
        if (impl_->decoder) {
            opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
        }
    }
}

} // namespace bgmedia::wave
