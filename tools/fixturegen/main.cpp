#include "support/PathUtf8.hpp"
#include "support/SyntheticSignals.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using bgmedia::test::SyntheticTrack;

bool writeTrack(const std::filesystem::path& directory, const std::string& name,
                const SyntheticTrack& track) {
    const std::filesystem::path path = directory / name;
    std::string error;
    if (!bgmedia::test::writeWav16(path, track.audio, &error)) {
        std::cerr << "failed to write " << path.string() << ": " << error << '\n';
        return false;
    }
    std::cout << "wrote " << path.string() << "  (" << std::fixed << std::setprecision(2)
              << track.audio.duration() << " s, " << track.audio.sampleRate << " Hz, "
              << track.audio.channels << " ch)\n";
    return true;
}

void printUsage() {
    std::cout
        << "bgmedia_fixturegen - deterministic synthetic test material\n\n"
        << "Usage: bgmedia_fixturegen [output-directory] [--long]\n\n"
        << "Writes WAV files for 92/123/163 BPM, tempo change, silence,\n"
        << "bass accents and an ambiguous polyrhythm. --long also writes a\n"
        << "5-minute 92 BPM track for performance measurements.\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    bgmedia::test::Utf8CommandLine arguments(argc, argv);
    std::filesystem::path outputDirectory = "fixtures";
    bool longTrack = false;
    for (int i = 1; i < arguments.count; ++i) {
        const std::string arg = arguments.values[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--long") {
            longTrack = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << '\n';
            return 2;
        } else {
            outputDirectory = bgmedia::test::pathFromUtf8(arg);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        std::cerr << "cannot create output directory: " << ec.message() << '\n';
        return 2;
    }

    using namespace bgmedia::test;
    bool ok = true;

    TrackOptions options92;
    options92.bpm = 92.0;
    options92.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_092.wav", makeClickTrack(options92));

    TrackOptions options123;
    options123.bpm = 123.0;
    options123.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_123.wav", makeClickTrack(options123));

    TrackOptions options163;
    options163.bpm = 163.0;
    options163.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_163.wav", makeClickTrack(options163));

    TrackOptions options060;
    options060.bpm = 60.0;
    options060.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_060.wav", makeClickTrack(options060));

    TrackOptions options118;
    options118.bpm = 118.0;
    options118.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_118.wav", makeClickTrack(options118));

    TrackOptions options120;
    options120.bpm = 120.0;
    options120.duration = 24.0;
    ok &= writeTrack(outputDirectory, "beat_120.wav", makeClickTrack(options120));

    ok &= writeTrack(outputDirectory, "tempo_092_123.wav",
                     makeTempoChangeTrack(92.0, 123.0, 12.0, 24.0));

    ok &= writeTrack(outputDirectory, "silence_gap.wav", makeSilenceTrack());
    ok &= writeTrack(outputDirectory, "fully_silent.wav", makeFullySilentTrack(6.0));
    ok &= writeTrack(outputDirectory, "bass_accents.wav", makeBassAccentTrack());
    ok &= writeTrack(outputDirectory, "ambiguous.wav", makeAmbiguousTrack());
    ok &= writeTrack(outputDirectory, "offbeat_100.wav", makeOffbeatTrack(100.0, 24.0));
    ok &= writeTrack(outputDirectory, "ternary_66.wav", makeTernaryTrack(66.2, 24.0));
    ok &= writeTrack(outputDirectory, "halftime_80.wav", makeHalfTimeTrack(80.0, 24.0));
    ok &= writeTrack(outputDirectory, "shuffle_80.wav", makeShuffleTrack(80.0, 24.0));
    ok &= writeTrack(outputDirectory, "dense_hats_99.wav", makeDenseHatTrack(99.0, 24.0));
    ok &= writeTrack(outputDirectory, "syncopated_bass_100.wav",
                     makeSyncopatedBassTrack(100.0, 24.0));
    ok &= writeTrack(outputDirectory, "drift_80.wav", makeDriftTrack(80.0, 24.0));

    if (longTrack) {
        TrackOptions longOptions;
        longOptions.bpm = 118.0;
        longOptions.duration = 300.0;
        ok &= writeTrack(outputDirectory, "long_118_5min.wav",
                         makeClickTrack(longOptions));
    }

    if (!ok) {
        return 2;
    }
    std::cout << "\nSynthetic fixtures generated. These are original test signals,\n"
                 "not third-party music.\n";
    return 0;
}
