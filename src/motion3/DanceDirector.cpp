#include "bgmedia/motion3/DanceDirector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>

namespace bgmedia::motion {
namespace {

float smootherstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Search window of the planted-foot hand-over (A1): the next swing of the foot is looked for
// over at most this many seconds after the fade starts.
constexpr double kHandOverWindow = 4.0;
constexpr double kHandOverStep = 1.0 / 60.0;
constexpr double kHandOverMinSwing = 0.25;  // seconds a hand-over swing must last
constexpr int kHandOverSamples = static_cast<int>(kHandOverWindow / kHandOverStep);

// A segment's hand-over can hold its feet until this media time. Every place that keeps or
// drops segments uses this one lifetime, so a predecessor is never dropped while it still
// anchors a planted foot.
// A swing found in the window ends inside it, but a rebase to a slower tempo can move it
// later; its own end then extends the lifetime.
double handOverEnd(const ScheduleSegment& segment) {
    double end = segment.startTime + segment.fadeSeconds + kHandOverWindow;
    for (const auto& window : segment.footSwing) {
        if (window.resolved && !window.slide) {
            end = std::max(end, window.end);
        }
    }
    return end;
}

double circularDistance(double a, double b, double period) {
    double d = std::fmod(std::abs(a - b), period);
    return std::min(d, period - d);
}

double wrapPositive(double value, double period) {
    if (!(period > 1e-9)) {
        return 0.0;
    }
    double wrapped = std::fmod(value, period);
    if (wrapped < 0.0) {
        wrapped += period;
    }
    return wrapped;
}

bool upperBodyBone(vrm::HumanoidBone bone) {
    switch (bone) {
    case vrm::HumanoidBone::Hips:
    case vrm::HumanoidBone::LeftUpperLeg:
    case vrm::HumanoidBone::LeftLowerLeg:
    case vrm::HumanoidBone::LeftFoot:
    case vrm::HumanoidBone::LeftToes:
    case vrm::HumanoidBone::RightUpperLeg:
    case vrm::HumanoidBone::RightLowerLeg:
    case vrm::HumanoidBone::RightFoot:
    case vrm::HumanoidBone::RightToes:
        return false;
    default:
        return true;
    }
}

struct Mapping {
    double linearBeat = 0.0;
    double beat = 0.0;
    double rate = 1.0;       // clip beats per music beat, including warp
    double warpRate = 1.0;   // dimensionless warp multiplier
    double deviation = 0.0;  // beat domain deviation
    bool warpActive = false;
    float accent = 0.0f;
};

float accentAtNextBoundary(const ScheduleSegment& segment, double musicBeat,
                           const BeatClock& clock, const MusicalTimeline* timeline,
                           float* confidence) {
    const double nextBeat = std::floor(musicBeat + 1e-9) + 1.0;
    const double nextTime = clock.timeAtBeat(nextBeat);
    float accent = 0.0f;
    if (timeline) {
        accent = std::max(timeline->accentAt(nextTime, 0.12),
                          timeline->accentAt(clock.timeAtBeat(std::floor(musicBeat + 1e-9)),
                                             0.10));
        const int nextIndex = static_cast<int>(std::lround(nextBeat));
        accent = std::max(accent, timeline->beatAccent(nextIndex));
    }
    // A Motion Lab / clip-authored accent is a legitimate musical event too.
    const double entry = segment.entryClipBeat;
    const double alignBeat = clock.beatAt(segment.alignTime);
    const double nextClipBeat = entry + (nextBeat - alignBeat) * segment.rate;
    const double currentClipBeat = entry + (musicBeat - alignBeat) * segment.rate;
    float clipAccent = 0.0f;
    if (segment.clip) {
        clipAccent = std::max(clipAccentStrength(*segment.clip, nextClipBeat),
                              0.35f * clipAccentStrength(*segment.clip, currentClipBeat));
        accent = std::max(accent, clipAccent);
    }
    if (confidence) {
        const int nextIndex = static_cast<int>(std::lround(nextBeat));
        const float beatConfidence =
            timeline ? timeline->beatConfidence(nextIndex) : 0.0f;
        *confidence = clipAccent > 0.05f
                          ? 1.0f
                          : std::clamp(std::max(0.35f, beatConfidence), 0.0f, 1.0f);
    }
    return std::clamp(accent, 0.0f, 1.0f);
}

// Inverse of the linear (unwarped) clip-beat mapping: the media time at which `segment`
// reaches linear clip beat `beat` on `clock`.
double timeAtLinearClipBeat(const ScheduleSegment& segment, double beat, const BeatClock& clock) {
    if (!segment.clip) {
        return segment.alignTime;
    }
    if (!segment.clip->tempoSync) {
        return segment.alignTime + (beat - segment.entryClipBeat) * 60.0 / segment.clip->bpm;
    }
    const double rate = std::max(segment.rate, 1e-6);
    return clock.timeAtBeat(clock.beatAt(segment.alignTime) +
                            (beat - segment.entryClipBeat) / rate);
}

Mapping mapSegment(const ScheduleSegment& segment, double mediaTime,
                   const BeatClock& clock, const MusicalTimeline* timeline,
                   const WarpProfile& warp, bool footContact) {
    Mapping mapping;
    if (!segment.clip) {
        return mapping;
    }
    if (!segment.clip->tempoSync) {
        mapping.linearBeat = segment.entryClipBeat +
                             (mediaTime - segment.alignTime) * segment.clip->bpm / 60.0;
        mapping.beat = mapping.linearBeat;
        return mapping;
    }
    const double musicBeat = clock.beatAt(mediaTime);
    const double alignBeat = clock.beatAt(segment.alignTime);
    mapping.linearBeat = segment.entryClipBeat + (musicBeat - alignBeat) * segment.rate;
    mapping.beat = mapping.linearBeat;
    mapping.rate = segment.rate;
    if (warp.expression == MotionExpression::Off || warp.intensity <= 0.0f) {
        return mapping;
    }
    float accentConfidence = 0.35f;
    const float accent =
        accentAtNextBoundary(segment, musicBeat, clock, timeline, &accentConfidence);
    if (timeline) {
        const double nextTime =
            clock.timeAtBeat(std::floor(musicBeat + 1e-9) + 1.0);
        accentConfidence = std::max(
            accentConfidence, timeline->accentConfidenceAt(nextTime, 0.12));
    }
    const float bass = timeline
                           ? timeline->bassAt(clock.timeAtBeat(
                                                  std::floor(musicBeat + 1e-9) + 1.0),
                                              0.12)
                           : 0.0f;
    // Fade the warp in after a seek/rebase/transition, and fade it out around
    // silence. This keeps the correction zero at the segment's start and avoids
    // a phase jump when a warp switches on or off.
    const double sinceAlign = musicBeat - alignBeat;
    const double ramp = std::clamp(sinceAlign, 0.0, 1.0);
    const double silence = timeline ? timeline->silenceWeightAt(mediaTime, 0.12) : 1.0;
    const double gate = ramp * silence;
    const float gatedAccent = static_cast<float>(accent * gate);
    const float gatedBass = static_cast<float>(bass * gate);
    if (gatedAccent <= 1e-4f && gatedBass <= 1e-4f) {
        return mapping;
    }
    WarpSample input;
    input.musicalBeat = musicBeat;
    input.accent = gatedAccent;
    input.bass = gatedBass;
    input.confidence = accentConfidence;
    input.footContact = footContact;
    input.silent = false;
    input.allow = true;
    const WarpResult warped = MusicalTimeWarp::sample(input, warp);
    if (!warped.active) {
        return mapping;
    }
    mapping.deviation = warped.deviation;
    mapping.beat = segment.entryClipBeat +
                   (musicBeat + mapping.deviation - alignBeat) * segment.rate;
    mapping.warpRate = warped.rate;
    mapping.rate = segment.rate * warped.rate;
    mapping.warpActive = true;
    mapping.accent = accent;
    return mapping;
}

bool anyFootContact(const ClipPose& pose) {
    for (const ClipPose::Foot& foot : pose.foot) {
        if (foot.contact >= 0.5f) {
            return true;
        }
        switch (foot.phase) {
        case FootPhase::Stance:
        case FootPhase::Pivot:
        case FootPhase::Plant:
            return true;
        default:
            break;
        }
    }
    return false;
}

ClipPose adaptPose(const ClipPose& stable, const ClipPose& warped, bool fullBody,
                   bool contact) {
    ClipPose out = stable;
    const bool moveLower = fullBody && !contact;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto bone = static_cast<vrm::HumanoidBone>(b);
        if (upperBodyBone(bone) || moveLower) {
            out.bone[b] = warped.bone[b];
        }
    }
    if (moveLower) {
        out.hips = warped.hips;
    } else {
        out.hips = stable.hips;
    }
    out.rootX = stable.rootX;
    out.rootZ = stable.rootZ;
    out.rootYaw = stable.rootYaw;
    out.foot = stable.foot;
    out.hand = warped.hand;
    // The detailed hand layer (fingers + wrist) is a time-varying channel just
    // like the scalar curl: it must come from the same warped sample, otherwise
    // the hand and its fingers sit in different musical phases.
    out.handPose = warped.handPose;
    out.detailedHands = warped.detailedHands;
    out.energy = stable.energy;
    out.accent = std::max(stable.accent, warped.accent);
    return out;
}

// BG Motion 3.4 variety. Every loop of a looping clip gets its own amplitude for the spine,
// head and arm groups: a deterministic scale in [1 - span, 1 + span] that depends only on
// (clip id, loop index, group). The swing is scaled about the clip's mean pose of each bone,
// not about the bind pose, so the posture stays and only the swing grows or shrinks. The
// scale changes between loops only inside a window around the seam, so the pose is continuous.
constexpr float kAmplitudeSpan = 0.08f;
constexpr double kAmplitudeSeamBeats = 0.5;
constexpr std::size_t kAmplitudeGroups = 3;

// -1: not scaled (hips, legs, feet, eyes: planted contacts and the root must not change).
int amplitudeGroupOf(std::size_t bone) {
    using B = vrm::HumanoidBone;
    const auto at = [](B b) { return static_cast<std::size_t>(b); };
    if (bone == at(B::Spine) || bone == at(B::Chest) || bone == at(B::UpperChest)) return 0;
    if (bone == at(B::Neck) || bone == at(B::Head)) return 1;
    if ((bone >= at(B::LeftShoulder) && bone <= at(B::LeftHand)) ||
        (bone >= at(B::RightShoulder) && bone <= at(B::RightHand))) {
        return 2;
    }
    return -1;
}

float loopAmplitude(std::uint64_t clipHash, std::int64_t loop, std::size_t group) {
    // The first pass of a clip is the authored choreography: its accents keep their exact pose.
    // Variation starts with the second loop.
    if (loop == 0) {
        return 1.0f;
    }
    const std::uint64_t h = variationMix(clipHash ^ variationMix(static_cast<std::uint64_t>(loop)) ^
                                         variationMix(0x100u + group));
    const double unit = static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);  // [0, 1)
    return 1.0f + kAmplitudeSpan * static_cast<float>(2.0 * unit - 1.0);
}

// Mean rotation of one bone over the loop (a seamless track repeats its first key last, so
// the last key is left out). Empty track = bind pose = identity.
Quat trackMean(const std::vector<Quat>& track, int keyCount) {
    if (keyCount < 3 || track.size() != static_cast<std::size_t>(keyCount)) {
        return Quat::identity();
    }
    const int last = keyCount - 1;
    const int samples = std::min(32, last);
    const Quat reference = track[0];
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    for (int s = 0; s < samples; ++s) {
        const Quat& q = track[static_cast<std::size_t>(s * last / samples)];
        const float sign = dot(q, reference) < 0.0f ? -1.0f : 1.0f;
        x += sign * q.x;
        y += sign * q.y;
        z += sign * q.z;
        w += sign * q.w;
    }
    return Quat{x, y, z, w}.normalized();
}

// mean * (mean^-1 q)^scale: the swing away from the mean posture, scaled by its angle.
Quat scaleSwing(const Quat& q, const Quat& mean, float scale) {
    Quat d = (mean.conjugate() * q).normalized();
    if (d.w < 0.0f) {
        d = {-d.x, -d.y, -d.z, -d.w};
    }
    const float vectorLength = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float halfAngle = std::atan2(vectorLength, d.w);
    const float scaledHalf = halfAngle * scale;
    const float k = vectorLength > 1e-7f ? std::sin(scaledHalf) / vectorLength : scale;
    return (mean * Quat{k * d.x, k * d.y, k * d.z, std::cos(scaledHalf)}).normalized();
}

void applyLoopAmplitude(ClipPose& pose, const HumanoidClip& clip, double clipBeat) {
    if (!clip.loop || !(clip.beats > 0.0f)) {
        return;
    }
    const LoopBlend blend = loopBlendAt(clipBeat, clip.beats, kAmplitudeSeamBeats);
    const std::uint64_t hash = variationHash(clip.id);
    std::array<float, kAmplitudeGroups> scale{};
    for (std::size_t group = 0; group < kAmplitudeGroups; ++group) {
        const float from = loopAmplitude(hash, blend.from, group);
        const float to = loopAmplitude(hash, blend.to, group);
        scale[group] = from + (to - from) * blend.w;
    }
    for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
        const int group = amplitudeGroupOf(bone);
        if (group < 0 || scale[static_cast<std::size_t>(group)] == 1.0f) {
            continue;  // unscaled groups keep the authored rotation bit for bit
        }
        const Quat mean = trackMean(clip.bones[bone], clip.keyCount);
        pose.bone[bone] = scaleSwing(pose.bone[bone], mean, scale[static_cast<std::size_t>(group)]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// DanceSchedule
// ---------------------------------------------------------------------------

void DanceSchedule::append(ScheduleSegment segment) {
    if (!segment.clip) {
        return;
    }
    if (!segments_.empty() && segment.startTime <= segments_.back().startTime) {
        segment.startTime = segments_.back().startTime + 1e-3;
    }
    segments_.push_back(std::move(segment));
}

void DanceSchedule::append(ScheduleSegment segment, const MusicalTimeline& timeline,
                           const WarpProfile& warp) {
    const std::size_t before = segments_.size();
    append(std::move(segment));
    if (segments_.size() > before && segments_.size() > 1) {
        const std::size_t index = segments_.size() - 1;
        for (int foot = 0; foot < 2; ++foot) {
            segments_[index].footSwing[static_cast<std::size_t>(foot)] =
                findFootSwing(index, foot, timeline, warp);
        }
    }
}

void DanceSchedule::refreshFootSwings(const MusicalTimeline& timeline, const WarpProfile& warp) {
    for (std::size_t index = 1; index < segments_.size(); ++index) {
        for (int foot = 0; foot < 2; ++foot) {
            segments_[index].footSwing[static_cast<std::size_t>(foot)] =
                findFootSwing(index, foot, timeline, warp);
        }
    }
}

void DanceSchedule::truncateAfter(double time) {
    while (segments_.size() > 1 && segments_.back().startTime > time) {
        segments_.pop_back();
    }
}

void DanceSchedule::pruneBefore(double time, std::size_t keepAtLeast) {
    // Drop segments whose blend finished long before `time`; the newest
    // surviving one is the new base and no longer needs a predecessor.
    while (segments_.size() > keepAtLeast && handOverEnd(segments_[1]) < time - 5.0) {
        segments_.erase(segments_.begin());
    }
}

double DanceSchedule::segmentClipBeat(const ScheduleSegment& segment, double mediaTime,
                                      const BeatClock& clock) {
    if (!segment.clip) {
        return 0.0;
    }
    if (!segment.clip->tempoSync) {
        return segment.entryClipBeat +
               (mediaTime - segment.alignTime) * segment.clip->bpm / 60.0;
    }
    return segment.entryClipBeat +
           (clock.beatAt(mediaTime) - clock.beatAt(segment.alignTime)) * segment.rate;
}

double DanceSchedule::segmentClipBeat(const ScheduleSegment& segment, double mediaTime,
                                      const MusicalTimeline& timeline,
                                      const WarpProfile& warp, double* rate,
                                      bool* warpActive, double* deviation) {
    const Mapping mapping =
        mapSegment(segment, mediaTime, timeline.grid(), &timeline, warp, false);
    if (rate) {
        *rate = mapping.rate;
    }
    if (warpActive) {
        *warpActive = mapping.warpActive;
    }
    if (deviation) {
        *deviation = mapping.deviation;
    }
    return mapping.beat;
}

namespace {

// Bones carried by the inertial transition: spine, head and both arms with hands.
// Hips, legs and feet keep the smootherstep blend (planted contacts must not drift).
bool isInertialBone(std::size_t bone) {
    using B = vrm::HumanoidBone;
    const auto at = [](B b) { return static_cast<std::size_t>(b); };
    return (bone >= at(B::Spine) && bone <= at(B::Head)) ||
           (bone >= at(B::LeftShoulder) && bone <= at(B::LeftHand)) ||
           (bone >= at(B::RightShoulder) && bone <= at(B::RightHand));
}

// Inertialization of a bone offset (BG Motion 3.3, A8). The offset is a rotation vector o
// (axis times angle). In decay time x = elapsed * 5 / fade it follows
//   o(x) = o0 p(x) + v q(x) - g(x) (o0 p(5) + v q(5)) - h(x) S,
// with p(x) = (1 + x) e^-x and q(x) = x e^-x, so o(0) = o0 and the slope at 0 is the offset's
// own velocity v (in decay time). g(x) = u^2 (3 - 2u), u = min(x / 5, 1), brings the offset to
// exactly 0 at the fade's end; h(x) = u^2 (u - 1) times S, the slope (per u) of the first
// two terms at x = 5, makes the slope 0 there too, so the hand-over to the incoming clip has
// no step in value or velocity.
// The offset's velocity is carried through the fade start: the outgoing motion goes on, and
// the incoming clip's own motion is added to it.
using RotationVector = std::array<double, 3>;

// The rotation vector of q as given: the caller picks the quaternion sign. Flipping it here
// instead would move the vector to the opposite side at a half turn (w changing sign between
// two samples), which reads as a velocity spike.
RotationVector rotationVectorOf(const Quat& q) {
    const double x = q.x;
    const double y = q.y;
    const double z = q.z;
    const double w = q.w;
    const double s = std::sqrt(x * x + y * y + z * z);
    const double angle = 2.0 * std::atan2(s, w);
    const double k = s > 1.0e-12 ? angle / s : 2.0;
    return {k * x, k * y, k * z};
}

Quat quaternionOf(const RotationVector& v) {
    const double angle = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (angle < 1.0e-12) {
        return Quat{static_cast<float>(v[0] * 0.5), static_cast<float>(v[1] * 0.5),
                    static_cast<float>(v[2] * 0.5), 1.0f}
            .normalized();
    }
    const double k = std::sin(angle * 0.5) / angle;
    return Quat{static_cast<float>(k * v[0]), static_cast<float>(k * v[1]),
                static_cast<float>(k * v[2]), static_cast<float>(std::cos(angle * 0.5))}
        .normalized();
}

// `o0` is the offset at the transition start, `velocity` its rate of change (rad/s).
Quat inertialOffset(const RotationVector& o0, const RotationVector& velocity, double elapsed,
                    double fadeSeconds) {
    constexpr double kLambda = 5.0;
    // Velocity in decay time: d/dx = (fade / lambda) d/dt.
    const double scale = fadeSeconds / kLambda;
    const double x = std::min(std::max(elapsed, 0.0) / fadeSeconds, 1.0) * kLambda;
    const auto raw = [&](double at) {
        const double p = (1.0 + at) * std::exp(-at);
        const double q = at * std::exp(-at);
        RotationVector r{};
        for (std::size_t i = 0; i < 3; ++i) {
            r[i] = o0[i] * p + velocity[i] * scale * q;
        }
        return r;
    };
    const RotationVector atX = raw(x);
    const RotationVector atEnd = raw(kLambda);
    // Slope of raw at the end, per unit u = x / lambda: p'(x) = -x e^-x, q'(x) = (1 - x) e^-x.
    const double e = std::exp(-kLambda);
    RotationVector slopeEnd{};
    for (std::size_t i = 0; i < 3; ++i) {
        slopeEnd[i] = kLambda * (o0[i] * (-kLambda * e) + velocity[i] * scale * (1.0 - kLambda) * e);
    }
    // Cubic Hermite correction: g takes the end value away (g(1) = 1, g'(1) = 0) and h the end
    // slope (h(1) = 0, h'(1) = 1); both are flat at u = 0, so the start keeps o0 and its slope.
    const double u = x / kLambda;
    const double g = u * u * (3.0 - 2.0 * u);
    const double hEnd = u * u * (u - 1.0);
    RotationVector o{};
    for (std::size_t i = 0; i < 3; ++i) {
        o[i] = atX[i] - g * atEnd[i] - hEnd * slopeEnd[i];
    }
    return quaternionOf(o);
}

} // namespace

namespace {

// BG Motion 3.3 (#9) follow-through. A spring x'' + c x' + k x = -g w(t), driven by the
// bone's angular velocity w, is evaluated as a finite convolution with its impulse
// response over a fixed window of history. The window is clamped to the segment start.
constexpr double kFollowPi = 3.14159265358979323846;
constexpr double kFollowHz = 2.5;        // natural frequency of the lag
constexpr double kFollowDamping = 0.45;  // damping ratio
constexpr double kFollowGain = 6.0;      // drive, 1/s
constexpr double kFollowStep = 1.0 / 120.0;
constexpr double kFollowWindow = 0.6;    // seconds of history
constexpr float kFollowMaxRad = 0.2f;    // cap on the lag per bone

bool isFollowBone(std::size_t bone) {
    using B = vrm::HumanoidBone;
    return bone == static_cast<std::size_t>(B::Head) ||
           bone == static_cast<std::size_t>(B::LeftHand) ||
           bone == static_cast<std::size_t>(B::RightHand);
}

// Impulse response of x'' + c x' + k x = u, for tau >= 0.
double followImpulse(double tau) {
    const double w0 = 2.0 * kFollowPi * kFollowHz;
    const double k = w0 * w0;
    const double c = 2.0 * kFollowDamping * w0;
    const double wd = std::sqrt(k - c * c / 4.0);
    return std::exp(-c * tau / 2.0) * std::sin(wd * tau) / wd;
}

// Angular velocity (rad/s, local frame) between two orientations one step apart.
Vec3 angularVelocity(const Quat& earlier, const Quat& later) {
    Quat rel = (later * conjugate(earlier)).normalized();
    if (rel.w < 0.0f) {
        rel = {-rel.x, -rel.y, -rel.z, -rel.w};
    }
    const float scale = static_cast<float>(2.0 / kFollowStep);
    return {rel.x * scale, rel.y * scale, rel.z * scale};
}

} // namespace

void DanceSchedule::applyFollowThrough(ClipPose& pose, std::size_t index, double mediaTime,
                                       const MusicalTimeline& timeline,
                                       const WarpProfile& warp) const {
    const double startTime = segments_[index].startTime;
    const int samples = static_cast<int>(std::lround(kFollowWindow / kFollowStep));
    // Base poses at the window times, newest first: entry j is at mediaTime - j * step.
    std::vector<ClipPose> history;
    history.reserve(static_cast<std::size_t>(samples + 2));
    for (int j = 0; j <= samples + 1; ++j) {
        const double t = std::max(startTime, mediaTime - static_cast<double>(j) * kFollowStep);
        history.push_back(basePose(index, t, timeline, warp, nullptr));
    }
    for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
        if (!isFollowBone(bone)) {
            continue;
        }
        Vec3 lag{};
        for (int j = 0; j <= samples; ++j) {
            const auto at = static_cast<std::size_t>(j);
            const Vec3 w = angularVelocity(history[at + 1].bone[bone], history[at].bone[bone]);
            const auto weight = static_cast<float>(
                -kFollowGain * followImpulse(static_cast<double>(j) * kFollowStep) * kFollowStep);
            lag = lag + w * weight;
        }
        const float length = std::sqrt(dot(lag, lag));
        if (length > kFollowMaxRad) {
            lag = lag * (kFollowMaxRad / length);
        }
        // Small-angle rotation vector as a quaternion, applied in the parent frame.
        const Quat offset = Quat{lag.x * 0.5f, lag.y * 0.5f, lag.z * 0.5f, 1.0f}.normalized();
        pose.bone[bone] = (offset * pose.bone[bone]).normalized();
    }
}

ClipPose DanceSchedule::basePose(std::size_t index, double mediaTime,
                                 const MusicalTimeline& timeline,
                                 const WarpProfile& warp, double* clipBeat) const {
    const ScheduleSegment& segment = segments_[index];
    const Mapping stable = mapSegment(segment, mediaTime, timeline.grid(), &timeline,
                                      WarpProfile{MotionExpression::Off, 0.0f, true, 0.0f},
                                      false);
    ClipPose linear = sampleClip(*segment.clip, stable.linearBeat);
    const bool contact = anyFootContact(linear);

    Mapping mapped = stable;
    ClipPose own = linear;
    if (warp.expression != MotionExpression::Off && warp.intensity > 0.0f) {
        mapped = mapSegment(segment, mediaTime, timeline.grid(), &timeline, warp, contact);
        if (mapped.warpActive &&
            std::abs(mapped.beat - stable.linearBeat) > 1e-6) {
            const ClipPose warped = sampleClip(*segment.clip, mapped.beat);
            own = adaptPose(linear, warped, !warp.upperBodyOnly, contact);
        }
    }
    applyLoopAmplitude(own, *segment.clip, mapped.beat);
    if (clipBeat) {
        *clipBeat = mapped.beat;
    }
    return own;
}

namespace {

// Grid samples of one segment's foot contact, from the start of the fade.
struct SwingSamples {
    int begin = -1;  // sample index where the foot leaves the ground
    int end = -1;    // sample index where it lands again
};

SwingSamples findSwingSamples(const ScheduleSegment& segment, int foot, double start,
                              const MusicalTimeline& timeline, const WarpProfile& warp) {
    // The feet of a pose always come from the linear (unwarped) clip beat: the warp moves
    // the upper body only. The search reads the same beat, so it agrees with the pose.
    const auto contactAt = [&](int k) {
        if (!segment.clip) return 0.0f;
        const Mapping mapping = mapSegment(segment, start + k * kHandOverStep, timeline.grid(),
                                           &timeline, warp, false);
        return footContactAt(*segment.clip, foot, mapping.linearBeat);
    };
    SwingSamples swing;
    float previous = contactAt(0);
    const float next = contactAt(1);
    for (int k = 0; k <= kHandOverSamples; ++k) {
        const float c = k == 0 ? previous : contactAt(k);
        // A swing starts where the contact falls below one half (a lift); a foot that is
        // already low and not rising at the fade start counts as swinging from the start.
        // A planting ramp (rising contact) is not a swing.
        const bool lifts = k == 0 ? (c < 0.5f && next <= c + 1e-6f)
                                  : (previous >= 0.5f && c < 0.5f);
        if (swing.begin < 0 && lifts) {
            swing.begin = k;
        } else if (swing.begin >= 0 && c >= 0.5f) {
            // A remainder of a swing that lands within a few frames cannot carry the
            // offset without a jump: look for the next lift instead.
            if ((k - swing.begin) * kHandOverStep >= kHandOverMinSwing) {
                swing.end = k;
                break;
            }
            swing.begin = -1;
        }
        previous = c;
    }
    return swing;
}

} // namespace

ScheduleSegment::FootSwingWindow DanceSchedule::findFootSwing(std::size_t index, int foot,
                                                              const MusicalTimeline& timeline,
                                                              const WarpProfile& warp) const {
    const ScheduleSegment& incoming = segments_[index];
    const ScheduleSegment& outgoing = segments_[index - 1];
    const double start = incoming.startTime;
    // The hand-over follows whichever swing of this foot comes first: the incoming clip's own
    // swing, or the outgoing clip's next lift when the incoming clip keeps the foot planted.
    // Only complete swings (lift and landing both inside the window) can carry the foot; an
    // outgoing lift without a landing must not hide a complete incoming swing.
    const SwingSamples in = findSwingSamples(incoming, foot, start, timeline, warp);
    const SwingSamples out = findSwingSamples(outgoing, foot, start, timeline, warp);
    const bool inComplete = in.begin >= 0 && in.end >= 0;
    const bool outComplete = out.begin >= 0 && out.end >= 0;
    const bool useIncoming = inComplete && (!outComplete || in.begin <= out.begin);
    const bool useOutgoing = !useIncoming && outComplete;

    ScheduleSegment::FootSwingWindow window;
    window.resolved = true;
    if (!useIncoming && !useOutgoing) {
        // Neither clip swings this foot in the window: time-based hand-over inside the fade.
        // The foot slides here; the entry choice keeps this rare.
        window.slide = true;
        return window;
    }
    const SwingSamples& swing = useIncoming ? in : out;
    const ScheduleSegment& source = useIncoming ? incoming : outgoing;
    window.begin = start + swing.begin * kHandOverStep;
    window.end = start + swing.end * kHandOverStep;
    window.beginBeat = segmentClipBeat(source, window.begin, timeline.grid());
    window.endBeat = segmentClipBeat(source, window.end, timeline.grid());
    window.fromOutgoing = !useIncoming;
    return window;
}

std::array<DanceSchedule::FootHandOver, 2> DanceSchedule::footHandOver(
    std::size_t index, double mediaTime, const MusicalTimeline& timeline,
    const WarpProfile& warp) const {
    const ScheduleSegment& incoming = segments_[index];
    const double start = incoming.startTime;
    const double fade = incoming.fadeSeconds;
    const double linear = (mediaTime - start) / fade;

    std::array<FootHandOver, 2> out;
    for (int f = 0; f < 2; ++f) {
        FootHandOver& hand = out[static_cast<std::size_t>(f)];
        const auto& stored = incoming.footSwing[static_cast<std::size_t>(f)];
        const ScheduleSegment::FootSwingWindow window =
            stored.resolved ? stored : findFootSwing(index, f, timeline, warp);
        if (window.slide) {
            hand.progress = smootherstep(static_cast<float>(linear));
            hand.active = linear < 1.0;
            continue;
        }
        // Progress runs in the swing's own clip beats (the pose's feet read the same linear
        // beat), so it stays on the swing when a rebase changes the tempo.
        const ScheduleSegment& source = window.fromOutgoing ? segments_[index - 1] : incoming;
        const double bA = window.beginBeat;
        const double bB = window.endBeat;
        const double beat = segmentClipBeat(source, mediaTime, timeline.grid());
        if (bB <= bA || beat <= bA) {
            hand.progress = bB <= bA && mediaTime >= window.end ? 1.0f : 0.0f;
            hand.active = hand.progress < 1.0f;
        } else if (beat >= bB) {
            hand.progress = 1.0f;
            hand.active = false;
        } else {
            hand.progress = smootherstep(static_cast<float>((beat - bA) / (bB - bA)));
            hand.active = true;
        }
    }
    return out;
}

ClipPose DanceSchedule::ownPose(std::size_t index, double mediaTime,
                                const MusicalTimeline& timeline,
                                const WarpProfile& warp) const {
    double clipBeat = 0.0;
    ClipPose own = basePose(index, mediaTime, timeline, warp, &clipBeat);
    if (followThrough_) {
        applyFollowThrough(own, index, mediaTime, timeline, warp);
    }
    if (decorator_) {
        decorator_(own, segments_[index], clipBeat);
    }
    return own;
}

ClipPose DanceSchedule::poseOf(std::size_t index, double mediaTime,
                               const MusicalTimeline& timeline,
                               const WarpProfile& warp, float* transition,
                               PoseMemo& memo) const {
    const auto key = std::make_pair(index, mediaTime);
    if (const auto found = memo.find(key); found != memo.end()) {
        return found->second;
    }
    const ScheduleSegment& segment = segments_[index];
    ClipPose result;
    if (index > 0 && segment.fadeSeconds > 1e-4) {
        const double u = (mediaTime - segment.startTime) / segment.fadeSeconds;
        // A planted foot can outlast the fade: its hand-over ends at the incoming swing.
        const bool mayHoldFeet = mediaTime < handOverEnd(segment);
        if (u < 1.0 || mayHoldFeet) {
            if (transition && u < 1.0) {
                *transition = static_cast<float>(std::clamp(u, 0.0, 1.0));
            }
            // The incoming pose is evaluated first, as before: decorator consumers rely on
            // its call being the first one of a sample.
            const ClipPose own = ownPose(index, mediaTime, timeline, warp);
            if (u <= 0.0) {
                result = poseOf(index - 1, mediaTime, timeline, warp, nullptr, memo);
                memo.emplace(key, result);
                return result;
            }
            const ClipPose previous = poseOf(index - 1, mediaTime, timeline, warp, nullptr, memo);
            const auto hand = footHandOver(index, mediaTime, timeline, warp);
            const bool feetHeld = hand[0].active || hand[1].active;
            if (u >= 1.0 && !feetHeld) {
                memo.emplace(key, own);
                return own;
            }
            if (u < 1.0) {
                result = blendPoses(previous, own, smootherstep(static_cast<float>(u)));
            } else {
                result = own;
            }
            // A1: each planted foot is handed over by its own swing, not by the time blend.
            for (std::size_t f = 0; f < 2; ++f) {
                ClipPose::Foot& foot = result.foot[f];
                const ClipPose::Foot& from = previous.foot[f];
                const ClipPose::Foot& to = own.foot[f];
                // Contact and phase follow the same progress as the position, so a foot
                // that is moving is never reported as planted.
                const float progress = hand[f].progress;
                foot.position = from.position + (to.position - from.position) * progress;
                float yawDelta = to.yaw - from.yaw;
                yawDelta -= std::round(yawDelta);
                foot.yaw = from.yaw + yawDelta * progress;
                foot.contact = from.contact + (to.contact - from.contact) * progress;
                foot.phase = progress < 0.5f ? from.phase : to.phase;
            }
            if (u < 1.0 && inertialTransitions_) {
                // Inertialization: the offset between the outgoing and incoming upper body
                // is fixed at the transition start, so it depends only on the segment and
                // not on how the time was reached (seek and playback stay identical).
                // The offset's velocity at the start is sampled one step later, so the
                // outgoing motion is carried through the fade start, not frozen there.
                const double startTime = segment.startTime;
                constexpr double kVelocityStep = 1.0 / 240.0;
                const ClipPose outgoingAtStart =
                    poseOf(index - 1, startTime, timeline, warp, nullptr, memo);
                const ClipPose incomingAtStart = ownPose(index, startTime, timeline, warp);
                const ClipPose outgoingNext =
                    poseOf(index - 1, startTime + kVelocityStep, timeline, warp, nullptr, memo);
                const ClipPose incomingNext =
                    ownPose(index, startTime + kVelocityStep, timeline, warp);
                for (std::size_t bone = 0; bone < vrm::kHumanoidBoneCount; ++bone) {
                    if (!isInertialBone(bone)) {
                        continue;
                    }
                    Quat offset =
                        (outgoingAtStart.bone[bone] * conjugate(incomingAtStart.bone[bone]))
                            .normalized();
                    Quat offsetNext =
                        (outgoingNext.bone[bone] * conjugate(incomingNext.bone[bone])).normalized();
                    // q and -q are the same rotation. The start takes the minimal (w >= 0)
                    // representation, and the next sample is put in the same hemisphere, so the
                    // velocity is the change of one continuous rotation vector.
                    if (offset.w < 0.0f) {
                        offset = Quat{-offset.x, -offset.y, -offset.z, -offset.w};
                    }
                    if (dot(offset, offsetNext) < 0.0f) {
                        offsetNext = Quat{-offsetNext.x, -offsetNext.y, -offsetNext.z,
                                          -offsetNext.w};
                    }
                    const RotationVector before = rotationVectorOf(offset);
                    const RotationVector after = rotationVectorOf(offsetNext);
                    RotationVector velocity{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        velocity[i] = (after[i] - before[i]) / kVelocityStep;
                    }
                    const Quat carried = inertialOffset(before, velocity, mediaTime - startTime,
                                                        segment.fadeSeconds);
                    result.bone[bone] = (carried * own.bone[bone]).normalized();
                }
            }
            memo.emplace(key, result);
            return result;
        }
    }
    result = ownPose(index, mediaTime, timeline, warp);
    memo.emplace(key, result);
    return result;
}

DanceFrame DanceSchedule::sample(double mediaTime, const BeatClock& clock) const {
    // Grid-only timeline for the legacy entry point. It keeps the exact clock
    // (including its bar offset), so diagnostics remain meaningful.
    MusicalTimeline timeline = MusicalTimeline::fromClock(clock, true);
    WarpProfile off;
    off.expression = MotionExpression::Off;
    return sample(mediaTime, clock, timeline, off);
}

DanceFrame DanceSchedule::sample(double mediaTime, const BeatClock& clock,
                                 const MusicalTimeline& timeline,
                                 const WarpProfile& warp) const {
    DanceFrame frame;
    frame.pose = neutralClipPose();
    frame.musicBeat = clock.beatAt(mediaTime);
    frame.beatsPerBar = timeline.beatsPerBar();
    frame.meterConfidence = timeline.meterConfidence();
    frame.downbeatConfidence = timeline.downbeatConfidence();
    frame.barPhaseTrusted = timeline.downbeatTrusted();
    if (segments_.empty()) {
        return frame;
    }
    std::size_t k = 0;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].startTime <= mediaTime) {
            k = i;
        } else {
            break;
        }
    }
    float transition = -1.0f;
    PoseMemo memo;
    frame.pose = poseOf(k, mediaTime, timeline, warp, &transition, memo);
    frame.transitionProgress = transition;

    const ScheduleSegment& current = segments_[k];
    {
        const auto isIdle = [](const ScheduleSegment& s) {
            return s.clip && s.clip->style == DanceStyle::Idle ? 1.0f : 0.0f;
        };
        const float incoming = isIdle(current);
        frame.idleWeight = transition >= 0.0f && k > 0
                               ? isIdle(segments_[k - 1]) +
                                     (incoming - isIdle(segments_[k - 1])) * transition
                               : incoming;
    }
    const ScheduleSegment* shown = &current;
    const ScheduleSegment* next = k + 1 < segments_.size() ? &segments_[k + 1] : nullptr;
    if (transition >= 0.0f && transition < 0.5f && k > 0) {
        // While crossfading the outgoing clip is still "current".
        shown = &segments_[k - 1];
        next = &current;
    }
    frame.clipId = shown->clip->id;
    frame.clipName = shown->clip->name;
    frame.style = shown->clip->style;
    frame.nextClipId = next ? next->clip->id : std::string();
    frame.reason = current.reason;
    const Mapping mapping = mapSegment(*shown, mediaTime, clock, &timeline, warp, false);
    frame.clipBeat = mapping.beat;
    frame.unwarpedClipBeat = mapping.linearBeat;
    frame.warpDeviationBeats = mapping.deviation;
    frame.warpRate = mapping.warpRate;
    frame.warpActive = mapping.warpActive;
    frame.musicAccent = std::max(
        mapping.accent, timeline.accentAt(mediaTime, 0.10));
    frame.bassStrength = timeline.bassAt(mediaTime, 0.10);
    const double beat = mapping.beat;
    const double length = std::max(1.0, static_cast<double>(shown->clip->beats));
    double wrapped = std::fmod(beat, length);
    if (wrapped < 0.0) {
        wrapped += length;
    }
    if (!shown->loop && beat >= length) {
        frame.oneShotFinished = true;
        wrapped = length;
    }
    frame.phase = static_cast<float>(wrapped / length);
    frame.bpm = clock.bpmAt(mediaTime);
    if (shown->clip->tempoSync) {
        const double effective = frame.bpm * shown->rate;
        frame.playbackSpeed = static_cast<float>(effective / shown->clip->bpm);
        frame.effectiveSpeed = static_cast<float>(
            frame.bpm * mapping.rate / shown->clip->bpm);
        frame.speedLimited =
            effective < shown->clip->bpmMin - 0.5 || effective > shown->clip->bpmMax + 0.5;
    } else {
        frame.playbackSpeed = 1.0f;
        frame.effectiveSpeed = 1.0f;
    }
    frame.beatInBar = timeline.beatInBarAt(mediaTime);
    frame.barIndex = timeline.barAt(mediaTime);
    frame.downbeat = timeline.downbeatAt(mediaTime);
    frame.alignmentErrorMs = shown->alignmentErrorSeconds * 1000.0;
    return frame;
}

// ---------------------------------------------------------------------------
// DanceDirector
// ---------------------------------------------------------------------------

void DanceDirector::setBeatClock(BeatClock clock) {
    clock_ = std::move(clock);
    timeline_ = MusicalTimeline::fromClock(clock_, true);
    schedule_.refreshFootSwings(timeline_, warp_);
}

void DanceDirector::setMusicalTimeline(MusicalTimeline timeline) {
    timeline_ = std::move(timeline);
    clock_ = timeline_.grid();
    schedule_.refreshFootSwings(timeline_, warp_);
}

const HumanoidClip* DanceDirector::clipFor(DanceStyle style) const {
    return library_ ? library_->forStyle(style) : nullptr;
}

double DanceDirector::chooseRate(const HumanoidClip& clip, double musicBpm, bool* limited) {
    static constexpr double kLevels[] = {1.0, 0.5, 2.0};
    double best = 1.0;
    double bestScore = 1e9;
    bool bestInRange = false;
    double level1Score = 1e9;
    bool level1InRange = false;
    for (double level : kLevels) {
        const double effective = musicBpm * level;
        const bool inRange =
            effective >= clip.bpmMin - 0.5 && effective <= clip.bpmMax + 0.5;
        // A soft range penalty keeps a slightly-fast 1x reading instead of
        // forcing a large half-time jump (180 BPM Energetic -> 90 BPM). The
        // clip still reports speedLimited, so the choice is honest.
        double rangePenalty = 0.0;
        if (effective > clip.bpmMax) {
            rangePenalty = 2.0 * std::log(effective / std::max(1.0, clip.bpmMax));
        } else if (effective < clip.bpmMin) {
            rangePenalty = 2.0 * std::log(std::max(1.0, clip.bpmMin) / effective);
        }
        const double score =
            std::abs(std::log(effective / clip.bpm)) + rangePenalty;
        if (level == 1.0) {
            level1Score = score;
            level1InRange = inRange;
        }
        if (score < bestScore) {
            bestScore = score;
            best = level;
            bestInRange = inRange;
        }
    }
    // Hysteresis: do not jump to 0.5/2.0 when 1x is within a small score
    // margin. A metric level is a musical decision, not a per-frame reaction.
    if (best != 1.0 && level1Score <= bestScore + 0.05) {
        best = 1.0;
        bestScore = level1Score;
        bestInRange = level1InRange;
    }
    if (limited) {
        *limited = !bestInRange;
    }
    return best;
}

double DanceDirector::fadeFor(double atTime) const {
    const double bpm = std::max(30.0, clock_.bpmAt(atTime));
    return std::clamp(2.0 * 60.0 / bpm, 0.45, 1.1);
}

double DanceDirector::entryTimeFor(const HumanoidClip& clip, double now,
                                   bool* alignedToDownbeat,
                                   double* alignmentError) const {
    if (alignedToDownbeat) {
        *alignedToDownbeat = false;
    }
    if (alignmentError) {
        *alignmentError = 0.0;
    }
    if (!clip.tempoSync) {
        return now;
    }
    const int clipBar = std::max(1, clip.beatsPerBar);
    const int meter = std::max(1, clock_.beatsPerBar());
    const double fromBeat = std::ceil(clock_.beatAt(now + 0.10));
    double firstCompatible = fromBeat;
    bool haveCompatible = false;
    for (int k = 0; k <= meter; ++k) {
        const double beat = fromBeat + k;
        const int inBar = clock_.beatInBar(beat);
        if (clip.entryBeats.empty()) {
            if (inBar == 0) {
                if (alignedToDownbeat) {
                    *alignedToDownbeat = timeline_.downbeatTrusted();
                }
                return clock_.timeAtBeat(beat);
            }
            continue;
        }
        for (float entry : clip.entryBeats) {
            const int entryIndex = static_cast<int>(std::lround(entry));
            if (entryIndex % clipBar != inBar % clipBar) {
                continue;
            }
            const bool downbeatEntry = timeline_.downbeatTrusted() && inBar == 0 &&
                                       entryIndex % clipBar == 0;
            if (downbeatEntry) {
                if (alignedToDownbeat) {
                    *alignedToDownbeat = true;
                }
                return clock_.timeAtBeat(beat);
            }
            if (!haveCompatible) {
                firstCompatible = beat;
                haveCompatible = true;
            }
            break;
        }
    }
    if (haveCompatible) {
        return clock_.timeAtBeat(firstCompatible);
    }
    return clock_.timeAtBeat(fromBeat);
}

double DanceDirector::nextAlignmentTime(double now) const {
    const HumanoidClip* clip = clipFor(selected_);
    if (!clip) {
        return now;
    }
    return entryTimeFor(*clip, now, nullptr, nullptr);
}

ScheduleSegment DanceDirector::makeSegment(const HumanoidClip& clip, double startTime,
                                           double alignTime, double fadeSeconds) const {
    ScheduleSegment segment;
    segment.clip = &clip;
    segment.startTime = startTime;
    segment.alignTime = alignTime;
    segment.fadeSeconds = fadeSeconds;
    segment.loop = true;
    segment.meter = timeline_.beatsPerBar();
    segment.barPhaseTrusted = timeline_.downbeatTrusted();
    if (!clip.tempoSync) {
        segment.rate = 1.0;
        segment.entryClipBeat = 0.0;
        segment.alignedToDownbeat = false;
        return segment;
    }
    segment.rate = chooseRate(clip, clock_.bpmAt(alignTime));
    const double beatPos = clock_.beatAt(alignTime);
    const int inBar = clock_.beatInBar(beatPos);
    const double fraction = beatPos - std::floor(beatPos + 1e-6);
    const double barLength = std::max(1, clip.beatsPerBar);
    const double desired =
        std::fmod((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
    double best = 0.0;
    double bestDistance = 1e9;
    std::vector<float> entries = clip.entryBeats;
    if (entries.empty()) {
        entries.push_back(0.0f);
    }
    std::sort(entries.begin(), entries.end());
    for (float entry : entries) {
        const double d = circularDistance(
            std::fmod(static_cast<double>(entry), barLength), desired, barLength);
        if (d < bestDistance - 1e-6) {
            bestDistance = d;
            best = entry;
        }
    }
    segment.entryClipBeat = best;
    segment.alignedToDownbeat =
        timeline_.downbeatTrusted() && inBar == 0 && fraction < 1e-4 &&
        std::abs(std::fmod(best, barLength)) < 1e-4;
    return segment;
}

void DanceDirector::reset(double now) {
    schedule_.clear();
    selected_ = DanceStyle::Idle;
    selectedClip_.clear();
    if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
        selectedClip_ = idle->id;
        ScheduleSegment segment;
        segment.clip = idle;
        segment.startTime = now - 1.0e6;
        segment.alignTime = now;
        segment.fadeSeconds = 0.0;
        segment.reason = "idle";
        segment.meter = timeline_.beatsPerBar();
        segment.barPhaseTrusted = timeline_.downbeatTrusted();
        schedule_.append(segment, timeline_, warp_);
    }
}

void DanceDirector::reseed(double now) {
    const HumanoidClip* clip = library_ ? library_->find(selectedClip_) : nullptr;
    if (!clip) {
        clip = clipFor(selected_);
    }
    if (!clip) {
        clip = clipFor(DanceStyle::Idle);
    }
    schedule_.clear();
    if (!clip) {
        return;
    }
    ScheduleSegment segment = makeSegment(*clip, now - 1.0e6, now, 0.0);
    if (clip->tempoSync) {
        const double beatPos = clock_.beatAt(now);
        const int inBar = clock_.beatInBar(beatPos);
        const double fraction = beatPos - std::floor(beatPos + 1e-6);
        const double barLength = std::max(1, clip->beatsPerBar);
        if (timeline_.downbeatTrusted()) {
            segment.entryClipBeat =
                wrapPositive((inBar + std::max(0.0, fraction)) * segment.rate, barLength);
            segment.alignedToDownbeat = inBar == 0 && fraction < 1e-4;
        } else {
            // No trusted bar: preserve the fractional beat phase only.
            segment.entryClipBeat =
                wrapPositive(std::max(0.0, fraction) * segment.rate, barLength);
            segment.alignedToDownbeat = false;
        }
        segment.barPhaseTrusted = timeline_.downbeatTrusted();
    } else {
        segment.entryClipBeat = 0.0;
    }
    segment.reason = "re-aligned after seek";
    schedule_.append(segment, timeline_, warp_);
}

void DanceDirector::rebase(double now, BeatClock newClock) {
    if (schedule_.empty()) {
        setBeatClock(std::move(newClock));
        return;
    }
    const BeatClock before = clock_;
    clock_ = std::move(newClock);
    timeline_ = MusicalTimeline::fromClock(clock_, true);
    reanchorSchedule(now, before);
}

void DanceDirector::rebase(double now, MusicalTimeline newTimeline) {
    if (schedule_.empty()) {
        setMusicalTimeline(std::move(newTimeline));
        return;
    }
    const BeatClock before = clock_;
    timeline_ = std::move(newTimeline);
    clock_ = timeline_.grid();
    reanchorSchedule(now, before);
}

void DanceDirector::reanchorSchedule(double now, const BeatClock& before) {
    // Segments are kept, not collapsed: the schedule holds the outgoing and incoming
    // clips of a crossfade in progress and any queued one-shot return.
    const std::vector<ScheduleSegment> old = schedule_.segments();
    std::size_t first = old.size();
    for (std::size_t i = 0; i < old.size(); ++i) {
        if (handOverEnd(old[i]) > now) {
            first = i;
            break;
        }
    }
    // The segment before `first` is the base at `now`, or the outgoing side of the fade in
    // progress. Segments before it have finished blending and no longer affect the pose.
    const std::size_t keepFrom = first == 0 ? 0 : first - 1;
    std::vector<ScheduleSegment> kept;
    std::vector<bool> pending(old.size(), false);  // by index in `kept`
    for (std::size_t i = keepFrom; i < old.size(); ++i) {
        ScheduleSegment segment = old[i];
        if (!segment.clip) {
            continue;
        }
        if (segment.startTime <= now) {
            // Active or blending: the clip beat at `now` is preserved, and the mapping
            // restarts from `now` on the new grid. startTime and fadeSeconds are kept, so an
            // in-progress crossfade keeps its progress.
            segment.entryClipBeat = DanceSchedule::segmentClipBeat(segment, now, before);
            segment.alignTime = now;
            if (segment.clip->tempoSync) {
                segment.rate = chooseRate(*segment.clip, clock_.bpmAt(now));
            }
            segment.meter = timeline_.beatsPerBar();
            segment.barPhaseTrusted = timeline_.downbeatTrusted();
        } else {
            // Pending: keep the musical beat it was aligned to, placed on the new grid.
            const double oldAlign = segment.alignTime;
            double alignTime = clock_.timeAtBeat(before.beatAt(oldAlign));
            double fade = segment.fadeSeconds;
            const bool oneShotReturn = segment.reason == "one-shot finished" && !kept.empty() &&
                                       !kept.back().loop && kept.back().clip->tempoSync;
            if (oneShotReturn) {
                // A one-shot's return follows the one-shot's own end: the rebased clip keeps
                // its beat at `now` and plays its remaining beats at the new rate.
                const ScheduleSegment& shot = kept.back();
                alignTime = timeAtLinearClipBeat(shot, shot.clip->beats, clock_);
                fade = fadeFor(alignTime);
            } else if (alignTime < now) {
                // A faster grid moved the entry into the past: enter again as a fresh
                // selection at `now` would, so the clip does not start mid-way past its
                // entry (and past the swing that carries its planted feet over).
                double error = 0.0;
                bool aligned = false;
                alignTime = segment.clip->tempoSync
                                ? entryTimeFor(*segment.clip, now, &aligned, &error)
                                : now;
            }
            const bool loop = segment.loop;
            const std::string reason = segment.reason;
            const double alignmentError = segment.alignmentErrorSeconds;
            segment = makeSegment(*segment.clip, std::max(now, alignTime - fade * 0.5),
                                  alignTime, fade);
            segment.loop = loop;
            segment.reason = reason;
            segment.alignmentErrorSeconds = alignmentError;
            // Nothing of a pending segment has been shown yet, so its planted-foot swings
            // are searched again from its new start on the new grid (append below). Kept
            // windows could land before that start when a faster grid moves the entry into
            // the past, and the hand-over would then be complete on its first frame.
            pending[kept.size()] = true;
        }
        kept.push_back(std::move(segment));
    }
    // The swing windows' media seconds follow their clip beats on the new grid, so the
    // hand-over lifetime (handOverEnd) lasts until the rebased swing has landed.
    for (std::size_t k = 1; k < kept.size(); ++k) {
        if (pending[k]) {
            continue;
        }
        for (auto& window : kept[k].footSwing) {
            if (!window.resolved || window.slide) {
                continue;
            }
            const ScheduleSegment& source = window.fromOutgoing ? kept[k - 1] : kept[k];
            window.begin = timeAtLinearClipBeat(source, window.beginBeat, clock_);
            window.end = timeAtLinearClipBeat(source, window.endBeat, clock_);
        }
    }
    schedule_.clear();
    for (std::size_t k = 0; k < kept.size(); ++k) {
        if (pending[k]) {
            schedule_.append(std::move(kept[k]), timeline_, warp_);
        } else {
            schedule_.append(std::move(kept[k]));
        }
    }
}

void DanceDirector::select(DanceStyle style, double now, bool oneShot, bool immediate) {
    const HumanoidClip* clip = clipFor(style);
    if (!clip) {
        return;
    }
    if (schedule_.empty()) {
        reset(now - 0.01);
    }
    selected_ = style;
    selectedClip_ = clip->id;
    schedule_.truncateAfter(now);

    double align = now;
    bool aligned = false;
    double alignmentError = 0.0;
    if (!immediate && style != DanceStyle::Idle && clip->tempoSync) {
        align = entryTimeFor(*clip, now, &aligned, &alignmentError);
    }
    const double fade = immediate ? 0.0 : fadeFor(align);
    const double start = std::max(now, align - fade * 0.5);
    ScheduleSegment segment = makeSegment(*clip, start, align, fade);
    segment.loop = !oneShot;
    segment.alignedToDownbeat = aligned || segment.alignedToDownbeat;
    segment.alignmentErrorSeconds = alignmentError;
    segment.reason = oneShot ? "manual one-shot" : "manual";
    schedule_.append(segment, timeline_, warp_);

    if (oneShot && clip->tempoSync) {
        const ScheduleSegment& placed = schedule_.segments().back();
        const double endBeat =
            clock_.beatAt(placed.alignTime) + (clip->beats - placed.entryClipBeat) / placed.rate;
        const double endTime = clock_.timeAtBeat(endBeat);
        if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
            const double idleFade = fadeFor(endTime);
            ScheduleSegment back = makeSegment(*idle, endTime - idleFade * 0.5, endTime,
                                               idleFade);
            back.reason = "one-shot finished";
            schedule_.append(back, timeline_, warp_);
        }
    }
    schedule_.pruneBefore(now);
}

void DanceDirector::selectClip(const std::string& clipId, double now, bool oneShot,
                               bool immediate) {
    const HumanoidClip* clip = library_ ? library_->find(clipId) : nullptr;
    if (!clip) {
        return;
    }
    if (schedule_.empty()) {
        reset(now - 0.01);
    }
    selected_ = clip->style;
    selectedClip_ = clip->id;
    schedule_.truncateAfter(now);

    double align = now;
    bool aligned = false;
    double alignmentError = 0.0;
    if (!immediate && clip->tempoSync) {
        align = entryTimeFor(*clip, now, &aligned, &alignmentError);
    }
    const double fade = immediate ? 0.0 : fadeFor(align);
    const double start = std::max(now, align - fade * 0.5);
    ScheduleSegment segment = makeSegment(*clip, start, align, fade);
    segment.loop = !oneShot;
    segment.alignedToDownbeat = aligned || segment.alignedToDownbeat;
    segment.alignmentErrorSeconds = alignmentError;
    segment.reason = "manual clip";
    schedule_.append(segment, timeline_, warp_);
    if (oneShot && clip->tempoSync) {
        const ScheduleSegment& placed = schedule_.segments().back();
        const double endBeat =
            clock_.beatAt(placed.alignTime) + (clip->beats - placed.entryClipBeat) / placed.rate;
        const double endTime = clock_.timeAtBeat(endBeat);
        if (const HumanoidClip* idle = clipFor(DanceStyle::Idle)) {
            const double idleFade = fadeFor(endTime);
            ScheduleSegment back = makeSegment(*idle, endTime - idleFade * 0.5, endTime,
                                               idleFade);
            back.reason = "one-shot finished";
            schedule_.append(back, timeline_, warp_);
        }
    }
    schedule_.pruneBefore(now);
}

void DanceDirector::startTestDance(double now, double beatsPerStyle) {
    startSequence({DanceStyle::Groove, DanceStyle::HipHop, DanceStyle::SideStep,
                   DanceStyle::Energetic, DanceStyle::Idle},
                  now, beatsPerStyle, false);
}

void DanceDirector::startSequence(const std::vector<DanceStyle>& styles, double now,
                                  double beatsPerStyle, bool immediate) {
    if (schedule_.empty()) {
        reset(now - 0.01);
    }
    schedule_.truncateAfter(now);
    double beat = std::ceil(clock_.beatAt(now + 0.10));
    bool firstAligned = false;
    if (immediate) {
        beat = clock_.beatAt(now);
    } else {
        for (DanceStyle style : styles) {
            if (const HumanoidClip* firstClip = clipFor(style)) {
                if (style != DanceStyle::Idle) {
                    const double align = entryTimeFor(*firstClip, now, &firstAligned, nullptr);
                    beat = clock_.beatAt(align);
                }
                break;
            }
        }
    }
    const int meter = std::max(1, clock_.beatsPerBar());
    double step = std::max(1.0, beatsPerStyle);
    if (timeline_.downbeatTrusted()) {
        const double bars = std::max(1.0, std::round(step / static_cast<double>(meter)));
        step = bars * static_cast<double>(meter);
    }
    bool first = true;
    for (DanceStyle style : styles) {
        const HumanoidClip* clip = clipFor(style);
        if (!clip) {
            continue;
        }
        const double align = clock_.timeAtBeat(beat);
        const double fade = fadeFor(align);
        const double start = first ? (immediate ? now : std::max(now, align - fade * 0.5))
                                   : align - fade * 0.5;
        ScheduleSegment segment = makeSegment(*clip, start, align, fade);
        if (first && immediate) {
            segment.fadeSeconds = 0.0;
        }
        segment.alignedToDownbeat = first ? (firstAligned || segment.alignedToDownbeat)
                                          : segment.alignedToDownbeat;
        segment.reason = "sequence";
        schedule_.append(segment, timeline_, warp_);
        first = false;
        selected_ = style;
        selectedClip_ = clip->id;
        beat += step;
    }
}

std::string DanceDirector::describeSchedule() const {
    std::string out;
    char line[192];
    for (const ScheduleSegment& seg : schedule_.segments()) {
        if (seg.startTime < -1.0e5) {
            std::snprintf(line, sizeof line, "  start      %-10s %s%s\n",
                          seg.clip->id.c_str(), seg.reason.c_str(),
                          seg.alignedToDownbeat ? "  [downbeat]" : "");
        } else {
            std::snprintf(line, sizeof line, "  %7.2f s  %-10s rate %.1f  %s%s\n",
                          seg.alignTime, seg.clip->id.c_str(), seg.rate,
                          seg.reason.c_str(),
                          seg.alignedToDownbeat ? "  [downbeat]" : "");
        }
        out += line;
    }
    return out;
}

DanceFrame DanceDirector::sample(double mediaTime) const {
    return schedule_.sample(mediaTime, clock_, timeline_, warp_);
}

void DanceDirector::planAuto(const AnalysisResult& analysis, const AutoPlanOptions& options) {
    setMusicalTimeline(MusicalTimeline::fromAnalysis(analysis));
    reset(0.0);
    const HumanoidClip* groove = clipFor(DanceStyle::Groove);
    if (!groove) {
        return;
    }
    const std::size_t beatCount = analysis.beats.size();
    const bool confident = beatCount >= 8 && clock_.confidence() >= options.minConfidence;
    const bool trustedBar = timeline_.downbeatTrusted();
    std::size_t firstDownbeat = 0;
    if (trustedBar) {
        for (std::size_t i = 0; i < beatCount && i < 16; ++i) {
            if (analysis.beats[i].downbeat) {
                firstDownbeat = i;
                break;
            }
        }
    }
    const double trackEnd = analysis.info.duration;
    const double phrase = trustedBar && options.phraseBars > 0
                              ? static_cast<double>(options.phraseBars * timeline_.beatsPerBar())
                              : std::max(4.0, options.phraseBeats);
    const double firstBeat = static_cast<double>(firstDownbeat);
    const double lastBeat = std::max(firstBeat + 1.0, clock_.beatAt(trackEnd));
    const int phraseCount = std::max(1, static_cast<int>(std::ceil((lastBeat - firstBeat) / phrase)));

    // Phrase statistics.
    struct PhraseInfo {
        double t0 = 0, t1 = 0;
        float onsets = 0, bass = 0, section = -1, silent = 0, loud = 0;
    };
    std::vector<PhraseInfo> phrases(static_cast<std::size_t>(phraseCount));
    for (int p = 0; p < phraseCount; ++p) {
        PhraseInfo& info = phrases[static_cast<std::size_t>(p)];
        info.t0 = clock_.timeAtBeat(firstBeat + p * phrase);
        info.t1 = clock_.timeAtBeat(firstBeat + (p + 1) * phrase);
        const double span = std::max(0.1, std::min(info.t1, trackEnd) - info.t0);
        for (const Onset& onset : analysis.onsets) {
            if (onset.time >= info.t0 && onset.time < info.t1) {
                info.onsets += onset.strength;
            }
        }
        info.onsets /= static_cast<float>(span);
        for (const BassAccent& bass : analysis.bassAccents) {
            if (bass.time >= info.t0 && bass.time < info.t1) {
                info.bass += bass.strength * std::max(0.3f, bass.confidence);
            }
        }
        info.bass /= static_cast<float>(span);
        if (analysis.spectrogram.valid() && analysis.spectrogram.framesPerSecond > 0.0) {
            // mean level (dB-scaled 0..255) over the phrase: a loudness proxy that tracks
            // what the ear hears better than a peak envelope
            const Spectrogram& spec = analysis.spectrogram;
            const std::size_t from = static_cast<std::size_t>(
                std::max(0.0, info.t0 * spec.framesPerSecond));
            const std::size_t to = std::min<std::size_t>(
                spec.frameCount,
                static_cast<std::size_t>(std::max(0.0, info.t1 * spec.framesPerSecond)));
            double sum = 0.0;
            std::size_t count = 0;
            for (std::size_t f = from; f < to; ++f) {
                for (int band = 0; band < spec.bands; ++band) {
                    sum += spec.magnitudes[f * static_cast<std::size_t>(spec.bands) +
                                           static_cast<std::size_t>(band)];
                    ++count;
                }
            }
            info.loud = count ? static_cast<float>(sum / static_cast<double>(count) / 255.0)
                              : 0.0f;
        }
        const double mid = 0.5 * (info.t0 + info.t1);
        for (const Section& section : analysis.sections) {
            if (mid >= section.start && mid < section.end) {
                info.section = section.energy;
            }
        }
        for (const SilenceRegion& silence : analysis.silences) {
            const double overlap = std::min(info.t1, silence.end) - std::max(info.t0, silence.start);
            if (overlap > 0.0) {
                info.silent += static_cast<float>(overlap / span);
            }
        }
        // BG Beat reports the gaps between sparse hits as silence too; a phrase only
        // counts as silent when it also has (almost) no onsets.
        if (info.onsets > 0.4f) {
            info.silent = 0.0f;
        }
    }
    const auto percentile = [&](auto member) {
        std::vector<float> values;
        for (const PhraseInfo& info : phrases) values.push_back(info.*member);
        std::sort(values.begin(), values.end());
        return std::max(1e-3f, values[values.size() * 9 / 10]);
    };
    const float onsetRef = percentile(&PhraseInfo::onsets);
    const float bassRef = percentile(&PhraseInfo::bass);
    const float loudRef = percentile(&PhraseInfo::loud);
    float loudMin = loudRef;
    for (const PhraseInfo& info : phrases) {
        loudMin = std::min(loudMin, info.loud);
    }
    const bool haveLoudness = analysis.spectrogram.valid();

    struct Plan {
        DanceStyle style;
        std::string reason;
    };
    std::vector<Plan> plan;
    DanceStyle previous = DanceStyle::Groove;
    int alternate = 0;
    for (const PhraseInfo& info : phrases) {
        const float onset = std::min(1.0f, info.onsets / onsetRef);
        const float bass = std::min(1.0f, info.bass / bassRef);
        const float loud =
            std::clamp((info.loud - loudMin) / std::max(1e-3f, loudRef - loudMin), 0.0f, 1.0f);
        float energy;
        if (info.section >= 0.0f) {
            energy = haveLoudness ? 0.4f * info.section + 0.25f * onset + 0.2f * loud +
                                        0.15f * bass
                                  : 0.5f * info.section + 0.35f * onset + 0.15f * bass;
        } else {
            energy = haveLoudness ? 0.4f * loud + 0.35f * onset + 0.25f * bass
                                  : 0.7f * onset + 0.3f * bass;
        }
        energy = std::clamp(energy, 0.0f, 1.0f);
        DanceStyle style;
        std::string reason = "energy " + std::to_string(static_cast<int>(energy * 100)) + "%";
        if (info.silent > 0.6f) {
            style = DanceStyle::Idle;
            reason = "silence";
        } else if (!confident) {
            style = DanceStyle::Groove;
            reason = "low beat confidence: stable Groove";
        } else {
            // Hysteresis keeps a style unless the energy clearly crosses over.
            const float margin = 0.06f;
            const auto classOf = [&](float e, DanceStyle prev) {
                float calm = options.calmThreshold;
                float intense = options.intenseThreshold;
                if (prev == DanceStyle::Groove) calm += margin;
                if (prev == DanceStyle::Energetic) intense -= margin;
                if (e < calm) return DanceStyle::Groove;
                if (e > intense) return DanceStyle::Energetic;
                return DanceStyle::HipHop;  // placeholder for "active"
            };
            DanceStyle prevClass =
                previous == DanceStyle::SideStep ? DanceStyle::HipHop : previous;
            style = classOf(energy, prevClass);
            if (style == DanceStyle::HipHop) {
                if (previous == DanceStyle::HipHop || previous == DanceStyle::SideStep) {
                    style = previous;  // stay in the active family for the phrase
                    if (++alternate % 2 == 0) {
                        style = previous == DanceStyle::HipHop ? DanceStyle::SideStep
                                                               : DanceStyle::HipHop;
                    }
                }
                reason += " -> active";
            } else if (style == DanceStyle::Energetic) {
                reason += " -> intense";
            } else {
                reason += " -> calm";
            }
        }
        if (!trustedBar) {
            reason += " (meter uncertain)";
        }
        if (!clipFor(style)) {
            style = DanceStyle::Groove;
        }
        plan.push_back({style, reason});
        previous = style;
    }

    // Emit one segment per style change; equal consecutive phrases continue. Each new segment
    // of a style alternates between the authored clip and its mirror image, so a style that
    // comes back does not repeat the same bit-identical swing. The mirror is a library-owned
    // clip, so the segment's pointer stays valid.
    std::array<bool, 6> mirrorNext{};  // per DanceStyle: the next segment of it is mirrored
    std::size_t index = 0;
    bool anySegment = false;
    for (int p = 0; p < phraseCount; ++p, ++index) {
        const Plan& entry = plan[index];
        if (anySegment && entry.style == plan[index - 1].style) {
            continue;
        }
        const HumanoidClip* clip = clipFor(entry.style);
        if (!clip) {
            continue;
        }
        const std::size_t styleIndex = static_cast<std::size_t>(entry.style);
        const bool mirrored = entry.style != DanceStyle::Idle && mirrorNext[styleIndex];
        if (entry.style != DanceStyle::Idle) {
            mirrorNext[styleIndex] = !mirrorNext[styleIndex];
        }
        if (mirrored && library_) {
            if (const HumanoidClip* mirror = library_->mirrorOf(*clip)) {
                clip = mirror;
            }
        }
        const double align = phrases[index].t0;
        const double fade = fadeFor(align);
        const double start = std::max(0.0, align - fade * 0.5);
        ScheduleSegment segment = makeSegment(*clip, start, align, fade);
        segment.reason = mirrored ? entry.reason + " -> mirrored repeat" : entry.reason;
        schedule_.append(segment, timeline_, warp_);
        anySegment = true;
    }
    selected_ = DanceStyle::Groove;
}

} // namespace bgmedia::motion
