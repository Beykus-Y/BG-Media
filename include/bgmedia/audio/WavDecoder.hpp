#pragma once

#include "bgmedia/audio/IAudioDecoder.hpp"

namespace bgmedia {

// Small, dependency-free RIFF/WAVE reader supporting PCM 8/16/24/32-bit,
// IEEE float 32/64-bit, WAVE_FORMAT_EXTENSIBLE, mono and multi-channel data.
// It is intentionally strict about malformed sizes and returns a useful error
// instead of reading out of bounds.
class WavDecoder final : public IAudioDecoder {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "builtin-wav"; }

    [[nodiscard]] bool supports(const std::filesystem::path& path) const override;

    [[nodiscard]] DecodeResult decode(
        const std::filesystem::path& path,
        const DecodeProgressCallback& progress = {}) const override;
};

} // namespace bgmedia
