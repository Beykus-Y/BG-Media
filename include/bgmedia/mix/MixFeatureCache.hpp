#pragma once

// Versioned binary cache for BG Mix 0.3 feature maps (.bgf).
// Stored alongside BG Beat (.bgm) caches without altering AnalysisCache's
// schema, so existing BG Beat caches remain readable and independent.

#include "bgmedia/mix/MixFeatureMap.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace bgmedia::mix {

enum class FeatureCacheStatus {
    Hit,
    Miss,
    Corrupt,
    Error
};

struct FeatureCacheLoadResult {
    FeatureCacheStatus status = FeatureCacheStatus::Miss;
    MixFeatureMap features;
    std::string message;

    [[nodiscard]] bool hit() const noexcept {
        return status == FeatureCacheStatus::Hit;
    }
};

class MixFeatureCache {
public:
    explicit MixFeatureCache(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return directory_;
    }

    [[nodiscard]] FeatureCacheLoadResult load(
        std::uint64_t contentHash,
        std::uint64_t analysisConfigHash,
        const MixFeatureConfig& config,
        std::uint32_t expectedSampleRate,
        std::uint32_t expectedSourceChannels,
        double expectedDuration) const;

    [[nodiscard]] FeatureCacheLoadResult load(
        const AudioData& audio,
        const PcmBuffer& pcm,
        const AnalysisResult& analysis,
        const MixFeatureConfig& config) const;

    [[nodiscard]] FeatureCacheLoadResult load(
        const PcmTrack& track,
        const AnalysisResult& analysis,
        const MixFeatureConfig& config) const;

    [[nodiscard]] FeatureCacheLoadResult load(
        const PcmBuffer& pcm,
        const AnalysisResult& analysis,
        const MixFeatureConfig& config) const;

    [[nodiscard]] bool store(const MixFeatureMap& features,
                             const MixFeatureConfig& config,
                             std::string* error = nullptr) const;

    [[nodiscard]] static std::string fileNameFor(
        std::uint64_t contentHash,
        std::uint64_t analysisConfigHash,
        std::uint64_t featureConfigHash);

private:
    std::filesystem::path directory_;
};

} // namespace bgmedia::mix
