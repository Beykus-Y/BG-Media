#include "bgmedia/wave/WaveAudioPipeline.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <sstream>

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
        counter.fetch_add(1, std::memory_order_acq_rel);
    }
    ~AudioFlightGuard() {
        counter.fetch_sub(1, std::memory_order_release);
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
    if (transport_) {
        transport_->setCallbacks({});
        transport_->close();
    }
    if (externalCall_) {
        externalCall_->setCallbacks({});
        externalCall_->stop();
    }
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
    WaveTransportCallbacks cb;
    cb.onStateChanged = [this](WaveTransportState tState,
                               const std::string& reason) {
        switch (tState) {
        case WaveTransportState::Connected:
            setState(WaveSessionState::LiveP2P, reason);
            break;
        case WaveTransportState::Failed:
            if (state() != WaveSessionState::Ended &&
                state() != WaveSessionState::Idle) {
                setState(WaveSessionState::P2PFailed, reason);
                emitError(WaveErrorCode::TransportFailed, reason);
            }
            break;
        case WaveTransportState::Closed:
            if (state() == WaveSessionState::LiveP2P ||
                state() == WaveSessionState::ConnectingP2P) {
                setState(WaveSessionState::Ended, reason);
            }
            break;
        default:
            break;
        }
    };
    cb.onLocalSignalReady = [this](const WaveSdpSignal& localSig) {
        lastLocalSignal_ = localSig;
        hasLastLocalSignal_ = true;
    };
    cb.onMediaPacket = [this](WaveMediaPacket packet,
                              double arrivalTimeSeconds) {
        if (config_.role == WaveRole::Listener &&
            route() == WaveTransportRoute::P2P) {
            bytesReceived_ += packet.payload.size();
            jitterBuffer_.push(std::move(packet), arrivalTimeSeconds);
        }
    };
    transport_->setCallbacks(std::move(cb));
}

void WaveAudioPipeline::performResetUnderProcessLock() {
    hostRing_.reset();
    listenerRing_.reset();
    jitterBuffer_.reset();
    hostResampler_.reset();
    listenerResampler_.reset();
    hostChunk_.clear();
    codec_->reset();
    nextSequence_ = 0;
    nextTimestamp48k_ = 0;
    packetsSent_ = 0;
    plcFrames_ = 0;
    bytesSent_ = 0;
    bytesReceived_ = 0;
    publishStatsSnapshot();
    pendingRingReset_.store(false, std::memory_order_release);
    audioPausedForReset_.store(false, std::memory_order_release);
}

void WaveAudioPipeline::requestOrPerformReset() {
    audioPausedForReset_.store(true, std::memory_order_release);
    pendingRingReset_.store(true, std::memory_order_release);
    // Bounded wait: never block control/UI thread indefinitely if an audio
    // callback is stalled. If audioInFlight_ or processMutex_ is busy, the
    // reset completes deferred in process().
    for (int i = 0; i < 64; ++i) {
        if (audioInFlight_.load(std::memory_order_acquire) == 0) {
            break;
        }
        std::this_thread::yield();
    }
    if (audioInFlight_.load(std::memory_order_acquire) == 0) {
        std::unique_lock<std::mutex> pLock(processMutex_, std::try_to_lock);
        if (pLock.owns_lock() &&
            audioInFlight_.load(std::memory_order_acquire) == 0) {
            performResetUnderProcessLock();
        }
    }
}

bool WaveAudioPipeline::startOffer(std::int64_t nowUnixSeconds,
                                   std::int64_t ttlSeconds) {
    std::shared_ptr<IWaveTransport> tx;
    std::array<std::uint8_t, 16> newSid{};
    std::uint64_t nonce = 0;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (config_.role == WaveRole::Host && transport_) {
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
                    tx = transport_;
                }
            }
        }
    }
    if (config_.role != WaveRole::Host) {
        emitError(WaveErrorCode::InvalidState,
                  "Only Host role can start an offer");
        return false;
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

    requestOrPerformReset();
    setState(WaveSessionState::Offering, "starting offer");
    if (!tx->startOffer() || !hasLastLocalSignal_) {
        setState(WaveSessionState::P2PFailed, "failed to gather local offer");
        return false;
    }

    const std::int64_t ts = resolveTimestamp(nowUnixSeconds);
    const std::int64_t rawTtl =
        ttlSeconds > 0 ? ttlSeconds : config_.defaultOfferTtlSeconds;
    const std::int64_t ttl =
        std::clamp<std::int64_t>(rawTtl, 1, kWaveMaxSignalTtlSeconds);
    WaveSignalMessage msg;
    msg.version = kWaveSignalVersion;
    msg.type = WaveSignalType::Offer;
    msg.sid = newSid;
    msg.iat = ts;
    msg.exp = ts + ttl;
    msg.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp |
               kWaveCapExternalCallFallback;
    msg.nonce = nonce;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        msg.signal = lastLocalSignal_;
    }

    setState(WaveSessionState::AwaitingAnswer, "offer ready");
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
    std::string err;
    if (!validateWaveSignal(offer, nowUnixSeconds, &err) ||
        offer.type != WaveSignalType::Offer) {
        emitError(WaveErrorCode::SignalingFailed,
                  err.empty() ? "Invalid Offer message" : err);
        return false;
    }
    std::shared_ptr<IWaveTransport> tx;
    std::uint64_t nonce = 0;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        tx = transport_;
        if (tx) {
            {
                std::lock_guard<std::mutex> sLock(sidMutex_);
                sid_ = offer.sid;
            }
            hasLastLocalSignal_ = false;
            hasLastRemoteNonce_ = true;
            lastRemoteNonce_ = offer.nonce;
            nonce = nextNonce_++;
        }
    }
    if (!tx) {
        emitError(WaveErrorCode::TransportFailed, "No P2P transport configured");
        return false;
    }

    requestOrPerformReset();
    setState(WaveSessionState::ConnectingP2P, "accepting offer");
    if (!tx->acceptOffer(offer.signal)) {
        setState(WaveSessionState::P2PFailed, "failed to accept offer");
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
            ans.iat = resolveTimestamp(nowUnixSeconds);
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
    if (state() != WaveSessionState::AwaitingAnswer) {
        emitError(WaveErrorCode::InvalidState,
                  "Not awaiting an answer");
        return false;
    }
    std::string err;
    const auto curSid = sessionId();
    if (!validateWaveSignal(answer, nowUnixSeconds, &err) ||
        answer.type != WaveSignalType::Answer || answer.sid != curSid) {
        emitError(WaveErrorCode::SignalingFailed,
                  err.empty() ? "Invalid or mismatched Answer" : err);
        return false;
    }
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        hasLastRemoteNonce_ = true;
        lastRemoteNonce_ = answer.nonce;
        tx = transport_;
    }
    setState(WaveSessionState::ConnectingP2P, "applying answer");
    if (!tx || !tx->applyAnswer(answer.signal)) {
        setState(WaveSessionState::P2PFailed, "failed to apply answer");
        return false;
    }
    return true;
}

bool WaveAudioPipeline::handleRemoteSignal(const WaveSignalMessage& msg,
                                           std::int64_t nowUnixSeconds) {
    std::string err;
    if (!validateWaveSignal(msg, nowUnixSeconds, &err)) {
        emitError(WaveErrorCode::SignalingFailed, err);
        return false;
    }
    if (msg.type == WaveSignalType::Offer) {
        return acceptOffer(msg, nowUnixSeconds);
    }
    if (msg.type == WaveSignalType::Answer) {
        return acceptAnswer(msg, nowUnixSeconds);
    }
    std::shared_ptr<IWaveTransport> tx;
    std::shared_ptr<IExternalCallTransport> ext;
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
        ext = externalCall_;
    }
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
    if (state() != WaveSessionState::P2PFailed &&
        state() != WaveSessionState::Ended &&
        state() != WaveSessionState::Failed) {
        return false;
    }
    std::shared_ptr<IWaveTransport> tx;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
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
    bool shouldEmit = false;
    WaveSignalMessage endMsg;
    std::shared_ptr<IWaveTransport> tx;
    std::shared_ptr<IExternalCallTransport> ext;
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
            endMsg.version = kWaveSignalVersion;
            endMsg.type = (cur == WaveSessionState::Offering ||
                           cur == WaveSessionState::AwaitingAnswer)
                              ? WaveSignalType::Cancel
                              : WaveSignalType::End;
            endMsg.sid = sessionId();
            endMsg.iat = ts;
            endMsg.exp = ts + 60;
            endMsg.nonce = nextNonce_++;
        }
        tx = transport_;
        ext = externalCall_;
    }
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
    requestOrPerformReset();
}

void WaveAudioPipeline::reset() {
    std::shared_ptr<IWaveTransport> tx;
    std::shared_ptr<IExternalCallTransport> ext;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        tx = transport_;
        ext = externalCall_;
    }
    setState(WaveSessionState::Idle, "reset");
    if (tx) {
        tx->close();
    }
    if (ext) {
        ext->stop();
    }
    requestOrPerformReset();
    {
        std::lock_guard<std::mutex> sLock(sidMutex_);
        sid_.fill(0);
    }
}

bool WaveAudioPipeline::switchToExternalCallFallback(
    std::shared_ptr<IExternalCallTransport> externalCall) {
    if (state() != WaveSessionState::P2PFailed) {
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
        std::lock_guard<std::mutex> lock(controlMutex_);
        externalCall_ = externalCall;
    }
    ExternalCallCallbacks cb;
    cb.onStateChanged = [this](ExternalCallState extState,
                               const std::string& reason) {
        switch (extState) {
        case ExternalCallState::Connecting:
            setState(WaveSessionState::ConnectingTelegramCall, reason);
            break;
        case ExternalCallState::Live:
            setState(WaveSessionState::LiveTelegramCall, reason);
            break;
        case ExternalCallState::Failed:
            setState(WaveSessionState::Failed, reason);
            emitError(WaveErrorCode::FallbackFailed, reason);
            break;
        case ExternalCallState::Ended:
            setState(WaveSessionState::Ended, reason);
            break;
        default:
            break;
        }
    };
    externalCall->setCallbacks(std::move(cb));
    setState(WaveSessionState::OfferingTelegramFallback,
             "offering external call fallback");
    setState(WaveSessionState::ConnectingTelegramCall,
             "connecting external call fallback");
    if (!externalCall->start()) {
        setState(WaveSessionState::Failed, "external call start failed");
        emitError(WaveErrorCode::FallbackFailed,
                  "External call transport failed to start");
        return false;
    }
    return true;
}

void WaveAudioPipeline::setPublishingEnabled(bool enabled) noexcept {
    publishingEnabled_.store(enabled, std::memory_order_release);
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
    if (audioPausedForReset_.load(std::memory_order_acquire) ||
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
    if (audioPausedForReset_.load(std::memory_order_acquire)) {
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
    std::lock_guard<std::mutex> pLock(processMutex_);
    if (pendingRingReset_.load(std::memory_order_acquire) &&
        audioInFlight_.load(std::memory_order_acquire) == 0) {
        performResetUnderProcessLock();
    }
    if (transport_) {
        transport_->poll(currentTimeSeconds);
    }
    if (config_.role == WaveRole::Host) {
        processHostStep(currentTimeSeconds);
    } else {
        processListenerStep(currentTimeSeconds);
    }
    publishStatsSnapshot();
}

void WaveAudioPipeline::processHostStep(double currentTimeSeconds) {
    const WaveTransportRoute r = route_.load(std::memory_order_acquire);
    if (r == WaveTransportRoute::None ||
        !publishingEnabled_.load(std::memory_order_acquire)) {
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

    if (r == WaveTransportRoute::ExternalCall && externalCall_) {
        externalCall_->pushOutgoingPcm(hostResampled_.data(),
                                       hostResampled_.size() / 2,
                                       kWaveSampleRate);
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
            pkt.sequence = nextSequence_++;
            pkt.timestamp48k = nextTimestamp48k_;
            pkt.frameSamples = static_cast<std::uint16_t>(frameSamples);
            pkt.flags = kWavePacketNone;
            pkt.payload = encodedPayload_;
            if (transport_->sendMediaPacket(pkt)) {
                ++packetsSent_;
                bytesSent_ += pkt.payload.size();
            }
            nextTimestamp48k_ += static_cast<std::uint32_t>(frameSamples);
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
    if (r == WaveTransportRoute::ExternalCall && externalCall_) {
        const std::size_t wantFrames = kWaveFrameSamples;
        listenerDecodeBuf_.resize(wantFrames * 2);
        const std::size_t got = externalCall_->pullIncomingPcm(
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
        return;
    }

    if (r != WaveTransportRoute::P2P) {
        return;
    }

    const std::uint32_t outRate =
        listenerOutputRate_.load(std::memory_order_acquire);
    listenerResampler_.configure(kWaveSampleRate, outRate);

    WaveMediaPacket pkt;
    while (true) {
        const auto status = jitterBuffer_.popNext(pkt);
        if (status == WaveJitterBuffer::PopStatus::Buffering) {
            break;
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

    (void)currentTimeSeconds;
}

void WaveAudioPipeline::publishStatsSnapshot() {
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
    std::lock_guard<std::mutex> sLock(statsMutex_);
    publishedStats_ = s;
}

std::array<std::uint8_t, 16> WaveAudioPipeline::sessionId() const noexcept {
    std::lock_guard<std::mutex> lock(sidMutex_);
    return sid_;
}

WaveStats WaveAudioPipeline::stats() const {
    WaveStats s;
    {
        std::lock_guard<std::mutex> sLock(statsMutex_);
        s = publishedStats_;
    }
    s.ringOverruns = hostRing_.overruns() + listenerRing_.overruns();
    s.ringUnderruns = hostRing_.underruns() + listenerRing_.underruns();
    s.clippedSamples =
        hostRing_.clippedSamples() + listenerRing_.clippedSamples();
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
