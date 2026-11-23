#ifndef BGMEDIA_VOX_LYRICS_CAPI_H
#define BGMEDIA_VOX_LYRICS_CAPI_H

/* C ABI for BG Vox Lyrics (JNI / iOS hosts). Never throws.
 *
 * The library never fetches lyrics: the host passes provider text it obtained
 * legally (a BeyGram adapter), and sets the provenance flags itself.
 * Ownership: every bgm_vox_lyrics* returned through an out parameter is owned
 * by the host and destroyed with bgm_vox_lyrics_free(). Strings returned by
 * accessors belong to the handle and stay valid until it is freed or (for
 * bgm_vox_lyrics_to_lrc) until the next to_lrc call on the same handle. The
 * handle is not thread-safe; use one handle per thread. Text produced by ASR is
 * never the original lyrics (see bgm_vox_lyrics_is_original_text). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors bgmedia::vox::VoxLyricsStatus. */
typedef enum bgm_vox_lyrics_status_e {
    BGM_VOX_LY_OK = 0,
    BGM_VOX_LY_CANCELLED = 1,
    BGM_VOX_LY_INVALID_INPUT = 2,
    BGM_VOX_LY_UNSUPPORTED_FORMAT = 3,
    BGM_VOX_LY_TOO_LONG = 4,
    BGM_VOX_LY_PARSE_ERROR = 5,
    BGM_VOX_LY_NO_LYRICS = 6,
    BGM_VOX_LY_NO_VOCALS = 7,
    BGM_VOX_LY_ALIGNMENT_FAILED = 8,
    BGM_VOX_LY_NO_BACKEND = 9,
    BGM_VOX_LY_BACKEND_ERROR = 10,
    BGM_VOX_LY_OUT_OF_MEMORY = 11,
    BGM_VOX_LY_INTERNAL_ERROR = 12
} bgm_vox_lyrics_status_code;

/* Mirrors bgmedia::vox::VoxLyricsSource. */
typedef enum bgm_vox_lyrics_source_e {
    BGM_VOX_LY_SRC_PROVIDER_SYNCED = 0,
    BGM_VOX_LY_SRC_PROVIDER_UNSYNCED = 1,
    BGM_VOX_LY_SRC_ALIGNED_TO_AUDIO = 2,
    BGM_VOX_LY_SRC_ASR_HYPOTHESIS = 3
} bgm_vox_lyrics_source_code;

typedef struct bgm_vox_lyrics bgm_vox_lyrics;

/* fraction in [0, 1], called from the working thread. */
typedef void (*bgm_vox_lyrics_progress_fn)(float fraction, void* user);

/* Parsers (UTF-8 text, len bytes, not necessarily NUL-terminated). *out always
 * receives a handle when the allocation worked (also for errors, so that the
 * detail is available); NULL only when even that failed. Return the status. */
int bgm_vox_lyrics_parse_lrc(const char* text, size_t len, bgm_vox_lyrics** out);
int bgm_vox_lyrics_parse_plain(const char* text, size_t len, bgm_vox_lyrics** out);

/* The host sets what the provider's terms allow; the library only carries it. */
void bgm_vox_lyrics_set_provenance(bgm_vox_lyrics* lyrics, const char* provider_tag,
                                   int host_may_cache);

/* Spreads the lines of `unsynced` over the vocal regions found in 16-bit PCM
 * (8000..48000 Hz, 1..2 channels) and, when instrumental_gap_seconds > 0, adds
 * instrumental lines for longer gaps. A ROUGH estimate (confidence <= 0.3,
 * lines flagged interpolated). cancel_flag / progress may be NULL. */
int bgm_vox_lyrics_align_to_vocals_s16(const bgm_vox_lyrics* unsynced, const int16_t* pcm,
                                       size_t frames, uint32_t sample_rate, uint32_t channels,
                                       double instrumental_gap_seconds,
                                       const volatile int* cancel_flag,
                                       bgm_vox_lyrics_progress_fn progress, void* progress_user,
                                       bgm_vox_lyrics** out);

void bgm_vox_lyrics_free(bgm_vox_lyrics* lyrics);

int bgm_vox_lyrics_status(const bgm_vox_lyrics* lyrics);
const char* bgm_vox_lyrics_detail(const bgm_vox_lyrics* lyrics);
int bgm_vox_lyrics_source(const bgm_vox_lyrics* lyrics);
const char* bgm_vox_lyrics_provider_tag(const bgm_vox_lyrics* lyrics);
int bgm_vox_lyrics_host_may_cache(const bgm_vox_lyrics* lyrics);
/* 1 only when the text is the provider's text; 0 for ASR hypotheses. */
int bgm_vox_lyrics_is_original_text(const bgm_vox_lyrics* lyrics);

size_t bgm_vox_lyrics_line_count(const bgm_vox_lyrics* lyrics);
/* Out-of-range index: -1 / -1 / "" / -1 / source of the lyrics / 0 / 0 / 0. */
double bgm_vox_lyrics_line_start(const bgm_vox_lyrics* lyrics, size_t index);
double bgm_vox_lyrics_line_end(const bgm_vox_lyrics* lyrics, size_t index);
const char* bgm_vox_lyrics_line_text(const bgm_vox_lyrics* lyrics, size_t index);
float bgm_vox_lyrics_line_confidence(const bgm_vox_lyrics* lyrics, size_t index);
int bgm_vox_lyrics_line_source(const bgm_vox_lyrics* lyrics, size_t index);
int bgm_vox_lyrics_line_instrumental(const bgm_vox_lyrics* lyrics, size_t index);
int bgm_vox_lyrics_line_stanza_break(const bgm_vox_lyrics* lyrics, size_t index);
int bgm_vox_lyrics_line_interpolated(const bgm_vox_lyrics* lyrics, size_t index);

/* LRC text of the timed lines (with provenance metadata for non-provider
 * sources). Owned by the handle. */
const char* bgm_vox_lyrics_to_lrc(bgm_vox_lyrics* lyrics);

/* Static strings, never NULL. */
const char* bgm_vox_lyrics_status_name(int status);
const char* bgm_vox_lyrics_source_name(int source);

#ifdef __cplusplus
}
#endif

#endif /* BGMEDIA_VOX_LYRICS_CAPI_H */
