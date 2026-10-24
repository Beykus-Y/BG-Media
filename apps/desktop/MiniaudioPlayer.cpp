#include "MiniaudioPlayer.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bgmedia::desktop {
namespace {

constexpr ma_uint32 kDeviceSampleRate = 48000;
constexpr ma_uint32 kChannels = 2;
constexpr ma_uint32 kPeriodFrames = 512;

ma_result openDecoder(const std::filesystem::path& path, ma_decoder* decoder) {
    const ma_decoder_config config =
        ma_decoder_config_init(ma_format_f32, kChannels, kDeviceSampleRate);
#ifdef _WIN32
    return ma_decoder_init_file_w(path.wstring().c_str(), &config, decoder);
#else
    const std::string native = path.string();
    return ma_decoder_init_file(native.c_str(), &config, decoder);
#endif
}

double hostSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct MiniaudioPlayer::Impl {
    static constexpr std::size_t kCapacityFrames = 32768;
    static constexpr ma_uint64 kReadFrames = 4096;

    ma_device device{};
    ma_decoder decoder{};
    bool decoderReady = false;
    bool deviceReady = false;

    std::vector<float> source; // interleaved stereo, capacity frames
    std::size_t sourceFrames = 0;
    double sourcePos = 0.0;
    double absoluteBase = 0.0;
    bool eof = false;

    std::atomic<bool> playing{false};
    std::atomic<bool> ended{false};
    std::atomic<double> rate{1.0};
    std::atomic<double> sourcePositionFrames{0.0};
    std::atomic<double> baseLatency{0.0};
    std::atomic<double> trim{0.0};
    std::atomic<std::uint64_t> discontinuity{0};

    double duration = 0.0;
    std::string deviceName;
    std::string error;

    Impl() : source(kCapacityFrames * kChannels, 0.0f) {}

    void fillSilence(float* output, ma_uint32 frames) {
        std::fill_n(output, static_cast<std::size_t>(frames) * kChannels, 0.0f);
    }

    bool readMore() {
        if (eof) {
            return false;
        }
        if (sourceFrames >= kCapacityFrames) {
            return false;
        }
        const ma_uint64 request = std::min<ma_uint64>(
            kReadFrames, kCapacityFrames - sourceFrames);
        ma_uint64 framesRead = 0;
        const ma_result result = ma_decoder_read_pcm_frames(
            &decoder, source.data() + sourceFrames * kChannels, request, &framesRead);
        if (framesRead > 0) {
            sourceFrames += static_cast<std::size_t>(framesRead);
        }
        if (result == MA_AT_END || result != MA_SUCCESS || framesRead < request) {
            eof = true;
        }
        return framesRead > 0;
    }

    void render(float* output, ma_uint32 frameCount) {
        fillSilence(output, frameCount);
        if (!playing.load(std::memory_order_relaxed)) {
            return;
        }

        const double speed = std::clamp(rate.load(std::memory_order_relaxed), 0.25, 2.0);
        ma_uint32 produced = 0;
        while (produced < frameCount) {
            if (sourcePos + 1.0 >= static_cast<double>(sourceFrames)) {
                const int drop = static_cast<int>(std::floor(sourcePos));
                if (drop > 0) {
                    const std::size_t remaining = sourceFrames - static_cast<std::size_t>(drop);
                    if (remaining > 0) {
                        std::memmove(source.data(),
                                     source.data() + static_cast<std::size_t>(drop) * kChannels,
                                     remaining * kChannels * sizeof(float));
                    }
                    sourceFrames = remaining;
                    sourcePos -= static_cast<double>(drop);
                    absoluteBase += static_cast<double>(drop);
                }
                while (sourcePos + 1.0 >= static_cast<double>(sourceFrames) && readMore()) {
                }
            }
            if (sourcePos + 1.0 >= static_cast<double>(sourceFrames)) {
                eof = true;
                ended.store(true, std::memory_order_relaxed);
                if (duration > 0.0) {
                    sourcePositionFrames.store(duration * kDeviceSampleRate,
                                               std::memory_order_relaxed);
                }
                break;
            }

            const std::size_t index = static_cast<std::size_t>(std::floor(sourcePos));
            const float frac = static_cast<float>(sourcePos - static_cast<double>(index));
            const float* a = source.data() + index * kChannels;
            const float* b = a + kChannels;
            output[static_cast<std::size_t>(produced) * kChannels] =
                a[0] + (b[0] - a[0]) * frac;
            output[static_cast<std::size_t>(produced) * kChannels + 1] =
                a[1] + (b[1] - a[1]) * frac;
            sourcePos += speed;
            ++produced;
        }

        double absolute = absoluteBase + sourcePos;
        if (duration > 0.0) {
            absolute = std::min(absolute, duration * kDeviceSampleRate);
        }
        sourcePositionFrames.store(absolute, std::memory_order_relaxed);
        if (eof && produced == 0) {
            ended.store(true, std::memory_order_relaxed);
        }
    }

    static void dataCallback(ma_device* device, void* output, const void*,
                             ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(device->pUserData);
        if (self) {
            self->render(static_cast<float*>(output), frameCount);
        } else {
            std::fill_n(static_cast<float*>(output),
                        static_cast<std::size_t>(frameCount) * kChannels, 0.0f);
        }
    }

    void resetStream(double startFrame) {
        sourceFrames = 0;
        sourcePos = 0.0;
        absoluteBase = startFrame;
        eof = false;
        ended.store(false, std::memory_order_relaxed);
        sourcePositionFrames.store(startFrame, std::memory_order_relaxed);
    }

    [[nodiscard]] double audibleLatency() const noexcept {
        return std::max(0.0, baseLatency.load(std::memory_order_relaxed) +
                                  trim.load(std::memory_order_relaxed));
    }
};

MiniaudioPlayer::MiniaudioPlayer() : impl_(std::make_unique<Impl>()) {}

MiniaudioPlayer::~MiniaudioPlayer() {
    close();
}

bool MiniaudioPlayer::open(const std::filesystem::path& path, double durationHintSeconds,
                           std::string* error) {
    close();
    impl_->error.clear();

    const ma_result decoderResult = openDecoder(path, &impl_->decoder);
    if (decoderResult != MA_SUCCESS) {
        impl_->error = std::string("cannot open audio: ") +
                       ma_result_description(decoderResult);
        if (error) {
            *error = impl_->error;
        }
        return false;
    }
    impl_->decoderReady = true;

    ma_uint64 lengthFrames = 0;
    const bool haveLength =
        ma_decoder_get_length_in_pcm_frames(&impl_->decoder, &lengthFrames) == MA_SUCCESS &&
        lengthFrames > 0;
    if (durationHintSeconds > 0.0) {
        impl_->duration = durationHintSeconds;
    } else if (haveLength) {
        impl_->duration = static_cast<double>(lengthFrames) / kDeviceSampleRate;
    } else {
        impl_->duration = 0.0;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kDeviceSampleRate;
    config.periodSizeInFrames = kPeriodFrames;
    config.periods = 3;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_.get();

    const ma_result deviceResult = ma_device_init(nullptr, &config, &impl_->device);
    if (deviceResult != MA_SUCCESS) {
        impl_->error = std::string("audio device unavailable: ") +
                       ma_result_description(deviceResult);
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoderReady = false;
        if (error) {
            *error = impl_->error;
        }
        return false;
    }
    impl_->deviceReady = true;

    char name[256] = {};
    if (ma_device_get_name(&impl_->device, ma_device_type_playback, name, sizeof(name),
                           nullptr) == MA_SUCCESS &&
        name[0] != '\0') {
        impl_->deviceName = name;
    } else {
        impl_->deviceName = "default output";
    }

    const double periods = std::max(1u, impl_->device.playback.internalPeriods);
    const double periodFrames =
        std::max(1u, impl_->device.playback.internalPeriodSizeInFrames);
    impl_->baseLatency.store(periods * periodFrames / kDeviceSampleRate,
                             std::memory_order_relaxed);
    impl_->rate.store(1.0, std::memory_order_relaxed);
    impl_->playing.store(false, std::memory_order_relaxed);
    impl_->resetStream(0.0);
    impl_->discontinuity.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void MiniaudioPlayer::close() {
    if (impl_->deviceReady) {
        impl_->playing.store(false, std::memory_order_relaxed);
        ma_device_stop(&impl_->device);
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
    }
    if (impl_->decoderReady) {
        ma_decoder_uninit(&impl_->decoder);
        impl_->decoderReady = false;
    }
    impl_->sourceFrames = 0;
    impl_->sourcePos = 0.0;
    impl_->duration = 0.0;
    impl_->deviceName.clear();
    impl_->ended.store(false, std::memory_order_relaxed);
}

bool MiniaudioPlayer::audioAvailable() const noexcept {
    return impl_->deviceReady && impl_->decoderReady;
}

bool MiniaudioPlayer::isOpen() const noexcept {
    return impl_->decoderReady;
}

bool MiniaudioPlayer::ended() const noexcept {
    return impl_->ended.load(std::memory_order_relaxed);
}

double MiniaudioPlayer::duration() const noexcept {
    return impl_->duration;
}

double MiniaudioPlayer::baseLatency() const noexcept {
    return impl_->baseLatency.load(std::memory_order_relaxed);
}

double MiniaudioPlayer::latencyTrim() const noexcept {
    return impl_->trim.load(std::memory_order_relaxed);
}

void MiniaudioPlayer::setLatencyTrim(double seconds) {
    impl_->trim.store(std::clamp(seconds, -0.25, 0.25), std::memory_order_relaxed);
}

const std::string& MiniaudioPlayer::deviceName() const noexcept {
    return impl_->deviceName;
}

const std::string& MiniaudioPlayer::lastError() const noexcept {
    return impl_->error;
}

void MiniaudioPlayer::play() {
    if (!audioAvailable()) {
        return;
    }
    if (impl_->ended.load(std::memory_order_relaxed) ||
        (impl_->duration > 0.0 &&
         impl_->sourcePositionFrames.load(std::memory_order_relaxed) >=
             impl_->duration * kDeviceSampleRate - 1.0)) {
        seek(0.0);
    }
    impl_->ended.store(false, std::memory_order_relaxed);
    impl_->playing.store(true, std::memory_order_relaxed);
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        impl_->playing.store(false, std::memory_order_relaxed);
        impl_->error = "failed to start audio device";
    }
}

void MiniaudioPlayer::pause() {
    if (!impl_->deviceReady) {
        impl_->playing.store(false, std::memory_order_relaxed);
        return;
    }
    if (impl_->playing.load(std::memory_order_relaxed)) {
        ma_device_stop(&impl_->device);
    }
    impl_->playing.store(false, std::memory_order_relaxed);
}

void MiniaudioPlayer::seek(double mediaSeconds) {
    if (!impl_->decoderReady) {
        return;
    }
    const bool wasPlaying = impl_->playing.load(std::memory_order_relaxed);
    pause();

    mediaSeconds = std::clamp(mediaSeconds, 0.0, std::max(0.0, impl_->duration));
    const ma_uint64 frame =
        static_cast<ma_uint64>(std::llround(mediaSeconds * kDeviceSampleRate));
    if (ma_decoder_seek_to_pcm_frame(&impl_->decoder, frame) != MA_SUCCESS) {
        impl_->error = "audio decoder failed to seek";
    }
    impl_->resetStream(static_cast<double>(frame));
    impl_->sourcePositionFrames.store(static_cast<double>(frame),
                                      std::memory_order_relaxed);
    impl_->discontinuity.fetch_add(1, std::memory_order_relaxed);

    if (wasPlaying) {
        play();
    }
}

void MiniaudioPlayer::setRate(double rate) {
    impl_->rate.store(std::clamp(rate, 0.25, 2.0), std::memory_order_relaxed);
    impl_->discontinuity.fetch_add(1, std::memory_order_relaxed);
}

bgmedia::TransportSnapshot MiniaudioPlayer::snapshot() const {
    bgmedia::TransportSnapshot snapshot;
    snapshot.sourcePosition =
        impl_->sourcePositionFrames.load(std::memory_order_relaxed) / kDeviceSampleRate;
    snapshot.duration = impl_->duration;
    snapshot.playing = impl_->playing.load(std::memory_order_relaxed) &&
                       !impl_->ended.load(std::memory_order_relaxed);
    snapshot.rate = impl_->rate.load(std::memory_order_relaxed);
    snapshot.outputLatency = impl_->audibleLatency();
    snapshot.discontinuity = impl_->discontinuity.load(std::memory_order_relaxed);
    snapshot.hostTime = hostSeconds();
    return snapshot;
}

} // namespace bgmedia::desktop
