#ifndef BGMEDIA_VIDEO_RANGE_SOURCE_H
#define BGMEDIA_VIDEO_RANGE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version 1. A successful read fills the entire requested span. Length and
 * representation remain fixed for this handle. The caller clips reads at EOF.
 * Cancel and its context are valid only while read_at is running.
 * No callback may unwind across the language boundary. */
enum {
    BG_VIDEO_RANGE_OK = 0,
    BG_VIDEO_RANGE_CANCELLED = 1,
    BG_VIDEO_RANGE_TIMEOUT = 2,
    BG_VIDEO_RANGE_EXPIRED = 3,
    BG_VIDEO_RANGE_IO = 4
};

typedef struct bg_video_range_v1 {
    void* context;
    uint64_t length;
    int (*read_at)(void* context, uint64_t offset, uint8_t* output, size_t count,
                   uint32_t timeout_ms, int (*cancelled)(void*), void* cancel_context);
    void (*release)(void* context);
} bg_video_range_v1;

#ifdef __cplusplus
}
#endif

#endif
