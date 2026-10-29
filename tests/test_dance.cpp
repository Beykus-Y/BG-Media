#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/motion3/AliveLayer.hpp"
#include "bgmedia/motion3/DancePlayer.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bgmedia;

std::filesystem::path dancesDir() {
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "assets" / "dances";
}

std::filesystem::path minimalVrm() {
    return std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "minimal.vrm";
}

std::filesystem::path model1Path() {
    if (const char* env = std::getenv("BGMEDIA_VRM_TEST_MODEL")) {
        return env;
    }
    return std::filesystem::path(BGMEDIA_SOURCE_DIR) / "models" / "Model1.vrm";
}

const motion::DanceLibrary& library() {
    static const motion::DanceLibrary lib = [] {
        motion::DanceLibrary l;
        l.loadDirectory(dancesDir());
        return l;
    }();
    return lib;
}

struct Rig {
    vrm::LoadResult loaded;
    std::unique_ptr<animation::HumanoidRetargeter> rig;
    bool ok() const { return loaded.ok() && rig && rig->valid(); }
};

Rig makeRig(const std::filesystem::path& path) {
    Rig r;
    r.loaded = vrm::VrmLoader::load(path);
    if (r.loaded.ok()) {
        r.rig = std::make_unique<animation::HumanoidRetargeter>(*r.loaded.model);
    }
    return r;
}

// A tiny valid clip as JSON text, used by the loader tests.
std::string tinyClipJson(const std::string& mutation = {}) {
    std::ostringstream out;
    const int keys = 2 * 4 + 1;  // 2 beats x 4 keys + 1
    const auto quats = [&](const char* q) {
        std::string s = "[";
        for (int i = 0; i < keys; ++i) {
            s += (i ? "," : "");
            s += q;
        }
        return s + "]";
    };
    const auto zeros = [&](int stride) {
        std::string s = "[";
        for (int i = 0; i < keys * stride; ++i) {
            s += (i ? ",0" : "0");
        }
        return s + "]";
    };
    out << R"({"format":"bgmotion.clip","version":1,"id":"tiny","name":"Tiny","style":"groove",)"
        << R"("bpm":100,"beats":2,"keysPerBeat":4,"loop":true,"seamless":true,)"
        << R"("tracks":{"bones":{"hips":)" << quats("0,0,0,1") << "},"
        << R"("hips":)" << zeros(3) << R"(,"root":)" << zeros(3) << R"(,"footLeft":)" << zeros(4)
        << R"(,"footRight":)" << zeros(4) << "}}";
    std::string text = out.str();
    if (mutation == "badversion") {
        text.replace(text.find("\"version\":1"), 11, "\"version\":99");
    } else if (mutation == "badbone") {
        text.replace(text.find("\"hips\":["), 8, "\"tailbone\":[");
    } else if (mutation == "badnorm") {
        text.replace(text.find("0,0,0,1"), 7, "0,0,0,3");
    } else if (mutation == "noid") {
        text.replace(text.find("\"id\":\"tiny\","), 12, "");
    } else if (mutation == "short") {
        text.replace(text.find("\"keysPerBeat\":4"), 15, "\"keysPerBeat\":8");
    } else if (mutation == "syntax") {
        text.replace(text.find("\"bpm\":100"), 9, "\"bpm\" 100");
    } else if (mutation == "seam") {
        text.replace(text.find("0,0,0,1]"), 8, "0,0.5,0,0.866]");
    }
    return text;
}

bool hasMessage(const motion::ClipLoadResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics) {
        if (d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Format, loader, diagnostics
// ---------------------------------------------------------------------------

TEST_CASE(dance.json_valid_clip_loads) {
    const motion::ClipLoadResult r = motion::parseClipJson(tinyClipJson(), "tiny.json");
    CHECK(r.ok);
    CHECK(r.clip.id == "tiny");
    CHECK(r.clip.keyCount == 9);
    CHECK(r.clip.style == motion::DanceStyle::Groove);
    const motion::ClipPose pose = motion::sampleClip(r.clip, 0.7);
    CHECK_NEAR(pose.bone[static_cast<std::size_t>(vrm::HumanoidBone::Hips)].w, 1.0, 1e-5);
}

TEST_CASE(dance.json_invalid_clips_give_clear_errors) {
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("badversion"), "a"), "unsupported clip format version"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("badbone"), "a"), "unknown humanoid bone 'tailbone'"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("badnorm"), "a"), "not a unit quaternion"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("noid"), "a"), "missing required field 'id'"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("short"), "a"), "expected"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("syntax"), "a"), "line 1, column"));
    CHECK(hasMessage(motion::parseClipJson(tinyClipJson("seam"), "a"), "seamless"));
    CHECK(!motion::parseClipJson("", "empty").ok);
    CHECK(!motion::parseClipJson("[1,2,3]", "array").ok);
    CHECK(!motion::loadClipFile(dancesDir() / "does_not_exist.json").ok);
    // Diagnostics carry the file name.
    const auto r = motion::parseClipJson(tinyClipJson("noid"), "myclip.json");
    CHECK(!r.diagnostics.empty() && r.diagnostics.front().file == "myclip.json");
}

TEST_CASE(dance.json_random_corruption_never_crashes) {
    const std::string good = tinyClipJson();
    std::uint32_t state = 12345u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    int accepted = 0;
    for (int i = 0; i < 400; ++i) {
        std::string text = good;
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = next() % text.size();
            switch (next() % 3) {
            case 0: text[at] = static_cast<char>(32 + next() % 90); break;
            case 1: text.erase(at, 1 + next() % 8); break;
            default: text.insert(at, 1, static_cast<char>(32 + next() % 90)); break;
            }
            if (text.empty()) text = "x";
        }
        const motion::ClipLoadResult r = motion::parseClipJson(text, "fuzz");
        accepted += r.ok ? 1 : 0;
        if (r.ok) {
            (void)motion::sampleClip(r.clip, 0.3);  // an accepted clip must be playable
        }
    }
    CHECK(accepted < 400);
}

TEST_CASE(dance.library_ships_the_four_dances) {
    const motion::DanceLibrary& lib = library();
    for (const auto& d : lib.diagnostics()) {
        CHECK(d.severity != motion::ClipDiagnostic::Severity::Error);
    }
    CHECK(lib.clips().size() >= 5);
    const motion::HumanoidClip* groove = lib.forStyle(motion::DanceStyle::Groove);
    const motion::HumanoidClip* hiphop = lib.forStyle(motion::DanceStyle::HipHop);
    const motion::HumanoidClip* side = lib.forStyle(motion::DanceStyle::SideStep);
    const motion::HumanoidClip* energetic = lib.forStyle(motion::DanceStyle::Energetic);
    CHECK(groove && hiphop && side && energetic && lib.forStyle(motion::DanceStyle::Idle));
    CHECK(groove->beats >= 8.0f);
    CHECK(hiphop->beats >= 16.0f);
    CHECK(side->beats >= 8.0f);
    CHECK(energetic->beats >= 16.0f);
    for (const motion::HumanoidClip& clip : lib.clips()) {
        CHECK(!clip.id.empty() && !clip.name.empty() && clip.bpm > 0.0);
        CHECK(clip.bpmMin < clip.bpmMax);
        CHECK(clip.formatVersion >= 1 &&
              clip.formatVersion <= motion::kClipFormatVersion);
        CHECK(!clip.origin.empty());
        if (clip.style != motion::DanceStyle::Idle) {
            CHECK(!clip.accents.empty());
            CHECK(!clip.contacts.empty());
            CHECK(!clip.entryBeats.empty() && !clip.exitBeats.empty());
            CHECK(clip.loop && clip.seamless);
        }
    }
}

TEST_CASE(dance.clips_are_normalized_and_model_independent) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            for (const math::Quat& q : clip.bones[b]) {
                CHECK_NEAR(std::sqrt(q.lengthSquared()), 1.0, 1e-3);
                CHECK(std::isfinite(q.x + q.y + q.z + q.w));
            }
        }
        // Leg bones are never keyed: legs come from the foot targets.
        CHECK(clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperLeg)].empty());
        CHECK(clip.bones[static_cast<std::size_t>(vrm::HumanoidBone::RightFoot)].empty());
    }
}

// ---------------------------------------------------------------------------
// Motion quality
// ---------------------------------------------------------------------------

TEST_CASE(dance.accent_confidence_is_optional_and_affects_the_envelope) {
    const std::string marker = "\"tracks\"";
    std::string base = tinyClipJson();
    {
        const std::size_t at = base.find(marker);
        REQUIRE(at != std::string::npos);
        std::string text = base;
        text.insert(at, "\"accents\":[{\"beat\":0,\"strength\":1}],");
        const motion::ClipLoadResult loaded = motion::parseClipJson(text, "no-confidence.json");
        CHECK(loaded.ok);
        REQUIRE(loaded.clip.accents.size() == 1);
        CHECK_NEAR(loaded.clip.accents[0].confidence, 1.0, 1e-6);
        CHECK_NEAR(motion::clipAccentStrength(loaded.clip, 0.0), 1.0, 1e-6);
    }
    {
        const std::size_t at = base.find(marker);
        std::string text = base;
        text.insert(at,
                    "\"accents\":[{\"beat\":0,\"strength\":1,\"confidence\":0.4}],");
        const motion::ClipLoadResult loaded = motion::parseClipJson(text, "confident.json");
        CHECK(loaded.ok);
        REQUIRE(loaded.clip.accents.size() == 1);
        CHECK_NEAR(loaded.clip.accents[0].confidence, 0.4, 1e-6);
        CHECK_NEAR(motion::clipAccentStrength(loaded.clip, 0.0), 0.4, 1e-5);
    }
    {
        const std::size_t at = base.find(marker);
        std::string text = base;
        text.insert(at,
                    "\"accents\":[{\"beat\":0,\"strength\":1,\"confidence\":3}],");
        const motion::ClipLoadResult loaded = motion::parseClipJson(text, "bad-confidence.json");
        CHECK(loaded.ok);   // a warning, not a rejected clip
        CHECK(hasMessage(loaded, "confidence"));
    }
}

TEST_CASE(dance.loops_have_no_seam_jump) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        const qa::ClipReport r = qa::analyseClip(clip, nullptr, nullptr, clip.bpm);
        CHECK(r.loopRotationDeg < 0.05);
        CHECK(r.loopPositionHeight < 0.001);
        // Velocity continuity: the angular speed jump at the seam stays small compared
        // with the clip's own peak speed.
        CHECK(r.loopVelocityJumpDegPerSec < 0.08 * std::max(r.maxAngularSpeed, 50.0));
    }
}

TEST_CASE(dance.joint_speed_and_acceleration_are_bounded) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        for (double bpm : {clip.bpmMin, clip.bpm, clip.bpmMax}) {
            const qa::ClipReport r = qa::analyseClip(clip, nullptr, nullptr, bpm);
            CHECK(r.maxQuatNormError < 1e-4);
            CHECK(r.maxAngularSpeed < 1000.0);          // deg/s: no whip-crack joints
            CHECK(r.maxAngularAcceleration < 2.5e5);    // deg/s^2: no pathological spikes
            CHECK(r.maxHipsSpeed < 3.0);                // body heights per second
            CHECK(r.maxFootSpeed < 8.0);
        }
    }
}

TEST_CASE(dance.root_motion_is_bounded_and_closed) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        const qa::ClipReport r = qa::analyseClip(clip, nullptr, nullptr, clip.bpm);
        CHECK(r.maxRootExcursion < 0.35);   // stays well inside the camera frame
        CHECK(r.maxRootYawDeg <= 181.0);
        const motion::ClipPose a = motion::sampleClip(clip, 0.0);
        const motion::ClipPose b = motion::sampleClip(clip, clip.beats - 1e-6);
        CHECK_NEAR(a.rootX, b.rootX, 1e-3);
        CHECK_NEAR(a.rootZ, b.rootZ, 1e-3);
        CHECK_NEAR(a.rootYaw, b.rootYaw, 1e-3);
    }
}

TEST_CASE(dance.styles_have_distinct_choreography) {
    const auto& lib = library();
    const motion::DanceStyle styles[] = {motion::DanceStyle::Groove, motion::DanceStyle::HipHop,
                                         motion::DanceStyle::SideStep, motion::DanceStyle::Energetic};
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            const auto& ca = *lib.forStyle(styles[a]);
            const auto& cb = *lib.forStyle(styles[b]);
            // Compare the pose at the same musical positions over 16 beats.
            double bone = 0.0;
            double foot = 0.0;
            int n = 0;
            for (double beat = 0.0; beat < 16.0; beat += 0.25, ++n) {
                const motion::ClipPose pa = motion::sampleClip(ca, beat);
                const motion::ClipPose pb = motion::sampleClip(cb, beat);
                for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
                    bone += qa::angleBetween(pa.bone[i], pb.bone[i]);
                }
                foot += (pa.foot[0].position - pb.foot[0].position).length() +
                        (pa.foot[1].position - pb.foot[1].position).length();
            }
            CHECK(bone / (n * 14.0) > 6.0);   // degrees per active bone
            CHECK(foot / n > 0.02);           // height units
        }
    }
}

TEST_CASE(dance.dances_use_the_whole_body) {
    for (motion::DanceStyle style : {motion::DanceStyle::Groove, motion::DanceStyle::HipHop,
                                     motion::DanceStyle::SideStep, motion::DanceStyle::Energetic}) {
        const motion::HumanoidClip& clip = *library().forStyle(style);
        float range[vrm::kHumanoidBoneCount] = {};
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            const auto& track = clip.bones[b];
            for (const math::Quat& q : track) {
                range[b] = std::max<float>(range[b], static_cast<float>(qa::angleBetween(q, track.front())));
            }
        }
        using B = vrm::HumanoidBone;
        const auto r = [&](B bone) { return range[static_cast<std::size_t>(bone)]; };
        CHECK(r(B::LeftUpperArm) > 40.0f && r(B::RightUpperArm) > 40.0f);  // big arm travel
        CHECK(r(B::LeftLowerArm) > 25.0f && r(B::RightLowerArm) > 25.0f);  // elbows work
        CHECK(r(B::Chest) > 5.0f);                                        // torso
        CHECK(r(B::Hips) > 4.0f);                                         // pelvis
        CHECK(r(B::Head) > 5.0f);                                         // head
        CHECK(r(B::LeftShoulder) + r(B::RightShoulder) > 2.0f);           // shoulders
        double hipsLow = 0.0;
        for (const math::Vec3& h : clip.hips) hipsLow = std::min<double>(hipsLow, h.y);
        CHECK(hipsLow < -0.04);                                           // knees bend
    }
}

// ---------------------------------------------------------------------------
// Feet
// ---------------------------------------------------------------------------

TEST_CASE(dance.foot_contacts_declared_and_planted_feet_do_not_slide) {
    Rig rig = makeRig(minimalVrm());
    CHECK(rig.ok());
    for (const motion::HumanoidClip& clip : library().clips()) {
        const qa::ClipReport r =
            qa::analyseClip(clip, rig.loaded.model.get(), rig.rig.get(), clip.bpm, 120.0);
        CHECK(r.slideHeightFraction < 0.0035);  // < ~5.6 mm on a 1.6 m character
        CHECK(r.footErrorMax < 0.004);          // never pulled off a target by the reach clamp
    }
}

TEST_CASE(dance.footwork_moves_the_feet_not_just_the_knees) {
    const motion::HumanoidClip& side = *library().forStyle(motion::DanceStyle::SideStep);
    const qa::ClipReport r = qa::analyseClip(side, nullptr, nullptr, side.bpm);
    CHECK(r.swings >= 10);               // a foot leaves the floor on almost every beat
    CHECK(r.swingDistanceMax > 0.12);    // real steps, not shuffles in place
    CHECK(r.footPathLength > 1.5);       // body heights travelled by both feet in one loop
    const qa::ClipReport groove =
        qa::analyseClip(*library().forStyle(motion::DanceStyle::Groove), nullptr, nullptr, 96.0);
    CHECK(groove.swings >= 6);
    CHECK(groove.swingDistanceMax > 0.08);
}

TEST_CASE(dance.turns_pivot_on_a_planted_foot) {
    const motion::HumanoidClip& energetic = *library().forStyle(motion::DanceStyle::Energetic);
    bool pivot = false;
    for (const auto& c : energetic.contacts) {
        pivot = pivot || c.pivot;
    }
    CHECK(pivot);
    const qa::ClipReport r = qa::analyseClip(energetic, nullptr, nullptr, energetic.bpm);
    CHECK(r.maxRootYawDeg > 170.0);
    const motion::HumanoidClip& hiphop = *library().forStyle(motion::DanceStyle::HipHop);
    const qa::ClipReport h = qa::analyseClip(hiphop, nullptr, nullptr, hiphop.bpm);
    CHECK(h.maxRootYawDeg > 30.0);
}

// ---------------------------------------------------------------------------
// Retargeting, portability, left / right
// ---------------------------------------------------------------------------

TEST_CASE(dance.neutral_pose_reproduces_the_bind_pose) {
    Rig rig = makeRig(minimalVrm());
    CHECK(rig.ok());
    const vrm::VrmModel& model = *rig.loaded.model;
    motion::ClipPose pose = motion::neutralClipPose();
    // the bind ankles are the neutral foot targets
    const animation::RetargetedPose out = rig.rig->applyClip(pose, 0.0);
    CHECK(out.stats.valid);
    CHECK(out.stats.maxFootError < 0.002f);
    for (vrm::HumanoidBone bone : {vrm::HumanoidBone::Head, vrm::HumanoidBone::LeftHand,
                                   vrm::HumanoidBone::RightHand, vrm::HumanoidBone::LeftFoot}) {
        if (!model.hasBone(bone)) continue;
        const math::Vec3 a = out.bonePosition(bone, model);
        const math::Vec3 b = model.boneBindPosition(bone);
        // hips settle a little (reach margin), everything else must be close
        CHECK((a - b).length() < 0.06f * model.height);
    }
}

TEST_CASE(dance.left_arm_clip_motion_moves_the_models_left_arm) {
    // Clip frame: +X is the character's LEFT. Raising the clip's left arm must lift
    // the VRM's *left* hand on both the tiny fixture and the real model.
    for (const std::filesystem::path& path : {minimalVrm(), model1Path()}) {
        if (!std::filesystem::exists(path)) continue;
        Rig rig = makeRig(path);
        CHECK(rig.ok());
        const vrm::VrmModel& model = *rig.loaded.model;
        motion::ClipPose pose = motion::neutralClipPose();
        // 90 degrees about +Z (forward) in the clip frame lifts a left arm from hanging to T-pose
        const math::Quat lift = math::fromAxisAngle({0, 0, 1}, 1.2f);
        pose.bone[static_cast<std::size_t>(vrm::HumanoidBone::LeftUpperArm)] = lift;
        const animation::RetargetedPose base = rig.rig->applyClip(motion::neutralClipPose(), 0.0);
        const animation::RetargetedPose moved = rig.rig->applyClip(pose, 0.0);
        const math::Vec3 handBase = base.bonePosition(vrm::HumanoidBone::LeftHand, model);
        const math::Vec3 handMoved = moved.bonePosition(vrm::HumanoidBone::LeftHand, model);
        const math::Vec3 rightBase = base.bonePosition(vrm::HumanoidBone::RightHand, model);
        const math::Vec3 rightMoved = moved.bonePosition(vrm::HumanoidBone::RightHand, model);
        CHECK((handMoved - handBase).length() > 0.05f * model.height);
        CHECK((rightMoved - rightBase).length() < 1e-4f);
        // and the left hand stays on the character's left side (-right axis)
        CHECK(math::dot(handMoved - rightMoved, model.right) < 0.0f);
    }
}

TEST_CASE(dance.all_clips_retarget_on_a_different_skeleton) {
    Rig rig = makeRig(minimalVrm());
    CHECK(rig.ok());
    for (const motion::HumanoidClip& clip : library().clips()) {
        for (double beat = 0.0; beat < clip.beats; beat += 0.37) {
            const motion::ClipPose pose = motion::sampleClip(clip, beat);
            const animation::RetargetedPose out = rig.rig->applyClip(pose, beat);
            CHECK(out.stats.valid);
            for (const math::Mat4& m : out.nodeGlobals) {
                CHECK(m.finite());
            }
        }
    }
}

TEST_CASE(dance.model1_if_available) {
    const std::filesystem::path path = model1Path();
    if (!std::filesystem::exists(path)) {
        std::cout << "            Model1.vrm not present: skipped";
        return;
    }
    Rig rig = makeRig(path);
    CHECK(rig.ok());
    for (const motion::HumanoidClip& clip : library().clips()) {
        const qa::ClipReport r =
            qa::analyseClip(clip, rig.loaded.model.get(), rig.rig.get(), clip.bpm, 120.0);
        CHECK(r.footErrorMax < 0.004);
        CHECK(r.slideHeightFraction < 0.0035);
        CHECK(r.hipsDropMax < 0.25);   // automatic reach drop stays sane
    }
}

// ---------------------------------------------------------------------------
// Musical time, schedule, determinism
// ---------------------------------------------------------------------------

TEST_CASE(dance.beat_clock_maps_time_and_beats_consistently) {
    const motion::BeatClock constant = motion::BeatClock::constant(120.0, 0.5);
    CHECK_NEAR(constant.beatAt(0.5), 0.0, 1e-9);
    CHECK_NEAR(constant.beatAt(1.5), 2.0, 1e-9);
    CHECK_NEAR(constant.timeAtBeat(4.0), 2.5, 1e-9);
    CHECK_NEAR(constant.bpmAt(10.0), 120.0, 1e-9);

    // An analysis with jittery beats gives a smooth monotonic grid.
    AnalysisResult analysis;
    analysis.info.duration = 40.0;
    analysis.bpm = 120.0;
    analysis.tempoStable = true;
    analysis.bpmConfidence = 0.9f;
    analysis.beatPhaseConfidence = 0.9f;
    std::uint32_t state = 7;
    for (int i = 0; i < 80; ++i) {
        state = state * 1664525u + 1013904223u;
        Beat beat;
        beat.time = 0.4 + i * 0.5 + (static_cast<int>((state >> 10) % 41) - 20) * 0.001;  // +-20 ms jitter
        beat.beatInBar = i % 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
    }
    const motion::BeatClock clock = motion::BeatClock::fromAnalysis(analysis);
    double lastBeat = -1e9;
    for (double t = 0.0; t < 40.0; t += 0.013) {
        const double beat = clock.beatAt(t);
        CHECK(beat > lastBeat);
        lastBeat = beat;
        CHECK_NEAR(clock.timeAtBeat(beat), t, 1e-6);
    }
    // smooth: the phase never deviates from the ideal grid by more than ~5 ms
    for (int i = 5; i < 70; ++i) {
        CHECK_NEAR(clock.timeAtBeat(i), 0.4 + i * 0.5, 0.008);
    }
}

TEST_CASE(dance.schedule_is_a_pure_function_of_media_time) {
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(112.0, 0.0));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    director.select(motion::DanceStyle::HipHop, 7.3);
    director.select(motion::DanceStyle::Energetic, 15.1);
    const double times[] = {0.0, 3.3, 7.4, 7.9, 8.4, 15.2, 15.7, 20.0, 31.0};
    std::vector<motion::ClipPose> forward;
    for (double t : times) forward.push_back(director.sample(t).pose);
    // reverse order and repeated calls give identical poses
    for (int i = static_cast<int>(std::size(times)) - 1; i >= 0; --i) {
        const motion::ClipPose again = director.sample(times[i]).pose;
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            CHECK(qa::angleBetween(forward[static_cast<std::size_t>(i)].bone[b], again.bone[b]) < 0.3);
        }
        CHECK_NEAR(forward[static_cast<std::size_t>(i)].hips.y, again.hips.y, 1e-6);
    }
    // frame rate independence: sampling on 30/60/144 Hz grids agrees at common times
    for (double t = 7.0; t < 12.0; t += 0.5) {
        const motion::ClipPose ref = director.sample(t).pose;
        for (double hz : {30.0, 60.0, 144.0}) {
            const double snapped = std::round(t * hz) / hz;
            const motion::ClipPose near = director.sample(snapped).pose;
            // adjacent grid times are within 1/hz of t; the pose must follow time, not frame count
            double worst = 0.0;
            for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
                worst = std::max(worst, qa::angleBetween(ref.bone[b], near.bone[b]));
            }
            CHECK(worst < 40.0 * std::abs(snapped - t) * 60.0 + 0.5);
        }
    }
}

TEST_CASE(dance.pause_resume_and_seek) {
    motion::DancePlayer player;
    CHECK(player.loadLibrary(dancesDir()) >= 5);
    player.setGrid(motion::BeatClock::constant(100.0, 0.0), 0.0);
    player.select(motion::DanceStyle::Groove, 0.0, true);
    // paused: the same media time yields the same pose
    const motion::DanceFrame a = player.update(5.0);
    const motion::DanceFrame b = player.update(5.0);
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        CHECK(qa::angleBetween(a.pose.bone[i], b.pose.bone[i]) < 1e-4);
    }
    // seek far forward and backward: always finite, always dancing the selected style
    for (double t : {120.0, 3.0, 61.7, 0.0, 500.25}) {
        const motion::DanceFrame f = player.update(t);
        CHECK(f.clipId == "groove");
        CHECK(std::isfinite(f.pose.hips.y));
        // A seek re-seeds the clip, but the bar phase still follows the music.
        const double beat = player.director().beatClock().beatAt(t);
        double phase = std::fmod(f.clipBeat - beat, 4.0);
        if (phase > 2.0) phase -= 4.0;
        if (phase < -2.0) phase += 4.0;
        CHECK_NEAR(phase, 0.0, 1e-6);
    }
    // one shot returns to idle
    player.setOneShot(true);
    (void)player.update(599.9);
    player.select(motion::DanceStyle::HipHop, 600.0);
    motion::DanceFrame during;
    motion::DanceFrame after;
    for (double t = 600.0; t < 625.0; t += 0.1) {   // real playback: small time steps
        after = player.update(t);
        if (t > 602.9 && t < 603.1) {
            during = after;
        }
    }
    CHECK(during.clipId == "hiphop");
    if (after.clipId != "idle") {
        for (const auto& seg : player.director().schedule().segments()) {
            std::cout << " seg " << seg.clip->id << " start " << seg.startTime << " align " << seg.alignTime
                      << " entry " << seg.entryClipBeat << " rate " << seg.rate << " loop " << seg.loop << std::endl;
        }
        std::cout << " frame " << after.clipId << " next " << after.nextClipId << " clipBeat " << after.clipBeat << std::endl;
    }
    CHECK(after.clipId == "idle");
}

TEST_CASE(dance.transitions_are_continuous_between_all_styles) {
    const motion::DanceStyle styles[] = {motion::DanceStyle::Idle, motion::DanceStyle::Groove,
                                         motion::DanceStyle::HipHop, motion::DanceStyle::SideStep,
                                         motion::DanceStyle::Energetic};
    for (double bpm : {80.0, 123.0, 180.0}) {
        // Foot speed of each style on its own (authored motion, no transition). A transition
        // may not move a foot faster than the authored motion of the styles it joins.
        double steadyFoot[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
        for (std::size_t s = 0; s < 5; ++s) {
            motion::DanceDirector steady(&library());
            steady.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
            steady.reset(0.0);
            steady.select(styles[s], 0.0, false, true);
            steadyFoot[s] = qa::measureWindow(steady, 2.0, 12.0, 240.0).maxFootStep;
        }
        for (std::size_t fromIndex = 0; fromIndex < 5; ++fromIndex) {
            for (std::size_t toIndex = 0; toIndex < 5; ++toIndex) {
                if (fromIndex == toIndex) continue;
                const motion::DanceStyle from = styles[fromIndex];
                const motion::DanceStyle to = styles[toIndex];
                motion::DanceDirector director(&library());
                director.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
                director.reset(0.0);
                director.select(from, 0.0, false, true);
                const double switchAt = 60.0 / bpm * 9.37;
                director.select(to, switchAt);
                const qa::JumpReport j = qa::measureWindow(director, switchAt - 0.5, switchAt + 4.0, 240.0);
                CHECK(j.finite);
                CHECK(j.maxAngularSpeed < 900.0);        // no pose pops at the boundary
                CHECK(j.maxAngularAcceleration < 4.0e4);
                CHECK(j.maxHipsStep < 0.006);
                CHECK(j.maxRootStep < 0.006);
                // No teleporting feet. The largest step of a foot per 240 Hz frame must stay
                // within the authored foot speed of the two styles (the crossfade adds no
                // foot motion of its own), or within 3 m/s on a 1.7 m model, whichever is
                // larger: a 25 cm stance change has to fit inside one swing.
                const double authored = std::max(steadyFoot[fromIndex], steadyFoot[toIndex]);
                const double physical = 3.0 / (1.7 * 240.0);
                CHECK(j.maxFootStep < std::max(1.25 * authored, physical));
            }
        }
    }
}

TEST_CASE(dance.repeated_switching_stays_stable) {
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(128.0, 0.0));
    director.reset(0.0);
    const motion::DanceStyle styles[] = {motion::DanceStyle::Groove, motion::DanceStyle::HipHop,
                                         motion::DanceStyle::SideStep, motion::DanceStyle::Energetic,
                                         motion::DanceStyle::Idle};
    double t = 0.0;
    for (int i = 0; i < 60; ++i) {
        // switch faster than the fade length on purpose
        director.select(styles[(i * 3) % 5], t);
        t += 0.2 + 0.07 * (i % 5);
    }
    const qa::JumpReport j = qa::measureWindow(director, 0.0, t + 6.0, 240.0);
    CHECK(j.finite);
    CHECK(j.maxAngularSpeed < 1100.0);
    CHECK(j.maxHipsStep < 0.008);
    CHECK(j.maxFootStep < 0.025);   // switching every 0.2-0.5 s is faster than any real dance; loose on purpose
    CHECK(director.schedule().segments().size() < 80);
}

TEST_CASE(dance.tempo_levels_cover_80_to_180_bpm) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        if (!clip.tempoSync) continue;
        for (double bpm : {80.0, 92.0, 123.0, 160.0, 180.0}) {
            bool limited = false;
            const double rate = motion::DanceDirector::chooseRate(clip, bpm, &limited);
            CHECK(rate == 1.0 || rate == 0.5 || rate == 2.0);
            const double effective = bpm * rate;
            // never further than ~1.6x from the authored tempo; if it is outside the
            // clip's range the limit is reported, not hidden
            CHECK(effective / clip.bpm < 1.7 && effective / clip.bpm > 0.55);
            const bool inRange = effective >= clip.bpmMin - 0.5 && effective <= clip.bpmMax + 0.5;
            CHECK(inRange == !limited);
        }
    }
    // the dance runs at the music's pace: one clip beat per music beat at rate 1
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(96.0, 0.0));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    const motion::DanceFrame f1 = director.sample(10.0);
    const motion::DanceFrame f2 = director.sample(10.0 + 60.0 / 96.0);
    CHECK_NEAR(f2.clipBeat - f1.clipBeat, 1.0, 1e-6);
    CHECK_NEAR(f1.playbackSpeed, 1.0, 0.01);
}

TEST_CASE(dance.metric_level_has_hysteresis_near_the_range_edge) {
    const motion::HumanoidClip& energetic =
        *library().forStyle(motion::DanceStyle::Energetic);
    bool limited = false;
    // 180 BPM is only just outside Energetic's 92..176 range. A half-time
    // reading at 90 is almost the same distance from the authored 126 BPM, so
    // hysteresis keeps the natural 1x reading and reports the soft limit.
    CHECK_NEAR(motion::DanceDirector::chooseRate(energetic, 180.0, &limited), 1.0, 1e-9);
    CHECK(limited);
    // 80 BPM is far below the range: double-time is the honest metric level.
    CHECK_NEAR(motion::DanceDirector::chooseRate(energetic, 80.0, &limited), 2.0, 1e-9);
    CHECK(!limited);
}

TEST_CASE(dance.manual_selection_lands_on_a_beat) {
    for (double now : {1.31, 2.02, 4.77, 9.99}) {
        motion::DanceDirector director(&library());
        director.setBeatClock(motion::BeatClock::constant(110.0, 0.2));
        director.reset(0.0);
        director.select(motion::DanceStyle::HipHop, now);
        const auto& seg = director.schedule().segments().back();
        CHECK(seg.clip->id == "hiphop");
        const double beat = director.beatClock().beatAt(seg.alignTime);
        CHECK_NEAR(beat, std::round(beat), 1e-6);          // musical beat
        CHECK(seg.alignTime >= now + 0.09);                 // never in the past
        CHECK(seg.alignTime <= now + 4.0 * 60.0 / 110.0 + 0.2);   // within one bar
        CHECK(seg.startTime >= now - 1e-9);                 // the dancer does not start earlier than asked
        // entry is consistent with the bar position
        const int inBar = director.beatClock().beatInBar(beat);
        CHECK(static_cast<int>(std::lround(seg.entryClipBeat)) % 4 == inBar);
    }
}

TEST_CASE(dance.auto_mode_is_stable_and_explainable) {
    AnalysisResult a;
    a.info.duration = 200.0;
    a.bpm = 120.0;
    a.bpmConfidence = 0.9f;
    a.beatPhaseConfidence = 0.9f;
    a.tempoStable = true;
    a.beatsPerBar = 4;
    for (int i = 0; i < 400; ++i) {
        Beat b;
        b.time = 0.5 + i * 0.5;
        b.beatInBar = i % 4;
        b.barIndex = i / 4;
        b.downbeat = b.beatInBar == 0;
        a.beats.push_back(b);
    }
    // calm intro, active middle, intense drop, calm outro, silence at the end
    const auto section = [&](double s, double e, float energy) {
        Section sec;
        sec.start = s;
        sec.end = e;
        sec.energy = energy;
        sec.confidence = 0.8f;
        a.sections.push_back(sec);
    };
    section(0, 40, 0.15f);
    section(40, 100, 0.5f);
    section(100, 150, 0.9f);
    section(150, 190, 0.2f);
    SilenceRegion silence;
    silence.start = 190.0;
    silence.end = 200.0;
    silence.confidence = 1.0f;
    a.silences.push_back(silence);
    for (double t = 0.5; t < 190.0; t += 0.25) {
        Onset o;
        o.time = t;
        o.strength = t < 40 ? 0.2f : (t < 100 ? 0.5f : (t < 150 ? 0.95f : 0.25f));
        a.onsets.push_back(o);
    }
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setMode(motion::DanceMode::Auto, 0.0);
    player.setAnalysis(&a);
    const auto& segments = player.director().schedule().segments();
    CHECK(segments.size() >= 4);
    CHECK(segments.size() <= 12);   // a style per phrase, not per beat
    for (std::size_t i = 2; i < segments.size(); ++i) {
        CHECK(segments[i].startTime - segments[i - 1].startTime >= 7.0);   // >= ~16 beats
        CHECK(!segments[i].reason.empty());
    }
    CHECK(player.update(20.0).style == motion::DanceStyle::Groove);
    const auto active = player.update(70.0).style;
    CHECK(active == motion::DanceStyle::HipHop || active == motion::DanceStyle::SideStep);
    CHECK(player.update(125.0).style == motion::DanceStyle::Energetic);
    CHECK(player.update(170.0).style == motion::DanceStyle::Groove);
    CHECK(player.update(198.0).style == motion::DanceStyle::Idle);

    // low confidence -> only stable Groove
    a.bpmConfidence = 0.1f;
    a.beatPhaseConfidence = 0.1f;
    motion::DancePlayer shaky;
    shaky.loadLibrary(dancesDir());
    shaky.setMode(motion::DanceMode::Auto, 0.0);
    shaky.setAnalysis(&a);
    for (const auto& seg : shaky.director().schedule().segments()) {
        CHECK(seg.clip->style == motion::DanceStyle::Groove || seg.clip->style == motion::DanceStyle::Idle);
    }
}

TEST_CASE(dance.tempo_change_does_not_jump_the_pose) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setGrid(motion::BeatClock::constant(100.0, 0.0), 0.0);
    player.select(motion::DanceStyle::SideStep, 0.0, true);
    const motion::DanceFrame before = player.update(9.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 9.0);
    const motion::DanceFrame after = player.update(9.0);
    double worst = 0.0;
    for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
        worst = std::max(worst, qa::angleBetween(before.pose.bone[b], after.pose.bone[b]));
    }
    CHECK(worst < 0.1);
    CHECK_NEAR(before.clipBeat, after.clipBeat, 1e-6);
}

// Largest travel of a foot the viewer sees as planted (blended contact >= 0.999) while the
// crossfade runs, including the window in which a planted foot waits for the incoming
// swing. Each planted run is anchored at its first sample. Returns metres.
namespace {
struct PlantedTravel {
    double worstM = 0.0;
    double worstT = 0.0;
    int worstFoot = 0;
};

PlantedTravel plantedTravelAcrossFade(motion::DanceDirector& director, double heightMetres) {
    PlantedTravel out;
    const auto& segs = director.schedule().segments();
    if (segs.size() < 2) return out;
    const motion::ScheduleSegment& incoming = segs.back();
    const double t0 = incoming.startTime;
    const double t1 = incoming.startTime + incoming.fadeSeconds + 2.0;
    const double dt = 1.0 / 240.0;
    for (int f = 0; f < 2; ++f) {
        bool inRun = false;
        math::Vec3 anchor{};
        for (double t = t0; t <= t1 + 1e-9; t += dt) {
            const motion::ClipPose::Foot foot = director.sample(t).pose.foot[static_cast<std::size_t>(f)];
            if (foot.contact < 0.999f) {
                inRun = false;
                continue;
            }
            if (!inRun) {
                inRun = true;
                anchor = foot.position;
                continue;
            }
            const double travel = (foot.position - anchor).length() * heightMetres;
            if (travel > out.worstM) {
                out.worstM = travel;
                out.worstT = t;
                out.worstFoot = f;
            }
        }
    }
    return out;
}
} // namespace

TEST_CASE(dance.planted_feet_hold_during_fades) {
    const motion::DanceStyle styles[] = {motion::DanceStyle::Idle, motion::DanceStyle::Groove,
                                         motion::DanceStyle::HipHop, motion::DanceStyle::SideStep,
                                         motion::DanceStyle::Energetic};
    constexpr double kHeight = 1.7;
    double worst = 0.0;
    for (double bpm : {80.0, 123.0, 180.0}) {
        for (motion::DanceStyle from : styles) {
            for (motion::DanceStyle to : styles) {
                if (from == to) continue;
                motion::DanceDirector director(&library());
                director.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
                director.reset(0.0);
                director.select(from, 0.0, false, true);
                const double switchAt = 60.0 / bpm * 9.37;
                director.select(to, switchAt);
                const PlantedTravel travel = plantedTravelAcrossFade(director, kHeight);
                CHECK(travel.worstM < 0.005);   // under 5 mm of slide per planted run
                worst = std::max(worst, travel.worstM);
            }
        }
    }
    CHECK(worst < 0.005);
}

namespace {

double worstBoneAngle(const motion::ClipPose& a, const motion::ClipPose& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        worst = std::max(worst, qa::angleBetween(a.bone[i], b.bone[i]));
    }
    return worst;
}

// A player that plays `clipId` from time zero without any jump: the reference for
// "where the dance should be" at a given media time.
void steadyPlayer(motion::DancePlayer& player, const std::string& clipId) {
    player.loadLibrary(dancesDir());
    player.setGrid(motion::BeatClock::constant(100.0, 0.0), 0.0);
    if (!player.selectClip(clipId, 0.0, true)) {
        throw std::runtime_error("clip not found in the library: " + clipId);
    }
}

} // namespace

TEST_CASE(dance.seek_keeps_a_generated_clip) {
    // Generated Custom clips share one style with the other showcase clips. A seek must
    // keep playing the selected clip, not the first clip of that style.
    motion::DancePlayer player;
    steadyPlayer(player, "streetCurrent");
    for (double t : {50.0, 3.0, 120.0, 7.5, 0.25}) {
        CHECK(player.update(t).clipId == "streetCurrent");
    }
    CHECK(player.selectedStyle() == motion::DanceStyle::Custom);
}

TEST_CASE(dance.hitch_does_not_move_the_pose) {
    // A stall of more than half a second is a jump of the host clock, not a new pose.
    motion::DancePlayer player;
    steadyPlayer(player, "groove");
    const motion::DanceFrame before = player.update(10.0);
    const motion::DanceFrame after = player.update(10.6);
    CHECK(worstBoneAngle(before.pose, after.pose) < 1e-3);
    // ...and playback picks the schedule up again: after the blend it is the pure pose.
    motion::DancePlayer reference;
    steadyPlayer(reference, "groove");
    const motion::DanceFrame settled = player.update(10.6 + 0.25);
    CHECK(worstBoneAngle(settled.pose, reference.update(10.85).pose) < 1e-3);
    CHECK(std::abs(settled.pose.hips.y - reference.update(10.85).pose.hips.y) < 1e-4);
}

TEST_CASE(dance.seek_never_pops) {
    motion::DancePlayer player;
    steadyPlayer(player, "hiphop");
    motion::DancePlayer referenceAt60;
    steadyPlayer(referenceAt60, "hiphop");
    const motion::DanceFrame before = player.update(10.0);
    // seek far away: the first frame after the seek is still the pose the viewer had
    const motion::DanceFrame jumped = player.update(60.0);
    CHECK(worstBoneAngle(before.pose, jumped.pose) < 1e-3);
    // the blend moves on with media time and lands on the schedule
    const motion::DanceFrame mid = player.update(60.09);
    CHECK(worstBoneAngle(mid.pose, jumped.pose) > 1e-4);
    CHECK(worstBoneAngle(mid.pose, referenceAt60.update(60.09).pose) > 1e-4);
    motion::DancePlayer reference;
    steadyPlayer(reference, "hiphop");
    const motion::DanceFrame settled = player.update(60.3);
    CHECK(worstBoneAngle(settled.pose, reference.update(60.3).pose) < 1e-3);
}

TEST_CASE(dance.seek_is_deterministic_and_pure_after_the_blend) {
    // Two players reach the same media time by different paths: once the blend is over,
    // the pose depends on the time only.
    motion::DancePlayer direct;
    steadyPlayer(direct, "sidestep");
    motion::DancePlayer wandering;
    steadyPlayer(wandering, "sidestep");
    (void)wandering.update(40.0);
    (void)wandering.update(3.0);
    (void)wandering.update(90.0);
    (void)direct.update(90.0);
    const motion::DanceFrame a = direct.update(90.5);
    const motion::DanceFrame b = wandering.update(90.5);
    CHECK(worstBoneAngle(a.pose, b.pose) < 1e-3);
}

TEST_CASE(dance.plan_and_grid_changes_fade_from_the_current_pose) {
    // Manual -> Auto with no music keeps the Manual dance; a tempo change in Auto must
    // still start from the pose the viewer had, not jump to the new grid.
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setGrid(motion::BeatClock::constant(100.0, 0.0), 0.0);
    player.select(motion::DanceStyle::SideStep, 0.0, true);
    const motion::DanceFrame before = player.update(9.0);
    player.setMode(motion::DanceMode::Auto, 9.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 9.0);
    const motion::DanceFrame after = player.update(9.0);
    CHECK(worstBoneAngle(before.pose, after.pose) < 1e-3);
    const motion::DanceFrame next = player.update(9.02);
    CHECK(worstBoneAngle(after.pose, next.pose) < 15.0);   // only natural motion in 20 ms
}

TEST_CASE(dance.rebase_keeps_crossfade_and_pending_one_shot_return) {
    // A grid change in the middle of a crossfade must keep the fade running from the same
    // pose, not collapse it to the incoming clip. A one-shot's return to idle is queued
    // after the clip, so a grid change must not drop it.
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(100.0, 0.0));
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    director.select(motion::DanceStyle::HipHop, 2.0);
    double mid = 2.0;
    for (double t = 2.0; t < 4.0; t += 1.0 / 240.0) {
        if (director.sample(t).transitionProgress >= 0.5f) {
            mid = t;
            break;
        }
    }
    const motion::DanceFrame before = director.sample(mid);
    CHECK(before.transitionProgress > 0.0f && before.transitionProgress < 1.0f);
    director.rebase(mid, motion::BeatClock::constant(140.0, 0.0));
    const motion::DanceFrame after = director.sample(mid);
    CHECK(after.transitionProgress > 0.0f && after.transitionProgress < 1.0f);
    // Re-anchoring the clip beat at the new tempo rounds in float: ~0.01 deg on the
    // fastest clips, far below a visible pop.
    CHECK(worstBoneAngle(before.pose, after.pose) < 0.05);

    motion::DanceDirector oneShot(&library());
    oneShot.setBeatClock(motion::BeatClock::constant(100.0, 0.0));
    oneShot.select(motion::DanceStyle::Energetic, 0.0, true, true);
    oneShot.rebase(1.0, motion::BeatClock::constant(120.0, 0.0));
    CHECK(oneShot.sample(60.0).style == motion::DanceStyle::Idle);
}

TEST_CASE(dance.paused_seek_reaches_the_correct_pose) {
    // The host is paused: media time stands still after the seek. The seek hand-over must
    // still finish on the schedule's pose instead of holding the pose from before the seek.
    motion::DancePlayer player;
    steadyPlayer(player, "hiphop");
    const motion::DanceFrame before = player.update(10.0);
    motion::DanceFrame settled;
    for (int i = 0; i < 100; ++i) {
        settled = player.update(60.0);
    }
    motion::DancePlayer reference;
    steadyPlayer(reference, "hiphop");
    CHECK(worstBoneAngle(settled.pose, reference.update(60.0).pose) < 1e-3);
    CHECK(std::abs(settled.pose.hips.y - reference.update(60.0).pose.hips.y) < 1e-4);
    // ...and the seek itself did not pop: the first frame after it still shows the old pose.
    motion::DancePlayer again;
    steadyPlayer(again, "hiphop");
    (void)again.update(10.0);
    CHECK(worstBoneAngle(again.update(60.0).pose, before.pose) < 1e-3);
}

TEST_CASE(dance.continuous_backward_scrub_follows_the_scrub_position) {
    // A scrub moves the media time backwards a little every frame. Each frame is a jump of the
    // host clock, but the pose must follow the scrub position, not stay at the pose where the
    // scrub started. Overlays are off so the schedule can be compared with a pure reference.
    motion::DancePlayer player;
    player.setOverlayMode(motion::OverlayMode::Off);
    steadyPlayer(player, "hiphop");
    for (double t = 0.0; t <= 60.0 + 1e-9; t += 1.0 / 60.0) {
        (void)player.update(t);
    }
    const motion::DanceFrame atStart = player.update(60.0);
    motion::DancePlayer reference;
    reference.setOverlayMode(motion::OverlayMode::Off);
    steadyPlayer(reference, "hiphop");

    double t = 60.0;
    double worstAfterWindow = 0.0;
    double farthestFromStart = 0.0;
    for (int frame = 1; frame <= 60; ++frame) {   // one second of scrubbing at 60 fps
        t -= 0.05;
        const motion::DanceFrame scrubbed = player.update(t);
        const motion::ClipPose target = reference.director().sample(t).pose;
        if (frame * 0.05 > 0.5) {   // the hand-over itself lasts a fraction of a second
            worstAfterWindow = std::max(worstAfterWindow, worstBoneAngle(scrubbed.pose, target));
        }
        farthestFromStart = std::max(farthestFromStart, worstBoneAngle(scrubbed.pose, atStart.pose));
    }
    CHECK(worstAfterWindow < 1e-3);
    CHECK(farthestFromStart > 5.0);   // it moved: the pose is not frozen at the scrub's start
}

TEST_CASE(dance.paused_tempo_change_does_not_stick) {
    // Auto without music keeps the Manual schedule, but a constant grid change moves the clip
    // beat at the same media time. While the host is paused the first frame holds the pose the
    // viewer had, and the next frame is the schedule on the new grid, not a blend stuck on the
    // old pose.
    motion::DancePlayer player;
    player.setOverlayMode(motion::OverlayMode::Off);
    steadyPlayer(player, "sidestep");
    const motion::DanceFrame before = player.update(9.0);
    player.setMode(motion::DanceMode::Auto, 9.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 9.0);
    const motion::DanceFrame held = player.update(9.0);
    CHECK(worstBoneAngle(held.pose, before.pose) < 1e-3);
    const motion::DanceFrame settled = player.update(9.0);
    CHECK(worstBoneAngle(settled.pose, player.director().sample(9.0).pose) < 1e-3);
    CHECK(worstBoneAngle(settled.pose, before.pose) > 1e-3);   // the tempo change is visible
}

TEST_CASE(dance.rebase_keeps_foot_targets_across_handover) {
    // A grid change while a foot is in its hand-over, or in the window the hand-over still
    // needs after the fade, must not move the feet. The swing is kept in media time, so a
    // tempo change moves nothing at the moment of the change.
    const motion::DanceStyle styles[] = {motion::DanceStyle::Idle, motion::DanceStyle::Groove,
                                         motion::DanceStyle::HipHop, motion::DanceStyle::SideStep,
                                         motion::DanceStyle::Energetic};
    constexpr double kMetresPerHeight = 1.7;
    double worstSame = 0.0;
    double worstChanged = 0.0;
    for (motion::DanceStyle from : styles) {
        for (motion::DanceStyle to : styles) {
            if (from == to) continue;
            for (double u : {0.3, 0.7, 1.2}) {
                for (double bpmAfter : {100.0, 112.0}) {
                    motion::DanceDirector director(&library());
                    director.setBeatClock(motion::BeatClock::constant(100.0, 0.0));
                    director.select(from, 0.0, false, true);
                    director.select(to, 5.37);
                    const motion::ScheduleSegment& incoming = director.schedule().segments().back();
                    const double now = incoming.startTime + u * incoming.fadeSeconds;
                    const motion::ClipPose before = director.sample(now).pose;
                    director.rebase(now, motion::BeatClock::constant(bpmAfter, 0.0));
                    const motion::ClipPose after = director.sample(now).pose;
                    double jump = 0.0;
                    for (std::size_t f = 0; f < 2; ++f) {
                        const double metres =
                            (before.foot[f].position - after.foot[f].position).length() *
                            kMetresPerHeight;
                        jump = std::max(jump, metres);
                    }
                    if (bpmAfter == 100.0) {
                        worstSame = std::max(worstSame, jump);
                    } else {
                        worstChanged = std::max(worstChanged, jump);
                    }
                }
            }
        }
    }
    CHECK(worstSame < 0.001);      // unchanged tempo: under 1 mm
    CHECK(worstChanged < 0.01);    // 100 -> 112 BPM: under 1 cm
}

TEST_CASE(dance.frame_reports_the_idle_share_through_a_crossfade) {
    // The desktop alive layer uses it to keep breathing from stacking on Idle's own.
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(100.0, 0.0));
    director.select(motion::DanceStyle::Idle, 0.0, false, true);
    CHECK(director.sample(1.0).idleWeight == 1.0f);
    director.select(motion::DanceStyle::Groove, 2.0);
    float previous = 1.0f;
    bool sawFade = false;
    for (double t = 2.0; t < 12.0; t += 1.0 / 120.0) {
        const motion::DanceFrame frame = director.sample(t);
        CHECK(frame.idleWeight >= 0.0f && frame.idleWeight <= 1.0f);
        CHECK(frame.idleWeight <= previous + 1.0e-6f);
        if (frame.transitionProgress >= 0.0f) {
            sawFade = true;
            CHECK_NEAR(frame.idleWeight, 1.0 - frame.transitionProgress, 1.0e-6);
        }
        previous = frame.idleWeight;
    }
    CHECK(sawFade);
    CHECK(director.sample(12.0).idleWeight == 0.0f);
}

TEST_CASE(dance.plan_blend_keeps_planted_feet_in_place) {
    // A tempo change re-maps the schedule: the body blends over to the new grid, but a
    // foot that stays planted must not slide there with it.
    motion::DancePlayer player;
    steadyPlayer(player, "hiphop");
    player.setOverlayMode(motion::OverlayMode::Off);
    for (double t = 0.0; t <= 9.0; t += 1.0 / 120.0) {
        (void)player.update(t);
    }
    player.setMode(motion::DanceMode::Auto, 9.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 9.0);
    motion::DanceFrame previous = player.update(9.0);
    double worstSlide = 0.0;
    for (double t = 9.0 + 1.0 / 240.0; t <= 10.0; t += 1.0 / 240.0) {
        const motion::DanceFrame frame = player.update(t);
        for (std::size_t f = 0; f < 2; ++f) {
            if (previous.pose.foot[f].contact >= 0.999f && frame.pose.foot[f].contact >= 0.999f) {
                const motion::Vec3 d = frame.pose.foot[f].position - previous.pose.foot[f].position;
                worstSlide = std::max(worstSlide,
                                      static_cast<double>(std::sqrt(d.x * d.x + d.z * d.z)));
            }
        }
        previous = frame;
    }
    // Per 240 Hz frame, a planted foot creeps by at most the slow fallback, never the
    // 0.5 s body blend (which moved it ~33 cm, ~1 cm per frame).
    CHECK(worstSlide < 0.0015);
}

TEST_CASE(dance.plan_blend_never_pushes_a_landed_foot_below_the_floor) {
    // The carried anchor is horizontal: a foot planted before the change but airborne in
    // the new schedule must land on the floor, not keep a negative height offset.
    motion::DancePlayer player;
    steadyPlayer(player, "hiphop");
    player.setOverlayMode(motion::OverlayMode::Off);
    for (double t = 0.0; t <= 2.0; t += 1.0 / 120.0) {
        (void)player.update(t);
    }
    player.setMode(motion::DanceMode::Auto, 2.0);
    player.setGrid(motion::BeatClock::constant(140.0, 0.0), 2.0);
    double lowest = 0.0;
    for (double t = 2.0; t <= 4.0; t += 1.0 / 240.0) {
        const motion::DanceFrame frame = player.update(t);
        for (std::size_t f = 0; f < 2; ++f) {
            lowest = std::min(lowest, static_cast<double>(frame.pose.foot[f].position.y));
        }
    }
    // HipHop itself dips to about -0.0025 (heel roll); the old carry reached -0.061.
    CHECK(lowest > -0.005);
}

TEST_CASE(dance.rebased_handover_follows_the_swing_on_the_new_tempo) {
    // A tempo rebase during a hand-over changes when the clip's swing happens. The hand-over
    // follows the swing's clip beats, so a planted foot is not dragged to its new place while
    // the clip still holds it down (HipHop 100 BPM -> Energetic, then 80 BPM mid-fade), and the
    // hand-over lasts until the rebased swing has landed (Idle 180 -> Energetic, then 100). The
    // planted slide after the rebase is compared with the same transition without a tempo
    // change, so slides a pair has anyway (no swing in the window) do not count.
    const motion::DanceStyle styles[] = {motion::DanceStyle::Idle, motion::DanceStyle::Groove,
                                         motion::DanceStyle::HipHop, motion::DanceStyle::SideStep,
                                         motion::DanceStyle::Energetic};
    constexpr double kMetresPerHeight = 1.7;
    constexpr double kStep = 1.0 / 120.0;
    const auto plantedSlide = [&](motion::DanceStyle from, motion::DanceStyle to, double u,
                                  double bpmBefore, double bpmAfter) {
        motion::DanceDirector director(&library());
        director.setBeatClock(motion::BeatClock::constant(bpmBefore, 0.0));
        director.select(from, 0.0, false, true);
        director.select(to, 5.37);
        const motion::ScheduleSegment& incoming = director.schedule().segments().back();
        const double now = incoming.startTime + u * incoming.fadeSeconds;
        director.rebase(now, motion::BeatClock::constant(bpmAfter, 0.0));
        // Horizontal travel of a foot over a run of frames where it is planted.
        motion::ClipPose previous = director.sample(now).pose;
        std::array<double, 2> run{0.0, 0.0};
        double worst = 0.0;
        // Long enough for a swing a slower tempo pushed past the original search window.
        for (double t = now + kStep; t < now + 10.0; t += kStep) {
            const motion::ClipPose pose = director.sample(t).pose;
            for (std::size_t f = 0; f < 2; ++f) {
                if (previous.foot[f].contact <= 0.95f || pose.foot[f].contact <= 0.95f) {
                    run[f] = 0.0;
                    continue;
                }
                const motion::Vec3 d = pose.foot[f].position - previous.foot[f].position;
                run[f] += std::sqrt(d.x * d.x + d.z * d.z) * kMetresPerHeight;
                worst = std::max(worst, run[f]);
            }
            previous = pose;
        }
        return worst;
    };
    double worstExcess = 0.0;
    for (motion::DanceStyle from : styles) {
        for (motion::DanceStyle to : styles) {
            if (from == to) continue;
            // u < 0 rebases before the queued transition starts; 140 BPM moves its entry
            // into the past.
            for (double u : {-1.0, 0.01, 0.2, 0.5, 0.8}) {
                const double steady = plantedSlide(from, to, u, 100.0, 100.0);
                for (double bpmAfter : {80.0, 125.0, 140.0}) {
                    worstExcess =
                        std::max(worstExcess, plantedSlide(from, to, u, 100.0, bpmAfter) - steady);
                }
                // 180 -> 100 BPM moves a swing past the window the transition was planned
                // with: the hand-over must last until that swing lands.
                const double fast = plantedSlide(from, to, u, 180.0, 180.0);
                worstExcess =
                    std::max(worstExcess, plantedSlide(from, to, u, 180.0, 100.0) - fast);
            }
        }
    }
    // A rebase adds under 3 cm of planted slide. What remains is the settling tail of a
    // landing (shown contact 0.95..1 while the hand-over finishes); the bugs this guards
    // against slid a planted foot by 38 to 48 cm.
    CHECK(worstExcess < 0.03);
}

TEST_CASE(dance.rebased_one_shot_returns_after_its_remaining_beats) {
    // The rebased one-shot keeps its clip beat and plays the rest at the new rate; its Idle
    // return must follow that end, not the old absolute beat (Energetic 100 -> 180 BPM at 5 s
    // cut the clip off at beat 8.3 of 16).
    for (double bpmAfter : {80.0, 140.0, 180.0}) {
        motion::DanceDirector director(&library());
        director.setBeatClock(motion::BeatClock::constant(100.0, 0.0));
        director.select(motion::DanceStyle::Energetic, 0.0, true, true);
        director.rebase(5.0, motion::BeatClock::constant(bpmAfter, 0.0));
        const auto& segments = director.schedule().segments();
        CHECK(segments.size() == 2);
        if (segments.size() != 2) continue;
        const motion::ScheduleSegment& shot = segments[0];
        const motion::ScheduleSegment& back = segments[1];
        const motion::BeatClock grid = motion::BeatClock::constant(bpmAfter, 0.0);
        // The clip's last beat is reached at the return's alignment, within a sample.
        const double endBeat =
            motion::DanceSchedule::segmentClipBeat(shot, back.alignTime, grid);
        CHECK_NEAR(endBeat, shot.clip->beats, 1e-6);
        // Until the return's fade starts, the one-shot is what plays.
        CHECK(director.sample(back.startTime - 0.01).style == motion::DanceStyle::Energetic);
        CHECK(director.sample(back.startTime + back.fadeSeconds + 0.5).style ==
              motion::DanceStyle::Idle);
    }
}

// ---------------------------------------------------------------------------
// BG Motion 3.4 variety: mirrors, per-loop amplitude and overlays, loop seams
// ---------------------------------------------------------------------------

namespace {

// Largest per-bone rotation difference of two poses, in degrees.
double maxBoneDeg(const motion::ClipPose& a, const motion::ClipPose& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        worst = std::max(worst, qa::angleBetween(a.bone[i], b.bone[i]));
    }
    return worst;
}

// Largest component difference of two poses (bones and hips). Exact comparison for purity:
// angleBetween has a rounding floor of about 1e-6 degrees even for identical rotations.
double maxComponentDiff(const motion::ClipPose& a, const motion::ClipPose& b) {
    double worst = 0.0;
    const auto take = [&](const math::Quat& p, const math::Quat& q) {
        worst = std::max({worst, std::abs(double(p.x) - q.x), std::abs(double(p.y) - q.y),
                          std::abs(double(p.z) - q.z), std::abs(double(p.w) - q.w)});
    };
    for (std::size_t i = 0; i < vrm::kHumanoidBoneCount; ++i) {
        take(a.bone[i], b.bone[i]);
    }
    worst = std::max({worst, std::abs(double(a.hips.x) - b.hips.x), std::abs(double(a.hips.y) - b.hips.y),
                      std::abs(double(a.hips.z) - b.hips.z)});
    return worst;
}

// Mirror image of a rotation, written out here independently of the implementation under test.
math::Quat reflectQuat(const math::Quat& q) {
    return {q.x, -q.y, -q.z, q.w};
}

math::Vec3 reflectAcross(const math::Vec3& v, const math::Vec3& normal) {
    return v - normal * (2.0f * math::dot(v, normal));
}

bool sameCues(const std::vector<motion::PrimitiveCue>& a, const std::vector<motion::PrimitiveCue>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].side != b[i].side) return false;
        if (std::abs(a[i].beat - b[i].beat) > 1e-6f || std::abs(a[i].intensity - b[i].intensity) > 1e-6f ||
            std::abs(a[i].durationBeats - b[i].durationBeats) > 1e-6f) {
            return false;
        }
    }
    return true;
}

// A 200 s, 120 BPM track with calm, active, intense, calm and silent phrases: the same plan
// as dance.auto_mode_is_stable_and_explainable, where a style comes back after another one.
AnalysisResult phrasedTrack() {
    AnalysisResult a;
    a.info.duration = 200.0;
    a.bpm = 120.0;
    a.bpmConfidence = 0.9f;
    a.beatPhaseConfidence = 0.9f;
    a.tempoStable = true;
    a.beatsPerBar = 4;
    for (int i = 0; i < 400; ++i) {
        Beat b;
        b.time = 0.5 + i * 0.5;
        b.beatInBar = i % 4;
        b.barIndex = i / 4;
        b.downbeat = b.beatInBar == 0;
        a.beats.push_back(b);
    }
    const auto section = [&](double s, double e, float energy) {
        Section sec;
        sec.start = s;
        sec.end = e;
        sec.energy = energy;
        sec.confidence = 0.8f;
        a.sections.push_back(sec);
    };
    section(0, 40, 0.15f);
    section(40, 100, 0.5f);
    section(100, 150, 0.9f);
    section(150, 190, 0.2f);
    SilenceRegion silence;
    silence.start = 190.0;
    silence.end = 200.0;
    silence.confidence = 1.0f;
    a.silences.push_back(silence);
    for (double t = 0.5; t < 190.0; t += 0.25) {
        Onset o;
        o.time = t;
        o.strength = t < 40 ? 0.2f : (t < 100 ? 0.5f : (t < 150 ? 0.95f : 0.25f));
        a.onsets.push_back(o);
    }
    return a;
}

} // namespace

TEST_CASE(dance.mirror_clip_is_an_involution_and_swaps_sides) {
    for (const motion::HumanoidClip& clip : library().clips()) {
        const motion::HumanoidClip mirror = motion::mirrorClip(clip);
        CHECK(mirror.id == clip.id + "~m");
        const motion::HumanoidClip twice = motion::mirrorClip(mirror);
        CHECK(twice.id == clip.id);
        CHECK(twice.name == clip.name);

        std::vector<motion::ClipDiagnostic> issues;
        motion::validateClip(mirror, mirror.id, issues);
        for (const motion::ClipDiagnostic& d : issues) {
            CHECK(d.severity != motion::ClipDiagnostic::Severity::Error);
        }

        // Mirroring twice gives back every key of every track.
        for (std::size_t b = 0; b < vrm::kHumanoidBoneCount; ++b) {
            CHECK(twice.bones[b].size() == clip.bones[b].size());
            for (std::size_t k = 0; k < std::min(twice.bones[b].size(), clip.bones[b].size()); ++k) {
                CHECK(qa::angleBetween(twice.bones[b][k], clip.bones[b][k]) < 1e-3);
            }
        }
        CHECK(twice.hips.size() == clip.hips.size());
        for (std::size_t k = 0; k < std::min(twice.hips.size(), clip.hips.size()); ++k) {
            CHECK_NEAR(twice.hips[k].x, clip.hips[k].x, 1e-6);
            CHECK_NEAR(twice.root[k].z, clip.root[k].z, 1e-6);
        }

        // The mirror's left side is the reflected right side of the original, at any beat.
        for (double fraction : {0.0, 0.13, 0.37, 0.5, 0.71, 0.94}) {
            const double beat = fraction * clip.beats;
            const motion::ClipPose orig = motion::sampleClip(clip, beat);
            const motion::ClipPose mir = motion::sampleClip(mirror, beat);
            for (vrm::HumanoidBone left : {vrm::HumanoidBone::LeftShoulder, vrm::HumanoidBone::LeftUpperArm,
                                           vrm::HumanoidBone::LeftLowerArm, vrm::HumanoidBone::LeftHand,
                                           vrm::HumanoidBone::LeftUpperLeg, vrm::HumanoidBone::LeftFoot}) {
                const auto l = static_cast<std::size_t>(left);
                const auto r = static_cast<std::size_t>(motion::mirroredBone(left));
                CHECK(qa::angleBetween(mir.bone[l], reflectQuat(orig.bone[r])) < 0.01);
                CHECK(qa::angleBetween(mir.bone[r], reflectQuat(orig.bone[l])) < 0.01);
            }
            for (std::size_t b : {static_cast<std::size_t>(vrm::HumanoidBone::Spine),
                                  static_cast<std::size_t>(vrm::HumanoidBone::Head)}) {
                CHECK(qa::angleBetween(mir.bone[b], reflectQuat(orig.bone[b])) < 0.01);
            }
            CHECK_NEAR(mir.rootX, -orig.rootX, 1e-5);
            CHECK_NEAR(mir.rootZ, orig.rootZ, 1e-5);
            CHECK_NEAR(mir.rootYaw, -orig.rootYaw, 1e-5);
            CHECK_NEAR(mir.hips.x, -orig.hips.x, 1e-5);
            CHECK_NEAR(mir.hips.y, orig.hips.y, 1e-5);
            for (std::size_t f = 0; f < 2; ++f) {
                const std::size_t other = 1 - f;
                CHECK_NEAR(mir.foot[f].position.x, -orig.foot[other].position.x, 1e-5);
                CHECK_NEAR(mir.foot[f].position.y, orig.foot[other].position.y, 1e-5);
                CHECK_NEAR(mir.foot[f].position.z, orig.foot[other].position.z, 1e-5);
                CHECK_NEAR(mir.foot[f].yaw, -orig.foot[other].yaw, 1e-5);
                CHECK_NEAR(mir.foot[f].contact, orig.foot[other].contact, 1e-5);
                CHECK_NEAR(mir.hand[f], orig.hand[1 - f], 1e-5);
            }
        }
    }
}

TEST_CASE(dance.mirrored_clip_retargets_as_a_mirror_image) {
    const motion::HumanoidClip& groove = *library().find("groove");
    const motion::HumanoidClip mirror = motion::mirrorClip(groove);
    for (const std::filesystem::path& path : {minimalVrm(), model1Path()}) {
        if (!std::filesystem::exists(path)) continue;
        Rig rig = makeRig(path);
        CHECK(rig.ok());
        const vrm::VrmModel& model = *rig.loaded.model;
        const math::Vec3 normal = model.right * (1.0f / model.right.length());
        for (double fraction : {0.1, 0.37, 0.6, 0.85}) {
            const double beat = fraction * groove.beats;
            const animation::RetargetedPose original =
                rig.rig->applyClip(motion::sampleClip(groove, beat), 0.0);
            const animation::RetargetedPose mirrored =
                rig.rig->applyClip(motion::sampleClip(mirror, beat), 0.0);
            for (auto [left, right] : {std::pair{vrm::HumanoidBone::LeftLowerArm, vrm::HumanoidBone::RightLowerArm},
                                       std::pair{vrm::HumanoidBone::LeftHand, vrm::HumanoidBone::RightHand}}) {
                if (!model.hasBone(left) || !model.hasBone(right)) continue;
                const math::Vec3 leftMoved = mirrored.bonePosition(left, model) - model.boneBindPosition(left);
                const math::Vec3 rightMoved = original.bonePosition(right, model) - model.boneBindPosition(right);
                CHECK((leftMoved - reflectAcross(rightMoved, normal)).length() < 2e-3f * model.height);
            }
        }
    }
}

TEST_CASE(dance.auto_mirrors_a_style_that_comes_back) {
    AnalysisResult a = phrasedTrack();
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setMode(motion::DanceMode::Auto, 0.0);
    player.setAnalysis(&a);
    const auto& segments = player.director().schedule().segments();

    // Every style alternates between its authored clip and the mirror, segment by segment.
    std::map<int, bool> lastMirrored;
    int mirrored = 0;
    for (const motion::ScheduleSegment& seg : segments) {
        const bool isMirror = seg.clip->id.size() > 2 && seg.clip->id.substr(seg.clip->id.size() - 2) == "~m";
        CHECK(player.library().find(seg.clip->id) == seg.clip);  // lifetime-safe library pointer
        CHECK(isMirror == (seg.reason.find("mirrored repeat") != std::string::npos));
        if (seg.clip->style != motion::DanceStyle::Idle) {
            const int style = static_cast<int>(seg.clip->style);
            if (lastMirrored.count(style)) {
                CHECK(lastMirrored[style] != isMirror);
            } else {
                CHECK(!isMirror);
            }
            lastMirrored[style] = isMirror;
        } else {
            CHECK(!isMirror);
        }
        mirrored += isMirror ? 1 : 0;
    }
    CHECK(mirrored >= 1);
    CHECK(segments.size() <= 12);  // the phrase plan keeps its shape

    // The plan is a pure function of the analysis.
    motion::DancePlayer again;
    again.loadLibrary(dancesDir());
    again.setMode(motion::DanceMode::Auto, 0.0);
    again.setAnalysis(&a);
    const auto& other = again.director().schedule().segments();
    CHECK(other.size() == segments.size());
    for (std::size_t i = 0; i < std::min(other.size(), segments.size()); ++i) {
        CHECK(other[i].clip->id == segments[i].clip->id);
        CHECK_NEAR(other[i].startTime, segments[i].startTime, 1e-12);
        CHECK(other[i].reason == segments[i].reason);
    }
}

TEST_CASE(dance.loop_blend_is_periodic_and_continuous_across_seams) {
    const double loopBeats = 16.0;
    const double window = 1.0;  // the last beat before each seam
    const double maxSlope = 1.875 / window;  // derivative of the quintic smootherstep, per beat
    double previous = 0.0;
    for (int i = -3200; i <= 3200; ++i) {
        const double beat = i * 0.01;
        const motion::LoopBlend lb = motion::loopBlendAt(beat, loopBeats, window);
        CHECK(lb.w >= 0.0f && lb.w <= 1.0f);
        const double loopPos = beat / loopBeats;
        const double seam = std::ceil(loopPos);
        const double untilSeam = (seam - loopPos) * loopBeats;
        if (seam >= 1.0 && untilSeam > 0.0 && untilSeam <= window) {
            // Inside the window before a seam: blend from the loop before it to the loop after it.
            CHECK(lb.from == static_cast<std::int64_t>(seam) - 1);
            CHECK(lb.to == static_cast<std::int64_t>(seam));
        } else {
            // Outside the window, and at the seam itself, there is one loop and no blend. The
            // clip origin has no loop before it, so nothing is blended there either.
            CHECK(lb.from == lb.to);
            CHECK(lb.w == 0.0f);
            CHECK(lb.from == std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(loopPos))));
        }
        // The effective loop value (loop index, blended across the window) changes continuously.
        const double effective = static_cast<double>(lb.from) + static_cast<double>(lb.to - lb.from) * lb.w;
        if (i > -3200) {
            CHECK(std::abs(effective - previous) <= 0.01 * maxSlope + 1e-6);
        }
        previous = effective;
        // Periodic: one loop later, the same blend one loop further on (from the clip origin on).
        if (beat >= 0.0) {
            const motion::LoopBlend next = motion::loopBlendAt(beat + loopBeats, loopBeats, window);
            CHECK(next.from == lb.from + 1 && next.to == lb.to + 1);
            CHECK_NEAR(next.w, lb.w, 1e-6);
        }
    }

    // The window ends at the seam: it starts a beat before it, the seam itself is the loop after
    // it, and the blend is half done half a beat before the seam.
    const auto at = [&](double beat) { return motion::loopBlendAt(beat, loopBeats, window); };
    CHECK(at(loopBeats - 1.0).w == 0.0f);
    CHECK(at(loopBeats - 1.0).from == 0 && at(loopBeats - 1.0).to == 1);
    CHECK_NEAR(at(loopBeats - 0.5).w, 0.5, 1e-6);
    CHECK(at(loopBeats - 1e-4).w > 0.999f);
    CHECK(at(loopBeats).from == 1 && at(loopBeats).to == 1 && at(loopBeats).w == 0.0f);
    CHECK(at(loopBeats - 1.0 - 1e-3).from == 0 && at(loopBeats - 1.0 - 1e-3).to == 0);
}

TEST_CASE(dance.first_pass_is_authored_exactly) {
    // Loop 0, the fade-in before beat 0 included, is never blended with a loop before it: the
    // choice is loop 0 with weight 0, so its amplitude is exactly 1 and its cues are its own.
    const double loopBeats = 8.0;
    for (double beat = -20.0; beat <= 0.5 + 1e-9; beat += 0.01) {
        const motion::LoopBlend lb = motion::loopBlendAt(beat, loopBeats, 0.5);
        CHECK(lb.from == 0);
        CHECK(lb.to == 0);
        CHECK(lb.w == 0.0f);
    }

    // The same through the dance: the authored clip plays unchanged at and just after beat 0.
    const motion::HumanoidClip& groove = *library().find("groove");
    const double bpm = groove.bpm;
    motion::DanceDirector director(&library());
    director.setBeatClock(motion::BeatClock::constant(bpm, 0.0));
    director.reset(0.0);
    director.select(motion::DanceStyle::Groove, 0.0, false, true);
    // The frame's own clip beat is used: the beat clock works in float time, so the clip beat is
    // the musical beat only to about 1e-4 beats. The tolerance is far below any amplitude change.
    for (double beat = 0.0; beat <= 0.5 + 1e-9; beat += 1.0 / 64.0) {
        const motion::DanceFrame frame = director.sample(beat * 60.0 / bpm);
        CHECK_NEAR(frame.clipBeat, beat, 1e-3);
        CHECK(maxComponentDiff(frame.pose, motion::sampleClip(groove, frame.clipBeat)) < 1e-4);
    }
    // Sanity: the variation does act once the second loop starts.
    const motion::DanceFrame second = director.sample((groove.beats + 0.25) * 60.0 / bpm);
    CHECK(maxComponentDiff(second.pose, motion::sampleClip(groove, second.clipBeat)) > 1e-3);
}

TEST_CASE(dance.loop_variation_is_bounded_deterministic_and_seamless) {
    const motion::HumanoidClip& groove = *library().find("groove");
    const double bpm = groove.bpm;  // rate 1: the clip beat is the music beat
    const auto makeDirector = [&] {
        auto director = std::make_unique<motion::DanceDirector>(&library());
        director->setBeatClock(motion::BeatClock::constant(bpm, 0.0));
        director->reset(0.0);
        director->select(motion::DanceStyle::Groove, 0.0, false, true);
        return director;
    };
    const auto d = makeDirector();
    const auto poseAt = [&](const motion::DanceDirector& dir, double beat) {
        return dir.sample(beat * 60.0 / bpm).pose;
    };
    CHECK_NEAR(d->sample(4.0 * 60.0 / bpm).clipBeat, 4.0, 1e-6);

    // Two loops at the same phase differ, but only a little: the amplitude is within 8 %.
    double worstDifference = 0.0;
    for (double beat : {1.0, 2.5, 4.0, 6.0, 9.0, 11.5, 13.0}) {
        for (int k = 0; k < 3; ++k) {
            const double diff = maxBoneDeg(poseAt(*d, beat + 16.0 * k), poseAt(*d, beat + 16.0 * (k + 1)));
            worstDifference = std::max(worstDifference, diff);
            CHECK(diff < 12.0);
        }
    }
    CHECK(worstDifference > 0.3);

    // Pure: the same inputs give the same pose, from a fresh director as well.
    const auto again = makeDirector();
    for (double beat : {0.7, 4.2, 17.3, 33.9}) {
        CHECK(maxComponentDiff(poseAt(*d, beat), poseAt(*again, beat)) == 0.0);
        CHECK(maxComponentDiff(poseAt(*d, beat), poseAt(*d, beat)) == 0.0);
    }

    // Seam: the largest step across the loop boundary is no larger than the steps inside a
    // loop, so the variation never pops.
    const double step = 1.0 / 480.0;
    double interior = 0.0;
    for (double beat = 4.0; beat < 12.0; beat += step) {
        interior = std::max(interior, maxBoneDeg(poseAt(*d, beat), poseAt(*d, beat + step)));
    }
    double seam = 0.0;
    for (double beat = 15.0; beat < 17.0; beat += step) {
        seam = std::max(seam, maxBoneDeg(poseAt(*d, beat), poseAt(*d, beat + step)));
    }
    CHECK(seam <= 1.3 * interior + 0.05);
}

TEST_CASE(dance.library_rejects_mirror_suffix_ids) {
    // "<base>~m" names the mirror of "<base>". A stored clip with that id would be shadowed by
    // the mirror in find(), so the library refuses it, from add(), replace() and file loading.
    motion::DanceLibrary lib;
    lib.loadDirectory(dancesDir());
    const motion::HumanoidClip groove = *lib.find("groove");
    motion::HumanoidClip imposter = groove;
    imposter.id = "groove" + std::string(motion::kMirrorSuffix);
    const std::size_t count = lib.clips().size();
    CHECK(!lib.add(imposter));
    CHECK(!lib.replace(imposter));
    CHECK(lib.clips().size() == count);
    CHECK(!lib.diagnostics().empty());

    // The id still resolves to the mirror of the original clip, the one the library owns.
    const motion::HumanoidClip* mirror = lib.find(imposter.id);
    CHECK(mirror != nullptr);
    if (mirror) {
        CHECK(maxComponentDiff(motion::sampleClip(*mirror, 1.25),
                               motion::sampleClip(motion::mirrorClip(groove), 1.25)) == 0.0);
    }

    // A clip file with such an id is reported and skipped.
    const std::string needle = "\"id\":\"tiny\"";
    std::string text = tinyClipJson();
    text.replace(text.find(needle), needle.size(), "\"id\":\"tiny" + std::string(motion::kMirrorSuffix) + "\"");
    const motion::ClipLoadResult loaded = motion::parseClipJson(text, "tiny~m.json");
    CHECK(loaded.ok);
    motion::DanceLibrary other;
    CHECK(!other.add(loaded.clip));
    CHECK(other.empty());
    CHECK(!other.diagnostics().empty());
}

TEST_CASE(dance.manual_seek_matches_continuous_play_in_later_loops) {
    // A re-aligned Manual dance (a seek before its schedule) has the pose that continuous play has
    // at the same time, in later loops too: the seek does not reset the per-loop variation.
    const motion::HumanoidClip& groove = *library().find("groove");
    const double bpm = groove.bpm;
    const auto makeDirector = [&] {
        auto director = std::make_unique<motion::DanceDirector>(&library());
        director->setBeatClock(motion::BeatClock::constant(bpm, 0.0));
        director->reset(0.0);
        director->select(motion::DanceStyle::Groove, 0.0, false, true);
        return director;
    };
    const auto played = makeDirector();
    for (double beat : {2.0 * groove.beats + 3.25, 5.0 * groove.beats + 0.5, 2.0 * groove.beats + 15.9}) {
        const double t = beat * 60.0 / bpm;
        const auto seeked = makeDirector();
        seeked->reseed(t);
        // A seek fades the musical warp in over its first beat (the continuous dance has it all
        // along), so the poses are compared from one beat after the seek on.
        for (double ahead : {1.0, 1.25, 2.7}) {
            const double when = t + ahead * 60.0 / bpm;
            const motion::DanceFrame expected = played->sample(when);
            const motion::DanceFrame got = seeked->sample(when);
            CHECK_NEAR(got.clipBeat, expected.clipBeat, 1e-3);
            CHECK(maxComponentDiff(got.pose, expected.pose) < 1e-5);
            // The loop is genuinely varied here, so the comparison above is not vacuous.
            CHECK(maxComponentDiff(expected.pose, motion::sampleClip(groove, expected.clipBeat)) > 1e-3);
        }
    }
}

TEST_CASE(dance.manual_seek_keeps_the_grid_bar_offset) {
    // An analysed grid whose first beat is the third of its bar: continuous play enters the
    // clip in phase with the bar, and a seek must land on the same clip beat, so the
    // choreography stays on the musical bars (it was two beats off before).
    AnalysisResult analysis;
    analysis.info.duration = 120.0;
    analysis.bpm = 96.0;
    analysis.tempoStable = true;
    analysis.bpmConfidence = 0.9f;
    analysis.beatPhaseConfidence = 0.9f;
    for (int i = 0; i < 180; ++i) {
        Beat beat;
        beat.time = i * 60.0 / 96.0;
        beat.beatInBar = (i + 2) % 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
    }
    const motion::BeatClock clock = motion::BeatClock::fromAnalysis(analysis);
    CHECK(clock.barOffset() == 2);
    const auto makeDirector = [&] {
        auto director = std::make_unique<motion::DanceDirector>(&library());
        director->setBeatClock(clock);
        director->reset(0.0);
        director->select(motion::DanceStyle::Groove, 0.0, false, true);
        return director;
    };
    const auto played = makeDirector();
    for (double beat : {22.0, 37.5, 61.25}) {
        const double t = clock.timeAtBeat(beat);
        const auto seeked = makeDirector();
        seeked->reseed(t);
        for (double ahead : {1.0, 2.5}) {
            const double when = clock.timeAtBeat(beat + ahead);
            CHECK_NEAR(seeked->sample(when).clipBeat, played->sample(when).clipBeat, 1e-3);
        }
    }
}

TEST_CASE(dance.overlay_cues_change_per_loop_and_are_deterministic) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    for (const char* id : {"groove", "hiphop", "energetic"}) {
        const std::vector<motion::PrimitiveCue> loop0 = player.overlaysFor(id, 0);
        const std::vector<motion::PrimitiveCue> loop1 = player.overlaysFor(id, 1);
        CHECK(!loop0.empty());
        CHECK(sameCues(loop0, player.overlaysFor(id, 0)));
        CHECK(!sameCues(loop0, loop1));

        // A fresh player with the same seed plans the same cues for the same loop.
        motion::DancePlayer other;
        other.loadLibrary(dancesDir());
        CHECK(sameCues(other.overlaysFor(id, 1), loop1));
        // Another host seed plans other cues, also after the first plan was cached.
        other.setOverlaySeed(77u);
        CHECK(!sameCues(other.overlaysFor(id, 1), loop1));

        // Cues that start near the end of a loop spill past the seam. The composition carries
        // those tails into the next loop, so the plans must contain some.
        const motion::HumanoidClip& clip = *player.library().find(id);
        int tails = 0;
        for (std::int64_t loop = 0; loop < 4; ++loop) {
            for (const motion::PrimitiveCue& cue : player.overlaysFor(id, loop)) {
                if (cue.beat + cue.durationBeats > clip.beats) ++tails;
            }
        }
        CHECK(tails > 0);
    }
}

TEST_CASE(dance.full_pipeline_is_seamless_across_loops) {
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    const motion::HumanoidClip& groove = *player.library().find("groove");
    player.setGrid(motion::BeatClock::constant(groove.bpm, 0.0), 0.0);
    player.select(motion::DanceStyle::Groove, 0.0, true);

    // Frame by frame at 240 Hz, with overlays and the per-loop amplitude both active.
    const double dt = 1.0 / 240.0;
    const double beatSeconds = 60.0 / groove.bpm;
    motion::ClipPose previous;
    bool havePrevious = false;
    double interior = 0.0;
    double seam = 0.0;
    for (int i = 0; i <= static_cast<int>(40.0 * beatSeconds / dt); ++i) {
        const double t = i * dt;
        const motion::DanceFrame frame = player.update(t);
        const double beat = t / beatSeconds;
        if (havePrevious) {
            const double step = maxBoneDeg(previous, frame.pose);
            if (beat >= 4.0 && beat < 12.0) interior = std::max(interior, step);
            if (beat >= 15.0 && beat < 17.0) seam = std::max(seam, step);
            if (beat >= 31.0 && beat < 33.0) seam = std::max(seam, step);
        }
        previous = frame.pose;
        havePrevious = true;
    }
    CHECK(interior > 0.0);
    CHECK(seam <= 1.5 * interior + 0.5);
}

TEST_CASE(dance.overlays_are_active_across_the_seams) {
    // The seam checks above are only meaningful if overlays play through the seams: the cue sets
    // change there, and their tails carry over. Frames around both seams must show overlays.
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setOverlayMode(motion::OverlayMode::Full);
    const motion::HumanoidClip& groove = *player.library().find("groove");
    player.setGrid(motion::BeatClock::constant(groove.bpm, 0.0), 0.0);
    player.select(motion::DanceStyle::Groove, 0.0, true);

    const double beatSeconds = 60.0 / groove.bpm;
    int nearSeam = 0;
    int frames = 0;
    for (int i = 0; i <= static_cast<int>(33.0 * beatSeconds * 120.0); ++i) {
        const double t = i / 120.0;
        const motion::DanceFrame frame = player.update(t);
        const double beat = t / beatSeconds;
        if ((beat >= 15.0 && beat < 17.0) || (beat >= 31.0 && beat < 33.0)) {
            ++frames;
            if (frame.overlayActive > 0) ++nearSeam;
        }
    }
    CHECK(frames > 0);
    CHECK(nearSeam > frames / 2);
}

TEST_CASE(dance.loop_cues_carry_tails_and_start_at_full_weight) {
    // The per-loop composition: a cue on the downbeat ramps in from zero and reaches its full
    // weight, a tail that spills past the seam finishes in the next loop, and a cue that ends
    // inside its loop does not come back. Loop sets are composed, never blended.
    const motion::ClipPose base = motion::neutralClipPose();
    const double beats = 8.0;

    motion::PrimitiveCue downbeat;
    downbeat.kind = motion::PrimitiveKind::ArmWave;
    downbeat.beat = 0.0f;
    downbeat.durationBeats = 1.0f;
    downbeat.intensity = 1.0f;

    motion::PrimitiveCue tail = downbeat;  // starts half a beat before the seam, ends 1.5 beats in
    tail.beat = 7.5f;
    tail.durationBeats = 2.0f;

    motion::PrimitiveCue inside = downbeat;  // ends well inside its loop
    inside.beat = 2.0f;

    const std::vector<motion::PrimitiveCue> downbeatSet{downbeat};
    const std::vector<motion::PrimitiveCue> tailSet{tail};
    const std::vector<motion::PrimitiveCue> insideSet{inside};

    // Downbeat: the envelope starts at zero, so the pose is the base on the downbeat, and at the
    // peak it is exactly the single composition of the cue at full weight.
    CHECK(maxComponentDiff(motion::composeLoopCues(base, downbeatSet, {}, 0.0, beats).pose, base) < 1e-9);
    // The fade-in before beat 0 is loop 0 with no cue started yet: the pose is the base, exactly.
    CHECK(maxComponentDiff(motion::composeLoopCues(base, downbeatSet, {}, -0.25, beats).pose, base) == 0.0);
    const motion::ComposedPose peak = motion::composeLoopCues(base, downbeatSet, {}, 0.5, beats);
    const motion::ComposedPose alone = motion::composeMotion(base, downbeatSet, 0.5, beats, false);
    CHECK(maxComponentDiff(peak.pose, alone.pose) == 0.0);
    CHECK(maxComponentDiff(peak.pose, base) > 1e-3);

    // The tail is active in the next loop until it ends; a cue that ends inside its loop is dropped.
    CHECK(maxComponentDiff(motion::composeLoopCues(base, {}, tailSet, 0.25, beats).pose, base) > 1e-3);
    CHECK(maxComponentDiff(motion::composeLoopCues(base, {}, tailSet, 1.6, beats).pose, base) == 0.0);
    CHECK(maxComponentDiff(motion::composeLoopCues(base, {}, insideSet, 0.5, beats).pose, base) == 0.0);

    // Across the seam the tail is continuous: the last instant of the loop equals the first
    // instant of the next one.
    const motion::ComposedPose lastOfLoop = motion::composeMotion(base, tailSet, beats, beats, false);
    const motion::ComposedPose firstOfNext = motion::composeLoopCues(base, {}, tailSet, 0.0, beats);
    CHECK(maxComponentDiff(lastOfLoop.pose, firstOfNext.pose) < 1e-6);
}

TEST_CASE(dance.overlay_pipeline_composes_each_loop_without_blending) {
    // The player's overlays, frame by frame across three loops, against the per-loop formula:
    // loop L's cues at its own beat, the tails of loop L-1 and nothing of loop L+1. A blend of two
    // loops' sets at the seam, or a cue of the next loop leaking in early, would differ.
    motion::DancePlayer off;
    off.loadLibrary(dancesDir());
    off.setOverlayMode(motion::OverlayMode::Off);
    motion::DancePlayer full;
    full.loadLibrary(dancesDir());
    full.setOverlayMode(motion::OverlayMode::Full);
    const motion::HumanoidClip& groove = *full.library().find("groove");
    const double bpm = groove.bpm;
    for (motion::DancePlayer* player : {&off, &full}) {
        player->setGrid(motion::BeatClock::constant(bpm, 0.0), 0.0);
        player->select(motion::DanceStyle::Groove, 0.0, true);
    }

    // Full overlays: every region is allowed, as the composition defaults are.
    const motion::CompositionOptions options;
    double worst = 0.0;
    int overlaid = 0;
    const int frames = static_cast<int>(3.0 * groove.beats * 60.0 / bpm * 120.0);
    for (int i = 0; i <= frames; ++i) {
        const double t = i / 120.0;
        const motion::ClipPose base = off.update(t).pose;
        const motion::DanceFrame frame = full.update(t);
        const std::int64_t loop = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(std::floor(frame.clipBeat / groove.beats)));
        const double local = frame.clipBeat - static_cast<double>(loop) * groove.beats;
        const std::vector<motion::PrimitiveCue> previous =
            loop > 0 ? full.overlaysFor(groove.id, loop - 1) : std::vector<motion::PrimitiveCue>{};
        const motion::ComposedPose expected = motion::composeLoopCues(
            base, full.overlaysFor(groove.id, loop), previous, local, groove.beats, options);
        worst = std::max(worst, maxComponentDiff(frame.pose, expected.pose));
        if (maxComponentDiff(expected.pose, base) > 1e-6) ++overlaid;
    }
    CHECK(worst < 1e-5);
    CHECK(overlaid > frames / 4);
}

namespace {

// BG Beat analysis of a synthetic track, as the desktop host gets it.
AnalysisResult analyzeSynthetic(const test::SyntheticTrack& track) {
    std::string error;
    const PcmBuffer pcm = PcmBuffer::fromAudio(track.audio, &error);
    if (!pcm.valid()) {
        throw test::Failure("failed to create PCM: " + error);
    }
    const Analyzer analyzer(AnalysisConfig{});
    AnalysisReport report = analyzer.analyze(pcm);
    if (!report.ok()) {
        throw test::Failure("analysis failed: " + report.message);
    }
    return std::move(report.result);
}

// A steady grid whose waveform has amplitude `before` until `switchAt` and `after` from then on.
AnalysisResult gridWithDynamics(double bpm, double duration, double switchAt, float before,
                                float after) {
    AnalysisResult analysis;
    analysis.info.duration = duration;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 0.95f;
    analysis.beatPhaseConfidence = 0.95f;
    analysis.meterConfidence = 0.9f;
    analysis.downbeatConfidence = 0.9f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = true;
    const int count = static_cast<int>(duration * bpm / 60.0);
    for (int i = 0; i < count; ++i) {
        Beat beat;
        beat.time = i * 60.0 / bpm;
        beat.strength = i % 4 == 0 ? 1.0f : 0.6f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % 4;
        beat.barIndex = i / 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
    }
    analysis.waveformPeaksPerSecond = 50.0f;
    const int bins = static_cast<int>(duration * 50.0);
    for (int k = 0; k < bins; ++k) {
        const float amplitude = k / 50.0 < switchAt ? before : after;
        analysis.waveform.push_back(WaveformPeak{-amplitude, amplitude});
    }
    return analysis;
}

// Three 8-second phrases at 120 BPM: calm, loud, calm. The sections and the onset strengths say
// so, which gives the auto plan Groove, Energetic, Groove, with an energy jump at each seam.
AnalysisResult threePhrases() {
    AnalysisResult analysis;
    const double bpm = 120.0;
    const double duration = 24.0;
    analysis.info.duration = duration;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 0.95f;
    analysis.beatPhaseConfidence = 0.95f;
    analysis.meterConfidence = 0.9f;
    analysis.downbeatConfidence = 0.9f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = true;
    const auto energyAt = [](double t) { return t >= 8.0 && t < 16.0 ? 0.95f : 0.1f; };
    for (int i = 0; i < static_cast<int>(duration * bpm / 60.0); ++i) {
        Beat beat;
        beat.time = i * 60.0 / bpm;
        beat.strength = i % 4 == 0 ? 1.0f : 0.6f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % 4;
        beat.barIndex = i / 4;
        beat.downbeat = beat.beatInBar == 0;
        analysis.beats.push_back(beat);
        Onset onset;
        onset.time = beat.time;
        onset.strength = energyAt(beat.time);
        analysis.onsets.push_back(onset);
    }
    for (int k = 0; k < 3; ++k) {
        Section section;
        section.start = 8.0 * k;
        section.end = 8.0 * (k + 1);
        section.kind = SectionKind::Main;
        section.energy = energyAt(section.start + 4.0);
        section.confidence = 0.9f;
        analysis.sections.push_back(section);
    }
    return analysis;
}

bool isHandOrHead(motion::PrimitiveKind kind) {
    using K = motion::PrimitiveKind;
    return kind == K::HandFlick || kind == K::FingerRipple || kind == K::WristRoll ||
           kind == K::FingerSpread || kind == K::FistPulse || kind == K::HeadTilt ||
           kind == K::LookDirection || kind == K::HeadAccent;
}

} // namespace

TEST_CASE(dance.musical_gestures_land_on_bass_accents) {
    // Auto plan over a track whose kick is the body's hit: at least 70 % of the hand and head
    // cues sit within 0.12 beat of a bass accent. Snares and hats may get a gesture too, but
    // rarely, so they stay a minority.
    const AnalysisResult analysis = analyzeSynthetic(test::makeBassAccentTrack());
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setAnalysis(&analysis, 0.0);
    player.setMode(motion::DanceMode::Auto, 0.0);
    const std::vector<motion::DancePlayer::PlannedOverlay> planned =
        player.plannedOverlays(0.0, analysis.info.duration);
    const motion::MusicalTimeline& timeline = player.timeline();
    int handHead = 0;
    int nearBass = 0;
    for (const auto& cue : planned) {
        if (!isHandOrHead(cue.kind)) continue;
        ++handHead;
        const double beat = timeline.beatAt(cue.mediaTime);
        for (const BassAccent& accent : analysis.bassAccents) {
            if (std::abs(beat - timeline.beatAt(accent.time)) <= 0.12) {
                ++nearBass;
                break;
            }
        }
    }
    CHECK(handHead >= 4);
    CHECK(nearBass >= 0.7 * handHead);
}

TEST_CASE(dance.musical_gestures_stay_out_of_silence) {
    // Nothing plays in the silent gap of the track: no overlay cue starts inside a silence.
    const AnalysisResult analysis = analyzeSynthetic(test::makeSilenceTrack());
    CHECK(!analysis.silences.empty());
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setAnalysis(&analysis, 0.0);
    player.setMode(motion::DanceMode::Auto, 0.0);
    const std::vector<motion::DancePlayer::PlannedOverlay> planned =
        player.plannedOverlays(0.0, analysis.info.duration);
    CHECK(!planned.empty());
    int inSilence = 0;
    for (const auto& cue : planned) {
        for (const SilenceRegion& silence : analysis.silences) {
            if (cue.mediaTime >= silence.start && cue.mediaTime <= silence.end) {
                ++inSilence;
            }
        }
    }
    CHECK(inSilence == 0);
}

TEST_CASE(dance.phrase_loudness_sets_the_upper_body_swing) {
    // Two copies of one grid: one is quiet before 20 s and loud after, the other the reverse. At
    // the same media time both play the same clip at the same beat, so the only difference is the
    // phrase loudness: 0.75 in the quiet passage, 1.15 in the loud one. The chest's angular speed
    // there scales with the swing, so it is about 0.65 times as fast in the quiet copy.
    const AnalysisResult quietFirst = gridWithDynamics(92.0, 40.0, 20.0, 0.1f, 0.6f);
    const AnalysisResult loudFirst = gridWithDynamics(92.0, 40.0, 20.0, 0.6f, 0.1f);
    motion::DancePlayer quiet;
    motion::DancePlayer loud;
    for (motion::DancePlayer* player : {&quiet, &loud}) {
        player->loadLibrary(dancesDir());
        player->setOverlayMode(motion::OverlayMode::Off);
    }
    quiet.setAnalysis(&quietFirst, 0.0);
    loud.setAnalysis(&loudFirst, 0.0);
    for (motion::DancePlayer* player : {&quiet, &loud}) {
        player->select(motion::DanceStyle::Groove, 0.0, true);
    }
    CHECK(quiet.timeline().phraseLoudnessAt(9.0) < 0.1f);
    CHECK(loud.timeline().phraseLoudnessAt(9.0) > 0.9f);

    const auto chestSpeed = [](motion::DancePlayer& player, double from, double to) {
        double total = 0.0;
        int steps = 0;
        motion::ClipPose previous;
        for (int i = 0; i <= static_cast<int>(to * 240.0); ++i) {
            const motion::ClipPose pose = player.update(i / 240.0).pose;
            if (i >= static_cast<int>(from * 240.0)) {
                if (steps > 0) {
                    const math::Quat& p =
                        previous.bone[static_cast<std::size_t>(vrm::HumanoidBone::Chest)];
                    const math::Quat& q = pose.bone[static_cast<std::size_t>(vrm::HumanoidBone::Chest)];
                    total += 2.0 * std::acos(std::min(1.0f, std::abs(math::dot(p, q))));
                }
                ++steps;
            }
            previous = pose;
        }
        return total / std::max(1, steps - 1);
    };
    const double quietSpeed = chestSpeed(quiet, 6.0, 12.0);
    const double loudSpeed = chestSpeed(loud, 6.0, 12.0);
    CHECK(loudSpeed > 0.0);
    // The ratio is 1.00 with the phrase factor off; scaling the larger swing angles is not exactly
    // linear, so the measured 0.59 sits a little under 0.65.
    const double ratio = quietSpeed / loudSpeed;
    CHECK(ratio > 0.5);
    CHECK(ratio < 0.8);
}

TEST_CASE(dance.auto_phrases_join_from_different_entries) {
    // Two calm phrases of one style, with a loud phrase between them: the second Groove starts
    // from another entry point of the clip, since the poses allow it.
    const AnalysisResult analysis = threePhrases();
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setAnalysis(&analysis, 0.0);
    player.setMode(motion::DanceMode::Auto, 0.0);
    std::vector<double> grooveEntries;
    for (const motion::ScheduleSegment& segment : player.director().schedule().segments()) {
        if (segment.clip && segment.clip->style == motion::DanceStyle::Groove) {
            grooveEntries.push_back(segment.entryClipBeat);
        }
    }
    CHECK(grooveEntries.size() == 2);
    if (grooveEntries.size() == 2) {
        CHECK(grooveEntries[0] != grooveEntries[1]);
    }
}

TEST_CASE(dance.auto_phrase_after_an_energy_jump_starts_with_a_hit) {
    const AnalysisResult analysis = threePhrases();
    motion::DancePlayer player;
    player.loadLibrary(dancesDir());
    player.setAnalysis(&analysis, 0.0);
    player.setMode(motion::DanceMode::Auto, 0.0);
    const auto planned = player.plannedOverlays(0.0, analysis.info.duration);
    for (double seam : {8.0, 16.0}) {
        bool hit = false;
        for (const auto& cue : planned) {
            if (cue.reason == "phrase hit" && std::abs(cue.mediaTime - seam) < 0.05) {
                hit = true;
            }
        }
        CHECK(hit);
    }
}

TEST_CASE(dance.musical_gestures_are_deterministic_per_seed) {
    const AnalysisResult analysis = analyzeSynthetic(test::makeBassAccentTrack());
    const auto plan = [&](std::uint32_t seed) {
        motion::DancePlayer player;
        player.loadLibrary(dancesDir());
        player.setOverlaySeed(seed);
        player.setAnalysis(&analysis, 0.0);
        player.setMode(motion::DanceMode::Auto, 0.0);
        return player.plannedOverlays(0.0, analysis.info.duration);
    };
    const auto a = plan(3u);
    const auto b = plan(3u);
    const auto c = plan(77u);
    CHECK(!a.empty());
    CHECK(a.size() == b.size());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = a[i].mediaTime == b[i].mediaTime && a[i].kind == b[i].kind &&
               a[i].clipId == b[i].clipId && a[i].reason == b[i].reason;
    }
    CHECK(same);
    // Another host seed plans other gestures on the same track.
    bool differs = a.size() != c.size();
    for (std::size_t i = 0; !differs && i < a.size(); ++i) {
        differs = a[i].mediaTime != c[i].mediaTime || a[i].kind != c[i].kind;
    }
    CHECK(differs);
}

TEST_CASE(dance.head_world_yaw_is_stabilised_during_the_spin) {
    // Energetic turns the whole body by its root yaw (about 180 deg in one bit). Calm
    // (energy 0.2), the head keeps about half of that turn in world space. At the dance's
    // own energy (0.6) there is no stabilisation, so the head goes round with the body.
    const motion::HumanoidClip& energetic = *library().forStyle(motion::DanceStyle::Energetic);
    constexpr double kPi = 3.14159265358979323846;
    const auto headYawRange = [&](float energy) {
        const int steps = 480;
        double previous = 0.0;
        double unwrapped = 0.0;
        double lo = 0.0;
        double hi = 0.0;
        for (int i = 0; i <= steps; ++i) {
            const double beat = energetic.beats * static_cast<double>(i) / steps;
            motion::ClipPose pose = motion::sampleClip(energetic, beat);
            motion::applyAlive(pose, 0.1 * i, energy, 7u, 1.0f);
            const math::Quat chain =
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::Hips)] *
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::Spine)] *
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::Chest)] *
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::UpperChest)] *
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::Neck)] *
                pose.bone[static_cast<std::size_t>(motion::HumanoidBone::Head)];
            const math::Quat root =
                math::fromAxisAngle({0.0f, 1.0f, 0.0f},
                                    static_cast<float>(2.0 * kPi * pose.rootYaw));
            const math::Vec3 forward = math::rotate(root * chain, {0.0f, 0.0f, 1.0f});
            const double yaw =
                std::atan2(static_cast<double>(forward.x), static_cast<double>(forward.z)) *
                180.0 / kPi;
            if (i == 0) {
                unwrapped = yaw;
            } else {
                double step = yaw - previous;
                step -= 360.0 * std::round(step / 360.0);  // shortest way round
                unwrapped += step;
            }
            previous = yaw;
            lo = std::min(lo, unwrapped);
            hi = std::max(hi, unwrapped);
        }
        return hi - lo;
    };
    const double rangeCalm = headYawRange(0.2f);
    const double rangeDancing = headYawRange(0.6f);
    CHECK(rangeDancing > 150.0);      // the spin really turns the head with the body
    CHECK(rangeCalm < 0.7 * rangeDancing);
    CHECK(rangeCalm > 0.3 * rangeDancing);  // half is kept, not zero: it still reads as a turn
}
