#include "VoxDsp.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::vox::detail {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kPollMask = 8191;
} // namespace

bool highPassFilter(float* data, std::size_t frames, std::size_t channels,
                    double sampleRate, double cutoffHz, const KeepGoing& keep) {
    if (frames == 0 || cutoffHz <= 0.0) {
        return true;
    }
    const double w0 = 2.0 * kPi * cutoffHz / sampleRate;
    const double cs = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 + cs) / 2.0 / a0;
    const double b1 = -(1.0 + cs) / a0;
    const double b2 = b0;
    const double a1 = -2.0 * cs / a0;
    const double a2 = (1.0 - alpha) / a0;

    double z1[2] = {0.0, 0.0};
    double z2[2] = {0.0, 0.0};
    for (std::size_t c = 0; c < channels && c < 2; ++c) {
        const double x0 = static_cast<double>(data[c]);
        z2[c] = b2 * x0;
        z1[c] = b1 * x0 + b2 * x0;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        if ((i & kPollMask) == 0 && keep && !keep()) {
            return false;
        }
        for (std::size_t c = 0; c < channels && c < 2; ++c) {
            float& s = data[i * channels + c];
            const double x = static_cast<double>(s);
            const double y = b0 * x + z1[c];
            z1[c] = b1 * x - a1 * y + z2[c];
            z2[c] = b2 * x - a2 * y;
            s = static_cast<float>(y);
        }
    }
    return true;
}

std::size_t vadFrameLength(std::uint32_t sampleRate) noexcept {
    return std::max<std::size_t>(1, static_cast<std::size_t>(sampleRate) / 100);
}

void frameMeanSquareDb(const float* data, std::size_t frames, std::size_t channels,
                       std::size_t frameLen, std::vector<float>& outDb) {
    outDb.clear();
    for (std::size_t begin = 0; begin < frames; begin += frameLen) {
        const std::size_t end = std::min(frames, begin + frameLen);
        double sum = 0.0;
        for (std::size_t i = begin * channels; i < end * channels; ++i) {
            sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
        }
        outDb.push_back(static_cast<float>(
            msToDb(sum / static_cast<double>((end - begin) * channels))));
    }
}

void runVad(const std::vector<float>& frameDb, const VadParams& params, VadResult& out) {
    const std::size_t n = frameDb.size();
    out.active.assign(n, 0);
    out.activeFrames = 0;
    out.hasSpeech = false;
    out.noiseFloorDb = kSilenceDb;
    out.maxDb = kSilenceDb;
    if (n == 0) {
        return;
    }
    std::vector<float> sorted(frameDb);
    const std::size_t idx = (n - 1) / 10;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(idx),
                     sorted.end());
    out.noiseFloorDb = static_cast<double>(sorted[idx]);
    out.maxDb = static_cast<double>(*std::max_element(frameDb.begin(), frameDb.end()));

    const double threshold =
        std::max(out.noiseFloorDb + params.marginDb, params.absoluteFloorDb);
    for (std::size_t i = 0; i < n; ++i) {
        out.active[i] = static_cast<double>(frameDb[i]) > threshold ? 1 : 0;
    }
    // Drop isolated short bursts (clicks).
    for (std::size_t i = 0; i < n;) {
        if (out.active[i] == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && out.active[j] != 0) {
            ++j;
        }
        if (j - i < params.minRunFrames) {
            std::fill(out.active.begin() + static_cast<std::ptrdiff_t>(i),
                      out.active.begin() + static_cast<std::ptrdiff_t>(j),
                      static_cast<std::uint8_t>(0));
        }
        i = j;
    }
    // Hangover.
    const std::size_t hang = static_cast<std::size_t>(
        std::lround(params.hangoverMs / params.frameMs));
    std::size_t remaining = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (out.active[i] != 0) {
            remaining = hang;
        } else if (remaining > 0) {
            out.active[i] = 1;
            --remaining;
        }
    }
    for (std::uint8_t a : out.active) {
        out.activeFrames += a;
    }
    out.hasSpeech = out.activeFrames >= 10 &&
                    (out.maxDb - out.noiseFloorDb) >= params.minRangeDb &&
                    out.maxDb > params.absoluteFloorDb;
}

double activeMeanSquare(const float* data, std::size_t frames, std::size_t channels,
                        std::size_t frameLen, const std::vector<std::uint8_t>& active) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t f = 0; f < active.size(); ++f) {
        if (active[f] == 0) {
            continue;
        }
        const std::size_t begin = f * frameLen;
        const std::size_t end = std::min(frames, begin + frameLen);
        for (std::size_t i = begin * channels; i < end * channels; ++i) {
            sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);
        }
        count += (end - begin) * channels;
    }
    return count == 0 ? -1.0 : sum / static_cast<double>(count);
}

bool compress(float* data, std::size_t frames, std::size_t channels, double sampleRate,
              const CompressorParams& p, const KeepGoing& keep) {
    const double detCoef = 1.0 - std::exp(-1.0 / (sampleRate * p.detectorMs * 1e-3));
    const double atk = 1.0 - std::exp(-1.0 / (sampleRate * p.attackMs * 1e-3));
    const double rel = 1.0 - std::exp(-1.0 / (sampleRate * p.releaseMs * 1e-3));
    const double slope = 1.0 - 1.0 / std::max(1.0, p.ratio);
    const double knee = std::max(1e-6, p.kneeDb);
    constexpr double kDbToNeper = 0.11512925464970229;  // ln(10) / 20
    double power = 0.0;
    double reduction = 0.0;  // dB, >= 0
    for (std::size_t i = 0; i < frames; ++i) {
        if ((i & kPollMask) == 0 && keep && !keep()) {
            return false;
        }
        float* s = data + i * channels;
        double ms = 0.0;
        for (std::size_t c = 0; c < channels; ++c) {
            ms += static_cast<double>(s[c]) * static_cast<double>(s[c]);
        }
        ms /= static_cast<double>(channels);
        power += detCoef * (ms - power);
        const double level = 10.0 * std::log10(power + 1e-12);
        const double over = level - p.thresholdDb;
        double target = 0.0;
        if (2.0 * over < -knee) {
            target = 0.0;
        } else if (2.0 * std::abs(over) <= knee) {
            const double t = over + knee * 0.5;
            target = slope * t * t / (2.0 * knee);
        } else {
            target = slope * over;
        }
        reduction += (target > reduction ? atk : rel) * (target - reduction);
        const float gain = static_cast<float>(std::exp(-reduction * kDbToNeper));
        for (std::size_t c = 0; c < channels; ++c) {
            s[c] *= gain;
        }
    }
    return true;
}

bool limit(float* data, std::size_t frames, std::size_t channels, double sampleRate,
           const LimiterParams& p, const KeepGoing& keep) {
    const double ceiling = dbToAmp(p.ceilingDb);
    const std::size_t look = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(sampleRate * p.lookaheadMs * 1e-3)));
    const double relCoef = 1.0 - std::exp(-1.0 / (sampleRate * p.releaseMs * 1e-3));
    const double pre = p.preGain;

    auto required = [&](std::size_t j) -> double {
        if (j >= frames) {
            return 1.0;
        }
        double peak = 0.0;
        for (std::size_t c = 0; c < channels; ++c) {
            peak = std::max(peak, std::abs(static_cast<double>(data[j * channels + c]) * pre));
        }
        return peak > ceiling ? ceiling / peak : 1.0;
    };

    // Monotonic deque (sliding minimum over [i, i + look]) in a small ring.
    const std::size_t cap = look + 2;
    std::vector<std::size_t> dqIdx(cap);
    std::vector<double> dqVal(cap);
    std::size_t head = 0;
    std::size_t size = 0;
    auto push = [&](std::size_t idx, double v) {
        while (size > 0 && dqVal[(head + size - 1) % cap] >= v) {
            --size;
        }
        const std::size_t pos = (head + size) % cap;
        dqIdx[pos] = idx;
        dqVal[pos] = v;
        ++size;
    };
    // Box average of the sliding minimum: guarantees gain <= required gain at
    // every sample while spreading the attack over the lookahead time.
    std::vector<double> box(look + 1, 1.0);
    double boxSum = static_cast<double>(look + 1);
    std::size_t boxPos = 0;

    for (std::size_t j = 0; j < look && j < frames; ++j) {
        push(j, required(j));
    }
    double gain = 1.0;
    const float ceilF = static_cast<float>(ceiling);
    for (std::size_t i = 0; i < frames; ++i) {
        if ((i & kPollMask) == 0 && keep && !keep()) {
            return false;
        }
        if (i + look < frames) {
            push(i + look, required(i + look));
        }
        while (size > 0 && dqIdx[head] < i) {
            head = (head + 1) % cap;
            --size;
        }
        const double gmin = size > 0 ? dqVal[head] : 1.0;
        boxSum += gmin - box[boxPos];
        box[boxPos] = gmin;
        boxPos = (boxPos + 1) % box.size();
        const double gbox = boxSum / static_cast<double>(box.size());
        gain = std::min(gbox, gain + relCoef * (1.0 - gain));
        const double g = gain * pre;
        for (std::size_t c = 0; c < channels; ++c) {
            float& s = data[i * channels + c];
            const float y = static_cast<float>(static_cast<double>(s) * g);
            s = std::max(-ceilF, std::min(ceilF, y));
        }
    }
    return true;
}

} // namespace bgmedia::vox::detail
