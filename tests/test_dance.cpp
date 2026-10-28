#include "framework.hpp"

#include "motion_metrics.hpp"

#include "bgmedia/motion3/DancePlayer.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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
