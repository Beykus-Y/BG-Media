#pragma once

// BG Motion 3.1 musical time warp.
//
// The warp is a periodic, C1-continuous speed modulation inside one musical
// beat. For a normalised beat phase u in [0,1]:
//
//   rate(u)      = 1 - A * sin(2*pi*u)
//   deviation(u) = A/(2*pi) * (cos(2*pi*u) - 1)
//   warpedBeat   = musicalBeat + deviation(u)
//
// Properties (checked by tests):
//   * deviation(0) = deviation(1) = 0, so accents that sit on a beat boundary
//     are reached at exactly the musical time;
//   * d(deviation)/du = rate - 1, so the derivative is analytic;
//   * min rate = 1 - A, max rate = 1 + A; A is capped below 0.5, therefore the
//     phase is strictly monotonic and never plays backwards;
//   * rate(0) = rate(1) = 1 for every A, so changing the accent strength from
//     one beat to the next does not create a speed discontinuity.
//
// The shape is a deliberate "hold, then snap into the accent": the animation
// stays slightly behind in the first half of the beat and accelerates into the
// authored accent pose. It never shifts the accent pose itself.

#include <filesystem>
#include <string>

namespace bgmedia::motion {

enum class MotionExpression : unsigned char {
    Off,
    Soft,
    Expressive
};

[[nodiscard]] const char* expressionName(MotionExpression expression);
[[nodiscard]] MotionExpression expressionFromName(const std::string& name);

struct WarpProfile {
    MotionExpression expression = MotionExpression::Soft;
    float intensity = 1.0f;       // 0..1 user scale
    bool upperBodyOnly = true;    // legs, hips and root keep the stable grid
    float contactScale = 0.35f;   // amplitude scale for a full-body profile in contact
};

struct WarpSample {
    double musicalBeat = 0.0;
    float accent = 0.0f;          // accent strength at the upcoming boundary
    float bass = 0.0f;
    float confidence = 0.0f;      // timeline confidence in the event
    bool footContact = false;
    bool silent = false;
    bool allow = true;
};

struct WarpResult {
    double beat = 0.0;       // warped musical beat
    double deviation = 0.0;  // beat - musicalBeat, in beats
    double rate = 1.0;       // d(warped)/d(musical), > 0
    float amplitude = 0.0f;  // effective A
    bool active = false;
};

class MusicalTimeWarp {
public:
    // Largest A for a profile (before accent/confidence shaping).
    [[nodiscard]] static float maxAmplitude(MotionExpression expression, float intensity);

    [[nodiscard]] static WarpResult sample(const WarpSample& input,
                                           const WarpProfile& profile);
    // Exposed for tests and diagnostics.
    [[nodiscard]] static double deviationAt(double phase, double amplitude);
    [[nodiscard]] static double rateAt(double phase, double amplitude);
};

// Small, dependency-free profile file used by the desktop host. Format is
// human readable key=value; unknown keys are ignored. The engine does not
// depend on a UI or a settings framework.
[[nodiscard]] bool saveWarpProfile(const std::filesystem::path& path,
                                  const WarpProfile& profile, std::string* error = nullptr);
[[nodiscard]] bool loadWarpProfile(const std::filesystem::path& path,
                                   WarpProfile& profile, std::string* error = nullptr);

} // namespace bgmedia::motion
