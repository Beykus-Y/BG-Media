// Isolated miniaudio implementation translation unit. Keeping the
// implementation here prevents every adapter source from recompiling the
// single-header library and keeps warnings out of BG Media's own code.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
