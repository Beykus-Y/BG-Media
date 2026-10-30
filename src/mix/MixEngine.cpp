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

} // namespace

MixEngine::MixEngine(std::uint32_t outputSampleRate)
    : outputSampleRate_(outputSampleRate ? outputSampleRate : 48000),
      stretchOwned_(std::make_shared<LinearResampleStretcher>()) {
    stretch_.store(stretchOwned_.get(), std::memory_order_release);
    MixSnapshot initial;
    initial.sampleRate = outputSampleRate_;
    snapshotBuffers_[0] = initial;
    snapshotBuffers_[1] = initial;
    scratchA_.resize(kChunkFrames * 2);
    scratchB_.resize(kChunkFrames * 2);
    for (int i = 0; i < 2; ++i) {
        decks_[i].owned.reset();
        decks_[i].audio.store(nullptr, std::memory_order_relaxed);
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
    // Ownership is handed over on the control thread, but the old buffer stays
    // in the retire list until the callback heartbeat proves that no in-flight
    // render can still hold a raw pointer to it.
    std::shared_ptr<const PcmTrack> previous = std::move(runtime.owned);
    runtime.owned = std::move(track);
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
                              : std::make_shared<LinearResampleStretcher>();
    stretch_.store(stretchOwned_.get(), std::memory_order_release);
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
    runtime_.directCut = plan.type == TransitionType::DirectCut;
    runtime_.startFrame = frame;
    const double seconds = std::max(0.0, plan.duration);
    runtime_.endFrame = frame + static_cast<std::uint64_t>(
                                    std::llround(seconds * outputSampleRate_));
    runtime_.rateStart = 1.0;
    runtime_.rateEnd = 1.0;
    runtime_.tempoCount = std::min<int>(
        static_cast<int>(plan.tempoCurve.size()), 64);
    for (int i = 0; i < runtime_.tempoCount; ++i) {
        runtime_.tempo[i] = plan.tempoCurve[static_cast<std::size_t>(i)];
    }
    if (runtime_.tempoCount > 0) {
        runtime_.rateStart = runtime_.tempo[0].rate;
        runtime_.rateEnd = runtime_.tempo[runtime_.tempoCount - 1].rate;
    }

    DeckRuntime& from = decks_[deckIndex(runtime_.from)];
    DeckRuntime& to = decks_[deckIndex(runtime_.to)];
    const bool fromWasPlaying = from.playing;
    if (const PcmTrack* source = from.audio.load(std::memory_order_acquire)) {
        from.positionFrames = std::clamp(plan.transitionStartA, 0.0,
                                         source->duration()) *
                              source->sampleRate;
    }
    from.playing = true;
    from.ended = false;
    from.fade = fromWasPlaying ? 1.0f : 0.0f;
    from.rate = 1.0;
    from.rampRate = 1.0;
    to.positionFrames = plan.startB;
    if (const PcmTrack* source = to.audio.load(std::memory_order_acquire)) {
        to.positionFrames =
            std::clamp(plan.startB, 0.0, source->duration()) * source->sampleRate;
    }
    to.playing = true;
    to.ended = false;
    to.fade = 0.0f;
    to.rate = 1.0;
    to.rampRate = 1.0;
    to.targetPlay.store(true, std::memory_order_release);
    from.targetPlay.store(true, std::memory_order_release);

    transitionStartFrame_ = frame;
    transitionEndFrame_ = runtime_.endFrame;
    handoverDone_ = false;
    planReady_.store(false, std::memory_order_release);

    if (runtime_.directCut || runtime_.endFrame <= frame) {
        from.targetPlay.store(false, std::memory_order_release);
        from.playing = false;
        from.gain = 0.0f;
        to.gain = 1.0f;
        to.fade = 1.0f;
        master_.store(deckIndex(runtime_.to), std::memory_order_release);
        runtimeStarted_ = false;
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
    for (DeckRuntime& deck : decks_) {
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
        }
    }

    // Apply pending seeks BEFORE consuming a transition plan. A deck loaded in
    // this same callback still has the zero seek published by loadDeck(); if
    // that loop ran after consumeStartRequest() it would overwrite the cue
    // positions selected by the plan.
    for (DeckRuntime& deck : decks_) {
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
        cancelStartProgress_ =
            std::clamp(static_cast<double>(framesRendered_ - transitionStartFrame_) /
                           span,
                       0.0, 1.0);
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
        const std::size_t chunk = std::min(kChunkFrames, frames - done);
        const int phase = phase_.load(std::memory_order_acquire);
        const bool running = phase == static_cast<int>(Phase::Running);
        const bool cancelling = phase == static_cast<int>(Phase::Cancelling);

        double progress0 = 1.0;
        double progress1 = 1.0;
        if (running || cancelling) {
            const double span = static_cast<double>(
                std::max<std::uint64_t>(1, transitionEndFrame_ - transitionStartFrame_));
            progress0 = std::clamp(
                static_cast<double>(framesRendered_ - transitionStartFrame_) / span,
                0.0, 1.0);
            progress1 = std::clamp(
                static_cast<double>(framesRendered_ + chunk - transitionStartFrame_) /
                    span,
                0.0, 1.0);
        }

        float gainFrom0 = 0.0f;
        float gainFrom1 = 0.0f;
        float gainTo0 = 0.0f;
        float gainTo1 = 0.0f;
        double rateFrom0 = 1.0;
        double rateFrom1 = 1.0;
        double rateTo = 1.0;
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
                static_cast<double>(framesRendered_ - transitionStartFrame_) /
                outputSampleRate_;
            const double elapsed1 =
                static_cast<double>(framesRendered_ + chunk - transitionStartFrame_) /
                outputSampleRate_;
            rateFrom0 = planRate(elapsed0);
            rateFrom1 = planRate(elapsed1);
            rateTo = decks_[deckIndex(toDeck)].requestedRate.load(
                std::memory_order_acquire);
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
            rateFrom0 = 1.0;
            rateFrom1 = 1.0;
            rateTo = 1.0;
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
                   rateFrom0, rateFrom1);
        renderDeck(toDeck, scratchB_.data(), chunk, gainTo0, gainTo1,
                   rateTo, rateTo);

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
        const float headroom = correlationHeadroom(rho);

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
                           double rate1) noexcept {
    DeckRuntime& runtime = decks_[deckIndex(deck)];
    runtime.blockPeak = 0.0f;
    const PcmTrack* source = runtime.audio.load(std::memory_order_acquire);
    if (!source || !runtime.playing) {
        std::memset(out, 0, frames * 2 * sizeof(float));
        runtime.gain = 0.0f;
        return;
    }

    if (runtime.positionFrames >=
        static_cast<double>(source->frameCount()) - 1.0) {
        runtime.ended = true;
        runtime.playing = false;
        runtime.targetPlay.store(false, std::memory_order_release);
        runtime.gain = 0.0f;
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
    const std::size_t playable =
        startPosition >= lastFrame || step <= 0.0
            ? 0
            : std::min(frames,
                       static_cast<std::size_t>(
                           std::ceil((lastFrame - startPosition) / step)));
    const bool reachesEnd = playable < frames;
    const ITimeStretchProcessor* stretch = stretch_.load(std::memory_order_acquire);
    if (!stretch) {
        std::memset(out, 0, frames * 2 * sizeof(float));
        return;
    }
    stretch->render(*source, runtime.positionFrames, step, out, frames);

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
    if (reachesEnd && playable > 0) {
        // De-click the physical end of the track with a short linear tail that
        // ends on the last playable sample, never on trailing zeros.
        const std::size_t fadeFrames = std::min<std::size_t>(
            playable, static_cast<std::size_t>(0.008 * outputSampleRate_));
        const std::size_t first = playable - fadeFrames;
        for (std::size_t i = 0; i < fadeFrames; ++i) {
            const float scale =
                static_cast<float>(fadeFrames - 1 - i) /
                static_cast<float>(std::max<std::size_t>(1, fadeFrames));
            const std::size_t index = first + i;
            out[index * 2] *= scale;
            out[index * 2 + 1] *= scale;
        }
    }
    if (reachesEnd) {
        runtime.ended = true;
        runtime.playing = false;
        runtime.targetPlay.store(false, std::memory_order_release);
        runtime.gain = 0.0f;
    }
}

void MixEngine::finishTransition() noexcept {
    runtimeStarted_ = false;
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
    if (phase != static_cast<int>(Phase::Idle)) {
        snapshot.transitionType = runtime_.type;
        snapshot.transitionDuration = static_cast<double>(
            runtime_.endFrame - runtime_.startFrame) / outputSampleRate_;
        snapshot.transitionElapsed =
            static_cast<double>(framesRendered_ - runtime_.startFrame) /
            outputSampleRate_;
        snapshot.transitionProgress =
            snapshot.transitionDuration > 0.0
                ? std::clamp(snapshot.transitionElapsed /
                                 snapshot.transitionDuration,
                             0.0, 1.0)
                : 1.0;
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
