#pragma once

#include "bgmedia/wave/WaveSignaling.hpp"
#include "bgmedia/wave/WaveTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bgmedia::wave {

enum class WaveTransportState {
    Idle = 0,
    Gathering,
    Connecting,
    Connected,
    Failed,
    Closed
};

struct WaveTransportCallbacks {
    std::function<void(WaveTransportState state, const std::string& reason)>
        onStateChanged;
    std::function<void(const WaveSdpSignal& localSignal)> onLocalSignalReady;
    std::function<void(WaveMediaPacket packet, double arrivalTimeSeconds)>
        onMediaPacket;
};

// Abstract P2P media transport (e.g. WebRTC DTLS-SRTP via libdatachannel or
// deterministic loopback/network simulator for tests).
class IWaveTransport {
public:
    virtual ~IWaveTransport() = default;

    virtual void setCallbacks(WaveTransportCallbacks callbacks) = 0;

    // Host side: gather ICE candidates and emit an Offer via onLocalSignalReady.
    virtual bool startOffer() = 0;
    // Listener side: apply remote Offer, gather ICE candidates, emit Answer.
    virtual bool acceptOffer(const WaveSdpSignal& remoteOffer) = 0;
    // Host side: apply remote Answer and begin DTLS/ICE connection.
    virtual bool applyAnswer(const WaveSdpSignal& remoteAnswer) = 0;

    virtual bool sendMediaPacket(const WaveMediaPacket& packet) = 0;
    virtual void close() = 0;

    // Optional poll hook for single-threaded / deterministic simulations.
    virtual void poll(double currentTimeSeconds) = 0;

    [[nodiscard]] virtual WaveTransportState state() const noexcept = 0;
};

// Configurable deterministic network simulator for Spike measurements,
// loopback verification, and loss/jitter/reorder unit tests.
struct SimulatedNetworkProfile {
    double baseLatencyMs = 20.0;
    double jitterMs = 0.0;
    double lossPercent = 0.0;
    double reorderPercent = 0.0;
    bool failOnConnect = false;
    std::uint32_t seed = 0x57415645u;
};

class SimulatedWaveTransport final : public IWaveTransport {
public:
    explicit SimulatedWaveTransport(SimulatedNetworkProfile profile = {});
    ~SimulatedWaveTransport() override;

    // Connects two SimulatedWaveTransport instances as peers.
    static void linkPeers(SimulatedWaveTransport& a,
                          SimulatedWaveTransport& b);

    void setCallbacks(WaveTransportCallbacks callbacks) override;
    bool startOffer() override;
    bool acceptOffer(const WaveSdpSignal& remoteOffer) override;
    bool applyAnswer(const WaveSdpSignal& remoteAnswer) override;
    bool sendMediaPacket(const WaveMediaPacket& packet) override;
    void close() override;
    void poll(double currentTimeSeconds) override;
    [[nodiscard]] WaveTransportState state() const noexcept override {
        return state_;
    }

    void setProfile(SimulatedNetworkProfile profile);

private:
    struct PendingPacket {
        double deliverAtSeconds = 0.0;
        WaveMediaPacket packet;
    };

    void deliverInbound(WaveMediaPacket packet, double sendTimeSeconds);
    void setState(WaveTransportState next, const std::string& reason = {});

    SimulatedNetworkProfile profile_;
    WaveTransportCallbacks callbacks_;
    WaveTransportState state_ = WaveTransportState::Idle;
    SimulatedWaveTransport* peer_ = nullptr;
    double lastTimeSeconds_ = 0.0;
    std::uint64_t rngState_ = 0;
    std::vector<PendingPacket> inboundQueue_;
};

} // namespace bgmedia::wave
