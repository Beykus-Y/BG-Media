#include "bgmedia/video/Feed.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bgmedia::video {
Prepared LocalProvider::prepare(const Media& media, std::size_t limit, std::stop_token stop) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(media.localPath, ec))
        throw std::runtime_error("local media is not a regular file");
    std::ifstream input(media.localPath, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open local media");
    Prepared result{media.id, {}, {}};
    const auto size = std::filesystem::file_size(media.localPath, ec);
    if (ec || size == 0) throw std::runtime_error("cannot read local media size");
    limit = static_cast<std::size_t>(std::min<std::uintmax_t>(limit, size));
    result.prefix.reserve(limit);
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
    std::unique_ptr<Prepared> prepared;
    std::uint64_t generation = 0;
    bool pending = false;
    bool quitting = false;
    bool exited = false;
    std::stop_source cancellation;
    std::thread worker;
    Impl(std::shared_ptr<Provider> p, std::size_t b) : provider(std::move(p)), budget(b) {}
    void schedule() {
        cancellation.request_stop();
        cancellation = std::stop_source{};
        ++generation;
        state.next.reset();
        prepared.reset();
        state.error.clear();
        pending = state.current && *state.current + 1 < queue.size() && budget > 0;
        state.preparation = pending ? Preparation::loading : Preparation::idle;
        cv.notify_one();
    }
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            cv.wait(lock, [this] { return pending || quitting; });
            if (quitting) {
                exited = true;
                cv.notify_all();
                return;
            }
            auto item = queue[*state.current + 1];
            auto bytes = budget;
            auto token = cancellation.get_token();
            auto version = generation;
            pending = false;
            lock.unlock();
            std::unique_ptr<Prepared> result;
            std::string error;
            try { result = std::make_unique<Prepared>(provider->prepare(item, bytes, token)); }
            catch (const std::exception& e) { error = e.what(); }
            catch (...) { error = "provider failed"; }
            lock.lock();
            if (version != generation || token.stop_requested()) continue;
            const auto decodedBytes = result && result->decoder ? result->decoder->bufferedBytes() : 0;
            if (result && (result->id != item.id || result->prefix.capacity() > bytes ||
                           decodedBytes > bytes - result->prefix.capacity())) {
                result.reset();
                error = "provider violated preparation contract";
            }
            prepared = std::move(result);
            if (prepared) state.next = PreparedInfo{prepared->id, prepared->prefix.capacity() + decodedBytes};
            state.error = std::move(error);
            state.preparation = state.next ? Preparation::ready : Preparation::error;
        }
    }
    void shutdown() {
        std::unique_lock lock(mutex);
        quitting = true;
        queue.clear();
        prepared.reset();
        cancellation.request_stop();
        cv.notify_one();
        const bool finished = cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return exited; });
        lock.unlock();
        // The worker owns shared state and its provider. A broken provider can
        // retain its one call after timeout, but cannot touch the destroyed Feed.
        if (finished) worker.join();
        else worker.detach();
    }
};
Feed::Feed(std::shared_ptr<Provider> provider, std::size_t budget) {
    if (!provider) throw std::invalid_argument("provider is null");
    impl_ = std::make_shared<Impl>(std::move(provider), budget);
    impl_->worker = std::thread([state = impl_] { state->run(); });
}
Feed::~Feed() { impl_->shutdown(); }
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
std::optional<Feed::Selection> Feed::activate(std::size_t index) {
    std::lock_guard lock(impl_->mutex);
    if (index >= impl_->queue.size()) return std::nullopt;
    Selection selection{impl_->queue[index], {}};
    if (impl_->state.current && index == *impl_->state.current + 1 && impl_->prepared)
        selection.decoder = std::move(impl_->prepared->decoder);
    impl_->state.current = index;
    impl_->schedule();
    return selection;
}
Snapshot Feed::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
}
