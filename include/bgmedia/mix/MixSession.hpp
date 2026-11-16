#pragma once

// BG Mix session: queue, background preparation and deck scheduling on top of
// the real-time MixEngine. It is UI-agnostic; the desktop app supplies a decode
// + BG Beat preparation callback and pumps update() once per frame.

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/MixPlanner.hpp"
#include "bgmedia/mix/MixTypes.hpp"
#include "bgmedia/mix/PcmTrack.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bgmedia::mix {

struct PreparedMixTrack {
    std::shared_ptr<const PcmTrack> audio;
    std::shared_ptr<const AnalysisResult> analysis;
    std::shared_ptr<const MixFeatureMap> features;
};

// Returns false with `error` set on failure. The progress callback may be
// invoked from the preparation thread.
using MixPrepareCallback = std::function<bool(
    const std::filesystem::path&, PreparedMixTrack&, std::string& error,
    std::function<void(float, std::string_view)> progress)>;

class MixSession {
public:
    MixSession(MixEngine& engine, MixPrepareCallback prepare,
               MixPlanner planner = MixPlanner{});
    ~MixSession();

    MixSession(const MixSession&) = delete;
    MixSession& operator=(const MixSession&) = delete;

    // Queue management. addTrack returns 0 when the path is empty.
    std::uint64_t addTrack(const std::filesystem::path& path);
    std::vector<std::uint64_t> addTracks(
        const std::vector<std::filesystem::path>& paths);
    bool removeTrack(std::uint64_t id);
    void clearQueue();

    // Transport.
    void play();
    void pause();
    void next();                  // manual transition with a smooth crossfade
    void startTransitionNow();    // same, but always uses the current plan
    void cancelTransition();
    void stopAll();

    void setTransitionType(TransitionType type);
    void setTransitionDuration(double seconds);  // 0 = automatic
    void setAdaptive(bool enabled);
    void setTempoRampEnabled(bool enabled);
    void setTempoRampMode(TempoRampMode mode);
    void setTempoRampMaxPercent(double percent);
    // 0 (or any value < 1) means automatic bar count.
    void setTempoRampBars(double bars);
    void setTempoCurve(TempoCurveKind curve);
    void setPitchLock(bool enabled);
    void setQuality(StretchQuality quality);
    void setMixMode(MixMode mode);
    void setAggressiveness(Aggressiveness aggressiveness);
    void setBassSwap(bool enabled);
    void setPhraseBars(int bars);
    void setAutoQueue(bool enabled);

    [[nodiscard]] TransitionType transitionType() const noexcept { return type_; }
    [[nodiscard]] double transitionDuration() const noexcept { return duration_; }
    [[nodiscard]] bool adaptive() const noexcept { return adaptive_; }
    [[nodiscard]] bool tempoRampEnabled() const noexcept {
        return tempoRampMode_ != TempoRampMode::Off;
    }
    [[nodiscard]] TempoRampMode tempoRampMode() const noexcept {
        return tempoRampMode_;
    }
    [[nodiscard]] double tempoRampMaxPercent() const noexcept {
        return rampMaxPercent_;
    }
    [[nodiscard]] double tempoRampBars() const noexcept { return rampBars_; }
    [[nodiscard]] TempoCurveKind tempoCurve() const noexcept { return curve_; }
    [[nodiscard]] bool pitchLock() const noexcept { return pitchLock_; }
    [[nodiscard]] StretchQuality quality() const noexcept { return quality_; }
    [[nodiscard]] MixMode mixMode() const noexcept { return mode_; }
    [[nodiscard]] Aggressiveness aggressiveness() const noexcept {
        return aggressiveness_;
    }
    [[nodiscard]] bool bassSwap() const noexcept { return bassSwap_; }
    [[nodiscard]] int phraseBars() const noexcept { return phraseBars_; }
    [[nodiscard]] bool autoQueue() const noexcept { return autoQueue_; }

    // Seeks a loaded deck to position in seconds.
    // Safe policy: rejected (returns false) during an active transition to prevent clicks/desync.
    bool seekDeck(DeckId deck, double seconds);

    // Jumps the master deck to ~leadSeconds before the planned transition start for rapid auditioning.
    bool jumpToPlannedTransition(double leadSeconds = 7.0);

    // Explicitly rebuilds the transition plan preview.
    void refreshPlan();

    // Must be called from the UI thread every frame.
    void update();

    [[nodiscard]] MixSessionSnapshot snapshot() const;
    [[nodiscard]] std::shared_ptr<const AnalysisResult> deckAnalysis(
        DeckId deck) const;
    [[nodiscard]] std::shared_ptr<const MixFeatureMap> deckFeatures(
        DeckId deck) const;
    [[nodiscard]] std::string deckName(DeckId deck) const;
    [[nodiscard]] MixEngine& engine() noexcept { return engine_; }

private:
    struct Item {
        std::uint64_t id = 0;
        std::filesystem::path path;
        std::string name;
        QueueState state = QueueState::Pending;
        float progress = 0.0f;
        std::shared_ptr<const PcmTrack> audio;
        std::shared_ptr<const AnalysisResult> analysis;
        std::shared_ptr<const MixFeatureMap> features;
        std::string error;
    };

    struct CompletedJob {
        std::uint64_t id = 0;
        bool ok = false;
        PreparedMixTrack prepared;
        std::string error;
    };

    [[nodiscard]] Item* findItem(std::uint64_t id);
    [[nodiscard]] const Item* findItem(std::uint64_t id) const;
    void workerLoop();
    void startWorkerIfNeeded();
    void collectCompleted();
    void pruneFinishedItems();
    void serviceDecks();
    void handleTransitionCompletion();
    void executeScheduledTransition(const MixPlan& plannedPreview);
    [[nodiscard]] double deckPosition(DeckId deck) const;
    [[nodiscard]] DeckId masterDeck() const;
    [[nodiscard]] double deckDuration(DeckId deck) const;
    [[nodiscard]] std::uint64_t loadedId(DeckId deck) const;
    void installDeck(DeckId deck, const Item& item);
    void applyStretchQuality();
    void startPlannedTransition(double durationOverride);
    [[nodiscard]] MixPlan buildPlan(DeckId from, DeckId to,
                                    const MixSnapshot& snap,
                                    double durationOverride, bool execution);
    void promoteEndedMaster(const MixSnapshot& snap);
    void ensurePlanValid();
    void invalidatePlan();

    MixEngine& engine_;
    MixPrepareCallback prepare_;
    MixPlanner planner_;

    mutable std::mutex mutex_;  // protects items_ and preview_/status strings
    std::vector<Item> items_;
    std::uint64_t nextId_ = 1;
    std::uint64_t currentId_ = 0;
    std::uint64_t lastCompleted_ = 0;

    TransitionType type_ = TransitionType::SmartCrossfade;
    double duration_ = 0.0;
    bool adaptive_ = true;
    TempoRampMode tempoRampMode_ = TempoRampMode::Auto;
    double rampMaxPercent_ = 6.0;
    double rampBars_ = 0.0;  // 0 = automatic
    TempoCurveKind curve_ = TempoCurveKind::SCurve;
    bool pitchLock_ = true;
    StretchQuality quality_ = StretchQuality::Quality;
    MixMode mode_ = MixMode::Auto;
    Aggressiveness aggressiveness_ = Aggressiveness::Balanced;
    bool bassSwap_ = true;
    int phraseBars_ = 4;
    bool stretchDirty_ = false;
    bool appliedPitchPreserving_ = true;
    bool autoQueue_ = true;
    bool autoPlay_ = false;
    bool nextRequested_ = false;
    bool clearDecksRequested_ = false;
    std::string status_;
    std::string lastError_;
    MixPlan preview_;
    MixPlan lastExecutedPlan_;
    bool hasExecutedPlan_ = false;
    bool planDirty_ = true;
    bool previewIsExecution_ = false;
    // Live master position the current execution plan was built from; limits
    // how often an expired-window plan is rebuilt.
    double executionPlanPosA_ = 0.0;
    std::uint64_t cachedFromId_ = 0;
    std::uint64_t cachedToId_ = 0;
    DeckId cachedMasterDeck_ = DeckId::A;

    std::thread worker_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> jobRunning_{false};
    // Track IDs are monotonic: cancellation cannot target a replacement job.
    std::atomic<std::uint64_t> cancelledJobId_{0};
    bool jobActive_ = false;
    std::uint64_t jobId_ = 0;
    std::filesystem::path jobPath_;
    std::uint64_t preparingId_ = 0;
    std::atomic<float> preparingProgress_{0.0f};
    std::string preparingStage_;
    std::vector<CompletedJob> completed_;
};

} // namespace bgmedia::mix
