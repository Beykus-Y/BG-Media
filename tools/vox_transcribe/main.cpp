// bgmedia_vox_transcribe: offline speech-to-text CLI for BG Vox Transcribe.
// Built only with -DBGMEDIA_VOX_WHISPER=ON (it needs an ASR backend).
//
//   bgmedia_vox_transcribe in.wav --model path/to/ggml-tiny.bin
//       [--lang ru] [--words] [--translate] [--threads N] [--chunk SECONDS]
//       [--json out.json] [--srt out.srt]
//
// Reads 16-bit PCM WAV (mono or stereo, 8000..48000 Hz), prints one line per
// segment plus a summary, and optionally writes JSON and SRT. The library never
// downloads a model: pass a file the host already has.

#include "bgmedia/vox/VoxTranscribe.hpp"
#include "bgmedia/vox/VoxWhisper.hpp"
#include "support/WavFile.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bgmedia::vox;
using bgmedia::toolsupport::readWav;
using bgmedia::toolsupport::WavFile;

namespace {

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

std::string srtTime(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const long long ms = static_cast<long long>(seconds * 1000.0 + 0.5);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", ms / 3600000,
                  (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000);
    return buf;
}

bool writeJson(const std::string& path, const VoxTranscript& t) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    f << std::fixed << std::setprecision(3);
    f << "{\n  \"status\": \"" << voxTranscribeStatusName(t.status) << "\",\n";
    f << "  \"detail\": \"" << jsonEscape(t.detail) << "\",\n";
    f << "  \"machine_generated\": true,\n";
    f << "  \"backend\": \"" << jsonEscape(t.backendName) << "\",\n";
    f << "  \"model\": \"" << jsonEscape(t.modelName) << "\",\n";
    f << "  \"language\": \"" << jsonEscape(t.detectedLanguage) << "\",\n";
    f << "  \"audio_seconds\": " << t.audioSeconds << ",\n";
    f << "  \"processing_ms\": " << t.processingMs << ",\n";
    f << "  \"segments\": [\n";
    for (std::size_t i = 0; i < t.segments.size(); ++i) {
        const VoxSegment& s = t.segments[i];
        f << "    {\"start\": " << s.startSec << ", \"end\": " << s.endSec
          << ", \"confidence\": " << s.confidence
          << ", \"low_confidence\": " << (s.lowConfidence ? "true" : "false")
          << ", \"language\": \"" << jsonEscape(s.language) << "\""
          << ", \"text\": \"" << jsonEscape(s.text) << "\"";
        if (!s.words.empty()) {
            f << ", \"words\": [";
            for (std::size_t w = 0; w < s.words.size(); ++w) {
                const VoxWord& word = s.words[w];
                f << (w ? ", " : "") << "{\"start\": " << word.startSec << ", \"end\": "
                  << word.endSec << ", \"confidence\": " << word.confidence << ", \"text\": \""
                  << jsonEscape(word.text) << "\"}";
            }
            f << "]";
        }
        f << "}" << (i + 1 < t.segments.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
    return static_cast<bool>(f);
}

// A blank line ends an SRT cue, so collapse any run of line breaks in the text.
std::string srtText(const std::string& text) {
    std::string out;
    bool pendingBreak = false;
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            pendingBreak = true;
            continue;
        }
        if (pendingBreak && !out.empty()) {
            out += '\n';
        }
        pendingBreak = false;
        out += ch;
    }
    return out;
}

bool writeSrt(const std::string& path, const VoxTranscript& t) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    int index = 0;
    for (const VoxSegment& s : t.segments) {
        f << ++index << "\n"
          << srtTime(s.startSec) << " --> " << srtTime(s.endSec) << "\n"
          << srtText(s.text) << "\n\n";
    }
    return static_cast<bool>(f);
}

int usage() {
    std::cerr << "usage: bgmedia_vox_transcribe <in.wav> --model <ggml-model.bin>\n"
                 "       [--lang ru] [--words] [--translate] [--threads N]\n"
                 "       [--chunk SECONDS] [--json out.json] [--srt out.srt]\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string input = argv[1];
    if (input.rfind("--", 0) == 0) {
        return usage();
    }
    std::string model;
    std::string jsonOut;
    std::string srtOut;
    VoxTranscribeOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (a == "--model") {
            if (!value(model)) return usage();
        } else if (a == "--lang") {
            if (!value(options.language)) return usage();
        } else if (a == "--json") {
            if (!value(jsonOut)) return usage();
        } else if (a == "--srt") {
            if (!value(srtOut)) return usage();
        } else if (a == "--prompt") {
            if (!value(options.initialPrompt)) return usage();
        } else if (a == "--threads") {
            std::string v;
            if (!value(v)) return usage();
            options.threads = std::atoi(v.c_str());
        } else if (a == "--chunk") {
            std::string v;
            if (!value(v)) return usage();
            options.chunkSeconds = std::atof(v.c_str());
        } else if (a == "--words") {
            options.wordTimestamps = true;
        } else if (a == "--translate") {
            options.translateToEnglish = true;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return usage();
        }
    }
    if (model.empty()) {
        std::cerr << "error: --model is required (the library never downloads models)\n";
        return usage();
    }

    WavFile wav;
    std::string error;
    if (!readWav(input, wav, error)) {
        std::cerr << "error: " << error << "\n";
        return 2;
    }

    VoxWhisperConfig cfg;
    cfg.modelPath = model;
    cfg.threads = options.threads;
    std::string loadError;
    VoxTranscribeStatus loadStatus = VoxTranscribeStatus::InternalError;
    auto backend = createWhisperBackend(cfg, &loadError, &loadStatus);
    if (!backend) {
        std::cerr << "error: " << voxTranscribeStatusName(loadStatus) << ": " << loadError << "\n";
        return 3;
    }
    const VoxAsrBackendInfo info = backend->info();
    std::cerr << "model " << info.modelName << ": file "
              << info.modelFileBytes / (1024 * 1024) << " MiB, approx RAM "
              << info.approxRamBytes / (1024 * 1024) << " MiB, multilingual "
              << (info.multilingual ? "yes" : "no") << "\n";

    VoxCancelToken cancel;
    int lastPercent = -1;
    VoxProgressFn progress = [&](float fraction) {
        const int percent = static_cast<int>(fraction * 100.0f);
        if (percent != lastPercent) {
            lastPercent = percent;
            std::cerr << "\rprogress " << percent << "%" << std::flush;
        }
    };
    const std::size_t frames = wav.samples.size() / (wav.channels ? wav.channels : 1);
    const VoxTranscript t =
        transcribe(backend.get(), wav.samples.data(), frames,
                   VoxPcmFormat{wav.sampleRate, wav.channels}, options, &cancel, progress);
    std::cerr << "\r" << std::flush;

    std::cout << std::fixed << std::setprecision(2);
    for (const VoxSegment& s : t.segments) {
        std::cout << "[" << s.startSec << " -> " << s.endSec << "] conf " << s.confidence
                  << (s.lowConfidence ? " LOW " : "     ") << s.text << "\n";
    }
    const double rtf = t.processingMs > 0.0 ? t.audioSeconds * 1000.0 / t.processingMs : 0.0;
    std::cout << "status=" << voxTranscribeStatusName(t.status)
              << " detail=\"" << t.detail << "\""
              << " language=" << (t.detectedLanguage.empty() ? "?" : t.detectedLanguage)
              << " segments=" << t.segments.size() << " audio_s=" << t.audioSeconds
              << " processing_ms=" << t.processingMs << " xRT=" << rtf
              << " machine_generated=1\n";

    if (!jsonOut.empty() && !writeJson(jsonOut, t)) {
        std::cerr << "error: cannot write " << jsonOut << "\n";
        return 2;
    }
    if (!srtOut.empty() && !writeSrt(srtOut, t)) {
        std::cerr << "error: cannot write " << srtOut << "\n";
        return 2;
    }
    return t.status == VoxTranscribeStatus::Ok ? 0 : 4;
}
