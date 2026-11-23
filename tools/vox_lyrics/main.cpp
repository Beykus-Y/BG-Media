// bgmedia_vox_lyrics: lyric timing CLI for BG Vox Lyrics.
//
//   bgmedia_vox_lyrics song.wav --lyrics text.txt | --lrc file.lrc
//       [--model ggml.bin --lang ru]   (ASR alignment; only when built with BGMEDIA_VOX_WHISPER)
//       [--gap SECONDS] [--out out.lrc] [--json out.json]
//
// --lrc: host-supplied synced lyrics, printed as they are (song.wav is not read).
// --lyrics: host-supplied plain text, timed against the audio: with --model by
// ASR + alignment, otherwise by the rough vocal-region heuristic.
// Without either, and with --model: ASR hypothesis (machine transcript).
// The tool never downloads lyrics or models; it only reads files you pass.

#include "bgmedia/vox/VoxLyrics.hpp"
#ifdef BGMEDIA_VOX_LYRICS_WITH_WHISPER
#include "bgmedia/vox/VoxWhisper.hpp"
#endif
#include "support/WavFile.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bgmedia::vox;

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (c < 0x20) {
                o << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<int>(c) << std::dec << std::setfill(' ');
            } else {
                o << static_cast<char>(c);
            }
        }
    }
    return o.str();
}

bool writeJson(const std::string& path, const VoxLyrics& l) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    f << std::fixed << std::setprecision(3);
    f << "{\n  \"status\": \"" << voxLyricsStatusName(l.status) << "\",\n"
      << "  \"detail\": \"" << jsonEscape(l.detail) << "\",\n"
      << "  \"source\": \"" << voxLyricsSourceName(l.source) << "\",\n"
      << "  \"is_original_text\": " << (l.isOriginalText ? "true" : "false") << ",\n"
      << "  \"provider_tag\": \"" << jsonEscape(l.providerTag) << "\",\n"
      << "  \"host_may_cache\": " << (l.hostMayCache ? "true" : "false") << ",\n"
      << "  \"lines\": [\n";
    for (std::size_t i = 0; i < l.lines.size(); ++i) {
        const VoxLyricLine& x = l.lines[i];
        f << "    {\"start\": " << x.startSec << ", \"end\": " << x.endSec
          << ", \"confidence\": " << x.confidence << ", \"source\": \""
          << voxLyricsSourceName(x.source) << "\", \"instrumental\": "
          << (x.instrumental ? "true" : "false") << ", \"interpolated\": "
          << (x.interpolated ? "true" : "false") << ", \"stanza_break\": "
          << (x.stanzaBreak ? "true" : "false") << ", \"text\": \"" << jsonEscape(x.text) << "\"";
        if (!x.words.empty()) {
            f << ", \"words\": [";
            for (std::size_t w = 0; w < x.words.size(); ++w) {
                f << (w ? ", " : "") << "{\"start\": " << x.words[w].startSec
                  << ", \"end\": " << x.words[w].endSec << ", \"text\": \""
                  << jsonEscape(x.words[w].text) << "\"}";
            }
            f << "]";
        }
        f << "}" << (i + 1 < l.lines.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
    return static_cast<bool>(f);
}

int usage() {
    std::cerr << "usage: bgmedia_vox_lyrics <song.wav> (--lyrics text.txt | --lrc file.lrc)\n"
                 "       [--model ggml.bin] [--lang ru] [--gap SECONDS]\n"
                 "       [--out out.lrc] [--json out.json]\n"
                 "  --model needs a build with -DBGMEDIA_VOX_WHISPER=ON\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]).rfind("--", 0) == 0) {
        return usage();
    }
    const std::string input = argv[1];
    std::string lyricsPath, lrcPath, model, outPath, jsonPath;
    VoxBuildOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--lyrics") {
            if (!value(lyricsPath)) return usage();
        } else if (a == "--lrc") {
            if (!value(lrcPath)) return usage();
        } else if (a == "--model") {
            if (!value(model)) return usage();
        } else if (a == "--lang") {
            if (!value(options.asr.language)) return usage();
        } else if (a == "--out") {
            if (!value(outPath)) return usage();
        } else if (a == "--json") {
            if (!value(jsonPath)) return usage();
        } else if (a == "--gap") {
            if (!value(v)) return usage();
            options.instrumentalGapSeconds = std::atof(v.c_str());
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return usage();
        }
    }
    if (!lyricsPath.empty() && !lrcPath.empty()) {
        std::cerr << "error: use either --lyrics or --lrc\n";
        return usage();
    }

    VoxProvidedLyrics provided;
    provided.providerTag = "cli-file";
    provided.hostMayCache = false;
    const std::string& textPath = !lrcPath.empty() ? lrcPath : lyricsPath;
    if (!textPath.empty()) {
        if (!readFile(textPath, provided.text)) {
            std::cerr << "error: cannot read " << textPath << "\n";
            return 2;
        }
        provided.synced = !lrcPath.empty();
    }

    std::unique_ptr<IVoxAsrBackend> backend;
    if (!model.empty()) {
#ifdef BGMEDIA_VOX_LYRICS_WITH_WHISPER
        VoxWhisperConfig cfg;
        cfg.modelPath = model;
        std::string err;
        VoxTranscribeStatus st = VoxTranscribeStatus::InternalError;
        backend = createWhisperBackend(cfg, &err, &st);
        if (!backend) {
            std::cerr << "error: " << voxTranscribeStatusName(st) << ": " << err << "\n";
            return 3;
        }
#else
        std::cerr << "error: --model needs a build with -DBGMEDIA_VOX_WHISPER=ON\n";
        return 3;
#endif
    }

    bgmedia::toolsupport::WavFile wav;
    std::vector<float> pcm;
    std::size_t frames = 0;
    if (!provided.synced) {
        std::string error;
        if (!bgmedia::toolsupport::readWav(input, wav, error)) {
            std::cerr << "error: " << error << "\n";
            return 2;
        }
        pcm.resize(wav.samples.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            pcm[i] = static_cast<float>(wav.samples[i]) / 32768.0f;
        }
        frames = wav.samples.size() / (wav.channels ? wav.channels : 1);
    }

    int lastPercent = -1;
    VoxProgressFn progress = [&](float f) {
        const int p = static_cast<int>(f * 100.0f);
        if (p != lastPercent) {
            lastPercent = p;
            std::cerr << "\rprogress " << p << "%" << std::flush;
        }
    };
    VoxCancelToken cancel;
    const VoxLyrics l = buildLyrics(provided, backend.get(), nullptr,
                                    pcm.empty() ? nullptr : pcm.data(), frames,
                                    VoxPcmFormat{wav.sampleRate, wav.channels}, options, &cancel,
                                    progress);
    std::cerr << "\r" << std::flush;

    std::cout << std::fixed << std::setprecision(2);
    for (const VoxLyricLine& x : l.lines) {
        std::cout << "[" << x.startSec << " -> " << x.endSec << "] conf " << x.confidence
                  << (x.isLowConfidence() ? " LOW " : "     ")
                  << (x.instrumental ? "(instrumental)" : x.text) << "\n";
    }
    std::cout << "status=" << voxLyricsStatusName(l.status) << " source="
              << voxLyricsSourceName(l.source) << " original_text=" << (l.isOriginalText ? 1 : 0)
              << " lines=" << l.lines.size() << " detail=\"" << l.detail << "\"\n";
    if (l.source == VoxLyricsSource::AsrHypothesis) {
        std::cout << "note: recognised automatically, not the original lyrics\n";
    } else if (l.source == VoxLyricsSource::AlignedToAudio) {
        std::cout << "note: times are estimated from the audio, not the provider's sync\n";
    }
    if (!outPath.empty()) {
        std::ofstream f(outPath, std::ios::binary);
        const std::string lrc = toLrc(l, true);
        f << lrc;
        if (!f) {
            std::cerr << "error: cannot write " << outPath << "\n";
            return 2;
        }
    }
    if (!jsonPath.empty() && !writeJson(jsonPath, l)) {
        std::cerr << "error: cannot write " << jsonPath << "\n";
        return 2;
    }
    return l.status == VoxLyricsStatus::Ok ? 0 : 4;
}
