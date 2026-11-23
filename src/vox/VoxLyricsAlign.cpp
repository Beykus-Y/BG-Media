// Alignment of host-supplied lyrics text to audio evidence (BG Vox Lyrics), the
// ASR-hypothesis lines and the buildLyrics() pipeline.

#include "bgmedia/vox/VoxLyrics.hpp"

#include "VoxLyricsText.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>

namespace bgmedia::vox {

namespace {

using namespace detail;

constexpr float kVocalFallbackMaxConfidence = 0.3f;
constexpr double kClusterGapSeconds = 6.0;

bool isBreakLine(const VoxLyricLine& l) {
    return l.stanzaBreak || l.instrumental || l.text.empty();
}

VoxLyricsStatus mapAsrStatus(VoxTranscribeStatus s) {
    switch (s) {
    case VoxTranscribeStatus::Ok: return VoxLyricsStatus::Ok;
    case VoxTranscribeStatus::Cancelled: return VoxLyricsStatus::Cancelled;
    case VoxTranscribeStatus::InvalidInput: return VoxLyricsStatus::InvalidInput;
    case VoxTranscribeStatus::UnsupportedFormat: return VoxLyricsStatus::UnsupportedFormat;
    case VoxTranscribeStatus::TooLong: return VoxLyricsStatus::TooLong;
    case VoxTranscribeStatus::NoBackend: return VoxLyricsStatus::NoBackend;
    case VoxTranscribeStatus::ModelUnavailable: return VoxLyricsStatus::NoBackend;
    case VoxTranscribeStatus::BackendError: return VoxLyricsStatus::BackendError;
    case VoxTranscribeStatus::OutOfMemory: return VoxLyricsStatus::OutOfMemory;
    case VoxTranscribeStatus::InternalError: return VoxLyricsStatus::InternalError;
    }
    return VoxLyricsStatus::InternalError;
}

VoxLyrics failed(VoxLyricsStatus status, std::string detail) {
    VoxLyrics r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

template <typename F>
VoxLyrics guarded(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return failed(VoxLyricsStatus::OutOfMemory, "out of memory");
    } catch (...) {
        return failed(VoxLyricsStatus::InternalError, "unexpected exception");
    }
}

struct AsrTok {
    double start = 0.0;
    double end = 0.0;
    float conf = -1.0f;
    std::u32string norm;
};

void addTokens(std::vector<AsrTok>& out, const std::string& text, double a, double b, float conf) {
    std::vector<std::string> parts = splitWords(text);
    std::vector<std::u32string> norms;
    std::vector<std::size_t> lens;
    std::size_t total = 0;
    for (const std::string& p : parts) {
        std::u32string n = normalizeWord(p);
        if (n.empty()) {
            continue;
        }
        total += n.size();
        lens.push_back(n.size());
        norms.push_back(std::move(n));
    }
    if (norms.empty()) {
        return;
    }
    if (b < a) {
        b = a;
    }
    double t = a;
    for (std::size_t i = 0; i < norms.size(); ++i) {
        const double share = (b - a) * static_cast<double>(lens[i]) / static_cast<double>(total);
        AsrTok tok;
        tok.start = t;
        tok.end = t + share;
        tok.conf = conf;
        tok.norm = std::move(norms[i]);
        t += share;
        out.push_back(std::move(tok));
    }
}

std::vector<AsrTok> asrTokens(const VoxTranscript& asr) {
    std::vector<AsrTok> out;
    for (const VoxSegment& s : asr.segments) {
        if (!s.words.empty()) {
            for (const VoxWord& w : s.words) {
                addTokens(out, w.text, w.startSec, w.endSec, w.confidence);
            }
        } else {
            addTokens(out, s.text, s.startSec, s.endSec, s.confidence);
        }
    }
    return out;
}

struct LyricTok {
    std::size_t line = 0;
    std::size_t surface = 0;
    std::u32string norm;
};

enum : std::uint8_t { kDiag = 1, kUp = 2, kLeft = 3 };

struct Match {
    long asr = -1;
    float sim = 0.0f;
};

} // namespace

VoxLyrics alignLyricsToTranscript(const VoxLyrics& unsynced, const VoxTranscript& asr,
                                  const VoxAlignOptions& options,
                                  const VoxCancelToken* cancel) noexcept {
    return guarded([&]() -> VoxLyrics {
        const std::size_t nLines = unsynced.lines.size();
        if (nLines == 0) {
            return failed(VoxLyricsStatus::NoLyrics, "no lyric lines");
        }
        // Tokens of the lyrics.
        std::vector<std::vector<std::string>> surfaces(nLines);
        std::vector<std::vector<long>> surfaceTok(nLines);  // token index or -1
        std::vector<LyricTok> lt;
        for (std::size_t li = 0; li < nLines; ++li) {
            if (isBreakLine(unsynced.lines[li])) {
                continue;
            }
            surfaces[li] = splitWords(unsynced.lines[li].text);
            for (std::size_t si = 0; si < surfaces[li].size(); ++si) {
                std::u32string n = normalizeWord(surfaces[li][si]);
                if (n.empty()) {
                    surfaceTok[li].push_back(-1);
                    continue;
                }
                surfaceTok[li].push_back(static_cast<long>(lt.size()));
                lt.push_back({li, si, std::move(n)});
            }
        }
        const std::vector<AsrTok> at = asrTokens(asr);
        if (lt.empty()) {
            return failed(VoxLyricsStatus::NoLyrics, "lyrics have no words");
        }
        if (at.empty()) {
            return failed(VoxLyricsStatus::AlignmentFailed, "transcript has no words");
        }
        const std::size_t N = lt.size();
        const std::size_t M = at.size();
        if (M != 0 && N > options.maxMatrixCells / M) {
            return failed(VoxLyricsStatus::TooLong, "alignment matrix too large");
        }

        // Semi-global alignment: leading/trailing ASR tokens are free (intro talk,
        // applause), every other skip costs `gap`.
        constexpr float gap = 0.6f;
        std::vector<std::uint8_t> trace(N * M, 0);
        std::vector<float> prev(M + 1, 0.0f);
        std::vector<float> cur(M + 1, 0.0f);
        for (std::size_t i = 1; i <= N; ++i) {
            if (cancel != nullptr && cancel->isCancelled()) {
                return failed(VoxLyricsStatus::Cancelled, "cancelled");
            }
            cur[0] = prev[0] - gap;
            for (std::size_t j = 1; j <= M; ++j) {
                const float sim = tokenSimilarity(lt[i - 1].norm, at[j - 1].norm);
                float best = prev[j] - gap;
                std::uint8_t dir = kUp;
                const float left = cur[j - 1] - gap;
                if (left > best) {
                    best = left;
                    dir = kLeft;
                }
                if (sim > 0.0f) {
                    const float diag = prev[j - 1] + 1.0f + sim;
                    if (diag >= best) {
                        best = diag;
                        dir = kDiag;
                    }
                }
                cur[j] = best;
                trace[(i - 1) * M + (j - 1)] = dir;
            }
            std::swap(prev, cur);
        }
        std::size_t endJ = 0;
        float endBest = -std::numeric_limits<float>::infinity();
        for (std::size_t j = 0; j <= M; ++j) {
            if (prev[j] > endBest) {
                endBest = prev[j];
                endJ = j;
            }
        }
        std::vector<Match> match(N);
        {
            std::size_t i = N;
            std::size_t j = endJ;
            while (i > 0 && j > 0) {
                const std::uint8_t d = trace[(i - 1) * M + (j - 1)];
                if (d == kDiag) {
                    match[i - 1].asr = static_cast<long>(j - 1);
                    match[i - 1].sim = tokenSimilarity(lt[i - 1].norm, at[j - 1].norm);
                    --i;
                    --j;
                } else if (d == kUp) {
                    --i;
                } else {
                    --j;
                }
            }
        }

        // Per line: keep the biggest temporal cluster of matches (drops a stray
        // fuzzy match far away from the rest of the line).
        std::vector<std::vector<std::size_t>> lineTokens(nLines);
        for (std::size_t t = 0; t < N; ++t) {
            lineTokens[lt[t].line].push_back(t);
        }
        std::size_t totalMatched = 0;
        for (std::size_t li = 0; li < nLines; ++li) {
            std::vector<std::size_t> ms;
            for (std::size_t t : lineTokens[li]) {
                if (match[t].asr >= 0) {
                    ms.push_back(t);
                }
            }
            if (ms.size() < 2) {
                continue;
            }
            std::size_t bestBegin = 0, bestLen = 0, begin = 0;
            for (std::size_t k = 1; k <= ms.size(); ++k) {
                const bool split =
                    k == ms.size() ||
                    at[static_cast<std::size_t>(match[ms[k]].asr)].start -
                            at[static_cast<std::size_t>(match[ms[k - 1]].asr)].end >
                        kClusterGapSeconds;
                if (split) {
                    if (k - begin > bestLen) {
                        bestLen = k - begin;
                        bestBegin = begin;
                    }
                    begin = k;
                }
            }
            for (std::size_t k = 0; k < ms.size(); ++k) {
                if (k < bestBegin || k >= bestBegin + bestLen) {
                    match[ms[k]].asr = -1;
                }
            }
        }

        VoxLyrics out;
        out.lines = unsynced.lines;
        out.providerTag = unsynced.providerTag;
        out.hostMayCache = unsynced.hostMayCache;
        out.isOriginalText = unsynced.isOriginalText;
        out.source = VoxLyricsSource::AlignedToAudio;

        std::vector<double> frac(nLines, 0.0);
        std::vector<char> anchored(nLines, 0);
        std::vector<double> perTok;  // seconds per token of anchored lines
        std::size_t anchoredCount = 0;
        for (std::size_t li = 0; li < nLines; ++li) {
            VoxLyricLine& line = out.lines[li];
            line.words.clear();
            line.source = VoxLyricsSource::AlignedToAudio;
            line.instrumental = unsynced.lines[li].instrumental;
            line.startSec = line.endSec = -1.0;
            line.confidence = -1.0f;
            line.interpolated = false;
            const std::vector<std::size_t>& toks = lineTokens[li];
            if (toks.empty()) {
                continue;
            }
            std::size_t matched = 0;
            double a = std::numeric_limits<double>::infinity();
            double b = -1.0;
            for (std::size_t t : toks) {
                if (match[t].asr >= 0) {
                    const AsrTok& k = at[static_cast<std::size_t>(match[t].asr)];
                    ++matched;
                    a = std::min(a, k.start);
                    b = std::max(b, k.end);
                }
            }
            totalMatched += matched;
            frac[li] = static_cast<double>(matched) / static_cast<double>(toks.size());
            if (matched > 0 && frac[li] >= static_cast<double>(options.minLineMatch)) {
                anchored[li] = 1;
                ++anchoredCount;
                line.startSec = a;
                line.endSec = std::max(a, b);
                line.confidence = static_cast<float>(frac[li]);
                perTok.push_back((line.endSec - line.startSec) / static_cast<double>(toks.size()));
            }
        }
        if (anchoredCount == 0) {
            VoxLyrics r = unsynced;
            r.status = VoxLyricsStatus::AlignmentFailed;
            r.detail = "no line could be matched to the transcript";
            return r;
        }

        // Words of anchored lines (lyric text, ASR times, gaps interpolated).
        for (std::size_t li = 0; li < nLines; ++li) {
            if (anchored[li] == 0) {
                continue;
            }
            VoxLyricLine& line = out.lines[li];
            const std::size_t ns = surfaces[li].size();
            std::vector<double> ws(ns, -1.0), we(ns, -1.0);
            std::vector<float> wc(ns, -1.0f);
            for (std::size_t si = 0; si < ns; ++si) {
                const long t = surfaceTok[li][si];
                if (t >= 0 && match[static_cast<std::size_t>(t)].asr >= 0) {
                    const AsrTok& k = at[static_cast<std::size_t>(match[static_cast<std::size_t>(t)].asr)];
                    ws[si] = k.start;
                    we[si] = k.end;
                    wc[si] = match[static_cast<std::size_t>(t)].sim;
                }
            }
            for (std::size_t si = 0; si < ns;) {
                if (ws[si] >= 0.0) {
                    ++si;
                    continue;
                }
                std::size_t sj = si;
                while (sj < ns && ws[sj] < 0.0) {
                    ++sj;
                }
                const double lo = si > 0 ? we[si - 1] : line.startSec;
                const double hi = sj < ns ? ws[sj] : line.endSec;
                const double span = std::max(0.0, hi - lo);
                const std::size_t cnt = sj - si;
                for (std::size_t k = 0; k < cnt; ++k) {
                    ws[si + k] = lo + span * static_cast<double>(k) / static_cast<double>(cnt);
                    we[si + k] = lo + span * static_cast<double>(k + 1) / static_cast<double>(cnt);
                    wc[si + k] = 0.0f;
                }
                si = sj;
            }
            for (std::size_t si = 0; si < ns; ++si) {
                VoxWord w;
                w.startSec = ws[si];
                w.endSec = we[si];
                w.text = surfaces[li][si];
                w.confidence = wc[si];
                line.words.push_back(std::move(w));
            }
        }

        // Interpolate runs of unanchored text lines between anchors.
        double medianPerTok = 0.45;
        if (!perTok.empty()) {
            std::sort(perTok.begin(), perTok.end());
            medianPerTok = std::max(0.15, perTok[perTok.size() / 2]);
        }
        const double audioEnd = asr.audioSeconds > 0.0 ? asr.audioSeconds : -1.0;
        std::size_t interpolatedCount = 0;
        auto isTextLine = [&](std::size_t li) { return !lineTokens[li].empty(); };
        for (std::size_t li = 0; li < nLines;) {
            if (!isTextLine(li) || anchored[li] != 0) {
                ++li;
                continue;
            }
            std::size_t lj = li;
            std::vector<std::size_t> run;
            while (lj < nLines && (!isTextLine(lj) || anchored[lj] == 0)) {
                if (isTextLine(lj)) {
                    run.push_back(lj);
                }
                ++lj;
            }
            // previous / next anchored text line
            long pa = -1, na = -1;
            for (long k = static_cast<long>(li) - 1; k >= 0; --k) {
                if (anchored[static_cast<std::size_t>(k)] != 0) {
                    pa = k;
                    break;
                }
            }
            if (lj < nLines) {
                na = static_cast<long>(lj);
            }
            double weightSum = 0.0;
            for (std::size_t r : run) {
                weightSum += static_cast<double>(lineTokens[r].size());
            }
            double t0 = 0.0;
            double t1 = 0.0;
            if (pa >= 0 && na >= 0) {
                t0 = out.lines[static_cast<std::size_t>(pa)].endSec;
                t1 = out.lines[static_cast<std::size_t>(na)].startSec;
                if (t1 < t0) {
                    t1 = t0;
                }
            } else if (pa >= 0) {
                t0 = out.lines[static_cast<std::size_t>(pa)].endSec;
                t1 = t0 + weightSum * medianPerTok;
                if (audioEnd > t0 && t1 > audioEnd) {
                    t1 = audioEnd;
                }
            } else {
                t1 = out.lines[static_cast<std::size_t>(na)].startSec;
                t0 = std::max(0.0, t1 - weightSum * medianPerTok);
            }
            double t = t0;
            for (std::size_t r : run) {
                const double share = (t1 - t0) * static_cast<double>(lineTokens[r].size()) / weightSum;
                VoxLyricLine& line = out.lines[r];
                line.startSec = t;
                line.endSec = t + share;
                t += share;
                line.interpolated = true;
                line.confidence = static_cast<float>(std::min(frac[r] * 0.5, 0.25));
                ++interpolatedCount;
            }
            li = lj;
        }

        // Monotonic starts, then break lines take the time of their neighbours.
        double lastStart = 0.0;
        for (std::size_t li = 0; li < nLines; ++li) {
            VoxLyricLine& line = out.lines[li];
            if (!isTextLine(li)) {
                continue;
            }
            line.startSec = std::max(line.startSec, lastStart);
            line.endSec = std::max(line.endSec, line.startSec);
            lastStart = line.startSec;
        }
        for (std::size_t li = 0; li < nLines; ++li) {
            if (isTextLine(li)) {
                continue;
            }
            VoxLyricLine& line = out.lines[li];
            double t = -1.0;
            float c = 1.0f;
            for (long k = static_cast<long>(li) - 1; k >= 0; --k) {
                if (isTextLine(static_cast<std::size_t>(k))) {
                    t = out.lines[static_cast<std::size_t>(k)].endSec;
                    c = out.lines[static_cast<std::size_t>(k)].confidence;
                    break;
                }
            }
            if (t < 0.0) {
                for (std::size_t k = li + 1; k < nLines; ++k) {
                    if (isTextLine(k)) {
                        t = out.lines[k].startSec;
                        c = out.lines[k].confidence;
                        break;
                    }
                }
            }
            line.startSec = line.endSec = std::max(0.0, t);
            line.confidence = c;
            line.interpolated = true;
        }

        std::size_t textLines = 0;
        for (std::size_t li = 0; li < nLines; ++li) {
            textLines += isTextLine(li) ? 1 : 0;
        }
        out.status = VoxLyricsStatus::Ok;
        out.detail = "aligned " + std::to_string(anchoredCount) + "/" + std::to_string(textLines) +
                     " lines to the transcript, " + std::to_string(interpolatedCount) +
                     " interpolated, matched " + std::to_string(totalMatched) + "/" +
                     std::to_string(N) + " words";
        return out;
    });
}

VoxLyrics alignLyricsToVocalRegions(const VoxLyrics& unsynced,
                                    const std::vector<VoxVocalRegion>& regionsIn,
                                    const VoxAlignOptions&) noexcept {
    return guarded([&]() -> VoxLyrics {
        if (unsynced.lines.empty()) {
            return failed(VoxLyricsStatus::NoLyrics, "no lyric lines");
        }
        std::vector<VoxVocalRegion> regions;
        for (const VoxVocalRegion& r : regionsIn) {
            if (std::isfinite(r.startSec) && std::isfinite(r.endSec) && r.endSec > r.startSec) {
                regions.push_back(r);
            }
        }
        std::sort(regions.begin(), regions.end(),
                  [](const VoxVocalRegion& a, const VoxVocalRegion& b) { return a.startSec < b.startSec; });
        if (regions.empty()) {
            VoxLyrics r = unsynced;
            r.status = VoxLyricsStatus::NoVocals;
            r.detail = "no vocal regions to distribute the lines over";
            return r;
        }
        const std::size_t n = unsynced.lines.size();
        std::vector<double> weight(n, 0.0);
        double totalW = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!isBreakLine(unsynced.lines[i])) {
                weight[i] = static_cast<double>(std::max<std::size_t>(1, syllableWeight(unsynced.lines[i].text)));
                totalW += weight[i];
            }
        }
        if (totalW <= 0.0) {
            return failed(VoxLyricsStatus::NoLyrics, "lyrics have no words");
        }
        double totalLen = 0.0;
        double confSum = 0.0;
        for (const VoxVocalRegion& r : regions) {
            const double len = r.endSec - r.startSec;
            totalLen += len;
            confSum += len * static_cast<double>(std::min(1.0f, std::max(0.0f, r.confidence)));
        }
        const float lineConf = static_cast<float>(
            std::min<double>(kVocalFallbackMaxConfidence, kVocalFallbackMaxConfidence * confSum / totalLen));
        // cumulative vocal time -> recording time
        auto mapTime = [&](double c, bool preferEarlier) {
            double acc = 0.0;
            for (std::size_t k = 0; k < regions.size(); ++k) {
                const double len = regions[k].endSec - regions[k].startSec;
                const bool last = k + 1 == regions.size();
                if (c < acc + len || (c <= acc + len && (preferEarlier || last))) {
                    return regions[k].startSec + std::max(0.0, c - acc);
                }
                acc += len;
            }
            return regions.back().endSec;
        };
        VoxLyrics out = unsynced;
        out.source = VoxLyricsSource::AlignedToAudio;
        double cum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            VoxLyricLine& l = out.lines[i];
            l.words.clear();
            l.source = VoxLyricsSource::AlignedToAudio;
            l.interpolated = true;
            if (weight[i] <= 0.0) {
                continue;
            }
            const double c0 = totalLen * cum / totalW;
            cum += weight[i];
            const double c1 = totalLen * cum / totalW;
            l.startSec = mapTime(c0, false);
            l.endSec = std::max(l.startSec, mapTime(c1, true));
            // A line is sung in one go: do not let it run across an instrumental
            // gap into the next region.
            for (const VoxVocalRegion& r : regions) {
                if (l.startSec >= r.startSec - 1e-9 && l.startSec <= r.endSec + 1e-9) {
                    l.endSec = std::max(l.startSec, std::min(l.endSec, r.endSec));
                    break;
                }
            }
            l.confidence = lineConf;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (weight[i] > 0.0) {
                continue;
            }
            double t = -1.0;
            for (long k = static_cast<long>(i) - 1; k >= 0; --k) {
                if (weight[static_cast<std::size_t>(k)] > 0.0) {
                    t = out.lines[static_cast<std::size_t>(k)].endSec;
                    break;
                }
            }
            if (t < 0.0) {
                for (std::size_t k = i + 1; k < n; ++k) {
                    if (weight[k] > 0.0) {
                        t = out.lines[k].startSec;
                        break;
                    }
                }
            }
            out.lines[i].startSec = out.lines[i].endSec = std::max(0.0, t);
            out.lines[i].confidence = lineConf;
            out.lines[i].source = VoxLyricsSource::AlignedToAudio;
            out.lines[i].interpolated = true;
        }
        out.status = VoxLyricsStatus::Ok;
        out.detail = "lines spread over " + std::to_string(regions.size()) +
                     " vocal regions by syllable count (rough estimate, no ASR)";
        return out;
    });
}

VoxLyrics lyricsFromTranscript(const VoxTranscript& asr) noexcept {
    return guarded([&]() -> VoxLyrics {
        VoxLyrics out;
        out.source = VoxLyricsSource::AsrHypothesis;
        out.isOriginalText = false;
        for (const VoxSegment& s : asr.segments) {
            if (s.text.empty()) {
                continue;
            }
            VoxLyricLine l;
            l.startSec = s.startSec;
            l.endSec = s.endSec;
            l.text = s.text;
            l.confidence = s.confidence;
            l.source = VoxLyricsSource::AsrHypothesis;
            l.words = s.words;
            out.lines.push_back(std::move(l));
        }
        if (asr.status != VoxTranscribeStatus::Ok) {
            out.status = mapAsrStatus(asr.status);
            out.detail = asr.detail.empty() ? voxTranscribeStatusName(asr.status) : asr.detail;
        } else if (out.lines.empty()) {
            out.status = VoxLyricsStatus::NoLyrics;
            out.detail = "ASR found no text";
        } else {
            out.status = VoxLyricsStatus::Ok;
            out.detail = "machine transcript, not the original lyrics";
        }
        return out;
    });
}

void markInstrumentalGaps(VoxLyrics& lyrics, const std::vector<VoxVocalRegion>& regionsIn,
                          double audioSeconds, double minGapSeconds) noexcept {
    try {
        std::vector<VoxVocalRegion> regions = regionsIn;
        std::sort(regions.begin(), regions.end(),
                  [](const VoxVocalRegion& a, const VoxVocalRegion& b) { return a.startSec < b.startSec; });
        std::vector<std::pair<double, double>> gaps;
        double prev = 0.0;
        for (const VoxVocalRegion& r : regions) {
            if (r.startSec - prev > minGapSeconds) {
                gaps.emplace_back(prev, r.startSec);
            }
            prev = std::max(prev, r.endSec);
        }
        if (audioSeconds - prev > minGapSeconds) {
            gaps.emplace_back(prev, audioSeconds);
        }
        std::vector<VoxLyricLine> added;
        for (const auto& g : gaps) {
            bool occupied = false;
            for (const VoxLyricLine& l : lyrics.lines) {
                if (l.hasTimes() && !l.instrumental && !isBreakLine(l) && l.startSec > g.first &&
                    l.startSec < g.second) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) {
                continue;
            }
            VoxLyricLine l;
            l.startSec = g.first;
            l.endSec = g.second;
            l.confidence = 0.5f;
            l.source = lyrics.source;
            l.instrumental = true;
            l.interpolated = true;
            added.push_back(std::move(l));
        }
        if (added.empty()) {
            return;
        }
        for (VoxLyricLine& l : added) {
            lyrics.lines.push_back(std::move(l));
        }
        std::stable_sort(lyrics.lines.begin(), lyrics.lines.end(),
                         [](const VoxLyricLine& a, const VoxLyricLine& b) {
                             if (a.hasTimes() != b.hasTimes()) {
                                 return a.hasTimes();
                             }
                             return a.hasTimes() && a.startSec < b.startSec;
                         });
    } catch (...) {
    }
}

namespace {

class SubProgress {
public:
    SubProgress(const VoxProgressFn& outer, float lo, float hi) : outer_(outer), lo_(lo), hi_(hi) {}
    [[nodiscard]] VoxProgressFn fn() const {
        if (!outer_) {
            return {};
        }
        const VoxProgressFn& o = outer_;
        const float lo = lo_, hi = hi_;
        return [&o, lo, hi](float f) { o(lo + (hi - lo) * std::min(1.0f, std::max(0.0f, f))); };
    }

private:
    const VoxProgressFn& outer_;
    float lo_, hi_;
};

bool blank(const std::string& s) {
    for (unsigned char c : s) {
        if (c > ' ') {
            return false;
        }
    }
    return true;
}

VoxLyrics buildImpl(const VoxProvidedLyrics& provided, IVoxAsrBackend* backend,
                    IVoxVocalSeparator* separator, const float* in, std::size_t frames,
                    VoxPcmFormat format, const VoxBuildOptions& options,
                    const VoxCancelToken* cancel, const VoxProgressFn& progress) {
    auto cancelled = [&]() { return cancel != nullptr && cancel->isCancelled(); };
    const bool hasText = !blank(provided.text);

    VoxLyrics unsynced;
    if (hasText && provided.synced) {
        VoxLyrics r = parseLrc(provided.text);
        if (r.status != VoxLyricsStatus::Ok) {
            return r;
        }
        r.providerTag = provided.providerTag;
        r.hostMayCache = provided.hostMayCache;
        if (progress) {
            progress(1.0f);
        }
        return r;
    }
    if (hasText) {
        unsynced = parsePlainLyrics(provided.text);
        if (unsynced.status != VoxLyricsStatus::Ok) {
            return unsynced;
        }
        unsynced.providerTag = provided.providerTag;
        unsynced.hostMayCache = provided.hostMayCache;
    }

    if (in == nullptr || frames == 0) {
        return failed(VoxLyricsStatus::InvalidInput, "no audio");
    }
    if (!isSupportedFormat(format)) {
        return failed(VoxLyricsStatus::UnsupportedFormat, "unsupported sample rate or channels");
    }
    if (!hasText && backend == nullptr) {
        return failed(VoxLyricsStatus::NoLyrics, "no lyrics text and no ASR backend");
    }
    const double audioSeconds = static_cast<double>(frames) / static_cast<double>(format.sampleRate);
    std::string notes;

    const float* srcPcm = in;
    std::size_t srcFrames = frames;
    VoxPcmFormat srcFormat = format;
    std::vector<float> vocals;
    if (separator != nullptr) {
        std::vector<float> mono(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (std::uint32_t c = 0; c < format.channels; ++c) {
                const float v = in[i * format.channels + c];
                if (!std::isfinite(v)) {
                    return failed(VoxLyricsStatus::InvalidInput, "NaN or Inf sample");
                }
                acc += v;
            }
            mono[i] = acc / static_cast<float>(format.channels);
        }
        std::string error;
        const VoxProgressFn sepProgress = SubProgress(progress, 0.0f, 0.25f).fn();
        VoxLyricsStatus st = VoxLyricsStatus::InternalError;
        try {
            st = separator->separate(mono.data(), frames, format.sampleRate, cancel,
                                     sepProgress ? &sepProgress : nullptr, vocals, error);
        } catch (const std::bad_alloc&) {
            return failed(VoxLyricsStatus::OutOfMemory, "separator ran out of memory");
        } catch (...) {
            st = VoxLyricsStatus::BackendError;
            error = "separator threw";
        }
        if (st == VoxLyricsStatus::Cancelled || cancelled()) {
            return failed(VoxLyricsStatus::Cancelled, "cancelled");
        }
        if (st == VoxLyricsStatus::Ok && vocals.size() == frames) {
            srcPcm = vocals.data();
            srcFrames = frames;
            srcFormat = VoxPcmFormat{format.sampleRate, 1};
        } else {
            notes += "separator failed (" + (st == VoxLyricsStatus::Ok ? std::string("length mismatch") : error) +
                     "), used the mix; ";
            vocals.clear();
        }
    }

    std::vector<VoxVocalRegion> regions;
    bool haveRegions = false;
    if (options.detectVocals) {
        const VoxProgressFn vp = SubProgress(progress, 0.25f, 0.4f).fn();
        VoxVocalRegions v = detectVocalRegions(srcPcm, srcFrames, srcFormat, options.vocal, cancel, vp);
        if (v.status == VoxLyricsStatus::Cancelled) {
            return failed(VoxLyricsStatus::Cancelled, "cancelled");
        }
        if (v.status == VoxLyricsStatus::Ok) {
            regions = std::move(v.regions);
            haveRegions = true;
        } else {
            notes += "vocal detection failed (" + v.detail + "); ";
        }
    }

    VoxLyrics result;
    VoxTranscript transcript;
    bool haveTranscript = false;
    if (backend != nullptr) {
        VoxTranscribeOptions asrOpt = options.asr;
        asrOpt.wordTimestamps = true;
        const VoxProgressFn tp = SubProgress(progress, 0.4f, 0.95f).fn();
        transcript = transcribe(backend, srcPcm, srcFrames, srcFormat, asrOpt, cancel, tp);
        if (transcript.status == VoxTranscribeStatus::Cancelled || cancelled()) {
            return failed(VoxLyricsStatus::Cancelled, "cancelled");
        }
        haveTranscript = transcript.status == VoxTranscribeStatus::Ok;
        if (!haveTranscript) {
            notes += std::string("ASR failed (") + voxTranscribeStatusName(transcript.status) +
                     (transcript.detail.empty() ? "" : ": " + transcript.detail) + "); ";
        }
    }

    if (hasText) {
        bool done = false;
        if (haveTranscript) {
            result = alignLyricsToTranscript(unsynced, transcript, options.align, cancel);
            if (result.status == VoxLyricsStatus::Cancelled) {
                return result;
            }
            if (result.status == VoxLyricsStatus::Ok) {
                done = true;
            } else {
                notes += "alignment to the transcript failed (" + result.detail + "); ";
            }
        }
        if (!done) {
            if (!haveRegions) {
                // Last resort: nothing measurable. Return the text unsynced.
                result = unsynced;
                result.status = VoxLyricsStatus::NoVocals;
                result.detail = notes + "no vocal regions, lyrics left unsynced";
                return result;
            }
            result = alignLyricsToVocalRegions(unsynced, regions, options.align);
            if (result.status != VoxLyricsStatus::Ok) {
                result.detail = notes + result.detail;
                return result;
            }
        }
    } else {
        result = lyricsFromTranscript(transcript);
        if (result.status != VoxLyricsStatus::Ok) {
            result.detail = notes + result.detail;
            return result;
        }
    }
    if (haveRegions) {
        markInstrumentalGaps(result, regions, audioSeconds, options.instrumentalGapSeconds);
    }
    result.detail = notes + result.detail;
    if (progress) {
        progress(1.0f);
    }
    return result;
}

} // namespace

VoxLyrics buildLyrics(const VoxProvidedLyrics& provided, IVoxAsrBackend* backend,
                      IVoxVocalSeparator* separator, const float* interleaved, std::size_t frames,
                      VoxPcmFormat format, const VoxBuildOptions& options,
                      const VoxCancelToken* cancel, const VoxProgressFn& progress) noexcept {
    return guarded([&]() {
        return buildImpl(provided, backend, separator, interleaved, frames, format, options,
                         cancel, progress);
    });
}

} // namespace bgmedia::vox
