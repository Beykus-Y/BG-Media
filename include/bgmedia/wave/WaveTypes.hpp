#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::wave {

inline constexpr std::uint32_t kWaveSampleRate = 48000;
inline constexpr std::uint32_t kWaveChannels = 2;
inline constexpr std::uint32_t kWaveFrameDurationMs = 20;
inline constexpr std::size_t kWaveFrameSamples =
    (kWaveSampleRate * kWaveFrameDurationMs) / 1000; // 960 frames per channel
inline constexpr std::uint32_t kWaveDefaultBitrateBps = 128000;
inline constexpr std::uint32_t kWaveMinBitrateBps = 48000;
inline constexpr std::uint32_t kWaveMaxBitrateBps = 160000;

enum class WaveRole {
    Host = 0,
    Listener = 1
};

enum class WaveSessionState {
    Idle = 0,
    Offering,
    AwaitingAnswer,
    ConnectingP2P,
    LiveP2P,
    P2PFailed,
    OfferingTelegramFallback,
    ConnectingTelegramCall,
    LiveTelegramCall,
    Failed,
    Ended
};

enum class WaveTransportRoute {
    None = 0,
    P2P,
    ExternalCall
};

struct WaveAudioFormat {
    std::uint32_t sampleRate = kWaveSampleRate;
    std::uint32_t channels = kWaveChannels;
    std::uint32_t frameDurationMs = kWaveFrameDurationMs;
    std::size_t frameSamples = kWaveFrameSamples;
};

enum WavePacketFlags : std::uint8_t {
    kWavePacketNone = 0,
    kWavePacketSilence = 1u << 0,
    kWavePacketFec = 1u << 1,
    kWavePacketDiscontinuity = 1u << 2
};

struct WaveMediaPacket {
    std::uint32_t sequence = 0;
    std::uint32_t timestamp48k = 0;
    std::uint16_t frameSamples = static_cast<std::uint16_t>(kWaveFrameSamples);
    std::uint8_t flags = kWavePacketNone;
    std::vector<std::uint8_t> payload;
};

enum class WaveErrorCode {
    None = 0,
    InvalidState,
    InvalidArgument,
    SignalingFailed,
    TransportFailed,
    CodecFailed,
    FallbackFailed,
    PublishingRevoked
};

struct WaveError {
    WaveErrorCode code = WaveErrorCode::None;
    std::string message;
};

struct WaveStats {
    std::uint64_t packetsSent = 0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;
    std::uint64_t packetsReordered = 0;
    std::uint64_t packetsDroppedLate = 0;
    std::uint64_t packetsDuplicate = 0;
    std::uint64_t plcFrames = 0;
    std::uint64_t ringOverruns = 0;
    std::uint64_t ringUnderruns = 0;
    std::uint64_t clippedSamples = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    double jitterMs = 0.0;
    double estimatedLatencyMs = 0.0;
    std::uint32_t targetBitrateBps = kWaveDefaultBitrateBps;
    std::uint32_t jitterBufferDepthMs = 40;
};

[[nodiscard]] inline const char* toString(WaveSessionState state) noexcept {
    switch (state) {
    case WaveSessionState::Idle:
        return "Idle";
    case WaveSessionState::Offering:
        return "Offering";
    case WaveSessionState::AwaitingAnswer:
        return "AwaitingAnswer";
    case WaveSessionState::ConnectingP2P:
        return "ConnectingP2P";
    case WaveSessionState::LiveP2P:
        return "LiveP2P";
    case WaveSessionState::P2PFailed:
        return "P2PFailed";
    case WaveSessionState::OfferingTelegramFallback:
        return "OfferingTelegramFallback";
    case WaveSessionState::ConnectingTelegramCall:
        return "ConnectingTelegramCall";
    case WaveSessionState::LiveTelegramCall:
        return "LiveTelegramCall";
    case WaveSessionState::Failed:
        return "Failed";
    case WaveSessionState::Ended:
        return "Ended";
    }
    return "Unknown";
}

[[nodiscard]] inline const char* toString(WaveTransportRoute route) noexcept {
    switch (route) {
    case WaveTransportRoute::None:
        return "None";
    case WaveTransportRoute::P2P:
        return "P2P";
    case WaveTransportRoute::ExternalCall:
        return "ExternalCall";
    }
    return "Unknown";
}

} // namespace bgmedia::wave
