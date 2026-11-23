#include "bgmedia/vox/vox_lyrics_capi.h"

#include "bgmedia/vox/VoxLyrics.hpp"

#include <new>
#include <string>

struct bgm_vox_lyrics {
    bgmedia::vox::VoxLyrics lyrics;
    std::string lrc;
};

using namespace bgmedia::vox;

namespace {

int finish(bgm_vox_lyrics* h, bgm_vox_lyrics** out) {
    const int status = static_cast<int>(h->lyrics.status);
    if (out != nullptr) {
        *out = h;
    } else {
        delete h;
    }
    return status;
}

const VoxLyricLine* lineAt(const bgm_vox_lyrics* h, size_t i) {
    return h != nullptr && i < h->lyrics.lines.size() ? &h->lyrics.lines[i] : nullptr;
}

} // namespace

extern "C" {

int bgm_vox_lyrics_parse_lrc(const char* text, size_t len, bgm_vox_lyrics** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    bgm_vox_lyrics* h = nullptr;
    try {
        h = new bgm_vox_lyrics();
        if (text == nullptr) {
            h->lyrics.status = VoxLyricsStatus::InvalidInput;
            h->lyrics.detail = "null text";
        } else {
            h->lyrics = parseLrc(std::string_view(text, len));
        }
        return finish(h, out);
    } catch (...) {
        delete h;
        return static_cast<int>(VoxLyricsStatus::InternalError);
    }
}

int bgm_vox_lyrics_parse_plain(const char* text, size_t len, bgm_vox_lyrics** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    bgm_vox_lyrics* h = nullptr;
    try {
        h = new bgm_vox_lyrics();
        if (text == nullptr) {
            h->lyrics.status = VoxLyricsStatus::InvalidInput;
            h->lyrics.detail = "null text";
        } else {
            h->lyrics = parsePlainLyrics(std::string_view(text, len));
        }
        return finish(h, out);
    } catch (...) {
        delete h;
        return static_cast<int>(VoxLyricsStatus::InternalError);
    }
}

void bgm_vox_lyrics_set_provenance(bgm_vox_lyrics* lyrics, const char* provider_tag,
                                   int host_may_cache) {
    if (lyrics == nullptr) {
        return;
    }
    try {
        lyrics->lyrics.providerTag = provider_tag != nullptr ? provider_tag : "";
        lyrics->lyrics.hostMayCache = host_may_cache != 0;
    } catch (...) {
    }
}

int bgm_vox_lyrics_align_to_vocals_s16(const bgm_vox_lyrics* unsynced, const int16_t* pcm,
                                       size_t frames, uint32_t sample_rate, uint32_t channels,
                                       double instrumental_gap_seconds,
                                       const volatile int* cancel_flag,
                                       bgm_vox_lyrics_progress_fn progress, void* progress_user,
                                       bgm_vox_lyrics** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    bgm_vox_lyrics* h = nullptr;
    try {
        h = new bgm_vox_lyrics();
        if (unsynced == nullptr) {
            h->lyrics.status = VoxLyricsStatus::InvalidInput;
            h->lyrics.detail = "null lyrics";
            return finish(h, out);
        }
        VoxCancelToken token(cancel_flag);
        VoxProgressFn fn;
        if (progress != nullptr) {
            fn = [progress, progress_user](float f) { progress(f, progress_user); };
        }
        const VoxPcmFormat fmt{sample_rate, channels};
        const VoxVocalRegions regions = detectVocalRegions(pcm, frames, fmt, {}, &token, fn);
        if (regions.status != VoxLyricsStatus::Ok) {
            h->lyrics = unsynced->lyrics;
            h->lyrics.status = regions.status;
            h->lyrics.detail = regions.detail;
            return finish(h, out);
        }
        h->lyrics = alignLyricsToVocalRegions(unsynced->lyrics, regions.regions);
        if (h->lyrics.status == VoxLyricsStatus::Ok && instrumental_gap_seconds > 0.0) {
            markInstrumentalGaps(h->lyrics, regions.regions, regions.audioSeconds,
                                 instrumental_gap_seconds);
        }
        return finish(h, out);
    } catch (...) {
        delete h;
        return static_cast<int>(VoxLyricsStatus::InternalError);
    }
}

void bgm_vox_lyrics_free(bgm_vox_lyrics* lyrics) {
    try {
        delete lyrics;
    } catch (...) {
    }
}

int bgm_vox_lyrics_status(const bgm_vox_lyrics* l) {
    return l != nullptr ? static_cast<int>(l->lyrics.status)
                        : static_cast<int>(VoxLyricsStatus::InvalidInput);
}
const char* bgm_vox_lyrics_detail(const bgm_vox_lyrics* l) {
    return l != nullptr ? l->lyrics.detail.c_str() : "";
}
int bgm_vox_lyrics_source(const bgm_vox_lyrics* l) {
    return l != nullptr ? static_cast<int>(l->lyrics.source)
                        : static_cast<int>(VoxLyricsSource::AsrHypothesis);
}
const char* bgm_vox_lyrics_provider_tag(const bgm_vox_lyrics* l) {
    return l != nullptr ? l->lyrics.providerTag.c_str() : "";
}
int bgm_vox_lyrics_host_may_cache(const bgm_vox_lyrics* l) {
    return l != nullptr && l->lyrics.hostMayCache ? 1 : 0;
}
int bgm_vox_lyrics_is_original_text(const bgm_vox_lyrics* l) {
    return l != nullptr && l->lyrics.isOriginalText ? 1 : 0;
}

size_t bgm_vox_lyrics_line_count(const bgm_vox_lyrics* l) {
    return l != nullptr ? l->lyrics.lines.size() : 0;
}
double bgm_vox_lyrics_line_start(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr ? x->startSec : -1.0;
}
double bgm_vox_lyrics_line_end(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr ? x->endSec : -1.0;
}
const char* bgm_vox_lyrics_line_text(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr ? x->text.c_str() : "";
}
float bgm_vox_lyrics_line_confidence(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr ? x->confidence : -1.0f;
}
int bgm_vox_lyrics_line_source(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr ? static_cast<int>(x->source) : bgm_vox_lyrics_source(l);
}
int bgm_vox_lyrics_line_instrumental(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr && x->instrumental ? 1 : 0;
}
int bgm_vox_lyrics_line_stanza_break(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr && x->stanzaBreak ? 1 : 0;
}
int bgm_vox_lyrics_line_interpolated(const bgm_vox_lyrics* l, size_t i) {
    const VoxLyricLine* x = lineAt(l, i);
    return x != nullptr && x->interpolated ? 1 : 0;
}

const char* bgm_vox_lyrics_to_lrc(bgm_vox_lyrics* l) {
    if (l == nullptr) {
        return "";
    }
    try {
        l->lrc = toLrc(l->lyrics, false);
        return l->lrc.c_str();
    } catch (...) {
        return "";
    }
}

const char* bgm_vox_lyrics_status_name(int status) {
    if (status < 0 || status > static_cast<int>(VoxLyricsStatus::InternalError)) {
        return "Unknown";
    }
    return voxLyricsStatusName(static_cast<VoxLyricsStatus>(status));
}
const char* bgm_vox_lyrics_source_name(int source) {
    if (source < 0 || source > static_cast<int>(VoxLyricsSource::AsrHypothesis)) {
        return "Unknown";
    }
    return voxLyricsSourceName(static_cast<VoxLyricsSource>(source));
}

} // extern "C"
