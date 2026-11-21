#pragma once

#include "bgmedia/video/Playback.hpp"
#include <memory>
#include <vector>

// All decoder operations run on a worker. pump() must be called on the thread
// that owns the video output; audio sink methods must support worker calls.
class AsyncPlayback {
public:
    AsyncPlayback(std::shared_ptr<bgmedia::video::Provider>, bgmedia::video::Playback::OpenDecoder,
                  bgmedia::video::PlaybackOutput&, bgmedia::video::PlaybackOptions = {});
    ~AsyncPlayback();
    AsyncPlayback(const AsyncPlayback&) = delete;
    AsyncPlayback& operator=(const AsyncPlayback&) = delete;
    void setQueue(std::vector<bgmedia::video::Media>);
    void select(std::size_t);
    void pause();
    void play();
    void stop();
    void seek(double);
    void pump();
    bgmedia::video::PlaybackSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
