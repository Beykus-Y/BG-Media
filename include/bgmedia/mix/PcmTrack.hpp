#pragma once

// Prepared PCM owned by the BG Mix engine. Decoding and format conversion happen
// on a worker thread; the audio callback only reads these buffers.

#include "bgmedia/audio/AudioData.hpp"

#include <cstdint>
#include <vector>

namespace bgmedia::mix {

struct PcmTrack {
    std::vector<float> samples;  // interleaved, nominally [-1, 1]
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t id = 0;

    [[nodiscard]] bool valid() const noexcept {
        return sampleRate > 0 && (channels == 1 || channels == 2) &&
               !samples.empty();
    }
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channels > 0 ? samples.size() / channels : 0;
    }
    [[nodiscard]] double duration() const noexcept {
        return sampleRate > 0
                   ? static_cast<double>(frameCount()) / sampleRate
                   : 0.0;
    }

    // Converts any decoded AudioData to the mixer's working format. Mono stays
    // mono; stereo stays stereo; >2 channels are downmixed to stereo by
    // averaging odd/even groups. The sample rate is converted with a windowed
    // linear interpolator so the audio thread never performs format conversion.
    static PcmTrack fromAudio(const AudioData& audio, std::uint32_t targetRate,
                              std::string* error = nullptr);
};

} // namespace bgmedia::mix
