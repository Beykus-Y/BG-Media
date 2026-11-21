#include "AsyncPlayback.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace bgmedia::video;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Sink : PlaybackOutput {
    std::thread::id main = std::this_thread::get_id();
    std::atomic<int> frames{0};
    std::size_t queuedAudioFrames() const override { return 0; }
    void queueAudio(std::span<const float>) override {}
    void pauseAudio(bool) override {}
    void clearAudio() override {}
    void flushAudio() override {}
    void present(const VideoFrame&) override { require(std::this_thread::get_id() == main, "frame was delivered off UI thread"); ++frames; }
    void clearVideo() override { require(std::this_thread::get_id() == main, "video cleared off UI thread"); }
};
struct Blocked : ByteSource {
    std::atomic<bool> started{false}, cancelled{false};
    std::uint64_t size() const noexcept override { return 8; }
    void readAt(std::uint64_t, std::span<std::uint8_t>, const ReadRequest& request) override {
        started = true;
        while (!request.stop.stop_requested() && std::chrono::steady_clock::now() < request.deadline) std::this_thread::sleep_for(1ms);
        cancelled = request.stop.stop_requested();
        checkRead(request);
    }
};
struct Simple : Decoder {
    VideoInfo info() const override { return {.width = 1, .height = 1, .fps = 25, .duration = 1}; }
    Decoded step() override { if (done) return EndOfStream{}; done = true; return VideoFrame{0, 1, 1, {1, 2, 3, 255}}; }
    void seek(double) override { done = false; }
    std::size_t bufferedBytes() const noexcept override { return 0; }
    bool done = false;
};
struct ProviderImpl : Provider {
    Prepared prepare(const Media& m, std::size_t, std::stop_token) override { return {m.id, {}, {}}; }
};
int main() {
    try {
        Sink sink;
        auto blocked = std::make_shared<Blocked>();
        const auto mainThread = std::this_thread::get_id();
        auto open = [mainThread](const Media& m) -> std::unique_ptr<Decoder> {
            require(std::this_thread::get_id() != mainThread, "decoder opened on UI thread");
            if (m.id.value == "slow") { std::uint8_t byte{}; m.bytes->readAt(0, {&byte, 1}, {{}, std::chrono::steady_clock::now() + 5s}); }
            return std::make_unique<Simple>();
        };
        AsyncPlayback player(std::make_shared<ProviderImpl>(), open, sink, PlaybackOptions{.preparationBytes = 0});
        player.setQueue({{{"fixture", "", "slow"}, {}, blocked}, {{"fixture", "", "fast"}, {}, {}}});
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!blocked->started && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        require(blocked->started, "read never started");
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) player.select(1);
        require(std::chrono::steady_clock::now() - start < 500ms, "navigation blocked UI thread");
        deadline = std::chrono::steady_clock::now() + 2s;
        while (!sink.frames && std::chrono::steady_clock::now() < deadline) { player.pump(); std::this_thread::sleep_for(1ms); }
        require(blocked->cancelled, "stale read was not cancelled");
        require(sink.frames > 0, "replacement item did not render");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
