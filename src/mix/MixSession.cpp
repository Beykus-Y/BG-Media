#include "bgmedia/mix/MixSession.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace bgmedia::mix {
namespace {

std::string utf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string fileNameOf(const std::filesystem::path& path) {
    std::string name = utf8(path.filename());
    return name.empty() ? utf8(path) : name;
}

} // namespace

MixSession::MixSession(MixEngine& engine, MixPrepareCallback prepare,
                       MixPlanner planner)
    : engine_(engine), prepare_(std::move(prepare)), planner_(std::move(planner)) {
    worker_ = std::thread([this] { workerLoop(); });
    applyStretchQuality();
}

MixSession::~MixSession() {
    {
        // Publish the wait predicate under its mutex, avoiding a missed wakeup.
        std::lock_guard lock(jobMutex_);
        stop_.store(true, std::memory_order_release);
    }
    jobCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::uint64_t MixSession::addTrack(const std::filesystem::path& path) {
    if (path.empty()) {
        return 0;
    }
    std::lock_guard lock(mutex_);
    Item item;
    item.id = nextId_++;
    item.path = path;
    item.name = fileNameOf(path);
    item.state = QueueState::Pending;
    items_.push_back(std::move(item));
    return items_.back().id;
}

std::vector<std::uint64_t> MixSession::addTracks(
    const std::vector<std::filesystem::path>& paths) {
    std::vector<std::uint64_t> ids;
    ids.reserve(paths.size());
    for (const auto& path : paths) {
        const std::uint64_t id = addTrack(path);
        if (id != 0) {
            ids.push_back(id);
        }
    }
    return ids;
}

MixSession::Item* MixSession::findItem(std::uint64_t id) {
    for (Item& item : items_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const MixSession::Item* MixSession::findItem(std::uint64_t id) const {
    for (const Item& item : items_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

bool MixSession::removeTrack(std::uint64_t id) {
    const MixSessionSnapshot snap = snapshot();
    if (id == 0 || id == snap.engine.decks[0].trackId ||
        id == snap.engine.decks[1].trackId) {
        return false;
    }
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [id](const Item& item) { return item.id == id; });
    if (it == items_.end()) {
        return false;
    }
    if (preparingId_ == id) cancelledJobId_.store(id, std::memory_order_release);
    items_.erase(it);
    return true;
}

void MixSession::clearQueue() {
    stopAll();
    {
        std::lock_guard lock(mutex_);
        if (preparingId_ != 0) cancelledJobId_.store(preparingId_, std::memory_order_release);
        items_.clear();
        currentId_ = 0;
        preview_ = {};
        previewIsExecution_ = false;
        status_ = "Queue cleared";
        clearDecksRequested_ = true;
    }
}

void MixSession::play() {
    autoPlay_ = true;
    engine_.setPaused(false);
    MixSnapshot snap = engine_.snapshot();
    DeckId master = snap.master;
    if (!snap.decks[deckIndex(master)].loaded) {
        std::lock_guard lock(mutex_);
        for (const Item& item : items_) {
            if (item.state == QueueState::Ready && item.audio) {
                Item& mutableItem = *findItem(item.id);
                installDeck(DeckId::A, mutableItem);
                currentId_ = item.id;
                master = DeckId::A;
                break;
            }
        }
    }
    snap = engine_.snapshot();
    const DeckStatus& deck = snap.decks[deckIndex(master)];
    if (deck.loaded) {
        if (deck.ended) {
            engine_.seek(master, 0.0);
        }
        engine_.setMaster(master);
        engine_.play(master);
        std::lock_guard lock(mutex_);
        status_ = "Playing";
    } else {
        std::lock_guard lock(mutex_);
        status_ = "Preparing the first track...";
    }
}

void MixSession::pause() {
    engine_.setPaused(true);
    std::lock_guard lock(mutex_);
    status_ = "Paused";
}

void MixSession::stopAll() {
    // Engine-side stop settles the decks and any active transition before the
    // paused early-out; pausing here and only clearing targetPlay left the
    // snapshots stuck on "playing" and blocked Clear Queue.
    engine_.stop();
    autoPlay_ = false;
    nextRequested_ = false;
    std::lock_guard lock(mutex_);
    status_ = "Stopped";
}

void MixSession::next() {
    nextRequested_ = true;
    engine_.setPaused(false);
    startPlannedTransition(duration_);
}

void MixSession::startTransitionNow() {
    engine_.setPaused(false);
    startPlannedTransition(duration_);
}

void MixSession::cancelTransition() {
    engine_.cancelTransition();
    std::lock_guard lock(mutex_);
    // A manual Next that is already running must not be re-armed after the
    // reverse fade: the user asked to cancel it, so drop the pending request.
    nextRequested_ = false;
    status_ = "Cancelling transition";
    planDirty_ = true;
    preview_ = {};
    previewIsExecution_ = false;
}

void MixSession::setTransitionType(TransitionType type) {
    type_ = type;
    if (type == TransitionType::TempoRamp) {
        tempoRampMode_ = TempoRampMode::Manual;
    }
    refreshPlan();
}

void MixSession::setTransitionDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
    refreshPlan();
}

void MixSession::setAdaptive(bool enabled) {
    adaptive_ = enabled;
    refreshPlan();
}

void MixSession::setTempoRampEnabled(bool enabled) {
    tempoRampMode_ = enabled ? TempoRampMode::Auto : TempoRampMode::Off;
    refreshPlan();
}

void MixSession::setTempoRampMode(TempoRampMode mode) {
    tempoRampMode_ = mode;
    refreshPlan();
}

void MixSession::setTempoRampMaxPercent(double percent) {
    rampMaxPercent_ = std::clamp(percent, 1.0, 16.0);
    refreshPlan();
}

void MixSession::setTempoRampBars(double bars) {
    rampBars_ = bars >= 1.0 ? std::min(32.0, bars) : 0.0;
    refreshPlan();
}

void MixSession::setTempoCurve(TempoCurveKind curve) {
    curve_ = curve;
    refreshPlan();
}

void MixSession::setPitchLock(bool enabled) {
    pitchLock_ = enabled;
    stretchDirty_ = true;
    applyStretchQuality();
    refreshPlan();
}

void MixSession::setQuality(StretchQuality quality) {
    quality_ = quality;
    stretchDirty_ = true;
    applyStretchQuality();
    refreshPlan();
}

void MixSession::setMixMode(MixMode mode) {
    mode_ = mode;
    if (mode == MixMode::Auto) {
        adaptive_ = true;
    } else if (mode == MixMode::Manual) {
        adaptive_ = false;
    }
    refreshPlan();
}

void MixSession::setAggressiveness(Aggressiveness aggressiveness) {
    aggressiveness_ = aggressiveness;
    refreshPlan();
}

void MixSession::setBassSwap(bool enabled) {
    bassSwap_ = enabled;
    refreshPlan();
}

void MixSession::setPhraseBars(int bars) {
    phraseBars_ = std::clamp(bars, 2, 16);
    refreshPlan();
}

void MixSession::setAutoQueue(bool enabled) {
    autoQueue_ = enabled;
}

void MixSession::applyStretchQuality() {
    const bool pitchPreserving = pitchLock_ && quality_ == StretchQuality::Quality;
    if (pitchPreserving == appliedPitchPreserving_) {
        stretchDirty_ = false;
        return;
    }
    // Rebuilding the stretch stream is safe only when no transition is being
    // rendered: the callback may still hold the old stream. Defer otherwise
    // and apply it from update() once the engine is idle again.
    if (engine_.snapshot().phase != TransitionPhase::Idle) {
        stretchDirty_ = true;
        return;
    }
    engine_.setTimeStretch(
        pitchPreserving ? std::shared_ptr<const ITimeStretchProcessor>(
                              std::make_shared<WsolaStretcher>())
                        : std::shared_ptr<const ITimeStretchProcessor>(
                              std::make_shared<LinearResampleStretcher>()));
    appliedPitchPreserving_ = pitchPreserving;
    stretchDirty_ = false;
}

std::uint64_t MixSession::loadedId(DeckId deck) const {
    return engine_.snapshot().decks[deckIndex(deck)].trackId;
}

DeckId MixSession::masterDeck() const {
    return engine_.snapshot().master;
}

double MixSession::deckPosition(DeckId deck) const {
    return engine_.snapshot().decks[deckIndex(deck)].positionSeconds;
}

double MixSession::deckDuration(DeckId deck) const {
    return engine_.snapshot().decks[deckIndex(deck)].durationSeconds;
}

std::shared_ptr<const AnalysisResult> MixSession::deckAnalysis(
    DeckId deck) const {
    const std::uint64_t id = loadedId(deck);
    std::lock_guard lock(mutex_);
    const Item* item = findItem(id);
    return item && item->analysis ? item->analysis
                                  : std::shared_ptr<const AnalysisResult>{};
}

std::shared_ptr<const MixFeatureMap> MixSession::deckFeatures(
    DeckId deck) const {
    const std::uint64_t id = loadedId(deck);
    std::lock_guard lock(mutex_);
    const Item* item = findItem(id);
    return item && item->features ? item->features
                                  : std::shared_ptr<const MixFeatureMap>{};
}

std::string MixSession::deckName(DeckId deck) const {
    const std::uint64_t id = loadedId(deck);
    std::lock_guard lock(mutex_);
    const Item* item = findItem(id);
    return item ? item->name : std::string{};
}

void MixSession::installDeck(DeckId deck, const Item& item) {
    if (!item.audio) {
        return;
    }
    const double bpm = item.analysis ? item.analysis->bpm : 0.0;
    if (engine_.loadDeck(deck, item.audio, item.id, bpm)) {
        status_ = "Loaded " + item.name;
        planDirty_ = true;
    } else {
        lastError_ = "Could not load " + item.name + " into a busy deck";
    }
}

void MixSession::startWorkerIfNeeded() {
    // A cleared or retired deck may still own PCM until a render callback acknowledges it.
    // Do not start preparing a third allocation while that old ownership is pending.
    if (clearDecksRequested_ || engine_.retiredPcmBytes() != 0) return;
    {
        std::lock_guard lock(jobMutex_);
        if (jobActive_ || jobRunning_.load(std::memory_order_acquire)) {
            return;
        }
    }
    std::lock_guard lock(mutex_);
    // The logical queue may contain thousands of paths. Only the two deck
    // slots may own prepared PCM (including a worker result not yet collected).
    // Loaded items retain their Ready state until transition completion.
    constexpr std::size_t kPreparedSlots = 2;
    const auto occupied = std::count_if(items_.begin(), items_.end(),
        [](const Item& item) {
            return item.audio != nullptr || item.state == QueueState::Preparing;
        });
    if (static_cast<std::size_t>(occupied) >= kPreparedSlots) {
        return;
    }
    for (Item& item : items_) {
        if (item.state == QueueState::Pending) {
            item.state = QueueState::Preparing;
            item.progress = 0.0f;
            preparingId_ = item.id;
            preparingProgress_.store(0.0f, std::memory_order_relaxed);
            std::lock_guard jobLock(jobMutex_);
            jobId_ = item.id;
            jobPath_ = item.path;
            jobActive_ = true;
            jobRunning_.store(true, std::memory_order_release);
            jobCv_.notify_one();
            return;
        }
    }
}

void MixSession::workerLoop() {
    for (;;) {
        std::uint64_t id = 0;
        std::filesystem::path path;
        {
            std::unique_lock lock(jobMutex_);
            jobCv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) ||
                                              jobActive_; });
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            id = jobId_;
            path = jobPath_;
            jobActive_ = false;
        }
        PreparedMixTrack prepared;
        std::string error;
        bool ok = false;
        const auto checkpoint = [this, id] {
            if (stop_.load(std::memory_order_acquire) ||
                cancelledJobId_.load(std::memory_order_acquire) == id) {
                throw std::runtime_error("mix preparation cancelled");
            }
        };
        try {
            checkpoint();
            ok = prepare_(
                path, prepared, error, [this, checkpoint](float progress, std::string_view stage) {
                    checkpoint();
                    preparingProgress_.store(std::clamp(progress, 0.0f, 1.0f),
                                             std::memory_order_relaxed);
                    if (!stage.empty()) {
                        std::lock_guard lock(mutex_);
                        preparingStage_ = std::string(stage);
                    }
                });
            checkpoint();
            if (ok && prepared.audio && prepared.analysis && !prepared.features) {
                MixFeatureExtractor extractor;
                prepared.features = std::make_shared<const MixFeatureMap>(
                    extractor.extractFromTrack(*prepared.audio, *prepared.analysis));
            }
            checkpoint();
        } catch (const std::exception& e) {
            ok = false;
            prepared = {};
            error = e.what();
        } catch (...) {
            ok = false;
            prepared = {};
            error = "track preparation threw an unknown exception";
        }
        if (stop_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lock(mutex_);
            completed_.push_back({id, ok, std::move(prepared), std::move(error)});
        }
        jobRunning_.store(false, std::memory_order_release);
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
    }
}

void MixSession::collectCompleted() {
    std::vector<CompletedJob> completed;
    {
        std::lock_guard lock(mutex_);
        completed.swap(completed_);
    }
    for (CompletedJob& job : completed) {
        std::lock_guard lock(mutex_);
        if (preparingId_ == job.id) {
            preparingId_ = 0;
        }
        Item* item = findItem(job.id);
        if (!item) {
            // The track was removed while it was being prepared.
            continue;
        }
        if (job.ok && job.prepared.audio && job.prepared.analysis) {
            item->audio = std::move(job.prepared.audio);
            item->analysis = std::move(job.prepared.analysis);
            item->features = std::move(job.prepared.features);
            item->state = QueueState::Ready;
            item->progress = 1.0f;
            status_ = "Ready: " + item->name;
            planDirty_ = true;
        } else {
            item->state = QueueState::Failed;
            item->error = job.error.empty() ? "preparation failed" : job.error;
            lastError_ = item->error;
        }
    }
}

void MixSession::handleTransitionCompletion() {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.completedTransitions <= lastCompleted_) {
        return;
    }
    lastCompleted_ = snap.completedTransitions;
    {
        std::lock_guard lock(mutex_);
        planDirty_ = true;
        preview_ = {};
        previewIsExecution_ = false;
    }
    const DeckId master = snap.master;
    const DeckId old = otherDeck(master);
    {
        std::lock_guard lock(mutex_);
        Item* oldItem = findItem(snap.decks[deckIndex(old)].trackId);
        if (oldItem && !snap.decks[deckIndex(old)].loaded) {
            oldItem->state = QueueState::Played;
        } else if (oldItem) {
            oldItem->state = QueueState::Played;
        }
        currentId_ = snap.decks[deckIndex(master)].trackId;
        nextRequested_ = false;
        hasExecutedPlan_ = false;
        planDirty_ = true;
        lastError_.clear();
        // A preparation failure collected on this very tick is not part of
        // the completed transition: keep it visible.
        for (const Item& queued : items_) {
            if (queued.state == QueueState::Failed && !queued.error.empty()) {
                lastError_ = queued.error;
                break;
            }
        }
        status_ = "Transition complete";
    }
    engine_.play(master);
    // The outgoing deck is silent after the fade. Clear it so the queue can
    // promote the next prepared track into it.
    if (!snap.decks[deckIndex(old)].playing) {
        engine_.clearDeck(old);
    }
    std::lock_guard lock(mutex_);
    const std::uint64_t a = loadedId(DeckId::A);
    const std::uint64_t b = loadedId(DeckId::B);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Item& item) {
                                    return item.state == QueueState::Played &&
                                           item.id != a && item.id != b;
                                }),
                 items_.end());
}

void MixSession::pruneFinishedItems() {
    // A track that has been played and is no longer on a deck leaves the queue
    // right away, not only at the next completed transition.
    std::lock_guard lock(mutex_);
    const std::uint64_t a = loadedId(DeckId::A);
    const std::uint64_t b = loadedId(DeckId::B);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Item& item) {
                                    return item.state == QueueState::Played &&
                                           item.id != a && item.id != b;
                                }),
                 items_.end());
}

void MixSession::serviceDecks() {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.phase != TransitionPhase::Idle) {
        return;
    }

    if (clearDecksRequested_) {
        if (!snap.decks[0].playing && !snap.decks[1].playing) {
            engine_.clearDeck(DeckId::A);
            engine_.clearDeck(DeckId::B);
            clearDecksRequested_ = false;
        }
        return;
    }

    const bool loadedA = snap.decks[0].loaded;
    const bool loadedB = snap.decks[1].loaded;
    std::uint64_t installId = 0;
    bool installA = false;

    {
        std::lock_guard lock(mutex_);
        auto pickReady = [&](std::uint64_t avoidA,
                             std::uint64_t avoidB) -> Item* {
            for (Item& item : items_) {
                if (item.state == QueueState::Ready && item.audio &&
                    item.id != avoidA && item.id != avoidB) {
                    return &item;
                }
            }
            return nullptr;
        };
        if (!loadedA && !loadedB) {
            if (Item* item = pickReady(0, 0)) {
                installA = true;
                installId = item->id;
            }
        } else if (!loadedB) {
            if (Item* item = pickReady(snap.decks[0].trackId, 0)) {
                installId = item->id;
            }
        } else if (!loadedA) {
            if (Item* item = pickReady(0, snap.decks[1].trackId)) {
                installA = true;
                installId = item->id;
            }
        }
    }

    if (installId != 0) {
        std::lock_guard lock(mutex_);
        Item* item = findItem(installId);
        if (item) {
            const DeckId deck = installA ? DeckId::A : DeckId::B;
            installDeck(deck, *item);
            if (deck == DeckId::A) {
                currentId_ = item->id;
            }
            // Only the very first usable track may auto-start. Refilling the
            // outgoing deck after a handover must stay stopped: the incoming
            // track is still playing and owns the master role.
            const bool anyPlaying = snap.decks[0].playing ||
                                    snap.decks[1].playing;
            if (autoPlay_ && deck == DeckId::A && !anyPlaying && !snap.paused) {
                engine_.play(DeckId::A);
                engine_.setMaster(DeckId::A);
            }
        }
    }

    // Manual "Next" pressed before the incoming track finished preparing.
    if (nextRequested_) {
        const MixSnapshot after = engine_.snapshot();
        if (after.decks[0].loaded && after.decks[1].loaded) {
            nextRequested_ = false;
            startPlannedTransition(duration_);
        }
    }
}

MixPlan MixSession::buildPlan(DeckId from, DeckId to, const MixSnapshot& snap,
                              double durationOverride, bool execution) {
    const Item* a = findItem(snap.decks[deckIndex(from)].trackId);
    const Item* b = findItem(snap.decks[deckIndex(to)].trackId);
    if (!a || !b || !a->analysis || !b->analysis) {
        return {};
    }
    PlannerConfig config = planner_.config();
    config.adaptive = adaptive_;
    config.tempoRampMode = tempoRampMode_;
    config.allowTempoRamp = tempoRampMode_ != TempoRampMode::Off;
    config.tempoRampBars = rampBars_;
    config.tempoCurveKind = curve_;
    config.maxTempoDeltaPercent = rampMaxPercent_;
    // Pitch lock is only real in the quality mode; the performance mode uses
    // linear resampling, so the planner must warn about pitch shift.
    config.pitchLock =
        pitchLock_ && quality_ == StretchQuality::Quality;
    config.aggressiveness = aggressiveness_;
    config.bassSwap = bassSwap_;
    config.phraseBars = phraseBars_;
    const double override = std::max(0.0, durationOverride);
    if (override > 0.0) {
        config.defaultCrossfadeSeconds = override;
        config.minCrossfadeSeconds = 0.0;
        config.maxCrossfadeSeconds = std::max(override, 32.0);
    }
    MixPlanner planner(config);
    PlanRequest request;
    request.type = type_;
    request.requestedDuration = override;
    // A preview deliberately plans the end-of-track window. Executing a
    // transition (automatic or manual Next) must start where the outgoing
    // deck is actually playing, otherwise the engine seeks it to the end.
    request.currentPositionA =
        execution ? snap.decks[deckIndex(from)].positionSeconds : -1.0;
    request.autoStart = execution;
    request.trackNameB = b->name;
    MixPlan plan = planner.plan(*a->analysis, *b->analysis,
                                a->features.get(), b->features.get(), request);
    plan.from = from;
    plan.to = to;
    return plan;
}

bool MixSession::seekDeck(DeckId deck, double seconds) {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.phase != TransitionPhase::Idle) {
        std::lock_guard lock(mutex_);
        lastError_ = "Cannot seek during an active transition";
        return false;
    }
    const int idx = deckIndex(deck);
    if (!snap.decks[idx].loaded || snap.decks[idx].durationSeconds <= 0.0) {
        return false;
    }
    const double target = std::clamp(seconds, 0.0, snap.decks[idx].durationSeconds);
    engine_.seek(deck, target);
    if (deck == snap.master) {
        std::lock_guard lock(mutex_);
        planDirty_ = true;
        preview_ = {};
        previewIsExecution_ = false;
    }
    return true;
}

bool MixSession::jumpToPlannedTransition(double leadSeconds) {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.phase != TransitionPhase::Idle) {
        std::lock_guard lock(mutex_);
        lastError_ = "Cannot jump during an active transition";
        return false;
    }
    MixPlan prev;
    {
        std::lock_guard lock(mutex_);
        prev = preview_;
    }
    if (!prev.valid || prev.from != snap.master) {
        return false;
    }
    const double sched = prev.overallStartA();
    const double target = std::max(0.0, sched - std::max(1.0, leadSeconds));
    return seekDeck(snap.master, target);
}

void MixSession::invalidatePlan() {
    std::lock_guard lock(mutex_);
    planDirty_ = true;
}

void MixSession::ensurePlanValid() {
    std::lock_guard lock(mutex_);
    const MixSnapshot snap = engine_.snapshot();
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    if (engine_.hasPendingSeek(from) || engine_.hasPendingSeek(to)) {
        return;
    }
    const std::uint64_t fromId = snap.decks[deckIndex(from)].trackId;
    const std::uint64_t toId = snap.decks[deckIndex(to)].trackId;
    const double livePosA = snap.decks[deckIndex(from)].positionSeconds;

    // Fast-path: both decks unchanged, configuration unchanged, plan not dirty,
    // not an emergency execution plan, and live cursor has not passed the scheduled transition window.
    const bool sameDecks = fromId == cachedFromId_ && toId == cachedToId_ &&
                           from == cachedMasterDeck_;
    if (!planDirty_ && preview_.valid && !previewIsExecution_ && sameDecks &&
        livePosA < preview_.overallStartA() + 0.5) {
        return;
    }
    // While a transition is running the plan must not be rebuilt: the planner
    // is expensive and the running transition owns its own copy of the plan.
    if (!planDirty_ && preview_.valid && sameDecks &&
        snap.phase != TransitionPhase::Idle) {
        return;
    }
    // An expired-window (execution) plan is anchored to the live cursor; only
    // rebuild it after the cursor moved noticeably, not on every update().
    if (!planDirty_ && preview_.valid && previewIsExecution_ && sameDecks &&
        std::fabs(livePosA - executionPlanPosA_) < 0.5) {
        return;
    }
    if (snap.decks[deckIndex(from)].loaded && snap.decks[deckIndex(to)].loaded) {
        // Build the planned ending transition first
        preview_ = buildPlan(from, to, snap, duration_, false);
        previewIsExecution_ = false;

        // Only if the live playback cursor has already passed the planned window,
        // re-plan from the live cursor so the transition fits the remaining audio.
        if (preview_.valid && livePosA > 0.0 && livePosA >= preview_.overallStartA() + 0.5) {
            preview_ = buildPlan(from, to, snap, 0.0, true);
            previewIsExecution_ = true;
            executionPlanPosA_ = livePosA;
        }
    } else {
        preview_ = {};
        previewIsExecution_ = false;
    }
    cachedFromId_ = fromId;
    cachedToId_ = toId;
    cachedMasterDeck_ = from;
    planDirty_ = false;
}

void MixSession::refreshPlan() {
    std::lock_guard lock(mutex_);
    const MixSnapshot snap = engine_.snapshot();
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    preview_ = {};
    previewIsExecution_ = false;
    if (snap.decks[deckIndex(from)].loaded && snap.decks[deckIndex(to)].loaded) {
        preview_ = buildPlan(from, to, snap, duration_, false);
    }
    cachedFromId_ = snap.decks[deckIndex(from)].trackId;
    cachedToId_ = snap.decks[deckIndex(to)].trackId;
    cachedMasterDeck_ = from;
    planDirty_ = false;
}

void MixSession::startPlannedTransition(double durationOverride) {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.phase != TransitionPhase::Idle) {
        return;
    }
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    if (!snap.decks[deckIndex(from)].loaded ||
        !snap.decks[deckIndex(to)].loaded) {
        nextRequested_ = true;
        std::lock_guard lock(mutex_);
        status_ = "Waiting for the next track to finish preparing";
        return;
    }

    MixPlan plan;
    {
        std::lock_guard lock(mutex_);
        plan = buildPlan(from, to, snap, durationOverride, true);
    }
    if (!plan.valid) {
        std::lock_guard lock(mutex_);
        lastError_ = plan.fallbackReason.empty() ? "transition plan is not valid"
                                                 : plan.fallbackReason;
        status_ = "Transition unavailable";
        return;
    }
    char reasonBuf[256];
    std::snprintf(reasonBuf, sizeof(reasonBuf),
                  "Manual NEXT transition from live position %.3f s",
                  snap.decks[deckIndex(from)].positionSeconds);
    plan.reasons.push_back(reasonBuf);
    {
        std::lock_guard lock(mutex_);
        preview_ = plan;
        lastExecutedPlan_ = plan;
        hasExecutedPlan_ = true;
    }
    if (!engine_.startTransition(plan)) {
        std::lock_guard lock(mutex_);
        lastError_ = "transition could not start (engine busy or tracks missing)";
        status_ = lastError_;
        return;
    }
    std::lock_guard lock(mutex_);
    // The request has been consumed. Leaving it set made a later Cancel start
    // another transition as soon as the reverse fade finished.
    nextRequested_ = false;
    status_ = std::string("Mixing: ") + transitionTypeName(plan.type);
}

void MixSession::executeScheduledTransition(const MixPlan& plannedPreview) {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.phase != TransitionPhase::Idle) {
        return;
    }
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    if (!snap.decks[deckIndex(from)].loaded || !snap.decks[deckIndex(to)].loaded) {
        return;
    }

    // Preserve the validated preview plan winner (cues, duration, strategy, gain curves, trims)
    MixPlan plan = plannedPreview;
    plan.from = from;
    plan.to = to;

    const double livePosA = snap.decks[deckIndex(from)].positionSeconds;
    const double remainingA = std::max(0.0, snap.decks[deckIndex(from)].durationSeconds - livePosA);
    if (livePosA > plan.overallStartA() + 0.5 && plan.duration > remainingA) {
        // Replan from the live position when the scheduled window has expired
        // so the fade fits the remaining audio.
        MixPlan refitted;
        {
            std::lock_guard lock(mutex_);
            refitted = buildPlan(from, to, snap, 0.0, true);
        }
        if (refitted.valid) {
            plan = refitted;
        } else {
            plan.duration = std::max(0.05, remainingA);
            plan.transitionStartA = livePosA;
            plan.transitionEndA = livePosA + plan.duration;
        }
    }
    char reasonBuf[256];
    std::snprintf(reasonBuf, sizeof(reasonBuf),
                  "Executing planned transition at live A %.2f s (strategy %s, cues A %.2f -> B %.2f, dur %.1f s)",
                  livePosA, mixStrategyKindName(plan.strategy), plan.transitionStartA, plan.startB, plan.duration);
    plan.reasons.push_back(reasonBuf);

    {
        std::lock_guard lock(mutex_);
        preview_ = plan;
        lastExecutedPlan_ = plan;
        hasExecutedPlan_ = true;
    }
    if (!engine_.startTransition(plan)) {
        std::lock_guard lock(mutex_);
        lastError_ = "scheduled transition could not start (engine busy or tracks missing)";
        status_ = lastError_;
        return;
    }
    std::lock_guard lock(mutex_);
    nextRequested_ = false;
    lastError_.clear();
    status_ = std::string("Mixing: ") + transitionTypeName(plan.type) +
              " (" + mixStrategyKindName(plan.strategy) + ")";
}

void MixSession::promoteEndedMaster(const MixSnapshot& snap) {
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    const DeckStatus& next = snap.decks[deckIndex(to)];
    if (!next.loaded) {
        return;
    }
    if (snap.paused) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        Item* oldItem = findItem(snap.decks[deckIndex(from)].trackId);
        if (oldItem) {
            oldItem->state = QueueState::Played;
        }
        currentId_ = next.trackId;
        status_ = "Track ended: starting the prepared track";
    }
    engine_.setMaster(to);
    engine_.play(to);
    // Release the finished deck so serviceDecks() can refill it with the next
    // prepared queue item.
    engine_.clearDeck(from);
    {
        std::lock_guard lock(mutex_);
        hasExecutedPlan_ = false;
        planDirty_ = true;
    }
}

void MixSession::update() {
    engine_.reclaimRetired();
    if (stretchDirty_) {
        applyStretchQuality();
    }
    collectCompleted();
    startWorkerIfNeeded();
    handleTransitionCompletion();
    pruneFinishedItems();
    serviceDecks();

    {
        std::lock_guard lock(mutex_);
        if (preparingId_ != 0) {
            if (Item* item = findItem(preparingId_)) {
                item->progress =
                    preparingProgress_.load(std::memory_order_relaxed);
            }
        }
    }

    ensurePlanValid();

    const MixSnapshot snap = engine_.snapshot();
    // Auto scheduling only runs in Auto mode. Manual and Preview still allow
    // the user to start a transition explicitly, and EOF promotion below keeps
    // playback continuous if nobody does.
    if (snap.phase != TransitionPhase::Idle || !autoQueue_ || !autoPlay_) {
        return;
    }
    const DeckId from = snap.master;
    const DeckStatus& deck = snap.decks[deckIndex(from)];
    // Promote only on a real EOF. Treating the last 50 ms as "ended" promoted
    // the prepared deck while the master was still playing; clearDeck() then
    // failed (the deck was still the play target), so the played track stayed
    // loaded and blocked the next queue item.
    if (deck.ended) {
        promoteEndedMaster(snap);
        return;
    }
    // Scheduling a transition is Auto-only; Manual and Preview wait for the
    // user (EOF promotion above still keeps playback continuous).
    if (mode_ != MixMode::Auto || !deck.playing) {
        return;
    }
    if (!snap.decks[0].loaded || !snap.decks[1].loaded) {
        return;
    }
    MixPlan preview;
    {
        std::lock_guard lock(mutex_);
        preview = preview_;
    }
    // Plan the fade against the outgoing window selected by the preview, not
    // against the physical file end. The planner deliberately ends the window
    // before detected trailing silence, so waiting for `duration - fade` would
    // start the incoming deck after the outgoing track has already gone silent.
    // A Direct Cut is a valid zero-duration plan; it must be submitted too,
    // otherwise automatic handover falls back to EOF promotion and loses the
    // planned incoming marker.
    constexpr double kLeadSeconds = 0.35;
    constexpr double kCutLeadSeconds = 0.02;
    const bool directCut = preview.type == TransitionType::DirectCut;
    const bool schedulable = preview.duration > 0.0 || directCut;
    const double lead = directCut ? kCutLeadSeconds : kLeadSeconds;
    // The ramp starts before the crossfade, so schedule from the overall start
    // rather than from transitionStartA.
    const double scheduleAt = preview.overallStartA();
    if (preview.valid && preview.from == from && schedulable &&
        !engine_.hasPendingSeek(from) && !engine_.hasPendingSeek(otherDeck(from)) &&
        deck.positionSeconds >= scheduleAt - lead) {
        if (deck.positionSeconds > scheduleAt + 0.5) {
            // Replan from the live position when the scheduled window has expired
            // so the fade fits the remaining audio.
            startPlannedTransition(0.0);
        } else {
            executeScheduledTransition(preview);
        }
    }
}

MixSessionSnapshot MixSession::snapshot() const {
    MixSessionSnapshot out;
    out.engine = engine_.snapshot();
    std::lock_guard lock(mutex_);
    out.queue.reserve(items_.size());
    out.retiredPcmBytes = engine_.retiredPcmBytes();
    for (const auto& job : completed_) {
        if (job.prepared.audio) out.preparedPcmBytes += static_cast<std::uint64_t>(job.prepared.audio->samples.capacity()) * sizeof(float);
    }
    const std::uint64_t loaded[2] = {
        out.engine.decks[0].trackId,
        out.engine.decks[1].trackId,
    };
    for (const Item& item : items_) {
        if (item.audio) {
            ++out.preparedCount;
            out.preparedPcmBytes += static_cast<std::uint64_t>(item.audio->samples.capacity()) * sizeof(float);
        }
        if (item.state == QueueState::Preparing) ++out.preparingCount;
        QueueItemStatus status;
        status.id = item.id;
        status.displayName = item.name;
        status.path = utf8(item.path);
        status.state = item.state;
        if (item.id == loaded[0] || item.id == loaded[1]) {
            status.state = QueueState::Playing;
        }
        status.bpm = item.analysis ? item.analysis->bpm : 0.0;
        status.durationSeconds = item.analysis ? item.analysis->info.duration : 0.0;
        status.progress = item.progress;
        status.error = item.error;
        status.experimentalTempo =
            item.analysis ? !item.analysis->tempoStable : false;
        out.queue.push_back(std::move(status));
    }
    out.transitionAvailable = out.engine.decks[0].loaded &&
                              out.engine.decks[1].loaded &&
                              out.engine.phase == TransitionPhase::Idle &&
                              type_ != TransitionType::Off;
    out.preparingNext = preparingId_ != 0;
    out.preparingName.clear();
    if (preparingId_ != 0) {
        const Item* item = findItem(preparingId_);
        out.preparingName = item ? item->name : std::string{};
        out.preparingProgress = preparingProgress_.load(std::memory_order_relaxed);
    }
    out.preview = preview_;
    out.executed = lastExecutedPlan_;
    out.hasExecutedPlan = hasExecutedPlan_;
    out.status = status_;
    out.lastError = lastError_;
    return out;
}

} // namespace bgmedia::mix
