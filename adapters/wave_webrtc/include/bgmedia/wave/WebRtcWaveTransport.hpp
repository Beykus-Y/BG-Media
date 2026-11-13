#pragma once

#include "bgmedia/wave/WaveTransport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::wave {

struct WebRtcTransportConfig {
    WaveRole role = WaveRole::Host;
    std::vector<std::string> stunServers = {"stun:stun.l.google.com:19302"};
    std::uint16_t portRangeBegin = 0;
    std::uint16_t portRangeEnd = 0;
    bool enableIceTcp = false;
    bool enableIpv6 = true;
    int gatheringTimeoutMs = 5000;
    int connectTimeoutMs = 15000;
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

} // namespace bgmedia::wave
