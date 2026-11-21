#pragma once

#include "bgmedia/video/Feed.hpp"

namespace bgmedia::video {

// Only immutable, explicitly host-managed representations may be persisted.
enum class DiskPolicy { noStore, hostManaged };
struct CacheKey {
    MediaId media;
    std::string revision;
};

struct DiskCacheLimits {
    std::uint64_t totalBytes = 512 * 1024 * 1024;
    std::uint64_t providerBytes = 256 * 1024 * 1024;
    std::uint64_t accountBytes = 128 * 1024 * 1024;
};

// Optional SQLite adapter. One owner per database. Use a private, app-owned
// directory. Quotas count payload bytes, excluding SQLite pages and journal.
class DiskCache {
public:
    DiskCache(const std::filesystem::path& directory, DiskCacheLimits = {});
    ~DiskCache();
    DiskCache(const DiskCache&) = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    std::shared_ptr<ByteSource> wrap(CacheKey, DiskPolicy, std::shared_ptr<ByteSource>);
    // Invalidate open wrappers in the affected scope and erase stored blocks.
    void clearAccount(const std::string& provider, const std::string& account);
    void clear();
    std::uint64_t storedBytes() const;
    // Bytes served from existing cached blocks since this cache was opened.
    std::uint64_t reusedBytes() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}
