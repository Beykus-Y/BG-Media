#include "bgmedia/vox/vox_transcribe_capi.h"

#include "VoxTranscribeCapi.hpp"

#include <cstring>
#include <new>

using namespace bgmedia::vox;

extern "C" {

#if !defined(BGMEDIA_VOX_WHISPER_ADAPTER) || !BGMEDIA_VOX_WHISPER_ADAPTER
// Without the optional adapter the factory still exists and reports why.
int bgm_vox_whisper_create(const char*, int, int, bgm_vox_asr** out, char* error,
                           size_t error_cap) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (error != nullptr && error_cap > 0) {
        const char msg[] = "built without BGMEDIA_VOX_WHISPER";
        const size_t n = error_cap - 1 < sizeof(msg) - 1 ? error_cap - 1 : sizeof(msg) - 1;
        std::memcpy(error, msg, n);
        error[n] = '\0';
    }
    return BGM_VOX_TR_NO_BACKEND;
}
#endif

void bgm_vox_asr_free(bgm_vox_asr* asr) {
    try {
        delete asr;
    } catch (...) {
    }
}

int bgm_vox_asr_info(const bgm_vox_asr* asr, uint64_t* model_file_bytes,
                     uint64_t* approx_ram_bytes) {
    if (asr == nullptr || !asr->backend) {
        return 1;
    }
    try {
        const VoxAsrBackendInfo info = asr->backend->info();
        if (model_file_bytes != nullptr) {
            *model_file_bytes = info.modelFileBytes;
        }
        if (approx_ram_bytes != nullptr) {
            *approx_ram_bytes = info.approxRamBytes;
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

int bgm_vox_transcribe_s16(bgm_vox_asr* asr, const int16_t* in, size_t frames,
                           uint32_t sample_rate, uint32_t channels, const char* language,
                           int flags, double max_duration_seconds, double chunk_seconds,
                           const volatile int* cancel_flag, bgm_vox_progress_fn progress,
                           void* progress_user, bgm_vox_transcript** out_result) {
    if (out_result != nullptr) {
        *out_result = nullptr;
    }
    bgm_vox_transcript* holder = nullptr;
    try {
        holder = new bgm_vox_transcript();
        VoxTranscribeOptions options;
        if (language != nullptr) {
            options.language = language;
        }
        options.translateToEnglish = (flags & 1) != 0;
        if (max_duration_seconds > 0.0) {
            options.maxDurationSeconds = max_duration_seconds;
        }
        if (chunk_seconds > 0.0) {
            options.chunkSeconds = chunk_seconds;
        }
        VoxCancelToken token(cancel_flag);
        VoxProgressFn progressFn;
        if (progress != nullptr) {
            progressFn = [progress, progress_user](float f) { progress(f, progress_user); };
        }
        if (asr == nullptr || !asr->backend) {
            holder->transcript.status = VoxTranscribeStatus::NoBackend;
            holder->transcript.detail = "no ASR backend";
        } else {
            std::lock_guard<std::mutex> lock(asr->mutex);
            holder->transcript = transcribe(asr->backend.get(), in, frames,
                                            VoxPcmFormat{sample_rate, channels}, options,
                                            &token, progressFn);
        }
        const int status = static_cast<int>(holder->transcript.status);
        if (out_result != nullptr) {
            *out_result = holder;
        } else {
            delete holder;
        }
        return status;
    } catch (...) {
        delete holder;
        return static_cast<int>(VoxTranscribeStatus::InternalError);
    }
}

void bgm_vox_transcript_free(bgm_vox_transcript* result) {
    try {
        delete result;
    } catch (...) {
    }
}

int bgm_vox_transcript_status(const bgm_vox_transcript* r) {
    return r != nullptr ? static_cast<int>(r->transcript.status)
                        : static_cast<int>(VoxTranscribeStatus::InvalidInput);
}
const char* bgm_vox_transcript_detail(const bgm_vox_transcript* r) {
    return r != nullptr ? r->transcript.detail.c_str() : "";
}
const char* bgm_vox_transcript_language(const bgm_vox_transcript* r) {
    return r != nullptr ? r->transcript.detectedLanguage.c_str() : "";
}
int bgm_vox_transcript_machine_generated(const bgm_vox_transcript*) { return 1; }
double bgm_vox_transcript_audio_seconds(const bgm_vox_transcript* r) {
    return r != nullptr ? r->transcript.audioSeconds : 0.0;
}
double bgm_vox_transcript_processing_ms(const bgm_vox_transcript* r) {
    return r != nullptr ? r->transcript.processingMs : 0.0;
}

size_t bgm_vox_transcript_segment_count(const bgm_vox_transcript* r) {
    return r != nullptr ? r->transcript.segments.size() : 0;
}

static const VoxSegment* segmentAt(const bgm_vox_transcript* r, size_t i) {
    return r != nullptr && i < r->transcript.segments.size() ? &r->transcript.segments[i]
                                                             : nullptr;
}

double bgm_vox_transcript_segment_start(const bgm_vox_transcript* r, size_t i) {
    const VoxSegment* s = segmentAt(r, i);
    return s != nullptr ? s->startSec : 0.0;
}
double bgm_vox_transcript_segment_end(const bgm_vox_transcript* r, size_t i) {
    const VoxSegment* s = segmentAt(r, i);
    return s != nullptr ? s->endSec : 0.0;
}
const char* bgm_vox_transcript_segment_text(const bgm_vox_transcript* r, size_t i) {
    const VoxSegment* s = segmentAt(r, i);
    return s != nullptr ? s->text.c_str() : "";
}
float bgm_vox_transcript_segment_confidence(const bgm_vox_transcript* r, size_t i) {
    const VoxSegment* s = segmentAt(r, i);
    return s != nullptr ? s->confidence : -1.0f;
}
int bgm_vox_transcript_segment_low_confidence(const bgm_vox_transcript* r, size_t i) {
    const VoxSegment* s = segmentAt(r, i);
    return s == nullptr || s->lowConfidence ? 1 : 0;
}

const char* bgm_vox_transcribe_status_name(int status) {
    if (status < 0 || status > static_cast<int>(VoxTranscribeStatus::InternalError)) {
        return "Unknown";
    }
    return voxTranscribeStatusName(static_cast<VoxTranscribeStatus>(status));
}

} // extern "C"
