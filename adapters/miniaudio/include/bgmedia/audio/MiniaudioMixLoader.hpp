#pragma once

// Decode + BG Beat + BG Mix 0.3 feature map preparation for BG Mix tracks.
// Runs on MixSession's worker thread; the audio callback never touches this path.

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/MixSession.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace bgmedia {

struct MixTrackLoadOptions {
    std::filesystem::path cacheDirectory;
    AnalysisConfig analysis;
    mix::MixFeatureConfig features;
    bool useCache = true;
    // Mixer working rate. Audio is resampled once, during preparation.
    std::uint32_t targetSampleRate = 48000;
    // Per-buffer limits for decoded source and mixer output. Hosts with tight
    // memory budgets can reject oversized tracks before analysing them.
    std::size_t maxPcmBytes = 1024ull * 1024ull * 1024ull;
};

using MixLoadProgress =
    std::function<void(float fraction, std::string_view stage)>;

// Decodes a file, analyses it with BG Beat (using the on-disk .bgm cache when
// available), extracts or loads the BG Mix 0.3 feature map (.bgf), and converts
// the audio to the mixer's interleaved stereo float format.
[[nodiscard]] bool prepareMixTrack(const std::filesystem::path& path,
                                   const MixTrackLoadOptions& options,
                                   mix::PreparedMixTrack& out,
                                   std::string& error,
                                   const MixLoadProgress& progress = {});

} // namespace bgmedia
