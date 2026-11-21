#include "bgmedia/video/Feed.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace bgmedia::video;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " << #x << " line " << __LINE__ << '\n'; std::exit(1); } } while (false)
struct Oversized final : Provider {
    Prepared prepare(const Media& m, std::size_t limit, std::stop_token) override {
        Prepared result{m.id, {}, {}};
        result.prefix.reserve(limit + 1);
        return result;
    }
};
struct IgnoresCancellation final : Provider {
    std::atomic<bool> entered{false}, release{false}, returned{false};
    Prepared prepare(const Media& m, std::size_t, std::stop_token) override {
        entered = true;
        while (!release) std::this_thread::sleep_for(1ms);
        returned = true;
        return {m.id, {}, {}};
    }
};
struct Blocking final : Provider {
    std::atomic<bool> entered{false}, cancelled{false};
    Prepared prepare(const Media& m, std::size_t, std::stop_token token) override {
        if (m.id.value == "1") {
            entered = true;
            while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
            cancelled = true;
        }
        return {m.id, {}, {}};
    }
};
template<class F> void waitFor(F f) {
    auto end = std::chrono::steady_clock::now() + 3s;
    while (!f() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    CHECK(f());
}
int main() {
    auto path = std::filesystem::current_path() / "bgmedia-video-feed-test.bin";
    { std::ofstream out(path, std::ios::binary); out << "0123456789"; }
    std::vector<Media> queue;
    for (int i = 0; i < 4; ++i) queue.push_back({{"provider", "account", std::to_string(i)}, path});
    {
        Feed feed(std::make_shared<LocalProvider>(), 4);
        feed.setQueue(queue);
        waitFor([&] { return feed.snapshot().preparation == Preparation::ready; });
        auto snapshot = feed.snapshot();
        CHECK(snapshot.next->id.value == "1"); CHECK(snapshot.next->bytes == 4);
        CHECK(feed.next()); CHECK(feed.previous()); CHECK(!feed.previous()); CHECK(!feed.select(99));
        feed.setBudget(0); CHECK(feed.snapshot().preparation == Preparation::idle);
        // Retaining the old metadata does not keep the prepared byte buffer alive.
        CHECK(snapshot.next->bytes == 4);
        feed.setBudget(20);
        waitFor([&] { return feed.snapshot().preparation == Preparation::ready; });
        CHECK(feed.snapshot().next->bytes == 10);
        feed.close(); CHECK(!feed.snapshot().current); CHECK(!feed.snapshot().next);
        queue[1].localPath = path.string() + ".missing";
        feed.setQueue(queue);
        waitFor([&] { return feed.snapshot().preparation == Preparation::error; });
        CHECK(!feed.snapshot().error.empty());
    }
    {
        auto provider = std::make_shared<Blocking>(); Feed feed(provider, 10); feed.setQueue(queue);
        waitFor([&] { return provider->entered.load(); });
        CHECK(feed.select(2));
        waitFor([&] { return feed.snapshot().preparation == Preparation::ready; });
        CHECK(provider->cancelled); CHECK(feed.snapshot().next->id.value == "3");
        std::thread a([&] { for (int i=0;i<1000;++i) feed.select(2); });
        std::thread b([&] { for (int i=0;i<1000;++i) (void)feed.snapshot(); });
        a.join(); b.join(); feed.close();
    }
    {
        Feed feed(std::make_shared<Oversized>(), 4);
        feed.setQueue(queue);
        waitFor([&] { return feed.snapshot().preparation == Preparation::error; });
        CHECK(!feed.snapshot().next);
    }
    {
        auto provider = std::make_shared<IgnoresCancellation>();
        std::weak_ptr<Provider> workerOwner = provider;
        auto feed = std::make_unique<Feed>(provider, 4);
        feed->setQueue(queue);
        waitFor([&] { return provider->entered.load(); });
        std::jthread watchdog([provider](std::stop_token token) {
            const auto end = std::chrono::steady_clock::now() + 1s;
            while (!token.stop_requested() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(1ms);
            if (!token.stop_requested()) provider->release = true;
        });
        const auto start = std::chrono::steady_clock::now();
        feed.reset();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(!provider->returned);
        provider->release = true;
        watchdog.request_stop();
        watchdog.join();
        provider.reset();
        waitFor([&] { return workerOwner.expired(); });
        CHECK(elapsed < 500ms);
    }
    std::filesystem::remove(path);
}
