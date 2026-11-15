#pragma once

#include "bgmedia/wave/WaveTransport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bgmedia::wave {

struct WebRtcTransportConfig {
    WaveRole role = WaveRole::Host;
    // Public STUN servers used for NAT traversal. libdatachannel's iceServers
    // parser also accepts TURN URIs (e.g. "turn:user:pass@host:port" or
    // "turns:user:pass@host:port?transport=tcp") here or in `turnServers` /
    // `iceServers`.
    std::vector<std::string> stunServers = {
        "stun:stun.l.google.com:19302",
        "stun:stun1.l.google.com:19302",
        "stun:stun.cloudflare.com:3478",
    };
    // Optional TURN relay server URIs (e.g. "turn:user:pass@host:port").
    std::vector<std::string> turnServers;
    // Optional additional ICE server URIs (STUN or TURN) passed directly to
    // libdatachannel.
    std::vector<std::string> iceServers;
    std::uint16_t portRangeBegin = 0;
    std::uint16_t portRangeEnd = 0;
    bool enableIceTcp = false;
    bool enableIpv6 = true;
    int gatheringTimeoutMs = 5000;
    int connectTimeoutMs = 15000;
    // Grace period before a transient ICE/PeerConnection Disconnected state
    // transitions to Failed.
    int disconnectGraceTimeoutMs = 5000;
    // Optional callback invoked when RTCP receiver reports indicate packet loss
    // or RTT changes, allowing the pipeline/codec to adapt bitrate.
    std::function<void(std::uint32_t suggestedBitrateBps)> onBitrateFeedback;
};

// Real WebRTC P2P media transport backed by libdatachannel (ICE/STUN + DTLS-SRTP).
class WebRtcWaveTransport final : public IWaveTransport {
public:
    explicit WebRtcWaveTransport(WebRtcTransportConfig config = {});
    ~WebRtcWaveTransport() override;

    WebRtcWaveTransport(const WebRtcWaveTransport&) = delete;
    WebRtcWaveTransport& operator=(const WebRtcWaveTransport&) = delete;

    void setCallbacks(WaveTransportCallbacks callbacks) override;
    bool startOffer() override;
    bool acceptOffer(const WaveSdpSignal& remoteOffer) override;
    bool applyAnswer(const WaveSdpSignal& remoteAnswer) override;
    bool sendMediaPacket(const WaveMediaPacket& packet) override;
    void close() override;
    void poll(double currentTimeSeconds) override;
    [[nodiscard]] WaveTransportState state() const noexcept override;

    [[nodiscard]] std::string selectedCandidatePairSummary() const;
    [[nodiscard]] bool remoteFingerprintVerified() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// RFC 3550 16-bit -> 32-bit RTP sequence unwrapper resilient to out-of-order
// delayed packets across 65535 -> 0 wraparound.
struct RtpSequenceUnwrapper {
    bool initialized = false;
    std::uint16_t maxSeq16 = 0;
    std::uint32_t cycles = 0;

    void reset() noexcept {
        initialized = false;
        maxSeq16 = 0;
        cycles = 0;
    }

    [[nodiscard]] std::uint32_t unwrap(std::uint16_t seq16) noexcept {
        if (!initialized) {
            initialized = true;
            maxSeq16 = seq16;
            cycles = 0;
            return seq16;
        }
        const std::int16_t delta = static_cast<std::int16_t>(seq16 - maxSeq16);
        std::uint32_t packetCycles = cycles;
        if (delta >= 0) {
            if (seq16 < maxSeq16) {
                cycles += 0x10000u;
                packetCycles = cycles;
            }
            maxSeq16 = seq16;
        } else if (seq16 > maxSeq16) {
            // Backward wraparound (e.g. a delayed packet from before sequence 0
            // when cycles == 0). Subtracting 0x10000u modulo 2^32 yields a
            // negative signed 32-bit delta relative to maxSeq16 so the jitter
            // buffer treats it as late without jumping cycles forward.
            packetCycles = cycles - 0x10000u;
        }
        return packetCycles + seq16;
    }
};

// Parsed fields and validated payload slice for an incoming RFC 3550 RTP packet.
struct ParsedRtpPacket {
    std::uint8_t payloadType = 0;
    bool marker = false;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    std::size_t payloadOffset = 0;
    std::size_t payloadSize = 0;
};

// Validates RTP v2 header bounds, CSRC list, optional header extension, and
// optional RFC 3550 padding before exposing the payload slice.
[[nodiscard]] bool parseRtpPacket(std::span<const std::uint8_t> data,
                                  ParsedRtpPacket& outPacket) noexcept;

// Deterministically selects up to `maxCandidates` ICE candidates preserving
// IPv4/IPv6 and srflx/relay/host reachability while fitting within
// `maxBinaryBytes` (and `kWaveMaxUriLength`). Returns false if no valid signal
// can fit within the compact wire budget.
[[nodiscard]] bool selectCompactIceCandidates(
    const WaveSdpSignal& baseSignal,
    const std::vector<WaveIceCandidate>& allCandidates,
    WaveSdpSignal& outSignal,
    std::size_t maxCandidates = kWaveMaxCandidates,
    std::size_t maxBinaryBytes = kWaveMaxBinaryPayloadBytes);

} // namespace bgmedia::wave
