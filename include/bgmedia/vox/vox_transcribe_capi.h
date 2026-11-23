#ifndef BGMEDIA_VOX_TRANSCRIBE_CAPI_H
#define BGMEDIA_VOX_TRANSCRIBE_CAPI_H

/* C ABI for BG Vox Transcribe (JNI / iOS hosts). Never throws.
 *
 * Ownership: bgm_vox_whisper_create() returns a backend handle (the loaded
 * model) that the host destroys with bgm_vox_asr_free(). bgm_vox_transcribe_s16()
 * returns a result handle that the host destroys with bgm_vox_transcript_free().
 * Strings returned by accessors are owned by the result handle and stay valid
 * until it is freed. A backend handle must not be used concurrently from two
 * threads: the host serialises calls on one handle (it may create one handle per
 * thread instead). Calling bgm_vox_asr_free() while a transcribe on that handle
 * is still in flight is undefined behaviour; cancel, wait for the call to
 * return, then free. The text is a MACHINE transcript: always label it as such. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes mirror bgmedia::vox::VoxTranscribeStatus. */
typedef enum bgm_vox_transcribe_status {
    BGM_VOX_TR_OK = 0,
    BGM_VOX_TR_CANCELLED = 1,
    BGM_VOX_TR_INVALID_INPUT = 2,
    BGM_VOX_TR_UNSUPPORTED_FORMAT = 3,
    BGM_VOX_TR_TOO_LONG = 4,
    BGM_VOX_TR_NO_BACKEND = 5,
    BGM_VOX_TR_MODEL_UNAVAILABLE = 6,
    BGM_VOX_TR_BACKEND_ERROR = 7,
    BGM_VOX_TR_OUT_OF_MEMORY = 8,
    BGM_VOX_TR_INTERNAL_ERROR = 9
} bgm_vox_transcribe_status;

typedef struct bgm_vox_asr bgm_vox_asr;
typedef struct bgm_vox_transcript bgm_vox_transcript;

/* fraction in [0, 1], called from the transcribing thread. */
typedef void (*bgm_vox_progress_fn)(float fraction, void* user);

/* Loads a whisper.cpp ggml model (UTF-8 path). Returns BGM_VOX_TR_OK and sets
 * *out, BGM_VOX_TR_NO_BACKEND when the library was built without
 * BGMEDIA_VOX_WHISPER, or BGM_VOX_TR_MODEL_UNAVAILABLE when the file is missing
 * or not a valid model. `error` (may be NULL) receives a message truncated to
 * error_cap bytes, always NUL-terminated. threads 0 = default. The library
 * never downloads models. */
int bgm_vox_whisper_create(const char* model_path_utf8, int use_gpu, int threads,
                           bgm_vox_asr** out, char* error, size_t error_cap);

void bgm_vox_asr_free(bgm_vox_asr* asr);

/* Memory needs for the host's "will it fit" check: model file size and an
 * approximate resident size (estimate, not a measurement). Either pointer may
 * be NULL. Returns 0 on success, non-zero when asr is NULL. */
int bgm_vox_asr_info(const bgm_vox_asr* asr, uint64_t* model_file_bytes,
                     uint64_t* approx_ram_bytes);

/* Transcribes interleaved 16-bit PCM (8000..48000 Hz, 1..2 channels).
 * language: NULL or "" = auto-detect, else e.g. "ru". flags: bit 0 = translate
 * to English. max_duration_seconds <= 0 means the default (1800).
 * chunk_seconds <= 0 means the default (30). cancel_flag may be NULL; the call
 * stops when *cancel_flag becomes non-zero. progress may be NULL.
 * *out_result receives a result handle whenever the call got far enough to
 * make one (also for errors, so that the detail text is available); it is
 * NULL only when even that allocation failed. Returns the status. */
int bgm_vox_transcribe_s16(bgm_vox_asr* asr, const int16_t* in, size_t frames,
                           uint32_t sample_rate, uint32_t channels, const char* language,
                           int flags, double max_duration_seconds, double chunk_seconds,
                           const volatile int* cancel_flag, bgm_vox_progress_fn progress,
                           void* progress_user, bgm_vox_transcript** out_result);

void bgm_vox_transcript_free(bgm_vox_transcript* result);

int bgm_vox_transcript_status(const bgm_vox_transcript* result);
const char* bgm_vox_transcript_detail(const bgm_vox_transcript* result);
/* Language of the text ("" when unknown or no speech). */
const char* bgm_vox_transcript_language(const bgm_vox_transcript* result);
/* Always 1: the text is machine generated and may be wrong. */
int bgm_vox_transcript_machine_generated(const bgm_vox_transcript* result);
double bgm_vox_transcript_audio_seconds(const bgm_vox_transcript* result);
double bgm_vox_transcript_processing_ms(const bgm_vox_transcript* result);

size_t bgm_vox_transcript_segment_count(const bgm_vox_transcript* result);
/* Out-of-range index: 0 / "" / -1 / 1 (low confidence) respectively. */
double bgm_vox_transcript_segment_start(const bgm_vox_transcript* result, size_t index);
double bgm_vox_transcript_segment_end(const bgm_vox_transcript* result, size_t index);
const char* bgm_vox_transcript_segment_text(const bgm_vox_transcript* result, size_t index);
/* 0..1, negative = unknown. */
float bgm_vox_transcript_segment_confidence(const bgm_vox_transcript* result, size_t index);
int bgm_vox_transcript_segment_low_confidence(const bgm_vox_transcript* result, size_t index);

/* Static string, never NULL. */
const char* bgm_vox_transcribe_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* BGMEDIA_VOX_TRANSCRIBE_CAPI_H */
