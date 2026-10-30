#pragma once

// Time-stretch seam for BG Mix.
//
// 0.1 exposed only a stateless linear resampler, which changes pitch with
// speed. 0.2 adds a per-deck streaming interface so a pitch-preserving
// algorithm can keep overlap-add history across render calls. The legacy
// `render()` entry point stays available for hosts and tests that only need a
// one-shot resample.
//
// Everything below the `createStream()` boundary must be real-time safe once
// the stream has been constructed: no allocation, locks or file I/O. Streams
// are created on the control thread (MixEngine::setTimeStretch) and retired
// with the same render-epoch protocol as PCM buffers.

#include "bgmedia/mix/PcmTrack.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bgmedia::mix {

class ITimeStretchStream {
public:
    virtual ~ITimeStretchStream() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual bool pitchPreserving() const noexcept = 0;

    // Jumps the read head. Called for a deck reset, seek or a new transition.
    // Must not allocate and must leave the stream click-free (the first window
    // fades in from silence).
    virtual void reset(double sourcePositionFrames) noexcept = 0;

    // Renders `outFrames` interleaved stereo frames. `ratioStart`/`ratioEnd`
    // are source frames consumed per output frame across the block, so a tempo
    // ramp is applied inside the block instead of stepping per callback.
    // `positionFrames` is advanced by the integral of that ratio.
    virtual void render(const PcmTrack& track, double& positionFrames,
                        double ratioStart, double ratioEnd, float* out,
                        std::size_t outFrames) noexcept = 0;

    // Algorithmic look-ahead in source frames. Hosts subtract this from their
    // musical clock when they need sample-accurate alignment.
    [[nodiscard]] virtual double latencyFrames() const noexcept = 0;
};

class ITimeStretchProcessor {
public:
    virtual ~ITimeStretchProcessor() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // False means tempo changes alter pitch. The UI must state that clearly.
    [[nodiscard]] virtual bool pitchPreserving() const noexcept = 0;

    // Control thread. May return nullptr for one-shot-only processors; the
    // engine then falls back to the legacy render() path.
    [[nodiscard]] virtual std::shared_ptr<ITimeStretchStream> createStream(
        std::uint32_t /*sampleRate*/) const {
        return nullptr;
    }

    // Legacy one-shot path. Kept for compatibility with 0.1 hosts.
    virtual void render(const PcmTrack& track, double& positionFrames,
                        double tempoRatio, float* out,
                        std::size_t outFrames) const noexcept = 0;
};

// The 0.1 default: linear interpolation. Pitch follows speed. It remains
// selectable as a deliberately cheap/performance mode, never as a DJ-grade
// pitch lock.
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

// WSOLA with a transient-aware similarity search: overlapping Hann windows are
// aligned by normalized cross-correlation on the mid signal so periodic
// material keeps its phase, while sharp onsets bypass the search so drums do
// not smear. Pitch is preserved because windows are never resampled.
class WsolaStretcher final : public ITimeStretchProcessor {
public:
    [[nodiscard]] static bool available() noexcept { return true; }
    [[nodiscard]] const char* name() const noexcept override {
        return "wsola-transient";
    }
    [[nodiscard]] bool pitchPreserving() const noexcept override {
        return true;
    }
    [[nodiscard]] std::shared_ptr<ITimeStretchStream> createStream(
        std::uint32_t sampleRate) const override;
    // One-shot compatibility path (no history); the engine uses streams.
    void render(const PcmTrack& track, double& positionFrames,
                double tempoRatio, float* out,
                std::size_t outFrames) const noexcept override;
};

} // namespace bgmedia::mix
