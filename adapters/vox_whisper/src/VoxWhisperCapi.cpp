// C ABI factory for the optional whisper.cpp backend. Lives in the adapter
// (not in bgmedia_vox) so that the core library has no link-time dependency on
// whisper; the core provides a NoBackend stub when the adapter is not built.

#include "bgmedia/vox/VoxWhisper.hpp"
#include "bgmedia/vox/vox_transcribe_capi.h"

#include "VoxTranscribeCapi.hpp"

#include <cstring>
#include <new>

extern "C" int bgm_vox_whisper_create(const char* model_path_utf8, int use_gpu, int threads,
                                      bgm_vox_asr** out, char* error, size_t error_cap) {
    using namespace bgmedia::vox;
    if (out != nullptr) {
        *out = nullptr;
    }
    if (error != nullptr && error_cap > 0) {
        error[0] = '\0';
    }
    try {
        VoxWhisperConfig cfg;
        if (model_path_utf8 != nullptr) {
            cfg.modelPath = model_path_utf8;
        }
        cfg.useGpu = use_gpu != 0;
        cfg.threads = threads;
        std::string message;
        VoxTranscribeStatus status = VoxTranscribeStatus::InternalError;
        auto backend = createWhisperBackend(cfg, &message, &status);
        if (!backend) {
            if (error != nullptr && error_cap > 0) {
                const size_t n = message.size() < error_cap - 1 ? message.size() : error_cap - 1;
                std::memcpy(error, message.data(), n);
                error[n] = '\0';
            }
            return static_cast<int>(status == VoxTranscribeStatus::Ok
                                        ? VoxTranscribeStatus::InternalError
                                        : status);
        }
        auto* handle = new bgm_vox_asr();
        handle->backend = std::move(backend);
        if (out != nullptr) {
            *out = handle;
        } else {
            delete handle;
        }
        return static_cast<int>(VoxTranscribeStatus::Ok);
    } catch (...) {
        return static_cast<int>(VoxTranscribeStatus::OutOfMemory);
    }
}
