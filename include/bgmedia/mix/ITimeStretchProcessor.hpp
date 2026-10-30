#pragma once

// Quality-related seams for BG Mix. Tempo changes in 0.1 use a linear
// resampler, which changes pitch. A future pitch-preserving implementation
// (WSOLA or a transient-aware phase vocoder) plugs in here without changing the
// dual-deck engine or the planner.

#include "bgmedia/mix/PcmTrack.hpp"

#include <cstddef>
#include <string>

namespace bgmedia::mix {

class ITimeStretchProcessor {
public:
    virtual ~ITimeStretchProcessor() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // False means tempo changes alter pitch. The UI must state that clearly.
    [[nodiscard]] virtual bool pitchPreserving() const noexcept = 0;

    // Renders `outFrames` interleaved stereo frames from `track`, starting at
    // `positionFrames` (in source frames) and advancing it according to
    // `tempoRatio`. Must be real-time safe: no allocation, no locks, no I/O.
    virtual void render(const PcmTrack& track, double& positionFrames,
                        double tempoRatio, float* out,
                        std::size_t outFrames) const noexcept = 0;
};

// The 0.1 default: linear interpolation. Pitch follows speed. It is honest
// about being experimental for tempo ramps and is not called a DJ algorithm.
class LinearResampleStretcher final : public ITimeStretchProcessor {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "linear-resample";
    }
    [[nodiscard]] bool pitchPreserving() const noexcept override {
        return false;
    }
    void render(const PcmTrack& track, double& positionFrames,
                double tempoRatio, float* out,
                std::size_t outFrames) const noexcept override;
};

// Placeholder for a later pitch-preserving implementation. It is deliberately
// not selectable: `available()` is false and the engine keeps the linear
// processor. This keeps the interface exercised without shipping a stub that
// pretends to work.
class WsolaStretcher final : public ITimeStretchProcessor {
public:
    [[nodiscard]] static bool available() noexcept { return false; }
    [[nodiscard]] const char* name() const noexcept override {
        return "wsola-not-implemented";
    }
    [[nodiscard]] bool pitchPreserving() const noexcept override {
        return true;
    }
    void render(const PcmTrack&, double&, double, float* out,
                std::size_t outFrames) const noexcept override;
};

} // namespace bgmedia::mix
