#include "bgmedia/mix/MixSession.hpp"

#include <algorithm>
#include <chrono>

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
}

MixSession::~MixSession() {
    stop_.store(true, std::memory_order_release);
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
    items_.erase(it);
    return true;
}

void MixSession::clearQueue() {
    stopAll();
    {
        std::lock_guard lock(mutex_);
        items_.clear();
        currentId_ = 0;
        preview_ = {};
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
}

void MixSession::setTransitionType(TransitionType type) {
    type_ = type;
    if (type == TransitionType::TempoRamp) {
        tempoRamp_ = true;
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
    tempoRamp_ = enabled;
    refreshPlan();
}

void MixSession::setTempoRampMaxPercent(double percent) {
    rampMaxPercent_ = std::clamp(percent, 1.0, 16.0);
    refreshPlan();
}

void MixSession::setAutoQueue(bool enabled) {
    autoQueue_ = enabled;
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
    } else {
        lastError_ = "Could not load " + item.name + " into a busy deck";
    }
}

void MixSession::startWorkerIfNeeded() {
    {
        std::lock_guard lock(jobMutex_);
        if (jobActive_ || jobRunning_.load(std::memory_order_acquire)) {
            return;
        }
    }
    std::lock_guard lock(mutex_);
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
        const bool ok = prepare_(
            path, prepared, error, [this](float progress, std::string_view stage) {
                preparingProgress_.store(std::clamp(progress, 0.0f, 1.0f),
                                         std::memory_order_relaxed);
                if (!stage.empty()) {
                    std::lock_guard lock(mutex_);
                    preparingStage_ = std::string(stage);
                }
            });
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
        Item* item = findItem(job.id);
        if (!item) {
            continue;
        }
        if (job.ok && job.prepared.audio && job.prepared.analysis) {
            item->audio = std::move(job.prepared.audio);
            item->analysis = std::move(job.prepared.analysis);
            item->state = QueueState::Ready;
            item->progress = 1.0f;
            status_ = "Ready: " + item->name;
        } else {
            item->state = QueueState::Failed;
            item->error = job.error.empty() ? "preparation failed" : job.error;
            lastError_ = item->error;
        }
        if (preparingId_ == job.id) {
            preparingId_ = 0;
        }
    }
}

void MixSession::handleTransitionCompletion() {
    const MixSnapshot snap = engine_.snapshot();
    if (snap.completedTransitions <= lastCompleted_) {
        return;
    }
    lastCompleted_ = snap.completedTransitions;
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
    config.allowTempoRamp = tempoRamp_;
    config.maxTempoDeltaPercent = rampMaxPercent_;
    const double override = std::max(0.0, durationOverride);
    if (override > 0.0) {
        config.defaultCrossfadeSeconds = override;
        config.minCrossfadeSeconds = 0.0;
        config.maxCrossfadeSeconds = std::max(override, 32.0);
    }
    MixPlanner planner(config);
    PlanRequest request;
    request.type = adaptive_ ? type_ : TransitionType::Crossfade;
    request.requestedDuration = override;
    // A preview deliberately plans the end-of-track window. Executing a
    // transition (automatic or manual Next) must start where the outgoing
    // deck is actually playing, otherwise the engine seeks it to the end.
    request.currentPositionA =
        execution ? snap.decks[deckIndex(from)].positionSeconds : -1.0;
    request.autoStart = execution;
    request.trackNameB = b->name;
    MixPlan plan = planner.plan(*a->analysis, *b->analysis, request);
    plan.from = from;
    plan.to = to;
    return plan;
}

void MixSession::refreshPlan() {
    std::lock_guard lock(mutex_);
    preview_ = {};
    const MixSnapshot snap = engine_.snapshot();
    const DeckId from = snap.master;
    const DeckId to = otherDeck(from);
    preview_ = buildPlan(from, to, snap, duration_, false);
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
    {
        std::lock_guard lock(mutex_);
        preview_ = plan;
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
}

void MixSession::update() {
    engine_.reclaimRetired();
    collectCompleted();
    startWorkerIfNeeded();
    handleTransitionCompletion();
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

    refreshPlan();

    const MixSnapshot snap = engine_.snapshot();
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
    if (!deck.playing) {
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
    if (preview.valid && preview.from == from && schedulable &&
        deck.positionSeconds >= preview.transitionStartA - lead) {
        startPlannedTransition(duration_);
    }
}

MixSessionSnapshot MixSession::snapshot() const {
    MixSessionSnapshot out;
    out.engine = engine_.snapshot();
    std::lock_guard lock(mutex_);
    out.queue.reserve(items_.size());
    const std::uint64_t loaded[2] = {
        out.engine.decks[0].trackId,
        out.engine.decks[1].trackId,
    };
    for (const Item& item : items_) {
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
    out.status = status_;
    out.lastError = lastError_;
    return out;
}

} // namespace bgmedia::mix
