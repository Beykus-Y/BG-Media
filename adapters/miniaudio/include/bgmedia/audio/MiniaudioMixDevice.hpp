#pragma once

// miniaudio output adapter for the BG Mix dual-deck engine. This is the only
// place that knows how a sound card is opened; MixEngine itself stays free of
// platform audio APIs.

#include "bgmedia/mix/MixEngine.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace bgmedia {

class MiniaudioMixDevice {
public:
    explicit MiniaudioMixDevice(mix::MixEngine& engine);
    ~MiniaudioMixDevice();

    MiniaudioMixDevice(const MiniaudioMixDevice&) = delete;
    MiniaudioMixDevice& operator=(const MiniaudioMixDevice&) = delete;

    [[nodiscard]] bool open(std::uint32_t sampleRate = 48000,
                            std::string* error = nullptr);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::string& deviceName() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;

    // Device-internal buffering reported by miniaudio, in seconds.
    [[nodiscard]] double baseLatency() const noexcept;
    // Share of one callback period used by the mixer in the last window.
    [[nodiscard]] double callbackLoad() const noexcept;
    // Callbacks whose render time exceeded their period (an approximation:
    // miniaudio does not expose an xrun counter for WASAPI).
    [[nodiscard]] std::uint64_t overruns() const noexcept;

    // Output validation gathered by the real callback. Stress runs use these
    // instead of an emulated render thread, so a real-device run measures the
    // samples the sound card actually received.
    [[nodiscard]] std::uint64_t renderedSamples() const noexcept;
    [[nodiscard]] std::uint64_t nonFiniteSamples() const noexcept;
    [[nodiscard]] float outputPeak() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia
