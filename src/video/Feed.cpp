#include "bgmedia/video/Feed.hpp"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bgmedia::video {
Prepared LocalProvider::prepare(const Media& media, std::size_t limit, std::stop_token stop) {
    std::ifstream input(media.localPath, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open local media");
    Prepared result{media.id, {}};
    constexpr std::size_t chunk = 16384;
    while (result.prefix.size() < limit && !stop.stop_requested()) {
        const auto count = std::min(chunk, limit - result.prefix.size());
        const auto old = result.prefix.size();
        result.prefix.resize(old + count);
        input.read(reinterpret_cast<char*>(result.prefix.data() + old), static_cast<std::streamsize>(count));
        result.prefix.resize(old + static_cast<std::size_t>(input.gcount()));
        if (input.bad()) throw std::runtime_error("local media read failed");
        if (input.eof()) break;
        if (input.fail()) throw std::runtime_error("local media read failed");
    }
    return result;
}

struct Feed::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Provider> provider;
    std::vector<Media> queue;
    std::size_t budget;
    Snapshot state;
    std::uint64_t generation = 0;
    bool pending = false;
    bool quitting = false;
    std::stop_source cancellation;
    std::thread worker;
    Impl(std::shared_ptr<Provider> p, std::size_t b) : provider(std::move(p)), budget(b), worker([this] { run(); }) {}
    void schedule() {
        cancellation.request_stop();
        cancellation = std::stop_source{};
        ++generation;
        state.next.reset();
        state.error.clear();
        pending = state.current && *state.current + 1 < queue.size() && budget > 0;
        state.preparation = pending ? Preparation::loading : Preparation::idle;
        cv.notify_one();
    }
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            cv.wait(lock, [this] { return pending || quitting; });
            if (quitting) return;
            auto item = queue[*state.current + 1];
            auto bytes = budget;
            auto token = cancellation.get_token();
            auto version = generation;
            pending = false;
            lock.unlock();
            std::shared_ptr<Prepared> result;
            std::string error;
            try { result = std::make_shared<Prepared>(provider->prepare(item, bytes, token)); }
            catch (const std::exception& e) { error = e.what(); }
            catch (...) { error = "provider failed"; }
            lock.lock();
            if (version != generation || token.stop_requested()) continue;
            if (result && (result->id != item.id || result->prefix.size() > bytes)) {
                result.reset();
                error = "provider violated preparation contract";
            }
            state.next = std::move(result);
            state.error = std::move(error);
            state.preparation = state.next ? Preparation::ready : Preparation::error;
        }
    }
    ~Impl() {
        { std::lock_guard lock(mutex); quitting = true; cancellation.request_stop(); cv.notify_one(); }
        worker.join();
    }
};
Feed::Feed(std::shared_ptr<Provider> provider, std::size_t budget) {
    if (!provider) throw std::invalid_argument("provider is null");
    impl_ = std::make_unique<Impl>(std::move(provider), budget);
}
Feed::~Feed() = default;
void Feed::setQueue(std::vector<Media> q) {
    std::lock_guard lock(impl_->mutex);
    impl_->queue = std::move(q);
    impl_->state.current = impl_->queue.empty() ? std::nullopt : std::optional<std::size_t>{0};
    impl_->schedule();
}
bool Feed::select(std::size_t index) {
    std::lock_guard lock(impl_->mutex);
    if (index >= impl_->queue.size()) return false;
    impl_->state.current = index;
    impl_->schedule();
    return true;
}
bool Feed::next() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.current || *impl_->state.current + 1 >= impl_->queue.size()) return false;
    ++*impl_->state.current; impl_->schedule(); return true;
}
bool Feed::previous() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->state.current || *impl_->state.current == 0) return false;
    --*impl_->state.current; impl_->schedule(); return true;
}
void Feed::setBudget(std::size_t budget) { std::lock_guard lock(impl_->mutex); impl_->budget = budget; impl_->schedule(); }
void Feed::close() { setQueue({}); }
Snapshot Feed::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
}
