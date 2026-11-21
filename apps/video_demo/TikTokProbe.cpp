// Explicit opt-in live probe. Network/decode work is performed by this CLI;
// it is not used by the SDL event thread.
#include "bgmedia/video/FFmpeg.hpp"
#include "bg_tok_explore.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace bgmedia::video;
using namespace std::chrono_literals;

int main() {
    try {
        using Host = std::unique_ptr<bg_tok_explore, decltype(&bg_tok_explore_destroy)>;
        Host host(bg_tok_explore_create(std::getenv("BG_TOK_LIVE_PROXY"), 120), bg_tok_explore_destroy);
        if (!host) throw std::runtime_error("could not create Explore host");
        const auto deadline = std::chrono::steady_clock::now() + 35s;
        int state = 0;
        while ((state = bg_tok_explore_status(host.get())) == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
        if (state != 1) throw std::runtime_error("Explore unavailable, cancelled, or timed out");
        if (bg_tok_explore_count(host.get()) != 3) throw std::runtime_error("fewer than three validated sources");
        for (std::size_t index = 0; index < 3; ++index) {
            bg_tok_explore_range range{};
            if (bg_tok_explore_open(host.get(), index, &range) != 0) throw std::runtime_error("could not open source");
            std::shared_ptr<ByteSource> source;
            try { source = adoptRangeSource({range.context, range.length, range.read_at, range.release}); }
            catch (...) { range.release(range.context); throw; }
            Media media{{"tiktok", "public", std::to_string(index)}, {}, std::move(source)};
            auto decoder = openFFmpeg(media);
            std::size_t video = 0, audio = 0;
            const auto started = std::chrono::steady_clock::now();
            for (std::size_t steps = 0; steps < 20000 && (!video || !audio); ++steps) {
                if (std::chrono::steady_clock::now() - started > 30s) throw std::runtime_error("decode timed out");
                auto decoded = decoder->step();
                if (std::holds_alternative<VideoFrame>(decoded)) ++video;
                else if (auto* frame = std::get_if<AudioFrame>(&decoded)) audio += frame->samples.size();
                else if (std::holds_alternative<EndOfStream>(decoded)) break;
            }
            if (!video || !audio) throw std::runtime_error("source did not produce video and audio");
            decoder->seek(0.5);
            bool afterSeek = false;
            for (int steps = 0; steps < 2000; ++steps) {
                auto decoded = decoder->step();
                if (std::holds_alternative<VideoFrame>(decoded)) { afterSeek = true; break; }
                if (std::holds_alternative<EndOfStream>(decoded)) break;
            }
            if (!afterSeek) throw std::runtime_error("no frame after seek");
            std::cout << "clip=" << (index + 1) << " video_frames=" << video << " audio_samples=" << audio << '\n';
        }
        bg_tok_explore_cancel(host.get());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TikTok live probe: " << error.what() << '\n';
        return 1;
    }
}
