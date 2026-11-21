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
struct Blocking final : Provider {
    std::atomic<bool> entered{false}, cancelled{false};
    Prepared prepare(const Media& m, std::size_t, std::stop_token token) override {
        if (m.id.value == "1") {
            entered = true;
            while (!token.stop_requested()) std::this_thread::sleep_for(1ms);
            cancelled = true;
        }
        return {m.id, {}};
    }
};
template<class F> void waitFor(F f) {
    auto end = std::chrono::steady_clock::now() + 3s;
    while (!f() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
    CHECK(f());
}
int main() {
    auto path = std::filesystem::temp_directory_path() / "bgmedia-video-feed-test.bin";
    { std::ofstream out(path, std::ios::binary); out << "0123456789"; }
    std::vector<Media> queue;
    for (int i = 0; i < 4; ++i) queue.push_back({{"provider", "account", std::to_string(i)}, path});
    {
        Feed feed(std::make_shared<LocalProvider>(), 4);
        feed.setQueue(queue);
        waitFor([&] { return feed.snapshot().preparation == Preparation::ready; });
        auto snapshot = feed.snapshot();
        CHECK(snapshot.next->id.value == "1"); CHECK(snapshot.next->prefix.size() == 4);
        CHECK(feed.next()); CHECK(feed.previous()); CHECK(!feed.previous()); CHECK(!feed.select(99));
        feed.setBudget(0); CHECK(feed.snapshot().preparation == Preparation::idle);
        feed.setBudget(20);
        waitFor([&] { return feed.snapshot().preparation == Preparation::ready; });
        CHECK(feed.snapshot().next->prefix.size() == 10);
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
    std::filesystem::remove(path);
}
