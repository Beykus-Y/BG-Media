#include "bgmedia/video/FFmpeg.hpp"
#include "bgmedia/video/Playback.hpp"
#ifdef BGMEDIA_TOK_FIXTURE
#include "bgmedia/video/DiskCache.hpp"
#include "bg_tok_fixture.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

class SdlOutput final : public PlaybackOutput {
public:
    SdlOutput(SDL_Renderer* renderer, SDL_AudioStream* audio) : renderer_(renderer), audio_(audio) {}

    std::size_t queuedAudioFrames() const override {
        const int bytes = SDL_GetAudioStreamQueued(audio_);
        return static_cast<std::size_t>(std::max(bytes, 0)) / (AudioFrame::channels * sizeof(float));
    }
    void queueAudio(std::span<const float> samples) override {
        require(SDL_PutAudioStreamData(audio_, samples.data(), static_cast<int>(samples.size_bytes())));
    }
    void pauseAudio(bool paused) override {
        require(paused ? SDL_PauseAudioStreamDevice(audio_) : SDL_ResumeAudioStreamDevice(audio_));
    }
    void clearAudio() override { require(SDL_ClearAudioStream(audio_)); }
    void flushAudio() override { require(SDL_FlushAudioStream(audio_)); }

    void present(const VideoFrame& frame) override {
        if (!texture || width != frame.width || height != frame.height) {
            width = frame.width;
            height = frame.height;
            texture.reset(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STREAMING, width, height));
            require(static_cast<bool>(texture));
        }
        require(SDL_UpdateTexture(texture.get(), nullptr, frame.rgba.data(), width * 4));
        if (!visible) firstFramePending = true;
        visible = dirty = true;
    }

    void clearVideo() override {
        firstFramePending = visible = false;
        dirty = true;
        switchStart = Clock::now();
        firstFrameMs = 0;
    }

    std::size_t textureBytes() const {
        return texture ? static_cast<std::size_t>(width) * height * 4 : 0;
    }

    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture{nullptr, SDL_DestroyTexture};
    int width = 0, height = 0;
    bool firstFramePending = false, visible = false, dirty = true;
    double firstFrameMs = 0;
    Clock::time_point switchStart{};
private:
    SDL_Renderer* renderer_;
    SDL_AudioStream* audio_;
};

class Player {
public:
    Player(SDL_Renderer* renderer, SDL_AudioStream* audio, bool preload, std::filesystem::path tokCache)
        : output_(renderer, audio),
          playback_(std::make_shared<FFmpegProvider>(), [](const Media& media) { return openFFmpeg(media); },
                    output_, preload ? PlaybackOptions{} : PlaybackOptions{.preparationBytes = 0}),
          renderer_(renderer), tokCache_(std::move(tokCache)) {}
    void load(std::vector<std::filesystem::path> files) {
        files_ = files;
        std::vector<Media> queue;
#ifdef BGMEDIA_TOK_FIXTURE
        if (!tokCache_.empty()) {
            std::vector<std::string> names;
            std::vector<const char*> pointers;
            for (const auto& file : files) names.push_back(file.string());
            for (const auto& name : names) pointers.push_back(name.c_str());
            std::shared_ptr<bg_tok_fixture> host(bg_tok_fixture_create(pointers.data(), pointers.size()), bg_tok_fixture_destroy);
            if (!host || bg_tok_fixture_count(host.get()) != files.size()) throw std::runtime_error("BG Tok fixture open failed");
            auto cache = cache_ ? cache_ : std::make_shared<DiskCache>(tokCache_);
            for (std::size_t i = 0; i < files.size(); ++i) {
                bg_tok_fixture_range range{};
                if (bg_tok_fixture_open(host.get(), i, &range) != 0) throw std::runtime_error("BG Tok source open failed");
                bg_video_range_v1 callbacks{range.context, range.length, range.read_at, range.release};
                std::shared_ptr<ByteSource> source;
                try { source = adoptRangeSource(callbacks); }
                catch (...) { range.release(range.context); throw; }
                const auto size = bg_tok_fixture_revision(host.get(), i, nullptr, 0);
                if (!size || size > 4096) throw std::runtime_error("BG Tok source revision unavailable");
                std::string revision(size, '\0');
                if (bg_tok_fixture_revision(host.get(), i, revision.data(), revision.size()) != size)
                    throw std::runtime_error("BG Tok source revision changed");
                revision.pop_back();
                MediaId id{"bg-tok-fixture", account_, std::to_string(i)};
                queue.push_back({id, {}, cache->wrap({id, revision}, DiskPolicy::hostManaged, source)});
            }
            playback_.setQueue(std::move(queue));
            host_ = std::move(host);
            cache_ = std::move(cache);
            return;
        }
#else
        if (!tokCache_.empty()) throw std::runtime_error("BG Tok fixture support was not built");
#endif
        for (auto& file : files)
            queue.push_back({{"local", "", std::to_string(queue.size())}, std::move(file)});
        playback_.setQueue(std::move(queue));
    }
    void move(int direction) { if (direction < 0) playback_.previous(); else playback_.next(); }
    void pause() {
        const auto state = snapshot().state;
        if (state == PlaybackState::playing || state == PlaybackState::buffering) playback_.pause();
        else playback_.play();
    }
    void stop() { playback_.stop(); }
    void switchAccount() {
#ifdef BGMEDIA_TOK_FIXTURE
        if (!host_ || !cache_ || files_.empty()) return;
        bg_tok_fixture_cancel(host_.get());
        playback_.close();
        cache_->clearAccount("bg-tok-fixture", account_);
        account_ = account_ == "demo-a" ? "demo-b" : "demo-a";
        load(files_);
#endif
    }
    void seek(double seconds) { playback_.seek(seconds); }
    void seekFraction(double fraction) {
        const auto duration = snapshot().info.duration;
        if (duration > 0) seek(fraction * duration);
    }
    void update() {
        playback_.update();
        const auto state = snapshot();
        peakBuffers_ = std::max(peakBuffers_, state.bufferedBytes + output_.textureBytes());
    }
    void draw(bool force);
    PlaybackSnapshot snapshot() const { return playback_.snapshot(); }
    double position() const { return snapshot().position; }
    std::size_t peakBuffers() const { return peakBuffers_; }
    std::uint64_t providerCalls() const {
#ifdef BGMEDIA_TOK_FIXTURE
        return host_ ? bg_tok_fixture_read_calls(host_.get()) : 0;
#else
        return 0;
#endif
    }
    std::uint64_t reusedBytes() const {
#ifdef BGMEDIA_TOK_FIXTURE
        return cache_ ? cache_->reusedBytes() : 0;
#else
        return 0;
#endif
    }
private:
    std::string status(const PlaybackSnapshot&) const;
    std::string cacheStatus() const;
    SdlOutput output_;
    Playback playback_;
    SDL_Renderer* renderer_;
    std::size_t rss_ = 0, peakBuffers_ = 0;
    Clock::time_point lastDraw_{}, lastMemorySample_{};
    std::filesystem::path tokCache_;
    std::vector<std::filesystem::path> files_;
#ifdef BGMEDIA_TOK_FIXTURE
    std::string account_ = "demo-a";
    std::shared_ptr<bg_tok_fixture> host_;
    std::shared_ptr<DiskCache> cache_;
#endif
};

std::string Player::status(const PlaybackSnapshot& snapshot) const {
    const auto preparation = snapshot.feed.preparation;
    const char* next = preparation == Preparation::ready ? "ready" :
        preparation == Preparation::loading ? "loading" : preparation == Preparation::error ? "error" : "-";
    const char* state = "idle";
    switch (snapshot.state) {
    case PlaybackState::idle: return "Open MP4 files to play";
    case PlaybackState::error: return snapshot.error;
    case PlaybackState::buffering: state = "buffering"; break;
    case PlaybackState::playing: state = "playing"; break;
    case PlaybackState::paused: state = "paused"; break;
    case PlaybackState::stopped: state = "stopped"; break;
    case PlaybackState::ended: state = "ended"; break;
    }
    char text[256]{};
    std::snprintf(text, sizeof(text), "%zu/%zu  %s  %.2f / %.2fs  %.1f / %.1f FPS  next %s  buffers %.2f MiB",
        snapshot.feed.current.value_or(0) + 1, snapshot.queueSize, state, snapshot.position, snapshot.info.duration,
        snapshot.presentationFps, snapshot.info.fps, next,
        static_cast<double>(snapshot.bufferedBytes + output_.textureBytes()) / MiB);
    std::string result = text;
    if (output_.firstFrameMs > 0) result += "  first frame " + std::to_string(static_cast<int>(output_.firstFrameMs)) + " ms";
    if (!rss_) return result;
    char memory[32]{};
    std::snprintf(memory, sizeof(memory), "  RSS %.1f MiB", static_cast<double>(rss_) / MiB);
    return result + memory;
}

std::string Player::cacheStatus() const {
#ifdef BGMEDIA_TOK_FIXTURE
    if (host_ && cache_) return "Account " + account_ + "  cache " + std::to_string(cache_->storedBytes() / 1024) + " KiB  reused " + std::to_string(reusedBytes() / 1024) + " KiB  requests " + std::to_string(providerCalls());
#endif
    return {};
}

void Player::draw(bool force) {
    const auto now = Clock::now();
    if (!force && !output_.dirty && now - lastDraw_ < std::chrono::milliseconds(100)) return;
    lastDraw_ = now;
    output_.dirty = false;
    const auto playback = snapshot();
    if (now - lastMemorySample_ >= std::chrono::milliseconds(500)) {
        rss_ = residentBytes();
        lastMemorySample_ = now;
    }
    int w = 0, h = 0;
    require(SDL_GetRenderOutputSize(renderer_, &w, &h));
    SDL_SetRenderDrawColor(renderer_, 15, 18, 24, 255);
    require(SDL_RenderClear(renderer_));
    const float contentHeight = std::max(0.0f, h - 112.0f);
    if (output_.texture && output_.visible && contentHeight > 0) {
        const float scale = std::min(static_cast<float>(w) / output_.width, contentHeight / output_.height);
        const SDL_FRect dst{(w - output_.width * scale) / 2, (contentHeight - output_.height * scale) / 2,
                            output_.width * scale, output_.height * scale};
        require(SDL_RenderTexture(renderer_, output_.texture.get(), nullptr, &dst));
    }
    SDL_SetRenderDrawColor(renderer_, 57, 67, 84, 255);
    const SDL_FRect rail{16, contentHeight + 8, std::max(0.0f, w - 32.0f), 5};
    SDL_RenderFillRect(renderer_, &rail);
    if (playback.info.duration > 0) {
        SDL_SetRenderDrawColor(renderer_, 92, 205, 177, 255);
        auto progress = rail;
        progress.w *= static_cast<float>(std::clamp(playback.position / playback.info.duration, 0.0, 1.0));
        SDL_RenderFillRect(renderer_, &progress);
    }
    SDL_SetRenderDrawColor(renderer_, 228, 236, 247, 255);
    SDL_RenderDebugText(renderer_, 16, contentHeight + 25, status(playback).c_str());
    SDL_RenderDebugText(renderer_, 16, contentHeight + 42, cacheStatus().c_str());
    SDL_RenderDebugText(renderer_, 16, contentHeight + 59, "O Open files   Space Pause/Play   S Stop");
    SDL_RenderDebugText(renderer_, 16, contentHeight + 76, "N / P Next / Previous   Left / Right Seek");
#ifdef BGMEDIA_TOK_FIXTURE
    if (host_) SDL_RenderDebugText(renderer_, 16, contentHeight + 93, "A Switch test account (clears old account cache)");
#endif
    require(SDL_RenderPresent(renderer_));
    if (output_.firstFramePending) {
        output_.firstFramePending = false;
        const auto latency = std::chrono::duration<double, std::milli>(Clock::now() - output_.switchStart).count();
        output_.firstFrameMs = latency;
        std::printf("%zu %s first-frame-ms=%.2f\n", playback.feed.current.value_or(0) + 1, playback.preparedDecoder ? "warm" : "cold", latency);
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
    std::filesystem::path tokCache;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--verify") verify = true;
        else if (arg == "--no-preload") preload = false;
        else if (arg == "--stress") stress = true;
        else if (arg == "--tok-cache" && i + 1 < argc) tokCache = argv[++i];
        else paths.emplace_back(argv[i]);
    }
    Player player(renderer.get(), audio.get(), preload, tokCache);
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
        if (player.snapshot().state != PlaybackState::stopped || player.position() != 0)
            throw std::runtime_error("stop did not reset position");
        player.pause();
        if (!tokCache.empty()) player.switchAccount();
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
                const float rail = std::max(0.0f, height - 112.0f) + 8;
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
            case SDLK_A: player.switchAccount(); break;
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
            const auto playback = player.snapshot();
            if (playback.state == PlaybackState::error)
                throw std::runtime_error("playback verification failed: " + playback.error);
            if (Clock::now() > deadline) throw std::runtime_error("playback verification timed out");
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
            if (playback.state == PlaybackState::ended) {
                if (!testedSeek || playback.presentedFrames < 3 || playback.submittedAudioFrames < 1000 ||
                    playback.maxPresentationError > 0.15)
                    throw std::runtime_error("video, audio, or presentation timing verification failed");
                std::printf("clip=%u presented=%zu audio-samples/channel=%zu position=%.3f max-video-clock-error-ms=%.2f\n",
                    verified + 1, playback.presentedFrames, playback.submittedAudioFrames, player.position(), playback.maxPresentationError * 1000);
                if (++verified == 3) {
                    if (!tokCache.empty() && player.providerCalls() == 0)
                        throw std::runtime_error("BG Tok provider was not called");
                    if (!tokCache.empty()) std::printf("bg-tok-read-calls=%llu\n", static_cast<unsigned long long>(player.providerCalls()));
                    if (!tokCache.empty()) std::printf("cache-reused-bytes=%llu\n", static_cast<unsigned long long>(player.reusedBytes()));
                    std::printf("peak-accounted-buffers-mib=%.2f\n", static_cast<double>(player.peakBuffers()) / MiB);
                    return 0;
                }
                if (preload && playback.feed.preparation != Preparation::ready)
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
