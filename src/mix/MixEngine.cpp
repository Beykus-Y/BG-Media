#include "bgmedia/mix/MixEngine.hpp"

#include "bgmedia/mix/Crossfade.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace bgmedia::mix {
namespace {

constexpr std::size_t kChunkFrames = 2048;
constexpr double kDeclickSeconds = 0.008;
constexpr double kCancelSeconds = 0.220;

float sanitize(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

// Control-thread sample-rate conversion. WSOLA copies source windows without
// resampling them, so a track recorded at a different rate must be normalized
// before it reaches the stretcher; otherwise the pitch would be wrong even at
// playback rate 1.0. Catmull-Rom keeps the conversion clean enough for music.
std::shared_ptr<const PcmTrack> resampleTrack(
    const std::shared_ptr<const PcmTrack>& source, std::uint32_t targetRate) {
    if (!source || targetRate == 0 || source->sampleRate == targetRate) {
        return source;
    }
    auto converted = std::make_shared<PcmTrack>();
    converted->sampleRate = targetRate;
    converted->channels = source->channels;
    const double ratio =
        static_cast<double>(targetRate) / static_cast<double>(source->sampleRate);
    const std::size_t sourceFrames = source->frameCount();
    const std::size_t outFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(
               static_cast<double>(sourceFrames) * ratio)));
    converted->samples.resize(outFrames * source->channels);
    const std::size_t last = sourceFrames > 0 ? sourceFrames - 1 : 0;
    auto at = [&](std::size_t frame, std::uint32_t channel) {
        const std::size_t clamped = std::min(frame, last);
        return sanitize(source->samples[clamped * source->channels + channel]);
    };
    for (std::size_t i = 0; i < outFrames; ++i) {
        const double position =
            static_cast<double>(i) / std::max(1e-9, ratio);
        const auto index = static_cast<std::size_t>(position);
        const float frac = static_cast<float>(position - static_cast<double>(index));
        for (std::uint32_t c = 0; c < source->channels; ++c) {
            const float p0 = at(index > 0 ? index - 1 : 0, c);
            const float p1 = at(index, c);
            const float p2 = at(index + 1, c);
            const float p3 = at(index + 2, c);
            const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
            const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
            const float cc = -0.5f * p0 + 0.5f * p2;
            const float value = ((a * frac + b) * frac + cc) * frac + p1;
            converted->samples[i * source->channels + c] = sanitize(value);
        }
    }
    return converted;
}

} // namespace

MixEngine::MixEngine(std::uint32_t outputSampleRate)
    : outputSampleRate_(outputSampleRate ? outputSampleRate : 48000),
      stretchOwned_(std::make_shared<WsolaStretcher>()) {
    stretch_.store(stretchOwned_.get(), std::memory_order_release);
    rebuildStretchStreams();
    MixSnapshot initial;
    initial.sampleRate = outputSampleRate_;
    snapshotBuffers_[0] = initial;
    snapshotBuffers_[1] = initial;
    scratchA_.resize(kChunkFrames * 2);
    scratchB_.resize(kChunkFrames * 2);
    for (int i = 0; i < 2; ++i) {
        decks_[i].owned.reset();
        decks_[i].audio.store(nullptr, std::memory_order_relaxed);
        decks_[i].eq.configure(outputSampleRate_);
    }
}

MixEngine::~MixEngine() = default;

bool MixEngine::loadAllowed(DeckId deck) const noexcept {
    // A deck may only be (re)loaded while the mixer is idle and that deck is
    // not producing sound. This keeps the render callback free of shared
    // pointer churn for buffers it is currently reading.
    if (decks_[deckIndex(deck)].targetPlay.load(std::memory_order_acquire)) {
        return false;
    }
    return phase_.load(std::memory_order_acquire) ==
           static_cast<int>(Phase::Idle);
}

bool MixEngine::loadDeck(DeckId deck, std::shared_ptr<const PcmTrack> track,
                         std::uint64_t trackId, double analysedBpm) {
    if (!track || !track->valid() || !loadAllowed(deck)) {
        return false;
    }
    DeckRuntime& runtime = decks_[deckIndex(deck)];
    // Normalize to the engine rate on the control thread before publishing.
    std::shared_ptr<const PcmTrack> prepared =
        resampleTrack(track, outputSampleRate_);
    // Ownership is handed over on the control thread, but the old buffer stays
    // in the retire list until the callback heartbeat proves that no in-flight
    // render can still hold a raw pointer to it.
    std::shared_ptr<const PcmTrack> previous = std::move(runtime.owned);
    runtime.owned = std::move(prepared);
    runtime.audio.store(runtime.owned.get(), std::memory_order_release);
    retireTrack(deck, std::move(previous));
    runtime.trackId.store(trackId, std::memory_order_release);
    runtime.analysedBpm.store(std::max(0.0, analysedBpm),
                              std::memory_order_release);
    runtime.targetPlay.store(false, std::memory_order_release);
    runtime.seekSeconds.store(0.0, std::memory_order_release);
    runtime.seekSerial.fetch_add(1, std::memory_order_acq_rel);
    // The callback owns position/playing/fade/gain. Ask it to reset them; the
    // control thread must not write those fields concurrently.
    runtime.resetSerial.fetch_add(1, std::memory_order_acq_rel);
    reclaimRetired();
    return true;
}

void MixEngine::clearDeck(DeckId deck) {
    if (!loadAllowed(deck)) {
        return;
    }
    DeckRuntime& runtime = decks_[deckIndex(deck)];
    runtime.audio.store(nullptr, std::memory_order_release);
    std::shared_ptr<const PcmTrack> previous = std::move(runtime.owned);
    retireTrack(deck, std::move(previous));
    runtime.trackId.store(0, std::memory_order_release);
    runtime.analysedBpm.store(0.0, std::memory_order_release);
    runtime.targetPlay.store(false, std::memory_order_release);
    runtime.resetSerial.fetch_add(1, std::memory_order_acq_rel);
    reclaimRetired();
}

void MixEngine::retireTrack(DeckId deck,
                            std::shared_ptr<const PcmTrack> owner) {
    if (!owner) {
        return;
    }
    // +1 guarantees that the callback which could have observed the old atomic
    // pointer has returned before this object may be destroyed.
    const std::uint64_t epoch =
        renderEpoch_.load(std::memory_order_acquire) + 1;
    retiredTracks_[deckIndex(deck)].push_back(
        RetiredTrack{std::move(owner), epoch});
}

void MixEngine::retireProcessor(
    std::shared_ptr<const ITimeStretchProcessor> owner) {
    if (!owner) {
        return;
    }
    const std::uint64_t epoch =
        renderEpoch_.load(std::memory_order_acquire) + 1;
    retiredProcessors_.push_back(RetiredProcessor{std::move(owner), epoch});
}

void MixEngine::retireStream(std::shared_ptr<ITimeStretchStream> owner) {
    if (!owner) {
        return;
    }
    const std::uint64_t epoch =
        renderEpoch_.load(std::memory_order_acquire) + 1;
    retiredStreams_.push_back(RetiredStream{std::move(owner), epoch});
}

void MixEngine::rebuildStretchStreams() {
    for (int i = 0; i < 2; ++i) {
        std::shared_ptr<ITimeStretchStream> previous =
            std::move(streamOwned_[i]);
        streamOwned_[i] = stretchOwned_->createStream(outputSampleRate_);
        // Reset before publication. Once the callback can observe the pointer,
        // its mutable ring/overlap state belongs exclusively to the audio
        // thread; resetting afterwards would race with render().
        if (streamOwned_[i]) {
            streamOwned_[i]->reset(0.0);
        }
        stream_[i].store(streamOwned_[i].get(), std::memory_order_release);
        retireStream(std::move(previous));
    }
    reclaimRetired();
}

void MixEngine::resetStretchStream(DeckId deck,
                                   double positionFrames) noexcept {
    ITimeStretchStream* stream =
        stream_[deckIndex(deck)].load(std::memory_order_acquire);
    if (stream) {
        stream->reset(positionFrames);
    }
}

void MixEngine::reclaimRetired() {
    const std::uint64_t epoch = renderEpoch_.load(std::memory_order_acquire);
    for (auto& retired : retiredTracks_) {
        retired.erase(
            std::remove_if(retired.begin(), retired.end(),
                           [epoch](const RetiredTrack& entry) {
                               return entry.epoch <= epoch;
                           }),
            retired.end());
    }
    retiredProcessors_.erase(
        std::remove_if(retiredProcessors_.begin(), retiredProcessors_.end(),
                       [epoch](const RetiredProcessor& entry) {
                           return entry.epoch <= epoch;
                       }),
        retiredProcessors_.end());
    retiredStreams_.erase(
        std::remove_if(retiredStreams_.begin(), retiredStreams_.end(),
                       [epoch](const RetiredStream& entry) {
                           return entry.epoch <= epoch;
                       }),
        retiredStreams_.end());
}


bool MixEngine::deckLoaded(DeckId deck) const noexcept {
    return decks_[deckIndex(deck)].audio.load(std::memory_order_acquire) !=
           nullptr;
}

double MixEngine::deckDuration(DeckId deck) const noexcept {
    const PcmTrack* track =
        decks_[deckIndex(deck)].audio.load(std::memory_order_acquire);
    return track ? track->duration() : 0.0;
}

void MixEngine::play(DeckId deck) {
    decks_[deckIndex(deck)].targetPlay.store(true, std::memory_order_release);
}

void MixEngine::pause(DeckId deck) {
    decks_[deckIndex(deck)].targetPlay.store(false, std::memory_order_release);
}

void MixEngine::seek(DeckId deck, double seconds) {
    DeckRuntime& runtime = decks_[deckIndex(deck)];
    runtime.seekSeconds.store(std::max(0.0, seconds), std::memory_order_release);
    runtime.seekSerial.fetch_add(1, std::memory_order_acq_rel);
}

void MixEngine::setDeckGain(DeckId deck, float gain) {
    decks_[deckIndex(deck)].userGain.store(std::clamp(gain, 0.0f, 2.0f),
                                           std::memory_order_release);
}

void MixEngine::setDeckRate(DeckId deck, double rate) {
    decks_[deckIndex(deck)].requestedRate.store(
        std::clamp(rate, 0.25, 2.0), std::memory_order_release);
}

void MixEngine::setMaster(DeckId deck) {
    master_.store(deckIndex(deck), std::memory_order_release);
}

bool MixEngine::startTransition(const MixPlan& plan) {
    if (!plan.valid) {
        return false;
    }
    if (phase_.load(std::memory_order_acquire) !=
        static_cast<int>(Phase::Idle)) {
        return false;
    }
    if (planReady_.load(std::memory_order_acquire)) {
        return false;
    }
    if (plan.from == plan.to || !deckLoaded(plan.to) || !deckLoaded(plan.from)) {
        return false;
    }
    pendingPlan_ = plan;
    planReady_.store(true, std::memory_order_release);
    return true;
}

void MixEngine::cancelTransition() {
    if (phase_.load(std::memory_order_acquire) ==
        static_cast<int>(Phase::Running)) {
        cancelRequested_.store(true, std::memory_order_release);
    }
}

bool MixEngine::transitionActive() const noexcept {
    return phase_.load(std::memory_order_acquire) !=
           static_cast<int>(Phase::Idle);
}

void MixEngine::setPaused(bool paused) {
    paused_.store(paused, std::memory_order_release);
}

bool MixEngine::paused() const noexcept {
    return paused_.load(std::memory_order_acquire);
}

void MixEngine::stop() {
    paused_.store(true, std::memory_order_release);
    for (DeckRuntime& deck : decks_) {
        deck.targetPlay.store(false, std::memory_order_release);
    }
    stopRequested_.store(true, std::memory_order_release);
}

void MixEngine::setOutputLatency(double seconds) {
    outputLatency_.store(std::max(0.0, seconds), std::memory_order_release);
}

void MixEngine::setTimeStretch(
    std::shared_ptr<const ITimeStretchProcessor> processor) {
    std::shared_ptr<const ITimeStretchProcessor> previous =
        std::move(stretchOwned_);
    stretchOwned_ = processor ? std::move(processor)
                              : std::make_shared<WsolaStretcher>();
    stretch_.store(stretchOwned_.get(), std::memory_order_release);
    rebuildStretchStreams();
    retireProcessor(std::move(previous));
    reclaimRetired();
}

const ITimeStretchProcessor& MixEngine::timeStretch() const noexcept {
    return *stretch_.load(std::memory_order_acquire);
}

std::uint64_t MixEngine::completedTransitions() const noexcept {
    return completed_.load(std::memory_order_acquire);
}

double MixEngine::planRate(double elapsed) const noexcept {
    const RuntimePlan& plan = runtime_;
    if (plan.tempoCount <= 0) {
        return plan.rateStart;
    }
    if (elapsed <= plan.tempo[0].time) {
        return plan.tempo[0].rate;
    }
    for (int i = 1; i < plan.tempoCount; ++i) {
        if (elapsed <= plan.tempo[i].time) {
            const TempoPoint& a = plan.tempo[i - 1];
            const TempoPoint& b = plan.tempo[i];
            const double span = std::max(1e-9, b.time - a.time);
            const double t = std::clamp((elapsed - a.time) / span, 0.0, 1.0);
            return a.rate + (b.rate - a.rate) * t;
        }
    }
    return plan.tempo[plan.tempoCount - 1].rate;
}

EqGains MixEngine::planEq(const EqGainPoint* points, int count,
                          double elapsed) const noexcept {
    EqGains gains;
    if (count <= 0) {
        return gains;
    }
    if (elapsed <= points[0].time) {
        gains.low = points[0].low;
        gains.mid = points[0].mid;
        gains.high = points[0].high;
        return gains;
    }
    for (int i = 1; i < count; ++i) {
        if (elapsed <= points[i].time) {
            const EqGainPoint& a = points[i - 1];
            const EqGainPoint& b = points[i];
            const double span = std::max(1e-9, b.time - a.time);
            const float t = static_cast<float>(
                std::clamp((elapsed - a.time) / span, 0.0, 1.0));
            gains.low = a.low + (b.low - a.low) * t;
            gains.mid = a.mid + (b.mid - a.mid) * t;
            gains.high = a.high + (b.high - a.high) * t;
            return gains;
        }
    }
    const EqGainPoint& last = points[count - 1];
    gains.low = last.low;
    gains.mid = last.mid;
    gains.high = last.high;
    return gains;
}

TransitionStage MixEngine::liveStage() const noexcept {
    const int phase = phase_.load(std::memory_order_acquire);
    if (phase == static_cast<int>(Phase::Cancelling)) {
        return TransitionStage::Cancelled;
    }
    if (phase == static_cast<int>(Phase::Running)) {
        if (framesRendered_ < runtime_.rampStartFrame) {
            return TransitionStage::Preparing;
        }
        if (framesRendered_ < runtime_.crossfadeStartFrame) {
            const double remaining =
                static_cast<double>(runtime_.crossfadeStartFrame - framesRendered_) /
                outputSampleRate_;
            if (!runtime_.tempoRamp) {
                return TransitionStage::Preparing;
            }
            return remaining > 1.0 ? TransitionStage::TempoRamp
                                   : TransitionStage::BeatAlign;
        }
        if (framesRendered_ >= runtime_.endFrame) {
            return TransitionStage::Complete;
        }
        const double span = static_cast<double>(std::max<std::uint64_t>(
            1, runtime_.endFrame - runtime_.crossfadeStartFrame));
        const double progress = std::clamp(
            static_cast<double>(framesRendered_ - runtime_.crossfadeStartFrame) /
                span,
            0.0, 1.0);
        return progress < runtime_.handoverProgress ? TransitionStage::Mixing
                                                    : TransitionStage::Handover;
    }
    const std::uint64_t recent = outputSampleRate_ / 2;
    if (everCancelled_ &&
        framesRendered_ - lastCancelFrame_ < recent) {
        return TransitionStage::Cancelled;
    }
    if (everCompleted_ &&
        framesRendered_ - lastCompletionFrame_ < outputSampleRate_) {
        return TransitionStage::Complete;
    }
    return TransitionStage::Idle;
}

void MixEngine::consumeStartRequest(std::uint64_t frame) noexcept {
    if (!planReady_.load(std::memory_order_acquire)) {
        return;
    }
    const MixPlan& plan = pendingPlan_;
    runtime_ = {};
    runtime_.valid = plan.valid;
    runtime_.type = plan.type;
    runtime_.from = plan.from;
    runtime_.to = plan.to;
    runtime_.startB = plan.startB;
    runtime_.handoverProgress = std::clamp(plan.handoverProgress, 0.0, 1.0);
    runtime_.rateReleaseProgress =
        std::clamp(plan.rateReleaseProgress, 0.0, 1.0);
    runtime_.directCut = plan.type == TransitionType::DirectCut;
    runtime_.bassSwap = plan.bassSwap;
    runtime_.pitchLock = plan.pitchLock;
    runtime_.beatPhaseErrorMs = plan.beatPhaseErrorMs;
    runtime_.tempoRatioB =
        std::clamp(plan.tempoRatioB, 0.75, 1.33);

    const double sampleRate = static_cast<double>(outputSampleRate_);
    const double seconds = std::max(0.0, plan.duration);
    const double preSeconds =
        runtime_.directCut ? 0.0 : std::max(0.0, plan.preTransitionSeconds);

    DeckRuntime& from = decks_[deckIndex(runtime_.from)];
    DeckRuntime& to = decks_[deckIndex(runtime_.to)];
    const bool fromWasPlaying = from.playing;
    const PcmTrack* fromSource = from.audio.load(std::memory_order_acquire);
    double cueA = runtime_.directCut ? plan.transitionStartA
                                     : plan.overallStartA();
    const double currentMedia =
        fromSource && fromSource->sampleRate > 0
            ? from.positionFrames / static_cast<double>(fromSource->sampleRate)
            : 0.0;

    // A playing deck is never seeked backwards. If the planner selected a
    // future musical event, wait for it; if it selected a point slightly
    // behind the current cursor, start the ramp here instead of repeating
    // audio. The crossfade boundary shifts with the actual ramp start.
    std::uint64_t rampStartFrame = frame;
    if (fromWasPlaying) {
        if (cueA < currentMedia - 1e-6) {
            cueA = currentMedia;
        }
        const double delaySeconds =
            std::max(0.0, cueA - currentMedia) /
            std::max(0.25, from.requestedRate.load(std::memory_order_acquire));
        rampStartFrame += static_cast<std::uint64_t>(
            std::llround(delaySeconds * sampleRate));
    }
    runtime_.startFrame = rampStartFrame;
    runtime_.rampStartFrame = rampStartFrame;
    runtime_.crossfadeStartFrame =
        rampStartFrame +
        static_cast<std::uint64_t>(std::llround(preSeconds * sampleRate));
    runtime_.endFrame =
        runtime_.crossfadeStartFrame +
        static_cast<std::uint64_t>(std::llround(seconds * sampleRate));
    runtime_.tempoRamp = preSeconds > 0.0 && plan.tempoRampActive;
    runtime_.rateStart = 1.0;
    runtime_.rateEnd = 1.0;
    runtime_.tempoCount = std::min<int>(
        static_cast<int>(plan.tempoCurve.size()), 128);
    for (int i = 0; i < runtime_.tempoCount; ++i) {
        runtime_.tempo[i] = plan.tempoCurve[static_cast<std::size_t>(i)];
    }
    if (runtime_.tempoCount > 0) {
        runtime_.rateStart = runtime_.tempo[0].rate;
        runtime_.rateEnd = runtime_.tempo[runtime_.tempoCount - 1].rate;
    }
    runtime_.eqOutCount =
        std::min<int>(static_cast<int>(plan.eqCurveOut.size()), 64);
    for (int i = 0; i < runtime_.eqOutCount; ++i) {
        runtime_.eqOut[i] = plan.eqCurveOut[static_cast<std::size_t>(i)];
    }
    runtime_.eqInCount =
        std::min<int>(static_cast<int>(plan.eqCurveIn.size()), 64);
    for (int i = 0; i < runtime_.eqInCount; ++i) {
        runtime_.eqIn[i] = plan.eqCurveIn[static_cast<std::size_t>(i)];
    }

    // A playing deck keeps its cursor, WSOLA history and EQ state. A stopped
    // deck is cued to the planned point. This is what makes NEXT/auto-start
    // continuous instead of a hidden rewind.
    if (!fromWasPlaying) {
        double targetFromFrames = from.positionFrames;
        if (fromSource) {
            targetFromFrames = std::clamp(cueA, 0.0, fromSource->duration()) *
                               fromSource->sampleRate;
        }
        from.positionFrames = targetFromFrames;
        resetStretchStream(runtime_.from, from.positionFrames);
        from.eq.reset();
    }
    from.playing = true;
    from.ended = false;
    from.fade = fromWasPlaying ? 1.0f : 0.0f;
    from.rate = 1.0;
    from.rampRate = 1.0;
    // The incoming deck is prepared but stays stopped until the crossfade
    // boundary so its first beat lands exactly on the overlay start.
    to.positionFrames = plan.startB;
    if (const PcmTrack* source = to.audio.load(std::memory_order_acquire)) {
        to.positionFrames =
            std::clamp(plan.startB, 0.0, source->duration()) * source->sampleRate;
    }
    to.playing = false;
    to.ended = false;
    to.fade = 0.0f;
    to.rate = 1.0;
    to.rampRate = 1.0;
    to.eq.reset();
    to.targetPlay.store(false, std::memory_order_release);
    from.targetPlay.store(true, std::memory_order_release);
    resetStretchStream(runtime_.to, to.positionFrames);
    incomingStarted_ = false;

    transitionStartFrame_ = runtime_.crossfadeStartFrame;
    transitionEndFrame_ = runtime_.endFrame;
    handoverDone_ = false;
    planReady_.store(false, std::memory_order_release);

    if (runtime_.directCut || runtime_.endFrame <= frame) {
        from.targetPlay.store(false, std::memory_order_release);
        from.playing = false;
        from.gain = 0.0f;
        to.playing = true;
        to.ended = false;
        to.targetPlay.store(true, std::memory_order_release);
        to.gain = 1.0f;
        to.fade = 1.0f;
        master_.store(deckIndex(runtime_.to), std::memory_order_release);
        runtimeStarted_ = false;
        lastCompletionFrame_ = framesRendered_;
        everCompleted_ = true;
        completed_.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    runtimeStarted_ = true;
    phase_.store(static_cast<int>(Phase::Running), std::memory_order_release);
}

void MixEngine::render(float* output, std::size_t frames) noexcept {
    if (!output) {
        return;
    }
    renderEpoch_.fetch_add(1, std::memory_order_acq_rel);
    std::memset(output, 0, frames * 2 * sizeof(float));
    if (frames == 0) {
        publishSnapshot();
        return;
    }

    // A hard stop cancels an in-flight or pending transition before the pause
    // freeze. It also drops a plan that was published but not consumed yet.
    // The plan payload itself stays control-thread-owned: the callback only
    // clears the ready flag. Clearing pendingPlan_ here would race with
    // startTransition() overwriting it after it observed Phase::Idle.
    if (stopRequested_.exchange(false, std::memory_order_acq_rel)) {
        planReady_.store(false, std::memory_order_release);
        cancelRequested_.store(false, std::memory_order_release);
        phase_.store(static_cast<int>(Phase::Idle), std::memory_order_release);
        runtimeStarted_ = false;
        handoverDone_ = false;
    }

    // loadDeck()/clearDeck() publish a reset serial instead of writing the
    // callback-owned transport fields. Apply it first so a newly installed
    // track can never inherit the previous track's position or playing state.
    for (int i = 0; i < 2; ++i) {
        DeckRuntime& deck = decks_[i];
        const std::uint64_t serial = deck.resetSerial.load(std::memory_order_acquire);
        if (serial != deck.resetApplied) {
            deck.resetApplied = serial;
            deck.positionFrames = 0.0;
            deck.rate = 1.0;
            deck.rampRate = 1.0;
            deck.playing = false;
            deck.ended = false;
            deck.fade = 0.0f;
            deck.gain = 0.0f;
            deck.wasTargetPlaying = false;
            resetStretchStream(i == 0 ? DeckId::A : DeckId::B, 0.0);
        }
    }

    // Apply pending seeks BEFORE consuming a transition plan. A deck loaded in
    // this same callback still has the zero seek published by loadDeck(); if
    // that loop ran after consumeStartRequest() it would overwrite the cue
    // positions selected by the plan.
    for (int i = 0; i < 2; ++i) {
        DeckRuntime& deck = decks_[i];
        const std::uint64_t serial = deck.seekSerial.load(std::memory_order_acquire);
        if (serial != deck.seekApplied) {
            deck.seekApplied = serial;
            if (const PcmTrack* source = deck.audio.load(std::memory_order_acquire)) {
                const double seconds =
                    deck.seekSeconds.load(std::memory_order_acquire);
                deck.positionFrames =
                    std::clamp(seconds, 0.0, source->duration()) *
                    source->sampleRate;
            }
            deck.ended = false;
            deck.fade = 0.0f;
            deck.playing = deck.targetPlay.load(std::memory_order_acquire);
            resetStretchStream(i == 0 ? DeckId::A : DeckId::B,
                               deck.positionFrames);
        }
    }

    // A pending start is consumed at a block boundary so both decks begin on
    // exactly the same output frame. Plan markers win over load-time seeks.
    if (planReady_.load(std::memory_order_acquire)) {
        consumeStartRequest(framesRendered_);
    }
    if (cancelRequested_.exchange(false, std::memory_order_acq_rel) &&
        phase_.load(std::memory_order_acquire) ==
            static_cast<int>(Phase::Running)) {
        phase_.store(static_cast<int>(Phase::Cancelling),
                     std::memory_order_release);
        cancelStartFrame_ = framesRendered_;
        const double span = std::max(1.0, static_cast<double>(
                                                  transitionEndFrame_ -
                                                  transitionStartFrame_));
        cancelStartProgress_ = std::clamp(
            (static_cast<double>(framesRendered_) -
             static_cast<double>(transitionStartFrame_)) /
                span,
            0.0, 1.0);
        // A cancel can land during the tempo pre-roll, where the crossfade
        // progress is still zero. Capture the live ramp rate so the outgoing
        // deck glides back to 1.0 instead of jumping.
        const double elapsed =
            (static_cast<double>(framesRendered_) -
             static_cast<double>(transitionStartFrame_)) /
            outputSampleRate_;
        cancelRateStart_ = std::clamp(planRate(elapsed), 0.25, 2.0);
        cancelEqOutStart_ = planEq(runtime_.eqOut, runtime_.eqOutCount, elapsed);
        cancelEqInStart_ = planEq(runtime_.eqIn, runtime_.eqInCount, elapsed);
        cancelIncomingStarted_ = incomingStarted_;
    }

    // A play request on an empty deck is a no-op. Without this the deck would
    // stay "playing" forever, and loadAllowed() would reject every later load.
    for (DeckRuntime& deck : decks_) {
        if (deck.targetPlay.load(std::memory_order_acquire) &&
            deck.audio.load(std::memory_order_acquire) == nullptr) {
            deck.targetPlay.store(false, std::memory_order_release);
            deck.playing = false;
            deck.gain = 0.0f;
        }
    }

    const bool paused = paused_.load(std::memory_order_acquire);
    const float pauseStep =
        1.0f / static_cast<float>(outputSampleRate_ * kDeclickSeconds);
    // Already silent: freeze the transport without rendering another block.
    // The envelope is applied per sample below, so a pause that starts inside
    // this block still renders its 8 ms de-click ramp instead of muting the
    // whole callback and stepping from full amplitude to zero.
    if (paused && pauseFade_ <= 0.0f) {
        // Freeze the transport only after stop/pause requests have settled the
        // deck flags. Otherwise a stopped deck would report "playing" forever
        // and Clear Queue could never release it.
        for (DeckRuntime& deck : decks_) {
            deck.gain = 0.0f;
            if (!deck.targetPlay.load(std::memory_order_acquire)) {
                deck.playing = false;
                deck.fade = 0.0f;
            }
        }
        publishSnapshot();
        return;
    }
    float pauseEnvelope = pauseFade_;
    const float pauseDelta = paused ? -pauseStep : pauseStep;

    for (DeckRuntime& deck : decks_) {
        const bool target = deck.targetPlay.load(std::memory_order_acquire);
        if (target && !deck.playing && !deck.ended) {
            deck.playing = true;
            deck.ended = false;
            if (deck.positionFrames <= 0.0) {
                deck.fade = 0.0f;
            }
        }
        if (!target && deck.playing && deck.fade <= 0.0f) {
            deck.playing = false;
        }
    }

    std::size_t done = 0;
    while (done < frames) {
        std::size_t chunk = std::min(kChunkFrames, frames - done);
        int phase = phase_.load(std::memory_order_acquire);
        bool running = phase == static_cast<int>(Phase::Running);
        bool cancelling = phase == static_cast<int>(Phase::Cancelling);

        // Never let a chunk straddle the pre-roll/crossfade boundary: the
        // incoming deck must become audible on exactly the planned frame.
        if (running && framesRendered_ < runtime_.rampStartFrame) {
            chunk = std::min<std::size_t>(
                chunk, static_cast<std::size_t>(runtime_.rampStartFrame -
                                                framesRendered_));
        }
        if (running && framesRendered_ < transitionStartFrame_) {
            chunk = std::min<std::size_t>(
                chunk, static_cast<std::size_t>(transitionStartFrame_ -
                                                framesRendered_));
        }
        // The crossfade boundary itself: start the incoming deck here.
        if (running && !incomingStarted_ &&
            framesRendered_ >= transitionStartFrame_) {
            DeckRuntime& incoming = decks_[deckIndex(runtime_.to)];
            incoming.playing = true;
            incoming.ended = false;
            incoming.fade = 0.0f;
            incoming.rate = 1.0;
            incoming.rampRate = 1.0;
            if (const PcmTrack* source =
                    incoming.audio.load(std::memory_order_acquire)) {
                incoming.positionFrames =
                    std::clamp(runtime_.startB, 0.0, source->duration()) *
                    source->sampleRate;
            }
            incoming.targetPlay.store(true, std::memory_order_release);
            resetStretchStream(runtime_.to, incoming.positionFrames);
            incomingStarted_ = true;
        }

        double progress0 = 1.0;
        double progress1 = 1.0;
        if (running || cancelling) {
            const double span = static_cast<double>(
                std::max<std::uint64_t>(1, transitionEndFrame_ - transitionStartFrame_));
            // Cast before subtracting: during the pre-roll framesRendered_ is
            // smaller than transitionStartFrame_, and unsigned arithmetic would
            // wrap to a huge positive value and complete the transition.
            const double start = static_cast<double>(transitionStartFrame_);
            const double now = static_cast<double>(framesRendered_);
            progress0 = std::clamp((now - start) / span, 0.0, 1.0);
            progress1 =
                std::clamp((now + static_cast<double>(chunk) - start) / span,
                           0.0, 1.0);
        }

        float gainFrom0 = 0.0f;
        float gainFrom1 = 0.0f;
        float gainTo0 = 0.0f;
        float gainTo1 = 0.0f;
        double rateFrom0 = 1.0;
        double rateFrom1 = 1.0;
        double rateTo = 1.0;
        EqGains eqFrom0{};
        EqGains eqFrom1{};
        EqGains eqTo0{};
        EqGains eqTo1{};
        DeckId fromDeck = DeckId::A;
        DeckId toDeck = DeckId::B;

        if (running) {
            fromDeck = runtime_.from;
            toDeck = runtime_.to;
            gainFrom0 = equalPowerOut(progress0);
            gainFrom1 = equalPowerOut(progress1);
            gainTo0 = equalPowerIn(progress0);
            gainTo1 = equalPowerIn(progress1);
            const double elapsed0 =
                (static_cast<double>(framesRendered_) -
                 static_cast<double>(transitionStartFrame_)) /
                outputSampleRate_;
            const double elapsed1 =
                (static_cast<double>(framesRendered_) +
                 static_cast<double>(chunk) -
                 static_cast<double>(transitionStartFrame_)) /
                outputSampleRate_;
            if (framesRendered_ < runtime_.rampStartFrame) {
                // Waiting for a future musical event: keep the deck's current
                // playback rate, not the transition automation's first point.
                const double waitRate =
                    decks_[deckIndex(fromDeck)].requestedRate.load(
                        std::memory_order_acquire);
                rateFrom0 = waitRate;
                rateFrom1 = waitRate;
            } else {
                rateFrom0 = planRate(elapsed0);
                rateFrom1 = planRate(elapsed1);
            }
            // The incoming deck holds the matched tempo while both decks are
            // audible (so the beat grids stay locked), then returns to natural
            // speed across the very tail of the fade so the end of the
            // transition has no rate step.
            double ratioB = runtime_.tempoRatioB;
            if (progress0 >= runtime_.rateReleaseProgress && progress0 < 1.0) {
                const double span =
                    std::max(1e-9, 1.0 - runtime_.rateReleaseProgress);
                const double t = std::clamp(
                    (progress0 - runtime_.rateReleaseProgress) / span, 0.0, 1.0);
                ratioB = runtime_.tempoRatioB +
                         (1.0 - runtime_.tempoRatioB) * t;
            }
            rateTo = decks_[deckIndex(toDeck)].requestedRate.load(
                         std::memory_order_acquire) *
                     ratioB;
            eqFrom0 = planEq(runtime_.eqOut, runtime_.eqOutCount, elapsed0);
            eqFrom1 = planEq(runtime_.eqOut, runtime_.eqOutCount, elapsed1);
            eqTo0 = planEq(runtime_.eqIn, runtime_.eqInCount, elapsed0);
            eqTo1 = planEq(runtime_.eqIn, runtime_.eqInCount, elapsed1);
        } else if (cancelling) {
            fromDeck = runtime_.from;
            toDeck = runtime_.to;
            const double span = std::max(1.0, kCancelSeconds * outputSampleRate_);
            const double t0 = std::clamp(
                static_cast<double>(framesRendered_ - cancelStartFrame_) / span,
                0.0, 1.0);
            const double t1 = std::clamp(
                static_cast<double>(framesRendered_ + chunk - cancelStartFrame_) / span,
                0.0, 1.0);
            const float originalFrom = equalPowerOut(cancelStartProgress_);
            const float originalTo = equalPowerIn(cancelStartProgress_);
            gainFrom0 = originalFrom + (1.0f - originalFrom) * static_cast<float>(t0);
            gainFrom1 = originalFrom + (1.0f - originalFrom) * static_cast<float>(t1);
            gainTo0 = originalTo * (1.0f - static_cast<float>(t0));
            gainTo1 = originalTo * (1.0f - static_cast<float>(t1));
            if (!cancelIncomingStarted_) {
                gainTo0 = 0.0f;
                gainTo1 = 0.0f;
            }
            // Glide the outgoing deck back to its natural speed. Jumping
            // straight to 1.0 after a ramp would audibly step the pitch.
            rateFrom0 =
                cancelRateStart_ + (1.0 - cancelRateStart_) * t0;
            rateFrom1 =
                cancelRateStart_ + (1.0 - cancelRateStart_) * t1;
            rateTo = 1.0;
            // Return the EQ to unity over the reverse fade instead of
            // releasing a bass cut in one sample.
            eqFrom0 = {cancelEqOutStart_.low +
                           (1.0f - cancelEqOutStart_.low) * static_cast<float>(t0),
                       cancelEqOutStart_.mid +
                           (1.0f - cancelEqOutStart_.mid) * static_cast<float>(t0),
                       cancelEqOutStart_.high +
                           (1.0f - cancelEqOutStart_.high) * static_cast<float>(t0)};
            eqFrom1 = {cancelEqOutStart_.low +
                           (1.0f - cancelEqOutStart_.low) * static_cast<float>(t1),
                       cancelEqOutStart_.mid +
                           (1.0f - cancelEqOutStart_.mid) * static_cast<float>(t1),
                       cancelEqOutStart_.high +
                           (1.0f - cancelEqOutStart_.high) * static_cast<float>(t1)};
            eqTo0 = {cancelEqInStart_.low +
                         (1.0f - cancelEqInStart_.low) * static_cast<float>(t0),
                     cancelEqInStart_.mid +
                         (1.0f - cancelEqInStart_.mid) * static_cast<float>(t0),
                     cancelEqInStart_.high +
                         (1.0f - cancelEqInStart_.high) * static_cast<float>(t0)};
            eqTo1 = {cancelEqInStart_.low +
                         (1.0f - cancelEqInStart_.low) * static_cast<float>(t1),
                     cancelEqInStart_.mid +
                         (1.0f - cancelEqInStart_.mid) * static_cast<float>(t1),
                     cancelEqInStart_.high +
                         (1.0f - cancelEqInStart_.high) * static_cast<float>(t1)};
        } else {
            gainFrom0 = gainFrom1 = 1.0f;
            gainTo0 = gainTo1 = 1.0f;
            rateFrom0 = rateFrom1 = 1.0;
            rateTo = 1.0;
        }

        // Independent deck playback outside a transition.
        if (!running && !cancelling) {
            DeckRuntime& a = decks_[0];
            DeckRuntime& b = decks_[1];
            gainFrom0 = gainFrom1 = a.userGain.load(std::memory_order_relaxed);
            gainTo0 = gainTo1 = b.userGain.load(std::memory_order_relaxed);
            if (!a.targetPlay.load(std::memory_order_relaxed)) {
                gainFrom0 = gainFrom1 = 0.0f;
            }
            if (!b.targetPlay.load(std::memory_order_relaxed)) {
                gainTo0 = gainTo1 = 0.0f;
            }
            rateFrom0 = rateFrom1 =
                a.requestedRate.load(std::memory_order_relaxed);
            rateTo = b.requestedRate.load(std::memory_order_relaxed);
            fromDeck = DeckId::A;
            toDeck = DeckId::B;
        }

        if (running || cancelling) {
            // User gain still applies inside a mix, but the crossfade owns the
            // relative balance.
            gainFrom0 *= decks_[deckIndex(fromDeck)].userGain.load(std::memory_order_relaxed);
            gainFrom1 *= decks_[deckIndex(fromDeck)].userGain.load(std::memory_order_relaxed);
            gainTo0 *= decks_[deckIndex(toDeck)].userGain.load(std::memory_order_relaxed);
            gainTo1 *= decks_[deckIndex(toDeck)].userGain.load(std::memory_order_relaxed);
        }

        renderDeck(fromDeck, scratchA_.data(), chunk, gainFrom0, gainFrom1,
                   rateFrom0, rateFrom1, eqFrom0, eqFrom1);
        renderDeck(toDeck, scratchB_.data(), chunk, gainTo0, gainTo1, rateTo,
                   rateTo, eqTo0, eqTo1);

        // Correlation-aware headroom. Equal-power keeps uncorrelated material
        // at constant power; correlated material would peak near +6 dB.
        float rho = 0.0f;
        if (running || cancelling) {
            double dot = 0.0;
            double energyA = 0.0;
            double energyB = 0.0;
            for (std::size_t i = 0; i < chunk; ++i) {
                const double a0 = scratchA_[i * 2];
                const double a1 = scratchA_[i * 2 + 1];
                const double b0 = scratchB_[i * 2];
                const double b1 = scratchB_[i * 2 + 1];
                dot += a0 * b0 + a1 * b1;
                energyA += a0 * a0 + a1 * a1;
                energyB += b0 * b0 + b1 * b1;
            }
            if (energyA > 1e-12 && energyB > 1e-12) {
                rho = static_cast<float>(dot / std::sqrt(energyA * energyB));
                rho = std::clamp(rho, 0.0f, 1.0f);
            }
        }
        float headroom = 1.0f;
        if (running || cancelling) {
            const float gainA = 0.5f * (gainFrom0 + gainFrom1);
            const float gainB = 0.5f * (gainTo0 + gainTo1);
            const float target = correlationHeadroom(rho, gainA, gainB);
            const float coefficient =
                1.0f - std::exp(-static_cast<float>(chunk) /
                                (0.030f * static_cast<float>(outputSampleRate_)));
            headroomSmoothed_ += (target - headroomSmoothed_) * coefficient;
            headroom = headroomSmoothed_;
        } else {
            headroomSmoothed_ = 1.0f;
        }

        float peak = 0.0f;
        for (std::size_t i = 0; i < chunk; ++i) {
            // Per-sample de-click envelope. Advancing one step per sample
            // keeps the 8 ms ramp intact even for callback blocks longer than
            // the ramp (480 frames at 48 kHz), where a per-block step would
            // jump straight to zero.
            pauseEnvelope =
                std::clamp(pauseEnvelope + pauseDelta, 0.0f, 1.0f);
            for (int c = 0; c < 2; ++c) {
                float value =
                    (scratchA_[i * 2 + static_cast<std::size_t>(c)] +
                     scratchB_[i * 2 + static_cast<std::size_t>(c)]) *
                    headroom;
                value *= pauseEnvelope;
                value = sanitize(value);
                peak = std::max(peak, std::abs(value));
                output[(done + i) * 2 + static_cast<std::size_t>(c)] = value;
            }
        }

        // Smoothed peak limiter: instant attack, 80 ms release.
        const float ceiling = 0.98f;
        const float desired = peak > ceiling ? ceiling / peak : 1.0f;
        if (desired < limiterGain_) {
            limiterGain_ = desired;
        } else {
            const float release =
                1.0f - std::exp(-static_cast<float>(chunk) /
                                (0.080f * static_cast<float>(outputSampleRate_)));
            limiterGain_ += (desired - limiterGain_) * release;
        }
        limiterGain_ = std::clamp(limiterGain_, 0.05f, 1.0f);
        if (limiterGain_ < 0.9999f) {
            for (std::size_t i = 0; i < chunk; ++i) {
                output[(done + i) * 2] *= limiterGain_;
                output[(done + i) * 2 + 1] *= limiterGain_;
            }
        }
        outputPeak_ = peak * limiterGain_;

        framesRendered_ += chunk;
        done += chunk;

        if ((running || cancelling) && !handoverDone_ &&
            progress1 >= runtime_.handoverProgress) {
            handoverDone_ = true;
            master_.store(deckIndex(toDeck), std::memory_order_release);
        }
        if (running && progress1 >= 1.0) {
            finishTransition();
        } else if (cancelling &&
                   framesRendered_ - cancelStartFrame_ >=
                       static_cast<std::uint64_t>(kCancelSeconds * outputSampleRate_)) {
            const DeckId from = runtime_.from;
            const DeckId to = runtime_.to;
            decks_[deckIndex(to)].targetPlay.store(false,
                                                  std::memory_order_release);
            decks_[deckIndex(to)].playing = false;
            decks_[deckIndex(to)].gain = 0.0f;
            decks_[deckIndex(to)].fade = 0.0f;
            runtimeStarted_ = false;
            lastCancelFrame_ = framesRendered_;
            everCancelled_ = true;
            phase_.store(static_cast<int>(Phase::Idle),
                         std::memory_order_release);
            // If the cancel happened after the handover point, master_ points
            // at the stopped incoming deck. The outgoing deck kept playing, so
            // it must become the musical reference again.
            if (decks_[deckIndex(from)].audio.load(std::memory_order_acquire) !=
                nullptr) {
                decks_[deckIndex(from)].targetPlay.store(true,
                                                         std::memory_order_release);
                decks_[deckIndex(from)].rate = 1.0;
                decks_[deckIndex(from)].rampRate = 1.0;
                master_.store(deckIndex(from), std::memory_order_release);
            }
        }
        if (phase_.load(std::memory_order_acquire) ==
            static_cast<int>(Phase::Idle)) {
            // Remaining output in this callback is independent deck playback.
        }
    }

    pauseFade_ = pauseEnvelope;
    // The ramp finished inside this block: now it is safe to freeze the
    // transport and settle the deck flags.
    if (paused && pauseFade_ <= 0.0f) {
        for (DeckRuntime& deck : decks_) {
            deck.gain = 0.0f;
            if (!deck.targetPlay.load(std::memory_order_acquire)) {
                deck.playing = false;
                deck.fade = 0.0f;
            }
        }
    }

    publishSnapshot();
}

void MixEngine::renderDeck(DeckId deck, float* out, std::size_t frames,
                           float gain0, float gain1, double rate0,
                           double rate1, EqGains eq0, EqGains eq1) noexcept {
    DeckRuntime& runtime = decks_[deckIndex(deck)];
    runtime.blockPeak = 0.0f;
    const PcmTrack* source = runtime.audio.load(std::memory_order_acquire);
    if (!source || !runtime.playing) {
        std::memset(out, 0, frames * 2 * sizeof(float));
        runtime.gain = 0.0f;
        // Do not carry filter state across a silent gap: a later start must
        // not inherit an old crossover transient.
        runtime.eq.reset();
        return;
    }

    if (runtime.positionFrames >=
        static_cast<double>(source->frameCount()) - 1.0) {
        runtime.ended = true;
        runtime.playing = false;
        runtime.targetPlay.store(false, std::memory_order_release);
        runtime.gain = 0.0f;
        runtime.eq.reset();
        std::memset(out, 0, frames * 2 * sizeof(float));
        return;
    }

    runtime.rate = std::clamp(rate0, 0.25, 2.0);
    runtime.rampRate = std::clamp(rate1, 0.25, 2.0);
    const double step =
        std::clamp(rate0, 0.25, 2.0) *
        (static_cast<double>(source->sampleRate) /
         static_cast<double>(outputSampleRate_));
    const double lastFrame = static_cast<double>(source->frameCount()) - 1.0;
    const double startPosition = runtime.positionFrames;
    // Number of output frames that still land on real source samples. The
    // stretcher writes zeros after this point, so the de-click fade must be
    // anchored here rather than at the end of the requested callback block.
    const double remainingSource = std::max(0.0, lastFrame - startPosition);
    const double remainingOutput = step > 0.0 ? remainingSource / step : 0.0;
    const std::size_t playable =
        startPosition >= lastFrame || step <= 0.0
            ? 0
            : std::min(frames,
                       static_cast<std::size_t>(std::ceil(remainingOutput)));
    // WSOLA has algorithmic look-ahead, so the source may end a few samples
    // before or after the nominal last output frame. Treat a block that
    // contains the end (or lands exactly on it) as the EOF block and fade the
    // remaining region to zero instead of stepping from full level.
    const bool reachesEnd = remainingOutput <= static_cast<double>(frames);
    const double stepEnd =
        std::clamp(rate1, 0.25, 2.0) *
        (static_cast<double>(source->sampleRate) /
         static_cast<double>(outputSampleRate_));
    ITimeStretchStream* stream =
        stream_[deckIndex(deck)].load(std::memory_order_acquire);
    if (stream) {
        stream->render(*source, runtime.positionFrames, step, stepEnd, out,
                       frames);
    } else {
        const ITimeStretchProcessor* stretch =
            stretch_.load(std::memory_order_acquire);
        if (!stretch) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }
        stretch->render(*source, runtime.positionFrames, step, out, frames);
    }

    // Three-band automation runs on the deck signal after the stretcher and
    // before the crossfade gain. Unity gains are an exact bypass.
    runtime.eqGains = eq0;
    runtime.eq.process(out, frames, eq0, eq1);

    const float fadeStep =
        1.0f / static_cast<float>(outputSampleRate_ * kDeclickSeconds);
    const bool target = runtime.targetPlay.load(std::memory_order_acquire);
    const float gainEnd = target ? gain1 : 0.0f;
    float g = gain0;
    for (std::size_t i = 0; i < frames; ++i) {
        const float t =
            frames > 1 ? static_cast<float>(i) / static_cast<float>(frames - 1)
                       : 1.0f;
        const float envelope = g + (gainEnd - g) * t;
        if (target) {
            runtime.fade = std::min(1.0f, runtime.fade + fadeStep);
        } else {
            runtime.fade = std::max(0.0f, runtime.fade - fadeStep);
        }
        const float valueScale = envelope * runtime.fade;
        out[i * 2] *= valueScale;
        out[i * 2 + 1] *= valueScale;
        runtime.blockPeak = std::max(
            runtime.blockPeak, std::max(std::abs(out[i * 2]),
                                        std::abs(out[i * 2 + 1])));
    }
    runtime.gain = std::max(0.0f, std::abs(gainEnd)) * runtime.fade;
    if (reachesEnd) {
        // De-click the physical end of the track over the samples that still
        // map to source audio. The tail ends at silence, never on a full-scale
        // step to the zeros of the next callback.
        const std::size_t endIndex = playable;
        const std::size_t fadeFrames = std::min<std::size_t>(
            endIndex, static_cast<std::size_t>(0.008 * outputSampleRate_));
        const std::size_t first = endIndex - fadeFrames;
        for (std::size_t i = 0; i < fadeFrames; ++i) {
            const float scale =
                static_cast<float>(fadeFrames - 1 - i) /
                static_cast<float>(std::max<std::size_t>(1, fadeFrames));
            const std::size_t index = first + i;
            out[index * 2] *= scale;
            out[index * 2 + 1] *= scale;
        }
        for (std::size_t i = endIndex; i < frames; ++i) {
            out[i * 2] = 0.0f;
            out[i * 2 + 1] = 0.0f;
        }
        runtime.ended = true;
        runtime.playing = false;
        runtime.targetPlay.store(false, std::memory_order_release);
        runtime.gain = 0.0f;
    }
}

void MixEngine::finishTransition() noexcept {
    runtimeStarted_ = false;
    lastCompletionFrame_ = framesRendered_;
    everCompleted_ = true;
    phase_.store(static_cast<int>(Phase::Idle), std::memory_order_release);
    DeckRuntime& from = decks_[deckIndex(runtime_.from)];
    DeckRuntime& to = decks_[deckIndex(runtime_.to)];
    from.targetPlay.store(false, std::memory_order_release);
    from.gain = 0.0f;
    from.playing = false;
    from.fade = 0.0f;
    from.rate = 1.0;
    from.rampRate = 1.0;
    to.targetPlay.store(true, std::memory_order_release);
    to.rate = 1.0;
    to.rampRate = 1.0;
    master_.store(deckIndex(runtime_.to), std::memory_order_release);
    completed_.fetch_add(1, std::memory_order_acq_rel);
}

void MixEngine::publishSnapshot() noexcept {
    MixSnapshot snapshot;
    snapshot.sampleRate = outputSampleRate_;
    snapshot.master = master_.load(std::memory_order_acquire) == 0 ? DeckId::A
                                                                  : DeckId::B;
    const int phase = phase_.load(std::memory_order_acquire);
    snapshot.phase = phase == static_cast<int>(Phase::Running)
                         ? TransitionPhase::Running
                         : (phase == static_cast<int>(Phase::Cancelling)
                                ? TransitionPhase::Cancelling
                                : TransitionPhase::Idle);
    snapshot.transitionType = runtime_.type;
    snapshot.stage = liveStage();
    if (phase != static_cast<int>(Phase::Idle)) {
        const double sampleRate = static_cast<double>(outputSampleRate_);
        const double preSeconds = static_cast<double>(
            runtime_.crossfadeStartFrame - runtime_.startFrame) / sampleRate;
        const double fadeSeconds = static_cast<double>(
            runtime_.endFrame - runtime_.crossfadeStartFrame) / sampleRate;
        snapshot.preTransitionSeconds = std::max(0.0, preSeconds);
        snapshot.crossfadeDuration = std::max(0.0, fadeSeconds);
        snapshot.transitionDuration =
            snapshot.preTransitionSeconds + snapshot.crossfadeDuration;
        snapshot.transitionElapsed = std::clamp(
            (static_cast<double>(framesRendered_) -
             static_cast<double>(runtime_.startFrame)) /
                sampleRate,
            0.0, snapshot.transitionDuration);
        snapshot.transitionProgress =
            snapshot.transitionDuration > 0.0
                ? std::clamp(snapshot.transitionElapsed /
                                 snapshot.transitionDuration,
                             0.0, 1.0)
                : 1.0;
        snapshot.crossfadeElapsed = std::clamp(
            (static_cast<double>(framesRendered_) -
             static_cast<double>(runtime_.crossfadeStartFrame)) /
                sampleRate,
            0.0, snapshot.crossfadeDuration);
        const double elapsed =
            (static_cast<double>(framesRendered_) -
             static_cast<double>(runtime_.crossfadeStartFrame)) /
            sampleRate;
        snapshot.tempoRatioA = planRate(elapsed);
        snapshot.tempoRatioB =
            decks_[deckIndex(runtime_.to)].requestedRate.load(
                std::memory_order_relaxed);
        snapshot.beatPhaseErrorMs = runtime_.beatPhaseErrorMs;
        snapshot.pitchLock = runtime_.pitchLock;
        snapshot.bassSwap = runtime_.bassSwap;
        const EqGains out = planEq(runtime_.eqOut, runtime_.eqOutCount, elapsed);
        const EqGains in = planEq(runtime_.eqIn, runtime_.eqInCount, elapsed);
        const int fromIndex = deckIndex(runtime_.from);
        const int toIndex = deckIndex(runtime_.to);
        snapshot.eqLow[fromIndex] = out.low;
        snapshot.eqMid[fromIndex] = out.mid;
        snapshot.eqHigh[fromIndex] = out.high;
        snapshot.eqLow[toIndex] = in.low;
        snapshot.eqMid[toIndex] = in.mid;
        snapshot.eqHigh[toIndex] = in.high;
    }
    snapshot.completedTransitions = completed_.load(std::memory_order_acquire);
    snapshot.outputFrames = framesRendered_;
    snapshot.outputLatencySeconds = outputLatency_.load(std::memory_order_acquire);
    snapshot.outputPeak = outputPeak_;
    snapshot.limiterGain = limiterGain_;
    snapshot.paused = paused_.load(std::memory_order_acquire);
    for (int i = 0; i < 2; ++i) {
        const DeckRuntime& runtime = decks_[i];
        DeckStatus& deck = snapshot.decks[i];
        const PcmTrack* source = runtime.audio.load(std::memory_order_acquire);
        deck.loaded = source != nullptr;
        deck.playing = runtime.playing;
        deck.ended = runtime.ended;
        deck.trackId = runtime.trackId.load(std::memory_order_acquire);
        deck.durationSeconds = source ? source->duration() : 0.0;
        deck.positionSeconds =
            source && source->sampleRate > 0
                ? runtime.positionFrames / source->sampleRate
                : 0.0;
        deck.rate = runtime.rate;
        deck.analysedBpm = runtime.analysedBpm.load(std::memory_order_acquire);
        deck.bpm = deck.analysedBpm * runtime.rate;
        deck.gain = runtime.gain;
        deck.peak = runtime.blockPeak;
        deck.sampleRate = source ? source->sampleRate : 0;
        deck.channels = source ? source->channels : 0;
    }

    if (snapshotLock_.test_and_set(std::memory_order_acquire)) {
        // The UI is copying the current buffer. Skip this publication; the
        // UI will pick up the next one.
        return;
    }
    const int next = 1 - snapshotIndex_.load(std::memory_order_relaxed);
    snapshotBuffers_[next] = snapshot;
    snapshotIndex_.store(next, std::memory_order_release);
    snapshotLock_.clear(std::memory_order_release);
}

MixSnapshot MixEngine::snapshot() const {
    MixSnapshot copy;
    while (snapshotLock_.test_and_set(std::memory_order_acquire)) {
    }
    copy = snapshotBuffers_[snapshotIndex_.load(std::memory_order_acquire)];
    snapshotLock_.clear(std::memory_order_release);
    return copy;
}

} // namespace bgmedia::mix
