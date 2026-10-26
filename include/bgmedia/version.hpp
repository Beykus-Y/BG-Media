#pragma once

#include <cstdint>

// Public project version. Analyzer and schema versions are deliberately
// separate: cache files may stay readable across UI releases, but any change
// to analysis semantics must bump kAnalyzerVersion.
#define BGMEDIA_VERSION_MAJOR 0
#define BGMEDIA_VERSION_MINOR 3
#define BGMEDIA_VERSION_PATCH 0
#define BGMEDIA_VERSION_STRING "0.3.0"

namespace bgmedia {

// Schema 2 adds the split confidence fields and the metric-phase semantics.
// Analyzer 2 invalidates every prior tempo/beat cache entry. Cache format 2
// persists the new fields.
inline constexpr std::uint32_t kAnalysisSchemaVersion = 2;
inline constexpr std::uint32_t kAnalyzerVersion = 2;
inline constexpr std::uint32_t kCacheFormatVersion = 2;

} // namespace bgmedia
