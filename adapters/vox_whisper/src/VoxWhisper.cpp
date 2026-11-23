#include "bgmedia/vox/VoxWhisper.hpp"

#include "whisper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <new>
#include <thread>
#include <utility>

namespace bgmedia::vox {

namespace {

namespace fs = std::filesystem;

fs::path pathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// "ru-RU" / "RU" -> "ru"
std::string primaryLanguage(const std::string& tag) {
    std::string out;
    for (char ch : tag) {
        if (ch == '-' || ch == '_') {
            break;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

struct CallbackState {
    const VoxCancelToken* cancel = nullptr;
    const VoxProgressFn* progress = nullptr;
};

bool abortCallback(void* user) {
    const auto* s = static_cast<const CallbackState*>(user);
    return s->cancel != nullptr && s->cancel->isCancelled();
}

void progressCallback(struct whisper_context*, struct whisper_state*, int progress, void* user) {
    const auto* s = static_cast<const CallbackState*>(user);
    if (s->progress != nullptr && *s->progress) {
        (*s->progress)(static_cast<float>(progress) / 100.0f);
    }
}

class WhisperBackend final : public IVoxAsrBackend {
public:
    WhisperBackend(whisper_context* ctx, std::string modelName, std::uint64_t fileBytes,
                   int threads)
        : ctx_(ctx), modelName_(std::move(modelName)), fileBytes_(fileBytes), threads_(threads) {}
    ~WhisperBackend() override {
        if (ctx_ != nullptr) {
            whisper_free(ctx_);
        }
    }

    const char* name() const noexcept override { return "whisper.cpp"; }
    std::uint32_t expectedSampleRate() const noexcept override { return 16000; }

    VoxAsrBackendInfo info() const override {
        VoxAsrBackendInfo i;
        i.modelName = modelName_;
        i.modelFileBytes = fileBytes_;
        // Estimate: weights plus KV cache / compute buffers. Calibrated against the
        // whisper.cpp README memory table, NOT measured on a target device.
        i.approxRamBytes = static_cast<std::uint64_t>(static_cast<double>(fileBytes_) * 1.2) +
                           (256ull << 20);
        i.multilingual = whisper_is_multilingual(ctx_) != 0;
        if (!i.multilingual) {
            i.languages = {"en"};
        }
        i.supportsTranslation = i.multilingual;
        i.supportsWordTimestamps = true;
        return i;
    }

    VoxTranscribeStatus transcribeChunk(const float* mono, std::size_t frames,
                                        const VoxTranscribeOptions& options,
                                        const VoxCancelToken* cancel,
                                        const VoxProgressFn* chunkProgress,
                                        std::vector<VoxSegment>& out,
                                        std::string& error) override {
        const bool multilingual = whisper_is_multilingual(ctx_) != 0;
        std::string lang = primaryLanguage(options.language);
        if (!lang.empty() && lang != "auto") {
            if (whisper_lang_id(lang.c_str()) < 0) {
                error = "language not supported by whisper: " + options.language;
                return VoxTranscribeStatus::InvalidInput;
            }
            if (!multilingual && lang != "en") {
                error = "English-only model cannot transcribe language " + lang;
                return VoxTranscribeStatus::InvalidInput;
            }
        } else {
            lang = multilingual ? "auto" : "en";
        }
        if (options.translateToEnglish && !multilingual) {
            error = "English-only model cannot translate";
            return VoxTranscribeStatus::InvalidInput;
        }

        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.n_threads = options.threads > 0 ? options.threads : threads_;
        p.language = lang.c_str();
        p.detect_language = false;
        p.translate = options.translateToEnglish;
        p.no_context = true;
        p.print_progress = false;
        p.print_realtime = false;
        p.print_timestamps = false;
        p.print_special = false;
        p.suppress_nst = true;
        p.token_timestamps = options.wordTimestamps;
        if (!options.initialPrompt.empty()) {
            p.initial_prompt = options.initialPrompt.c_str();
        }
        CallbackState cb{cancel, chunkProgress};
        p.abort_callback = &abortCallback;
        p.abort_callback_user_data = &cb;
        p.progress_callback = &progressCallback;
        p.progress_callback_user_data = &cb;

        // whisper needs at least 1 s of audio; pad short chunks with silence (the
        // frontend clamps the resulting timestamps back to the real duration).
        const std::size_t minFrames = 16000 + 1600;
        std::vector<float> padded;
        const float* data = mono;
        std::size_t n = frames;
        if (frames < minFrames) {
            padded.assign(minFrames, 0.0f);
            std::copy(mono, mono + frames, padded.begin());
            data = padded.data();
            n = minFrames;
        }

        const int rc = whisper_full(ctx_, p, data, static_cast<int>(n));
        if (cancel != nullptr && cancel->isCancelled()) {
            return VoxTranscribeStatus::Cancelled;
        }
        if (rc != 0) {
            error = "whisper_full failed with code " + std::to_string(rc);
            return VoxTranscribeStatus::BackendError;
        }

        const int count = whisper_full_n_segments(ctx_);
        const int langId = whisper_full_lang_id(ctx_);
        const char* langStr = langId >= 0 ? whisper_lang_str(langId) : nullptr;
        const whisper_token eot = whisper_token_eot(ctx_);
        for (int i = 0; i < count; ++i) {
            VoxSegment seg;
            seg.startSec = static_cast<double>(whisper_full_get_segment_t0(ctx_, i)) * 0.01;
            seg.endSec = static_cast<double>(whisper_full_get_segment_t1(ctx_, i)) * 0.01;
            const char* text = whisper_full_get_segment_text(ctx_, i);
            seg.text = text != nullptr ? text : "";
            if (langStr != nullptr) {
                seg.language = langStr;
            }
            double sumP = 0.0;
            int textTokens = 0;
            const int nTok = whisper_full_n_tokens(ctx_, i);
            VoxWord word;
            double wordP = 0.0;
            int wordN = 0;
            auto flush = [&]() {
                if (wordN > 0 && !word.text.empty()) {
                    word.confidence = static_cast<float>(wordP / wordN);
                    if (word.endSec < word.startSec) {
                        word.endSec = word.startSec;
                    }
                    seg.words.push_back(std::move(word));
                }
                word = VoxWord{};
                wordP = 0.0;
                wordN = 0;
            };
            for (int t = 0; t < nTok; ++t) {
                const whisper_token_data d = whisper_full_get_token_data(ctx_, i, t);
                if (d.id >= eot) {
                    continue;  // special / timestamp token
                }
                sumP += static_cast<double>(d.p);
                ++textTokens;
                if (!options.wordTimestamps) {
                    continue;
                }
                const char* tt = whisper_full_get_token_text(ctx_, i, t);
                if (tt == nullptr) {
                    continue;
                }
                const bool startsWord = tt[0] == ' ' || wordN == 0;
                if (startsWord && wordN > 0) {
                    flush();
                }
                if (wordN == 0) {
                    word.startSec = d.t0 >= 0 ? static_cast<double>(d.t0) * 0.01 : seg.startSec;
                }
                word.text += tt;
                word.endSec = d.t1 >= 0 ? static_cast<double>(d.t1) * 0.01 : seg.endSec;
                wordP += static_cast<double>(d.p);
                ++wordN;
            }
            flush();
            seg.confidence = textTokens > 0 ? static_cast<float>(sumP / textTokens) : -1.0f;
            out.push_back(std::move(seg));
        }
        return VoxTranscribeStatus::Ok;
    }

private:
    whisper_context* ctx_;
    std::string modelName_;
    std::uint64_t fileBytes_;
    int threads_;
};

} // namespace

std::unique_ptr<IVoxAsrBackend> createWhisperBackend(const VoxWhisperConfig& config,
                                                     std::string* error,
                                                     VoxTranscribeStatus* status) noexcept {
    auto fail = [&](VoxTranscribeStatus st, const std::string& msg) {
        try {
            if (error != nullptr) {
                *error = msg;
            }
        } catch (...) {
        }
        if (status != nullptr) {
            *status = st;
        }
        return std::unique_ptr<IVoxAsrBackend>();
    };
    try {
        if (config.modelPath.empty()) {
            return fail(VoxTranscribeStatus::ModelUnavailable, "model path is empty");
        }
        const fs::path path = pathFromUtf8(config.modelPath);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            return fail(VoxTranscribeStatus::ModelUnavailable,
                        "model file not found: " + config.modelPath);
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
        if (ec || bytes < 1024) {
            return fail(VoxTranscribeStatus::ModelUnavailable,
                        "model file is empty or unreadable: " + config.modelPath);
        }
        whisper_context_params cp = whisper_context_default_params();
        cp.use_gpu = config.useGpu;
        whisper_context* ctx = whisper_init_from_file_with_params(config.modelPath.c_str(), cp);
        if (ctx == nullptr) {
            return fail(VoxTranscribeStatus::ModelUnavailable,
                        "whisper.cpp could not load the model (corrupt, truncated or unsupported "
                        "format): " + config.modelPath);
        }
        int threads = config.threads;
        if (threads <= 0) {
            threads = static_cast<int>(std::min(4u, std::max(1u, std::thread::hardware_concurrency())));
        }
        const std::u8string stem8 = path.stem().u8string();
        std::string name(reinterpret_cast<const char*>(stem8.data()), stem8.size());
        auto backend = std::make_unique<WhisperBackend>(ctx, name, bytes, threads);
        if (status != nullptr) {
            *status = VoxTranscribeStatus::Ok;
        }
        return backend;
    } catch (const std::bad_alloc&) {
        return fail(VoxTranscribeStatus::OutOfMemory, "out of memory while loading the model");
    } catch (...) {
        return fail(VoxTranscribeStatus::InternalError, "unexpected exception while loading the model");
    }
}

} // namespace bgmedia::vox
