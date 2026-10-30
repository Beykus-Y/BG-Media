#include "bgmedia/audio/MiniaudioMixDevice.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace bgmedia {
namespace {

constexpr ma_uint32 kChannels = 2;
constexpr ma_uint32 kPeriodFrames = 512;

} // namespace

struct MiniaudioMixDevice::Impl {
    explicit Impl(mix::MixEngine& engine) : engine(engine) {}

    mix::MixEngine& engine;
    ma_device device{};
    bool deviceReady = false;
    ma_uint32 sampleRate = 48000;
    std::string deviceName;
    std::string error;

    std::atomic<double> baseLatency{0.0};
    std::atomic<double> callbackLoad{0.0};
    std::atomic<std::uint64_t> overruns{0};

    static void dataCallback(ma_device* device, void* output, const void*,
                             ma_uint32 frameCount) {
        auto* self = static_cast<Impl*>(device->pUserData);
        auto* samples = static_cast<float*>(output);
        if (!self) {
            std::memset(samples, 0,
                        static_cast<std::size_t>(frameCount) * kChannels *
                            sizeof(float));
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        self->engine.render(samples, static_cast<std::size_t>(frameCount));
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          started)
                .count();
        const double period =
            static_cast<double>(frameCount) /
            static_cast<double>(self->sampleRate);
        if (period > 0.0) {
            const double load = elapsed / period;
            const double previous =
                self->callbackLoad.load(std::memory_order_relaxed);
            self->callbackLoad.store(std::max(previous * 0.90, load),
                                     std::memory_order_relaxed);
            if (load > 1.0) {
                self->overruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};

MiniaudioMixDevice::MiniaudioMixDevice(mix::MixEngine& engine)
    : impl_(std::make_unique<Impl>(engine)) {}

MiniaudioMixDevice::~MiniaudioMixDevice() {
    close();
}

bool MiniaudioMixDevice::open(std::uint32_t sampleRate,
                              std::string* error) {
    close();
    impl_->error.clear();
    impl_->sampleRate = sampleRate ? sampleRate : 48000;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = impl_->sampleRate;
    config.periodSizeInFrames = kPeriodFrames;
    config.periods = 3;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = impl_.get();

    const ma_result result = ma_device_init(nullptr, &config, &impl_->device);
    if (result != MA_SUCCESS) {
        impl_->error = std::string("mix audio device unavailable: ") +
                       ma_result_description(result);
        if (error) {
            *error = impl_->error;
        }
        return false;
    }
    impl_->deviceReady = true;

    char name[256] = {};
    if (ma_device_get_name(&impl_->device, ma_device_type_playback, name,
                           sizeof(name), nullptr) == MA_SUCCESS &&
        name[0] != '\0') {
        impl_->deviceName = name;
    } else {
        impl_->deviceName = "default output";
    }

    const double periods = std::max(1u, impl_->device.playback.internalPeriods);
    const double frames =
        std::max(1u, impl_->device.playback.internalPeriodSizeInFrames);
    impl_->baseLatency.store(periods * frames / impl_->sampleRate,
                             std::memory_order_relaxed);
    impl_->callbackLoad.store(0.0, std::memory_order_relaxed);
    impl_->overruns.store(0, std::memory_order_relaxed);

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        impl_->error = "failed to start mix audio device";
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
        if (error) {
            *error = impl_->error;
        }
        return false;
    }

    impl_->engine.setOutputLatency(
        impl_->baseLatency.load(std::memory_order_relaxed));
    return true;
}

void MiniaudioMixDevice::close() {
    if (impl_->deviceReady) {
        ma_device_stop(&impl_->device);
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
    }
    impl_->engine.setOutputLatency(0.0);
}

bool MiniaudioMixDevice::isOpen() const noexcept {
    return impl_->deviceReady;
}

const std::string& MiniaudioMixDevice::deviceName() const noexcept {
    return impl_->deviceName;
}

const std::string& MiniaudioMixDevice::lastError() const noexcept {
    return impl_->error;
}

double MiniaudioMixDevice::baseLatency() const noexcept {
    return impl_->baseLatency.load(std::memory_order_relaxed);
}

double MiniaudioMixDevice::callbackLoad() const noexcept {
    return impl_->callbackLoad.load(std::memory_order_relaxed);
}

std::uint64_t MiniaudioMixDevice::overruns() const noexcept {
    return impl_->overruns.load(std::memory_order_relaxed);
}

} // namespace bgmedia
