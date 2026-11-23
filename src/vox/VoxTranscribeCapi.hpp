#pragma once

// Internal layout of the opaque C ABI handles, shared between the core
// library and the optional whisper adapter (which creates backend handles).

#include "bgmedia/vox/VoxTranscribe.hpp"

#include <memory>
#include <mutex>

struct bgm_vox_asr {
    std::unique_ptr<bgmedia::vox::IVoxAsrBackend> backend;
    std::mutex mutex;  // serialises use of one backend from several threads
};

struct bgm_vox_transcript {
    bgmedia::vox::VoxTranscript transcript;
};
