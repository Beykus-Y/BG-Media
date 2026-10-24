#include "framework.hpp"

#include "bgmedia/choreography/Choreography.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

bgmedia::AnalysisResult makeAnalysis() {
    using namespace bgmedia;
    AnalysisResult analysis;
    analysis.info.duration = 8.0;
    analysis.info.sampleRate = 44100;
    analysis.info.sourceChannels = 2;
    analysis.info.analyzerVersion = 1;
    analysis.bpm = 120.0;
    analysis.bpmConfidence = 0.9f;
    analysis.beatsPerBar = 4;
    for (int i = 0; i < 12; ++i) {
        Beat beat;
        beat.time = 0.5 + i * 0.5;
        beat.strength = i % 4 == 0 ? 1.0f : 0.55f;
        beat.confidence = 0.9f;
        beat.beatInBar = i % 4;
        beat.barIndex = i / 4;
        beat.downbeat = i % 4 == 0;
        analysis.beats.push_back(beat);
    }
    BassAccent accent;
    accent.time = 2.5;
    accent.strength = 0.9f;
    accent.lowBandRatio = 0.7f;
    accent.confidence = 0.8f;
    accent.nearestBeat = 4;
    analysis.bassAccents.push_back(accent);

    SilenceRegion silence;
    silence.start = 6.3;
    silence.end = 8.0;
    silence.confidence = 0.9f;
    analysis.silences.push_back(silence);
    return analysis;
}

} // namespace

TEST_CASE(choreography.builds_beat_cues) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeAnalysis();
    ChoreographyBuilder builder;
    const Choreography choreography = builder.build(analysis);

    CHECK(!choreography.cues.empty());
    CHECK(choreography.beatsPerBar == 4);
    CHECK(choreography.cues.size() >= analysis.beats.size());

    double previous = -1.0;
    bool hasFreeze = false;
    bool hasAccent = false;
    for (const DanceCue& cue : choreography.cues) {
        CHECK(cue.time >= previous - 1e-9);
        previous = cue.time;
        CHECK(cue.duration > 0.0);
        CHECK(cue.time <= analysis.info.duration + 1e-9);
        hasFreeze = hasFreeze || cue.move == MoveKind::Freeze;
        hasAccent = hasAccent || cue.bassAccent;
    }
    CHECK(hasFreeze);
    CHECK(hasAccent);

    ChoreographySampler sampler;
    const DanceCue* active = sampler.activeCue(choreography, 0.6);
    REQUIRE(active != nullptr);
    CHECK_NEAR(active->time, 0.5, 0.51);

    const CharacterPose poseA = sampler.sample(choreography, 0.55);
    const CharacterPose poseB = sampler.sample(choreography, 0.70);
    CHECK(poseA.energy > 0.0f);
    CHECK(poseB.energy > 0.0f);
    CHECK(poseA.move != MoveKind::Idle);
    CHECK(std::abs(poseA.bob - poseB.bob) > 1e-4f ||
          std::abs(poseA.lean - poseB.lean) > 1e-4f ||
          std::abs(poseA.armLeft - poseB.armLeft) > 1e-4f);

    const CharacterPose freeze = sampler.sample(choreography, 7.0);
    CHECK(freeze.move == MoveKind::Freeze);
    CHECK_NEAR(freeze.energy, 0.0, 1e-6);
}

TEST_CASE(choreography.empty_analysis_is_idle) {
    using namespace bgmedia;
    AnalysisResult empty;
    empty.info.duration = 4.0;
    empty.info.sampleRate = 44100;
    empty.info.sourceChannels = 1;
    const Choreography choreography = ChoreographyBuilder{}.build(empty);
    CHECK(choreography.cues.empty());
    const CharacterPose pose = ChoreographySampler{}.sample(choreography, 1.0);
    CHECK(pose.move == MoveKind::Idle);
    CHECK_NEAR(pose.energy, 0.0, 1e-6);
}

TEST_CASE(choreography.sampling_is_deterministic) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeAnalysis();
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    for (double time : {0.1, 0.5, 1.2, 2.45, 3.9, 5.5, 7.5}) {
        const CharacterPose a = sampler.sample(choreography, time);
        const CharacterPose b = sampler.sample(choreography, time);
        CHECK(a.bob == b.bob);
        CHECK(a.lean == b.lean);
        CHECK(a.armLeft == b.armLeft);
        CHECK(a.armRight == b.armRight);
        CHECK(a.energy == b.energy);
        CHECK(a.move == b.move);
    }
}
