#include "bgmedia/video/DiskCache.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

using namespace bgmedia::video;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " << #x << " line " << __LINE__ << '\n'; std::exit(1); } } while (false)

constexpr auto block = 64 * 1024ULL;

struct Origin : ByteSource {
    explicit Origin(std::uint8_t value) : fill(value) {}
    std::uint64_t size() const noexcept override { return block * 2 + 1; }
    void readAt(std::uint64_t, std::span<std::uint8_t> output, const ReadRequest& request) override {
        checkRead(request);
        ++reads;
        if (offline) throw ReadError(BG_VIDEO_RANGE_IO);
        std::fill(output.begin(), output.end(), fill);
    }
    unsigned reads = 0;
    bool offline = false;
    std::uint8_t fill;
};

void expect(ByteSource& source, std::uint64_t offset, std::uint8_t wanted) {
    std::array<std::uint8_t, 2> output{};
    source.readAt(offset, output, {{}, std::chrono::steady_clock::now() + 1s});
    CHECK((output == std::array<std::uint8_t, 2>{wanted, wanted}));
}

CacheKey key(std::string account, std::string item, std::string revision = "r1") {
    return {{"provider", std::move(account), std::move(item)}, std::move(revision)};
}

int main() {
    const auto directory = std::filesystem::current_path() /
        ("video-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Remove { std::filesystem::path dir; ~Remove() { std::error_code ec; std::filesystem::remove_all(dir, ec); } } remove{directory};
    auto input = std::make_shared<Origin>(42);
    auto alternate = std::make_shared<Origin>(27);
    const DiskCacheLimits limits{3 * block, 3 * block, 2 * block};
    {
        DiskCache cache(directory, limits);
        auto a = cache.wrap(key("alice", "a"), DiskPolicy::hostManaged, input);
        auto b = cache.wrap(key("alice", "b"), DiskPolicy::hostManaged, input);
        auto c = cache.wrap(key("alice", "c"), DiskPolicy::hostManaged, input);
        expect(*a, 1, 42);
        expect(*b, 7, 42);
        expect(*a, 4, 42);
        CHECK(input->reads == 2);
        expect(*c, 2, 42); // b was least recently used.
        expect(*a, 3, 42);
        CHECK(input->reads == 3 && cache.storedBytes() == 2 * block);
        auto pinnedA = a->pin();
        auto pinnedC = c->pin();
        expect(*b, 9, 42);
        expect(*b, 9, 42); // Both resident blocks are pinned, b is not written.
        CHECK(input->reads == 5);
        pinnedC.reset();
        expect(*b, 9, 42);
        expect(*b, 9, 42);
        CHECK(input->reads == 6);
        auto bob = cache.wrap(key("bob", "a"), DiskPolicy::hostManaged, alternate);
        expect(*bob, 1, 27);
        CHECK(cache.storedBytes() == 3 * block);
        auto noStore = cache.wrap(key("bob", "secret"), DiskPolicy::noStore, alternate);
        CHECK(noStore == alternate);
        expect(*noStore, 2, 27);
        CHECK(cache.storedBytes() == 3 * block);
    }
    {
        DiskCache cache(directory, limits);
        auto a = cache.wrap(key("alice", "a"), DiskPolicy::hostManaged, input);
        input->offline = true;
        expect(*a, 0, 42);
        CHECK(input->reads == 6);
        cache.clearAccount("alice", "alice-other"); // no matching account or provider
        expect(*a, 0, 42);
        cache.clearAccount("provider", "alice");
        CHECK(cache.storedBytes() == block);
        try { expect(*a, 0, 42); CHECK(false); }
        catch (const ReadError& error) { CHECK(error.status() == BG_VIDEO_RANGE_CANCELLED); }
        auto bob = cache.wrap(key("bob", "a", "r2"), DiskPolicy::hostManaged, input);
        try { expect(*bob, 0, 42); CHECK(false); }
        catch (const ReadError& error) { CHECK(error.status() == BG_VIDEO_RANGE_IO); }
        cache.clear();
        CHECK(cache.storedBytes() == 0);
    }
    {
        input->offline = false;
        const auto initialReads = input->reads;
        DiskCache cache(directory, {3 * block, 2 * block, 2 * block});
        auto a = cache.wrap(key("alice", "a"), DiskPolicy::hostManaged, input);
        auto b = cache.wrap(key("bob", "b"), DiskPolicy::hostManaged, input);
        auto c = cache.wrap(key("charlie", "c"), DiskPolicy::hostManaged, input);
        expect(*a, block - 1, 42); // Two blocks, with one read spanning the boundary.
        expect(*b, 0, 42); // Provider quota crosses account boundaries.
        CHECK(cache.storedBytes() == 2 * block);
        expect(*c, 0, 42);
        auto otherKey = key("alice", "a");
        otherKey.media.provider = "another-provider";
        auto other = cache.wrap(otherKey, DiskPolicy::hostManaged, alternate);
        expect(*other, 0, 27);
        CHECK(cache.storedBytes() == 3 * block);
        expect(*other, block, 27); // Global quota evicts the least recently used.
        CHECK(cache.storedBytes() == 3 * block);
        expect(*c, 0, 42);
        CHECK(input->reads == initialReads + 4);
        std::stop_source stop;
        stop.request_stop();
        std::array<std::uint8_t, 1> bytes{};
        try {
            c->readAt(0, bytes, {stop.get_token(), std::chrono::steady_clock::now() + 1s});
            CHECK(false);
        } catch (const ReadError& error) { CHECK(error.status() == BG_VIDEO_RANGE_CANCELLED); }
    }
}
