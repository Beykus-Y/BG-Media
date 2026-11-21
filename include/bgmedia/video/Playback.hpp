#pragma once

#include "bgmedia/video/Feed.hpp"

#include <chrono>
#include <functional>
#include <span>

namespace bgmedia::video {

// Calls are made on the playback thread. PCM is interleaved float stereo at
// AudioFrame::sampleRate. A sink must consume or copy spans and frames during
// the call. queuedAudioFrames() counts sample frames, in the submitted format.
class PlaybackOutput {
public:
    virtual ~PlaybackOutput() = default;
    virtual std::size_t queuedAudioFrames() const = 0;
    virtual void queueAudio(std::span<const float>) = 0;
    virtual void pauseAudio(bool paused) = 0;
    virtual void clearAudio() = 0;
    virtual void flushAudio() = 0;
    virtual void present(const VideoFrame&) = 0;
    virtual void clearVideo() = 0;
};

enum class PlaybackState { idle, buffering, playing, paused, stopped, ended, error };

struct PlaybackOptions {
    std::size_t preparationBytes = 40 * 1024 * 1024;
    std::size_t videoQueueBytes = 64 * 1024 * 1024;
    std::size_t maxVideoFrameBytes = 3840 * 2160 * 4;
};

struct PlaybackSnapshot {
    Snapshot feed;
    std::size_t queueSize = 0;
    PlaybackState state = PlaybackState::idle;
    VideoInfo info;
    double position = 0;
    bool preparedDecoder = false;
    std::size_t presentedFrames = 0;
    std::size_t submittedAudioFrames = 0;
    double presentationFps = 0;
    double maxPresentationError = 0;
    std::size_t bufferedBytes = 0; // Excludes textures and codec working memory.
    std::string error;
};

// Owns the Feed, current decoder and presentation scheduler. Call from one
// thread. Output must outlive Playback. The factory runs on the caller thread;
// the provider prepares the next item on Feed's worker.
class Playback {
public:
    using Clock = std::chrono::steady_clock;
    using OpenDecoder = std::function<std::unique_ptr<Decoder>(const Media&)>;

    Playback(std::shared_ptr<Provider>, OpenDecoder, PlaybackOutput&, PlaybackOptions = {});
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    void setQueue(std::vector<Media>);
    bool select(std::size_t);
    bool next();
    bool previous();
    void pause();
    void play();
    void stop();
    void seek(double seconds);
    void close();
    // Use one monotonic clock domain across all updates.
    void update(Clock::time_point now = Clock::now());
    PlaybackSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
