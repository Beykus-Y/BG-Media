#include "bgmedia/mix/PcmTrack.hpp"

#include <algorithm>
#include <cmath>

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
    const double ratio = static_cast<double>(targetRate) / audio.sampleRate;
    const std::size_t outFrames =
        std::max<std::size_t>(1, static_cast<std::size_t>(
                                     std::llround(sourceFrames * ratio)));

    track.samples.resize(outFrames * outChannels);
    track.sampleRate = targetRate;
    track.channels = outChannels;

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

    for (std::size_t i = 0; i < outFrames; ++i) {
        const double sourcePosition =
            static_cast<double>(i) / std::max(1e-9, ratio);
        const auto index = static_cast<std::size_t>(std::floor(sourcePosition));
        const auto next = std::min(index + 1, static_cast<std::size_t>(sourceFrames) - 1);
        const float frac =
            static_cast<float>(sourcePosition - static_cast<double>(index));
        for (std::uint32_t c = 0; c < outChannels; ++c) {
            const float a = channelAt(index, c);
            const float b = channelAt(next, c);
            track.samples[i * outChannels + c] = a + (b - a) * frac;
        }
    }
    return track;
}

} // namespace bgmedia::mix
