#include "bgmedia/mix/PcmTrack.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bgmedia::mix {
namespace {

float sanitize(float value) {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}

float readChannel(const std::vector<float>& samples, std::size_t frame,
                  std::uint32_t channels, std::uint32_t channel) {
    return samples[frame * channels + channel];
}

} // namespace

PcmTrack PcmTrack::fromAudio(const AudioData& audio, std::uint32_t targetRate,
                             std::string* error) {
    PcmTrack track;
    if (!audio.valid() || audio.sampleRate == 0 || audio.channels == 0) {
        if (error) {
            *error = "decoded audio is empty";
        }
        return track;
    }
    if (targetRate == 0) {
        targetRate = audio.sampleRate;
    }

    const std::uint32_t sourceChannels = audio.channels;
    const std::uint32_t outChannels = sourceChannels == 1 ? 1u : 2u;
    const double sourceFrames = static_cast<double>(audio.frameCount());
    if (sourceFrames < 2.0) {
        if (error) {
            *error = "decoded audio is too short";
        }
        return track;
    }
    const auto channelAt = [&](std::size_t frame, std::uint32_t channel) {
        if (sourceChannels == 1) {
            return sanitize(readChannel(audio.samples, frame, sourceChannels, 0));
        }
        if (sourceChannels == 2) {
            return sanitize(readChannel(audio.samples, frame, sourceChannels, channel));
        }
        // Fold arbitrary multichannel material into L/R by averaging the even
        // and odd channel groups. Mono compatibility is preferred over picking
        // an arbitrary pair.
        float sum = 0.0f;
        int count = 0;
        for (std::uint32_t c = channel; c < sourceChannels; c += 2) {
            sum += sanitize(readChannel(audio.samples, frame, sourceChannels, c));
            ++count;
        }
        return count > 0 ? sum / static_cast<float>(count) : 0.0f;
    };

    const std::size_t srcFrames = static_cast<std::size_t>(sourceFrames);
    track.samples.resize(srcFrames * outChannels);
    track.sampleRate = audio.sampleRate;
    track.channels = outChannels;
    for (std::size_t f = 0; f < srcFrames; ++f) {
        for (std::uint32_t c = 0; c < outChannels; ++c) {
            track.samples[f * outChannels + c] = channelAt(f, c);
        }
    }
    if (targetRate == track.sampleRate) {
        return track;
    }
    return resampled(track, targetRate);
}

PcmTrack PcmTrack::fromAudio(AudioData&& audio, std::uint32_t targetRate,
                             std::string* error, const std::function<void()>& checkpoint) {
    if (!audio.valid() || audio.frameCount() < 2) {
        return fromAudio(static_cast<const AudioData&>(audio), targetRate, error);
    }
    if (checkpoint) checkpoint();
    PcmTrack track;
    track.sampleRate = audio.sampleRate;
    const auto sourceChannels = audio.channels;
    const auto frames = audio.frameCount();
    track.channels = std::min(sourceChannels, 2u);
    track.samples = std::move(audio.samples);
    if (sourceChannels <= 2) {
        for (std::size_t i = 0; i < track.samples.size(); ++i) {
            if (checkpoint && (i % 65536 == 0)) checkpoint();
            track.samples[i] = sanitize(track.samples[i]);
        }
    } else {
        // Read both channel groups before overwriting the current frame.
        // The two-channel output never overwrites input for a later frame.
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (checkpoint && (frame % 32768 == 0)) checkpoint();
            float stereo[2]{};
            int counts[2]{};
            for (std::uint32_t c = 0; c < sourceChannels; ++c) {
                stereo[c % 2] += sanitize(track.samples[frame * sourceChannels + c]);
                ++counts[c % 2];
            }
            for (std::uint32_t c = 0; c < 2; ++c) {
                track.samples[frame * 2 + c] = stereo[c] / static_cast<float>(counts[c]);
            }
        }
        track.samples.resize(frames * 2);
    }
    if (checkpoint) checkpoint();
    if (targetRate == 0 || targetRate == track.sampleRate) return track;
    return resampled(track, targetRate, checkpoint);
}

PcmTrack PcmTrack::resampled(const PcmTrack& source, std::uint32_t targetRate,
                              const std::function<void()>& checkpoint) {
    if (checkpoint) checkpoint();
    if (targetRate == 0 || source.sampleRate == 0 || source.channels == 0 ||
        targetRate == source.sampleRate || source.samples.empty()) {
        return source;
    }
    PcmTrack track;
    const std::uint32_t outChannels = source.channels;
    const std::size_t srcFrames = source.frameCount();
    const double ratio = static_cast<double>(targetRate) / source.sampleRate;
    const std::size_t outFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(static_cast<double>(srcFrames) * ratio)));
    track.samples.resize(outFrames * outChannels);
    track.sampleRate = targetRate;
    track.channels = outChannels;

    // Band-limited resampling: Kaiser-windowed sinc, polyphase table with
    // linear interpolation between phases. The cutoff drops to the target
    // Nyquist when downsampling so nothing aliases.
    constexpr int kHalfTaps = 24;
    constexpr int kPhases = 512;
    const double cutoff = std::min(1.0, ratio) * 0.97;
    const double kaiserBeta = 9.0;
    const auto bessel0 = [](double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    const double i0Beta = bessel0(kaiserBeta);
    const double support = kHalfTaps / cutoff;
    const int halfWidth = static_cast<int>(std::ceil(support));
    const int width = halfWidth * 2;
    const double kPi = 3.14159265358979323846;
    // table[phase][tap]: tap t reads source index base - halfWidth + 1 + t.
    std::vector<float> table(static_cast<std::size_t>(kPhases + 1) * width);
    for (int ph = 0; ph <= kPhases; ++ph) {
        if (checkpoint && (ph % 16 == 0)) checkpoint();
        const double frac = static_cast<double>(ph) / kPhases;
        double sum = 0.0;
        for (int t = 0; t < width; ++t) {
            const double d = (t - halfWidth + 1) - frac;
            const double x = d * cutoff;
            const double sinc = std::fabs(x) < 1e-12
                                    ? 1.0
                                    : std::sin(kPi * x) / (kPi * x);
            const double u = d / support;
            const double w = std::fabs(u) >= 1.0
                                 ? 0.0
                                 : bessel0(kaiserBeta * std::sqrt(1.0 - u * u)) / i0Beta;
            const double v = sinc * w * cutoff;
            table[static_cast<std::size_t>(ph) * width + t] = static_cast<float>(v);
            sum += v;
        }
        if (sum > 0.0) {
            for (int t = 0; t < width; ++t) {
                table[static_cast<std::size_t>(ph) * width + t] /= static_cast<float>(sum);
            }
        }
    }

    const double step = 1.0 / ratio;
    for (std::size_t i = 0; i < outFrames; ++i) {
        if (checkpoint && (i % 4096 == 0)) checkpoint();
        const double pos = static_cast<double>(i) * step;
        const auto base = static_cast<long long>(std::floor(pos));
        const double fracPos = (pos - static_cast<double>(base)) * kPhases;
        const int ph = std::min(kPhases - 1, static_cast<int>(fracPos));
        const float mix = static_cast<float>(fracPos - ph);
        const float* t0 = &table[static_cast<std::size_t>(ph) * width];
        const float* t1 = t0 + width;
        const long long first = base - halfWidth + 1;
        for (std::uint32_t c = 0; c < outChannels; ++c) {
            float acc = 0.0f;
            for (int t = 0; t < width; ++t) {
                const long long idx = first + t;
                if (idx < 0 || idx >= static_cast<long long>(srcFrames)) {
                    continue;
                }
                acc += sanitize(source.samples[static_cast<std::size_t>(idx) * outChannels + c]) * (t0[t] + (t1[t] - t0[t]) * mix);
            }
            track.samples[i * outChannels + c] = std::clamp(acc, -1.0f, 1.0f);
        }
    }
    return track;
}

} // namespace bgmedia::mix
