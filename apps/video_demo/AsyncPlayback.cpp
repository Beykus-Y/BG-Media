#include "AsyncPlayback.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using namespace bgmedia::video;
namespace {
// Stops the current generation and prevents a stale decoder from initiating a
// fresh read before the worker consumes the navigation command.
class Interruptible final : public ByteSource {
public:
    explicit Interruptible(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}
    std::uint64_t size() const noexcept override { return source_->size(); }
    std::shared_ptr<void> pin() override { return source_->pin(); }
    void suspend() { std::lock_guard lock(mutex_); suspended_ = true; stop_.request_stop(); }
    void resume() { std::lock_guard lock(mutex_); stop_ = std::stop_source{}; suspended_ = false; }
    void readAt(std::uint64_t offset, std::span<std::uint8_t> out, const ReadRequest& request) override {
        std::stop_token local;
        { std::lock_guard lock(mutex_); if (suspended_) throw ReadError(BG_VIDEO_RANGE_CANCELLED); local = stop_.get_token(); }
        std::stop_source combined;
        std::stop_callback fromCaller(request.stop, [&] { combined.request_stop(); });
        std::stop_callback fromNavigation(local, [&] { combined.request_stop(); });
        source_->readAt(offset, out, {combined.get_token(), request.deadline});
    }
private:
    std::shared_ptr<ByteSource> source_;
    std::mutex mutex_;
    std::stop_source stop_;
    bool suspended_ = false;
};
}

struct AsyncPlayback::Impl {
    struct Sink final : PlaybackOutput {
        explicit Sink(PlaybackOutput& output) : target(output) {}
        std::size_t queuedAudioFrames() const override { return target.queuedAudioFrames(); }
        void queueAudio(std::span<const float> samples) override { target.queueAudio(samples); }
        void pauseAudio(bool paused) override { target.pauseAudio(paused); }
        void clearAudio() override { target.clearAudio(); }
        void flushAudio() override { target.flushAudio(); }
        void present(const VideoFrame& frame) override { std::lock_guard lock(mutex); image = frame; }
        void clearVideo() override { std::lock_guard lock(mutex); clear = true; image.reset(); }
        void pump() {
            bool blank; std::optional<VideoFrame> frame;
            { std::lock_guard lock(mutex); blank = std::exchange(clear, false); frame.swap(image); }
            if (blank) target.clearVideo();
            if (frame) target.present(*frame);
        }
        PlaybackOutput& target;
        std::mutex mutex;
        bool clear = false;
        std::optional<VideoFrame> image;
    };
    struct Command { std::function<void(Playback&)> run; bool navigation = false; };
    Impl(std::shared_ptr<Provider> provider, Playback::OpenDecoder opener, PlaybackOutput& output, PlaybackOptions options)
        : sink(output), worker([this, provider = std::move(provider), opener = std::move(opener), options] {
            try {
                Playback player(provider, opener, sink, options);
                for (;;) {
                    std::optional<Command> command;
                    { std::unique_lock lock(mutex); cv.wait_for(lock, std::chrono::milliseconds(4), [&] { return quitting || !commands.empty(); });
                      if (quitting) break;
                      if (!commands.empty()) { command = std::move(commands.front()); commands.pop_front(); } }
                    if (command) command->run(player);
                    player.update();
                    { std::lock_guard lock(mutex); snapshot = player.snapshot(); }
                }
                player.close();
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex); snapshot.state = PlaybackState::error; snapshot.error = error.what();
            } catch (...) {
                std::lock_guard lock(mutex); snapshot.state = PlaybackState::error; snapshot.error = "playback worker failed";
            }
        }) {}
    ~Impl() {
        { std::lock_guard lock(mutex); quitting = true; for (const auto& gate : gates) gate->suspend(); cv.notify_one(); }
        worker.join();
    }
    void submit(std::function<void(Playback&)> command, bool interrupt = false) {
        std::lock_guard lock(mutex);
        if (interrupt) {
            for (const auto& gate : gates) gate->suspend();
            std::erase_if(commands, [](const Command& queued) { return queued.navigation; });
        }
        commands.push_back({std::move(command), interrupt}); cv.notify_one();
    }
    void resume() { std::lock_guard lock(mutex); for (const auto& gate : gates) gate->resume(); }
    Sink sink;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Command> commands;
    std::vector<std::shared_ptr<Interruptible>> gates;
    PlaybackSnapshot snapshot;
    bool quitting = false;
    std::thread worker;
};

AsyncPlayback::AsyncPlayback(std::shared_ptr<Provider> p, Playback::OpenDecoder o, PlaybackOutput& sink, PlaybackOptions opts)
    : impl_(std::make_unique<Impl>(std::move(p), std::move(o), sink, opts)) {}
AsyncPlayback::~AsyncPlayback() = default;
void AsyncPlayback::setQueue(std::vector<Media> queue) {
    std::vector<std::shared_ptr<Interruptible>> gates;
    for (auto& media : queue) if (media.bytes) { auto gate = std::make_shared<Interruptible>(media.bytes); media.bytes = gate; gates.push_back(std::move(gate)); }
    { std::lock_guard lock(impl_->mutex); for (const auto& gate : impl_->gates) gate->suspend(); impl_->gates = std::move(gates);
      impl_->commands.clear();
      impl_->commands.push_back({[queue = std::move(queue)](Playback& player) mutable { player.setQueue(std::move(queue)); }, false}); impl_->cv.notify_one(); }
}
void AsyncPlayback::select(std::size_t index) { impl_->submit([this, index](Playback& p) { impl_->resume(); p.select(index); }, true); }
void AsyncPlayback::pause() { impl_->submit([](Playback& p) { p.pause(); }); }
void AsyncPlayback::play() { impl_->submit([](Playback& p) { p.play(); }); }
void AsyncPlayback::stop() { impl_->submit([this](Playback& p) { impl_->resume(); p.stop(); }, true); }
void AsyncPlayback::seek(double seconds) { impl_->submit([this, seconds](Playback& p) { impl_->resume(); p.seek(seconds); }, true); }
void AsyncPlayback::pump() { impl_->sink.pump(); }
PlaybackSnapshot AsyncPlayback::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->snapshot; }
