#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace bgmedia::video {

struct MediaId {
    std::string provider, account, value;
    bool operator==(const MediaId&) const = default;
};
struct Media {
    MediaId id;
    std::filesystem::path localPath;
};
struct Prepared {
    MediaId id;
    // Bounded prefix owned by this object; this is not a decoded frame.
    std::vector<std::byte> prefix;
};

class Provider {
public:
    virtual ~Provider() = default;
    // Called on the worker thread. Implementations must honor stop requests and
    // bound both their allocation and blocking operations. Exceptions are errors.
    virtual Prepared prepare(const Media&, std::size_t maxBytes, std::stop_token) = 0;
};

// Local files only. Intended for development and host-owned files; never deletes them.
class LocalProvider final : public Provider {
public:
    Prepared prepare(const Media&, std::size_t, std::stop_token) override;
};

enum class Preparation { idle, loading, ready, error };
struct PreparedInfo {
    MediaId id;
    std::size_t bytes = 0;
};
struct Snapshot {
    std::optional<std::size_t> current;
    Preparation preparation = Preparation::idle;
    std::string error;
    std::optional<PreparedInfo> next;
};

// Thread safe. The provider must outlive in-flight calls (shared ownership).
// Only the next item is prepared; navigation preserves the supplied order.
class Feed {
public:
    Feed(std::shared_ptr<Provider>, std::size_t prefixBudget);
    ~Feed();
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;
    void setQueue(std::vector<Media>);
    bool select(std::size_t);
    bool next();
    bool previous();
    void setBudget(std::size_t);
    void close();
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
