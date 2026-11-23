#include "VoxResample.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::vox::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kPhases = 512;           // table resolution (linear interpolation between rows)
constexpr double kStopbandDb = 80.0;
constexpr int kMaxHalfTaps = 512;
constexpr std::size_t kPollMask = (1u << 15) - 1u;

double besselI0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

double sinc(double x) noexcept {
    if (std::abs(x) < 1e-12) {
        return 1.0;
    }
    const double px = kPi * x;
    return std::sin(px) / px;
}

} // namespace

bool resampleMono(const float* in, std::size_t frames, std::uint32_t inRate,
                  std::uint32_t outRate, std::vector<float>& out, const KeepGoing& keep) {
    out.clear();
    if (frames == 0 || in == nullptr || inRate == 0 || outRate == 0) {
        return true;
    }
    if (inRate == outRate) {
        out.assign(in, in + frames);
        return true;
    }
    const std::uint64_t outCount64 =
        static_cast<std::uint64_t>(frames) * outRate / inRate;
    const std::size_t outCount = static_cast<std::size_t>(std::max<std::uint64_t>(outCount64, 1));

    // Filter design in units of cycles per INPUT sample.
    const double lowNyquistHz = 0.5 * static_cast<double>(std::min(inRate, outRate));
    const double fc = 0.95 * lowNyquistHz / static_cast<double>(inRate);
    const double transition = 0.10 * lowNyquistHz / static_cast<double>(inRate);
    const double beta = 0.1102 * (kStopbandDb - 8.7);
    const double taps = (kStopbandDb - 8.0) / (2.285 * 2.0 * kPi * transition);
    const int half = std::clamp(static_cast<int>(std::ceil(taps * 0.5)) + 1, 4, kMaxHalfTaps);
    const int width = 2 * half;  // taps per phase: input offsets -half+1 .. half

    // table[p][k] = h(j - p/kPhases), j = k - half + 1; rows normalised to sum 1.
    std::vector<float> table(static_cast<std::size_t>(kPhases + 1) * static_cast<std::size_t>(width));
    const double i0Beta = besselI0(beta);
    std::vector<double> row(static_cast<std::size_t>(width));
    for (int p = 0; p <= kPhases; ++p) {
        const double d = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        for (int k = 0; k < width; ++k) {
            const double x = static_cast<double>(k - half + 1) - d;
            const double r = x / static_cast<double>(half);
            double w = 0.0;
            if (std::abs(r) < 1.0) {
                w = besselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta;
            }
            row[static_cast<std::size_t>(k)] = 2.0 * fc * sinc(2.0 * fc * x) * w;
            sum += row[static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < width; ++k) {
            table[static_cast<std::size_t>(p) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(k)] =
                static_cast<float>(row[static_cast<std::size_t>(k)] / sum);
        }
    }

    out.resize(outCount);
    const std::int64_t n = static_cast<std::int64_t>(frames);
    for (std::size_t o = 0; o < outCount; ++o) {
        if ((o & kPollMask) == 0 && keep && !keep()) {
            return false;
        }
        const std::uint64_t num = static_cast<std::uint64_t>(o) * inRate;
        const std::int64_t idx = static_cast<std::int64_t>(num / outRate);
        const std::uint64_t rem = num % outRate;
        const double ph = static_cast<double>(rem) * kPhases / static_cast<double>(outRate);
        const int p0 = std::min(static_cast<int>(ph), kPhases - 1);
        const float w1 = static_cast<float>(ph - static_cast<double>(p0));
        const float w0 = 1.0f - w1;
        const float* t0 = &table[static_cast<std::size_t>(p0) * static_cast<std::size_t>(width)];
        const float* t1 = t0 + width;
        const std::int64_t first = idx - half + 1;
        double acc = 0.0;
        if (first >= 0 && first + width <= n) {
            const float* x = in + first;
            float a = 0.0f;
            float b = 0.0f;
            for (int k = 0; k < width; ++k) {
                a += t0[k] * x[k];
                b += t1[k] * x[k];
            }
            acc = static_cast<double>(w0 * a + w1 * b);
        } else {
            for (int k = 0; k < width; ++k) {
                const std::int64_t i = first + k;
                if (i < 0 || i >= n) {
                    continue;
                }
                acc += static_cast<double>(w0 * t0[k] + w1 * t1[k]) * static_cast<double>(in[i]);
            }
        }
        out[o] = static_cast<float>(acc);
    }
    return true;
}

} // namespace bgmedia::vox::detail
