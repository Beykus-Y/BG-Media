#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/audio/AudioData.hpp"

#include <filesystem>
#include <string>

namespace bgmedia {

enum class CacheStatus {
    Hit,
    Miss,
    Corrupt,
    Error
};

struct CacheLoadResult {
    CacheStatus status = CacheStatus::Miss;
    AnalysisResult result;
    std::string message;

    [[nodiscard]] bool hit() const noexcept { return status == CacheStatus::Hit; }
};

// Versioned binary cache. Keys are content hash + analysis configuration
// fingerprint; source paths are never part of the key. Writes are atomic
// (temporary file + rename) and every load validates structure, hashes and
// value ranges, so a truncated/corrupt file is a miss, never a crash.
class AnalysisCache {
public:
    explicit AnalysisCache(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }

    [[nodiscard]] CacheLoadResult load(const PcmBuffer& pcm, const AnalysisConfig& config) const;

    [[nodiscard]] bool store(const AnalysisResult& result, const PcmBuffer& pcm,
                             const AnalysisConfig& config, std::string* error = nullptr) const;

    [[nodiscard]] static std::string fileNameFor(std::uint64_t contentHash,
                                                 std::uint64_t configHash);

private:
    std::filesystem::path directory_;
};

} // namespace bgmedia
