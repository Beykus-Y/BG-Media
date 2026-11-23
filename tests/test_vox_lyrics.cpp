#include "framework.hpp"

#include "bgmedia/vox/VoxLyrics.hpp"
#include "bgmedia/vox/vox_lyrics_capi.h"
#include "support/VoxSynthetic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace bgmedia::vox;
using namespace bgmedia::voxsynth;

struct W {
    const char* text;
    double a;
    double b;
};

// One segment per call, words with the given times.
VoxSegment seg(const std::vector<W>& words, float conf = 0.8f) {
    VoxSegment s;
    std::string text;
    for (const W& w : words) {
        VoxWord v;
        v.text = std::string(" ") + w.text;
        v.startSec = w.a;
        v.endSec = w.b;
        v.confidence = conf;
        s.words.push_back(v);
        text += (text.empty() ? "" : " ") + std::string(w.text);
    }
    s.text = text;
    s.startSec = words.front().a;
    s.endSec = words.back().b;
    s.confidence = conf;
    s.lowConfidence = false;
    return s;
}

VoxTranscript transcriptOf(std::vector<VoxSegment> segs, double audioSeconds = 60.0) {
    VoxTranscript t;
    t.segments = std::move(segs);
    t.status = VoxTranscribeStatus::Ok;
    t.audioSeconds = audioSeconds;
    return t;
}

class ScriptedBackend final : public IVoxAsrBackend {
public:
    std::vector<VoxSegment> script;  // chunk-relative times; first chunk only
    VoxTranscribeStatus status = VoxTranscribeStatus::Ok;
    int calls = 0;
    bool wordsRequested = false;
    const char* name() const noexcept override { return "scripted"; }
    std::uint32_t expectedSampleRate() const noexcept override { return 16000; }
    VoxAsrBackendInfo info() const override { return {}; }
    VoxTranscribeStatus transcribeChunk(const float*, std::size_t, const VoxTranscribeOptions& o,
                                        const VoxCancelToken*, const VoxProgressFn*,
                                        std::vector<VoxSegment>& out,
                                        std::string& error) override {
        wordsRequested = o.wordTimestamps;
        if (calls++ == 0 && status == VoxTranscribeStatus::Ok) {
            out = script;
        }
        if (status != VoxTranscribeStatus::Ok) {
            error = "scripted failure";
        }
        return status;
    }
};

// Songs used by the vocal tests. Spans are in seconds.
const std::vector<VoiceSpan> kSpans = {{4.0, 10.0}, {14.0, 20.0}};
constexpr std::uint32_t kRate = 44100;
constexpr double kSeconds = 24.0;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

// ------------------------------------------------------------- LRC parsing --

TEST_CASE(vox_lyrics.lrc_basic_and_multi_stamp) {
    const VoxLyrics l = parseLrc(
        "[ti:Song]\n[ar:Someone]\n[00:12.50]Hello\n[00:05.00][00:30.25]Refrain\n"
        "[01:02]Last\n[00:20.123]Mid\n");
    CHECK(l.status == VoxLyricsStatus::Ok);
    CHECK(l.source == VoxLyricsSource::ProviderSynced);
    CHECK(l.isOriginalText);
    CHECK(!l.hostMayCache);
    REQUIRE(l.lines.size() == 5);
    CHECK_NEAR(l.lines[0].startSec, 5.0, 1e-9);
    CHECK(l.lines[0].text == "Refrain");
    CHECK_NEAR(l.lines[1].startSec, 12.5, 1e-9);
    CHECK_NEAR(l.lines[1].endSec, 20.123, 1e-9);  // end = next start
    CHECK_NEAR(l.lines[2].startSec, 20.123, 1e-9);
    CHECK_NEAR(l.lines[3].startSec, 30.25, 1e-9);
    CHECK(l.lines[3].text == "Refrain");
    CHECK_NEAR(l.lines[4].startSec, 62.0, 1e-9);
    CHECK(l.lines[4].endSec < 0.0);
    CHECK(l.isSynced());
    for (const VoxLyricLine& x : l.lines) {
        CHECK(x.source == VoxLyricsSource::ProviderSynced);
        CHECK(x.confidence == 1.0f);
    }
}

TEST_CASE(vox_lyrics.lrc_offset_and_crlf) {
    const VoxLyrics l = parseLrc("[offset:+500]\r\n[00:10.00]A\r\n[00:11.00]B\r\n[00:00.20]C\r\n");
    CHECK(l.status == VoxLyricsStatus::Ok);
    REQUIRE(l.lines.size() == 3);
    CHECK_NEAR(l.lines[0].startSec, 0.0, 1e-9);  // clamped at 0
    CHECK_NEAR(l.lines[1].startSec, 9.5, 1e-9);
    CHECK_NEAR(l.lines[2].startSec, 10.5, 1e-9);
    const VoxLyrics n = parseLrc("[offset:-250]\n[00:01.00]A\n");
    REQUIRE(n.lines.size() == 1);
    CHECK_NEAR(n.lines[0].startSec, 1.25, 1e-9);
}

TEST_CASE(vox_lyrics.lrc_enhanced_words) {
    const VoxLyrics l = parseLrc("[00:10.00]<00:10.00>Hel<00:10.40>lo <00:11.00>world\n[00:13.00]Next\n");
    CHECK(l.status == VoxLyricsStatus::Ok);
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].text == "Hello world");
    REQUIRE(l.lines[0].words.size() == 3);
    CHECK(l.lines[0].words[0].text == "Hel");
    CHECK_NEAR(l.lines[0].words[0].endSec, 10.4, 1e-9);
    CHECK(l.lines[0].words[2].text == "world");
    CHECK_NEAR(l.lines[0].words[2].startSec, 11.0, 1e-9);
    CHECK_NEAR(l.lines[0].words[2].endSec, 13.0, 1e-9);  // up to the next line
    // A '<' that is not a time tag stays text.
    const VoxLyrics t = parseLrc("[00:01.00]a <3 b\n");
    REQUIRE(t.lines.size() == 1);
    CHECK(t.lines[0].text == "a <3 b");
    CHECK(t.lines[0].words.empty());
}

TEST_CASE(vox_lyrics.lrc_garbage_never_throws) {
    CHECK(parseLrc("").status == VoxLyricsStatus::ParseError);
    CHECK(parseLrc("just some text\nwithout stamps").status == VoxLyricsStatus::ParseError);
    CHECK(parseLrc("[00:99.00]x\n[ab:cd]\n[").status == VoxLyricsStatus::ParseError);
    std::string bin;
    for (int i = 0; i < 4000; ++i) {
        bin.push_back(static_cast<char>((i * 131 + 7) & 0xFF));
    }
    const VoxLyrics b = parseLrc(bin);
    CHECK(b.status == VoxLyricsStatus::ParseError || b.status == VoxLyricsStatus::Ok);
    const VoxLyrics nul = parseLrc(std::string_view("[00:01.00]a\0b\n[00:02.00]c\n", 26));
    CHECK(nul.status == VoxLyricsStatus::Ok);
    // Mixed: unstamped line ignored and counted.
    const VoxLyrics m = parseLrc("[00:01.00]one\nstray\n[00:02.00]two\n");
    CHECK(m.status == VoxLyricsStatus::Ok);
    CHECK(m.lines.size() == 2);
    CHECK(m.detail.find("ignored") != std::string::npos);
}

TEST_CASE(vox_lyrics.lrc_utf8_cyrillic_and_invalid_bytes) {
    const VoxLyrics l = parseLrc("\xEF\xBB\xBF[00:01.00]Привет, мир\n[00:03.00]Ёлка \xFF\xFE зелёная\n");
    CHECK(l.status == VoxLyricsStatus::Ok);
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].text == "Привет, мир");
    // Invalid bytes were replaced, the rest is intact and valid UTF-8.
    CHECK(l.lines[1].text.find("Ёлка") == 0);
    CHECK(l.lines[1].text.find("зелёная") != std::string::npos);
    CHECK(l.lines[1].text.find("\xEF\xBF\xBD") != std::string::npos);
    // Never a stray continuation byte at the start of a char.
    for (const VoxLyricLine& x : l.lines) {
        std::size_t i = 0;
        while (i < x.text.size()) {
            const unsigned char c = static_cast<unsigned char>(x.text[i]);
            std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            REQUIRE(len != 0);
            REQUIRE(i + len <= x.text.size());
            for (std::size_t k = 1; k < len; ++k) {
                CHECK((static_cast<unsigned char>(x.text[i + k]) & 0xC0) == 0x80);
            }
            i += len;
        }
    }
}

TEST_CASE(vox_lyrics.lrc_instrumental_and_empty_lines) {
    const VoxLyrics l = parseLrc("[00:00.00]\xE2\x99\xAA\n[00:10.00]Text\n[00:15.00]\n[00:20.00]More\n");
    REQUIRE(l.lines.size() == 4);
    CHECK(l.lines[0].instrumental);
    CHECK(l.lines[0].text.empty());
    CHECK(!l.lines[1].instrumental);
    CHECK(l.lines[2].stanzaBreak);
}

TEST_CASE(vox_lyrics.plain_parse_stanza_breaks) {
    const VoxLyrics l = parsePlainLyrics("\n\nline one\r\n  line   two \n\n\n\nsecond stanza\n\n");
    CHECK(l.status == VoxLyricsStatus::Ok);
    CHECK(l.source == VoxLyricsSource::ProviderUnsynced);
    CHECK(l.isOriginalText);
    REQUIRE(l.lines.size() == 4);
    CHECK(l.lines[0].text == "line one");
    CHECK(l.lines[1].text == "line two");
    CHECK(l.lines[2].stanzaBreak);
    CHECK(l.lines[2].text.empty());
    CHECK(l.lines[3].text == "second stanza");
    for (const VoxLyricLine& x : l.lines) {
        CHECK(!x.hasTimes());
    }
    CHECK(!l.isSynced());
    CHECK(parsePlainLyrics("  \n \n").status == VoxLyricsStatus::ParseError);
}

TEST_CASE(vox_lyrics.tolrc_round_trip) {
    VoxLyrics l = parseLrc("[00:05.10]Привет\n[00:09.55]\xE2\x99\xAA\n[01:15.02]мир\n[02:00.00]end\n");
    REQUIRE(l.status == VoxLyricsStatus::Ok);
    const std::string lrc = toLrc(l);
    const VoxLyrics r = parseLrc(lrc);
    REQUIRE(r.status == VoxLyricsStatus::Ok);
    REQUIRE(r.lines.size() == l.lines.size());
    for (std::size_t i = 0; i < l.lines.size(); ++i) {
        CHECK_NEAR(r.lines[i].startSec, l.lines[i].startSec, 0.006);
        CHECK(r.lines[i].text == l.lines[i].text);
        CHECK(r.lines[i].instrumental == l.lines[i].instrumental);
    }
    CHECK(lrc.find("x-bg-source") == std::string::npos);  // provider lines carry no extra tags
    // Non-provider output keeps its provenance label; the parser ignores it.
    VoxLyrics h;
    h.source = VoxLyricsSource::AsrHypothesis;
    h.isOriginalText = false;
    VoxLyricLine x;
    x.startSec = 3.0;
    x.text = "guess";
    h.lines.push_back(x);
    VoxLyricLine untimed;
    untimed.text = "skipped";
    h.lines.push_back(untimed);
    const std::string hl = toLrc(h);
    CHECK(hl.find("[x-bg-source:AsrHypothesis]") != std::string::npos);
    CHECK(hl.find("[x-bg-original:0]") != std::string::npos);
    CHECK(hl.find("skipped") == std::string::npos);
    const VoxLyrics back = parseLrc(hl);
    CHECK(back.status == VoxLyricsStatus::Ok);
    CHECK(back.lines.size() == 1);
    // Words as enhanced tags.
    VoxLyrics e = parseLrc("[00:10.00]<00:10.00>one <00:10.50>two\n[00:12.00]x\n");
    const VoxLyrics e2 = parseLrc(toLrc(e, true));
    REQUIRE(e2.lines.size() == 2);
    REQUIRE(e2.lines[0].words.size() == 2);
    CHECK_NEAR(e2.lines[0].words[1].startSec, 10.5, 0.006);
}

// --------------------------------------------------------- vocal detection --

TEST_CASE(vox_lyrics.vocal_detect_span_accuracy) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    const VoxVocalRegions r = detectVocalRegions(m.samples.data(), m.samples.size(),
                                                 VoxPcmFormat{kRate, 1});
    CHECK(r.status == VoxLyricsStatus::Ok);
    REQUIRE(r.regions.size() == kSpans.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < kSpans.size(); ++i) {
        const double es = std::fabs(r.regions[i].startSec - kSpans[i].startSec);
        const double ee = std::fabs(r.regions[i].endSec - kSpans[i].endSec);
        worst = std::max({worst, es, ee});
        CHECK(es <= 0.3);
        CHECK(ee <= 0.3);
        CHECK(r.regions[i].confidence > 0.5f);
    }
    std::printf("[vox_lyrics] vocal span worst boundary error %.3f s (0 dB voice/bed)\n", worst);
    CHECK_NEAR(r.vocalFraction(), 12.0 / kSeconds, 0.04);
}

TEST_CASE(vox_lyrics.vocal_detect_bed_only_has_no_regions) {
    for (std::uint64_t seed : {1u, 2u, 3u}) {
        const MusicSignal m = makeSungMusic(kRate, kSeconds, {}, seed, 0.0);
        const VoxVocalRegions r = detectVocalRegions(m.samples.data(), m.samples.size(),
                                                     VoxPcmFormat{kRate, 1});
        CHECK(r.status == VoxLyricsStatus::Ok);
        CHECK(r.regions.empty());
    }
}

TEST_CASE(vox_lyrics.vocal_detect_stereo_int16_and_rates) {
    const MusicSignal m = makeSungMusic(32000, kSeconds, kSpans, 5, -3.0);
    std::vector<std::int16_t> st(m.samples.size() * 2);
    for (std::size_t i = 0; i < m.samples.size(); ++i) {
        const auto v = static_cast<std::int16_t>(m.samples[i] * 30000.0f);
        st[2 * i] = v;
        st[2 * i + 1] = static_cast<std::int16_t>(v / 2);
    }
    const VoxVocalRegions r = detectVocalRegions(st.data(), m.samples.size(), VoxPcmFormat{32000, 2});
    REQUIRE(r.status == VoxLyricsStatus::Ok);
    REQUIRE(r.regions.size() == 2);
    CHECK_NEAR(r.regions[0].startSec, 4.0, 0.3);
    CHECK_NEAR(r.regions[1].endSec, 20.0, 0.3);
}

TEST_CASE(vox_lyrics.vocal_detect_snr_sweep) {
    // Measured, not promised: how far below the bed the voice may drop.
    int detected = 0;
    for (double db : {6.0, 0.0, -6.0}) {
        const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 21, db);
        const VoxVocalRegions r = detectVocalRegions(m.samples.data(), m.samples.size(),
                                                     VoxPcmFormat{kRate, 1});
        bool ok = r.status == VoxLyricsStatus::Ok && r.regions.size() == 2 &&
                  near(r.regions[0].startSec, 4.0, 0.3) && near(r.regions[1].endSec, 20.0, 0.3);
        std::printf("[vox_lyrics] voice %+.0f dB vs bed: %zu regions %s\n", db, r.regions.size(),
                    ok ? "ok" : "MISS");
        detected += ok ? 1 : 0;
    }
    CHECK(detected >= 2);
}

TEST_CASE(vox_lyrics.vocal_detect_known_limit_harmonic_pad) {
    // Documented limitation, printed for the record and deliberately not asserted:
    // a sustained harmonic pad in the vocal band looks like a voice to a
    // harmonicity detector (there is no separation model).
    const MusicSignal bed = makeSungMusic(kRate, kSeconds, {}, 1, 0.0, true);
    const VoxVocalRegions r = detectVocalRegions(bed.samples.data(), bed.samples.size(),
                                                 VoxPcmFormat{kRate, 1});
    CHECK(r.status == VoxLyricsStatus::Ok);
    std::printf("[vox_lyrics] harmonic pad, no voice: %zu false regions, %.0f%% of the time\n",
                r.regions.size(), 100.0 * r.vocalFraction());
}

TEST_CASE(vox_lyrics.vocal_detect_validation_and_cancel) {
    const std::vector<float> x(48000, 0.1f);
    CHECK(detectVocalRegions(static_cast<const float*>(nullptr), 100, VoxPcmFormat{16000, 1}).status ==
          VoxLyricsStatus::InvalidInput);
    CHECK(detectVocalRegions(x.data(), 0, VoxPcmFormat{16000, 1}).status == VoxLyricsStatus::InvalidInput);
    CHECK(detectVocalRegions(x.data(), x.size(), VoxPcmFormat{4000, 1}).status ==
          VoxLyricsStatus::UnsupportedFormat);
    std::vector<float> bad = x;
    bad[100] = std::nanf("");
    CHECK(detectVocalRegions(bad.data(), bad.size(), VoxPcmFormat{16000, 1}).status ==
          VoxLyricsStatus::InvalidInput);
    VoxVocalDetectOptions o;
    o.maxDurationSeconds = 1.0;
    CHECK(detectVocalRegions(x.data(), x.size(), VoxPcmFormat{16000, 1}, o).status ==
          VoxLyricsStatus::TooLong);
    // Silence and DC: nothing vocal, no crash.
    const std::vector<float> zero(48000, 0.0f);
    const VoxVocalRegions z = detectVocalRegions(zero.data(), zero.size(), VoxPcmFormat{16000, 1});
    CHECK(z.status == VoxLyricsStatus::Ok);
    CHECK(z.regions.empty());
    // Cancellation.
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    VoxCancelToken tok;
    tok.cancel();
    CHECK(detectVocalRegions(m.samples.data(), m.samples.size(), VoxPcmFormat{kRate, 1}, {}, &tok).status ==
          VoxLyricsStatus::Cancelled);
    VoxCancelToken late;
    int calls = 0;
    const VoxVocalRegions c = detectVocalRegions(m.samples.data(), m.samples.size(), VoxPcmFormat{kRate, 1}, {},
                                                 &late, [&](float) {
                                                     if (++calls == 2) late.cancel();
                                                 });
    CHECK(c.status == VoxLyricsStatus::Cancelled);
}

// --------------------------------------------------------------- alignment --

namespace {

VoxLyrics russianLyrics() {
    return parsePlainLyrics(
        "Ой, мороз, мороз,\nне морозь меня,\n\nне морозь меня,\nмоего коня.\nВсё пройдёт");
}

} // namespace

TEST_CASE(vox_lyrics.align_dp_with_errors_ru) {
    VoxLyrics text = russianLyrics();
    text.providerTag = "test-provider";
    text.hostMayCache = true;
    // ASR: "Ой" lost, "мороз" mis-heard once, an extra word, "коня" -> "каня",
    // "Всё" -> "все" (ё folded), plus chatter before the song.
    const VoxTranscript asr = transcriptOf({
        seg({{"привет", 1.0, 1.5}}),
        seg({{"мороз", 10.0, 10.6}, {"мороз", 10.7, 11.3}}),
        seg({{"не", 12.0, 12.2}, {"марозь", 12.3, 12.9}, {"ну", 13.0, 13.2}, {"меня", 13.3, 13.8}}),
        seg({{"не", 15.0, 15.2}, {"морозь", 15.3, 15.9}, {"меня", 16.0, 16.5}}),
        seg({{"моего", 18.0, 18.6}, {"каня", 18.7, 19.3}}),
        seg({{"все", 21.0, 21.4}, {"пройдет", 21.5, 22.2}}),
    });
    const VoxLyrics a = alignLyricsToTranscript(text, asr);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    CHECK(a.source == VoxLyricsSource::AlignedToAudio);
    CHECK(a.isOriginalText);              // the text is still the provider's
    CHECK(a.providerTag == "test-provider");
    CHECK(a.hostMayCache);
    REQUIRE(a.lines.size() == 6);
    // line 0 "Ой, мороз, мороз,": two of three words found
    CHECK_NEAR(a.lines[0].startSec, 10.0, 1e-6);
    CHECK_NEAR(a.lines[0].endSec, 11.3, 1e-6);
    CHECK_NEAR(a.lines[0].confidence, 2.0 / 3.0, 1e-3);
    CHECK_NEAR(a.lines[1].startSec, 12.0, 1e-6);
    CHECK_NEAR(a.lines[1].endSec, 13.8, 1e-6);
    CHECK(a.lines[2].stanzaBreak);
    CHECK_NEAR(a.lines[3].startSec, 15.0, 1e-6);
    CHECK_NEAR(a.lines[3].confidence, 1.0, 1e-3);
    CHECK_NEAR(a.lines[4].startSec, 18.0, 1e-6);
    CHECK_NEAR(a.lines[5].startSec, 21.0, 1e-6);
    CHECK_NEAR(a.lines[5].endSec, 22.2, 1e-6);
    // Words carry the lyric text, ASR times; the lost word is interpolated.
    REQUIRE(a.lines[0].words.size() == 3);
    CHECK(a.lines[0].words[0].text == "Ой,");
    CHECK(a.lines[0].words[0].endSec <= 10.0 + 1e-6);
    CHECK(a.lines[0].words[1].text == "мороз,");
    CHECK_NEAR(a.lines[0].words[1].startSec, 10.0, 1e-6);
    for (const VoxLyricLine& l : a.lines) {
        CHECK(l.source == VoxLyricsSource::AlignedToAudio);
    }
    // Monotonic.
    for (std::size_t i = 1; i < a.lines.size(); ++i) {
        CHECK(a.lines[i].startSec >= a.lines[i - 1].startSec);
    }
}

TEST_CASE(vox_lyrics.align_dp_english_case_and_punctuation) {
    const VoxLyrics text = parsePlainLyrics("Don't stop believin',\nHold on to that feelin'!\nStreetlights, people");
    const VoxTranscript asr = transcriptOf({
        seg({{"dont", 5.0, 5.3}, {"stop", 5.4, 5.8}, {"believing", 5.9, 6.6}}),
        seg({{"hold", 8.0, 8.3}, {"on", 8.4, 8.5}, {"to", 8.6, 8.7}, {"that", 8.8, 9.0}, {"feeling", 9.1, 9.8}}),
        seg({{"street", 11.0, 11.4}, {"lights", 11.5, 11.9}, {"people", 12.0, 12.5}}),
    });
    const VoxLyrics a = alignLyricsToTranscript(text, asr);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    REQUIRE(a.lines.size() == 3);
    CHECK_NEAR(a.lines[0].startSec, 5.0, 1e-6);
    CHECK_NEAR(a.lines[0].confidence, 1.0, 1e-3);
    CHECK_NEAR(a.lines[1].startSec, 8.0, 1e-6);
    CHECK_NEAR(a.lines[1].endSec, 9.8, 1e-6);
    // "Streetlights" vs "street"+"lights": one lyric word, two ASR words; the
    // line is still anchored through "people".
    CHECK(a.lines[2].startSec >= 11.0 - 1e-6);
    CHECK_NEAR(a.lines[2].endSec, 12.5, 1e-6);
    CHECK(a.lines[2].confidence >= 0.5f);
}

TEST_CASE(vox_lyrics.align_interpolates_unmatched_lines) {
    const VoxLyrics text = parsePlainLyrics(
        "alpha bravo charlie\ndelta echo foxtrot\ngolf hotel india\njuliet kilo lima");
    // ASR heard nothing for lines 1 and 2.
    const VoxTranscript asr = transcriptOf({
        seg({{"alpha", 10.0, 10.5}, {"bravo", 10.6, 11.0}, {"charlie", 11.1, 11.8}}),
        seg({{"juliet", 30.0, 30.5}, {"kilo", 30.6, 31.0}, {"lima", 31.1, 31.8}}),
    });
    const VoxLyrics a = alignLyricsToTranscript(text, asr);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    REQUIRE(a.lines.size() == 4);
    CHECK(!a.lines[0].interpolated);
    CHECK(a.lines[1].interpolated);
    CHECK(a.lines[2].interpolated);
    CHECK(!a.lines[3].interpolated);
    // Both interpolated lines lie in the gap, in order, equal share.
    CHECK_NEAR(a.lines[1].startSec, 11.8, 1e-6);
    CHECK_NEAR(a.lines[1].endSec, 20.9, 1e-6);
    CHECK_NEAR(a.lines[2].startSec, 20.9, 1e-6);
    CHECK_NEAR(a.lines[2].endSec, 30.0, 1e-6);
    CHECK(a.lines[1].isLowConfidence());
    CHECK(a.lines[2].isLowConfidence());
    CHECK(a.lines[1].confidence <= 0.25f);
    CHECK(!a.lines[0].isLowConfidence());
    CHECK(a.lines[1].words.empty());
}

TEST_CASE(vox_lyrics.align_leading_and_trailing_unmatched) {
    const VoxLyrics text = parsePlainLyrics("one two three\nfour five six\nseven eight nine\nten eleven twelve");
    const VoxTranscript asr = transcriptOf({
        seg({{"four", 20.0, 20.4}, {"five", 20.5, 20.9}, {"six", 21.0, 21.4}}),
        seg({{"seven", 24.0, 24.4}, {"eight", 24.5, 24.9}, {"nine", 25.0, 25.4}}),
    }, 40.0);
    const VoxLyrics a = alignLyricsToTranscript(text, asr);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    CHECK(a.lines[0].interpolated);
    CHECK(a.lines[0].endSec <= 20.0 + 1e-6);
    CHECK(a.lines[0].startSec >= 0.0);
    CHECK(a.lines[3].interpolated);
    CHECK(a.lines[3].startSec >= 25.4 - 1e-6);
    CHECK(a.lines[3].endSec <= 40.0 + 1e-6);
}

TEST_CASE(vox_lyrics.align_low_confidence_and_failure) {
    const VoxLyrics text = parsePlainLyrics("completely different words here\nanother unrelated line");
    const VoxTranscript unrelated = transcriptOf({seg({{"zebra", 1.0, 1.5}, {"quartz", 1.6, 2.0}})});
    const VoxLyrics f = alignLyricsToTranscript(text, unrelated);
    CHECK(f.status == VoxLyricsStatus::AlignmentFailed);
    CHECK(f.source == VoxLyricsSource::ProviderUnsynced);
    for (const VoxLyricLine& l : f.lines) {
        CHECK(!l.hasTimes());
    }
    // One word in three is not enough to trust a line as an anchor.
    const VoxLyrics t2 = parsePlainLyrics("sun moon star\nrain wind snow\nfire earth water");
    const VoxTranscript weak = transcriptOf({
        seg({{"sun", 5.0, 5.4}, {"moon", 5.5, 5.9}, {"star", 6.0, 6.4}}),
        seg({{"rain", 9.0, 9.4}}),
        seg({{"fire", 14.0, 14.4}, {"earth", 14.5, 14.9}, {"water", 15.0, 15.4}}),
    });
    const VoxLyrics a = alignLyricsToTranscript(t2, weak);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    CHECK(a.lines[1].interpolated);
    CHECK(a.lines[1].isLowConfidence());
    CHECK(a.lines[1].startSec >= 6.4 - 1e-6);
    CHECK(a.lines[1].endSec <= 14.0 + 1e-6);
    // Empty inputs.
    CHECK(alignLyricsToTranscript(VoxLyrics{}, weak).status == VoxLyricsStatus::NoLyrics);
    CHECK(alignLyricsToTranscript(text, transcriptOf({})).status == VoxLyricsStatus::AlignmentFailed);
}

TEST_CASE(vox_lyrics.align_segments_without_words_and_cancel) {
    const VoxLyrics text = parsePlainLyrics("first line of the song\nsecond line of the song");
    VoxSegment s1;
    s1.text = "first line of the song";
    s1.startSec = 4.0;
    s1.endSec = 8.0;
    s1.confidence = 0.7f;
    VoxSegment s2;
    s2.text = "second line of the song";
    s2.startSec = 9.0;
    s2.endSec = 13.0;
    s2.confidence = 0.7f;
    const VoxLyrics a = alignLyricsToTranscript(text, transcriptOf({s1, s2}));
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    CHECK_NEAR(a.lines[0].startSec, 4.0, 1e-6);
    CHECK_NEAR(a.lines[0].endSec, 8.0, 1e-6);
    CHECK_NEAR(a.lines[1].startSec, 9.0, 1e-6);
    VoxCancelToken tok;
    tok.cancel();
    CHECK(alignLyricsToTranscript(text, transcriptOf({s1, s2}), {}, &tok).status ==
          VoxLyricsStatus::Cancelled);
    VoxAlignOptions tiny;
    tiny.maxMatrixCells = 4;
    CHECK(alignLyricsToTranscript(text, transcriptOf({s1, s2}), tiny).status == VoxLyricsStatus::TooLong);
}

TEST_CASE(vox_lyrics.align_to_vocal_regions_fallback) {
    const VoxLyrics text = parsePlainLyrics("раз два три\n\nпять шесть семь");
    const std::vector<VoxVocalRegion> regions = {{4.0, 10.0, 0.9f}, {14.0, 20.0, 0.9f}};
    const VoxLyrics a = alignLyricsToVocalRegions(text, regions);
    REQUIRE(a.status == VoxLyricsStatus::Ok);
    CHECK(a.source == VoxLyricsSource::AlignedToAudio);
    CHECK(a.isOriginalText);
    REQUIRE(a.lines.size() == 3);
    CHECK_NEAR(a.lines[0].startSec, 4.0, 1e-6);
    // equal weights: the first line ends exactly where region 1 ends
    CHECK_NEAR(a.lines[0].endSec, 10.0, 1e-6);
    CHECK_NEAR(a.lines[2].startSec, 14.0, 1e-6);
    CHECK_NEAR(a.lines[2].endSec, 20.0, 1e-6);
    for (const VoxLyricLine& l : a.lines) {
        CHECK(l.confidence <= 0.3f + 1e-6f);
        CHECK(l.interpolated);
        CHECK(l.isLowConfidence());
    }
    CHECK(alignLyricsToVocalRegions(text, {}).status == VoxLyricsStatus::NoVocals);
    CHECK(alignLyricsToVocalRegions(VoxLyrics{}, regions).status == VoxLyricsStatus::NoLyrics);
}

TEST_CASE(vox_lyrics.asr_hypothesis_is_never_original) {
    VoxSegment s;
    s.text = "maybe words";
    s.startSec = 1.0;
    s.endSec = 3.0;
    s.confidence = 0.4f;
    const VoxLyrics h = lyricsFromTranscript(transcriptOf({s}));
    REQUIRE(h.status == VoxLyricsStatus::Ok);
    CHECK(h.source == VoxLyricsSource::AsrHypothesis);
    CHECK(!h.isOriginalText);
    CHECK(!h.hostMayCache);
    REQUIRE(h.lines.size() == 1);
    CHECK(h.lines[0].source == VoxLyricsSource::AsrHypothesis);
    CHECK_NEAR(h.lines[0].confidence, 0.4, 1e-6);
    CHECK(h.lines[0].isLowConfidence());
    CHECK(lyricsFromTranscript(transcriptOf({})).status == VoxLyricsStatus::NoLyrics);
    VoxTranscript failedAsr;
    failedAsr.status = VoxTranscribeStatus::BackendError;
    CHECK(lyricsFromTranscript(failedAsr).status == VoxLyricsStatus::BackendError);
    CHECK(!lyricsFromTranscript(failedAsr).isOriginalText);
    CHECK(toLrc(h).find("[x-bg-original:0]") != std::string::npos);
}

TEST_CASE(vox_lyrics.instrumental_gap_lines) {
    VoxLyrics l = parsePlainLyrics("a b\nc d");
    l.lines[0].startSec = 12.0;
    l.lines[0].endSec = 15.0;
    l.lines[1].startSec = 16.0;
    l.lines[1].endSec = 18.0;
    l.source = VoxLyricsSource::AlignedToAudio;
    const std::vector<VoxVocalRegion> regions = {{12.0, 15.0, 0.9f}, {16.0, 18.0, 0.9f}, {30.0, 32.0, 0.9f}};
    markInstrumentalGaps(l, regions, 40.0, 8.0);
    // intro 0..12, gap 18..30, outro 32..40 is exactly 8 s and not longer.
    REQUIRE(l.lines.size() == 4);
    CHECK(l.lines[0].instrumental);
    CHECK_NEAR(l.lines[0].startSec, 0.0, 1e-9);
    CHECK_NEAR(l.lines[0].endSec, 12.0, 1e-9);
    CHECK(l.lines[0].text.empty());
    CHECK(!l.lines[1].instrumental);
    CHECK(!l.lines[2].instrumental);
    CHECK(l.lines[3].instrumental);
    CHECK_NEAR(l.lines[3].startSec, 18.0, 1e-9);
    CHECK_NEAR(l.lines[3].endSec, 30.0, 1e-9);
    const std::string lrc = toLrc(l);
    CHECK(lrc.find("\xE2\x99\xAA") != std::string::npos);
    // A gap in which a lyric line starts is not marked.
    VoxLyrics o = parsePlainLyrics("x y");
    o.lines[0].startSec = 20.0;
    o.lines[0].endSec = 22.0;
    markInstrumentalGaps(o, {{2.0, 3.0, 0.9f}}, 30.0, 8.0);
    int instr = 0;
    for (const VoxLyricLine& x : o.lines) instr += x.instrumental ? 1 : 0;
    CHECK(instr == 0);  // intro 0..2 is short and the gap 3..30 holds a lyric line
}

// ---------------------------------------------------------------- pipeline --

TEST_CASE(vox_lyrics.build_provider_synced_is_returned_as_is) {
    VoxProvidedLyrics p;
    p.text = "[00:01.00]one\n[00:02.00]two\n";
    p.synced = true;
    p.providerTag = "prov";
    p.hostMayCache = true;
    const VoxLyrics l = buildLyrics(p, nullptr, nullptr, nullptr, 0, VoxPcmFormat{0, 0});
    CHECK(l.status == VoxLyricsStatus::Ok);
    CHECK(l.source == VoxLyricsSource::ProviderSynced);
    CHECK(l.providerTag == "prov");
    CHECK(l.hostMayCache);
    CHECK(l.isOriginalText);
    CHECK(l.lines.size() == 2);
    p.text = "no stamps here";
    CHECK(buildLyrics(p, nullptr, nullptr, nullptr, 0, VoxPcmFormat{0, 0}).status ==
          VoxLyricsStatus::ParseError);
}

TEST_CASE(vox_lyrics.build_unsynced_with_fake_asr) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    ScriptedBackend be;
    be.script = {
        seg({{"hello", 4.2, 4.8}, {"darkness", 4.9, 5.6}, {"my", 5.7, 5.9}, {"old", 6.0, 6.3}, {"friend", 6.4, 7.0}}),
        seg({{"ive", 14.2, 14.4}, {"come", 14.5, 14.8}, {"to", 14.9, 15.0}, {"talk", 15.1, 15.5}, {"with", 15.6, 15.8}, {"you", 15.9, 16.2}}),
    };
    VoxProvidedLyrics p;
    p.text = "Hello darkness, my old friend\nI've come to talk with you again";
    p.providerTag = "prov-x";
    p.hostMayCache = false;
    VoxBuildOptions o;
    o.asr.language = "en";
    std::vector<float> prog;
    const VoxLyrics l = buildLyrics(p, &be, nullptr, m.samples.data(), m.samples.size(),
                                    VoxPcmFormat{kRate, 1}, o, nullptr,
                                    [&](float f) { prog.push_back(f); });
    REQUIRE(l.status == VoxLyricsStatus::Ok);
    CHECK(be.wordsRequested);
    CHECK(l.source == VoxLyricsSource::AlignedToAudio);
    CHECK(l.isOriginalText);
    CHECK(l.providerTag == "prov-x");
    CHECK(!l.hostMayCache);
    // intro 0..4 and outro 20..24 are shorter than 8 s, the gap 10..14 too: no gap lines.
    REQUIRE(l.lines.size() == 2);
    CHECK_NEAR(l.lines[0].startSec, 4.2, 1e-3);
    CHECK_NEAR(l.lines[0].endSec, 7.0, 1e-3);
    CHECK_NEAR(l.lines[1].startSec, 14.2, 1e-3);
    CHECK(l.lines[1].confidence >= 0.8f);  // "again" missing: 5 of 6 words
    CHECK(!prog.empty());
    CHECK(prog.back() == 1.0f);
    for (std::size_t i = 1; i < prog.size(); ++i) CHECK(prog[i] >= prog[i - 1]);
}

TEST_CASE(vox_lyrics.build_instrumental_gap_lines_from_regions) {
    // 60 s: voice 4-10 s and 44-50 s, 34 s of bed between them.
    const MusicSignal m = makeSungMusic(kRate, 60.0, {{4.0, 10.0}, {44.0, 50.0}}, 13, 0.0);
    VoxProvidedLyrics p;
    p.text = "first verse text here\nsecond verse text here";
    const VoxLyrics l = buildLyrics(p, nullptr, nullptr, m.samples.data(), m.samples.size(),
                                    VoxPcmFormat{kRate, 1});
    REQUIRE(l.status == VoxLyricsStatus::Ok);
    CHECK(l.source == VoxLyricsSource::AlignedToAudio);
    int instrumental = 0;
    for (const VoxLyricLine& x : l.lines) {
        if (x.instrumental) {
            ++instrumental;
            CHECK(x.text.empty());
            CHECK(x.endSec - x.startSec > 8.0);
        }
    }
    for (const VoxLyricLine& x : l.lines) {
        if (!x.instrumental && !x.text.empty()) {
            // a sung line never runs across the 34 s break
            CHECK(x.endSec <= 10.5 || x.startSec >= 43.5);
        }
    }
    CHECK(instrumental == 2);  // the 34 s break and the 10 s outro
}

TEST_CASE(vox_lyrics.build_without_text) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    VoxProvidedLyrics none;
    CHECK(buildLyrics(none, nullptr, nullptr, m.samples.data(), m.samples.size(),
                      VoxPcmFormat{kRate, 1}).status == VoxLyricsStatus::NoLyrics);
    ScriptedBackend be;
    be.script = {seg({{"la", 5.0, 5.5}, {"la", 5.6, 6.0}}, 0.35f)};
    const VoxLyrics h = buildLyrics(none, &be, nullptr, m.samples.data(), m.samples.size(),
                                    VoxPcmFormat{kRate, 1});
    REQUIRE(h.status == VoxLyricsStatus::Ok);
    CHECK(h.source == VoxLyricsSource::AsrHypothesis);
    CHECK(!h.isOriginalText);
    REQUIRE(!h.lines.empty());
    CHECK(h.lines[0].isLowConfidence());
    for (const VoxLyricLine& x : h.lines) {
        CHECK(x.source == VoxLyricsSource::AsrHypothesis);
    }
}

TEST_CASE(vox_lyrics.build_asr_failure_falls_back_to_vocal_regions) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    ScriptedBackend be;
    be.status = VoxTranscribeStatus::BackendError;
    VoxProvidedLyrics p;
    p.text = "line one here\nline two here";
    const VoxLyrics l = buildLyrics(p, &be, nullptr, m.samples.data(), m.samples.size(),
                                    VoxPcmFormat{kRate, 1});
    REQUIRE(l.status == VoxLyricsStatus::Ok);
    CHECK(l.detail.find("ASR failed") != std::string::npos);
    REQUIRE(l.lines.size() >= 2);
    CHECK(l.lines[0].interpolated);
    CHECK(l.lines[0].confidence <= 0.3f);
    CHECK(l.isOriginalText);
}

TEST_CASE(vox_lyrics.build_cancel_and_validation) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    VoxProvidedLyrics p;
    p.text = "some words";
    VoxCancelToken tok;
    tok.cancel();
    CHECK(buildLyrics(p, nullptr, nullptr, m.samples.data(), m.samples.size(), VoxPcmFormat{kRate, 1}, {},
                      &tok).status == VoxLyricsStatus::Cancelled);
    CHECK(buildLyrics(p, nullptr, nullptr, nullptr, 0, VoxPcmFormat{kRate, 1}).status ==
          VoxLyricsStatus::InvalidInput);
    CHECK(buildLyrics(p, nullptr, nullptr, m.samples.data(), m.samples.size(), VoxPcmFormat{100, 1}).status ==
          VoxLyricsStatus::UnsupportedFormat);
}

namespace {
class HalfSeparator final : public IVoxVocalSeparator {
public:
    int calls = 0;
    const char* name() const noexcept override { return "half"; }
    VoxLyricsStatus separate(const float* mono, std::size_t frames, std::uint32_t, const VoxCancelToken*,
                             const VoxProgressFn*, std::vector<float>& out, std::string&) override {
        ++calls;
        out.assign(mono, mono + frames);
        return VoxLyricsStatus::Ok;
    }
};
}

TEST_CASE(vox_lyrics.build_uses_separator_output) {
    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    HalfSeparator sep;
    VoxProvidedLyrics p;
    p.text = "one two\nthree four";
    const VoxLyrics l = buildLyrics(p, nullptr, &sep, m.samples.data(), m.samples.size(),
                                    VoxPcmFormat{kRate, 1});
    CHECK(sep.calls == 1);
    CHECK(l.status == VoxLyricsStatus::Ok);
}

// -------------------------------------------------------------------- C ABI --

TEST_CASE(vox_lyrics.capi_smoke) {
    CHECK(std::string(bgm_vox_lyrics_status_name(BGM_VOX_LY_PARSE_ERROR)) == "ParseError");
    CHECK(std::string(bgm_vox_lyrics_source_name(BGM_VOX_LY_SRC_ASR_HYPOTHESIS)) == "AsrHypothesis");
    CHECK(std::string(bgm_vox_lyrics_status_name(99)) == "Unknown");

    const char* lrc = "[00:01.50]Привет\n[00:03.00]мир\n";
    bgm_vox_lyrics* l = nullptr;
    CHECK(bgm_vox_lyrics_parse_lrc(lrc, std::char_traits<char>::length(lrc), &l) == BGM_VOX_LY_OK);
    REQUIRE(l != nullptr);
    CHECK(bgm_vox_lyrics_line_count(l) == 2);
    CHECK(std::string(bgm_vox_lyrics_line_text(l, 0)) == "Привет");
    CHECK_NEAR(bgm_vox_lyrics_line_start(l, 0), 1.5, 1e-9);
    CHECK_NEAR(bgm_vox_lyrics_line_end(l, 0), 3.0, 1e-9);
    CHECK(bgm_vox_lyrics_line_source(l, 0) == BGM_VOX_LY_SRC_PROVIDER_SYNCED);
    CHECK(bgm_vox_lyrics_is_original_text(l) == 1);
    CHECK(bgm_vox_lyrics_host_may_cache(l) == 0);
    bgm_vox_lyrics_set_provenance(l, "adapter", 1);
    CHECK(std::string(bgm_vox_lyrics_provider_tag(l)) == "adapter");
    CHECK(bgm_vox_lyrics_host_may_cache(l) == 1);
    CHECK(std::string(bgm_vox_lyrics_to_lrc(l)).find("Привет") != std::string::npos);
    CHECK(bgm_vox_lyrics_line_start(l, 9) < 0.0);
    CHECK(std::string(bgm_vox_lyrics_line_text(l, 9)).empty());
    bgm_vox_lyrics_free(l);

    bgm_vox_lyrics* bad = nullptr;
    CHECK(bgm_vox_lyrics_parse_lrc("garbage", 7, &bad) == BGM_VOX_LY_PARSE_ERROR);
    REQUIRE(bad != nullptr);
    CHECK(bgm_vox_lyrics_line_count(bad) == 0);
    bgm_vox_lyrics_free(bad);
    bgm_vox_lyrics_free(nullptr);
    CHECK(bgm_vox_lyrics_parse_lrc(nullptr, 0, nullptr) == BGM_VOX_LY_INVALID_INPUT);

    const char* plain = "one two\nthree four\n";
    bgm_vox_lyrics* u = nullptr;
    REQUIRE(bgm_vox_lyrics_parse_plain(plain, std::char_traits<char>::length(plain), &u) == BGM_VOX_LY_OK);
    CHECK(bgm_vox_lyrics_source(u) == BGM_VOX_LY_SRC_PROVIDER_UNSYNCED);

    const MusicSignal m = makeSungMusic(kRate, kSeconds, kSpans, 11, 0.0);
    std::vector<std::int16_t> pcm(m.samples.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(m.samples[i] * 30000.0f);
    }
    bgm_vox_lyrics* a = nullptr;
    CHECK(bgm_vox_lyrics_align_to_vocals_s16(u, pcm.data(), pcm.size(), kRate, 1, 0.0, nullptr, nullptr,
                                             nullptr, &a) == BGM_VOX_LY_OK);
    REQUIRE(a != nullptr);
    CHECK(bgm_vox_lyrics_source(a) == BGM_VOX_LY_SRC_ALIGNED_TO_AUDIO);
    REQUIRE(bgm_vox_lyrics_line_count(a) == 2);
    CHECK_NEAR(bgm_vox_lyrics_line_start(a, 0), 4.0, 0.3);
    CHECK(bgm_vox_lyrics_line_confidence(a, 0) <= 0.3f);
    CHECK(bgm_vox_lyrics_line_interpolated(a, 0) == 1);
    CHECK(bgm_vox_lyrics_line_instrumental(a, 0) == 0);
    bgm_vox_lyrics_free(a);

    volatile int flag = 1;
    bgm_vox_lyrics* c = nullptr;
    CHECK(bgm_vox_lyrics_align_to_vocals_s16(u, pcm.data(), pcm.size(), kRate, 1, 0.0, &flag, nullptr,
                                             nullptr, &c) == BGM_VOX_LY_CANCELLED);
    bgm_vox_lyrics_free(c);
    bgm_vox_lyrics* n = nullptr;
    CHECK(bgm_vox_lyrics_align_to_vocals_s16(nullptr, pcm.data(), pcm.size(), kRate, 1, 0.0, nullptr,
                                             nullptr, nullptr, &n) == BGM_VOX_LY_INVALID_INPUT);
    bgm_vox_lyrics_free(n);
    bgm_vox_lyrics_free(u);
}
