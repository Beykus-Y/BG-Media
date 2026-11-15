#include "bgmedia/wave/WaveAudioPipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>

namespace bgmedia::wave {
namespace {
std::int64_t resolveTimestamp(std::int64_t nowUnixSeconds) noexcept {
    if (nowUnixSeconds > 0) {
        return nowUnixSeconds;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
struct AudioFlightGuard {
    std::atomic<std::uint32_t>& counter;
    explicit AudioFlightGuard(std::atomic<std::uint32_t>& c) noexcept : counter(c) {
        counter.fetch_add(1, std::memory_order_seq_cst);
    }
    ~AudioFlightGuard() {
        counter.fetch_sub(1, std::memory_order_seq_cst);
    }
};
} // namespace

WaveAudioPipeline::WaveAudioPipeline(WavePipelineConfig config,
                                     std::shared_ptr<IWaveAudioCodec> codec,
                                     std::shared_ptr<IWaveTransport> transport)
    : config_(config),
      codec_(codec ? std::move(codec) : std::make_shared<Pcm16WaveCodec>()),
      transport_(std::move(transport)),
      hostRing_(config.ringCapacityFrames),
      listenerRing_(config.ringCapacityFrames),
      jitterBuffer_(config.jitterConfig) {
    codec_->setBitrate(std::clamp(config_.targetBitrateBps, kWaveMinBitrateBps,
                                  kWaveMaxBitrateBps));
    hostStaging_.reserve(4096 * 2);
    hostResampled_.reserve(4096 * 2);
    hostChunk_.reserve(codec_->frameSamples() * codec_->channels());
    encodedPayload_.reserve(4096);
    listenerDecodeBuf_.resize(codec_->frameSamples() * codec_->channels(), 0.0f);
    listenerResampled_.reserve(4096 * 2);
    wireTransportCallbacks();
}

WaveAudioPipeline::~WaveAudioPipeline() {
    if (lifecycle_) {
        std::lock_guard<std::recursive_mutex> gLock(lifecycle_->cbMutex);
        lifecycle_->alive = false;
    }
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> cLock(controlMutex_);
        tx = transport_;
    }
    std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
    if (tx) {
        tx->setCallbacks({});
        tx->close();
    }
    if (ext) {
        ext->setCallbacks({});
        ext->stop();
    }
}

std::shared_ptr<IExternalCallTransport>
WaveAudioPipeline::snapshotExternalCall() const {
    std::lock_guard<std::mutex> lock(externalCallMutex_);
    return externalCall_;
}

void WaveAudioPipeline::setCallbacks(WavePipelineCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    callbacks_ = std::move(callbacks);
}

WavePipelineCallbacks WaveAudioPipeline::snapshotCallbacks() const {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    return callbacks_;
}

void WaveAudioPipeline::setState(WaveSessionState next,
                                 const std::string& reason) {
    const WaveSessionState prev =
        state_.exchange(next, std::memory_order_acq_rel);
    if (prev == next) {
        return;
    }
    if (next == WaveSessionState::LiveP2P) {
        route_.store(WaveTransportRoute::P2P, std::memory_order_release);
    } else if (next == WaveSessionState::LiveTelegramCall) {
        route_.store(WaveTransportRoute::ExternalCall,
                     std::memory_order_release);
    } else if (next == WaveSessionState::Ended ||
               next == WaveSessionState::Failed ||
               next == WaveSessionState::P2PFailed ||
               next == WaveSessionState::Idle) {
        route_.store(WaveTransportRoute::None, std::memory_order_release);
    }

    const auto cb = snapshotCallbacks();
    if (cb.onStateChanged) {
        cb.onStateChanged(next, reason);
    }
}

void WaveAudioPipeline::emitError(WaveErrorCode code,
                                  const std::string& message) {
    const auto cb = snapshotCallbacks();
    if (cb.onError) {
        cb.onError(WaveError{code, message});
    }
}

void WaveAudioPipeline::wireTransportCallbacks() {
    if (!transport_) {
        return;
    }
    std::shared_ptr<LifecycleGuard> guard = lifecycle_;
    WaveAudioPipeline* self = this;
    WaveTransportCallbacks cb;
    cb.onStateChanged = [guard, self](WaveTransportState tState,
                                      const std::string& reason) {
        std::lock_guard<std::recursive_mutex> gLock(guard->cbMutex);
        if (!guard->alive) {
            return;
        }
        switch (tState) {
        case WaveTransportState::Connected:
            self->setState(WaveSessionState::LiveP2P, reason);
            break;
        case WaveTransportState::Failed:
            if (self->state() != WaveSessionState::Ended &&
                self->state() != WaveSessionState::Idle) {
                self->setState(WaveSessionState::P2PFailed, reason);
                self->emitError(WaveErrorCode::TransportFailed, reason);
            }
            break;
        case WaveTransportState::Closed:
            if (self->state() == WaveSessionState::LiveP2P ||
                self->state() == WaveSessionState::ConnectingP2P) {
                self->setState(WaveSessionState::Ended, reason);
            }
            break;
        default:
            break;
        }
    };
    cb.onLocalSignalReady = [guard, self](const WaveSdpSignal& localSig) {
        std::lock_guard<std::recursive_mutex> gLock(guard->cbMutex);
        if (!guard->alive) {
            return;
        }
        std::lock_guard<std::mutex> cLock(self->controlMutex_);
        self->lastLocalSignal_ = localSig;
        self->hasLastLocalSignal_ = true;
    };
    cb.onMediaPacket = [guard, self](WaveMediaPacket packet,
                                     double arrivalTimeSeconds) {
        std::lock_guard<std::recursive_mutex> gLock(guard->cbMutex);
        if (!guard->alive) {
            return;
        }
        if (self->config_.role != WaveRole::Listener ||
            self->route() != WaveTransportRoute::P2P) {
            return;
        }
        constexpr std::size_t kMaxInboundPackets = 256;
        std::lock_guard<std::mutex> iLock(self->inboundMutex_);
        if (self->inboundQueue_.size() >= kMaxInboundPackets) {
            self->inboundQueue_.pop_front();
        }
        self->inboundQueue_.push_back(
            InboundPacket{std::move(packet), arrivalTimeSeconds});
    };
    transport_->setCallbacks(std::move(cb));
}

void WaveAudioPipeline::performResetUnderProcessLock(bool clearStats) {
    if (!clearStats) {
        publishStatsSnapshot(true);
    }
    {
        std::lock_guard<std::mutex> iLock(inboundMutex_);
        inboundQueue_.clear();
    }
    hostRing_.reset();
    listenerRing_.reset();
    jitterBuffer_.reset();
    hostResampler_.reset();
    listenerResampler_.reset();
    hostChunk_.clear();
    codec_->reset();
    hasPlayoutClock_ = false;
    nextPlayoutTimeSec_ = 0.0;
    hostDiscardPending_.store(false, std::memory_order_release);
    if (clearStats) {
        nextSequence_ = 0;
        nextTimestamp48k_ = 0;
        packetsSent_ = 0;
        plcFrames_ = 0;
        bytesSent_ = 0;
        bytesReceived_ = 0;
        {
            std::lock_guard<std::mutex> sLock(statsMutex_);
            statsFrozenOnStop_ = false;
        }
        publishStatsSnapshot(false);
    }
    pendingStatsClear_.store(false, std::memory_order_release);
    pendingRingReset_.store(false, std::memory_order_release);
    audioPausedForReset_.store(false, std::memory_order_seq_cst);
}

void WaveAudioPipeline::requestOrPerformReset(bool clearStats) {
    if (clearStats) {
        pendingStatsClear_.store(true, std::memory_order_release);
    }
    audioPausedForReset_.store(true, std::memory_order_seq_cst);
    pendingRingReset_.store(true, std::memory_order_release);
    // Bounded wait: never block control/UI thread indefinitely if an audio
    // callback is stalled. If audioInFlight_ or processMutex_ is busy, the
    // reset completes deferred in process() or ensureResetCompletedForNewSession().
    for (int i = 0; i < 64; ++i) {
        if (audioInFlight_.load(std::memory_order_seq_cst) == 0) {
            break;
        }
        std::this_thread::yield();
    }
    if (audioInFlight_.load(std::memory_order_seq_cst) == 0) {
        std::unique_lock<std::mutex> pLock(processMutex_, std::try_to_lock);
        if (pLock.owns_lock() &&
            audioInFlight_.load(std::memory_order_seq_cst) == 0) {
            performResetUnderProcessLock(
                pendingStatsClear_.load(std::memory_order_acquire));
        }
    }
}

bool WaveAudioPipeline::ensureResetCompletedForNewSession() {
    pendingStatsClear_.store(true, std::memory_order_release);
    audioPausedForReset_.store(true, std::memory_order_seq_cst);
    pendingRingReset_.store(true, std::memory_order_release);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    for (int attempt = 0;; ++attempt) {
        if (!pendingRingReset_.load(std::memory_order_acquire) &&
            !audioPausedForReset_.load(std::memory_order_seq_cst)) {
            return true;
        }
        if (audioInFlight_.load(std::memory_order_seq_cst) == 0) {
            std::unique_lock<std::mutex> pLock(processMutex_, std::try_to_lock);
            if (pLock.owns_lock() &&
                audioInFlight_.load(std::memory_order_seq_cst) == 0) {
                performResetUnderProcessLock(true);
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (attempt < 16) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    return !pendingRingReset_.load(std::memory_order_acquire) &&
           !audioPausedForReset_.load(std::memory_order_seq_cst);
}

bool WaveAudioPipeline::startOffer(std::int64_t nowUnixSeconds,
                                   std::int64_t ttlSeconds) {
    if (config_.role != WaveRole::Host) {
        emitError(WaveErrorCode::InvalidState,
                  "Only Host role can start an offer");
        return false;
    }
    const std::int64_t ts = resolveTimestamp(nowUnixSeconds);
    const std::int64_t rawTtl =
        ttlSeconds > 0 ? ttlSeconds : config_.defaultOfferTtlSeconds;
    const std::int64_t ttl =
        std::clamp<std::int64_t>(rawTtl, 1, kWaveMaxSignalTtlSeconds);
    if (ts <= 0 || ts > std::numeric_limits<std::int64_t>::max() - ttl) {
        emitError(WaveErrorCode::InvalidArgument,
                  "Offer timestamp out of range");
        return false;
    }
    std::shared_ptr<IWaveTransport> tx;
    std::array<std::uint8_t, 16> newSid{};
    std::uint64_t nonce = 0;
    std::uint64_t myGen = 0;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (!transport_) {
            // handled below
        } else {
            const WaveSessionState cur = state();
            if (cur == WaveSessionState::Idle || cur == WaveSessionState::Ended ||
                cur == WaveSessionState::P2PFailed || cur == WaveSessionState::Failed) {
                std::string sidErr;
                if (generateWaveSessionId(newSid, &sidErr)) {
                    {
                        std::lock_guard<std::mutex> sLock(sidMutex_);
                        sid_ = newSid;
                    }
                    hasLastLocalSignal_ = false;
                    hasLastRemoteNonce_ = false;
                    lastRemoteNonce_ = 0;
                    nonce = nextNonce_++;
                    myGen = sessionGen_.fetch_add(1, std::memory_order_acq_rel) + 1;
                    tx = transport_;
                }
            }
        }
    }
    if (!transport_) {
        emitError(WaveErrorCode::TransportFailed, "No P2P transport configured");
        return false;
    }
    if (!tx) {
        emitError(WaveErrorCode::InvalidState,
                  "Cannot start offer from current state");
        return false;
    }
    if (!ensureResetCompletedForNewSession()) {
        emitError(WaveErrorCode::InvalidState,
                  "Audio pipeline is still quiescing from previous session");
        return false;
    }
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    setState(WaveSessionState::Offering, "starting offer");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    if (!tx->startOffer()) {
        if (sessionGen_.load(std::memory_order_acquire) == myGen) {
            setState(WaveSessionState::P2PFailed, "failed to gather local offer");
        }
        return false;
    }
    WaveSdpSignal localSig;
    bool haveLocalSig = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        haveLocalSig = hasLastLocalSignal_;
        if (haveLocalSig) {
            localSig = lastLocalSignal_;
        }
    }
    if (!haveLocalSig) {
        if (sessionGen_.load(std::memory_order_acquire) == myGen) {
            setState(WaveSessionState::P2PFailed, "failed to gather local offer");
        }
        return false;
    }
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }

    WaveSignalMessage msg;
    msg.version = kWaveSignalVersion;
    msg.type = WaveSignalType::Offer;
    msg.sid = newSid;
    msg.iat = ts;
    msg.exp = ts + ttl;
    msg.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp |
               kWaveCapExternalCallFallback;
    msg.nonce = nonce;
    msg.signal = std::move(localSig);

    setState(WaveSessionState::AwaitingAnswer, "offer ready");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        // Cancelled/stopped inside onStateChanged(AwaitingAnswer): do not emit orphan OFFER.
        return false;
    }
    const auto cb = snapshotCallbacks();
    if (cb.onOutboundSignal) {
        cb.onOutboundSignal(msg);
    }
    return true;
}

bool WaveAudioPipeline::acceptOffer(const WaveSignalMessage& offer,
                                    std::int64_t nowUnixSeconds) {
    if (config_.role != WaveRole::Listener) {
        emitError(WaveErrorCode::InvalidState,
                  "Only Listener role can accept an offer");
        return false;
    }
    const std::int64_t effectiveNow = resolveTimestamp(nowUnixSeconds);
    std::string err;
    if (!validateWaveSignal(offer, effectiveNow, &err) ||
        offer.type != WaveSignalType::Offer) {
        emitError(WaveErrorCode::SignalingFailed,
                  err.empty() ? "Invalid Offer message" : err);
        return false;
    }
    std::shared_ptr<IWaveTransport> tx;
    std::uint64_t nonce = 0;
    std::uint64_t myGen = 0;
    bool badState = false;
    bool replayedOffer = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        const WaveSessionState cur = state();
        const bool stateAllowed =
            (cur == WaveSessionState::Idle ||
             cur == WaveSessionState::Ended ||
             cur == WaveSessionState::P2PFailed ||
             cur == WaveSessionState::Failed);
        if (!stateAllowed) {
            badState = true;
        } else {
            replayedOffer = std::any_of(
                acceptedOfferHistory_.begin(), acceptedOfferHistory_.end(),
                [&](const AcceptedOfferRecord& rec) {
                    return rec.sid == offer.sid && rec.nonce == offer.nonce;
                });
        }
        if (!badState && !replayedOffer && transport_) {
            tx = transport_;
            if (acceptedOfferHistory_.size() >= kMaxAcceptedOfferHistory) {
                acceptedOfferHistory_.pop_front();
            }
            acceptedOfferHistory_.push_back(
                AcceptedOfferRecord{offer.sid, offer.nonce});
            {
                std::lock_guard<std::mutex> sLock(sidMutex_);
                sid_ = offer.sid;
            }
            hasLastLocalSignal_ = false;
            hasLastRemoteNonce_ = true;
            lastRemoteNonce_ = offer.nonce;
            nonce = nextNonce_++;
            myGen = sessionGen_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
    }
    if (badState) {
        emitError(WaveErrorCode::InvalidState,
                  "Cannot accept offer from current state");
        return false;
    }
    if (replayedOffer) {
        emitError(WaveErrorCode::SignalingFailed, "Replayed offer signal");
        return false;
    }
    if (!tx) {
        emitError(WaveErrorCode::TransportFailed, "No P2P transport configured");
        return false;
    }
    if (!ensureResetCompletedForNewSession()) {
        emitError(WaveErrorCode::InvalidState,
                  "Audio pipeline is still quiescing from previous session");
        return false;
    }
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    setState(WaveSessionState::ConnectingP2P, "accepting offer");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        // Cancelled/stopped inside onStateChanged(ConnectingP2P): do not acceptOffer or emit ANSWER.
        return false;
    }
    if (!tx->acceptOffer(offer.signal)) {
        if (sessionGen_.load(std::memory_order_acquire) == myGen) {
            setState(WaveSessionState::P2PFailed, "failed to accept offer");
        }
        return false;
    }
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }

    bool emitAns = false;
    WaveSignalMessage ans;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (hasLastLocalSignal_) {
            emitAns = true;
            ans.version = kWaveSignalVersion;
            ans.type = WaveSignalType::Answer;
            ans.sid = offer.sid;
            ans.iat = effectiveNow;
            ans.exp = offer.exp;
            ans.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp |
                       kWaveCapExternalCallFallback;
            ans.nonce = nonce;
            ans.signal = lastLocalSignal_;
        }
    }
    const auto cb = snapshotCallbacks();
    if (emitAns && cb.onOutboundSignal) {
        cb.onOutboundSignal(ans);
    }
    return true;
}

bool WaveAudioPipeline::acceptAnswer(const WaveSignalMessage& answer,
                                     std::int64_t nowUnixSeconds) {
    if (config_.role != WaveRole::Host) {
        emitError(WaveErrorCode::InvalidState,
                  "Only Host role can accept an answer");
        return false;
    }
    WaveSessionState curState = WaveSessionState::Idle;
    std::array<std::uint8_t, 16> curSid{};
    std::shared_ptr<IWaveTransport> tx;
    std::uint64_t myGen = 0;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        curState = state();
        curSid = sessionId();
        tx = transport_;
        myGen = sessionGen_.load(std::memory_order_acquire);
    }
    if (curState != WaveSessionState::AwaitingAnswer) {
        emitError(WaveErrorCode::InvalidState,
                  "Not awaiting an answer");
        return false;
    }
    const std::int64_t effectiveNow = resolveTimestamp(nowUnixSeconds);
    std::string err;
    if (!validateWaveSignal(answer, effectiveNow, &err) ||
        answer.type != WaveSignalType::Answer || answer.sid != curSid) {
        emitError(WaveErrorCode::SignalingFailed,
                  err.empty() ? "Invalid or mismatched Answer" : err);
        return false;
    }
    bool answerStillCurrent = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        answerStillCurrent = state() == WaveSessionState::AwaitingAnswer &&
            sessionGen_.load(std::memory_order_acquire) == myGen;
        if (answerStillCurrent) {
            hasLastRemoteNonce_ = true;
            lastRemoteNonce_ = answer.nonce;
        }
    }
    if (!answerStillCurrent) {
        emitError(WaveErrorCode::InvalidState, "Not awaiting an answer");
        return false;
    }
    setState(WaveSessionState::ConnectingP2P, "applying answer");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    if (!tx || !tx->applyAnswer(answer.signal)) {
        if (sessionGen_.load(std::memory_order_acquire) == myGen) {
            setState(WaveSessionState::P2PFailed, "failed to apply answer");
        }
        return false;
    }
    return true;
}

bool WaveAudioPipeline::handleRemoteSignal(const WaveSignalMessage& msg,
                                           std::int64_t nowUnixSeconds) {
    const std::int64_t effectiveNow = resolveTimestamp(nowUnixSeconds);
    std::string err;
    if (!validateWaveSignal(msg, effectiveNow, &err)) {
        emitError(WaveErrorCode::SignalingFailed, err);
        return false;
    }
    if (msg.type == WaveSignalType::Offer) {
        return acceptOffer(msg, effectiveNow);
    }
    if (msg.type == WaveSignalType::Answer) {
        return acceptAnswer(msg, effectiveNow);
    }
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (msg.sid != sessionId()) {
            return false;
        }
        if (hasLastRemoteNonce_ && msg.nonce <= lastRemoteNonce_) {
            return false; // replay guard
        }
        lastRemoteNonce_ = msg.nonce;
        hasLastRemoteNonce_ = true;
        tx = transport_;
    }
    std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
    if (tx) {
        tx->close();
    }
    if (ext) {
        ext->stop();
    }
    setState(WaveSessionState::Ended,
             msg.type == WaveSignalType::Cancel ? "cancelled by peer"
                                                : "ended by peer");
    return true;
}

bool WaveAudioPipeline::reconnect(std::int64_t nowUnixSeconds) {
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        const WaveSessionState cur = state();
        if (cur != WaveSessionState::P2PFailed &&
            cur != WaveSessionState::Ended &&
            cur != WaveSessionState::Failed) {
            return false;
        }
        tx = transport_;
    }
    if (tx) {
        tx->close();
    }
    if (config_.role == WaveRole::Host) {
        return startOffer(nowUnixSeconds);
    }
    setState(WaveSessionState::Idle, "ready for new offer");
    return true;
}

void WaveAudioPipeline::stop(bool emitEndSignal, std::int64_t nowUnixSeconds) {
    sessionGen_.fetch_add(1, std::memory_order_acq_rel);
    pendingStatsClear_.store(false, std::memory_order_release);
    bool shouldEmit = false;
    WaveSignalMessage endMsg;
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        const WaveSessionState cur = state();
        if (emitEndSignal &&
            (cur == WaveSessionState::Offering ||
             cur == WaveSessionState::AwaitingAnswer ||
             cur == WaveSessionState::ConnectingP2P ||
             cur == WaveSessionState::LiveP2P)) {
            shouldEmit = true;
            const std::int64_t ts = resolveTimestamp(nowUnixSeconds);
            constexpr std::int64_t kMaxInt64 =
                std::numeric_limits<std::int64_t>::max();
            if (ts <= 0 || ts >= kMaxInt64) {
                shouldEmit = false;
            } else {
                endMsg.version = kWaveSignalVersion;
                endMsg.type = (cur == WaveSessionState::Offering ||
                               cur == WaveSessionState::AwaitingAnswer)
                                  ? WaveSignalType::Cancel
                                  : WaveSignalType::End;
                endMsg.sid = sessionId();
                endMsg.iat = ts;
                endMsg.exp = (ts > kMaxInt64 - 60) ? kMaxInt64 : (ts + 60);
                endMsg.nonce = nextNonce_++;
            }
        }
        tx = transport_;
    }
    std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
    if (shouldEmit) {
        const auto cb = snapshotCallbacks();
        if (cb.onOutboundSignal) {
            cb.onOutboundSignal(endMsg);
        }
    }
    setState(WaveSessionState::Ended, "stopped");
    if (tx) {
        tx->close();
    }
    if (ext) {
        ext->stop();
    }
    requestOrPerformReset(false);
}

void WaveAudioPipeline::reset() {
    sessionGen_.fetch_add(1, std::memory_order_acq_rel);
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        hasLastLocalSignal_ = false;
        lastLocalSignal_ = WaveSdpSignal{};
        tx = transport_;
    }
    std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
    setState(WaveSessionState::Idle, "reset");
    if (tx) {
        tx->close();
    }
    if (ext) {
        ext->stop();
    }
    requestOrPerformReset(true);
    {
        std::lock_guard<std::mutex> sLock(sidMutex_);
        sid_.fill(0);
    }
}

bool WaveAudioPipeline::switchToExternalCallFallback(
    std::shared_ptr<IExternalCallTransport> externalCall) {
    std::uint64_t myGen = 0;
    bool validState = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (state() == WaveSessionState::P2PFailed) {
            validState = true;
            myGen = sessionGen_.load(std::memory_order_acquire);
        }
    }
    if (!validState) {
        emitError(WaveErrorCode::InvalidState,
                  "ExternalCall fallback only allowed from P2PFailed");
        return false;
    }
    if (!externalCall) {
        setState(WaveSessionState::Failed, "No external call transport");
        emitError(WaveErrorCode::FallbackFailed,
                  "External call transport not available");
        return false;
    }
    {
        std::lock_guard<std::mutex> eLock(externalCallMutex_);
        externalCall_ = externalCall;
    }
    hostDiscardPending_.store(true, std::memory_order_release);

    std::shared_ptr<LifecycleGuard> guard = lifecycle_;
    WaveAudioPipeline* self = this;
    ExternalCallCallbacks cb;
    cb.onStateChanged = [guard, self, myGen](ExternalCallState extState,
                                             const std::string& reason) {
        std::lock_guard<std::recursive_mutex> gLock(guard->cbMutex);
        if (!guard->alive ||
            self->sessionGen_.load(std::memory_order_acquire) != myGen) {
            return;
        }
        switch (extState) {
        case ExternalCallState::Connecting:
            self->setState(WaveSessionState::ConnectingTelegramCall, reason);
            break;
        case ExternalCallState::Live:
            self->setState(WaveSessionState::LiveTelegramCall, reason);
            break;
        case ExternalCallState::Failed:
            self->setState(WaveSessionState::Failed, reason);
            self->emitError(WaveErrorCode::FallbackFailed, reason);
            break;
        case ExternalCallState::Ended:
            self->setState(WaveSessionState::Ended, reason);
            break;
        default:
            break;
        }
    };
    externalCall->setCallbacks(std::move(cb));
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    setState(WaveSessionState::OfferingTelegramFallback,
             "offering external call fallback");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    setState(WaveSessionState::ConnectingTelegramCall,
             "connecting external call fallback");
    if (sessionGen_.load(std::memory_order_acquire) != myGen) {
        return false;
    }
    if (!externalCall->start()) {
        if (sessionGen_.load(std::memory_order_acquire) == myGen) {
            setState(WaveSessionState::Failed, "external call start failed");
            emitError(WaveErrorCode::FallbackFailed,
                      "External call transport failed to start");
        }
        return false;
    }
    return true;
}

void WaveAudioPipeline::setPublishingEnabled(bool enabled) noexcept {
    publishingEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        hostDiscardPending_.store(true, std::memory_order_release);
    }
}

void WaveAudioPipeline::setListenerGain(float gain) noexcept {
    if (!std::isfinite(gain)) {
        return;
    }
    listenerGain_.store(std::clamp(gain, 0.0f, 2.0f),
                        std::memory_order_release);
}

void WaveAudioPipeline::setListenerMuted(bool muted) noexcept {
    listenerMuted_.store(muted, std::memory_order_release);
}

void WaveAudioPipeline::onPcmRendered(const float* interleavedStereo,
                                      std::size_t frames,
                                      std::uint32_t sampleRate) noexcept {
    AudioFlightGuard guard(audioInFlight_);
    if (audioPausedForReset_.load(std::memory_order_seq_cst) ||
        config_.role != WaveRole::Host || !interleavedStereo || frames == 0) {
        return;
    }
    if (!publishingEnabled_.load(std::memory_order_acquire)) {
        return;
    }
    const WaveTransportRoute r = route_.load(std::memory_order_acquire);
    if (r == WaveTransportRoute::None) {
        return;
    }
    hostRing_.push(interleavedStereo, frames, sampleRate, true);
}

std::size_t WaveAudioPipeline::pullListenerPcm(
    float* outInterleavedStereo, std::size_t frames,
    std::uint32_t sampleRate) noexcept {
    AudioFlightGuard guard(audioInFlight_);
    if (!outInterleavedStereo || frames == 0) {
        return 0;
    }
    if (audioPausedForReset_.load(std::memory_order_seq_cst) ||
        pendingRingReset_.load(std::memory_order_acquire)) {
        std::memset(outInterleavedStereo, 0, frames * 2 * sizeof(float));
        return 0;
    }
    if (sampleRate > 0) {
        listenerOutputRate_.store(sampleRate, std::memory_order_release);
    }
    const std::size_t read = listenerRing_.pop(outInterleavedStereo, frames);
    const bool muted = listenerMuted_.load(std::memory_order_acquire);
    const float gain = muted ? 0.0f : listenerGain_.load(std::memory_order_acquire);
    const std::size_t totalSamples = frames * 2;
    if (gain == 0.0f) {
        std::memset(outInterleavedStereo, 0, totalSamples * sizeof(float));
    } else if (gain != 1.0f) {
        for (std::size_t i = 0; i < totalSamples; ++i) {
            outInterleavedStereo[i] =
                std::clamp(outInterleavedStereo[i] * gain, -1.0f, 1.0f);
        }
    }
    return read;
}

void WaveAudioPipeline::process(double currentTimeSeconds) {
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> cLock(controlMutex_);
        tx = transport_;
    }
    if (tx) {
        tx->poll(currentTimeSeconds);
    }
    std::lock_guard<std::mutex> pLock(processMutex_);
    if (pendingRingReset_.load(std::memory_order_acquire) &&
        audioInFlight_.load(std::memory_order_seq_cst) == 0) {
        performResetUnderProcessLock(
            pendingStatsClear_.load(std::memory_order_acquire));
    }
    if (config_.role == WaveRole::Host) {
        processHostStep(currentTimeSeconds);
    } else {
        processListenerStep(currentTimeSeconds);
    }
    publishStatsSnapshot(false);
}

void WaveAudioPipeline::clearHostPendingAudio() {
    hostChunk_.clear();
    hostResampler_.reset();
    std::size_t remaining = hostRing_.availableRead();
    while (remaining > 0) {
        const std::size_t chunk = std::min<std::size_t>(remaining, 4096);
        hostStaging_.resize(chunk * 2);
        const std::size_t popped = hostRing_.pop(hostStaging_.data(), chunk);
        if (popped == 0) {
            break;
        }
        remaining -= popped;
    }
}

void WaveAudioPipeline::processHostStep(double currentTimeSeconds) {
    if (!publishingEnabled_.load(std::memory_order_acquire)) {
        hostDiscardPending_.store(false, std::memory_order_release);
        clearHostPendingAudio();
        return;
    }
    if (hostDiscardPending_.exchange(false, std::memory_order_acq_rel)) {
        clearHostPendingAudio();
    }

    const WaveTransportRoute r = route_.load(std::memory_order_acquire);
    if (r == WaveTransportRoute::None) {
        return;
    }

    const std::size_t available = hostRing_.availableRead();
    if (available == 0) {
        return;
    }

    const std::size_t toPop = std::min<std::size_t>(available, 4096);
    hostStaging_.resize(toPop * 2);
    const std::size_t readFrames = hostRing_.pop(hostStaging_.data(), toPop);
    if (readFrames == 0) {
        return;
    }
    hostStaging_.resize(readFrames * 2);

    const std::uint32_t inRate = hostRing_.lastSampleRate();
    hostResampler_.configure(inRate, kWaveSampleRate);
    hostResampled_.clear();
    hostResampler_.process(hostStaging_, hostResampled_);

    if (r == WaveTransportRoute::ExternalCall) {
        std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
        if (ext) {
            ext->pushOutgoingPcm(hostResampled_.data(),
                                 hostResampled_.size() / 2,
                                 kWaveSampleRate);
        }
        return;
    }

    if (r != WaveTransportRoute::P2P || !transport_) {
        return;
    }

    hostChunk_.insert(hostChunk_.end(), hostResampled_.begin(),
                      hostResampled_.end());

    const std::size_t frameSamples = codec_->frameSamples();
    const std::size_t samplesPerFrame = frameSamples * codec_->channels();
    std::size_t offset = 0;
    while (offset + samplesPerFrame <= hostChunk_.size()) {
        std::span<const float> frameSpan(hostChunk_.data() + offset,
                                         samplesPerFrame);
        if (codec_->encode(frameSpan, encodedPayload_)) {
            WaveMediaPacket pkt;
            pkt.sequence = nextSequence_;
            pkt.timestamp48k = nextTimestamp48k_;
            pkt.frameSamples = static_cast<std::uint16_t>(frameSamples);
            pkt.flags = kWavePacketNone;
            pkt.payload = encodedPayload_;
            if (transport_->sendMediaPacket(pkt)) {
                ++nextSequence_;
                nextTimestamp48k_ += static_cast<std::uint32_t>(frameSamples);
                ++packetsSent_;
                bytesSent_ += pkt.payload.size();
            }
        }
        offset += samplesPerFrame;
    }

    if (offset > 0) {
        hostChunk_.erase(hostChunk_.begin(),
                         hostChunk_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    (void)currentTimeSeconds;
}

void WaveAudioPipeline::processListenerStep(double currentTimeSeconds) {
    const WaveTransportRoute r = route_.load(std::memory_order_acquire);
    if (r == WaveTransportRoute::ExternalCall) {
        std::shared_ptr<IExternalCallTransport> ext = snapshotExternalCall();
        if (ext) {
            const std::size_t wantFrames = kWaveFrameSamples;
            listenerDecodeBuf_.resize(wantFrames * 2);
            const std::size_t got = ext->pullIncomingPcm(
                listenerDecodeBuf_.data(), wantFrames, kWaveSampleRate);
            if (got > 0) {
                const std::uint32_t outRate =
                    listenerOutputRate_.load(std::memory_order_acquire);
                listenerResampler_.configure(kWaveSampleRate, outRate);
                listenerResampled_.clear();
                listenerResampler_.process(
                    std::span<const float>(listenerDecodeBuf_.data(), got * 2),
                    listenerResampled_);
                listenerRing_.push(listenerResampled_.data(),
                                   listenerResampled_.size() / 2, outRate, true);
            }
        }
        return;
    }

    if (r != WaveTransportRoute::P2P) {
        return;
    }

    {
        std::deque<InboundPacket> drained;
        {
            std::lock_guard<std::mutex> iLock(inboundMutex_);
            drained.swap(inboundQueue_);
        }
        for (auto& item : drained) {
            bytesReceived_ += item.packet.payload.size();
            jitterBuffer_.push(std::move(item.packet), item.arrivalTimeSeconds);
        }
    }

    const std::uint32_t outRate =
        listenerOutputRate_.load(std::memory_order_acquire);
    listenerResampler_.configure(kWaveSampleRate, outRate);

    const std::uint32_t codecRate = std::max<std::uint32_t>(1, codec_->sampleRate());
    const double frameDurSec =
        static_cast<double>(codec_->frameSamples()) / static_cast<double>(codecRate);
    const std::size_t outFrameFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(frameDurSec * static_cast<double>(outRate))));
    const std::size_t maxLeadFrames = std::max<std::size_t>(
        outFrameFrames,
        std::min<std::size_t>(listenerRing_.capacityFrames() / 2,
                              outFrameFrames * 3));
    const bool validTime = std::isfinite(currentTimeSeconds);

    WaveMediaPacket pkt;
    while (listenerRing_.availableRead() < maxLeadFrames &&
           listenerRing_.availableWrite() >= (outFrameFrames + 4)) {
        const bool excessBuffered =
            jitterBuffer_.queuedCount() > jitterBuffer_.targetDepthFrames();
        if (validTime) {
            if (!hasPlayoutClock_) {
                nextPlayoutTimeSec_ = currentTimeSeconds;
                hasPlayoutClock_ = true;
            } else if (currentTimeSeconds + 1.0 < nextPlayoutTimeSec_ ||
                       currentTimeSeconds - nextPlayoutTimeSec_ >
                           frameDurSec * 4.0) {
                nextPlayoutTimeSec_ = currentTimeSeconds;
            }

            const bool timeDue =
                (currentTimeSeconds + 1e-9) >= nextPlayoutTimeSec_;
            const bool needCatchUp =
                excessBuffered &&
                listenerRing_.availableRead() < (outFrameFrames * 2);
            if (!timeDue && !needCatchUp) {
                break;
            }
        } else if (!excessBuffered &&
                   listenerRing_.availableRead() >= outFrameFrames) {
            break;
        }

        const auto status = jitterBuffer_.popNext(pkt);
        if (status == WaveJitterBuffer::PopStatus::Buffering) {
            hasPlayoutClock_ = false;
            break;
        }
        if (validTime) {
            if ((currentTimeSeconds + 1e-9) >= nextPlayoutTimeSec_) {
                nextPlayoutTimeSec_ += frameDurSec;
            } else {
                nextPlayoutTimeSec_ = currentTimeSeconds + frameDurSec;
            }
        }
        bool decoded = false;
        if (status == WaveJitterBuffer::PopStatus::Packet) {
            decoded = codec_->decode(pkt.payload, listenerDecodeBuf_);
        } else {
            ++plcFrames_;
            decoded = codec_->decodePlc(listenerDecodeBuf_);
        }
        if (decoded) {
            listenerResampled_.clear();
            listenerResampler_.process(listenerDecodeBuf_, listenerResampled_);
            listenerRing_.push(listenerResampled_.data(),
                               listenerResampled_.size() / 2, outRate, true);
        }
    }
}

void WaveAudioPipeline::publishStatsSnapshot(bool freezeOnStop) {
    WaveStats s;
    s.packetsSent = packetsSent_;
    s.packetsReceived = jitterBuffer_.packetsReceived();
    s.packetsLost = jitterBuffer_.packetsLost();
    s.packetsReordered = jitterBuffer_.packetsReordered();
    s.packetsDroppedLate = jitterBuffer_.packetsDroppedLate();
    s.packetsDuplicate = jitterBuffer_.packetsDuplicate();
    s.plcFrames = plcFrames_;
    s.bytesSent = bytesSent_;
    s.bytesReceived = bytesReceived_;
    s.jitterMs = jitterBuffer_.jitterMs();
    s.jitterBufferDepthMs = static_cast<std::uint32_t>(
        jitterBuffer_.targetDepthFrames() * kWaveFrameDurationMs);
    s.estimatedLatencyMs = static_cast<double>(s.jitterBufferDepthMs) +
                           s.jitterMs +
                           static_cast<double>(kWaveFrameDurationMs);
    s.targetBitrateBps = codec_ ? codec_->bitrate() : config_.targetBitrateBps;
    if (freezeOnStop) {
        s.ringOverruns = hostRing_.overruns() + listenerRing_.overruns();
        s.ringUnderruns = hostRing_.underruns() + listenerRing_.underruns();
        s.clippedSamples =
            hostRing_.clippedSamples() + listenerRing_.clippedSamples();
    }
    std::lock_guard<std::mutex> sLock(statsMutex_);
    if (statsFrozenOnStop_) {
        return;
    }
    publishedStats_ = s;
    if (freezeOnStop) {
        statsFrozenOnStop_ = true;
    }
}

std::array<std::uint8_t, 16> WaveAudioPipeline::sessionId() const noexcept {
    std::lock_guard<std::mutex> lock(sidMutex_);
    return sid_;
}

WaveStats WaveAudioPipeline::stats() const {
    WaveStats s;
    bool frozen = false;
    {
        std::lock_guard<std::mutex> sLock(statsMutex_);
        s = publishedStats_;
        frozen = statsFrozenOnStop_;
    }
    if (!frozen) {
        s.ringOverruns = hostRing_.overruns() + listenerRing_.overruns();
        s.ringUnderruns = hostRing_.underruns() + listenerRing_.underruns();
        s.clippedSamples =
            hostRing_.clippedSamples() + listenerRing_.clippedSamples();
    }
    return s;
}

std::string WaveAudioPipeline::diagnosticsSummary() const {
    const WaveStats s = stats();
    std::ostringstream oss;
    oss << "WavePipeline{role="
        << (config_.role == WaveRole::Host ? "Host" : "Listener")
        << ", state=" << toString(state())
        << ", route=" << toString(route())
        << ", codec=" << (codec_ ? codec_->name() : "none")
        << ", bitrateBps=" << s.targetBitrateBps
        << ", sent=" << s.packetsSent
        << ", recv=" << s.packetsReceived
        << ", lost=" << s.packetsLost
        << ", plc=" << s.plcFrames
        << ", jitterMs=" << s.jitterMs
        << ", depthMs=" << s.jitterBufferDepthMs << "}";
    return oss.str();
}

} // namespace bgmedia::wave
