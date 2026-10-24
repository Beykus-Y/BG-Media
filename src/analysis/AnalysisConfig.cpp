#include "bgmedia/analysis/AnalysisConfig.hpp"

#include "bgmedia/version.hpp"

#include <cstring>
#include <sstream>

namespace bgmedia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashBytes(std::uint64_t& h, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
}

template <typename T>
void hashValue(std::uint64_t& h, const T& value) noexcept {
    hashBytes(h, &value, sizeof(T));
}

} // namespace

std::uint64_t AnalysisConfig::fingerprint() const noexcept {
    std::uint64_t h = kFnvOffset;
    hashValue(h, kAnalyzerVersion);
    hashValue(h, minBpm);
    hashValue(h, maxBpm);
    hashValue(h, preferredBpm);
    hashValue(h, stftSize);
    hashValue(h, hopSize);
    hashValue(h, melBands);
    hashValue(h, onsetSensitivity);
    hashValue(h, bassAccentSensitivity);
    hashValue(h, minSilenceSeconds);
    hashValue(h, silenceThresholdDb);
    hashValue(h, beatsPerBarHint);
    hashValue(h, detectTempoChanges);
    hashValue(h, tempoWindowSeconds);
    hashValue(h, tempoHopSeconds);
    return h;
}

std::string AnalysisConfig::toJson() const {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(4);
    out << "{\"minBpm\":" << minBpm
        << ",\"maxBpm\":" << maxBpm
        << ",\"preferredBpm\":" << preferredBpm
        << ",\"stftSize\":" << stftSize
        << ",\"hopSize\":" << hopSize
        << ",\"melBands\":" << melBands
        << ",\"onsetSensitivity\":" << onsetSensitivity
        << ",\"bassAccentSensitivity\":" << bassAccentSensitivity
        << ",\"minSilenceSeconds\":" << minSilenceSeconds
        << ",\"silenceThresholdDb\":" << silenceThresholdDb
        << ",\"beatsPerBarHint\":" << beatsPerBarHint
        << ",\"detectTempoChanges\":" << (detectTempoChanges ? "true" : "false")
        << ",\"tempoWindowSeconds\":" << tempoWindowSeconds
        << ",\"tempoHopSeconds\":" << tempoHopSeconds
        << "}";
    return out.str();
}

} // namespace bgmedia
