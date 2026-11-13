#pragma once

#include "bgmedia/wave/WaveTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace bgmedia::wave {

enum class ExternalCallState {
    Idle = 0,
    Offering,
    Connecting,
    Live,
    Failed,
    Ended
};

struct ExternalCallCallbacks {
    std::function<void(ExternalCallState state, const std::string& reason)>
        onStateChanged;
};

// Host-implemented external call PCM bridge (e.g. Telegram Call fallback in
// BeyGram). BG-Media itself never depends on TDLib, NTgCalls, or Telegram APIs.
class IExternalCallTransport {
public:
    virtual ~IExternalCallTransport() = default;

    virtual void setCallbacks(ExternalCallCallbacks callbacks) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;

    // Host side: push 48 kHz interleaved stereo float32 PCM into the call
    // audio injection path (no microphone capture).
    virtual std::size_t pushOutgoingPcm(const float* interleavedStereo,
                                        std::size_t frames,
                                        std::uint32_t sampleRate) = 0;

    // Listener side: pull decoded PCM from the call audio output.
    virtual std::size_t pullIncomingPcm(float* interleavedStereo,
                                        std::size_t frames,
                                        std::uint32_t sampleRate) = 0;

    [[nodiscard]] virtual ExternalCallState state() const noexcept = 0;
    [[nodiscard]] virtual std::string fallbackNotice() const {
        return "Direct P2P connection failed. Using external call relay; audio quality may be lower.";
    }
};

} // namespace bgmedia::wave
