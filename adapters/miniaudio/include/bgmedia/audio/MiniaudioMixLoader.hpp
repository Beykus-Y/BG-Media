#pragma once

// Decode + BG Beat preparation for BG Mix tracks. Runs on MixSession's worker
// thread; the audio callback never touches this path.

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/mix/MixSession.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace bgmedia {

struct MixTrackLoadOptions {
    std::filesystem::path cacheDirectory;
    AnalysisConfig analysis;
    bool useCache = true;
    // Mixer working rate. Audio is resampled once, during preparation.
    std::uint32_t targetSampleRate = 48000;
};

using MixLoadProgress =
    std::function<void(float fraction, std::string_view stage)>;

// Decodes a file, analyses it with BG Beat (using the on-disk cache when
// available) and converts it to the mixer's interleaved stereo float format.
[[nodiscard]] bool prepareMixTrack(const std::filesystem::path& path,
                                   const MixTrackLoadOptions& options,
                                   mix::PreparedMixTrack& out,
                                   std::string& error,
                                   const MixLoadProgress& progress = {});

} // namespace bgmedia
