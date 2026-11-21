#pragma once

#include "bgmedia/video/range_source.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <stdexcept>

namespace bgmedia::video {

struct ReadRequest {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline;
};

class ReadError : public std::runtime_error {
public:
    explicit ReadError(int status);
    int status() const noexcept { return status_; }
private:
    int status_;
};

// Immutable seekable representation. readAt is exact and rejects reads beyond
// size(). Calls may be concurrent and must honor cancellation and deadlines.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Optional lease for active decoders. Dropping it allows cached eviction.
    virtual std::shared_ptr<void> pin() { return {}; }
    virtual std::uint64_t size() const noexcept = 0;
    virtual void readAt(std::uint64_t, std::span<std::uint8_t>, const ReadRequest&) = 0;
};

void checkRead(const ReadRequest&);
// Read-only view of a complete host-owned regular file. Does not cache or
// remove it. The host must keep the representation immutable while open.
std::shared_ptr<ByteSource> openFileByteSource(const std::filesystem::path&);
// Transfers ownership on success. On failure the caller still owns callbacks.context.
std::shared_ptr<ByteSource> adoptRangeSource(bg_video_range_v1 callbacks);

}
