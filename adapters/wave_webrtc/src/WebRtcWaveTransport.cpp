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

bool parseRtpPacket(std::span<const std::uint8_t> data,
                    ParsedRtpPacket& outPacket) noexcept {
    constexpr std::size_t kRtpFixedHeaderBytes = 12;
    if (data.size() < kRtpFixedHeaderBytes) {
        return false;
    }

    const std::uint8_t b0 = data[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) {
        return false;
    }
    const bool hasPadding = ((b0 >> 5) & 0x01u) != 0;
    const bool hasExtension = ((b0 >> 4) & 0x01u) != 0;
    const std::size_t csrcCount = static_cast<std::size_t>(b0 & 0x0Fu);

    const std::size_t baseHdr = kRtpFixedHeaderBytes + csrcCount * 4u;
    if (baseHdr > data.size()) {
        return false;
    }

    std::size_t hdrSize = baseHdr;
    if (hasExtension) {
        if (baseHdr + 4u > data.size()) {
            return false;
        }
        const std::size_t extWords =
            (static_cast<std::size_t>(data[baseHdr + 2u]) << 8) |
            static_cast<std::size_t>(data[baseHdr + 3u]);
        const std::size_t extBytes = 4u + extWords * 4u;
        if (extBytes > data.size() - baseHdr) {
            return false;
        }
        hdrSize = baseHdr + extBytes;
    }

    std::size_t payloadEnd = data.size();
    if (hasPadding) {
        if (hdrSize >= payloadEnd) {
            return false;
        }
        const std::size_t padLen = static_cast<std::size_t>(data.back());
        if (padLen == 0 || padLen > (payloadEnd - hdrSize)) {
            return false;
        }
        payloadEnd -= padLen;
    }

    const std::uint8_t b1 = data[1];
    outPacket.marker = ((b1 >> 7) & 0x01u) != 0;
    outPacket.payloadType = static_cast<std::uint8_t>(b1 & 0x7Fu);
    outPacket.sequence = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[2]) << 8) |
        static_cast<std::uint16_t>(data[3]));
    outPacket.timestamp =
        (static_cast<std::uint32_t>(data[4]) << 24) |
        (static_cast<std::uint32_t>(data[5]) << 16) |
        (static_cast<std::uint32_t>(data[6]) << 8) |
        static_cast<std::uint32_t>(data[7]);
    outPacket.ssrc =
        (static_cast<std::uint32_t>(data[8]) << 24) |
        (static_cast<std::uint32_t>(data[9]) << 16) |
        (static_cast<std::uint32_t>(data[10]) << 8) |
        static_cast<std::uint32_t>(data[11]);
    outPacket.payloadOffset = hdrSize;
    outPacket.payloadSize = payloadEnd - hdrSize;
    return true;
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
            const WaveTransportState cur =
                state.load(std::memory_order_acquire);
            if (cur == next) {
                return;
            }
            // Keep terminal Failed/Closed sticky for this generation so a
            // subsequent PeerConnection::State::Closed after peer->close() on
            // failure cannot overwrite Failed with Closed.
            if (cur == WaveTransportState::Failed ||
                cur == WaveTransportState::Closed || failedGen == gen) {
                return;
            }
            state.store(next, std::memory_order_release);
            if (next == WaveTransportState::Failed) {
                failedGen = gen;
            }
            if (next == WaveTransportState::Connected ||
                next == WaveTransportState::Failed ||
                next == WaveTransportState::Closed) {
                connectTimerActive = false;
            }
            if (next == WaveTransportState::Failed ||
                next == WaveTransportState::Closed) {
                iceDisconnected = false;
                peerDisconnected = false;
                disconnectTimerActive = false;
            }
            cb = callbacks.onStateChanged;
        }
        if (cb && gen == generation.load(std::memory_order_acquire)) {
            cb(next, reason);
        }
    }

    void updateDisconnectedFlag(std::uint64_t gen, bool isIce,
                                bool disconnected) {
        std::lock_guard<std::mutex> lock(mutex);
        if (gen != generation.load(std::memory_order_acquire)) {
            return;
        }
        const WaveTransportState cur = state.load(std::memory_order_acquire);
        if (cur == WaveTransportState::Failed ||
            cur == WaveTransportState::Closed || failedGen == gen) {
            return;
        }
        if (isIce) {
            iceDisconnected = disconnected;
        } else {
            peerDisconnected = disconnected;
        }
        const bool anyDisconnected = iceDisconnected || peerDisconnected;
        if (anyDisconnected && !disconnectTimerActive) {
            disconnectStartTime_ = std::chrono::steady_clock::now();
            disconnectTimerActive = true;
        } else if (!anyDisconnected) {
            disconnectTimerActive = false;
        }
    }

    void closeCurrentPeer(std::uint64_t gen) {
        std::shared_ptr<rtc::PeerConnection> oldPc;
        std::shared_ptr<rtc::Track> oldTrack;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (gen == 0 || gen != generation.load(std::memory_order_acquire)) {
                return;
            }
            oldPc = std::move(pc);
            oldTrack = std::move(track);
            packetizer.reset();
            srReporter.reset();
            rtcpSession.reset();
            rtpConfig.reset();
        }
        if (oldTrack) {
            oldTrack->resetCallbacks();
            oldTrack->close();
        }
        if (oldPc) {
            oldPc->resetCallbacks();
            oldPc->close();
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
            selectedCandidateSummary.clear();
            remoteFingerprintVerified.store(false, std::memory_order_release);
            hasExpectedRemoteSsrc = false;
            expectedRemoteSsrc = 0;
            connectTimerActive = false;
            iceDisconnected = false;
            peerDisconnected = false;
            disconnectTimerActive = false;
            lastFeedbackTime = -1.0;
            failedGen = 0;
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
        auto appendIceServers = [&](const std::vector<std::string>& servers) {
            for (const auto& s : servers) {
                if (!s.empty()) {
                    rtcCfg.iceServers.emplace_back(s);
                }
            }
        };
        appendIceServers(config.stunServers);
        appendIceServers(config.turnServers);
        appendIceServers(config.iceServers);
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
                self->updateDisconnectedFlag(gen, true, true);
            } else if (ist == rtc::PeerConnection::IceState::Connected ||
                       ist == rtc::PeerConnection::IceState::Completed) {
                self->updateDisconnectedFlag(gen, true, false);
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
                    self->setState(gen, WaveTransportState::Failed,
                                   "DTLS fingerprint verification failed");
                    peerRef->close();
                    return;
                }

                self->remoteFingerprintVerified.store(
                    true, std::memory_order_release);
                self->updateDisconnectedFlag(gen, false, false);

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
                self->updateDisconnectedFlag(gen, false, true);
                break;
            case rtc::PeerConnection::State::Failed:
                self->setState(gen, WaveTransportState::Failed,
                               "peer connection failed");
                break;
            case rtc::PeerConnection::State::Closed: {
                bool isFailed = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    isFailed = (self->failedGen == gen) ||
                               (self->state.load(std::memory_order_acquire) ==
                                WaveTransportState::Failed);
                }
                if (!isFailed) {
                    self->setState(gen, WaveTransportState::Closed,
                                   "peer connection closed");
                }
                break;
            }
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
                const std::span<const std::uint8_t> raw(
                    reinterpret_cast<const std::uint8_t*>(data.data()),
                    data.size());
                ParsedRtpPacket rtp;
                if (!parseRtpPacket(raw, rtp) || rtp.payloadType != 111 ||
                    rtp.payloadSize == 0 || rtp.ssrc == 0) {
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
                    if (!self->hasExpectedRemoteSsrc) {
                        self->expectedRemoteSsrc = rtp.ssrc;
                        self->hasExpectedRemoteSsrc = true;
                    } else if (rtp.ssrc != self->expectedRemoteSsrc) {
                        return;
                    }
                    seq32 = self->seqUnwrapper.unwrap(rtp.sequence);
                    cb = self->callbacks.onMediaPacket;
                }

                if (!cb) {
                    return;
                }
                WaveMediaPacket pkt;
                pkt.sequence = seq32;
                pkt.timestamp48k = rtp.timestamp;
                pkt.frameSamples = static_cast<std::uint16_t>(kWaveFrameSamples);
                pkt.flags = rtp.marker ? kWavePacketDiscontinuity
                                       : kWavePacketNone;
                const std::uint8_t* body = raw.data() + rtp.payloadOffset;
                pkt.payload.assign(body, body + rtp.payloadSize);

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
        std::uint32_t localSsrc = 0;
        rtc::Configuration rtcCfg;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                gen = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                gatheringComplete = false;
                gatheredCandidates.clear();
                expectedRemoteFingerprint.clear();
                selectedCandidateSummary.clear();
                remoteFingerprintVerified.store(false,
                                                std::memory_order_release);
                hasExpectedRemoteSsrc = false;
                expectedRemoteSsrc = 0;
                iceDisconnected = false;
                peerDisconnected = false;
                disconnectTimerActive = false;
                lastFeedbackTime = -1.0;
                failedGen = 0;
                ssrc = generateRandomSsrc();
                localSsrc = ssrc;
                state.store(WaveTransportState::Idle,
                            std::memory_order_release);
                rtcCfg = buildRtcConfig();
            }

            auto peer = std::make_shared<rtc::PeerConnection>(rtcCfg);
            attachPeerCallbacks(peer, gen);

            rtc::Description::Audio media("audio",
                                          rtc::Description::Direction::SendOnly);
            media.addOpusCodec(111);
            media.addSSRC(localSsrc, "bgwave-audio");
            auto localTrack = peer->addTrack(media);

            auto localRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                localSsrc, "bgwave-audio", std::uint8_t{111},
                std::uint32_t{48000});
            auto localPacketizer =
                std::make_shared<rtc::OpusRtpPacketizer>(localRtpConfig);
            auto localSrReporter =
                std::make_shared<rtc::RtcpSrReporter>(localRtpConfig);
            auto localRtcpSession =
                std::make_shared<rtc::RtcpReceivingSession>();
            localPacketizer->addToChain(localSrReporter);
            localPacketizer->addToChain(localRtcpSession);
            localTrack->setMediaHandler(localPacketizer);

            bool committed = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (gen == generation.load(std::memory_order_acquire)) {
                    pc = peer;
                    track = localTrack;
                    rtpConfig = std::move(localRtpConfig);
                    packetizer = std::move(localPacketizer);
                    srReporter = std::move(localSrReporter);
                    rtcpSession = std::move(localRtcpSession);
                    committed = true;
                }
            }
            if (!committed) {
                localTrack->resetCallbacks();
                localTrack->close();
                peer->resetCallbacks();
                peer->close();
                return false;
            }

            setState(gen, WaveTransportState::Gathering,
                     "gathering ICE candidates");
            if (gen != generation.load(std::memory_order_acquire)) {
                return false;
            }
            peer->setLocalDescription(rtc::Description::Type::Offer);
        } catch (const std::exception& e) {
            if (gen != 0) {
                setState(gen, WaveTransportState::Failed,
                         std::string("failed to start offer: ") + e.what());
            }
            closeCurrentPeer(gen);
            return false;
        } catch (...) {
            if (gen != 0) {
                setState(gen, WaveTransportState::Failed,
                         "failed to start offer");
            }
            closeCurrentPeer(gen);
            return false;
        }

        if (!waitForGatheringAndEmitSignal(gen)) {
            setState(gen, WaveTransportState::Failed,
                     "ICE gathering produced no usable candidates");
            closeCurrentPeer(gen);
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
        rtc::Configuration rtcCfg;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                gen = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                gatheringComplete = false;
                gatheredCandidates.clear();
                expectedRemoteFingerprint = fpHex;
                selectedCandidateSummary.clear();
                remoteFingerprintVerified.store(false,
                                                std::memory_order_release);
                hasExpectedRemoteSsrc = false;
                expectedRemoteSsrc = 0;
                iceDisconnected = false;
                peerDisconnected = false;
                disconnectTimerActive = false;
                lastFeedbackTime = -1.0;
                failedGen = 0;
                seqUnwrapper.reset();
                state.store(WaveTransportState::Idle,
                            std::memory_order_release);
                rtcCfg = buildRtcConfig();
            }

            auto peer = std::make_shared<rtc::PeerConnection>(rtcCfg);
            attachPeerCallbacks(peer, gen);

            bool committed = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (gen == generation.load(std::memory_order_acquire)) {
                    pc = peer;
                    committed = true;
                }
            }
            if (!committed) {
                peer->resetCallbacks();
                peer->close();
                return false;
            }

            setState(gen, WaveTransportState::Gathering,
                     "gathering ICE answer");
            if (gen != generation.load(std::memory_order_acquire)) {
                return false;
            }
            const std::string offerSdp =
                formatWaveSignalToSdp(remoteOffer, true);
            peer->setRemoteDescription(
                rtc::Description(offerSdp, rtc::Description::Type::Offer));
            peer->setLocalDescription(rtc::Description::Type::Answer);
        } catch (const std::exception& e) {
            if (gen != 0) {
                setState(gen, WaveTransportState::Failed,
                         std::string("failed to accept remote offer: ") +
                             e.what());
            }
            closeCurrentPeer(gen);
            return false;
        } catch (...) {
            if (gen != 0) {
                setState(gen, WaveTransportState::Failed,
                         "failed to accept remote offer");
            }
            closeCurrentPeer(gen);
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
            closeCurrentPeer(gen);
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
        } catch (const std::exception& e) {
            setState(gen, WaveTransportState::Failed,
                     std::string("failed to apply remote answer: ") + e.what());
            return false;
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
        bool disconnectTimedOut = false;
        std::function<void(std::uint32_t)> bitrateCb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            gen = generation.load(std::memory_order_acquire);
            peer = pc;
            const WaveTransportState cur =
                state.load(std::memory_order_acquire);
            const auto now = std::chrono::steady_clock::now();
            if (connectTimerActive &&
                (cur == WaveTransportState::Gathering ||
                 cur == WaveTransportState::Connecting)) {
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - connectStartTime_)
                        .count();
                if (elapsedMs > config.connectTimeoutMs) {
                    connectTimerActive = false;
                    timedOut = true;
                }
            }
            if (!timedOut && disconnectTimerActive &&
                (cur == WaveTransportState::Connected ||
                 cur == WaveTransportState::Connecting)) {
                const auto discElapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - disconnectStartTime_)
                        .count();
                if (discElapsedMs >
                    std::max(0, config.disconnectGraceTimeoutMs)) {
                    disconnectTimerActive = false;
                    disconnectTimedOut = true;
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
            setState(gen, WaveTransportState::Failed,
                     "ICE/DTLS connection timeout");
            if (peer) {
                peer->close();
            }
            return;
        }
        if (disconnectTimedOut) {
            setState(gen, WaveTransportState::Failed,
                     "ICE/peer connection disconnected timeout");
            if (peer) {
                peer->close();
            }
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
    bool hasExpectedRemoteSsrc = false;
    std::uint32_t expectedRemoteSsrc = 0;
    RtpSequenceUnwrapper seqUnwrapper;
    std::chrono::steady_clock::time_point connectStartTime_{};
    bool connectTimerActive = false;
    std::chrono::steady_clock::time_point disconnectStartTime_{};
    bool iceDisconnected = false;
    bool peerDisconnected = false;
    bool disconnectTimerActive = false;
    std::uint64_t failedGen = 0;
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
