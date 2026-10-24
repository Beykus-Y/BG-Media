#pragma once

#include <cstdint>

// Public project version. Analyzer and schema versions are deliberately
// separate: cache files may stay readable across UI releases, but any change
// to analysis semantics must bump kAnalyzerVersion.
#define BGMEDIA_VERSION_MAJOR 0
#define BGMEDIA_VERSION_MINOR 1
#define BGMEDIA_VERSION_PATCH 0
#define BGMEDIA_VERSION_STRING "0.1.0"

namespace bgmedia {

inline constexpr std::uint32_t kAnalysisSchemaVersion = 1;
inline constexpr std::uint32_t kAnalyzerVersion = 1;
inline constexpr std::uint32_t kCacheFormatVersion = 1;

} // namespace bgmedia
