#include "bgmedia/video/ByteSource.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>

namespace bgmedia::video {
namespace {
const char* errorText(int status) {
    switch (status) {
    case BG_VIDEO_RANGE_CANCELLED: return "media read cancelled";
    case BG_VIDEO_RANGE_TIMEOUT: return "media read timed out";
    case BG_VIDEO_RANGE_EXPIRED: return "media source expired";
    default: return "media read failed";
    }
}

class FileSource final : public ByteSource {
public:
    explicit FileSource(const std::filesystem::path& path) {
        if (!std::filesystem::is_regular_file(path)) throw ReadError(BG_VIDEO_RANGE_IO);
        file_.open(path, std::ios::binary | std::ios::ate);
        const auto length = file_.tellg();
        if (length < 0) throw ReadError(BG_VIDEO_RANGE_IO);
        length_ = static_cast<std::uint64_t>(length);
    }

    std::uint64_t size() const noexcept override { return length_; }

    void readAt(std::uint64_t offset, std::span<std::uint8_t> output, const ReadRequest& request) override {
        if (offset > size() || output.size() > size() - offset) throw ReadError(BG_VIDEO_RANGE_IO);
        std::lock_guard lock(mutex_);
        checkRead(request);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        if (!file_) throw ReadError(BG_VIDEO_RANGE_IO);
        while (!output.empty()) {
            const auto count = std::min<std::size_t>(output.size(), 64 * 1024);
            file_.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(count));
            if (!file_) throw ReadError(BG_VIDEO_RANGE_IO);
            checkRead(request);
            output = output.subspan(count);
        }
    }

private:
    std::mutex mutex_;
    std::ifstream file_;
    std::uint64_t length_ = 0;
};

class CallbackSource final : public ByteSource {
public:
    explicit CallbackSource(bg_video_range_v1 c) : callbacks_(c) {}
    ~CallbackSource() override { callbacks_.release(callbacks_.context); }
    std::uint64_t size() const noexcept override { return callbacks_.length; }

    void readAt(std::uint64_t offset, std::span<std::uint8_t> output, const ReadRequest& request) override {
        checkRead(request);
        if (offset > size() || output.size() > size() - offset) throw ReadError(BG_VIDEO_RANGE_IO);
        if (output.empty()) return;
        using namespace std::chrono;
        const auto maxTimeout = milliseconds(std::numeric_limits<std::uint32_t>::max());
        const auto remaining = request.deadline - steady_clock::now();
        const auto available = remaining >= maxTimeout ? maxTimeout.count() : ceil<milliseconds>(remaining).count();
        const auto timeout = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(available, 1, std::numeric_limits<std::uint32_t>::max()));
        auto stop = request.stop;
        const auto cancelled = [](void* opaque) -> int {
            return static_cast<std::stop_token*>(opaque)->stop_requested();
        };
        const int status = callbacks_.read_at(callbacks_.context, offset, output.data(), output.size(),
                                              timeout, cancelled, &stop);
        if (status != BG_VIDEO_RANGE_OK) throw ReadError(status);
        checkRead(request);
    }

private:
    bg_video_range_v1 callbacks_;
};
}

ReadError::ReadError(int status) : std::runtime_error(errorText(status)), status_(status) {}

void checkRead(const ReadRequest& request) {
    if (request.stop.stop_requested()) throw ReadError(BG_VIDEO_RANGE_CANCELLED);
    if (std::chrono::steady_clock::now() >= request.deadline) throw ReadError(BG_VIDEO_RANGE_TIMEOUT);
}

std::shared_ptr<ByteSource> openFileByteSource(const std::filesystem::path& path) {
    return std::make_shared<FileSource>(path);
}

std::shared_ptr<ByteSource> adoptRangeSource(bg_video_range_v1 callbacks) {
    if (!callbacks.context || !callbacks.length || !callbacks.read_at || !callbacks.release)
        throw std::invalid_argument("invalid media range source");
    return std::make_shared<CallbackSource>(callbacks);
}

}
