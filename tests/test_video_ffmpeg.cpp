#include "bgmedia/video/FFmpeg.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace bgmedia::video;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " << #x << " line " << __LINE__ << '\n'; std::exit(1); } } while (false)

Snapshot waitForPreparation(Feed& feed) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = feed.snapshot();
        if (snapshot.preparation != Preparation::loading) return snapshot;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("preparation timed out");
}

void decodeAll(Decoder& decoder, bool checkCounts) {
    int frames = 0;
    double lastPts = -1, lastAudioPts = -1;
    std::size_t samples = 0;
    double energy = 0;
    for (int steps = 0; steps < 1000; ++steps) {
        auto decoded = decoder.step();
        if (auto* frame = std::get_if<VideoFrame>(&decoded)) {
            CHECK(frame->pts > lastPts);
            CHECK(frame->width == 320 && frame->height == 180);
            CHECK(frame->rgba.size() == 320 * 180 * 4);
            CHECK(frame->rgba[3] == 255);
            if (checkCounts) CHECK(std::abs(frame->pts - frames / 25.0) < 0.0001);
            lastPts = frame->pts;
            ++frames;
        } else if (auto* audio = std::get_if<AudioFrame>(&decoded)) {
            CHECK(audio->pts >= lastAudioPts);
            lastAudioPts = audio->pts;
            for (const auto sample : audio->samples) energy += sample * sample;
            samples += audio->samples.size() / AudioFrame::channels;
        } else if (std::holds_alternative<EndOfStream>(decoded)) {
            CHECK(frames > 0);
            CHECK(std::abs(lastPts - 1.96) < 0.001);
            CHECK(energy > 1);
            if (checkCounts) {
                CHECK(frames == 50);
                CHECK(samples >= 96000 && samples < 98000);
            }
            return;
        }
    }
    CHECK(false);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::filesystem::path dir(argv[1]);
    std::vector<Media> queue;
    for (const char* name : {"1", "2", "3", "broken", "empty"})
        queue.push_back({{"local", "", name}, dir / (std::string(name) + ".mp4")});
    {
        auto decoder = openFFmpeg(queue[0]);
        const auto info = decoder->info();
        CHECK(info.width == 320 && info.height == 180 && info.fps == 25);
        CHECK(std::abs(info.duration - 2.0) < 0.001);
        CHECK(info.sourceSampleRate == 44100 && info.sourceChannels == 1);
        decodeAll(*decoder, true);
        decoder->seek(1.25);
        double first = -1;
        for (int steps = 0; steps < 100 && first < 0; ++steps) {
            auto decoded = decoder->step();
            if (auto* frame = std::get_if<VideoFrame>(&decoded)) first = frame->pts;
        }
        CHECK(std::abs(first - 1.0) < 0.001); // previous keyframe, then the player trims to target
        decodeAll(*decoder, false);
    }
    constexpr auto budget = 512 * 1024;
    Feed feed(std::make_shared<FFmpegProvider>(), budget);
    std::vector<Snapshot> history;
    for (int cycle = 0; cycle < 10; ++cycle) {
        feed.setQueue(queue);
        for (int j = 0; j < 20; ++j) CHECK(feed.select(static_cast<std::size_t>(j % 2)));
        auto snap = waitForPreparation(feed);
        CHECK(snap.preparation == Preparation::ready);
        CHECK(snap.next->id.value == "3" && snap.next->bytes > 0 && snap.next->bytes <= budget);
        history.push_back(snap);
        auto selection = feed.activate(2);
        CHECK(selection && selection->decoder);
        decodeAll(*selection->decoder, true);
        CHECK(waitForPreparation(feed).preparation == Preparation::error);
        for (const auto index : {3, 4}) {
            bool failed = false;
            try { auto decoder = openFFmpeg(queue[index]); }
            catch (const std::runtime_error&) { failed = true; }
            CHECK(failed);
        }
        CHECK(feed.activate(1));
        CHECK(waitForPreparation(feed).preparation == Preparation::ready);
        feed.close();
        CHECK(!feed.snapshot().current && !feed.snapshot().next);
    }
    feed.setBudget(1024);
    feed.setQueue(queue);
    CHECK(waitForPreparation(feed).preparation == Preparation::error);
    CHECK(history.front().next->id.value == "3");
}
