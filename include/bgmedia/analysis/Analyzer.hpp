#pragma once

#include "bgmedia/analysis/AnalysisConfig.hpp"
#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/audio/AudioData.hpp"

#include <functional>
#include <string>

namespace bgmedia {

// Progress runs on the calling thread. Cancellation is polled cooperatively.
using AnalysisProgressCallback =
    std::function<void(float fraction, const std::string& stage)>;
using AnalysisCancelCallback = std::function<bool()>;

// Deterministic, GUI-free analysis pipeline. A single Analyzer instance is
// immutable and safe to call from multiple worker threads.
class Analyzer {
public:
    explicit Analyzer(AnalysisConfig config = {});

    [[nodiscard]] const AnalysisConfig& config() const noexcept { return config_; }

    [[nodiscard]] AnalysisReport analyze(
        const PcmBuffer& pcm,
        const AnalysisProgressCallback& progress = {},
        const AnalysisCancelCallback& cancel = {}) const;

private:
    AnalysisConfig config_;
};

// Resolves 0-valued STFT settings for a concrete sample rate. Public so tests
// and diagnostics can report the actual window used.
[[nodiscard]] int resolveStftSize(int requested, std::uint32_t sampleRate) noexcept;

} // namespace bgmedia
