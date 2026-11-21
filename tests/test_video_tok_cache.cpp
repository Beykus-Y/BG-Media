#include "bgmedia/video/DiskCache.hpp"
#include "bg_tok_fixture.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace bgmedia::video;
using namespace std::chrono_literals;

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::shared_ptr<ByteSource> source(bg_tok_fixture* host) {
    bg_tok_fixture_range r{};
    check(bg_tok_fixture_open(host, 0, &r) == 0, "source open");
    try { return adoptRangeSource({r.context, r.length, r.read_at, r.release}); }
    catch (...) { r.release(r.context); throw; }
}
std::array<std::uint8_t, 16> read(ByteSource& source, std::uint64_t offset = 0) {
    std::array<std::uint8_t, 16> data{};
    source.readAt(offset, data, {{}, std::chrono::steady_clock::now() + 2s});
    return data;
}
int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected fixture and cache directory");
        std::filesystem::remove_all(argv[2]);
        const char* paths[]{argv[1]};
        std::unique_ptr<bg_tok_fixture, decltype(&bg_tok_fixture_destroy)> host(bg_tok_fixture_create(paths, 1), bg_tok_fixture_destroy);
        check(bool(host), "fixture host");
        const auto n = bg_tok_fixture_revision(host.get(), 0, nullptr, 0);
        check(n > 1 && n < 4096, "revision length");
        std::vector<char> revision(n);
        check(bg_tok_fixture_revision(host.get(), 0, revision.data(), n) == n, "revision");
        auto raw = source(host.get());
        CacheKey alice{{"fixture", "alice", "0"}, revision.data()};
        CacheKey bob{{"fixture", "bob", "0"}, revision.data()};
        std::array<std::uint8_t, 16> expected{};
        {
            DiskCache cache(argv[2]);
            auto a = cache.wrap(alice, DiskPolicy::hostManaged, raw);
            expected = read(*a);
            check(bg_tok_fixture_read_calls(host.get()) == 1, "initial provider read");
            check(read(*a) == expected && bg_tok_fixture_read_calls(host.get()) == 1, "cache reuse");
            check(cache.reusedBytes() == expected.size(), "cache reuse byte count");
            auto b = cache.wrap(bob, DiskPolicy::hostManaged, raw);
            check(read(*b) == expected && bg_tok_fixture_read_calls(host.get()) == 2, "account isolation");
            const auto before = cache.storedBytes();
            auto noStore = cache.wrap({{"fixture", "alice", "private"}, revision.data()}, DiskPolicy::noStore, raw);
            check(read(*noStore) == expected && bg_tok_fixture_read_calls(host.get()) == 3, "no-store read");
            check(cache.storedBytes() == before, "no-store wrote to disk");
        }
        {
            DiskCache cache(argv[2]);
            auto a = cache.wrap(alice, DiskPolicy::hostManaged, raw);
            auto b = cache.wrap(bob, DiskPolicy::hostManaged, raw);
            bg_tok_fixture_cancel(host.get());
            check(read(*a) == expected && read(*b) == expected, "offline cached reads");
            check(cache.reusedBytes() == 2 * expected.size(), "offline reused byte count");
            check(bg_tok_fixture_read_calls(host.get()) == 3, "offline fetched provider");
            cache.clearAccount("fixture", "alice");
            try { read(*a); throw std::runtime_error("cleared account remained readable"); }
            catch (const ReadError& error) { check(error.status() == BG_VIDEO_RANGE_CANCELLED, "invalidated account status"); }
            check(read(*b) == expected, "other account affected");
        }
        raw.reset();
        host.reset();
        std::filesystem::remove_all(argv[2]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
