#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia::test {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kSampleRate = 44100;

class Noise {
public:
    explicit Noise(std::uint32_t seed) : state_(seed == 0 ? 0x9E3779B9u : seed) {}

    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(static_cast<std::int32_t>(state_)) /
               static_cast<float>(std::numeric_limits<std::int32_t>::max());
    }

private:
    std::uint32_t state_;
};

void addKick(std::vector<float>& samples, std::uint32_t sampleRate, double start,
             float amplitude, Noise& noise) {
    const int begin = static_cast<int>(std::max(0.0, start * sampleRate));
    const int length = static_cast<int>(0.36 * sampleRate);
    double phase = 0.0;
    for (int i = 0; i < length && begin + i < static_cast<int>(samples.size()); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double frequency = 46.0 + 105.0 * std::exp(-t * 32.0);
        phase += 2.0 * kPi * frequency / sampleRate;
        const double envelope = std::exp(-t * 10.5);
        float value = static_cast<float>(std::sin(phase) * envelope * amplitude);
        if (t < 0.006) {
            value += noise.next() * static_cast<float>(amplitude * 0.25 * (1.0 - t / 0.006));
        }
        samples[static_cast<std::size_t>(begin + i)] += value;
    }
}

void addSnare(std::vector<float>& samples, std::uint32_t sampleRate, double start,
              float amplitude, Noise& noise) {
    const int begin = static_cast<int>(std::max(0.0, start * sampleRate));
    const int length = static_cast<int>(0.22 * sampleRate);
    for (int i = 0; i < length && begin + i < static_cast<int>(samples.size()); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double envelope = std::exp(-t * 24.0);
        const double tone = std::sin(2.0 * kPi * 185.0 * t) * 0.35;
        const double hiss = static_cast<double>(noise.next()) * 0.85;
        samples[static_cast<std::size_t>(begin + i)] +=
            static_cast<float>((tone + hiss) * envelope * amplitude);
    }
}

void addHat(std::vector<float>& samples, std::uint32_t sampleRate, double start,
            float amplitude, Noise& noise) {
    const int begin = static_cast<int>(std::max(0.0, start * sampleRate));
    const int length = static_cast<int>(0.055 * sampleRate);
    for (int i = 0; i < length && begin + i < static_cast<int>(samples.size()); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double envelope = std::exp(-t * 70.0);
        samples[static_cast<std::size_t>(begin + i)] +=
            noise.next() * static_cast<float>(envelope * amplitude);
    }
}

void addBassNote(std::vector<float>& samples, std::uint32_t sampleRate, double start,
                 double duration, float amplitude) {
    const int begin = static_cast<int>(std::max(0.0, start * sampleRate));
    const int length = static_cast<int>(duration * sampleRate);
    for (int i = 0; i < length && begin + i < static_cast<int>(samples.size()); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double attack = std::min(1.0, t / 0.02);
        const double release = std::exp(-t * 1.3);
        const double envelope = attack * release;
        const double value =
            std::sin(2.0 * kPi * 55.0 * t) * 0.75 +
            std::sin(2.0 * kPi * 82.5 * t) * 0.25;
        samples[static_cast<std::size_t>(begin + i)] +=
            static_cast<float>(value * envelope * amplitude);
    }
}

// High-frequency noise swell: loud, but intentionally not a bass accent.
void addBroadbandSwell(std::vector<float>& samples, std::uint32_t sampleRate,
                       double start, float amplitude, Noise& noise) {
    const int begin = static_cast<int>(std::max(0.0, start * sampleRate));
    const int length = static_cast<int>(0.45 * sampleRate);
    for (int i = 0; i < length && begin + i < static_cast<int>(samples.size()); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double envelope = std::sin(std::min(1.0, t / 0.45) * kPi);
        const double high = std::sin(2.0 * kPi * 1150.0 * t) * 0.3 +
                            std::sin(2.0 * kPi * 1720.0 * t) * 0.2 +
                            static_cast<double>(noise.next()) * 0.4;
        samples[static_cast<std::size_t>(begin + i)] +=
            static_cast<float>(high * envelope * amplitude);
    }
}

void addBeatCluster(std::vector<float>& samples, double time, double bpm,
                    int beatInBar, bool kick, bool snare, bool hat, bool bass,
                    Noise& noise, float kickGain = 1.0f) {
    if (kick) {
        addKick(samples, kSampleRate, time, 0.82f * kickGain, noise);
    }
    if (snare && (beatInBar == 1 || beatInBar == 3)) {
        addSnare(samples, kSampleRate, time, 0.38f, noise);
    }
    if (hat) {
        addHat(samples, kSampleRate, time, 0.14f, noise);
        addHat(samples, kSampleRate, time + 30.0 / bpm, 0.09f, noise);
    }
    if (bass) {
        addBassNote(samples, kSampleRate, time, std::min(0.42, 45.0 / bpm), 0.22f);
    }
}

void finish(std::vector<float>& samples, double duration) {
    const std::size_t total = static_cast<std::size_t>(duration * kSampleRate);
    samples.resize(total, 0.0f);

    float peak = 0.0f;
    for (float value : samples) {
        if (std::isfinite(value)) {
            peak = std::max(peak, std::abs(value));
        }
    }
    const float gain = peak > 1e-6f ? 0.88f / peak : 1.0f;
    const int fade = static_cast<int>(0.01 * kSampleRate);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] *= gain;
        if (i < static_cast<std::size_t>(fade)) {
            samples[i] *= static_cast<float>(i) / fade;
        }
        if (i + static_cast<std::size_t>(fade) >= samples.size()) {
            const std::size_t remaining = samples.size() - i;
            samples[i] *= static_cast<float>(remaining) / fade;
        }
    }
}

AudioData monoAudio(std::vector<float> samples) {
    AudioData audio;
    audio.samples = std::move(samples);
    audio.channels = 1;
    audio.sampleRate = kSampleRate;
    return audio;
}

} // namespace

SyntheticTrack makeClickTrack(const TrackOptions& options) {
    SyntheticTrack track;
    track.bpm = options.bpm;
    const double period = 60.0 / options.bpm;
    const int totalSamples = static_cast<int>(options.duration * kSampleRate);
    std::vector<float> samples(static_cast<std::size_t>(totalSamples), 0.0f);
    Noise noise(0xB6D1A2u);

    int beatIndex = 0;
    for (double time = options.beatOffset; time < options.duration - 0.1;
         time += period, ++beatIndex) {
        const int beatInBar = beatIndex % std::max(1, options.beatsPerBar);
        track.beatTimes.push_back(time);
        if (beatInBar == 0) {
            track.downbeatTimes.push_back(time);
        }
        if (options.kickEveryBeat || beatInBar == 0) {
            track.kickTimes.push_back(time);
        }
        const float gain = beatInBar == 0 ? 1.0f : 0.78f;
        const bool addKickHere = options.kickEveryBeat || beatInBar == 0;
        addBeatCluster(samples, time, options.bpm, beatInBar, addKickHere,
                       options.snare, options.hats, options.bass, noise, gain);
        if (options.broadbandSwells && beatInBar == 2) {
            const double swellTime = time + period * 0.5;
            if (swellTime < options.duration - 0.2) {
                track.swellTimes.push_back(swellTime);
                addBroadbandSwell(samples, kSampleRate, swellTime, 0.55f, noise);
            }
        }
    }

    if (options.silenceStart >= 0.0 && options.silenceEnd > options.silenceStart) {
        const int a = static_cast<int>(options.silenceStart * kSampleRate);
        const int b = std::min(totalSamples, static_cast<int>(options.silenceEnd * kSampleRate));
        std::fill(samples.begin() + a, samples.begin() + b, 0.0f);
        track.silenceRegions.emplace_back(options.silenceStart, options.silenceEnd);
        track.beatTimes.erase(
            std::remove_if(track.beatTimes.begin(), track.beatTimes.end(),
                           [&](double t) {
                               return t >= options.silenceStart + 0.05 &&
                                      t <= options.silenceEnd - 0.05;
                           }),
            track.beatTimes.end());
        track.kickTimes.erase(
            std::remove_if(track.kickTimes.begin(), track.kickTimes.end(),
                           [&](double t) {
                               return t >= options.silenceStart + 0.05 &&
                                      t <= options.silenceEnd - 0.05;
                           }),
            track.kickTimes.end());
    }

    finish(samples, options.duration);
    track.audio = monoAudio(std::move(samples));
    return track;
}

SyntheticTrack makeTempoChangeTrack(double bpmA, double bpmB,
                                    double splitSeconds, double duration) {
    SyntheticTrack track;
    const int totalSamples = static_cast<int>(duration * kSampleRate);
    std::vector<float> samples(static_cast<std::size_t>(totalSamples), 0.0f);
    Noise noise(0x51A2C3u);

    track.tempoChanges.push_back({0.0, bpmA});
    track.tempoChanges.push_back({splitSeconds, bpmB});

    int beatIndex = 0;
    const double periodA = 60.0 / bpmA;
    for (double time = 0.45; time < splitSeconds - 0.05; time += periodA, ++beatIndex) {
        const int beatInBar = beatIndex % 4;
        track.beatTimes.push_back(time);
        track.kickTimes.push_back(time);
        if (beatInBar == 0) {
            track.downbeatTimes.push_back(time);
        }
        addBeatCluster(samples, time, bpmA, beatInBar, true, true, true, true, noise,
                       beatInBar == 0 ? 1.0f : 0.8f);
    }

    beatIndex = 0;
    const double periodB = 60.0 / bpmB;
    const double startB = splitSeconds + 0.18;
    for (double time = startB; time < duration - 0.1; time += periodB, ++beatIndex) {
        const int beatInBar = beatIndex % 4;
        track.beatTimes.push_back(time);
        track.kickTimes.push_back(time);
        if (beatInBar == 0) {
            track.downbeatTimes.push_back(time);
        }
        addBeatCluster(samples, time, bpmB, beatInBar, true, true, true, true, noise,
                       beatInBar == 0 ? 1.0f : 0.8f);
    }

    finish(samples, duration);
    track.bpm = bpmB;
    track.audio = monoAudio(std::move(samples));
    return track;
}

SyntheticTrack makeSilenceTrack() {
    TrackOptions options;
    options.bpm = 92.0;
    options.duration = 13.0;
    options.silenceStart = 4.0;
    options.silenceEnd = 9.0;
    return makeClickTrack(options);
}

SyntheticTrack makeFullySilentTrack(double duration) {
    SyntheticTrack track;
    track.bpm = 0.0;
    std::vector<float> samples(static_cast<std::size_t>(duration * kSampleRate), 0.0f);
    track.audio = monoAudio(std::move(samples));
    return track;
}

SyntheticTrack makeBassAccentTrack() {
    TrackOptions options;
    options.bpm = 92.0;
    options.duration = 20.0;
    options.kickEveryBeat = false; // kicks on downbeats only
    options.snare = true;
    options.hats = true;
    options.bass = true;
    options.broadbandSwells = true;
    return makeClickTrack(options);
}

SyntheticTrack makeAmbiguousTrack() {
    // Two pulse layers a fifth apart in tempo (92 and 138 BPM) with no clear
    // backbeat or downbeat. A detector should expose both candidates and keep
    // confidence low instead of claiming a definitive tempo.
    SyntheticTrack track;
    const double duration = 20.0;
    std::vector<float> samples(static_cast<std::size_t>(duration * kSampleRate), 0.0f);
    Noise noise(0x9922AAu);

    for (double time = 0.45; time < duration - 0.1; time += 60.0 / 92.0) {
        track.beatTimes.push_back(time);
        addKick(samples, kSampleRate, time, 0.42f, noise);
        addHat(samples, kSampleRate, time, 0.08f, noise);
    }
    for (double time = 0.75; time < duration - 0.1; time += 60.0 / 138.0) {
        addSnare(samples, kSampleRate, time, 0.22f, noise);
        addHat(samples, kSampleRate, time, 0.07f, noise);
    }
    finish(samples, duration);
    track.bpm = 0.0;
    track.audio = monoAudio(std::move(samples));
    return track;
}

bool writeWav16(const std::filesystem::path& path, const AudioData& audio,
                std::string* error) {
    if (!audio.valid()) {
        if (error) {
            *error = "invalid audio data";
        }
        return false;
    }
    const std::uint16_t channels = static_cast<std::uint16_t>(audio.channels);
    if (channels == 0 || channels > 32) {
        if (error) {
            *error = "unsupported channel count";
        }
        return false;
    }
    const std::uint16_t bits = 16;
    const std::uint32_t byteRate = audio.sampleRate * channels * bits / 8;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * bits / 8);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(audio.samples.size() * sizeof(std::int16_t));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot open output file";
        }
        return false;
    }

    auto u16 = [&](std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
    };
    auto u32 = [&](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xFF));
        }
    };

    out.write("RIFF", 4);
    u32(36u + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(channels);
    u32(audio.sampleRate);
    u32(byteRate);
    u16(blockAlign);
    u16(bits);
    out.write("data", 4);
    u32(dataBytes);

    std::vector<std::int16_t> pcm(audio.samples.size());
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        float value = audio.samples[i];
        if (!std::isfinite(value)) {
            value = 0.0f;
        }
        value = std::clamp(value, -1.0f, 1.0f);
        pcm[i] = static_cast<std::int16_t>(std::lround(value * 32767.0f));
    }
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
    out.flush();
    if (!out) {
        if (error) {
            *error = "failed to write WAV file";
        }
        return false;
    }
    return true;
}

} // namespace bgmedia::test
