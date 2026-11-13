#include "bgmedia/wave/WaveTransport.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::wave {
namespace {

double nextUniform01(std::uint64_t& state) noexcept {
    // xorshift64*
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const std::uint64_t val = state * 2685821657736338717ULL;
    return static_cast<double>((val >> 11) & 0x1FFFFFFFFFFFFFULL) /
           static_cast<double>(1ULL << 53);
}

WaveSdpSignal makeSimulatedLocalSignal(bool isOffer) {
    WaveSdpSignal sig;
    sig.iceUfrag = isOffer ? "hostufrag" : "listufrag";
    sig.icePwd = isOffer ? "hostpassword1234567890" : "listpassword1234567890";
    sig.dtlsFingerprintAlg = "sha-256";
    sig.dtlsFingerprint =
        "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:"
        "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF";
    sig.setupRole =
        isOffer ? WaveDtlsSetupRole::Actpass : WaveDtlsSetupRole::Active;
    WaveIceCandidate c;
    c.foundation = "1";
    c.component = 1;
    c.protocol = WaveIceProtocol::Udp;
    c.priority = 2122260223u;
    c.address = "192.0.2.1";
    c.port = isOffer ? 50000 : 50001;
    c.type = WaveIceCandidateType::Host;
    sig.candidates.push_back(std::move(c));
    return sig;
}

} // namespace

SimulatedWaveTransport::SimulatedWaveTransport(SimulatedNetworkProfile profile)
    : profile_(profile),
      rngState_(profile.seed ? profile.seed : 0x57415645ULL) {}

SimulatedWaveTransport::~SimulatedWaveTransport() {
    if (peer_ && peer_->peer_ == this) {
        peer_->peer_ = nullptr;
    }
}

void SimulatedWaveTransport::linkPeers(SimulatedWaveTransport& a,
                                       SimulatedWaveTransport& b) {
    a.peer_ = &b;
    b.peer_ = &a;
}

void SimulatedWaveTransport::setCallbacks(WaveTransportCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void SimulatedWaveTransport::setProfile(SimulatedNetworkProfile profile) {
    profile_ = profile;
}

void SimulatedWaveTransport::setState(WaveTransportState next,
                                      const std::string& reason) {
    if (state_ == next) {
        return;
    }
    state_ = next;
    if (callbacks_.onStateChanged) {
        callbacks_.onStateChanged(next, reason);
    }
}

bool SimulatedWaveTransport::startOffer() {
    setState(WaveTransportState::Gathering, "gathering simulated ICE");
    setState(WaveTransportState::Connecting, "awaiting answer");
    if (callbacks_.onLocalSignalReady) {
        callbacks_.onLocalSignalReady(makeSimulatedLocalSignal(true));
    }
    return true;
}

bool SimulatedWaveTransport::acceptOffer(const WaveSdpSignal& remoteOffer) {
    if (remoteOffer.iceUfrag.empty() || remoteOffer.icePwd.empty()) {
        setState(WaveTransportState::Failed, "invalid remote offer");
        return false;
    }
    setState(WaveTransportState::Gathering, "gathering simulated ICE");
    if (callbacks_.onLocalSignalReady) {
        callbacks_.onLocalSignalReady(makeSimulatedLocalSignal(false));
    }
    if (profile_.failOnConnect) {
        setState(WaveTransportState::Failed, "simulated NAT traversal failure");
        return false;
    }
    setState(WaveTransportState::Connected, "simulated DTLS-SRTP connected");
    return true;
}

bool SimulatedWaveTransport::applyAnswer(const WaveSdpSignal& remoteAnswer) {
    if (remoteAnswer.iceUfrag.empty() || remoteAnswer.icePwd.empty()) {
        setState(WaveTransportState::Failed, "invalid remote answer");
        return false;
    }
    if (profile_.failOnConnect) {
        setState(WaveTransportState::Failed, "simulated NAT traversal failure");
        return false;
    }
    setState(WaveTransportState::Connected, "simulated DTLS-SRTP connected");
    return true;
}

bool SimulatedWaveTransport::sendMediaPacket(const WaveMediaPacket& packet) {
    if (state_ != WaveTransportState::Connected || !peer_ ||
        peer_->state_ != WaveTransportState::Connected) {
        return false;
    }
    peer_->deliverInbound(packet, lastTimeSeconds_);
    return true;
}

void SimulatedWaveTransport::deliverInbound(WaveMediaPacket packet,
                                            double sendTimeSeconds) {
    if (profile_.lossPercent > 0.0) {
        const double r = nextUniform01(rngState_) * 100.0;
        if (r < profile_.lossPercent) {
            return;
        }
    }

    double delayMs = std::max(0.0, profile_.baseLatencyMs);
    if (profile_.jitterMs > 0.0) {
        const double u = nextUniform01(rngState_) * 2.0 - 1.0; // [-1, 1]
        delayMs = std::max(0.0, delayMs + u * profile_.jitterMs);
    }
    if (profile_.reorderPercent > 0.0) {
        const double r = nextUniform01(rngState_) * 100.0;
        if (r < profile_.reorderPercent) {
            delayMs += 25.0; // push behind next packet
        }
    }

    PendingPacket pending;
    pending.deliverAtSeconds = sendTimeSeconds + (delayMs / 1000.0);
    pending.packet = std::move(packet);
    inboundQueue_.push_back(std::move(pending));
}

void SimulatedWaveTransport::poll(double currentTimeSeconds) {
    lastTimeSeconds_ = currentTimeSeconds;
    if (inboundQueue_.empty()) {
        return;
    }
    std::vector<PendingPacket> remaining;
    remaining.reserve(inboundQueue_.size());
    for (auto& item : inboundQueue_) {
        if (item.deliverAtSeconds <= currentTimeSeconds + 1e-9) {
            if (callbacks_.onMediaPacket) {
                callbacks_.onMediaPacket(std::move(item.packet),
                                         currentTimeSeconds);
            }
        } else {
            remaining.push_back(std::move(item));
        }
    }
    inboundQueue_.swap(remaining);
}

void SimulatedWaveTransport::close() {
    inboundQueue_.clear();
    setState(WaveTransportState::Closed, "closed");
}

} // namespace bgmedia::wave
