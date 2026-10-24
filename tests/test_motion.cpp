#include "framework.hpp"

#include "bgmedia/choreography/Choreography.hpp"
#include "bgmedia/sync/Transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <vector>

namespace {

using bgmedia::AnalysisResult;
using bgmedia::Beat;
using bgmedia::BassAccent;
using bgmedia::CharacterPose;
using bgmedia::Choreography;
using bgmedia::ChoreographyBuilder;
using bgmedia::ChoreographySampler;
using bgmedia::Easing;
using bgmedia::MotionKeyframe;
using bgmedia::MoveKind;

AnalysisResult makeGrid(double bpm, double duration, int bars = 4,
                        bool addSilence = false) {
    using namespace bgmedia;
    AnalysisResult analysis;
    analysis.info.duration = duration;
    analysis.info.sampleRate = 44100;
    analysis.info.sourceChannels = 2;
    analysis.info.analyzerVersion = 1;
    analysis.bpm = bpm;
    analysis.bpmConfidence = 0.95f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = true;

    const double period = 60.0 / bpm;
    int index = 0;
    double time = 0.45;
    while (time < duration - 0.12 && index < bars * 4) {
        Beat beat;
        beat.time = time;
        beat.beatInBar = index % 4;
        beat.barIndex = index / 4;
        beat.downbeat = beat.beatInBar == 0;
        beat.strength = beat.downbeat ? 1.0f : 0.62f;
        beat.confidence = 0.94f;
        analysis.beats.push_back(beat);

        // Bass on even bars only, so odd-bar downbeats exercise AccentPose
        // and even-bar downbeats exercise BassDip.
        if (beat.downbeat && (beat.barIndex % 2 == 0)) {
            BassAccent accent;
            accent.time = time;
            accent.strength = 0.95f;
            accent.lowBandRatio = 0.72f;
            accent.confidence = 0.92f;
            accent.nearestBeat = index;
            accent.offsetFromBeat = 0.0;
            analysis.bassAccents.push_back(accent);
        }
        if (beat.barIndex == 3 && beat.beatInBar == 2) {
            BassAccent accent;
            accent.time = time;
            accent.strength = 0.8f;
            accent.lowBandRatio = 0.65f;
            accent.confidence = 0.85f;
            accent.nearestBeat = index;
            analysis.bassAccents.push_back(accent);
        }
        ++index;
        time += period;
    }

    if (addSilence) {
        SilenceRegion silence;
        silence.start = duration * 0.62;
        silence.end = duration * 0.82;
        silence.confidence = 0.9f;
        analysis.silences.push_back(silence);
    }
    return analysis;
}

AnalysisResult makeTempoChange() {
    using namespace bgmedia;
    AnalysisResult analysis;
    analysis.info.duration = 16.0;
    analysis.info.sampleRate = 44100;
    analysis.info.sourceChannels = 1;
    analysis.info.analyzerVersion = 1;
    analysis.bpm = 108.0;
    analysis.bpmConfidence = 0.55f;
    analysis.beatsPerBar = 4;
    analysis.tempoStable = false;

    int index = 0;
    double time = 0.45;
    const double split = 8.0;
    const double periodA = 60.0 / 92.0;
    while (time < split - 0.1) {
        Beat beat;
        beat.time = time;
        beat.beatInBar = index % 4;
        beat.barIndex = index / 4;
        beat.downbeat = beat.beatInBar == 0;
        beat.strength = beat.downbeat ? 1.0f : 0.62f;
        beat.confidence = 0.9f;
        analysis.beats.push_back(beat);
        ++index;
        time += periodA;
    }
    time = split + 0.1;
    int local = 0;
    const double periodB = 60.0 / 123.0;
    while (time < analysis.info.duration - 0.1) {
        Beat beat;
        beat.time = time;
        beat.beatInBar = local % 4;
        beat.barIndex = index / 4;
        beat.downbeat = beat.beatInBar == 0;
        beat.strength = beat.downbeat ? 1.0f : 0.62f;
        beat.confidence = 0.9f;
        analysis.beats.push_back(beat);
        ++index;
        ++local;
        time += periodB;
    }
    return analysis;
}

double angleDistance(float turnsA, float turnsB) {
    double delta = std::abs(static_cast<double>(turnsA) - turnsB);
    delta = std::fmod(delta + 0.5, 1.0);
    if (delta < 0.0) {
        delta += 1.0;
    }
    return std::abs(delta - 0.5);
}

double poseDelta(const CharacterPose& a, const CharacterPose& b) {
    double maxDelta = angleDistance(a.rotation, b.rotation);
#define BGM_DELTA_FIELD(field)                                                    \
    maxDelta = std::max(maxDelta, std::abs(static_cast<double>(a.field) - b.field));
    BGM_DELTA_FIELD(bob)
    BGM_DELTA_FIELD(lean)
    BGM_DELTA_FIELD(armLeft)
    BGM_DELTA_FIELD(armRight)
    BGM_DELTA_FIELD(legLeft)
    BGM_DELTA_FIELD(legRight)
    BGM_DELTA_FIELD(energy)
    BGM_DELTA_FIELD(headNod)
    BGM_DELTA_FIELD(headTilt)
    BGM_DELTA_FIELD(headTurn)
    BGM_DELTA_FIELD(chestTwist)
    BGM_DELTA_FIELD(hipShift)
    BGM_DELTA_FIELD(kneeLeft)
    BGM_DELTA_FIELD(kneeRight)
    BGM_DELTA_FIELD(elbowLeft)
    BGM_DELTA_FIELD(elbowRight)
    BGM_DELTA_FIELD(footLeft)
    BGM_DELTA_FIELD(footRight)
#undef BGM_DELTA_FIELD
    return maxDelta;
}

bool samePose(const CharacterPose& a, const CharacterPose& b) {
    return a.bob == b.bob && a.lean == b.lean && a.armLeft == b.armLeft &&
           a.armRight == b.armRight && a.legLeft == b.legLeft &&
           a.legRight == b.legRight && a.rotation == b.rotation &&
           a.energy == b.energy && a.headNod == b.headNod &&
           a.headTilt == b.headTilt && a.headTurn == b.headTurn &&
           a.chestTwist == b.chestTwist && a.hipShift == b.hipShift &&
           a.kneeLeft == b.kneeLeft && a.kneeRight == b.kneeRight &&
           a.elbowLeft == b.elbowLeft && a.elbowRight == b.elbowRight &&
           a.footLeft == b.footLeft && a.footRight == b.footRight;
}

Choreography makeCustom(const std::vector<MotionKeyframe>& keys) {
    Choreography choreography;
    choreography.keyframes = keys;
    choreography.duration = keys.empty() ? 0.0 : keys.back().time;
    choreography.smoothed = true;
    return choreography;
}

} // namespace

TEST_CASE(motion.accents_hit_exact_beat_times) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(120.0, 16.0);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    REQUIRE(!choreography.keyframes.empty());
    REQUIRE(!choreography.accents.empty());
    ChoreographySampler sampler;

    for (const AccentTarget& target : choreography.accents) {
        const CharacterPose pose = sampler.sample(choreography, target.time);
        CHECK(poseDelta(pose, target.pose) < 2e-4);
        bool onBeat = false;
        for (const Beat& beat : analysis.beats) {
            if (std::abs(beat.time - target.time) < 1e-7) {
                onBeat = true;
                break;
            }
        }
        CHECK(onBeat);
    }
}

TEST_CASE(motion.keyframes_are_continuous) {
    using namespace bgmedia;
    // Eight bars deliberately include the transition back into ArmWave, which
    // is the movement with the largest cross-bar arm excursion.
    const AnalysisResult analysis = makeGrid(120.0, 32.0, 8, true);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;

    double maxDelta = 0.0;
    constexpr double step = 0.002;
    for (double t = step; t < choreography.duration - step; t += step) {
        const CharacterPose before = sampler.sample(choreography, t - step);
        const CharacterPose after = sampler.sample(choreography, t);
        maxDelta = std::max(maxDelta, poseDelta(before, after));
    }
    CHECK(maxDelta < 0.15);

    double edgeMax = 0.0;
    constexpr double edge = 0.001;
    for (const MotionKeyframe& key : choreography.keyframes) {
        if (key.time <= edge || key.time >= choreography.duration - edge) {
            continue;
        }
        const CharacterPose before = sampler.sample(choreography, key.time - edge);
        const CharacterPose after = sampler.sample(choreography, key.time + edge);
        edgeMax = std::max(edgeMax, poseDelta(before, after));
    }
    // Cue boundaries are the legacy movement boundaries and must not introduce
    // a step either, even though smooth sampling no longer uses the cue pose.
    for (const DanceCue& cue : choreography.cues) {
        if (cue.time <= edge || cue.time >= choreography.duration - edge) {
            continue;
        }
        edgeMax = std::max(
            edgeMax, poseDelta(sampler.sample(choreography, cue.time - edge),
                               sampler.sample(choreography, cue.time + edge)));
    }
    CHECK(edgeMax < 0.07);
}

TEST_CASE(motion.deterministic_across_frame_rates_and_order) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(123.0, 12.0);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;

    for (double fps : {30.0, 60.0, 120.0}) {
        std::vector<double> times;
        for (int frame = 0; frame < static_cast<int>(fps * 10.0); ++frame) {
            times.push_back(static_cast<double>(frame) / fps);
        }
        std::vector<CharacterPose> forward;
        for (double t : times) {
            forward.push_back(sampler.sample(choreography, t));
        }
        for (std::size_t i = 0; i < times.size(); ++i) {
            const CharacterPose direct = sampler.sample(choreography, times[i]);
            CHECK(samePose(forward[i], direct));
        }
        for (std::size_t i = times.size(); i > 0; --i) {
            CHECK(samePose(forward[i - 1], sampler.sample(choreography, times[i - 1])));
        }
    }

    const std::vector<double> arbitrary = {6.1, 0.1, 9.4, 3.333, 7.77, 1.234, 11.9};
    std::vector<CharacterPose> first;
    for (double t : arbitrary) {
        first.push_back(sampler.sample(choreography, t));
    }
    for (std::size_t i = arbitrary.size(); i > 0; --i) {
        CHECK(samePose(first[i - 1], sampler.sample(choreography, arbitrary[i - 1])));
    }
}

TEST_CASE(motion.pause_and_seek_are_stateless) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(92.0, 12.0);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;

    const double pausedTime = 4.37;
    const CharacterPose atPause = sampler.sample(choreography, pausedTime);
    for (int i = 0; i < 10; ++i) {
        CHECK(samePose(atPause, sampler.sample(choreography, pausedTime)));
    }

    const double seekTime = 8.91;
    const CharacterPose atSeek = sampler.sample(choreography, seekTime);
    CHECK(samePose(atSeek, sampler.sample(choreography, seekTime)));
    const CharacterPose afterResume = sampler.sample(choreography, seekTime + 0.25);
    CHECK(!samePose(atSeek, afterResume));
}

TEST_CASE(motion.easing_profiles_and_shortest_turn) {
    using namespace bgmedia;
    auto makeOne = [](Easing easing) {
        Choreography choreography;
        MotionKeyframe a;
        a.time = 0.0;
        a.pose.bob = 0.0f;
        a.pose.rotation = 0.9f;
        a.easingToNext = easing;
        MotionKeyframe b;
        b.time = 1.0;
        b.pose.bob = 1.0f;
        b.pose.rotation = 0.1f;
        b.easingToNext = Easing::Linear;
        choreography.keyframes = {a, b};
        choreography.duration = 1.0;
        return choreography;
    };

    ChoreographySampler sampler;
    const CharacterPose linear = sampler.sample(makeOne(Easing::Linear), 0.25);
    const CharacterPose easeIn = sampler.sample(makeOne(Easing::EaseIn), 0.25);
    const CharacterPose easeOut = sampler.sample(makeOne(Easing::EaseOut), 0.25);
    const CharacterPose easeInOut = sampler.sample(makeOne(Easing::EaseInOut), 0.25);
    const CharacterPose smooth = sampler.sample(makeOne(Easing::SmoothStep), 0.25);

    CHECK_NEAR(linear.bob, 0.25, 1e-5);
    CHECK_NEAR(easeIn.bob, 0.0625, 1e-5);
    CHECK_NEAR(easeOut.bob, 0.4375, 1e-5);
    CHECK_NEAR(easeInOut.bob, 0.125, 1e-5);
    CHECK_NEAR(smooth.bob, 0.15625, 1e-5);
    CHECK(easeIn.bob < easeInOut.bob);
    CHECK(easeOut.bob > linear.bob);

    const CharacterPose mid = sampler.sample(makeOne(Easing::Linear), 0.5);
    // 0.9 turns to 0.1 turns uses the shortest arc through 0 turns.
    CHECK_NEAR(mid.rotation, 0.0, 1e-6);
    CHECK(angleDistance(mid.rotation, 0.5f) > 0.49);
}

TEST_CASE(motion.tempo_change_has_no_pose_jump) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeTempoChange();
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    CHECK(!choreography.keyframes.empty());
    CHECK(!choreography.accents.empty());

    const double edge = 0.001;
    double maxDelta = 0.0;
    for (const MotionKeyframe& key : choreography.keyframes) {
        if (key.time <= edge || key.time >= choreography.duration - edge) {
            continue;
        }
        maxDelta = std::max(
            maxDelta, poseDelta(sampler.sample(choreography, key.time - edge),
                                sampler.sample(choreography, key.time + edge)));
    }
    CHECK(maxDelta < 0.08);

    for (const AccentTarget& target : choreography.accents) {
        CHECK(poseDelta(sampler.sample(choreography, target.time), target.pose) < 2e-4);
    }
}

TEST_CASE(motion.fast_tempo_reduces_amplitude) {
    using namespace bgmedia;
    const auto slow = ChoreographyBuilder{}.build(makeGrid(100.0, 10.0, 4));
    const auto fast = ChoreographyBuilder{}.build(makeGrid(200.0, 10.0, 4));
    ChoreographySampler sampler;

    auto peakAmplitude = [&](const Choreography& choreography) {
        double peak = 0.0;
        for (double t = 0.0; t < choreography.duration; t += 0.005) {
            const CharacterPose pose = sampler.sample(choreography, t);
            peak = std::max(peak, static_cast<double>(std::abs(pose.bob)));
            peak = std::max(peak, static_cast<double>(std::abs(pose.kneeLeft)));
            peak = std::max(peak, static_cast<double>(std::abs(pose.armLeft)));
        }
        return peak;
    };
    const double slowPeak = peakAmplitude(slow);
    const double fastPeak = peakAmplitude(fast);
    CHECK(fastPeak < slowPeak * 0.82);
    CHECK(fastPeak > 0.25);
}

TEST_CASE(motion.exercises_all_motion_types) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(120.0, 16.0, 4);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    std::set<int> moves;
    for (const MotionClip& clip : choreography.clips) {
        moves.insert(static_cast<int>(clip.move));
    }
    CHECK(moves.count(static_cast<int>(MoveKind::Bounce)) > 0);
    CHECK(moves.count(static_cast<int>(MoveKind::Sway)) > 0);
    CHECK(moves.count(static_cast<int>(MoveKind::ArmWave)) > 0);
    CHECK(moves.count(static_cast<int>(MoveKind::HeadNod)) > 0);
    CHECK(moves.count(static_cast<int>(MoveKind::BassDip)) > 0);
    CHECK(moves.count(static_cast<int>(MoveKind::AccentPose)) > 0);
}

TEST_CASE(motion.anticipation_precedes_every_accent) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(123.0, 32.0, 8);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    int accentClips = 0;
    for (const MotionClip& clip : choreography.clips) {
        if (clip.accentTime < 0.0) {
            continue;
        }
        ++accentClips;
        CHECK(clip.start < clip.accentTime);
        CHECK(clip.anticipationStart <= clip.start + 1e-9);
        CHECK(clip.end > clip.accentTime);
    }
    CHECK(accentClips >= 8);
    for (const AccentTarget& target : choreography.accents) {
        CHECK_NEAR(sampler.sample(choreography, target.time).energy,
                   target.pose.energy, 2e-4);
    }
    for (std::size_t i = 1; i < choreography.keyframes.size(); ++i) {
        CHECK(choreography.keyframes[i].time >
              choreography.keyframes[i - 1].time);
    }
}

TEST_CASE(motion.playback_rate_and_clock_keep_media_time) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(123.0, 12.0, 6);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    PlaybackClock clock;

    constexpr double duration = 12.0;
    constexpr double latency = 0.02;
    constexpr double rate = 1.5;
    for (int frame = 0; frame <= 90; ++frame) {
        const double host = static_cast<double>(frame) / 60.0;
        TransportSnapshot snapshot;
        snapshot.sourcePosition = latency + host * rate;
        snapshot.duration = duration;
        snapshot.playing = true;
        snapshot.rate = rate;
        snapshot.outputLatency = latency;
        snapshot.discontinuity = 0;
        snapshot.hostTime = host;
        const double mediaTime = clock.update(snapshot);
        CHECK(samePose(sampler.sample(choreography, mediaTime),
                       sampler.sample(choreography, mediaTime)));
    }

    TransportSnapshot paused;
    paused.sourcePosition = latency + 1.5 * rate;
    paused.duration = duration;
    paused.playing = false;
    paused.rate = rate;
    paused.outputLatency = latency;
    paused.discontinuity = 0;
    paused.hostTime = 2.0;
    const double held = clock.update(paused);
    const CharacterPose frozen = sampler.sample(choreography, held);
    CHECK(samePose(frozen, sampler.sample(choreography, held)));
    CHECK(samePose(frozen, sampler.sample(choreography, clock.position())));
}

TEST_CASE(motion.stepped_sampler_still_available) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(120.0, 8.0, 2);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    const CharacterPose smooth = sampler.sample(choreography, 2.2);
    const CharacterPose stepped = sampler.sampleStepped(choreography, 2.2);
    CHECK(smooth.move != MoveKind::Idle);
    CHECK(stepped.move != MoveKind::Idle);
    CHECK(!samePose(smooth, stepped));
}

TEST_CASE(motion.silence_freeze_is_continuous) {
    using namespace bgmedia;
    const AnalysisResult analysis = makeGrid(120.0, 16.0, 4, true);
    const Choreography choreography = ChoreographyBuilder{}.build(analysis);
    ChoreographySampler sampler;
    const double inside = analysis.silences.front().start +
                          (analysis.silences.front().end - analysis.silences.front().start) * 0.5;
    const CharacterPose freeze = sampler.sample(choreography, inside);
    CHECK(freeze.move == MoveKind::Freeze);
    CHECK_NEAR(freeze.energy, 0.0, 1e-6);
    CHECK_NEAR(freeze.impact, 0.0, 1e-6);
}
