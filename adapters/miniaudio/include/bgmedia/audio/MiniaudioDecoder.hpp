#pragma once

#include "bgmedia/audio/IAudioDecoder.hpp"

#include <cstddef>

namespace bgmedia {

// Decodes WAV, MP3 and FLAC through miniaudio. The adapter lives outside the
// core library on purpose: the analyzer accepts PCM and does not know how it
// was obtained. The same binary also powers the desktop player, but playback
// is implemented by the host, not by this decoder.
class MiniaudioDecoder final : public IAudioDecoder {
public:
    explicit MiniaudioDecoder(std::size_t maxDecodedBytes = 1024ull * 1024ull * 1024ull)
        : maxDecodedBytes_(maxDecodedBytes) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "miniaudio"; }

    [[nodiscard]] bool supports(const std::filesystem::path& path) const override;

    [[nodiscard]] DecodeResult decode(
        const std::filesystem::path& path,
        const DecodeProgressCallback& progress = {}) const override;

private:
    std::size_t maxDecodedBytes_;
};

} // namespace bgmedia
