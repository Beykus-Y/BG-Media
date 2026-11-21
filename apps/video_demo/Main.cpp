#include "bgmedia/video/FFmpeg.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>

#ifdef __linux__
#include <unistd.h>
#endif

namespace {
using namespace bgmedia::video;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MiB = 1024 * 1024;
constexpr std::size_t preparationBudget = 40 * MiB;
constexpr std::size_t videoQueueBudget = 64 * MiB;
constexpr std::size_t maxVideoFrame = 3840 * 2160 * 4;
constexpr int audioFrameBytes = AudioFrame::channels * sizeof(float);
constexpr int audioBytesPerSecond = AudioFrame::sampleRate * audioFrameBytes;

std::size_t residentBytes() {
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    std::size_t total = 0, resident = 0;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0 && statm >> total >> resident) return resident * static_cast<std::size_t>(pageSize);
#endif
    return 0;
}

void require(bool ok) {
    if (!ok) throw std::runtime_error(SDL_GetError());
}

struct Dialog {
    std::mutex mutex;
    bool open = false;
    std::vector<std::filesystem::path> paths;
    std::string error;
};

void SDLCALL picked(void* userdata, const char* const* files, int) {
    std::unique_ptr<std::shared_ptr<Dialog>> data(static_cast<std::shared_ptr<Dialog>*>(userdata));
    auto& dialog = **data;
    std::lock_guard lock(dialog.mutex);
    dialog.open = false;
    try {
        if (!files) dialog.error = SDL_GetError();
        else for (; *files; ++files) dialog.paths.emplace_back(std::u8string(
            reinterpret_cast<const char8_t*>(*files)));
    } catch (...) {
        dialog.paths.clear();
    }
}

class Player {
public:
    Player(SDL_Renderer* renderer, SDL_AudioStream* audio, bool preload)
        : feed_(std::make_shared<FFmpegProvider>(), preload ? preparationBudget : 0),
          renderer_(renderer), audio_(audio) {}
    void load(std::vector<std::filesystem::path> files);
    void select(std::size_t);
    void move(int direction);
    void pause();
    void stop();
    void seek(double);
    void seekFraction(double fraction) { if (info_.duration > 0) seek(fraction * info_.duration); }
    void update();
    void draw(bool force);
    std::string status() const;
    bool finished() const { return ended_ && frames_.empty() && queuedAudio() == 0; }
    bool stopped() const { return stopped_; }
    double position() const { return position_; }
    std::size_t presented() const { return presented_; }
    std::size_t peakBuffers() const { return peakBuffers_; }
    std::size_t audioFrames() const { return audioFrames_; }
    double maxVideoLag() const { return maxVideoLag_; }
    const std::string& error() const { return error_; }
    Feed& feed() { return feed_; }
private:
    int queuedAudio() const { return std::max(0, SDL_GetAudioStreamQueued(audio_)); }
    void reset(double);
    void submitAudio();
    void present(VideoFrame&);
    void fail(const std::exception&);
    void updateClock(Clock::time_point now);
    std::size_t bufferBytes(const Snapshot&) const;

    Feed feed_;
    std::unique_ptr<Decoder> decoder_;
    SDL_Renderer* renderer_;
    SDL_AudioStream* audio_;
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture_{nullptr, SDL_DestroyTexture};
    std::deque<VideoFrame> frames_;
    std::optional<AudioFrame> pendingAudio_;
    std::size_t videoBytes_ = 0, presented_ = 0, audioFrames_ = 0, queueSize_ = 0, fpsFrames_ = 0;
    std::size_t index_ = 0, peakBuffers_ = 0, rss_ = 0;
    int width_ = 0, height_ = 0;
    VideoInfo info_;
    bool paused_ = false, stopped_ = false, ended_ = false, started_ = false, warm_ = false;
    double position_ = 0, target_ = 0, audioEnd_ = 0, fps_ = 0, maxVideoLag_ = 0;
    Clock::time_point lastTick_, switchStart_, fpsStart_, lastMemorySample_{};
    bool firstFramePending_ = false, pictureDirty_ = true;
    Clock::time_point lastDraw_{};
    std::string filename_, error_;
};

void Player::reset(double seconds) {
    require(SDL_PauseAudioStreamDevice(audio_));
    require(SDL_ClearAudioStream(audio_));
    frames_.clear();
    pendingAudio_.reset();
    videoBytes_ = presented_ = audioFrames_ = fpsFrames_ = 0;
    fps_ = maxVideoLag_ = 0;
    target_ = position_ = audioEnd_ = seconds;
    pictureDirty_ = true;
    ended_ = started_ = firstFramePending_ = false;
    lastTick_ = fpsStart_ = Clock::now();
}

void Player::load(std::vector<std::filesystem::path> files) {
    std::vector<Media> queue;
    for (auto& file : files)
        queue.push_back({{"local", "", std::to_string(queue.size())}, std::move(file)});
    queueSize_ = queue.size();
    feed_.setQueue(std::move(queue));
    if (queueSize_) select(0);
}

void Player::select(std::size_t index) {
    switchStart_ = Clock::now();
    auto selection = feed_.activate(index);
    if (!selection) return;
    warm_ = static_cast<bool>(selection->decoder);
    index_ = index;
    filename_ = selection->media.localPath.filename().string();
    error_.clear();
    paused_ = stopped_ = false;
    try {
        decoder_.reset();
        reset(0);
        decoder_ = warm_ ? std::move(selection->decoder) : openFFmpeg(selection->media);
        info_ = decoder_->info();
    } catch (const std::exception& error) { fail(error); }
}

void Player::move(int direction) {
    if (direction < 0 && index_ > 0) select(index_ - 1);
    if (direction > 0 && index_ + 1 < queueSize_) select(index_ + 1);
}

void Player::fail(const std::exception& error) {
    decoder_.reset();
    texture_.reset();
    reset(0);
    error_ = error.what();
    std::fprintf(stderr, "%s: %s\n", filename_.c_str(), error_.c_str());
}

void Player::pause() {
    if (!decoder_) return;
    if (stopped_ || finished()) {
        paused_ = false;
        seek(0);
        return;
    }
    paused_ = !paused_;
    lastTick_ = Clock::now();
    if (paused_) SDL_PauseAudioStreamDevice(audio_);
    else if (started_) SDL_ResumeAudioStreamDevice(audio_);
}

void Player::stop() {
    if (!decoder_) return;
    seek(0);
    stopped_ = paused_ = true;
}

void Player::seek(double seconds) {
    if (!decoder_) return;
    const double end = info_.duration > 0 ? std::max(0.0, info_.duration - 0.05) : seconds;
    const double target = std::clamp(seconds, 0.0, std::max(0.0, end));
    try {
        decoder_->seek(target);
        reset(target);
        switchStart_ = Clock::now();
        stopped_ = false;
    } catch (const std::exception& error) { fail(error); }
}

void Player::submitAudio() {
    if (!pendingAudio_) return;
    const auto& frame = *pendingAudio_;
    const int frames = static_cast<int>(frame.samples.size() / AudioFrame::channels);
    const double gap = frame.pts - audioEnd_;
    if (gap > 0.003) {
        static constexpr std::array<float, 2048> silence{};
        const auto count = static_cast<int>(std::min(gap * AudioFrame::sampleRate, 1024.0));
        require(SDL_PutAudioStreamData(audio_, silence.data(), count * audioFrameBytes));
        audioEnd_ += static_cast<double>(count) / AudioFrame::sampleRate;
        return;
    }
    const int skip = static_cast<int>(std::clamp(
        std::round(-gap * AudioFrame::sampleRate), 0.0, static_cast<double>(frames)));
    if (skip < frames) {
        require(SDL_PutAudioStreamData(audio_, frame.samples.data() + skip * AudioFrame::channels,
                                      (frames - skip) * audioFrameBytes));
        audioFrames_ += frames - skip;
        audioEnd_ += static_cast<double>(frames - skip) / AudioFrame::sampleRate;
    }
    pendingAudio_.reset();
}

void Player::updateClock(Clock::time_point now) {
    if (started_ && !paused_ && !finished()) {
        const int queued = queuedAudio();
        if (queued)
            position_ = std::max(position_, audioEnd_ - static_cast<double>(queued) / audioBytesPerSecond);
        else
            position_ += std::chrono::duration<double>(now - lastTick_).count();
        if (info_.duration > 0) position_ = std::min(position_, info_.duration);
    }
    lastTick_ = now;
}

void Player::update() {
    if (!decoder_ || stopped_ || (paused_ && started_) || finished()) return;
    try {
        const auto now = Clock::now();
        updateClock(now);
        for (int i = 0; i < 48; ++i) {
            if (pendingAudio_) {
                if (queuedAudio() >= audioBytesPerSecond / 5) break;
                submitAudio();
                continue;
            }
            if (ended_ || videoBytes_ > videoQueueBudget - maxVideoFrame ||
                (!frames_.empty() && frames_.back().pts > position_ + 0.25 &&
                 (!info_.sourceSampleRate || queuedAudio() >= audioBytesPerSecond / 10))) break;
            auto decoded = decoder_->step();
            if (auto* video = std::get_if<VideoFrame>(&decoded)) {
                if (video->pts + 0.001 < target_) continue;
                videoBytes_ += video->rgba.capacity();
                frames_.push_back(std::move(*video));
                if (presented_ == 0 && frames_.front().pts <= position_ + 0.008) break;
            } else if (auto* audio = std::get_if<AudioFrame>(&decoded)) {
                pendingAudio_ = std::move(*audio);
            } else if (std::holds_alternative<EndOfStream>(decoded)) {
                if (target_ == 0 && presented_ == 0 && frames_.empty())
                    throw std::runtime_error("no decodable video frames");
                require(SDL_FlushAudioStream(audio_));
                ended_ = true;
            }
        }
        if (!started_ && (ended_ || queuedAudio() >= audioBytesPerSecond / 20 ||
            (!frames_.empty() && (!info_.sourceSampleRate ||
                                  videoBytes_ > videoQueueBudget - maxVideoFrame)))) {
            started_ = true;
            if (!paused_) require(SDL_ResumeAudioStreamDevice(audio_));
        }
        // Skip overdue frames while still displaying the most recent eligible one.
        if (paused_ && presented_ == 0 && !frames_.empty()) {
            present(frames_.front());
            videoBytes_ -= frames_.front().rgba.capacity();
            decoder_->recycle(std::move(frames_.front()));
            frames_.pop_front();
        }
        while (!frames_.empty() && frames_.front().pts <= position_ + 0.008) {
            auto frame = std::move(frames_.front());
            frames_.pop_front();
            videoBytes_ -= frame.rgba.capacity();
            if (frames_.empty() || frames_.front().pts > position_ + 0.008) present(frame);
            decoder_->recycle(std::move(frame));
        }
        peakBuffers_ = std::max(peakBuffers_, bufferBytes(feed_.snapshot()));
        const auto elapsed = std::chrono::duration<double>(now - fpsStart_).count();
        if (elapsed >= 0.5) {
            fps_ = static_cast<double>(fpsFrames_) / elapsed;
            fpsFrames_ = 0;
            fpsStart_ = now;
        }
    } catch (const std::exception& error) { fail(error); }
}

void Player::present(VideoFrame& frame) {
    if (!texture_ || width_ != frame.width || height_ != frame.height) {
        width_ = frame.width;
        height_ = frame.height;
        texture_.reset(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STREAMING, width_, height_));
        require(static_cast<bool>(texture_));
    }
    require(SDL_UpdateTexture(texture_.get(), nullptr, frame.rgba.data(), width_ * 4));
    maxVideoLag_ = std::max(maxVideoLag_, std::abs(position_ - frame.pts));
    if (presented_++ == 0) firstFramePending_ = true;
    pictureDirty_ = true;
    ++fpsFrames_;
}

std::size_t Player::bufferBytes(const Snapshot& snapshot) const {
    return videoBytes_ + static_cast<std::size_t>(queuedAudio()) +
        (pendingAudio_ ? pendingAudio_->samples.capacity() * sizeof(float) : 0) +
        (decoder_ ? decoder_->bufferedBytes() : 0) +
        (texture_ ? static_cast<std::size_t>(width_) * height_ * 4 : 0) +
        (snapshot.next ? snapshot.next->bytes : 0);
}

std::string Player::status() const {
    const auto snapshot = feed_.snapshot();
    const char* next = snapshot.preparation == Preparation::ready ? "ready" :
        snapshot.preparation == Preparation::loading ? "loading" :
        snapshot.preparation == Preparation::error ? "error" : "-";
    const char* state = stopped_ ? "stopped" : finished() ? "ended" :
        paused_ ? "paused" : !started_ ? "buffering" : "playing";
    char text[256]{};
    std::snprintf(text, sizeof(text), "%zu/%zu  %s  %.2f / %.2fs  %.1f / %.1f FPS  next %s  buffers %.2f MiB",
        index_ + (queueSize_ > 0), queueSize_, state, position_, info_.duration, fps_, info_.fps,
        next, static_cast<double>(bufferBytes(snapshot)) / MiB);
    if (!error_.empty()) return error_;
    if (!decoder_) return "Open MP4 files to play";
    if (!rss_) return text;
    char memory[32]{};
    std::snprintf(memory, sizeof(memory), "  RSS %.1f MiB", static_cast<double>(rss_) / MiB);
    return std::string(text) + memory;
}

void Player::draw(bool force) {
    const auto now = Clock::now();
    if (!force && !pictureDirty_ && now - lastDraw_ < std::chrono::milliseconds(100)) return;
    lastDraw_ = now;
    pictureDirty_ = false;
    if (now - lastMemorySample_ >= std::chrono::milliseconds(500)) {
        rss_ = residentBytes();
        lastMemorySample_ = now;
    }
    int w = 0, h = 0;
    require(SDL_GetRenderOutputSize(renderer_, &w, &h));
    SDL_SetRenderDrawColor(renderer_, 15, 18, 24, 255);
    require(SDL_RenderClear(renderer_));
    const float contentHeight = std::max(0.0f, h - 95.0f);
    if (texture_ && presented_ > 0 && contentHeight > 0) {
        const float scale = std::min(static_cast<float>(w) / width_, contentHeight / height_);
        const SDL_FRect dst{(w - width_ * scale) / 2, (contentHeight - height_ * scale) / 2,
                            width_ * scale, height_ * scale};
        require(SDL_RenderTexture(renderer_, texture_.get(), nullptr, &dst));
    }
    SDL_SetRenderDrawColor(renderer_, 57, 67, 84, 255);
    const SDL_FRect rail{16, contentHeight + 8, std::max(0.0f, w - 32.0f), 5};
    SDL_RenderFillRect(renderer_, &rail);
    if (info_.duration > 0) {
        SDL_SetRenderDrawColor(renderer_, 92, 205, 177, 255);
        auto progress = rail;
        progress.w *= static_cast<float>(std::clamp(position_ / info_.duration, 0.0, 1.0));
        SDL_RenderFillRect(renderer_, &progress);
    }
    SDL_SetRenderDrawColor(renderer_, 228, 236, 247, 255);
    SDL_RenderDebugText(renderer_, 16, contentHeight + 25, status().c_str());
    SDL_RenderDebugText(renderer_, 16, contentHeight + 42, "O Open files   Space Pause/Play   S Stop");
    SDL_RenderDebugText(renderer_, 16, contentHeight + 59, "N / P Next / Previous   Left / Right Seek");
    require(SDL_RenderPresent(renderer_));
    if (firstFramePending_) {
        firstFramePending_ = false;
        const auto latency = std::chrono::duration<double, std::milli>(Clock::now() - switchStart_).count();
        std::printf("%zu %s first-frame-ms=%.2f\n", index_ + 1, warm_ ? "warm" : "cold", latency);
        std::fflush(stdout);
    }
}

int run(int argc, char** argv) {
    require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    struct Quit { ~Quit() { SDL_Quit(); } } quit;
    SDL_Window* rawWindow = nullptr;
    SDL_Renderer* rawRenderer = nullptr;
    require(SDL_CreateWindowAndRenderer("BG Video Demo", 920, 600, SDL_WINDOW_RESIZABLE,
                                        &rawWindow, &rawRenderer));
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(rawWindow, SDL_DestroyWindow);
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(rawRenderer, SDL_DestroyRenderer);
    const SDL_AudioSpec spec{SDL_AUDIO_F32, AudioFrame::channels, AudioFrame::sampleRate};
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)> audio(
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr),
        SDL_DestroyAudioStream);
    require(static_cast<bool>(audio));
    auto dialog = std::make_shared<Dialog>();
    bool verify = false;
    bool preload = true;
    bool stress = false;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--verify") verify = true;
        else if (arg == "--no-preload") preload = false;
        else if (arg == "--stress") stress = true;
        else paths.emplace_back(argv[i]);
    }
    Player player(renderer.get(), audio.get(), preload);
    if (verify && paths.size() != 3) throw std::runtime_error("--verify requires three short MP4 files");
    if (!paths.empty()) player.load(std::move(paths));
    if (verify && stress) {
        for (int i = 0; i < 12; ++i) {
            player.move(1);
            player.move(1);
            player.move(-1);
            player.move(-1);
        }
        player.stop();
        player.update();
        if (!player.stopped() || player.position() != 0)
            throw std::runtime_error("stop did not reset position");
        player.pause();
    }

    const auto deadline = Clock::now() + std::chrono::seconds(15);
    unsigned verified = 0;
    bool testedPause = false;
    bool testedSeek = false;
    double pausePosition = 0;
    Clock::time_point pauseUntil;
    std::vector<std::filesystem::path> dropped;
    while (true) {
        bool redraw = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) return verify ? 1 : 0;
            if (event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                redraw = true;
            if (event.type == SDL_EVENT_DROP_BEGIN) dropped.clear();
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                dropped.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(event.drop.data)));
            if (event.type == SDL_EVENT_DROP_COMPLETE && !dropped.empty()) {
                player.load(std::move(dropped));
                dropped.clear();
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                int width = 0, height = 0;
                require(SDL_GetRenderOutputSize(renderer.get(), &width, &height));
                float x = 0, y = 0;
                require(SDL_RenderCoordinatesFromWindow(renderer.get(), event.button.x, event.button.y, &x, &y));
                const float rail = std::max(0.0f, height - 95.0f) + 8;
                if (width > 32 && x >= 16 && x <= width - 16 && y >= rail - 6 && y <= rail + 11)
                    player.seekFraction((x - 16) / (width - 32));
            }
            if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) continue;
            switch (event.key.key) {
            case SDLK_ESCAPE: return verify ? 1 : 0;
            case SDLK_SPACE: player.pause(); break;
            case SDLK_S: player.stop(); break;
            case SDLK_N: player.move(1); break;
            case SDLK_P: player.move(-1); break;
            case SDLK_LEFT: player.seek(player.position() - 5); break;
            case SDLK_RIGHT: player.seek(player.position() + 5); break;
            case SDLK_O: {
                bool open;
                {
                    std::lock_guard lock(dialog->mutex);
                    open = !dialog->open;
                    dialog->open = true;
                }
                static const SDL_DialogFileFilter filter{"MP4 video", "mp4;m4v"};
                if (open) SDL_ShowOpenFileDialog(picked, new std::shared_ptr<Dialog>(dialog),
                                                window.get(), &filter, 1, nullptr, true);
                break;
            }
            default: break;
            }
        }
        {
            std::vector<std::filesystem::path> selected;
            std::string error;
            {
                std::lock_guard lock(dialog->mutex);
                selected.swap(dialog->paths);
                error.swap(dialog->error);
            }
            if (!selected.empty()) player.load(std::move(selected));
            if (!error.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open MP4", error.c_str(), window.get());
        }
        player.update();
        player.draw(redraw);
        if (verify) {
            if (!player.error().empty() || Clock::now() > deadline)
                throw std::runtime_error("playback verification failed or timed out");
            if (!testedPause && player.position() > 0.15) {
                pausePosition = player.position();
                pauseUntil = Clock::now() + std::chrono::milliseconds(120);
                player.pause();
                testedPause = true;
            }
            if (testedPause && !testedSeek && Clock::now() >= pauseUntil) {
                if (player.position() != pausePosition) throw std::runtime_error("pause advanced position");
                player.seek(0.50);
                if (std::abs(player.position() - 0.50) > 0.001) throw std::runtime_error("seek position mismatch");
                player.pause();
                testedSeek = true;
            }
            if (player.finished()) {
                if (!testedSeek || player.presented() < 3 || player.audioFrames() < 1000 ||
                    player.maxVideoLag() > 0.15)
                    throw std::runtime_error("video, audio, or presentation timing verification failed");
                std::printf("clip=%u presented=%zu audio-samples/channel=%zu position=%.3f max-video-clock-error-ms=%.2f\n",
                    verified + 1, player.presented(), player.audioFrames(), player.position(), player.maxVideoLag() * 1000);
                if (++verified == 3) {
                    std::printf("peak-accounted-buffers-mib=%.2f\n", static_cast<double>(player.peakBuffers()) / MiB);
                    return 0;
                }
                if (preload && player.feed().snapshot().preparation != Preparation::ready)
                    throw std::runtime_error("next decoder was not ready");
                player.move(1);
            }
        }
        SDL_Delay(4);
    }
}
}

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "BG Video Demo: %s\n", error.what());
        return 1;
    }
}
