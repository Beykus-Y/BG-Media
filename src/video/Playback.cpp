#include "bgmedia/video/Playback.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <stdexcept>
#include <utility>

namespace bgmedia::video {
namespace {
constexpr auto sampleRate = AudioFrame::sampleRate;
constexpr auto channels = AudioFrame::channels;
constexpr auto presentationLead = 0.008;
}

struct Playback::Impl {
    Impl(std::shared_ptr<Provider> provider, OpenDecoder opener, PlaybackOutput& sink,
         PlaybackOptions limits)
        : options(limits), feed(std::move(provider), options.preparationBytes),
          open(std::move(opener)), output(sink) {}

    bool select(std::size_t);
    void seek(double);
    void reset(double);
    void fail(const std::exception&);
    void update(Clock::time_point);
    void updateClock(Clock::time_point);
    void submitAudio();
    void present(const VideoFrame&);
    bool finished() const { return ended && frames.empty() && output.queuedAudioFrames() == 0; }
    PlaybackSnapshot snapshot() const;

    PlaybackOptions options;
    Feed feed;
    OpenDecoder open;
    PlaybackOutput& output;
    std::unique_ptr<Decoder> decoder;
    std::deque<VideoFrame> frames;
    std::optional<AudioFrame> pendingAudio;
    std::optional<std::size_t> index;
    std::size_t videoBytes = 0, presented = 0, audioFrames = 0, queueSize = 0, fpsFrames = 0;
    VideoInfo info;
    bool paused = false, stopped = false, ended = false, started = false, warm = false;
    double position = 0, target = 0, audioEnd = 0, fps = 0, maxError = 0;
    std::optional<Clock::time_point> lastTick, fpsStart;
    std::string error;
};

void Playback::Impl::reset(double seconds) {
    frames.clear();
    pendingAudio.reset();
    videoBytes = presented = audioFrames = fpsFrames = 0;
    fps = maxError = 0;
    target = position = audioEnd = seconds;
    ended = started = false;
    lastTick.reset();
    fpsStart.reset();
    output.pauseAudio(true);
    output.clearAudio();
    output.clearVideo();
}

bool Playback::Impl::select(std::size_t selectionIndex) {
    auto selection = feed.activate(selectionIndex);
    if (!selection) return false;
    index = selectionIndex;
    warm = static_cast<bool>(selection->decoder);
    error.clear();
    paused = stopped = false;
    info = {};
    try {
        decoder.reset();
        reset(0);
        decoder = warm ? std::move(selection->decoder) : open(selection->media);
        if (!decoder) throw std::runtime_error("no decoder for media");
        info = decoder->info();
        if (!std::isfinite(info.duration) || info.duration < 0) info.duration = 0;
    } catch (const std::exception& failure) { fail(failure); }
    return true;
}

void Playback::Impl::fail(const std::exception& failure) {
    error = failure.what();
    decoder.reset();
    info = {};
    try { reset(0); }
    catch (...) {} // Keep the original output or decoder error.
}

void Playback::Impl::seek(double seconds) {
    if (!decoder || !std::isfinite(seconds)) return;
    const double end = info.duration > 0 ? info.duration : std::max(0.0, seconds);
    const double offset = std::clamp(seconds, 0.0, end);
    try {
        output.pauseAudio(true);
        decoder->seek(offset);
        reset(offset);
        stopped = false;
    } catch (const std::exception& failure) { fail(failure); }
}

void Playback::Impl::submitAudio() {
    const auto& frame = *pendingAudio;
    const auto count = frame.samples.size() / channels;
    const double gap = frame.pts - audioEnd;
    if (gap > 0.003) {
        static constexpr std::array<float, 1024 * channels> silence{};
        const auto fill = static_cast<std::size_t>(std::min(gap * sampleRate, 1024.0));
        output.queueAudio(std::span(silence).first(fill * channels));
        audioEnd += static_cast<double>(fill) / sampleRate;
        return;
    }
    const auto skip = static_cast<std::size_t>(std::clamp(
        std::round(-gap * sampleRate), 0.0, static_cast<double>(count)));
    if (skip < count) {
        output.queueAudio(std::span(frame.samples).subspan(skip * channels));
        audioFrames += count - skip;
        audioEnd += static_cast<double>(count - skip) / sampleRate;
    }
    pendingAudio.reset();
}

void Playback::Impl::updateClock(Clock::time_point now) {
    if (started && !paused) {
        const auto queued = output.queuedAudioFrames();
        if (queued)
            position = std::max(position, audioEnd - static_cast<double>(queued) / sampleRate);
        else if (lastTick && now > *lastTick)
            position += std::chrono::duration<double>(now - *lastTick).count();
    }
    if (!lastTick || now >= *lastTick) lastTick = now;
}

void Playback::Impl::present(const VideoFrame& frame) {
    output.present(frame);
    maxError = std::max(maxError, std::abs(position - frame.pts));
    ++presented;
    ++fpsFrames;
}

void Playback::Impl::update(Clock::time_point now) {
    try {
        if (!decoder || stopped || (paused && started && (presented > 0 || ended)) || finished()) return;
        updateClock(now);
        for (int step = 0; step < 48; ++step) {
            if (pendingAudio) {
                if (output.queuedAudioFrames() >= sampleRate / 5) break;
                submitAudio();
                continue;
            }
            if (ended || videoBytes > options.videoQueueBytes - options.maxVideoFrameBytes ||
                (!frames.empty() && frames.back().pts > position + 0.25 &&
                 (!info.sourceSampleRate || output.queuedAudioFrames() >= sampleRate / 10))) break;
            auto decoded = decoder->step();
            if (auto* frame = std::get_if<VideoFrame>(&decoded)) {
                if (!std::isfinite(frame->pts) || frame->width <= 0 || frame->height <= 0 ||
                    frame->rgba.capacity() > options.maxVideoFrameBytes ||
                    static_cast<std::uint64_t>(frame->width) * frame->height * 4 != frame->rgba.size())
                    throw std::runtime_error("invalid video frame");
                if (frame->pts + 0.001 < target) continue;
                videoBytes += frame->rgba.capacity();
                frames.push_back(std::move(*frame));
                if (presented == 0 && frames.front().pts <= position + presentationLead) break;
            } else if (auto* audio = std::get_if<AudioFrame>(&decoded)) {
                if (!std::isfinite(audio->pts) || audio->samples.size() % channels != 0 ||
                    audio->samples.capacity() > sampleRate * channels)
                    throw std::runtime_error("invalid audio frame");
                if (!audio->samples.empty()) pendingAudio = std::move(*audio);
            } else if (std::holds_alternative<EndOfStream>(decoded)) {
                if (target == 0 && presented == 0 && frames.empty())
                    throw std::runtime_error("no decodable video frames");
                output.flushAudio();
                ended = true;
            }
        }
        if (!started && (ended || output.queuedAudioFrames() >= sampleRate / 20 ||
            (!frames.empty() && (!info.sourceSampleRate ||
                                videoBytes > options.videoQueueBytes - options.maxVideoFrameBytes)))) {
            started = true;
            if (!paused) output.pauseAudio(false);
        }
        // A paused seek can show its first decoded frame even before its PTS.
        if (paused && presented == 0 && !frames.empty()) {
            present(frames.front());
            videoBytes -= frames.front().rgba.capacity();
            decoder->recycle(std::move(frames.front()));
            frames.pop_front();
        }
        // Present the latest frame due at this tick.
        while (!frames.empty() && frames.front().pts <= position + presentationLead) {
            auto frame = std::move(frames.front());
            frames.pop_front();
            videoBytes -= frame.rgba.capacity();
            if (frames.empty() || frames.front().pts > position + presentationLead) present(frame);
            decoder->recycle(std::move(frame));
        }
        if (!fpsStart) fpsStart = now;
        const auto elapsed = std::chrono::duration<double>(now - *fpsStart).count();
        if (elapsed >= 0.5) {
            fps = static_cast<double>(fpsFrames) / elapsed;
            fpsFrames = 0;
            fpsStart = now;
        }
    } catch (const std::exception& failure) { fail(failure); }
}

PlaybackSnapshot Playback::Impl::snapshot() const {
    PlaybackSnapshot result;
    result.feed = feed.snapshot();
    result.queueSize = queueSize;
    if (!error.empty()) result.state = PlaybackState::error;
    else if (decoder) {
        if (stopped) result.state = PlaybackState::stopped;
        else if (finished()) result.state = PlaybackState::ended;
        else if (paused) result.state = PlaybackState::paused;
        else result.state = started ? PlaybackState::playing : PlaybackState::buffering;
    }
    result.info = info;
    result.position = info.duration > 0 ? std::min(position, info.duration) : position;
    result.preparedDecoder = warm;
    result.presentedFrames = presented;
    result.submittedAudioFrames = audioFrames;
    result.presentationFps = fps;
    result.maxPresentationError = maxError;
    result.bufferedBytes = videoBytes +
        (decoder ? output.queuedAudioFrames() * channels * sizeof(float) + decoder->bufferedBytes() : 0) +
        (pendingAudio ? pendingAudio->samples.capacity() * sizeof(float) : 0) +
        (result.feed.next ? result.feed.next->bytes : 0);
    result.error = error;
    return result;
}

Playback::Playback(std::shared_ptr<Provider> provider, OpenDecoder open, PlaybackOutput& output,
                   PlaybackOptions options) {
    if (!open || options.maxVideoFrameBytes == 0 || options.videoQueueBytes < options.maxVideoFrameBytes)
        throw std::invalid_argument("invalid playback factory or frame limits");
    impl_ = std::make_unique<Impl>(std::move(provider), std::move(open), output, options);
}

Playback::~Playback() {
    try { close(); }
    catch (...) {} // Destruction must still release the decoder and Feed.
}

void Playback::setQueue(std::vector<Media> queue) {
    const auto size = queue.size();
    if (size == 0) {
        close();
        return;
    }
    impl_->feed.setQueue(std::move(queue));
    impl_->queueSize = size;
    impl_->select(0);
}

bool Playback::select(std::size_t index) { return impl_->select(index); }

bool Playback::next() {
    return impl_->index && *impl_->index + 1 < impl_->queueSize && select(*impl_->index + 1);
}

bool Playback::previous() {
    return impl_->index && *impl_->index > 0 && select(*impl_->index - 1);
}

void Playback::pause() {
    if (!impl_->decoder || impl_->stopped || impl_->paused) return;
    try {
        impl_->output.pauseAudio(true);
        impl_->paused = true;
        impl_->lastTick.reset();
    } catch (const std::exception& failure) { impl_->fail(failure); }
}

void Playback::play() {
    if (!impl_->decoder) return;
    try {
        const bool restart = impl_->stopped || impl_->finished();
        if (!impl_->paused && !restart) return;
        impl_->paused = false;
        impl_->lastTick.reset();
        if (restart) impl_->seek(0);
        else if (impl_->started) impl_->output.pauseAudio(false);
    } catch (const std::exception& failure) { impl_->fail(failure); }
}

void Playback::stop() {
    impl_->seek(0);
    if (impl_->decoder) impl_->stopped = impl_->paused = true;
}

void Playback::seek(double seconds) { impl_->seek(seconds); }
void Playback::update(Clock::time_point now) { impl_->update(now); }
PlaybackSnapshot Playback::snapshot() const { return impl_->snapshot(); }

void Playback::close() {
    impl_->feed.close();
    impl_->decoder.reset();
    impl_->queueSize = 0;
    impl_->index.reset();
    impl_->paused = impl_->stopped = impl_->warm = false;
    impl_->info = {};
    impl_->error.clear();
    try { impl_->reset(0); }
    catch (const std::exception& failure) { impl_->fail(failure); }
}

}
