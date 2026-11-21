#include "bgmedia/video/Playback.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace bgmedia::video;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " << #x << " line " << __LINE__ << '\n'; std::exit(1); } } while (false)

namespace {

struct Output final : PlaybackOutput {
    std::size_t queued = 0;
    std::vector<double> pictures;
    std::vector<float> samples;
    bool paused = false;
    bool flushed = false;

    std::size_t queuedAudioFrames() const override { return queued; }
    void queueAudio(std::span<const float> pcm) override {
        queued += pcm.size() / AudioFrame::channels;
        samples.insert(samples.end(), pcm.begin(), pcm.end());
    }
    void pauseAudio(bool value) override { paused = value; }
    void clearAudio() override { queued = 0; samples.clear(); flushed = false; }
    void flushAudio() override { flushed = true; }
    void present(const VideoFrame& frame) override { pictures.push_back(frame.pts); }
    void clearVideo() override { pictures.clear(); }
};

struct Clip final : Decoder {
    explicit Clip(bool withSound) : sound(withSound) {}
    VideoInfo info() const override { return {1, 1, 10, 0.3, sound ? 48000 : 0, sound ? 2 : 0}; }
    Decoded step() override {
        switch (cursor++) {
        case 0: return VideoFrame{0.0, 1, 1, {0, 0, 0, 255}};
        case 1: if (sound) return AudioFrame{0.025, std::vector<float>(4800 * 2, 0.5f)}; return {};
        case 2: return VideoFrame{0.10, 1, 1, {0, 0, 0, 255}};
        case 3: if (sound) return AudioFrame{0.125, std::vector<float>(7200 * 2, 0.5f)}; return {};
        case 4: return VideoFrame{0.20, 1, 1, {0, 0, 0, 255}};
        default: return EndOfStream{};
        }
    }
    void seek(double) override { cursor = 0; } // Starts at the previous keyframe.
    std::size_t bufferedBytes() const noexcept override { return 0; }
    bool sound;
    unsigned cursor = 0;
};

struct Preload final : Provider {
    Prepared prepare(const Media& media, std::size_t, std::stop_token) override {
        return {media.id, {}, std::make_unique<Clip>(false)};
    }
};

void near(double actual, double expected) { CHECK(std::abs(actual - expected) < 0.0001); }

}

int main() {
    Output output;
    unsigned coldOpens = 0;
    const auto origin = Playback::Clock::time_point{};
    {
        Playback player(std::make_shared<Preload>(), [&](const Media& media) -> std::unique_ptr<Decoder> {
            ++coldOpens;
            if (media.id.value == "broken") throw std::runtime_error("broken source");
            return std::make_unique<Clip>(true);
        }, output);
        player.setQueue({{{"", "", "first"}, {}}, {{"", "", "second"}, {}}});
        CHECK(coldOpens == 1 && player.snapshot().state == PlaybackState::buffering && output.paused);
        player.update(origin);
        CHECK(output.pictures == std::vector<double>{0});
        player.update(origin + 1ms);
        CHECK(!output.paused && output.queued > 4800);
        CHECK(output.samples.at(0) == 0.0f && output.samples.at(1200 * 2) == 0.5f);

        // Hold the wall clock far ahead. The audio backlog is the clock master.
        output.queued -= 4800;
        player.update(origin + 800ms);
        near(player.snapshot().position, 0.10);
        CHECK(output.pictures.back() == 0.10);

        player.pause();
        player.update(origin + 4s);
        near(player.snapshot().position, 0.10);
        CHECK(output.paused && player.snapshot().state == PlaybackState::paused);
        player.seek(0.15);
        near(player.snapshot().position, 0.15);
        CHECK(output.queued == 0 && output.pictures.empty());
        player.update(origin + 5s);
        CHECK(output.paused && output.pictures == std::vector<double>{0.20});
        CHECK(output.samples.at(0) == 0.5f); // Samples before the target were trimmed.
        player.play();
        CHECK(!output.paused);
        output.queued = 0;
        player.update(origin + 5100ms);
        CHECK(player.snapshot().state == PlaybackState::ended && output.flushed);

        const auto deadline = Playback::Clock::now() + 2s;
        while (player.snapshot().feed.preparation == Preparation::loading && Playback::Clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        CHECK(player.next());
        CHECK(player.snapshot().preparedDecoder && coldOpens == 1);
        player.update(origin + 6s);
        player.update(origin + 6100ms);
        near(player.snapshot().position, 0.10);
        CHECK(output.pictures.back() == 0.10);
        player.stop();
        near(player.snapshot().position, 0.0);
        CHECK(player.snapshot().state == PlaybackState::stopped && output.paused);
        player.play();
        CHECK(player.snapshot().state == PlaybackState::buffering);
        CHECK(!player.next() && !player.select(99));
        player.setQueue({{{"", "", "broken"}, {}}, {{"", "", "recovery"}, {}}});
        CHECK(player.snapshot().state == PlaybackState::error && output.queued == 0);
        CHECK(player.next() && player.snapshot().state == PlaybackState::buffering);
        player.seek(std::numeric_limits<double>::quiet_NaN());
        CHECK(player.snapshot().position == 0.0);
        player.close();
        CHECK(player.snapshot().state == PlaybackState::idle && !player.snapshot().feed.current);
    }
    CHECK(output.paused && output.queued == 0 && output.pictures.empty());
}
