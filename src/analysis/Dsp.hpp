#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

extern "C" {
#include "kiss_fftr.h"
}

namespace bgmedia::dsp {

inline constexpr double kPi = 3.14159265358979323846;

inline double clampd(double value, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, value));
}

inline float clampf(float value, float lo, float hi) noexcept {
    return std::max(lo, std::min(hi, value));
}

inline int nextPowerOfTwo(int value) noexcept {
    int result = 1;
    while (result < value && result < (1 << 30)) {
        result <<= 1;
    }
    return result;
}

inline double hzToMel(double hz) noexcept {
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

inline double melToHz(double mel) noexcept {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

// Percentile with linear interpolation. Copies input (small arrays only).
inline float percentile(std::vector<float> values, double p) {
    if (values.empty()) {
        return 0.0f;
    }
    p = clampd(p, 0.0, 1.0);
    const std::size_t index = static_cast<std::size_t>(
        p * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

inline float median(std::vector<float> values) {
    return percentile(std::move(values), 0.5);
}

// Median of a slice without allocating per call when scratch is supplied.
inline float medianOfWindow(const std::vector<float>& values, std::size_t begin,
                            std::size_t end, std::vector<float>& scratch) {
    scratch.clear();
    scratch.insert(scratch.end(), values.begin() + static_cast<std::ptrdiff_t>(begin),
                   values.begin() + static_cast<std::ptrdiff_t>(end));
    if (scratch.empty()) {
        return 0.0f;
    }
    const std::size_t mid = scratch.size() / 2;
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                     scratch.end());
    return scratch[mid];
}

inline void normaliseInPlace(std::vector<float>& values, float target = 1.0f) {
    float peak = 0.0f;
    for (float v : values) {
        peak = std::max(peak, std::abs(v));
    }
    if (peak <= 1e-12f) {
        std::fill(values.begin(), values.end(), 0.0f);
        return;
    }
    const float scale = target / peak;
    for (float& v : values) {
        v *= scale;
    }
}

inline std::vector<float> hannWindow(int size) {
    std::vector<float> window(static_cast<std::size_t>(size), 1.0f);
    if (size <= 2) {
        return window;
    }
    const double denom = static_cast<double>(size - 1);
    for (int i = 0; i < size; ++i) {
        window[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / denom));
    }
    return window;
}

inline float parabolicPeakOffset(float left, float centre, float right) noexcept {
    const float denom = left - 2.0f * centre + right;
    if (std::abs(denom) < 1e-12f) {
        return 0.0f;
    }
    return clampf(0.5f * (left - right) / denom, -0.5f, 0.5f);
}

class RealFft {
public:
    explicit RealFft(int size) : size_(size) {
        config_ = kiss_fftr_alloc(size, 0, nullptr, nullptr);
    }

    ~RealFft() {
        if (config_) {
            kiss_fftr_free(config_);
        }
    }

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    [[nodiscard]] bool valid() const noexcept { return config_ != nullptr; }
    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] int binCount() const noexcept { return size_ / 2 + 1; }

    void forward(const float* input, kiss_fft_cpx* output) const {
        kiss_fftr(config_, const_cast<kiss_fft_scalar*>(input), output);
    }

private:
    int size_ = 0;
    kiss_fftr_cfg config_ = nullptr;
};

// Direct autocorrelation is fast enough for the short onset envelopes used
// here (a 10-minute track at ~86 fps has 51k frames and lags < 250) and avoids
// the normalisation pitfalls of an FFT implementation.
inline std::vector<double> autocorrelation(const std::vector<float>& signal,
                                           int maxLag) {
    const int n = static_cast<int>(signal.size());
    maxLag = std::min(maxLag, std::max(0, n - 1));
    std::vector<double> result(static_cast<std::size_t>(maxLag + 1), 0.0);
    if (n == 0) {
        return result;
    }

    double mean = 0.0;
    for (float v : signal) {
        mean += v;
    }
    mean /= static_cast<double>(n);

    std::vector<double> centred(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        centred[static_cast<std::size_t>(i)] = static_cast<double>(signal[static_cast<std::size_t>(i)]) - mean;
    }

    for (int lag = 0; lag <= maxLag; ++lag) {
        const int count = n - lag;
        double num = 0.0;
        double denomA = 0.0;
        double denomB = 0.0;
        for (int i = 0; i < count; ++i) {
            const double a = centred[static_cast<std::size_t>(i)];
            const double b = centred[static_cast<std::size_t>(i + lag)];
            num += a * b;
            denomA += a * a;
            denomB += b * b;
        }
        const double denom = std::sqrt(denomA * denomB);
        result[static_cast<std::size_t>(lag)] = denom > 1e-12 ? num / denom : 0.0;
    }
    return result;
}

inline std::vector<float> diffPositive(const std::vector<float>& values) {
    std::vector<float> out(values.size(), 0.0f);
    for (std::size_t i = 1; i < values.size(); ++i) {
        out[i] = std::max(0.0f, values[i] - values[i - 1]);
    }
    return out;
}

// Smooths a signal with a small normalised kernel; edge values are clamped.
inline std::vector<float> smooth(const std::vector<float>& values,
                                 const std::vector<float>& kernel) {
    if (values.empty() || kernel.empty()) {
        return values;
    }
    const int radius = static_cast<int>(kernel.size() / 2);
    const int n = static_cast<int>(values.size());
    std::vector<float> out(values.size(), 0.0f);
    for (int i = 0; i < n; ++i) {
        float sum = 0.0f;
        float weight = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            const int at = std::clamp(i + k, 0, n - 1);
            const float w = kernel[static_cast<std::size_t>(k + radius)];
            sum += values[static_cast<std::size_t>(at)] * w;
            weight += w;
        }
        out[static_cast<std::size_t>(i)] = weight > 0.0f ? sum / weight : 0.0f;
    }
    return out;
}

} // namespace bgmedia::dsp
