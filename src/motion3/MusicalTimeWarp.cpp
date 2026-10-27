#include "bgmedia/motion3/MusicalTimeWarp.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace bgmedia::motion {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kMaxAmplitude = 0.45;

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

double positivePhase(double beat) {
    double phase = beat - std::floor(beat);
    if (phase < 0.0) {
        phase += 1.0;
    }
    return std::clamp(phase, 0.0, 1.0);
}

} // namespace

const char* expressionName(MotionExpression expression) {
    switch (expression) {
    case MotionExpression::Off: return "off";
    case MotionExpression::Soft: return "soft";
    case MotionExpression::Expressive: return "expressive";
    }
    return "off";
}

MotionExpression expressionFromName(const std::string& name) {
    const std::string value = lower(trim(name));
    if (value == "off" || value == "none" || value == "0") {
        return MotionExpression::Off;
    }
    if (value == "expressive" || value == "exp" || value == "strong" || value == "2") {
        return MotionExpression::Expressive;
    }
    return MotionExpression::Soft;
}

float MusicalTimeWarp::maxAmplitude(MotionExpression expression, float intensity) {
    const double scale = std::clamp(static_cast<double>(intensity), 0.0, 1.0);
    double base = 0.0;
    switch (expression) {
    case MotionExpression::Off: base = 0.0; break;
    case MotionExpression::Soft: base = 0.10; break;
    case MotionExpression::Expressive: base = 0.22; break;
    }
    return static_cast<float>(std::clamp(base * scale, 0.0, kMaxAmplitude));
}

double MusicalTimeWarp::deviationAt(double phase, double amplitude) {
    const double a = std::clamp(std::isfinite(amplitude) ? amplitude : 0.0, 0.0,
                                kMaxAmplitude);
    const double u = std::clamp(std::isfinite(phase) ? phase : 0.0, 0.0, 1.0);
    return a / kTwoPi * (std::cos(kTwoPi * u) - 1.0);
}

double MusicalTimeWarp::rateAt(double phase, double amplitude) {
    const double a = std::clamp(std::isfinite(amplitude) ? amplitude : 0.0, 0.0,
                                kMaxAmplitude);
    const double u = std::clamp(std::isfinite(phase) ? phase : 0.0, 0.0, 1.0);
    return 1.0 - a * std::sin(kTwoPi * u);
}

WarpResult MusicalTimeWarp::sample(const WarpSample& input, const WarpProfile& profile) {
    WarpResult result;
    result.beat = input.musicalBeat;
    if (!std::isfinite(input.musicalBeat)) {
        result.beat = 0.0;
        return result;
    }
    if (!input.allow || input.silent) {
        return result;
    }
    const float accent = std::isfinite(input.accent) ? std::clamp(input.accent, 0.0f, 1.0f)
                                                     : 0.0f;
    const float bass = std::isfinite(input.bass) ? std::clamp(input.bass, 0.0f, 1.0f) : 0.0f;
    const float confidence =
        std::isfinite(input.confidence) ? std::clamp(input.confidence, 0.0f, 1.0f) : 0.0f;
    const float shape = std::clamp(0.65f * accent + 0.35f * bass, 0.0f, 1.0f);
    if (shape <= 1e-4f) {
        return result;
    }
    float amplitude = maxAmplitude(profile.expression, profile.intensity) * shape *
                      (0.35f + 0.65f * confidence);
    if (input.footContact && !profile.upperBodyOnly) {
        amplitude *= std::clamp(profile.contactScale, 0.0f, 1.0f);
    }
    amplitude = static_cast<float>(std::clamp(static_cast<double>(amplitude), 0.0, kMaxAmplitude));
    if (amplitude <= 1e-4f) {
        return result;
    }
    const double phase = positivePhase(input.musicalBeat);
    result.deviation = deviationAt(phase, amplitude);
    result.rate = rateAt(phase, amplitude);
    result.beat = input.musicalBeat + result.deviation;
    result.amplitude = amplitude;
    result.active = true;
    return result;
}

bool saveWarpProfile(const std::filesystem::path& path, const WarpProfile& profile,
                     std::string* error) {
    if (path.empty()) {
        if (error) {
            *error = "empty profile path";
        }
        return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot open profile for writing";
        }
        return false;
    }
    out << "# BG Motion 3.1 profile v1\n";
    out << "expression=" << expressionName(profile.expression) << "\n";
    out << "intensity=" << std::clamp(profile.intensity, 0.0f, 1.0f) << "\n";
    out << "upperBodyOnly=" << (profile.upperBodyOnly ? 1 : 0) << "\n";
    out << "contactScale=" << std::clamp(profile.contactScale, 0.0f, 1.0f) << "\n";
    out.flush();
    if (!out) {
        if (error) {
            *error = "failed to write profile";
        }
        return false;
    }
    return true;
}

bool loadWarpProfile(const std::filesystem::path& path, WarpProfile& profile,
                     std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open profile";
        }
        return false;
    }
    WarpProfile loaded;
    std::string line;
    int parsed = 0;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "expression") {
            loaded.expression = expressionFromName(value);
            ++parsed;
        } else if (key == "intensity") {
            try {
                loaded.intensity = std::stof(value);
            } catch (...) {
                continue;
            }
            loaded.intensity = std::clamp(loaded.intensity, 0.0f, 1.0f);
            ++parsed;
        } else if (key == "upperbodyonly") {
            loaded.upperBodyOnly = !(value == "0" || lower(value) == "false");
            ++parsed;
        } else if (key == "contactscale") {
            try {
                loaded.contactScale = std::stof(value);
            } catch (...) {
                continue;
            }
            loaded.contactScale = std::clamp(loaded.contactScale, 0.0f, 1.0f);
            ++parsed;
        }
    }
    if (parsed == 0) {
        if (error) {
            *error = "profile has no recognised keys";
        }
        return false;
    }
    profile = loaded;
    return true;
}

} // namespace bgmedia::motion
