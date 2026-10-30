#include "bgmedia/mix/ITimeStretchProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace bgmedia::mix {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t resolveWindow(std::uint32_t sampleRate) {
    const double target = 0.042 * static_cast<double>(sampleRate ? sampleRate : 48000);
    const std::uint32_t rounded =
        static_cast<std::uint32_t>(std::llround(target / 2.0) * 2.0);
    return std::clamp<std::uint32_t>(rounded, 1024u, 4096u);
}

float sourceSample(const PcmTrack& track, std::size_t frame, std::uint32_t channel) {
    if (frame >= track.frameCount()) {
        return 0.0f;
    }
    if (track.channels == 1) {
        return track.samples[frame];
    }
    return track.samples[frame * 2 + channel];
}

float sourceMono(const PcmTrack& track, std::size_t frame) {
    if (frame >= track.frameCount()) {
        return 0.0f;
    }
    if (track.channels == 1) {
        return track.samples[frame];
    }
    return 0.5f * (track.samples[frame * 2] + track.samples[frame * 2 + 1]);
}

class WsolaStream final : public ITimeStretchStream {
public:
    explicit WsolaStream(std::uint32_t sampleRate)
        : sampleRate_(sampleRate ? sampleRate : 48000),
          window_(resolveWindow(sampleRate_)),
          hop_(window_ / 2),
          search_(std::max<std::size_t>(64, window_ / 10)) {
        const std::size_t wanted = window_ * 2 + 4096 + hop_;
        capacity_ = 1;
        while (capacity_ < wanted) {
            capacity_ <<= 1;
        }
        ring_.assign(capacity_ * 2, 0.0f);
        acc_.assign(window_ * 2, 0.0f);
        previous_.assign(hop_, 0.0f);
        hann_.resize(window_);
        for (std::size_t i = 0; i < window_; ++i) {
            // Periodic Hann: hop = window / 2 sums to exactly 1.0, so ratio
            // 1.0 with offset 0 reconstructs the source.
            hann_[i] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                     static_cast<double>(window_)));
        }
        reset(0.0);
    }

    [[nodiscard]] const char* name() const noexcept override {
        return "wsola-transient";
    }
    [[nodiscard]] bool pitchPreserving() const noexcept override { return true; }

    void reset(double sourcePositionFrames) noexcept override {
        ringRead_ = 0;
        ringWrite_ = 0;
        ringCount_ = 0;
        std::fill(acc_.begin(), acc_.end(), 0.0f);
        std::fill(previous_.begin(), previous_.end(), 0.0f);
        sourcePosition_ = std::max(0.0, sourcePositionFrames);
        primed_ = false;
        eof_ = false;
    }

    void render(const PcmTrack& track, double& positionFrames,
                double ratioStart, double ratioEnd, float* out,
                std::size_t outFrames) noexcept override {
        if (!out) {
            return;
        }
        if (!track.valid() || outFrames == 0) {
            std::memset(out, 0, outFrames * 2 * sizeof(float));
            return;
        }
        if (positionFrames >= static_cast<double>(track.frameCount())) {
            std::memset(out, 0, outFrames * 2 * sizeof(float));
            return;
        }
        const double r0 = std::clamp(ratioStart, 0.05, 8.0);
        const double r1 = std::clamp(ratioEnd, 0.05, 8.0);
        const double average = 0.5 * (r0 + r1);

        // Re-anchor the analysis head to the output clock plus everything that
        // is already queued. This keeps the published media position and the
        // actual window source in lockstep across arbitrary callback sizes.
        sourcePosition_ =
            std::max(0.0, positionFrames + static_cast<double>(ringCount_) * average);

        // Generate and drain incrementally. The ring has a fixed capacity, so
        // filling it before draining would deadlock for any request larger
        // than the ring (for example a one-shot 48 kHz second).
        std::size_t written = 0;
        while (written < outFrames) {
            while (ringCount_ == 0) {
                generate(track, average);
            }
            const std::size_t chunk =
                std::min(outFrames - written, ringCount_);
            for (std::size_t i = 0; i < chunk; ++i) {
                const std::size_t index = written + i;
                out[index * 2] = ring_[ringRead_ * 2];
                out[index * 2 + 1] = ring_[ringRead_ * 2 + 1];
                ringRead_ = (ringRead_ + 1) % capacity_;
                --ringCount_;
            }
            written += chunk;
        }

        for (std::size_t i = 0; i < outFrames; ++i) {
            const double t = outFrames > 1
                                 ? static_cast<double>(i) /
                                       static_cast<double>(outFrames - 1)
                                 : 1.0;
            positionFrames += r0 + (r1 - r0) * t;
        }
    }

    [[nodiscard]] double latencyFrames() const noexcept override {
        return static_cast<double>(window_) * 0.5;
    }

private:
    void push(float left, float right) noexcept {
        if (ringCount_ >= capacity_) {
            return;
        }
        ring_[ringWrite_ * 2] = left;
        ring_[ringWrite_ * 2 + 1] = right;
        ringWrite_ = (ringWrite_ + 1) % capacity_;
        ++ringCount_;
    }

    bool transientAt(const PcmTrack& track, std::size_t start) const noexcept {
        const std::size_t span = std::min<std::size_t>(hop_, 256);
        double head = 0.0;
        double tail = 0.0;
        for (std::size_t i = 0; i < span; ++i) {
            const double value = sourceMono(track, start + i);
            head += value * value;
            const double later = sourceMono(track, start + hop_ + i);
            tail += later * later;
        }
        if (head <= 1e-8) {
            return false;
        }
        const double ratio = head / (tail + 1e-8);
        const double rms = std::sqrt(head / static_cast<double>(span));
        return (ratio > 2.4 && rms > 0.02) || rms > 0.55;
    }

    void renderWindow(const PcmTrack& track, std::size_t start) noexcept {
        const std::uint32_t channels = track.channels == 1 ? 1u : 2u;
        for (std::size_t i = 0; i < window_; ++i) {
            const std::size_t frame = start + i;
            const float weight = hann_[i];
            if (channels == 1) {
                const float value = sourceSample(track, frame, 0) * weight;
                acc_[i * 2] += value;
                acc_[i * 2 + 1] += value;
            } else {
                acc_[i * 2] += sourceSample(track, frame, 0) * weight;
                acc_[i * 2 + 1] += sourceSample(track, frame, 1) * weight;
            }
        }
    }

    void generate(const PcmTrack& track, double ratio) noexcept {
        const std::size_t frameCount = track.frameCount();
        // Windows may start up to the source end and are zero-padded past it.
        // Stopping at frameCount - window would cut a sub-unity stretched tail
        // short: at 0.25x the last window still has four times its length of
        // output to emit.
        const std::size_t maxStart = frameCount;
        if (eof_) {
            // Drain the overlap tail of the final window, then silence. Do not
            // clamp back to the last full-window start: that would repeat the
            // final window for the whole rest of the requested block.
            const std::size_t emit = std::min(
                hop_, ringCount_ < capacity_ ? capacity_ - ringCount_ : 0);
            for (std::size_t i = 0; i < emit; ++i) {
                push(acc_[i * 2], acc_[i * 2 + 1]);
            }
            const std::size_t keep = window_ - hop_;
            for (std::size_t i = 0; i < keep; ++i) {
                acc_[i * 2] = acc_[(i + hop_) * 2];
                acc_[i * 2 + 1] = acc_[(i + hop_) * 2 + 1];
            }
            std::fill(acc_.begin() + static_cast<std::ptrdiff_t>(keep * 2),
                      acc_.end(), 0.0f);
            sourcePosition_ += static_cast<double>(hop_) * ratio;
            return;
        }
        const double nominal = std::clamp(
            sourcePosition_, 0.0, static_cast<double>(maxStart));
        std::size_t base = static_cast<std::size_t>(std::llround(nominal));

        std::size_t best = base;
        if (primed_ && frameCount > window_) {
            const bool transient = transientAt(track, base);
            if (!transient) {
                const long long lower = std::max<long long>(
                    -static_cast<long long>(search_),
                    -static_cast<long long>(base));
                const long long upper = std::min<long long>(
                    static_cast<long long>(search_),
                    static_cast<long long>(maxStart) -
                        static_cast<long long>(base));
                auto scoreAt = [&](long long offset) {
                    const long long candidate =
                        static_cast<long long>(base) + offset;
                    double dot = 0.0;
                    double energy = 0.0;
                    for (std::size_t i = 0; i < hop_; i += 4) {
                        const float value = sourceMono(
                            track, static_cast<std::size_t>(candidate) + i);
                        dot += static_cast<double>(value) * previous_[i];
                        energy += static_cast<double>(value) * value;
                    }
                    if (energy <= 1e-9) {
                        return -std::numeric_limits<double>::max();
                    }
                    return dot / std::sqrt(energy + 1e-12);
                };
                // Zero is the exact overlap at unity rate. The coarse stride
                // starts at -search_ (odd at 48 kHz), so evaluate it
                // explicitly instead of relying on the loop to land on it.
                double bestScore = scoreAt(0);
                best = base;
                for (long long offset = lower; offset <= upper;
                     offset += 4) {
                    if (offset == 0) {
                        continue;
                    }
                    const double score = scoreAt(offset);
                    if (score > bestScore) {
                        bestScore = score;
                        best = static_cast<std::size_t>(
                            static_cast<long long>(base) + offset);
                    }
                }
                if (bestScore < 0.15) {
                    best = base;
                }
            }
        }

        renderWindow(track, best);

        // Remember the second half of the chosen source window. The next
        // window's first hop should match it, which is what keeps periodic
        // material phase-coherent instead of comb-filtered.
        for (std::size_t i = 0; i < hop_; ++i) {
            previous_[i] = sourceMono(track, best + hop_ + i);
        }
        primed_ = true;
        if (nominal >= static_cast<double>(maxStart)) {
            // The analysis head has reached the actual source end. Subsequent
            // calls drain the overlap tail and then emit silence.
            eof_ = true;
        }

        const std::size_t emit = std::min(hop_, ringCount_ < capacity_
                                                     ? capacity_ - ringCount_
                                                     : 0);
        for (std::size_t i = 0; i < emit; ++i) {
            push(acc_[i * 2], acc_[i * 2 + 1]);
        }

        // Shift the overlap accumulator left by one synthesis hop.
        const std::size_t keep = window_ - hop_;
        for (std::size_t i = 0; i < keep; ++i) {
            acc_[i * 2] = acc_[(i + hop_) * 2];
            acc_[i * 2 + 1] = acc_[(i + hop_) * 2 + 1];
        }
        std::fill(acc_.begin() + static_cast<std::ptrdiff_t>(keep * 2),
                  acc_.end(), 0.0f);

        sourcePosition_ =
            std::min(static_cast<double>(maxStart),
                     nominal + static_cast<double>(hop_) * ratio);
    }

    std::uint32_t sampleRate_ = 48000;
    std::size_t window_ = 2048;
    std::size_t hop_ = 1024;
    std::size_t search_ = 204;
    std::size_t capacity_ = 8192;
    std::size_t ringRead_ = 0;
    std::size_t ringWrite_ = 0;
    std::size_t ringCount_ = 0;
    std::vector<float> ring_;
    std::vector<float> acc_;
    std::vector<float> previous_;
    std::vector<float> hann_;
    double sourcePosition_ = 0.0;
    bool primed_ = false;
    bool eof_ = false;
};

} // namespace

void LinearResampleStretcher::render(const PcmTrack& track,
                                     double& positionFrames, double tempoRatio,
                                     float* out,
                                     std::size_t outFrames) const noexcept {
    if (!track.valid() || !out) {
        if (out) {
            std::fill_n(out, outFrames * 2, 0.0f);
        }
        return;
    }
    const std::uint32_t channels = track.channels;
    const std::size_t sourceFrames = track.frameCount();
    const double step = std::clamp(tempoRatio, 0.05, 8.0);
    for (std::size_t i = 0; i < outFrames; ++i) {
        if (positionFrames < 0.0 ||
            positionFrames >= static_cast<double>(sourceFrames) - 1.0) {
            out[i * 2] = 0.0f;
            out[i * 2 + 1] = 0.0f;
            positionFrames += step;
            continue;
        }
        const auto index = static_cast<std::size_t>(positionFrames);
        const float frac =
            static_cast<float>(positionFrames - static_cast<double>(index));
        if (channels == 1) {
            const float a = track.samples[index];
            const float b = track.samples[std::min(index + 1, sourceFrames - 1)];
            const float value = a + (b - a) * frac;
            out[i * 2] = value;
            out[i * 2 + 1] = value;
        } else {
            const float a0 = track.samples[index * 2];
            const float a1 = track.samples[index * 2 + 1];
            const std::size_t next = std::min(index + 1, sourceFrames - 1);
            const float b0 = track.samples[next * 2];
            const float b1 = track.samples[next * 2 + 1];
            out[i * 2] = a0 + (b0 - a0) * frac;
            out[i * 2 + 1] = a1 + (b1 - a1) * frac;
        }
        positionFrames += step;
    }
}

std::shared_ptr<ITimeStretchStream> WsolaStretcher::createStream(
    std::uint32_t sampleRate) const {
    return std::make_shared<WsolaStream>(sampleRate);
}

void WsolaStretcher::render(const PcmTrack& track, double& positionFrames,
                            double tempoRatio, float* out,
                            std::size_t outFrames) const noexcept {
    // Legacy one-shot path. It is explicitly not real-time safe; the engine
    // always uses createStream(). Keeping it functional means a 0.1 host that
    // calls render() directly still gets pitch-preserving output.
    static thread_local std::shared_ptr<ITimeStretchStream> oneShot;
    static thread_local std::uint32_t oneShotRate = 0;
    static thread_local const PcmTrack* oneShotTrack = nullptr;
    // A new track, a new stream rate, or a rewind to the start begins an
    // independent render. Reusing the previous track's overlap history here
    // would leak its queued samples into the new export.
    if (!oneShot || oneShotRate != track.sampleRate ||
        oneShotTrack != &track || positionFrames <= 0.0) {
        oneShotRate = track.sampleRate;
        oneShotTrack = &track;
        oneShot = std::make_shared<WsolaStream>(track.sampleRate);
        oneShot->reset(positionFrames);
    }
    oneShot->render(track, positionFrames, tempoRatio, tempoRatio, out,
                    outFrames);
}

} // namespace bgmedia::mix
