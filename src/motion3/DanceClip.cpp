#include "bgmedia/motion3/DanceClip.hpp"

#include "Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace bgmedia::motion {
namespace {

using json::Value;

constexpr float kTwoPi = 6.28318530717958647692f;

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

template <typename T>
T catmull(const T& p0, const T& p1, const T& p2, const T& p3, float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) *
           0.5f;
}

float catmullF(float p0, float p1, float p2, float p3, float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return 0.5f * (2.0f * p1 + (p2 - p0) * u +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * u3);
}

Quat alignTo(Quat q, const Quat& reference) {
    if (math::dot(q, reference) < 0.0f) {
        return {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

Quat catmullQuat(Quat p0, const Quat& p1, Quat p2, Quat p3, float u) {
    p0 = alignTo(p0, p1);
    p2 = alignTo(p2, p1);
    p3 = alignTo(p3, p2);
    const float u2 = u * u;
    const float u3 = u2 * u;
    const auto channel = [&](float a, float b, float c, float d) {
        return 0.5f * (2.0f * b + (c - a) * u + (2.0f * a - 5.0f * b + 4.0f * c - d) * u2 +
                       (3.0f * b - a - 3.0f * c + d) * u3);
    };
    return Quat{channel(p0.x, p1.x, p2.x, p3.x), channel(p0.y, p1.y, p2.y, p3.y),
                channel(p0.z, p1.z, p2.z, p3.z), channel(p0.w, p1.w, p2.w, p3.w)}
        .normalized();
}

struct KeyWindow {
    int i0 = 0;
    int im1 = 0;
    int i2 = 0;
    int i3 = 0;
    float u = 0.0f;
};

KeyWindow makeWindow(const HumanoidClip& clip, double clipBeat) {
    KeyWindow w;
    const int n = clip.keyCount;
    double beat = clipBeat;
    if (clip.loop) {
        beat = std::fmod(beat, static_cast<double>(clip.beats));
        if (beat < 0.0) {
            beat += clip.beats;
        }
    } else {
        beat = std::clamp(beat, 0.0, static_cast<double>(clip.beats));
    }
    const double f = beat * clip.keysPerBeat;
    int i0 = static_cast<int>(std::floor(f));
    float u = static_cast<float>(f - i0);
    if (i0 >= n - 1) {
        i0 = n - 2;
        u = 1.0f;
    }
    w.i0 = i0;
    w.u = u;
    if (clip.loop) {
        const int period = n - 1;
        w.im1 = (i0 - 1 + period) % period;
        w.i2 = (i0 + 1) % period;
        w.i3 = (i0 + 2) % period;
    } else {
        w.im1 = std::max(0, i0 - 1);
        w.i2 = std::min(n - 1, i0 + 1);
        w.i3 = std::min(n - 1, i0 + 2);
    }
    return w;
}

FootPhase footPhaseAt(const HumanoidClip& clip, int foot, double beat, float& contact) {
    constexpr float kRamp = 0.20f;
    double b = beat;
    if (clip.loop) {
        b = std::fmod(b, static_cast<double>(clip.beats));
        if (b < 0.0) {
            b += clip.beats;
        }
    }
    for (const ContactSegment& seg : clip.contacts) {
        if (seg.foot != foot || b < seg.start || b > seg.end) {
            continue;
        }
        const float sinceStart = static_cast<float>(b - seg.start);
        const float untilEnd = static_cast<float>(seg.end - b);
        contact = smoothstep(std::min(sinceStart, untilEnd) / kRamp);
        // Segments that touch the clip boundary continue across the loop seam:
        // they have no plant / release moment there.
        const bool touchesStart = seg.start <= 1e-4f && clip.loop;
        const bool touchesEnd = seg.end >= clip.beats - 1e-4f && clip.loop;
        if (touchesStart) contact = std::min(1.0f, smoothstep(untilEnd / kRamp));
        if (touchesEnd) contact = std::min(1.0f, smoothstep(sinceStart / kRamp));
        if (touchesStart && touchesEnd) contact = 1.0f;
        if (sinceStart < kRamp && !touchesStart) return FootPhase::Plant;
        if (untilEnd < kRamp && !touchesEnd) return FootPhase::Release;
        return seg.pivot ? FootPhase::Pivot : FootPhase::Stance;
    }
    contact = 0.0f;
    return FootPhase::Swing;
}

float accentEnvelope(const HumanoidClip& clip, double beat) {
    double b = beat;
    if (clip.loop) {
        b = std::fmod(b, static_cast<double>(clip.beats));
        if (b < 0.0) {
            b += clip.beats;
        }
    }
    float best = 0.0f;
    for (const AccentMark& mark : clip.accents) {
        double d = std::abs(b - mark.beat);
        if (clip.loop) {
            d = std::min(d, static_cast<double>(clip.beats) - d);
        }
        const float v = mark.strength *
                        std::clamp(mark.confidence, 0.0f, 1.0f) *
                        std::max(0.0f, 1.0f - static_cast<float>(d) / 0.35f);
        best = std::max(best, v);
    }
    return best;
}

// --- JSON helpers -----------------------------------------------------------

struct Reader {
    const std::string& source;
    std::vector<ClipDiagnostic>& out;

    void error(const std::string& message) {
        out.push_back({ClipDiagnostic::Severity::Error, source, message});
    }
    void warn(const std::string& message) {
        out.push_back({ClipDiagnostic::Severity::Warning, source, message});
    }

    bool number(const Value& obj, const char* key, double& dst, bool required = false) {
        const Value* v = obj.find(key);
        if (!v) {
            if (required) error(std::string("missing required field '") + key + "'");
            return !required;
        }
        if (!v->isNumber() || !std::isfinite(v->number)) {
            error(std::string("field '") + key + "' must be a finite number");
            return false;
        }
        dst = v->number;
        return true;
    }
    bool text(const Value& obj, const char* key, std::string& dst, bool required = false) {
        const Value* v = obj.find(key);
        if (!v) {
            if (required) error(std::string("missing required field '") + key + "'");
            return !required;
        }
        if (!v->isString()) {
            error(std::string("field '") + key + "' must be a string");
            return false;
        }
        dst = v->string;
        return true;
    }
    bool flag(const Value& obj, const char* key, bool& dst) {
        const Value* v = obj.find(key);
        if (!v) return true;
        if (v->type != Value::Type::Bool) {
            error(std::string("field '") + key + "' must be true or false");
            return false;
        }
        dst = v->boolean;
        return true;
    }
    bool floats(const Value& arr, const std::string& what, std::vector<float>& dst) {
        if (!arr.isArray()) {
            error(what + " must be an array of numbers");
            return false;
        }
        dst.clear();
        dst.reserve(arr.array.size());
        for (const Value& element : arr.array) {
            if (!element.isNumber() || !std::isfinite(element.number)) {
                error(what + " contains a non-numeric or non-finite value");
                return false;
            }
            dst.push_back(static_cast<float>(element.number));
        }
        return true;
    }
};

bool vec3Track(Reader& reader, const Value& arr, const std::string& what, int keyCount,
               int stride, std::vector<Vec3>& dst, std::vector<float>* extra) {
    std::vector<float> flat;
    if (!reader.floats(arr, what, flat)) {
        return false;
    }
    if (flat.size() != static_cast<std::size_t>(keyCount) * static_cast<std::size_t>(stride)) {
        reader.error(what + " has " + std::to_string(flat.size()) + " values, expected " +
                     std::to_string(keyCount) + " keys x " + std::to_string(stride));
        return false;
    }
    dst.resize(static_cast<std::size_t>(keyCount));
    if (extra) {
        extra->resize(static_cast<std::size_t>(keyCount));
    }
    for (int i = 0; i < keyCount; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * static_cast<std::size_t>(stride);
        dst[static_cast<std::size_t>(i)] = {flat[base], flat[base + 1], flat[base + 2]};
        if (extra && stride > 3) {
            (*extra)[static_cast<std::size_t>(i)] = flat[base + 3];
        }
    }
    return true;
}

bool footTrack(Reader& reader, const Value& arr, const std::string& what, int keyCount,
               FootTrack& dst) {
    std::vector<float> flat;
    if (!reader.floats(arr, what, flat)) {
        return false;
    }
    const std::size_t keys = static_cast<std::size_t>(keyCount);
    if (flat.size() != keys * 4 && flat.size() != keys * 5) {
        reader.error(what + " has " + std::to_string(flat.size()) +
                     " values, expected " + std::to_string(keyCount) +
                     " keys x 4 or 5 (x, y, z, yaw[, toe])");
        return false;
    }
    const std::size_t stride = flat.size() / keys;
    dst.position.resize(keys);
    dst.yaw.resize(keys);
    dst.toe.clear();
    if (stride == 5) {
        dst.toe.resize(keys);
    }
    for (std::size_t i = 0; i < keys; ++i) {
        const std::size_t base = i * stride;
        dst.position[i] = {flat[base], flat[base + 1], flat[base + 2]};
        dst.yaw[i] = flat[base + 3];
        if (stride == 5) {
            dst.toe[i] = flat[base + 4];
        }
    }
    return true;
}

} // namespace

const char* styleName(DanceStyle style) {
    switch (style) {
    case DanceStyle::Idle: return "idle";
    case DanceStyle::Groove: return "groove";
    case DanceStyle::HipHop: return "hiphop";
    case DanceStyle::SideStep: return "sidestep";
    case DanceStyle::Energetic: return "energetic";
    case DanceStyle::Custom: return "custom";
    }
    return "custom";
}

DanceStyle styleFromName(const std::string& name) {
    std::string lower;
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "idle") return DanceStyle::Idle;
    if (lower == "groove") return DanceStyle::Groove;
    if (lower == "hiphop" || lower == "hip-hop" || lower == "hip_hop") return DanceStyle::HipHop;
    if (lower == "sidestep" || lower == "side-step" || lower == "footwork") return DanceStyle::SideStep;
    if (lower == "energetic" || lower == "drop") return DanceStyle::Energetic;
    return DanceStyle::Custom;
}

const char* footPhaseName(FootPhase phase) {
    switch (phase) {
    case FootPhase::Swing: return "Swing";
    case FootPhase::Plant: return "Plant";
    case FootPhase::Stance: return "Stance";
    case FootPhase::Pivot: return "Pivot";
    case FootPhase::Release: return "Release";
    }
    return "?";
}

ClipPose neutralClipPose() {
    ClipPose pose;
    pose.bone.fill(Quat::identity());
    pose.handPose[0] = relaxedHandPose(pose.hand[0]);
    pose.handPose[1] = relaxedHandPose(pose.hand[1]);
    return pose;
}

HandPose relaxedHandPose(float curl) {
    HandPose pose;
    curl = std::clamp(curl, 0.0f, 1.0f);
    // The resting hand is not a flat blade: the middle and ring fingers relax
    // slightly more than the index, and the little finger trails a little.
    static constexpr float kRelax[5] = {0.65f, 0.90f, 1.0f, 0.95f, 0.80f};
    for (int finger = 0; finger < 5; ++finger) {
        for (int joint = 0; joint < 3; ++joint) {
            pose.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] =
                std::clamp(curl * kRelax[finger], 0.0f, 1.0f);
        }
    }
    return pose;
}

namespace {
float clamp01(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}
float clampSigned(float value) {
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}
}  // namespace

void clampHandPose(HandPose& pose) {
    for (auto& finger : pose.curl) {
        for (float& value : finger) {
            value = clamp01(value);
        }
    }
    for (float& value : pose.spread) {
        value = clampSigned(value);
    }
    pose.thumbOpposition = clamp01(pose.thumbOpposition);
    for (float& value : pose.wrist) {
        value = clampSigned(value);
    }
}

HandPose blendHandPose(const HandPose& a, const HandPose& b, float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    HandPose out;
    for (int finger = 0; finger < 5; ++finger) {
        for (int joint = 0; joint < 3; ++joint) {
            out.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] =
                a.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] *
                    (1.0f - w) +
                b.curl[static_cast<std::size_t>(finger)][static_cast<std::size_t>(joint)] * w;
        }
        out.spread[static_cast<std::size_t>(finger)] =
            a.spread[static_cast<std::size_t>(finger)] * (1.0f - w) +
            b.spread[static_cast<std::size_t>(finger)] * w;
    }
    out.thumbOpposition = a.thumbOpposition * (1.0f - w) + b.thumbOpposition * w;
    for (int axis = 0; axis < 3; ++axis) {
        out.wrist[static_cast<std::size_t>(axis)] =
            a.wrist[static_cast<std::size_t>(axis)] * (1.0f - w) +
            b.wrist[static_cast<std::size_t>(axis)] * w;
    }
    clampHandPose(out);
    return out;
}

void sampleClip(const HumanoidClip& clip, double clipBeat, ClipPose& out) {
    out = neutralClipPose();
    if (clip.keyCount < 2) {
        return;
    }
    const KeyWindow w = makeWindow(clip, clipBeat);
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto& track = clip.bones[b];
        if (track.size() != static_cast<std::size_t>(clip.keyCount)) {
            continue;
        }
        out.bone[b] = catmullQuat(track[static_cast<std::size_t>(w.im1)],
                                  track[static_cast<std::size_t>(w.i0)],
                                  track[static_cast<std::size_t>(w.i2)],
                                  track[static_cast<std::size_t>(w.i3)], w.u);
    }
    const auto vec = [&](const std::vector<Vec3>& track) {
        return catmull(track[static_cast<std::size_t>(w.im1)],
                       track[static_cast<std::size_t>(w.i0)],
                       track[static_cast<std::size_t>(w.i2)],
                       track[static_cast<std::size_t>(w.i3)], w.u);
    };
    const auto scalar = [&](const std::vector<float>& track) {
        return catmullF(track[static_cast<std::size_t>(w.im1)],
                        track[static_cast<std::size_t>(w.i0)],
                        track[static_cast<std::size_t>(w.i2)],
                        track[static_cast<std::size_t>(w.i3)], w.u);
    };
    if (clip.hips.size() == static_cast<std::size_t>(clip.keyCount)) {
        out.hips = vec(clip.hips);
    }
    if (clip.root.size() == static_cast<std::size_t>(clip.keyCount)) {
        const Vec3 r = vec(clip.root);
        out.rootX = r.x;
        out.rootZ = r.y;
        out.rootYaw = r.z;
    }
    for (int f = 0; f < 2; ++f) {
        const FootTrack& track = clip.feet[static_cast<std::size_t>(f)];
        ClipPose::Foot& foot = out.foot[static_cast<std::size_t>(f)];
        if (track.position.size() == static_cast<std::size_t>(clip.keyCount)) {
            foot.position = vec(track.position);
        }
        if (track.yaw.size() == static_cast<std::size_t>(clip.keyCount)) {
            foot.yaw = scalar(track.yaw);
        }
        if (track.toe.size() == static_cast<std::size_t>(clip.keyCount)) {
            foot.toePitch = std::clamp(scalar(track.toe), -1.0f, 1.0f);
        }
        foot.phase = footPhaseAt(clip, f, clipBeat, foot.contact);
    }
    for (int h = 0; h < 2; ++h) {
        const auto& track = clip.hands[static_cast<std::size_t>(h)];
        if (track.size() == static_cast<std::size_t>(clip.keyCount)) {
            out.hand[static_cast<std::size_t>(h)] = std::clamp(scalar(track), 0.0f, 1.0f);
        }
        const auto& detailed = clip.handPoses[static_cast<std::size_t>(h)];
        if (detailed.size() == static_cast<std::size_t>(clip.keyCount)) {
            const HandPose& h0 = detailed[static_cast<std::size_t>(w.i0)];
            const HandPose& hm1 = detailed[static_cast<std::size_t>(w.im1)];
            const HandPose& h2 = detailed[static_cast<std::size_t>(w.i2)];
            const HandPose& h3 = detailed[static_cast<std::size_t>(w.i3)];
            HandPose sampled;
            for (int finger = 0; finger < 5; ++finger) {
                for (int joint = 0; joint < 3; ++joint) {
                    sampled.curl[static_cast<std::size_t>(finger)]
                                [static_cast<std::size_t>(joint)] =
                        catmullF(hm1.curl[static_cast<std::size_t>(finger)]
                                      [static_cast<std::size_t>(joint)],
                                 h0.curl[static_cast<std::size_t>(finger)]
                                      [static_cast<std::size_t>(joint)],
                                 h2.curl[static_cast<std::size_t>(finger)]
                                      [static_cast<std::size_t>(joint)],
                                 h3.curl[static_cast<std::size_t>(finger)]
                                      [static_cast<std::size_t>(joint)],
                                 w.u);
                }
                sampled.spread[static_cast<std::size_t>(finger)] =
                    catmullF(hm1.spread[static_cast<std::size_t>(finger)],
                             h0.spread[static_cast<std::size_t>(finger)],
                             h2.spread[static_cast<std::size_t>(finger)],
                             h3.spread[static_cast<std::size_t>(finger)], w.u);
            }
            sampled.thumbOpposition =
                catmullF(hm1.thumbOpposition, h0.thumbOpposition,
                         h2.thumbOpposition, h3.thumbOpposition, w.u);
            for (int axis = 0; axis < 3; ++axis) {
                sampled.wrist[static_cast<std::size_t>(axis)] =
                    catmullF(hm1.wrist[static_cast<std::size_t>(axis)],
                             h0.wrist[static_cast<std::size_t>(axis)],
                             h2.wrist[static_cast<std::size_t>(axis)],
                             h3.wrist[static_cast<std::size_t>(axis)], w.u);
            }
            clampHandPose(sampled);
            out.handPose[static_cast<std::size_t>(h)] = sampled;
            out.detailedHands = true;
        } else {
            out.handPose[static_cast<std::size_t>(h)] =
                relaxedHandPose(out.hand[static_cast<std::size_t>(h)]);
        }
    }
    out.energy = clip.energy;
    out.accent = accentEnvelope(clip, clipBeat);
}

ClipPose sampleClip(const HumanoidClip& clip, double clipBeat) {
    ClipPose pose;
    sampleClip(clip, clipBeat, pose);
    return pose;
}

float footContactAt(const HumanoidClip& clip, int foot, double clipBeat) {
    float contact = 0.0f;
    footPhaseAt(clip, foot, clipBeat, contact);
    return contact;
}

float clipAccentStrength(const HumanoidClip& clip, double clipBeat) {
    return accentEnvelope(clip, clipBeat);
}

ClipPose blendPoses(const ClipPose& a, const ClipPose& b, float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    if (w <= 0.0f) return a;
    if (w >= 1.0f) return b;
    ClipPose out;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        out.bone[i] = math::slerp(a.bone[i], b.bone[i], w);
    }
    out.hips = math::lerp(a.hips, b.hips, w);
    out.rootX = a.rootX + (b.rootX - a.rootX) * w;
    out.rootZ = a.rootZ + (b.rootZ - a.rootZ) * w;
    const auto arc = [&](float from, float to) {
        float d = to - from;
        d -= std::round(d);
        return from + d * w;
    };
    out.rootYaw = arc(a.rootYaw, b.rootYaw);
    for (std::size_t f = 0; f < 2; ++f) {
        out.foot[f].position = math::lerp(a.foot[f].position, b.foot[f].position, w);
        out.foot[f].yaw = arc(a.foot[f].yaw, b.foot[f].yaw);
        out.foot[f].toePitch = a.foot[f].toePitch + (b.foot[f].toePitch - a.foot[f].toePitch) * w;
        out.foot[f].contact = a.foot[f].contact + (b.foot[f].contact - a.foot[f].contact) * w;
        out.foot[f].phase = w < 0.5f ? a.foot[f].phase : b.foot[f].phase;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        out.hand[h] = a.hand[h] + (b.hand[h] - a.hand[h]) * w;
        out.handPose[h] = blendHandPose(a.handPose[h], b.handPose[h], w);
    }
    out.detailedHands = a.detailedHands || b.detailedHands;
    out.energy = a.energy + (b.energy - a.energy) * w;
    out.accent = std::max(a.accent * (1.0f - w), b.accent * w);
    return out;
}

// --- parsing ---------------------------------------------------------------

ClipLoadResult parseClipJson(const std::string& text, const std::string& sourceName) {
    ClipLoadResult result;
    Reader reader{sourceName, result.diagnostics};
    Value root;
    std::string parseError;
    json::Parser parser(text);
    if (!parser.parse(root, parseError)) {
        reader.error(parseError);
        return result;
    }
    if (!root.isObject()) {
        reader.error("top level must be a JSON object");
        return result;
    }
    HumanoidClip& clip = result.clip;
    std::string format;
    reader.text(root, "format", format, true);
    if (!format.empty() && format != kClipFormatName) {
        reader.error("format is '" + format + "', expected '" + kClipFormatName + "'");
    }
    double version = 0;
    reader.number(root, "version", version, true);
    clip.formatVersion = static_cast<int>(version);
    if ((clip.formatVersion != 1 && clip.formatVersion != kClipFormatVersion) && version != 0) {
        reader.error("unsupported clip format version " +
                     std::to_string(clip.formatVersion) + " (this build reads version " +
                     std::to_string(kClipFormatVersion) + ")");
    }
    reader.text(root, "id", clip.id, true);
    reader.text(root, "name", clip.name);
    reader.text(root, "description", clip.description);
    reader.text(root, "author", clip.author);
    reader.text(root, "origin", clip.origin);
    std::string style;
    reader.text(root, "style", style);
    clip.style = styleFromName(style);
    if (clip.name.empty()) clip.name = clip.id;

    double beats = 0;
    double keys = 16;
    double bpmRange[2] = {0, 0};
    double beatsPerBar = 4;
    double energy = 0.5;
    reader.number(root, "bpm", clip.bpm, true);
    reader.number(root, "beats", beats, true);
    reader.number(root, "keysPerBeat", keys);
    reader.number(root, "beatsPerBar", beatsPerBar);
    reader.number(root, "energy", energy);
    reader.flag(root, "loop", clip.loop);
    reader.flag(root, "seamless", clip.seamless);
    reader.flag(root, "tempoSync", clip.tempoSync);
    clip.beats = static_cast<float>(beats);
    clip.keysPerBeat = static_cast<int>(keys);
    clip.beatsPerBar = static_cast<int>(beatsPerBar);
    clip.energy = static_cast<float>(energy);
    clip.bpmMin = clip.bpm * 0.75;
    clip.bpmMax = clip.bpm * 1.3;
    if (const Value* range = root.find("bpmRange")) {
        if (range->isArray() && range->array.size() == 2 && range->array[0].isNumber() &&
            range->array[1].isNumber()) {
            bpmRange[0] = range->array[0].number;
            bpmRange[1] = range->array[1].number;
            clip.bpmMin = bpmRange[0];
            clip.bpmMax = bpmRange[1];
        } else {
            reader.error("'bpmRange' must be [min, max]");
        }
    }
    const auto beatList = [&](const char* key, std::vector<float>& dst) {
        if (const Value* v = root.find(key)) {
            std::vector<float> tmp;
            if (reader.floats(*v, key, tmp)) dst = tmp;
        }
    };
    beatList("entryBeats", clip.entryBeats);
    beatList("exitBeats", clip.exitBeats);

    if (const Value* accents = root.find("accents")) {
        if (!accents->isArray()) {
            reader.error("'accents' must be an array");
        } else {
            for (const Value& item : accents->array) {
                AccentMark mark;
                double beat = 0;
                double strength = 1;
                double confidence = 1;
                if (item.isObject() && reader.number(item, "beat", beat, true)) {
                    reader.number(item, "strength", strength);
                    reader.number(item, "confidence", confidence);
                    reader.text(item, "kind", mark.kind);
                    mark.beat = static_cast<float>(beat);
                    mark.strength = static_cast<float>(strength);
                    mark.confidence = static_cast<float>(confidence);
                    clip.accents.push_back(mark);
                } else if (!item.isObject()) {
                    reader.error("each accent must be an object {beat, strength, kind}");
                }
            }
        }
    }
    if (const Value* contacts = root.find("contacts")) {
        if (!contacts->isArray()) {
            reader.error("'contacts' must be an array");
        } else {
            for (const Value& item : contacts->array) {
                if (!item.isObject()) {
                    reader.error("each contact must be an object");
                    continue;
                }
                ContactSegment seg;
                std::string foot;
                double start = 0, end = 0;
                reader.text(item, "foot", foot, true);
                reader.number(item, "start", start, true);
                reader.number(item, "end", end, true);
                std::string kind = "stance";
                reader.text(item, "kind", kind);
                if (foot == "left" || foot == "L") seg.foot = 0;
                else if (foot == "right" || foot == "R") seg.foot = 1;
                else reader.error("contact foot must be 'left' or 'right'");
                seg.start = static_cast<float>(start);
                seg.end = static_cast<float>(end);
                seg.pivot = kind == "pivot";
                clip.contacts.push_back(seg);
            }
        }
    }

    if (beats <= 0 || keys < 1 || keys > 240 || beats > 4096) {
        reader.error("'beats' and 'keysPerBeat' are out of range");
    } else {
        clip.keyCount = static_cast<int>(std::lround(beats * keys)) + 1;
        if (std::abs(beats * keys - std::round(beats * keys)) > 1e-6) {
            reader.error("beats * keysPerBeat must be a whole number");
        }
    }

    const Value* tracks = root.find("tracks");
    if (!tracks || !tracks->isObject()) {
        reader.error("missing 'tracks' object");
    } else if (clip.keyCount > 1) {
        if (const Value* bones = tracks->find("bones")) {
            if (!bones->isObject()) {
                reader.error("'tracks.bones' must be an object keyed by humanoid bone name");
            } else {
                for (const auto& entry : bones->object) {
                    const auto bone = vrm::humanoidBoneFromVrmName(entry.first);
                    if (!bone) {
                        reader.error("unknown humanoid bone '" + entry.first + "'");
                        continue;
                    }
                    std::vector<float> flat;
                    if (!reader.floats(entry.second, "bone '" + entry.first + "'", flat)) {
                        continue;
                    }
                    if (flat.size() != static_cast<std::size_t>(clip.keyCount) * 4) {
                        reader.error("bone '" + entry.first + "' has " +
                                     std::to_string(flat.size()) + " values, expected " +
                                     std::to_string(clip.keyCount) + " keys x 4");
                        continue;
                    }
                    auto& dst = clip.bones[static_cast<std::size_t>(*bone)];
                    dst.resize(static_cast<std::size_t>(clip.keyCount));
                    for (int i = 0; i < clip.keyCount; ++i) {
                        const std::size_t base = static_cast<std::size_t>(i) * 4;
                        Quat q{flat[base], flat[base + 1], flat[base + 2], flat[base + 3]};
                        const float norm = std::sqrt(q.lengthSquared());
                        if (norm < 0.9f || norm > 1.1f) {
                            reader.error("bone '" + entry.first + "' key " + std::to_string(i) +
                                         " is not a unit quaternion (|q|=" +
                                         std::to_string(norm) + ")");
                            break;
                        }
                        dst[static_cast<std::size_t>(i)] = q.normalized();
                    }
                }
            }
        }
        if (const Value* v = tracks->find("hips")) {
            vec3Track(reader, *v, "tracks.hips", clip.keyCount, 3, clip.hips, nullptr);
        }
        if (const Value* v = tracks->find("root")) {
            vec3Track(reader, *v, "tracks.root", clip.keyCount, 3, clip.root, nullptr);
        }
        if (const Value* v = tracks->find("footLeft")) {
            footTrack(reader, *v, "tracks.footLeft", clip.keyCount, clip.feet[0]);
        }
        for (int h = 0; h < 2; ++h) {
            if (const Value* v = tracks->find(h == 0 ? "handLeft" : "handRight")) {
                std::vector<float> flat;
                if (reader.floats(*v, h == 0 ? "tracks.handLeft" : "tracks.handRight", flat)) {
                    if (flat.size() == static_cast<std::size_t>(clip.keyCount)) {
                        clip.hands[static_cast<std::size_t>(h)] = std::move(flat);
                    } else {
                        reader.error(std::string("tracks.") + (h == 0 ? "handLeft" : "handRight") +
                                     " has " + std::to_string(flat.size()) + " values, expected " +
                                     std::to_string(clip.keyCount));
                    }
                }
            }
            const char* detailedKey = h == 0 ? "handPoseLeft" : "handPoseRight";
            if (const Value* v = tracks->find(detailedKey)) {
                std::vector<float> flat;
                constexpr int kHandStride = 15 + 5 + 1 + 3;  // curls, spreads, opposition, wrist
                if (reader.floats(*v, std::string("tracks.") + detailedKey, flat)) {
                    if (flat.size() != static_cast<std::size_t>(clip.keyCount) * kHandStride) {
                        reader.error(std::string("tracks.") + detailedKey + " has " +
                                     std::to_string(flat.size()) + " values, expected " +
                                     std::to_string(clip.keyCount) + " keys x " +
                                     std::to_string(kHandStride));
                    } else {
                        auto& track = clip.handPoses[static_cast<std::size_t>(h)];
                        track.resize(static_cast<std::size_t>(clip.keyCount));
                        for (int k = 0; k < clip.keyCount; ++k) {
                            const float* row = flat.data() + static_cast<std::size_t>(k) * kHandStride;
                            HandPose pose;
                            int at = 0;
                            for (int finger = 0; finger < 5; ++finger) {
                                for (int joint = 0; joint < 3; ++joint) {
                                    pose.curl[static_cast<std::size_t>(finger)]
                                              [static_cast<std::size_t>(joint)] = row[at++];
                                }
                            }
                            for (int finger = 0; finger < 5; ++finger) {
                                pose.spread[static_cast<std::size_t>(finger)] = row[at++];
                            }
                            pose.thumbOpposition = row[at++];
                            for (int axis = 0; axis < 3; ++axis) {
                                pose.wrist[static_cast<std::size_t>(axis)] = row[at++];
                            }
                            clampHandPose(pose);
                            track[static_cast<std::size_t>(k)] = pose;
                        }
                    }
                }
            }
        }
        if (const Value* v = tracks->find("footRight")) {
            footTrack(reader, *v, "tracks.footRight", clip.keyCount, clip.feet[1]);
        }
    }

    bool hadError = false;
    for (const ClipDiagnostic& d : result.diagnostics) {
        hadError = hadError || d.severity == ClipDiagnostic::Severity::Error;
    }
    if (!hadError) {
        validateClip(clip, sourceName, result.diagnostics);
    }
    result.ok = true;
    for (const ClipDiagnostic& d : result.diagnostics) {
        if (d.severity == ClipDiagnostic::Severity::Error) {
            result.ok = false;
        }
    }
    return result;
}

ClipLoadResult loadClipFile(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ClipLoadResult result;
        result.diagnostics.push_back(
            {ClipDiagnostic::Severity::Error, name, "cannot open file"});
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parseClipJson(buffer.str(), name);
}

namespace {

void writeNumber(std::ostream& out, double value) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    out << value;
}

void writeFloatArray(std::ostream& out, const float* values, std::size_t count) {
    out << '[';
    for (std::size_t i = 0; i < count; ++i) {
        if (i) out << ',';
        writeNumber(out, values[i]);
    }
    out << ']';
}

void writeQuatArray(std::ostream& out, const std::vector<Quat>& track) {
    out << '[';
    for (std::size_t i = 0; i < track.size(); ++i) {
        if (i) out << ',';
        const Quat& q = track[i];
        writeNumber(out, q.x);
        out << ',';
        writeNumber(out, q.y);
        out << ',';
        writeNumber(out, q.z);
        out << ',';
        writeNumber(out, q.w);
    }
    out << ']';
}

void writeVec3Array(std::ostream& out, const std::vector<Vec3>& track) {
    out << '[';
    for (std::size_t i = 0; i < track.size(); ++i) {
        if (i) out << ',';
        writeNumber(out, track[i].x);
        out << ',';
        writeNumber(out, track[i].y);
        out << ',';
        writeNumber(out, track[i].z);
    }
    out << ']';
}

void writeFootTrack(std::ostream& out, const FootTrack& track, int keyCount) {
    const bool hasToe = track.toe.size() == static_cast<std::size_t>(keyCount);
    const bool hasPos = track.position.size() == static_cast<std::size_t>(keyCount);
    const bool hasYaw = track.yaw.size() == static_cast<std::size_t>(keyCount);
    out << '[';
    for (int i = 0; i < keyCount; ++i) {
        if (i) out << ',';
        const Vec3 p = hasPos ? track.position[static_cast<std::size_t>(i)] : Vec3{};
        writeNumber(out, p.x);
        out << ',';
        writeNumber(out, p.y);
        out << ',';
        writeNumber(out, p.z);
        out << ',';
        writeNumber(out, hasYaw ? track.yaw[static_cast<std::size_t>(i)] : 0.0f);
        if (hasToe) {
            out << ',';
            writeNumber(out, track.toe[static_cast<std::size_t>(i)]);
        }
    }
    out << ']';
}

void writeHandPoseTrack(std::ostream& out, const std::vector<HandPose>& track) {
    out << '[';
    bool first = true;
    for (const HandPose& pose : track) {
        float row[24];
        int at = 0;
        for (int finger = 0; finger < 5; ++finger) {
            for (int joint = 0; joint < 3; ++joint) {
                row[at++] = pose.curl[static_cast<std::size_t>(finger)]
                                      [static_cast<std::size_t>(joint)];
            }
        }
        for (int finger = 0; finger < 5; ++finger) {
            row[at++] = pose.spread[static_cast<std::size_t>(finger)];
        }
        row[at++] = pose.thumbOpposition;
        for (int axis = 0; axis < 3; ++axis) {
            row[at++] = pose.wrist[static_cast<std::size_t>(axis)];
        }
        for (float value : row) {
            if (!first) out << ',';
            first = false;
            writeNumber(out, value);
        }
    }
    out << ']';
}

std::string escapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += ' ';
            } else {
                out += c;
            }
        }
    }
    return out;
}

}  // namespace

std::string clipToJson(const HumanoidClip& clip) {
    std::ostringstream out;
    out.precision(6);
    out << "{\"format\":\"" << kClipFormatName << "\","
        << "\"version\":" << kClipFormatVersion << ','
        << "\"id\":\"" << escapeJson(clip.id) << "\","
        << "\"name\":\"" << escapeJson(clip.name) << "\","
        << "\"style\":\"" << styleName(clip.style) << "\","
        << "\"author\":\"" << escapeJson(clip.author) << "\","
        << "\"origin\":\"" << escapeJson(clip.origin) << "\","
        << "\"description\":\"" << escapeJson(clip.description) << "\","
        << "\"bpm\":" << clip.bpm << ','
        << "\"bpmRange\":[" << clip.bpmMin << ',' << clip.bpmMax << "],"
        << "\"beats\":" << clip.beats << ','
        << "\"beatsPerBar\":" << clip.beatsPerBar << ','
        << "\"keysPerBeat\":" << clip.keysPerBeat << ','
        << "\"loop\":" << (clip.loop ? "true" : "false") << ','
        << "\"seamless\":" << (clip.seamless ? "true" : "false") << ','
        << "\"tempoSync\":" << (clip.tempoSync ? "true" : "false") << ','
        << "\"energy\":" << clip.energy;
    out << ",\"entryBeats\":";
    writeFloatArray(out, clip.entryBeats.data(), clip.entryBeats.size());
    out << ",\"exitBeats\":";
    writeFloatArray(out, clip.exitBeats.data(), clip.exitBeats.size());
    out << ",\"accents\":[";
    for (std::size_t i = 0; i < clip.accents.size(); ++i) {
        if (i) out << ',';
        const AccentMark& mark = clip.accents[i];
        out << "{\"beat\":" << mark.beat << ",\"strength\":" << mark.strength
            << ",\"confidence\":" << mark.confidence << ",\"kind\":\""
            << escapeJson(mark.kind) << "\"}";
    }
    out << "],\"contacts\":[";
    for (std::size_t i = 0; i < clip.contacts.size(); ++i) {
        if (i) out << ',';
        const ContactSegment& seg = clip.contacts[i];
        out << "{\"foot\":\"" << (seg.foot == 0 ? "left" : "right")
            << "\",\"start\":" << seg.start << ",\"end\":" << seg.end
            << ",\"kind\":\"" << (seg.pivot ? "pivot" : "stance") << "\"}";
    }
    out << "],\"tracks\":{\"bones\":{";
    bool first = true;
    const auto beginTrack = [&](std::string_view name) {
        if (!first) out << ',';
        first = false;
        out << '"' << name << "\":";
    };
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto& track = clip.bones[b];
        if (track.size() != static_cast<std::size_t>(clip.keyCount)) {
            continue;
        }
        beginTrack(vrm::humanoidBoneName(static_cast<vrm::HumanoidBone>(b)));
        writeQuatArray(out, track);
    }
    out << "},\"hips\":";
    if (clip.hips.size() == static_cast<std::size_t>(clip.keyCount)) {
        writeVec3Array(out, clip.hips);
    } else {
        out << "[]";
    }
    out << ",\"root\":";
    if (clip.root.size() == static_cast<std::size_t>(clip.keyCount)) {
        writeVec3Array(out, clip.root);
    } else {
        out << "[]";
    }
    first = false;
    for (int f = 0; f < 2; ++f) {
        beginTrack(f == 0 ? "footLeft" : "footRight");
        writeFootTrack(out, clip.feet[static_cast<std::size_t>(f)], clip.keyCount);
    }
    for (int h = 0; h < 2; ++h) {
        if (clip.hands[static_cast<std::size_t>(h)].size() ==
            static_cast<std::size_t>(clip.keyCount)) {
            beginTrack(h == 0 ? "handLeft" : "handRight");
            writeFloatArray(out, clip.hands[static_cast<std::size_t>(h)].data(),
                            clip.hands[static_cast<std::size_t>(h)].size());
        }
        if (clip.handPoses[static_cast<std::size_t>(h)].size() ==
            static_cast<std::size_t>(clip.keyCount)) {
            beginTrack(h == 0 ? "handPoseLeft" : "handPoseRight");
            writeHandPoseTrack(out, clip.handPoses[static_cast<std::size_t>(h)]);
        }
    }
    out << "}}";
    return out.str();
}

bool saveClipFile(const HumanoidClip& clip, const std::filesystem::path& path,
                  std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    file << clipToJson(clip) << '\n';
    if (!file) {
        if (error) *error = "failed to write " + path.string();
        return false;
    }
    return true;
}

void validateClip(const HumanoidClip& clip, const std::string& sourceName,
                  std::vector<ClipDiagnostic>& out) {
    const auto error = [&](const std::string& m) {
        out.push_back({ClipDiagnostic::Severity::Error, sourceName, m});
    };
    const auto warn = [&](const std::string& m) {
        out.push_back({ClipDiagnostic::Severity::Warning, sourceName, m});
    };
    if (clip.id.empty()) error("clip has no id");
    if (!(clip.bpm > 20.0 && clip.bpm < 400.0)) error("bpm must be within 20..400");
    if (!(clip.bpmMin > 0.0 && clip.bpmMax >= clip.bpmMin)) error("bpmRange is invalid");
    if (clip.beats < 1.0f) error("a clip needs at least one beat");
    if (clip.keyCount < 2) {
        error("clip has no keys");
        return;
    }
    bool anyTrack = !clip.hips.empty() || !clip.root.empty();
    for (const auto& bone : clip.bones) {
        anyTrack = anyTrack || !bone.empty();
    }
    if (!anyTrack) error("clip has no animation tracks");
    for (float entry : clip.entryBeats) {
        if (entry < 0.0f || entry > clip.beats) warn("entry beat outside the clip");
    }
    for (const AccentMark& a : clip.accents) {
        if (a.beat < 0.0f || a.beat > clip.beats) warn("accent beat outside the clip");
        if (!(a.confidence >= 0.0f && a.confidence <= 1.0f)) {
            warn("accent confidence must be within 0..1");
        }
    }

    // Contact segments.
    for (std::size_t i = 0; i < clip.contacts.size(); ++i) {
        const ContactSegment& a = clip.contacts[i];
        if (!(a.end > a.start) || a.start < -1e-4f || a.end > clip.beats + 1e-4f) {
            error("contact segment " + std::to_string(i) + " has an invalid range");
        }
        for (std::size_t j = i + 1; j < clip.contacts.size(); ++j) {
            const ContactSegment& b = clip.contacts[j];
            if (a.foot == b.foot && a.start < b.end - 1e-4f && b.start < a.end - 1e-4f) {
                error("contact segments " + std::to_string(i) + " and " +
                      std::to_string(j) + " overlap on the same foot");
            }
        }
        // A planted foot must not translate.
        const FootTrack& track = clip.feet[static_cast<std::size_t>(a.foot & 1)];
        if (track.position.size() == static_cast<std::size_t>(clip.keyCount)) {
            const int k0 = std::clamp(static_cast<int>(std::ceil(a.start * clip.keysPerBeat - 1e-4)), 0,
                                      clip.keyCount - 1);
            const int k1 = std::clamp(static_cast<int>(std::floor(a.end * clip.keysPerBeat + 1e-4)), 0,
                                      clip.keyCount - 1);
            float worst = 0.0f;
            for (int k = k0; k <= k1; ++k) {
                const Vec3 d = track.position[static_cast<std::size_t>(k)] -
                               track.position[static_cast<std::size_t>(k0)];
                worst = std::max(worst, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
            }
            if (worst > 0.004f) {
                warn("foot " + std::string(a.foot == 0 ? "left" : "right") +
                     " moves " + std::to_string(worst) +
                     " height-units during a planted contact (slide)");
            }
        }
    }

    // BG Motion 3.2 detailed hand tracks must match the key count and stay in
    // their documented ranges; a malformed hand pose is a hard error because it
    // would otherwise be silently clamped into a different gesture.
    for (int h = 0; h < 2; ++h) {
        const auto& track = clip.handPoses[static_cast<std::size_t>(h)];
        if (track.empty()) {
            continue;
        }
        if (track.size() != static_cast<std::size_t>(clip.keyCount)) {
            error(std::string("hand pose ") + (h == 0 ? "left" : "right") +
                  " track does not match the key count");
            continue;
        }
        for (const HandPose& pose : track) {
            bool finite = true;
            for (const auto& finger : pose.curl) {
                for (float v : finger) finite = finite && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
            }
            for (float v : pose.spread) finite = finite && std::isfinite(v) && v >= -1.0f && v <= 1.0f;
            for (float v : pose.wrist) finite = finite && std::isfinite(v) && v >= -1.0f && v <= 1.0f;
            finite = finite && std::isfinite(pose.thumbOpposition) &&
                     pose.thumbOpposition >= 0.0f && pose.thumbOpposition <= 1.0f;
            if (!finite) {
                error(std::string("hand pose ") + (h == 0 ? "left" : "right") +
                      " has values outside 0..1 / -1..1");
                break;
            }
        }
    }

    // Loop seam.
    if (clip.loop && clip.seamless) {
        float rotWorst = 0.0f;
        for (const auto& bone : clip.bones) {
            if (bone.size() == static_cast<std::size_t>(clip.keyCount)) {
                const float d = std::abs(math::dot(bone.front(), bone.back()));
                rotWorst = std::max(rotWorst, 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f)));
            }
        }
        float posWorst = 0.0f;
        const auto check = [&](const std::vector<Vec3>& track) {
            if (track.size() == static_cast<std::size_t>(clip.keyCount)) {
                const Vec3 d = track.front() - track.back();
                posWorst = std::max(posWorst, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
            }
        };
        check(clip.hips);
        check(clip.root);
        check(clip.feet[0].position);
        check(clip.feet[1].position);
        float scalarWorst = 0.0f;
        for (const auto& foot : clip.feet) {
            if (foot.toe.size() == static_cast<std::size_t>(clip.keyCount)) {
                scalarWorst = std::max(scalarWorst, std::abs(foot.toe.front() - foot.toe.back()));
            }
        }
        for (int h = 0; h < 2; ++h) {
            const auto& track = clip.handPoses[static_cast<std::size_t>(h)];
            if (track.size() == static_cast<std::size_t>(clip.keyCount)) {
                const HandPose& first = track.front();
                const HandPose& last = track.back();
                for (int finger = 0; finger < 5; ++finger) {
                    for (int joint = 0; joint < 3; ++joint) {
                        scalarWorst = std::max(
                            scalarWorst,
                            std::abs(first.curl[static_cast<std::size_t>(finger)]
                                          [static_cast<std::size_t>(joint)] -
                                     last.curl[static_cast<std::size_t>(finger)]
                                         [static_cast<std::size_t>(joint)]));
                    }
                    scalarWorst = std::max(
                        scalarWorst,
                        std::abs(first.spread[static_cast<std::size_t>(finger)] -
                                 last.spread[static_cast<std::size_t>(finger)]));
                }
                scalarWorst = std::max(scalarWorst,
                                       std::abs(first.thumbOpposition - last.thumbOpposition));
            }
        }
        if (rotWorst > 0.01f || posWorst > 0.002f || scalarWorst > 0.005f) {
            error("clip is marked seamless but the last key differs from the first (rot " +
                  std::to_string(rotWorst * 57.29578f) + " deg, pos " +
                  std::to_string(posWorst) + ", scalar " + std::to_string(scalarWorst) + ")");
        }
    }
}

// --- BG Motion 3.4 variety ----------------------------------------------------

namespace {

// Quintic smootherstep: C2 at both ends of a seam window, so the blended choice has no
// visible kink where the window starts or ends.
float smootherstepUnit(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

Quat mirrorQuat(const Quat& q) {
    return {q.x, -q.y, -q.z, q.w};
}

Vec3 mirrorLateral(const Vec3& v) {
    return {-v.x, v.y, v.z};
}

std::string toggleSuffix(const std::string& text, const std::string& suffix) {
    if (text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return text.substr(0, text.size() - suffix.size());
    }
    return text + suffix;
}

}  // namespace

vrm::HumanoidBone mirroredBone(vrm::HumanoidBone bone) {
    using B = vrm::HumanoidBone;
    static constexpr std::array<std::pair<B, B>, 9> kPairs = {{
        {B::LeftShoulder, B::RightShoulder},
        {B::LeftUpperArm, B::RightUpperArm},
        {B::LeftLowerArm, B::RightLowerArm},
        {B::LeftHand, B::RightHand},
        {B::LeftUpperLeg, B::RightUpperLeg},
        {B::LeftLowerLeg, B::RightLowerLeg},
        {B::LeftFoot, B::RightFoot},
        {B::LeftToes, B::RightToes},
        {B::LeftEye, B::RightEye},
    }};
    for (const auto& [left, right] : kPairs) {
        if (bone == left) return right;
        if (bone == right) return left;
    }
    return bone;
}

HumanoidClip mirrorClip(const HumanoidClip& clip) {
    HumanoidClip out = clip;
    out.id = toggleSuffix(clip.id, kMirrorSuffix);
    out.name = toggleSuffix(clip.name, " (mirrored)");

    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        const auto bone = static_cast<vrm::HumanoidBone>(b);
        const std::vector<Quat>& source = clip.bones[b];
        std::vector<Quat>& target = out.bones[static_cast<std::size_t>(mirroredBone(bone))];
        target.resize(source.size());
        for (std::size_t k = 0; k < source.size(); ++k) {
            target[k] = mirrorQuat(source[k]);
        }
    }
    for (Vec3& v : out.hips) {
        v = mirrorLateral(v);
    }
    for (Vec3& r : out.root) {
        // root: x (left), z (forward) stored in y, yaw (turns) stored in z
        r = {-r.x, r.y, -r.z};
    }

    // Feet: the track of each side moves to the other side; lateral position and yaw flip.
    std::swap(out.feet[0], out.feet[1]);
    for (FootTrack& track : out.feet) {
        for (Vec3& p : track.position) {
            p = mirrorLateral(p);
        }
        for (float& yaw : track.yaw) {
            yaw = -yaw;
        }
        // toe roll is a pitch about the lateral axis: unchanged
    }
    for (ContactSegment& seg : out.contacts) {
        seg.foot = 1 - seg.foot;
    }

    std::swap(out.hands[0], out.hands[1]);
    std::swap(out.handPoses[0], out.handPoses[1]);
    for (auto& track : out.handPoses) {
        for (HandPose& pose : track) {
            // wrist: pitch (flexion about the lateral axis) stays, yaw and roll flip
            pose.wrist[1] = -pose.wrist[1];
            pose.wrist[2] = -pose.wrist[2];
            // finger curl and thumb opposition are sided by the hand, not by the body: unchanged
        }
    }
    return out;
}

LoopBlend loopBlendAt(double clipBeat, double loopBeats, double halfWindowBeats) {
    LoopBlend blend;
    if (!(loopBeats > 1e-9) || !std::isfinite(clipBeat)) {
        return blend;
    }
    const double loopPos = clipBeat / loopBeats;
    const double seam = std::round(loopPos);
    const double half = std::min(std::max(halfWindowBeats, 0.0), 0.5 * loopBeats);
    const double distance = (loopPos - seam) * loopBeats;  // beats from the nearest seam
    if (half <= 1e-9 || std::abs(distance) >= half) {
        blend.from = blend.to = static_cast<std::int64_t>(std::floor(loopPos));
        return blend;
    }
    blend.from = static_cast<std::int64_t>(seam) - 1;
    blend.to = static_cast<std::int64_t>(seam);
    blend.w = smootherstepUnit(static_cast<float>((distance + half) / (2.0 * half)));
    return blend;
}

std::uint64_t variationMix(std::uint64_t value) {
    // splitmix64 finaliser
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

std::uint64_t variationHash(const std::string& text) {
    // FNV-1a, 64 bit
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// --- library ---------------------------------------------------------------

bool DanceLibrary::replace(const HumanoidClip& clip) {
    for (HumanoidClip& existing : clips_) {
        if (existing.id == clip.id) {
            existing = clip;
            if (const auto mirror = mirrorIndex_.find(clip.id); mirror != mirrorIndex_.end()) {
                mirrors_[mirror->second] = mirrorClip(clip);
            }
            return true;
        }
    }
    return false;
}

const HumanoidClip* DanceLibrary::mirrorOf(const HumanoidClip& clip) const {
    const auto found = mirrorIndex_.find(clip.id);
    if (found != mirrorIndex_.end()) {
        return &mirrors_[found->second];
    }
    mirrors_.push_back(mirrorClip(clip));
    mirrorIndex_.emplace(clip.id, mirrors_.size() - 1);
    return &mirrors_.back();
}

bool DanceLibrary::add(HumanoidClip clip) {
    if (find(clip.id)) {
        diagnostics_.push_back({ClipDiagnostic::Severity::Error, clip.id,
                                "duplicate clip id '" + clip.id + "'"});
        return false;
    }
    clips_.push_back(std::move(clip));
    return true;
}

void DanceLibrary::loadDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        diagnostics_.push_back({ClipDiagnostic::Severity::Error, directory.string(),
                                "dance directory does not exist"});
        return;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        ClipLoadResult loaded = loadClipFile(file);
        for (const ClipDiagnostic& d : loaded.diagnostics) {
            diagnostics_.push_back(d);
        }
        if (loaded.ok) {
            (void)add(std::move(loaded.clip));
        }
    }
}

const HumanoidClip* DanceLibrary::findOriginal(const std::string& id) const {
    for (const HumanoidClip& clip : clips_) {
        if (clip.id == id) return &clip;
    }
    return nullptr;
}

const HumanoidClip* DanceLibrary::find(const std::string& id) const {
    if (const HumanoidClip* clip = findOriginal(id)) {
        return clip;
    }
    const std::string suffix = kMirrorSuffix;
    if (id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0) {
        if (const HumanoidClip* original = findOriginal(id.substr(0, id.size() - suffix.size()))) {
            const HumanoidClip* mirror = mirrorOf(*original);
            return mirror->id == id ? mirror : nullptr;
        }
    }
    return nullptr;
}

const HumanoidClip* DanceLibrary::forStyle(DanceStyle style) const {
    for (const HumanoidClip& clip : clips_) {
        if (clip.style == style) return &clip;
    }
    return nullptr;
}

} // namespace bgmedia::motion
