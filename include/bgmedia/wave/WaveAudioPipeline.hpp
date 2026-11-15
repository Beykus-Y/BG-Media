#pragma once

#include "bgmedia/audio/IPcmOutputTap.hpp"
#include "bgmedia/wave/WaveCodec.hpp"
#include "bgmedia/wave/WaveFallback.hpp"
#include "bgmedia/wave/WaveJitterBuffer.hpp"
#include "bgmedia/wave/WaveResampler.hpp"
#include "bgmedia/wave/WaveRingBuffer.hpp"
#include "bgmedia/wave/WaveSignaling.hpp"
#include "bgmedia/wave/WaveTransport.hpp"
#include "bgmedia/wave/WaveTypes.hpp"

#include <atomic>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bgmedia::wave {

struct WavePipelineConfig {
    WaveRole role = WaveRole::Host;
    std::uint32_t targetBitrateBps = kWaveDefaultBitrateBps;
    std::size_t ringCapacityFrames = 16384; // ~341 ms @ 48 kHz
    std::int64_t defaultOfferTtlSeconds = 120;
    WaveJitterBuffer::Config jitterConfig{};
};

struct WavePipelineCallbacks {
    std::function<void(WaveSessionState state, const std::string& reason)>
        onStateChanged;
    std::function<void(const WaveSignalMessage& signal)> onOutboundSignal;
    std::function<void(const WaveError& error)> onError;
};

// Portable BG Wave runtime pipeline and state machine for Host and Listener.
// Separates engine core (ring buffers, jitter buffer, clock/framing, signaling
// codec, state machine) from pluggable IWaveAudioCodec / IWaveTransport /
// IExternalCallTransport adapters.
class WaveAudioPipeline final
    : public IPcmOutputTap,
      public std::enable_shared_from_this<WaveAudioPipeline> {
public:
    WaveAudioPipeline(WavePipelineConfig config,
                      std::shared_ptr<IWaveAudioCodec> codec,
                      std::shared_ptr<IWaveTransport> transport);
    ~WaveAudioPipeline() override;

    WaveAudioPipeline(const WaveAudioPipeline&) = delete;
    WaveAudioPipeline& operator=(const WaveAudioPipeline&) = delete;

    void setCallbacks(WavePipelineCallbacks callbacks);

    // Signaling & lifecycle control.
    // For acceptOffer/acceptAnswer/handleRemoteSignal, nowUnixSeconds <= 0
    // resolves to wall-clock time (system_clock::now()) so TTL expiry checks
    // are never bypassed.
    bool startOffer(std::int64_t nowUnixSeconds,
                    std::int64_t ttlSeconds = 0);
    bool acceptOffer(const WaveSignalMessage& offer,
                     std::int64_t nowUnixSeconds);
    bool acceptAnswer(const WaveSignalMessage& answer,
                      std::int64_t nowUnixSeconds);
    bool handleRemoteSignal(const WaveSignalMessage& msg,
                            std::int64_t nowUnixSeconds);
    bool reconnect(std::int64_t nowUnixSeconds);
    void stop(bool emitEndSignal = true, std::int64_t nowUnixSeconds = 0);
    void reset();

    // Phase-2 fallback route: P2PFailed -> OfferingTelegramFallback ->
    // ConnectingTelegramCall -> LiveTelegramCall | Failed.
    bool switchToExternalCallFallback(
        std::shared_ptr<IExternalCallTransport> externalCall);

    // Instant host-side broadcast revocation (lock-free, safe from any thread).
    void setPublishingEnabled(bool enabled) noexcept;
    [[nodiscard]] bool publishingEnabled() const noexcept {
        return publishingEnabled_.load(std::memory_order_acquire);
    }

    // Listener-side local playback controls (lock-free).
    void setListenerGain(float gain) noexcept;
    [[nodiscard]] float listenerGain() const noexcept {
        return listenerGain_.load(std::memory_order_acquire);
    }
    void setListenerMuted(bool muted) noexcept;
    [[nodiscard]] bool listenerMuted() const noexcept {
        return listenerMuted_.load(std::memory_order_acquire);
    }

    // Host real-time audio boundary (IPcmOutputTap).
    void onPcmRendered(const float* interleavedStereo,
                       std::size_t frames,
                       std::uint32_t sampleRate) noexcept override;

    // Convenience alias for JNI/host callbacks that push PCM directly.
    void pushHostPcm(const float* interleavedStereo,
                     std::size_t frames,
                     std::uint32_t sampleRate) noexcept {
        onPcmRendered(interleavedStereo, frames, sampleRate);
    }

    // Listener real-time audio boundary.
    std::size_t pullListenerPcm(float* outInterleavedStereo,
                                std::size_t frames,
                                std::uint32_t sampleRate) noexcept;

    // Worker / tick pump (call periodically off the audio thread, e.g. every
    // 5-10 ms or per frame).
    void process(double currentTimeSeconds);

    [[nodiscard]] WaveRole role() const noexcept { return config_.role; }
    [[nodiscard]] WaveSessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] WaveTransportRoute route() const noexcept {
        return route_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::array<std::uint8_t, 16> sessionId() const noexcept;
    [[nodiscard]] WaveStats stats() const;
    [[nodiscard]] std::string diagnosticsSummary() const;

private:
    void setState(WaveSessionState next, const std::string& reason = {});
    void emitError(WaveErrorCode code, const std::string& message);
    void wireTransportCallbacks();
    void processHostStep(double currentTimeSeconds);
    void processListenerStep(double currentTimeSeconds);
    void requestOrPerformReset();
    [[nodiscard]] bool ensureResetCompletedForNewSession();
    void performResetUnderProcessLock();
    [[nodiscard]] WavePipelineCallbacks snapshotCallbacks() const;
    void publishStatsSnapshot();

    WavePipelineConfig config_;
    std::shared_ptr<IWaveAudioCodec> codec_;
    std::shared_ptr<IWaveTransport> transport_;
    std::shared_ptr<IExternalCallTransport> externalCall_;
    WavePipelineCallbacks callbacks_;

    mutable std::mutex controlMutex_;
    mutable std::mutex callbacksMutex_;
    mutable std::mutex sidMutex_;
    mutable std::mutex processMutex_;
    mutable std::mutex statsMutex_;
    std::atomic<bool> pendingRingReset_{false};
    std::atomic<std::uint64_t> sessionGen_{0};
    struct InboundPacket {
        WaveMediaPacket packet;
        double arrivalTimeSeconds = 0.0;
    };
    mutable std::mutex inboundMutex_;
    std::deque<InboundPacket> inboundQueue_;
    WaveStats publishedStats_{};
    std::atomic<bool> audioPausedForReset_{false};
    std::atomic<std::uint32_t> audioInFlight_{0};
    std::atomic<WaveSessionState> state_{WaveSessionState::Idle};
    std::atomic<WaveTransportRoute> route_{WaveTransportRoute::None};
    std::atomic<bool> publishingEnabled_{true};
    std::atomic<float> listenerGain_{1.0f};
    std::atomic<bool> listenerMuted_{false};
    std::atomic<std::uint32_t> listenerOutputRate_{kWaveSampleRate};

    std::array<std::uint8_t, 16> sid_{};
    std::uint64_t nextNonce_ = 1;
    std::uint64_t lastRemoteNonce_ = 0;
    bool hasLastRemoteNonce_ = false;
    struct AcceptedOfferRecord {
        std::array<std::uint8_t, 16> sid{};
        std::uint64_t nonce = 0;
    };
    static constexpr std::size_t kMaxAcceptedOfferHistory = 32;
    std::deque<AcceptedOfferRecord> acceptedOfferHistory_;
    WaveSdpSignal lastLocalSignal_{};
    bool hasLastLocalSignal_ = false;

    WavePcmRingBuffer hostRing_;
    WavePcmRingBuffer listenerRing_;
    WaveStereoResampler hostResampler_;
    WaveStereoResampler listenerResampler_;
    WaveJitterBuffer jitterBuffer_;

    std::vector<float> hostStaging_;
    std::vector<float> hostResampled_;
    std::vector<float> hostChunk_;
    std::vector<std::uint8_t> encodedPayload_;
    std::vector<float> listenerDecodeBuf_;
    std::vector<float> listenerResampled_;

    std::uint32_t nextSequence_ = 0;
    std::uint32_t nextTimestamp48k_ = 0;
    std::uint64_t packetsSent_ = 0;
    std::uint64_t plcFrames_ = 0;
    std::uint64_t bytesSent_ = 0;
    std::uint64_t bytesReceived_ = 0;
};

} // namespace bgmedia::wave
