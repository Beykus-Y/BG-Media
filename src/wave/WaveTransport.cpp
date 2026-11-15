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
    SimulatedWaveTransport* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer = peer_;
        peer_ = nullptr;
    }
    if (peer && peer != this) {
        std::lock_guard<std::mutex> peerLock(peer->mutex_);
        if (peer->peer_ == this) {
            peer->peer_ = nullptr;
        }
    }
}

void SimulatedWaveTransport::linkPeers(SimulatedWaveTransport& a,
                                       SimulatedWaveTransport& b) {
    if (&a == &b) {
        std::lock_guard<std::mutex> lock(a.mutex_);
        a.peer_ = &b;
        return;
    }
    std::scoped_lock lock(a.mutex_, b.mutex_);
    a.peer_ = &b;
    b.peer_ = &a;
}

void SimulatedWaveTransport::setCallbacks(WaveTransportCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_ = std::move(callbacks);
}

void SimulatedWaveTransport::setProfile(SimulatedNetworkProfile profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    profile_ = profile;
}

void SimulatedWaveTransport::setState(WaveTransportState next,
                                      const std::string& reason) {
    std::function<void(WaveTransportState, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == next) {
            return;
        }
        state_.store(next, std::memory_order_release);
        cb = callbacks_.onStateChanged;
    }
    if (cb) {
        cb(next, reason);
    }
}

bool SimulatedWaveTransport::startOffer() {
    setState(WaveTransportState::Gathering, "gathering simulated ICE");
    setState(WaveTransportState::Connecting, "awaiting answer");
    std::function<void(const WaveSdpSignal&)> sigCb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sigCb = callbacks_.onLocalSignalReady;
    }
    if (sigCb) {
        sigCb(makeSimulatedLocalSignal(true));
    }
    return true;
}

bool SimulatedWaveTransport::acceptOffer(const WaveSdpSignal& remoteOffer) {
    if (remoteOffer.iceUfrag.empty() || remoteOffer.icePwd.empty()) {
        setState(WaveTransportState::Failed, "invalid remote offer");
        return false;
    }
    setState(WaveTransportState::Gathering, "gathering simulated ICE");
    std::function<void(const WaveSdpSignal&)> sigCb;
    bool failOnConnect = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sigCb = callbacks_.onLocalSignalReady;
        failOnConnect = profile_.failOnConnect;
    }
    if (sigCb) {
        sigCb(makeSimulatedLocalSignal(false));
    }
    if (failOnConnect) {
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
    bool failOnConnect = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failOnConnect = profile_.failOnConnect;
    }
    if (failOnConnect) {
        setState(WaveTransportState::Failed, "simulated NAT traversal failure");
        return false;
    }
    setState(WaveTransportState::Connected, "simulated DTLS-SRTP connected");
    return true;
}

bool SimulatedWaveTransport::sendMediaPacket(const WaveMediaPacket& packet) {
    SimulatedWaveTransport* peer = nullptr;
    double sendTime = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load(std::memory_order_relaxed) !=
                WaveTransportState::Connected ||
            !peer_) {
            return false;
        }
        peer = peer_;
        sendTime = lastTimeSeconds_;
    }
    return peer->deliverInbound(packet, sendTime);
}

bool SimulatedWaveTransport::deliverInbound(WaveMediaPacket packet,
                                            double sendTimeSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.load(std::memory_order_relaxed) !=
        WaveTransportState::Connected) {
        return false;
    }
    if (profile_.lossPercent > 0.0) {
        const double r = nextUniform01(rngState_) * 100.0;
        if (r < profile_.lossPercent) {
            return true;
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
    return true;
}

void SimulatedWaveTransport::poll(double currentTimeSeconds) {
    std::vector<WaveMediaPacket> ready;
    std::function<void(WaveMediaPacket, double)> onMedia;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTimeSeconds_ = currentTimeSeconds;
        if (inboundQueue_.empty()) {
            return;
        }
        std::vector<PendingPacket> remaining;
        remaining.reserve(inboundQueue_.size());
        for (auto& item : inboundQueue_) {
            if (item.deliverAtSeconds <= currentTimeSeconds + 1e-9) {
                ready.push_back(std::move(item.packet));
            } else {
                remaining.push_back(std::move(item));
            }
        }
        inboundQueue_.swap(remaining);
        onMedia = callbacks_.onMediaPacket;
    }
    if (onMedia) {
        for (auto& pkt : ready) {
            onMedia(std::move(pkt), currentTimeSeconds);
        }
    }
}

void SimulatedWaveTransport::close() {
    std::function<void(WaveTransportState, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inboundQueue_.clear();
        if (state_.load(std::memory_order_relaxed) ==
            WaveTransportState::Closed) {
            return;
        }
        state_.store(WaveTransportState::Closed, std::memory_order_release);
        cb = callbacks_.onStateChanged;
    }
    if (cb) {
        cb(WaveTransportState::Closed, "closed");
    }
}

} // namespace bgmedia::wave
