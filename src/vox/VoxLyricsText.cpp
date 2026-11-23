#include "bgmedia/vox/VoxLyrics.hpp"

#include "VoxLyricsText.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace bgmedia::vox {

const char* voxLyricsStatusName(VoxLyricsStatus status) noexcept {
    switch (status) {
    case VoxLyricsStatus::Ok: return "Ok";
    case VoxLyricsStatus::Cancelled: return "Cancelled";
    case VoxLyricsStatus::InvalidInput: return "InvalidInput";
    case VoxLyricsStatus::UnsupportedFormat: return "UnsupportedFormat";
    case VoxLyricsStatus::TooLong: return "TooLong";
    case VoxLyricsStatus::ParseError: return "ParseError";
    case VoxLyricsStatus::NoLyrics: return "NoLyrics";
    case VoxLyricsStatus::NoVocals: return "NoVocals";
    case VoxLyricsStatus::AlignmentFailed: return "AlignmentFailed";
    case VoxLyricsStatus::NoBackend: return "NoBackend";
    case VoxLyricsStatus::BackendError: return "BackendError";
    case VoxLyricsStatus::OutOfMemory: return "OutOfMemory";
    case VoxLyricsStatus::InternalError: return "InternalError";
    }
    return "Unknown";
}

const char* voxLyricsSourceName(VoxLyricsSource source) noexcept {
    switch (source) {
    case VoxLyricsSource::ProviderSynced: return "ProviderSynced";
    case VoxLyricsSource::ProviderUnsynced: return "ProviderUnsynced";
    case VoxLyricsSource::AlignedToAudio: return "AlignedToAudio";
    case VoxLyricsSource::AsrHypothesis: return "AsrHypothesis";
    }
    return "Unknown";
}

bool VoxLyrics::isSynced() const noexcept {
    bool any = false;
    for (const VoxLyricLine& l : lines) {
        if (l.stanzaBreak && !l.hasTimes()) {
            continue;
        }
        if (!l.hasTimes()) {
            return false;
        }
        any = true;
    }
    return any;
}

double VoxVocalRegions::vocalFraction() const noexcept {
    if (audioSeconds <= 0.0) {
        return 0.0;
    }
    double sum = 0.0;
    for (const VoxVocalRegion& r : regions) {
        sum += std::max(0.0, r.endSec - r.startSec);
    }
    return std::min(1.0, sum / audioSeconds);
}

namespace detail {

bool decodeUtf8(std::string_view s, std::size_t& i, char32_t& cp) noexcept {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    cp = 0xFFFD;
    if (c < 0x80) {
        cp = c;
        ++i;
        return true;
    }
    std::size_t len = 0;
    char32_t v = 0;
    char32_t minV = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2; v = c & 0x1Fu; minV = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3; v = c & 0x0Fu; minV = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4; v = c & 0x07u; minV = 0x10000;
    } else {
        ++i;
        return false;
    }
    if (i + len > s.size()) {
        ++i;
        return false;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char cc = b(i + k);
        if ((cc & 0xC0u) != 0x80u) {
            ++i;
            return false;
        }
        v = (v << 6) | (cc & 0x3Fu);
    }
    if (v < minV || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        ++i;
        return false;
    }
    cp = v;
    i += len;
    return true;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string sanitizeUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        if (decodeUtf8(s, i, cp)) {
            out.append(s.substr(start, i - start));
        } else {
            appendUtf8(out, 0xFFFD);
        }
    }
    return out;
}

std::u32string decodeLossy(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        decodeUtf8(s, i, cp);
        out.push_back(cp);
    }
    return out;
}

bool isSpaceCp(char32_t cp) noexcept {
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200B) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

bool isWordCp(char32_t cp) noexcept {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    }
    if (cp < 0xC0) {
        return cp == 0xAA || cp == 0xB5 || cp == 0xBA;  // feminine/masculine ordinal, micro
    }
    if (cp == 0xD7 || cp == 0xF7) {
        return false;
    }
    if (cp >= 0x2000 && cp <= 0x2BFF) {  // punctuation, symbols, arrows, dingbats
        return false;
    }
    if (cp >= 0x3000 && cp <= 0x303F) {
        return false;
    }
    if (cp >= 0xFE00 && cp <= 0xFE0F) {
        return false;
    }
    if (cp >= 0xFF00 && cp <= 0xFF0F) {
        return false;
    }
    if (cp >= 0xFFF0 && cp <= 0xFFFF) {  // specials incl. U+FFFD
        return false;
    }
    if (cp >= 0x1F000 && cp <= 0x1FAFF) {  // emoji
        return false;
    }
    return true;
}

char32_t foldCp(char32_t cp) noexcept {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 0x20;
    }
    if (cp < 0x80) {
        return cp;
    }
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {  // Latin-1 supplement
        return cp + 0x20;
    }
    if (cp >= 0x100 && cp <= 0x17F) {  // Latin Extended-A: pairs, even = upper
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
            return (cp & 1u) ? cp + 1 : cp;  // odd = upper in these ranges
        }
        if (cp == 0x130 || cp == 0x131 || cp == 0x138 || cp == 0x149 || cp == 0x178 ||
            cp == 0x17F) {
            return cp;
        }
        return (cp & 1u) ? cp : cp + 1;
    }
    if (cp == 0x401) {  // Ё
        return 0x435;
    }
    if (cp == 0x451) {  // ё
        return 0x435;
    }
    if (cp >= 0x410 && cp <= 0x42F) {
        return cp + 0x20;
    }
    if (cp >= 0x400 && cp <= 0x40F) {
        return cp + 0x50;
    }
    if (cp >= 0x460 && cp <= 0x481) {  // historic Cyrillic pairs
        return (cp & 1u) ? cp : cp + 1;
    }
    if (cp >= 0x48A && cp <= 0x4BF) {
        return (cp & 1u) ? cp : cp + 1;
    }
    if (cp >= 0x4D0 && cp <= 0x52F) {
        return (cp & 1u) ? cp : cp + 1;
    }
    return cp;
}

bool isVowelCp(char32_t f) noexcept {
    switch (f) {
    case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
    case 0x430: case 0x435: case 0x438: case 0x43E: case 0x443:  // а е и о у
    case 0x44B: case 0x44D: case 0x44E: case 0x44F:              // ы э ю я
    case 0x454: case 0x456: case 0x457:                          // є і ї
        return true;
    default:
        return false;
    }
}

std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        const bool ok = decodeUtf8(text, i, cp);
        if (ok && isSpaceCp(cp)) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else if (ok) {
            cur.append(text.substr(start, i - start));
        } else {
            appendUtf8(cur, 0xFFFD);
        }
    }
    if (!cur.empty()) {
        out.push_back(std::move(cur));
    }
    return out;
}

std::u32string normalizeWord(std::string_view word) {
    std::u32string out;
    std::size_t i = 0;
    while (i < word.size()) {
        char32_t cp = 0;
        if (!decodeUtf8(word, i, cp)) {
            continue;
        }
        if (isWordCp(cp)) {
            out.push_back(foldCp(cp));
        }
    }
    return out;
}

float tokenSimilarity(const std::u32string& a, const std::u32string& b) noexcept {
    const std::size_t la = a.size();
    const std::size_t lb = b.size();
    if (la == 0 || lb == 0) {
        return 0.0f;
    }
    if (a == b) {
        return 1.0f;
    }
    const std::size_t mx = std::max(la, lb);
    const std::size_t mn = std::min(la, lb);
    if (mn <= 3) {
        return 0.0f;  // short words must match exactly
    }
    const float minSim = mx <= 4 ? 0.74f : 0.6f;
    const std::size_t maxEdits =
        static_cast<std::size_t>(std::floor((1.0f - minSim) * static_cast<float>(mx) + 1e-4f));
    if (mx - mn > maxEdits) {
        return 0.0f;
    }
    // Levenshtein with two rows (tokens are short).
    std::vector<std::size_t> prev(lb + 1);
    std::vector<std::size_t> cur(lb + 1);
    for (std::size_t j = 0; j <= lb; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= la; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= lb; ++j) {
            const std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    const float sim = 1.0f - static_cast<float>(prev[lb]) / static_cast<float>(mx);
    return sim >= minSim ? sim : 0.0f;
}

std::size_t syllableWeight(std::string_view text) {
    std::size_t vowels = 0;
    std::size_t letters = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t cp = 0;
        if (!decodeUtf8(text, i, cp) || !isWordCp(cp)) {
            continue;
        }
        ++letters;
        if (isVowelCp(foldCp(cp))) {
            ++vowels;
        }
    }
    if (letters == 0) {
        return 0;
    }
    return vowels > 0 ? vowels : std::max<std::size_t>(1, letters / 3);
}

} // namespace detail

namespace {

using namespace detail;

constexpr std::size_t kMaxTextBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxLines = 200000;

// "mm:ss", "mm:ss.f", ".ff", ".fff" (1..3 digits of fraction).
bool parseTime(std::string_view t, double& out) noexcept {
    std::size_t i = 0;
    auto digits = [&](std::size_t minN, std::size_t maxN, long& v, std::size_t& n) {
        v = 0;
        n = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9' && n < maxN) {
            v = v * 10 + (t[i] - '0');
            ++i;
            ++n;
        }
        return n >= minN;
    };
    long mm = 0, ss = 0, frac = 0;
    std::size_t n = 0;
    if (!digits(1, 4, mm, n)) {
        return false;
    }
    if (i >= t.size() || t[i] != ':') {
        return false;
    }
    ++i;
    if (!digits(1, 2, ss, n) || ss > 59) {
        return false;
    }
    double f = 0.0;
    if (i < t.size() && (t[i] == '.' || t[i] == ':')) {
        ++i;
        if (!digits(1, 3, frac, n)) {
            return false;
        }
        f = static_cast<double>(frac) / std::pow(10.0, static_cast<double>(n));
    }
    if (i != t.size()) {
        return false;
    }
    out = static_cast<double>(mm) * 60.0 + static_cast<double>(ss) + f;
    return true;
}

bool isMetaTag(std::string_view tag) noexcept {
    if (tag.empty() || !((tag[0] >= 'a' && tag[0] <= 'z') || (tag[0] >= 'A' && tag[0] <= 'Z'))) {
        return false;
    }
    std::size_t i = 0;
    while (i < tag.size() && tag[i] != ':') {
        const char c = tag[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
        ++i;
    }
    return i < tag.size();
}

bool parseOffsetTag(std::string_view tag, long& ms) noexcept {
    constexpr std::string_view key = "offset:";
    if (tag.size() < key.size()) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        if (c != key[i]) {
            return false;
        }
    }
    std::size_t i = key.size();
    while (i < tag.size() && tag[i] == ' ') {
        ++i;
    }
    bool neg = false;
    if (i < tag.size() && (tag[i] == '+' || tag[i] == '-')) {
        neg = tag[i] == '-';
        ++i;
    }
    long v = 0;
    std::size_t n = 0;
    while (i < tag.size() && tag[i] >= '0' && tag[i] <= '9' && n < 9) {
        v = v * 10 + (tag[i] - '0');
        ++i;
        ++n;
    }
    while (i < tag.size() && tag[i] == ' ') {
        ++i;
    }
    if (n == 0 || i != tag.size()) {
        return false;
    }
    ms = neg ? -v : v;
    return true;
}

std::string collapseSpaces(const std::string& s) {
    std::string out;
    bool pendingSpace = false;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        decodeUtf8(s, i, cp);
        if (isSpaceCp(cp)) {
            pendingSpace = !out.empty();
        } else {
            if (pendingSpace) {
                out.push_back(' ');
                pendingSpace = false;
            }
            out.append(s, start, i - start);
        }
    }
    return out;
}

bool isNoteOnly(const std::string& s) {
    bool any = false;
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        decodeUtf8(s, i, cp);
        if (cp == 0x266A || cp == 0x266B) {
            any = true;
        } else if (!isSpaceCp(cp)) {
            return false;
        }
    }
    return any;
}

struct RawLine {
    double t = 0.0;
    std::string text;
    std::vector<VoxWord> words;
    std::size_t order = 0;
};

std::string_view trimView(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// Splits the text part of an LRC line into enhanced-word chunks. Returns the
// plain text; fills `words` (times absolute, end filled from the next marker,
// -1 for the last) when markers exist.
std::string parseEnhanced(std::string_view rest, std::vector<VoxWord>& words) {
    std::string plain;
    struct Marker {
        double t;
        std::string chunk;
    };
    std::vector<Marker> markers;
    std::string lead;
    std::string cur;
    bool haveMarker = false;
    double curT = 0.0;
    auto flush = [&]() {
        if (haveMarker) {
            markers.push_back({curT, cur});
        } else {
            lead = cur;
        }
        cur.clear();
    };
    std::size_t i = 0;
    while (i < rest.size()) {
        if (rest[i] == '<') {
            const std::size_t close = rest.find('>', i + 1);
            double t = 0.0;
            if (close != std::string_view::npos && close - i <= 12 &&
                parseTime(rest.substr(i + 1, close - i - 1), t)) {
                flush();
                haveMarker = true;
                curT = t;
                i = close + 1;
                continue;
            }
        }
        cur.push_back(rest[i]);
        ++i;
    }
    flush();
    plain = lead;
    for (const Marker& m : markers) {
        plain += m.chunk;
    }
    plain = collapseSpaces(plain);
    for (std::size_t k = 0; k < markers.size(); ++k) {
        const std::string w = collapseSpaces(markers[k].chunk);
        if (w.empty()) {
            continue;
        }
        VoxWord word;
        word.text = w;
        word.startSec = markers[k].t;
        word.endSec = k + 1 < markers.size() ? markers[k + 1].t : -1.0;
        words.push_back(std::move(word));
    }
    return plain;
}

VoxLyrics parseLrcImpl(std::string_view input) {
    VoxLyrics out;
    out.source = VoxLyricsSource::ProviderSynced;
    out.isOriginalText = true;
    if (input.size() > kMaxTextBytes) {
        out.status = VoxLyricsStatus::TooLong;
        out.detail = "lyrics text too large";
        return out;
    }
    if (input.size() >= 3 && input.substr(0, 3) == "\xEF\xBB\xBF") {
        input.remove_prefix(3);
    }
    const std::string clean = sanitizeUtf8(input);
    std::vector<RawLine> raw;
    long offsetMs = 0;
    std::size_t ignored = 0;
    std::size_t pos = 0;
    std::size_t order = 0;
    const std::string_view text = clean;
    while (pos <= text.size()) {
        std::size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        const std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (eol < text.size() && text[eol] == '\r' && pos < text.size() && text[pos] == '\n') {
            ++pos;
        }
        std::vector<double> stamps;
        bool meta = false;
        std::size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        while (p < line.size() && line[p] == '[') {
            const std::size_t close = line.find(']', p + 1);
            if (close == std::string_view::npos) {
                break;
            }
            const std::string_view tag = line.substr(p + 1, close - p - 1);
            double t = 0.0;
            long ms = 0;
            if (parseTime(tag, t)) {
                stamps.push_back(t);
            } else if (parseOffsetTag(tag, ms)) {
                offsetMs = ms;
                meta = true;
            } else if (isMetaTag(tag)) {
                meta = true;
            } else {
                break;
            }
            p = close + 1;
        }
        if (stamps.empty()) {
            if (!meta && !trimView(line).empty()) {
                ++ignored;
            }
            continue;
        }
        if (raw.size() + stamps.size() > kMaxLines) {
            out.status = VoxLyricsStatus::TooLong;
            out.detail = "too many lyric lines";
            return out;
        }
        std::vector<VoxWord> words;
        const std::string plain = parseEnhanced(line.substr(p), words);
        for (std::size_t s = 0; s < stamps.size(); ++s) {
            RawLine r;
            r.t = stamps[s];
            r.text = plain;
            r.order = order++;
            const double shift = stamps[s] - stamps[0];
            for (VoxWord w : words) {
                w.startSec += shift;
                if (w.endSec >= 0.0) {
                    w.endSec += shift;
                }
                r.words.push_back(std::move(w));
            }
            raw.push_back(std::move(r));
        }
    }
    if (raw.empty()) {
        out.status = VoxLyricsStatus::ParseError;
        out.detail = "no time-stamped lines (not LRC)";
        return out;
    }
    const double off = static_cast<double>(offsetMs) / 1000.0;
    for (RawLine& r : raw) {
        r.t = std::max(0.0, r.t - off);
        for (VoxWord& w : r.words) {
            w.startSec = std::max(0.0, w.startSec - off);
            if (w.endSec >= 0.0) {
                w.endSec = std::max(w.startSec, w.endSec - off);
            }
        }
    }
    std::stable_sort(raw.begin(), raw.end(), [](const RawLine& a, const RawLine& b) {
        return a.t < b.t;
    });
    out.lines.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        VoxLyricLine l;
        l.startSec = raw[i].t;
        std::size_t j = i + 1;
        while (j < raw.size() && raw[j].t <= raw[i].t) {
            ++j;
        }
        l.endSec = j < raw.size() ? raw[j].t : -1.0;
        l.text = std::move(raw[i].text);
        l.confidence = 1.0f;
        l.source = VoxLyricsSource::ProviderSynced;
        if (isNoteOnly(l.text)) {
            l.instrumental = true;
            l.text.clear();
        } else if (l.text.empty()) {
            l.stanzaBreak = true;
        }
        l.words = std::move(raw[i].words);
        for (std::size_t w = 0; w < l.words.size(); ++w) {
            VoxWord& word = l.words[w];
            if (word.endSec < 0.0) {
                word.endSec = l.endSec >= word.startSec ? l.endSec : word.startSec;
            }
            if (word.confidence < 0.0f) {
                word.confidence = 1.0f;
            }
        }
        out.lines.push_back(std::move(l));
    }
    out.status = VoxLyricsStatus::Ok;
    out.detail = std::to_string(out.lines.size()) + " lines";
    if (ignored > 0) {
        out.detail += ", " + std::to_string(ignored) + " non-stamped lines ignored";
    }
    return out;
}

VoxLyrics parsePlainImpl(std::string_view input) {
    VoxLyrics out;
    out.source = VoxLyricsSource::ProviderUnsynced;
    out.isOriginalText = true;
    if (input.size() > kMaxTextBytes) {
        out.status = VoxLyricsStatus::TooLong;
        out.detail = "lyrics text too large";
        return out;
    }
    if (input.size() >= 3 && input.substr(0, 3) == "\xEF\xBB\xBF") {
        input.remove_prefix(3);
    }
    const std::string clean = sanitizeUtf8(input);
    const std::string_view text = clean;
    std::size_t pos = 0;
    bool pendingBreak = false;
    while (pos <= text.size()) {
        std::size_t eol = text.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (eol < text.size() && text[eol] == '\r' && pos < text.size() && text[pos] == '\n') {
            ++pos;
        }
        const std::string content = collapseSpaces(std::string(line));
        if (content.empty()) {
            pendingBreak = !out.lines.empty();
            continue;
        }
        if (out.lines.size() >= kMaxLines) {
            out.status = VoxLyricsStatus::TooLong;
            out.detail = "too many lyric lines";
            out.lines.clear();
            return out;
        }
        if (pendingBreak) {
            VoxLyricLine b;
            b.stanzaBreak = true;
            b.source = VoxLyricsSource::ProviderUnsynced;
            out.lines.push_back(std::move(b));
            pendingBreak = false;
        }
        VoxLyricLine l;
        l.text = content;
        l.source = VoxLyricsSource::ProviderUnsynced;
        out.lines.push_back(std::move(l));
    }
    if (out.lines.empty()) {
        out.status = VoxLyricsStatus::ParseError;
        out.detail = "no text";
        return out;
    }
    out.status = VoxLyricsStatus::Ok;
    out.detail = std::to_string(out.lines.size()) + " lines";
    return out;
}

void appendStamp(std::string& s, double sec, char open, char close) {
    const long long cs = static_cast<long long>(std::llround(std::max(0.0, sec) * 100.0));
    char buf[48];
    std::snprintf(buf, sizeof buf, "%c%02lld:%02lld.%02lld%c", open, cs / 6000, (cs / 100) % 60,
                  cs % 100, close);
    s += buf;
}

} // namespace

VoxLyrics parseLrc(std::string_view text) noexcept {
    try {
        return parseLrcImpl(text);
    } catch (const std::bad_alloc&) {
        VoxLyrics r;
        r.status = VoxLyricsStatus::OutOfMemory;
        return r;
    } catch (...) {
        VoxLyrics r;
        r.status = VoxLyricsStatus::InternalError;
        return r;
    }
}

VoxLyrics parsePlainLyrics(std::string_view text) noexcept {
    try {
        return parsePlainImpl(text);
    } catch (const std::bad_alloc&) {
        VoxLyrics r;
        r.status = VoxLyricsStatus::OutOfMemory;
        return r;
    } catch (...) {
        VoxLyrics r;
        r.status = VoxLyricsStatus::InternalError;
        return r;
    }
}

std::string toLrc(const VoxLyrics& lyrics, bool includeWords) {
    std::string out;
    try {
        if (lyrics.source != VoxLyricsSource::ProviderSynced) {
            out += "[re:bgmedia-vox-lyrics]\n";
            out += std::string("[x-bg-source:") + voxLyricsSourceName(lyrics.source) + "]\n";
            out += std::string("[x-bg-original:") + (lyrics.isOriginalText ? "1" : "0") + "]\n";
        }
        for (const VoxLyricLine& l : lyrics.lines) {
            if (!l.hasTimes()) {
                continue;
            }
            appendStamp(out, l.startSec, '[', ']');
            if (l.instrumental) {
                out += "\xE2\x99\xAA";  // musical note
            } else if (includeWords && !l.words.empty()) {
                for (std::size_t w = 0; w < l.words.size(); ++w) {
                    appendStamp(out, l.words[w].startSec, '<', '>');
                    out += l.words[w].text;
                    if (w + 1 < l.words.size()) {
                        out += ' ';
                    }
                }
            } else {
                out += l.text;
            }
            out += '\n';
        }
    } catch (...) {
    }
    return out;
}

} // namespace bgmedia::vox
