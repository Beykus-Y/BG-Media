#ifndef BGMEDIA_VOX_CAPI_H
#define BGMEDIA_VOX_CAPI_H

/* C ABI for BG Vox Enhance (JNI / iOS hosts). Never throws. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bgm_vox_mode {
    BGM_VOX_MODE_OFF = 0,
    BGM_VOX_MODE_NATURAL = 1,
    BGM_VOX_MODE_STRONG = 2
} bgm_vox_mode;

/* Status codes mirror bgmedia::vox::VoxStatus. */
typedef enum bgm_vox_status {
    BGM_VOX_PROCESSED = 0,
    BGM_VOX_BYPASSED = 1,
    BGM_VOX_FALLBACK_UNSUPPORTED_FORMAT = 2,
    BGM_VOX_FALLBACK_TOO_LONG = 3,
    BGM_VOX_FALLBACK_OUT_OF_MEMORY = 4,
    BGM_VOX_FALLBACK_INTERNAL_ERROR = 5,
    BGM_VOX_FALLBACK_QUALITY_GUARD = 6,
    BGM_VOX_CANCELLED = 7,
    BGM_VOX_FALLBACK_INVALID_INPUT = 8
} bgm_vox_status;

/* Processes interleaved 16-bit PCM. `out` must hold frames*channels samples
 * and may not overlap `in`. On every status other than PROCESSED, `out`
 * receives a bit-exact copy of `in` (when both pointers are non-null).
 * `cancel_flag` may be NULL; the call stops when *cancel_flag becomes non-zero.
 * Returns a bgm_vox_status value. */
int bgm_vox_enhance_s16(const int16_t* in, size_t frames, uint32_t sample_rate,
                        uint32_t channels, int mode, int16_t* out,
                        const volatile int* cancel_flag);

/* Static string, never NULL. */
const char* bgm_vox_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* BGMEDIA_VOX_CAPI_H */
