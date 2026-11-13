#include "bgmedia/wave/WebRtcWaveTransport.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>

namespace bgmedia::wave {

namespace {

std::string normalizeSha256Hex(std::string_view input) {
    std::string out;
    out.reserve(64);
    for (unsigned char ch : input) {
        if (ch == ':' || std::isspace(ch)) {
            continue;
        }
        if (!std::isxdigit(ch)) {
            return {};
        }
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out.size() == 64 ? out : std::string{};
}

std::uint32_t generateRandomSsrc() {
    std::random_device rd;
    std::uint32_t v = static_cast<std::uint32_t>(rd());
    return v == 0 ? 0x42475731u : v;
}

std::string trimSdpCandidatesToLimit(std::string_view sdp,
                                     std::size_t maxCandidates) {
    std::string out;
    out.reserve(sdp.size());
    std::size_t candCount = 0;
    std::size_t pos = 0;
    while (pos < sdp.size()) {
        const std::size_t nl = sdp.find('\n', pos);
        const std::size_t end = (nl == std::string_view::npos) ? sdp.size() : nl + 1;
        const std::string_view line = sdp.substr(pos, end - pos);
        pos = end;
        if (line.rfind("a=candidate:", 0) == 0) {
            if (candCount >= maxCandidates) {
                continue;
            }
            ++candCount;
        }
        out.append(line);
    }
    return out;
}

bool appendCandidateLineToSignal(std::string_view candLine,
                                 WaveSdpSignal& signal) {
    std::string line(candLine);
    if (line.rfind("a=", 0) == 0) {
        line.erase(0, 2);
    }
    if (line.rfind("candidate:", 0) != 0) {
        return false;
    }
    if (signal.candidates.size() >= kWaveMaxCandidates) {
        return false;
    }
    WaveSdpSignal tmp;
    std::ostringstream oss;
    oss << "a=ice-ufrag:" << (signal.iceUfrag.empty() ? "x" : signal.iceUfrag)
        << "\r\na=ice-pwd:" << (signal.icePwd.empty() ? "x" : signal.icePwd)
        << "\r\na=" << line << "\r\n";
    if (!parseSdpToWaveSignal(oss.str(), tmp, nullptr) ||
        tmp.candidates.empty()) {
        return false;
    }
    signal.candidates.push_back(std::move(tmp.candidates.front()));
    return true;
}

} // namespace

struct WebRtcWaveTransport::Impl
    : public std::enable_shared_from_this<WebRtcWaveTransport::Impl> {
    explicit Impl(WebRtcTransportConfig cfg) : config(std::move(cfg)) {}

    ~Impl() { shutdown(false); }

    void setCallbacks(WaveTransportCallbacks cb) {
        std::lock_guard<std::mutex> lock(mutex);
        callbacks = std::move(cb);
    }

    void setState(std::uint64_t gen, WaveTransportState next,
                  const std::string& reason) {
        std::function<void(WaveTransportState, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (gen != generation.load(std::memory_order_acquire)) {
                return;
            }
            if (state.load(std::memory_order_acquire) == next) {
                return;
            }
            state.store(next, std::memory_order_release);
            if (next == WaveTransportState::Connected ||
                next == WaveTransportState::Failed ||
                next == WaveTransportState::Closed) {
                connectTimerActive = false;
            }
            cb = callbacks.onStateChanged;
        }
        if (cb && gen == generation.load(std::memory_order_acquire)) {
            cb(next, reason);
        }
    }

    void shutdown(bool emitClosedState) {
        std::uint64_t newGen = 0;
        std::shared_ptr<rtc::PeerConnection> oldPc;
        std::shared_ptr<rtc::Track> oldTrack;
        std::function<void(WaveTransportState, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            newGen = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            oldPc = std::move(pc);
            oldTrack = std::move(track);
            packetizer.reset();
            srReporter.reset();
            rtcpSession.reset();
            rtpConfig.reset();
            gatheringComplete = false;
            gatheredCandidates.clear();
            expectedRemoteFingerprint.clear();
            remoteFingerprintVerified.store(false, std::memory_order_release);
            connectTimerActive = false;
            hasFirstSeq = false;
            seqCycles = 0;
            lastSeq16 = 0;
            if (emitClosedState &&
                state.load(std::memory_order_acquire) !=
                    WaveTransportState::Closed) {
                state.store(WaveTransportState::Closed,
                            std::memory_order_release);
                cb = callbacks.onStateChanged;
            } else if (!emitClosedState) {
                state.store(WaveTransportState::Closed,
                            std::memory_order_release);
            }
            gatherCv.notify_all();
        }
        if (oldTrack) {
            oldTrack->resetCallbacks();
            oldTrack->close();
        }
        if (oldPc) {
            oldPc->resetCallbacks();
            oldPc->close();
        }
        if (cb && newGen == generation.load(std::memory_order_acquire)) {
            cb(WaveTransportState::Closed, "transport closed");
        }
    }

    rtc::Configuration buildRtcConfig() const {
        rtc::Configuration rtcCfg;
        for (const auto& s : config.stunServers) {
            if (!s.empty()) {
                rtcCfg.iceServers.emplace_back(s);
            }
        }
        rtcCfg.enableIceTcp = config.enableIceTcp;
        rtcCfg.disableAutoNegotiation = true;
        if (config.portRangeBegin > 0 &&
            config.portRangeEnd >= config.portRangeBegin) {
            rtcCfg.portRangeBegin = config.portRangeBegin;
            rtcCfg.portRangeEnd = config.portRangeEnd;
        }
        rtcCfg.disableFingerprintVerification = false;
        return rtcCfg;
    }

    void attachPeerCallbacks(const std::shared_ptr<rtc::PeerConnection>& peer,
                             std::uint64_t gen) {
        std::weak_ptr<Impl> weak = shared_from_this();

        peer->onGatheringStateChange(
            [weak, gen](rtc::PeerConnection::GatheringState gs) {
                auto self = weak.lock();
                if (!self) return;
                if (gs == rtc::PeerConnection::GatheringState::Complete) {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (gen == self->generation.load(std::memory_order_acquire)) {
                        self->gatheringComplete = true;
                        self->gatherCv.notify_all();
                    }
                }
            });

        peer->onLocalCandidate([weak, gen](rtc::Candidate cand) {
            auto self = weak.lock();
            if (!self) return;
            std::lock_guard<std::mutex> lock(self->mutex);
            if (gen != self->generation.load(std::memory_order_acquire)) {
                return;
            }
            if (!self->config.enableIpv6 &&
                cand.family() == rtc::Candidate::Family::Ipv6) {
                return;
            }
            if (self->gatheredCandidates.size() < kWaveMaxCandidates) {
                self->gatheredCandidates.push_back(std::move(cand));
            }
        });

        peer->onIceStateChange([weak, gen](rtc::PeerConnection::IceState ist) {
            auto self = weak.lock();
            if (!self) return;
            if (ist == rtc::PeerConnection::IceState::Failed) {
                self->setState(gen, WaveTransportState::Failed,
                               "ICE connectivity check failed");
            } else if (ist == rtc::PeerConnection::IceState::Disconnected) {
                self->setState(gen, WaveTransportState::Failed,
                               "ICE connection lost");
            }
        });

        peer->onStateChange([weak, gen](rtc::PeerConnection::State st) {
            auto self = weak.lock();
            if (!self) return;
            switch (st) {
            case rtc::PeerConnection::State::Connecting:
                self->setState(gen, WaveTransportState::Connecting,
                               "DTLS/ICE connecting");
                break;
            case rtc::PeerConnection::State::Connected: {
                std::shared_ptr<rtc::PeerConnection> peerRef;
                std::string expectedFp;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (gen !=
                        self->generation.load(std::memory_order_acquire)) {
                        return;
                    }
                    peerRef = self->pc;
                    expectedFp = self->expectedRemoteFingerprint;
                }
                if (!peerRef) return;

                const rtc::CertificateFingerprint remoteFp =
                    peerRef->remoteFingerprint();
                const std::string actualHex =
                    normalizeSha256Hex(remoteFp.value);
                const bool fpOk =
                    remoteFp.isValid() &&
                    remoteFp.algorithm ==
                        rtc::CertificateFingerprint::Algorithm::Sha256 &&
                    !expectedFp.empty() && actualHex == expectedFp;

                if (!fpOk) {
                    self->remoteFingerprintVerified.store(
                        false, std::memory_order_release);
                    peerRef->close();
                    self->setState(gen, WaveTransportState::Failed,
                                   "DTLS fingerprint verification failed");
                    return;
                }

                self->remoteFingerprintVerified.store(
                    true, std::memory_order_release);

                rtc::Candidate localCand;
                rtc::Candidate remoteCand;
                std::string summary;
                if (peerRef->getSelectedCandidatePair(&localCand,
                                                      &remoteCand)) {
                    summary = localCand.candidate() + " <-> " +
                              remoteCand.candidate();
                }
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (gen ==
                        self->generation.load(std::memory_order_acquire)) {
                        self->selectedCandidateSummary = std::move(summary);
                    }
                }
                self->setState(gen, WaveTransportState::Connected,
                               "DTLS-SRTP connected");
                break;
            }
            case rtc::PeerConnection::State::Disconnected:
                self->setState(gen, WaveTransportState::Failed,
                               "peer connection disconnected");
                break;
            case rtc::PeerConnection::State::Failed:
                self->setState(gen, WaveTransportState::Failed,
                               "peer connection failed");
                break;
            case rtc::PeerConnection::State::Closed:
                self->setState(gen, WaveTransportState::Closed,
                               "peer connection closed");
                break;
            default:
                break;
            }
        });

        peer->onTrack([weak, gen](std::shared_ptr<rtc::Track> tr) {
            auto self = weak.lock();
            if (!self || !tr) return;
            auto session = std::make_shared<rtc::RtcpReceivingSession>();
            tr->setMediaHandler(session);
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (gen != self->generation.load(std::memory_order_acquire)) {
                    return;
                }
                self->rtcpSession = session;
                self->track = tr;
            }
            self->attachTrackReceiveCallback(tr, gen);
        });
    }

    void attachTrackReceiveCallback(const std::shared_ptr<rtc::Track>& tr,
                                    std::uint64_t gen) {
        std::weak_ptr<Impl> weak = shared_from_this();
        tr->onMessage(
            [weak, gen](rtc::binary data) {
                auto self = weak.lock();
                if (!self) return;
                if (gen != self->generation.load(std::memory_order_acquire)) {
                    return;
                }
                if (data.size() < sizeof(rtc::RtpHeader)) {
                    return;
                }
                const auto* rtp =
                    reinterpret_cast<const rtc::RtpHeader*>(data.data());
                if (rtp->version() != 2 || rtp->payloadType() != 111) {
                    return;
                }
                const std::size_t hdrSize =
                    rtp->getSize() + rtp->getExtensionHeaderSize();
                if (hdrSize > data.size()) {
                    return;
                }

                std::uint32_t seq32 = 0;
                std::function<void(WaveMediaPacket, double)> cb;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (gen !=
                        self->generation.load(std::memory_order_acquire)) {
                        return;
                    }
                    const std::uint16_t seq16 = rtp->seqNumber();
                    if (!self->hasFirstSeq) {
                        self->hasFirstSeq = true;
                        self->lastSeq16 = seq16;
                        seq32 = seq16;
                    } else {
                        const std::int32_t diff = static_cast<std::int16_t>(
                            seq16 - self->lastSeq16);
                        if (diff > 0 && seq16 < self->lastSeq16) {
                            self->seqCycles += 0x10000u;
                        } else if (diff < 0 && seq16 > self->lastSeq16) {
                            if (self->seqCycles >= 0x10000u) {
                                self->seqCycles -= 0x10000u;
                            }
                        }
                        if (diff >= 0) {
                            self->lastSeq16 = seq16;
                        }
                        seq32 = self->seqCycles + seq16;
                    }
                    cb = self->callbacks.onMediaPacket;
                }

                if (!cb) {
                    return;
                }
                WaveMediaPacket pkt;
                pkt.sequence = seq32;
                pkt.timestamp48k = rtp->timestamp();
                pkt.frameSamples = static_cast<std::uint16_t>(kWaveFrameSamples);
                pkt.flags = rtp->marker() ? kWavePacketDiscontinuity
                                          : kWavePacketNone;
                const auto* body =
                    reinterpret_cast<const std::uint8_t*>(data.data()) +
                    hdrSize;
                const std::size_t bodySize = data.size() - hdrSize;
                pkt.payload.assign(body, body + bodySize);

                const double arrivalSec =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                cb(std::move(pkt), arrivalSec);
            },
            nullptr);
    }

    bool waitForGatheringAndEmitSignal(std::uint64_t gen) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  std::max(100, config.gatheringTimeoutMs));
        gatherCv.wait_until(lock, deadline, [&]() {
            return gatheringComplete ||
                   gen != generation.load(std::memory_order_acquire);
        });
        if (gen != generation.load(std::memory_order_acquire) || !pc) {
            return false;
        }
        auto localDesc = pc->localDescription();
        if (!localDesc.has_value()) {
            return false;
        }
        const std::string sdpStr =
            trimSdpCandidatesToLimit(localDesc->generateSdp(), kWaveMaxCandidates);
        WaveSdpSignal sig;
        if (!parseSdpToWaveSignal(sdpStr, sig, nullptr)) {
            return false;
        }
        if (sig.candidates.empty() && !gatheredCandidates.empty()) {
            for (const auto& c : gatheredCandidates) {
                appendCandidateLineToSignal(c.candidate(), sig);
            }
        }
        if (!config.enableIpv6) {
            sig.candidates.erase(
                std::remove_if(sig.candidates.begin(), sig.candidates.end(),
                               [](const WaveIceCandidate& c) {
                                   return c.address.find(':') !=
                                          std::string::npos;
                               }),
                sig.candidates.end());
        }
        if (sig.candidates.empty()) {
            return false;
        }
        auto cb = callbacks.onLocalSignalReady;
        lock.unlock();
        if (cb && gen == generation.load(std::memory_order_acquire)) {
            cb(sig);
        }
        return true;
    }

    bool startOffer() {
        shutdown(false);
        std::uint64_t gen = 0;
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            gatheringComplete = false;
            gatheredCandidates.clear();
            expectedRemoteFingerprint.clear();
            selectedCandidateSummary.clear();
            remoteFingerprintVerified.store(false, std::memory_order_release);
            ssrc = generateRandomSsrc();
            state.store(WaveTransportState::Gathering,
                        std::memory_order_release);

            peer = std::make_shared<rtc::PeerConnection>(buildRtcConfig());
            pc = peer;
            attachPeerCallbacks(peer, gen);

            rtc::Description::Audio media("audio",
                                          rtc::Description::Direction::SendOnly);
            media.addOpusCodec(111);
            media.addSSRC(ssrc, "bgwave-audio");
            track = peer->addTrack(media);

            rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "bgwave-audio", std::uint8_t{111},
                std::uint32_t{48000});
            packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
            srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
            packetizer->addToChain(srReporter);
            track->setMediaHandler(packetizer);
        }

        setState(gen, WaveTransportState::Gathering, "gathering ICE candidates");
        peer->setLocalDescription(rtc::Description::Type::Offer);
        if (!waitForGatheringAndEmitSignal(gen)) {
            setState(gen, WaveTransportState::Failed,
                     "ICE gathering produced no usable candidates");
            return false;
        }
        return true;
    }

    bool acceptOffer(const WaveSdpSignal& remoteOffer) {
        if (remoteOffer.dtlsFingerprintAlg != "sha-256") {
            return false;
        }
        const std::string fpHex =
            normalizeSha256Hex(remoteOffer.dtlsFingerprint);
        if (fpHex.empty()) {
            return false;
        }

        shutdown(false);
        std::uint64_t gen = 0;
        std::shared_ptr<rtc::PeerConnection> peer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            gatheringComplete = false;
            gatheredCandidates.clear();
            expectedRemoteFingerprint = fpHex;
            selectedCandidateSummary.clear();
            remoteFingerprintVerified.store(false, std::memory_order_release);
            hasFirstSeq = false;
            seqCycles = 0;
            lastSeq16 = 0;
            state.store(WaveTransportState::Gathering,
                        std::memory_order_release);

            peer = std::make_shared<rtc::PeerConnection>(buildRtcConfig());
            pc = peer;
            attachPeerCallbacks(peer, gen);
        }

        setState(gen, WaveTransportState::Gathering, "gathering ICE answer");
        const std::string offerSdp = formatWaveSignalToSdp(remoteOffer, true);
        try {
            peer->setRemoteDescription(
                rtc::Description(offerSdp, rtc::Description::Type::Offer));
            peer->setLocalDescription(rtc::Description::Type::Answer);
        } catch (const std::exception& e) {
            setState(gen, WaveTransportState::Failed,
                     std::string("failed to apply remote offer: ") + e.what());
            return false;
        } catch (...) {
            setState(gen, WaveTransportState::Failed,
                     "failed to apply remote offer");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (gen == generation.load(std::memory_order_acquire)) {
                connectStartTime_ = std::chrono::steady_clock::now();
                connectTimerActive = true;
            }
        }
        if (!waitForGatheringAndEmitSignal(gen)) {
            setState(gen, WaveTransportState::Failed,
                     "ICE answer gathering produced no usable candidates");
            return false;
        }
        return true;
    }

    bool applyAnswer(const WaveSdpSignal& remoteAnswer) {
        if (remoteAnswer.dtlsFingerprintAlg != "sha-256") {
            return false;
        }
        const std::string fpHex =
            normalizeSha256Hex(remoteAnswer.dtlsFingerprint);
        if (fpHex.empty()) {
            return false;
        }

        std::shared_ptr<rtc::PeerConnection> peer;
        std::uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation.load(std::memory_order_acquire);
            if (!pc) {
                return false;
            }
            expectedRemoteFingerprint = fpHex;
            connectStartTime_ = std::chrono::steady_clock::now();
            connectTimerActive = true;
            peer = pc;
        }

        setState(gen, WaveTransportState::Connecting,
                 "starting ICE/DTLS handshake");
        const std::string answerSdp =
            formatWaveSignalToSdp(remoteAnswer, false);
        try {
            peer->setRemoteDescription(
                rtc::Description(answerSdp, rtc::Description::Type::Answer));
        } catch (...) {
            setState(gen, WaveTransportState::Failed,
                     "failed to apply remote answer");
            return false;
        }
        return true;
    }

    bool sendMediaPacket(const WaveMediaPacket& packet) {
        std::shared_ptr<rtc::Track> tr;
        std::shared_ptr<rtc::RtpPacketizationConfig> cfg;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (state.load(std::memory_order_acquire) !=
                    WaveTransportState::Connected ||
                !remoteFingerprintVerified.load(std::memory_order_acquire)) {
                return false;
            }
            tr = track;
            cfg = rtpConfig;
        }
        if (!tr || !cfg || !tr->isOpen() || packet.payload.empty()) {
            return false;
        }
        cfg->sequenceNumber = static_cast<std::uint16_t>(packet.sequence);
        cfg->timestamp = packet.timestamp48k;
        try {
            return tr->send(
                reinterpret_cast<const rtc::byte*>(packet.payload.data()),
                packet.payload.size());
        } catch (...) {
            return false;
        }
    }

    void poll(double currentTimeSeconds) {
        std::shared_ptr<rtc::PeerConnection> peer;
        std::uint64_t gen = 0;
        bool timedOut = false;
        std::function<void(std::uint32_t)> bitrateCb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation.load(std::memory_order_acquire);
            peer = pc;
            const WaveTransportState cur =
                state.load(std::memory_order_acquire);
            if (connectTimerActive &&
                (cur == WaveTransportState::Gathering ||
                 cur == WaveTransportState::Connecting)) {
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - connectStartTime_)
                        .count();
                if (elapsedMs > config.connectTimeoutMs) {
                    connectTimerActive = false;
                    timedOut = true;
                }
            }
            if (cur == WaveTransportState::Connected &&
                config.role == WaveRole::Host && config.onBitrateFeedback &&
                (lastFeedbackTime < 0.0 ||
                 currentTimeSeconds - lastFeedbackTime >= 1.0)) {
                lastFeedbackTime = currentTimeSeconds;
                bitrateCb = config.onBitrateFeedback;
            }
        }
        if (timedOut) {
            if (peer) {
                peer->close();
            }
            setState(gen, WaveTransportState::Failed,
                     "ICE/DTLS connection timeout");
            return;
        }
        if (bitrateCb && peer) {
            const auto rttOpt = peer->rtt();
            if (rttOpt.has_value()) {
                const auto ms = rttOpt->count();
                if (ms > 250) {
                    bitrateCb(96000);
                } else if (ms > 150) {
                    bitrateCb(112000);
                } else {
                    bitrateCb(kWaveDefaultBitrateBps);
                }
            }
        }
    }

    WebRtcTransportConfig config;
    mutable std::mutex mutex;
    std::condition_variable gatherCv;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<WaveTransportState> state{WaveTransportState::Idle};
    std::atomic<bool> remoteFingerprintVerified{false};
    WaveTransportCallbacks callbacks;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::OpusRtpPacketizer> packetizer;
    std::shared_ptr<rtc::RtcpSrReporter> srReporter;
    std::shared_ptr<rtc::RtcpReceivingSession> rtcpSession;
    bool gatheringComplete = false;
    std::vector<rtc::Candidate> gatheredCandidates;
    std::string expectedRemoteFingerprint;
    std::string selectedCandidateSummary;
    std::uint32_t ssrc = 0x42475731u;
    std::uint16_t lastSeq16 = 0;
    std::uint32_t seqCycles = 0;
    bool hasFirstSeq = false;
    std::chrono::steady_clock::time_point connectStartTime_{};
    bool connectTimerActive = false;
    double lastFeedbackTime = -1.0;
};

WebRtcWaveTransport::WebRtcWaveTransport(WebRtcTransportConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}

WebRtcWaveTransport::~WebRtcWaveTransport() {
    if (impl_) {
        impl_->shutdown(false);
    }
}

void WebRtcWaveTransport::setCallbacks(WaveTransportCallbacks callbacks) {
    impl_->setCallbacks(std::move(callbacks));
}

bool WebRtcWaveTransport::startOffer() { return impl_->startOffer(); }

bool WebRtcWaveTransport::acceptOffer(const WaveSdpSignal& remoteOffer) {
    return impl_->acceptOffer(remoteOffer);
}

bool WebRtcWaveTransport::applyAnswer(const WaveSdpSignal& remoteAnswer) {
    return impl_->applyAnswer(remoteAnswer);
}

bool WebRtcWaveTransport::sendMediaPacket(const WaveMediaPacket& packet) {
    return impl_->sendMediaPacket(packet);
}

void WebRtcWaveTransport::close() { impl_->shutdown(true); }

void WebRtcWaveTransport::poll(double currentTimeSeconds) {
    impl_->poll(currentTimeSeconds);
}

WaveTransportState WebRtcWaveTransport::state() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

std::string WebRtcWaveTransport::selectedCandidatePairSummary() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->selectedCandidateSummary;
}

bool WebRtcWaveTransport::remoteFingerprintVerified() const noexcept {
    return impl_->remoteFingerprintVerified.load(std::memory_order_acquire);
}

} // namespace bgmedia::wave
