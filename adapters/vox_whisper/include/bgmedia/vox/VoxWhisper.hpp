#pragma once

// Optional whisper.cpp backend for BG Vox Transcribe. Built only with
// -DBGMEDIA_VOX_WHISPER=ON; the core library and its headers never include
// whisper.h, so hosts that do not enable the adapter carry no ASR dependency.

#include "bgmedia/vox/VoxTranscribe.hpp"

#include <memory>
#include <string>

namespace bgmedia::vox {

struct VoxWhisperConfig {
    // UTF-8 path to a ggml whisper model (ggml-tiny.bin, ggml-base.bin, ...).
    // Downloading, verifying and deleting model files is the host's job.
    // Note: whisper.cpp opens the file with fopen(), so on Windows a path with
    // characters outside the active code page may fail to load even though the
    // existence check (std::filesystem) succeeded; keep model files in an
    // ASCII path there.
    std::string modelPath;
    bool useGpu = false;  // CPU by default; GPU only if whisper.cpp was built with it
    int threads = 0;      // 0 = min(4, hardware concurrency)
};

// Loads the model (this is the slow, memory-hungry step: do it once, on a
// worker thread, and reuse the backend). Returns nullptr and fills `error`
// when the file is missing or is not a valid model; `status` (optional) then
// says ModelUnavailable (or OutOfMemory / InternalError). The returned
// backend is not thread-safe: one transcribe() at a time per instance.
[[nodiscard]] std::unique_ptr<IVoxAsrBackend> createWhisperBackend(
    const VoxWhisperConfig& config, std::string* error = nullptr,
    VoxTranscribeStatus* status = nullptr) noexcept;

} // namespace bgmedia::vox
