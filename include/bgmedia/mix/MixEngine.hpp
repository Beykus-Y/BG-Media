#pragma once

// Dual-deck BG Mix engine. The render() call is real-time safe: no allocation,
// no file I/O, no locks, no decoding. Tracks are decoded and converted on a
// worker thread and published here while the deck is idle.
//
// One engine owns both decks and the single output clock. Hosts pump render()
// from one audio callback and call the control methods from the UI thread.

#include "bgmedia/mix/ITimeStretchProcessor.hpp"
#include "bgmedia/mix/MixTypes.hpp"
#include "bgmedia/mix/PcmTrack.hpp"
#include "bgmedia/mix/SpectralEq.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bgmedia::mix {

class MixEngine {
public:
    explicit MixEngine(std::uint32_t outputSampleRate = 48000);
    ~MixEngine();

    MixEngine(const MixEngine&) = delete;
    MixEngine& operator=(const MixEngine&) = delete;

    [[nodiscard]] std::uint32_t outputSampleRate() const noexcept {
        return outputSampleRate_;
    }

    // Control thread. Loading is rejected while the deck is audible: the host
    // must stop/prepare the deck first. This keeps the callback free of shared
    // pointer churn for buffers it is reading.
    [[nodiscard]] bool loadDeck(DeckId deck,
                                std::shared_ptr<const PcmTrack> track,
                                std::uint64_t trackId, double analysedBpm);
    void clearDeck(DeckId deck);
    [[nodiscard]] bool deckLoaded(DeckId deck) const noexcept;
    [[nodiscard]] double deckDuration(DeckId deck) const noexcept;

    void play(DeckId deck);
    void pause(DeckId deck);
    void seek(DeckId deck, double seconds);
    void setDeckGain(DeckId deck, float gain);
    void setDeckRate(DeckId deck, double rate);
    void setMaster(DeckId deck);

    // The plan is copied into a fixed-size runtime block. Returns false when a
    // transition is already running or the plan is invalid.
    [[nodiscard]] bool startTransition(const MixPlan& plan);
    // Reverse-fades to the outgoing deck and stops the incoming deck.
    void cancelTransition();
    [[nodiscard]] bool transitionActive() const noexcept;

    // Global transport freeze. Both decks and the transition clock stop.
    void setPaused(bool paused);
    [[nodiscard]] bool paused() const noexcept;

    // Full stop: freezes the transport, clears both play requests and drops an
    // active transition. Unlike setPaused() this is a one-way request; the
    // callback settles deck state before its paused early-out.
    void stop();

    // Control thread: frees tracks that the audio callback can no longer
    // reference. Tracks replaced by loadDeck()/clearDeck() are retained until
    // the render epoch proves that at least one callback started afterwards.
    void reclaimRetired();

    void setOutputLatency(double seconds);
    void setTimeStretch(std::shared_ptr<const ITimeStretchProcessor> processor);
    [[nodiscard]] const ITimeStretchProcessor& timeStretch() const noexcept;

    // Audio callback. `output` is interleaved stereo float.
    void render(float* output, std::size_t frames) noexcept;

    [[nodiscard]] MixSnapshot snapshot() const;
    [[nodiscard]] std::uint64_t completedTransitions() const noexcept;

private:
    struct RuntimePlan {
        bool valid = false;
        TransitionType type = TransitionType::Off;
        DeckId from = DeckId::A;
        DeckId to = DeckId::B;
        std::uint64_t startFrame = 0;          // ramp / overall start
        std::uint64_t crossfadeStartFrame = 0; // overlay begins
        std::uint64_t endFrame = 0;            // overlay ends
        double startB = 0.0;
        double handoverProgress = 0.5;
        double rateStart = 1.0;
        double rateEnd = 1.0;
        double tempoRatioB = 1.0;
        int tempoCount = 0;
        TempoPoint tempo[128]{};
        int eqOutCount = 0;
        EqGainPoint eqOut[64]{};
        int eqInCount = 0;
        EqGainPoint eqIn[64]{};
        bool directCut = false;
        bool tempoRamp = false;
        bool bassSwap = false;
        bool pitchLock = false;
        double beatPhaseErrorMs = 0.0;
    };

    enum class Phase : int {
        Idle = 0,
        Running = 1,
        Cancelling = 2
    };

    struct DeckRuntime {
        std::atomic<const PcmTrack*> audio{nullptr};
        std::shared_ptr<const PcmTrack> owned;
        std::atomic<std::uint64_t> trackId{0};
        std::atomic<double> analysedBpm{0.0};
        std::atomic<bool> targetPlay{false};
        std::atomic<float> userGain{1.0f};
        std::atomic<double> requestedRate{1.0};

        std::atomic<std::uint64_t> seekSerial{0};
        // Seek request. The payload is atomic because a later seek() may
        // overwrite it while the callback is still reading the previous one;
        // seekSerial only tells the callback that a new value is available.
        std::atomic<double> seekSeconds{0.0};
        std::uint64_t seekApplied = 0;

        // Audio-thread reset request. loadDeck()/clearDeck() must not touch
        // positionFrames/playing/gain: those belong to the callback.
        std::atomic<std::uint64_t> resetSerial{0};
        std::uint64_t resetApplied = 0;

        // Audio-thread state.
        double positionFrames = 0.0;
        double rate = 1.0;
        double rampRate = 1.0;
        float gain = 0.0f;
        float fade = 0.0f;
        bool playing = false;
        bool ended = false;
        bool wasTargetPlaying = false;
        float blockPeak = 0.0f;
        float lastSample[2] = {0.0f, 0.0f};
        ThreeBandEq eq;
        EqGains eqGains{};
    };

    [[nodiscard]] bool loadAllowed(DeckId deck) const noexcept;
    void publishSnapshot() noexcept;
    void consumeStartRequest(std::uint64_t frame) noexcept;
    [[nodiscard]] double planRate(double elapsed) const noexcept;
    [[nodiscard]] EqGains planEq(const EqGainPoint* points, int count,
                                 double elapsed) const noexcept;
    [[nodiscard]] TransitionStage liveStage() const noexcept;
    void renderDeck(DeckId deck, float* out, std::size_t frames, float gain0,
                    float gain1, double rate0, double rate1, EqGains eq0,
                    EqGains eq1) noexcept;
    void finishTransition() noexcept;

    struct RetiredTrack {
        std::shared_ptr<const PcmTrack> owner;
        std::uint64_t epoch = 0;
    };
    struct RetiredProcessor {
        std::shared_ptr<const ITimeStretchProcessor> owner;
        std::uint64_t epoch = 0;
    };
    struct RetiredStream {
        std::shared_ptr<ITimeStretchStream> owner;
        std::uint64_t epoch = 0;
    };
    void retireTrack(DeckId deck, std::shared_ptr<const PcmTrack> owner);
    void retireProcessor(
        std::shared_ptr<const ITimeStretchProcessor> owner);
    void retireStream(std::shared_ptr<ITimeStretchStream> owner);
    void rebuildStretchStreams();
    void resetStretchStream(DeckId deck, double positionFrames) noexcept;

    std::uint32_t outputSampleRate_ = 48000;
    DeckRuntime decks_[2];
    std::shared_ptr<const ITimeStretchProcessor> stretchOwned_;
    std::atomic<const ITimeStretchProcessor*> stretch_{nullptr};
    std::shared_ptr<ITimeStretchStream> streamOwned_[2];
    std::atomic<ITimeStretchStream*> stream_[2]{nullptr, nullptr};

    std::atomic<int> master_{0};
    std::atomic<int> phase_{static_cast<int>(Phase::Idle)};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> planReady_{false};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<double> outputLatency_{0.0};

    // Monotonic heartbeat incremented at the start of every render call. It is
    // the acknowledgement used to defer destruction of replaced audio buffers
    // and time-stretch processors.
    std::atomic<std::uint64_t> renderEpoch_{0};
    std::vector<RetiredTrack> retiredTracks_[2];
    std::vector<RetiredProcessor> retiredProcessors_;
    std::vector<RetiredStream> retiredStreams_;

    MixPlan pendingPlan_;
    RuntimePlan runtime_;
    bool runtimeStarted_ = false;
    std::uint64_t transitionStartFrame_ = 0;
    std::uint64_t transitionEndFrame_ = 0;
    std::uint64_t cancelStartFrame_ = 0;
    double cancelStartProgress_ = 0.0;
    double cancelRateStart_ = 1.0;
    EqGains cancelEqOutStart_{};
    EqGains cancelEqInStart_{};
    bool cancelIncomingStarted_ = false;
    bool incomingStarted_ = false;
    std::uint64_t lastCompletionFrame_ = 0;
    std::uint64_t lastCancelFrame_ = 0;
    bool everCompleted_ = false;
    bool everCancelled_ = false;
    double liveRateA_ = 1.0;
    double liveRateB_ = 1.0;
    std::atomic<bool> cancelRequested_{false};
    float pauseFade_ = 0.0f;
    std::uint64_t framesRendered_ = 0;
    bool handoverDone_ = false;
    float limiterGain_ = 1.0f;
    float outputPeak_ = 0.0f;
    std::vector<float> scratchA_;
    std::vector<float> scratchB_;

    // Snapshot publication: the audio thread writes the inactive buffer and
    // publishes it with a short-lived flag. It never blocks on the UI thread:
    // when the flag is taken the update is skipped and the previous snapshot
    // stays available.
    mutable std::atomic_flag snapshotLock_ = ATOMIC_FLAG_INIT;
    mutable MixSnapshot snapshotBuffers_[2];
    mutable std::atomic<int> snapshotIndex_{0};
};

} // namespace bgmedia::mix
