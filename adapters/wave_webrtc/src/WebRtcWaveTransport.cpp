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

inline constexpr std::size_t kMaxCollectedCandidates = 256;

bool isIpv6Address(std::string_view addr) {
    return addr.find(':') != std::string_view::npos;
}

bool isLoopbackOrLinkLocal(std::string_view addr) {
    return addr == "127.0.0.1" || addr == "::1" ||
           addr.rfind("169.254.", 0) == 0 || addr.rfind("fe80:", 0) == 0;
}

bool parseCandidateLine(std::string_view candLine,
                        std::string_view iceUfrag,
                        std::string_view icePwd,
                        WaveIceCandidate& outCandidate) {
    std::string line(candLine);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.rfind("a=", 0) == 0) {
        line.erase(0, 2);
    }
    if (line.rfind("candidate:", 0) != 0) {
        return false;
    }
    WaveSdpSignal tmp;
    std::ostringstream oss;
    oss << "a=ice-ufrag:" << (iceUfrag.empty() ? "x" : iceUfrag)
        << "\r\na=ice-pwd:" << (icePwd.empty() ? "x" : icePwd)
        << "\r\na=" << line << "\r\n";
    if (!parseSdpToWaveSignal(oss.str(), tmp, nullptr) ||
        tmp.candidates.empty()) {
        return false;
    }
    outCandidate = std::move(tmp.candidates.front());
    return true;
}

bool extractSdpAndCandidates(std::string_view sdp,
                             WaveSdpSignal& outBaseSignal,
                             std::vector<WaveIceCandidate>& outCandidates) {
    std::string baseSdp;
    baseSdp.reserve(sdp.size());
    std::vector<std::string_view> candLines;
    std::size_t pos = 0;
    while (pos < sdp.size()) {
        const std::size_t nl = sdp.find('\n', pos);
        const std::size_t end =
            (nl == std::string_view::npos) ? sdp.size() : nl + 1;
        const std::string_view line = sdp.substr(pos, end - pos);
        pos = end;
        if (line.rfind("a=candidate:", 0) == 0) {
            candLines.push_back(line);
        } else {
            baseSdp.append(line);
        }
    }
    if (!parseSdpToWaveSignal(baseSdp, outBaseSignal, nullptr)) {
        return false;
    }
    outCandidates.clear();
    outCandidates.reserve(candLines.size());
    for (std::string_view cl : candLines) {
        WaveIceCandidate c;
        if (parseCandidateLine(cl, outBaseSignal.iceUfrag,
                               outBaseSignal.icePwd, c)) {
            outCandidates.push_back(std::move(c));
        }
    }
    return true;
}

int candidateTypeRank(WaveIceCandidateType t) {
    switch (t) {
    case WaveIceCandidateType::Srflx: return 3;
    case WaveIceCandidateType::Prflx: return 2;
    case WaveIceCandidateType::Relay: return 1;
    case WaveIceCandidateType::Host: return 0;
    }
    return 0;
}

bool sameCandidateEndpoint(const WaveIceCandidate& a,
                           const WaveIceCandidate& b) {
    return a.protocol == b.protocol && a.type == b.type &&
           a.port == b.port && a.address == b.address;
}

bool fitsWireBudget(const WaveSdpSignal& sig, std::size_t maxBinaryBytes) {
    WaveSignalMessage probe;
    probe.version = kWaveSignalVersion;
    probe.type = WaveSignalType::Offer;
    probe.sid.fill(0x5Au);
    probe.iat = 1700000000;
    probe.exp = 1700000120;
    probe.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp;
    probe.nonce = 1;
    probe.signal = sig;
    std::vector<std::uint8_t> bin;
    if (!encodeWaveSignalBinary(probe, bin, nullptr) ||
        bin.size() > maxBinaryBytes) {
        return false;
    }
    std::string uriErr;
    const std::string uri = encodeWaveHttpsFragmentUri(
        probe, "https://beygram.beykus.fun/", &uriErr);
    return !uri.empty() && uri.size() <= kWaveMaxUriLength;
}

} // namespace

bool selectCompactIceCandidates(
    const WaveSdpSignal& baseSignal,
    const std::vector<WaveIceCandidate>& allCandidates,
    WaveSdpSignal& outSignal,
    std::size_t maxCandidates,
    std::size_t maxBinaryBytes) {
    outSignal = baseSignal;
    outSignal.candidates.clear();
    if (maxCandidates == 0) {
        return false;
    }

    // 1. Deduplicate incoming candidates.
    std::vector<WaveIceCandidate> unique;
    unique.reserve(allCandidates.size());
    for (const auto& c : allCandidates) {
        auto it = std::find_if(unique.begin(), unique.end(),
                               [&](const WaveIceCandidate& u) {
                                   return sameCandidateEndpoint(u, c);
                               });
        if (it == unique.end()) {
            unique.push_back(c);
        } else if (c.priority > it->priority) {
            *it = c;
        }
    }
    if (unique.empty()) {
        return false;
    }

    // 2. Sort deterministically: reachability type (srflx/prflx/relay > host),
    // UDP > TCP, non-loopback > loopback, priority descending.
    std::stable_sort(unique.begin(), unique.end(),
                     [](const WaveIceCandidate& a, const WaveIceCandidate& b) {
                         const int ra = candidateTypeRank(a.type);
                         const int rb = candidateTypeRank(b.type);
                         if (ra != rb) return ra > rb;
                         const bool udpA = (a.protocol == WaveIceProtocol::Udp);
                         const bool udpB = (b.protocol == WaveIceProtocol::Udp);
                         if (udpA != udpB) return udpA > udpB;
                         const bool lbA = isLoopbackOrLinkLocal(a.address);
                         const bool lbB = isLoopbackOrLinkLocal(b.address);
                         if (lbA != lbB) return lbA < lbB;
                         return a.priority > b.priority;
                     });

    // 3. Pick diversity anchors first (best Srflx IPv4/IPv6, Relay IPv4/IPv6,
    // Host IPv4/IPv6), then fill remaining slots in priority order.
    std::vector<bool> picked(unique.size(), false);
    std::vector<WaveIceCandidate> ordered;
    ordered.reserve(std::min(unique.size(), maxCandidates));

    auto pickFirstMatching = [&](WaveIceCandidateType wantType, bool wantIpv6) {
        for (std::size_t i = 0; i < unique.size(); ++i) {
            if (!picked[i] && unique[i].type == wantType &&
                isIpv6Address(unique[i].address) == wantIpv6) {
                picked[i] = true;
                ordered.push_back(unique[i]);
                return;
            }
        }
    };

    pickFirstMatching(WaveIceCandidateType::Srflx, false);
    pickFirstMatching(WaveIceCandidateType::Srflx, true);
    pickFirstMatching(WaveIceCandidateType::Relay, false);
    pickFirstMatching(WaveIceCandidateType::Relay, true);
    pickFirstMatching(WaveIceCandidateType::Host, false);
    pickFirstMatching(WaveIceCandidateType::Host, true);

    for (std::size_t i = 0; i < unique.size() && ordered.size() < maxCandidates;
         ++i) {
        if (!picked[i]) {
            picked[i] = true;
            ordered.push_back(unique[i]);
        }
    }
    if (ordered.size() > maxCandidates) {
        ordered.resize(maxCandidates);
    }

    // 4. Enforce wire binary/URI byte budget incrementally.
    for (const auto& cand : ordered) {
        outSignal.candidates.push_back(cand);
        if (!fitsWireBudget(outSignal, maxBinaryBytes)) {
            outSignal.candidates.pop_back();
            break;
        }
    }
    return !outSignal.candidates.empty();
}

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
            seqUnwrapper.reset();
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
            if (self->gatheredCandidates.size() < kMaxCollectedCandidates) {
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
                    seq32 = self->seqUnwrapper.unwrap(rtp->seqNumber());
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
        const std::string rawSdp = localDesc->generateSdp();
        WaveSdpSignal baseSig;
        std::vector<WaveIceCandidate> allCandidates;
        if (!extractSdpAndCandidates(rawSdp, baseSig, allCandidates)) {
            return false;
        }
        for (const auto& c : gatheredCandidates) {
            WaveIceCandidate parsedCand;
            if (parseCandidateLine(c.candidate(), baseSig.iceUfrag,
                                   baseSig.icePwd, parsedCand)) {
                allCandidates.push_back(std::move(parsedCand));
            }
        }
        if (!config.enableIpv6) {
            allCandidates.erase(
                std::remove_if(allCandidates.begin(), allCandidates.end(),
                               [](const WaveIceCandidate& c) {
                                   return isIpv6Address(c.address);
                               }),
                allCandidates.end());
        }
        WaveSdpSignal sig;
        if (!selectCompactIceCandidates(baseSig, allCandidates, sig,
                                        kWaveMaxCandidates,
                                        kWaveMaxBinaryPayloadBytes)) {
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
            seqUnwrapper.reset();
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
    RtpSequenceUnwrapper seqUnwrapper;
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
