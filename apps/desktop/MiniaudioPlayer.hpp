#pragma once

#include "bgmedia/sync/Transport.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace bgmedia::desktop {

// Streaming playback adapter built directly on miniaudio's device + decoder.
// It implements the engine's IPlaybackController, so the UI and choreography
// never call a platform audio API themselves. Playback speed is implemented as
// a linear resampler (pitch follows speed); this is a preview feature, not a
// time-stretch DSP.
class MiniaudioPlayer final : public bgmedia::IPlaybackController {
public:
    MiniaudioPlayer();
    ~MiniaudioPlayer() override;

    MiniaudioPlayer(const MiniaudioPlayer&) = delete;
    MiniaudioPlayer& operator=(const MiniaudioPlayer&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path,
                            double durationHintSeconds,
                            std::string* error = nullptr);
    void close();

    [[nodiscard]] bool audioAvailable() const noexcept;
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] double duration() const noexcept;
    [[nodiscard]] double baseLatency() const noexcept;
    [[nodiscard]] double latencyTrim() const noexcept;
    void setLatencyTrim(double seconds);
    [[nodiscard]] const std::string& deviceName() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;

    void play() override;
    void pause() override;
    void seek(double mediaSeconds) override;
    void setRate(double rate) override;

    [[nodiscard]] bgmedia::TransportSnapshot snapshot() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::desktop
