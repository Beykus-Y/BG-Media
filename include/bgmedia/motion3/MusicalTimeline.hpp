#pragma once

// BG Motion 3.1 musical timeline.
//
// The timeline is the bridge between BG Beat (absolute onset/beat/bass events)
// and choreography. It keeps three things separate that used to be conflated:
//
//   1. the stable beat grid (BeatClock) used to sample animation smoothly;
//   2. the metric structure (meter, bar, downbeat) with an explicit confidence;
//   3. the accent map (real onset/bass timestamps, normalised strength).
//
// A low-confidence downbeat is never treated as a fact: alignment code must ask
// downbeatTrusted() before snapping a dance phrase to bar 1. Accent events keep
// the original media timestamps, so they are not quantised to a synthetic
// eight-slot pattern.

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/motion3/BeatClock.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bgmedia::motion {

enum class AccentKind : std::uint8_t {
    Beat,
    Downbeat,
    Bass,
    Offbeat,
    Subdivision
};

[[nodiscard]] const char* accentKindName(AccentKind kind);

struct AccentEvent {
    double time = 0.0;          // absolute media seconds
    float strength = 0.0f;      // normalised 0..1 within the track
    float confidence = 0.0f;    // heuristic 0..1, not a probability
    int beatIndex = -1;         // nearest musical beat, -1 if unknown
    float beatOffset = 0.0f;    // signed offset from beatIndex, in beats
    bool downbeat = false;
    bool bass = false;
    AccentKind kind = AccentKind::Beat;
};

struct TimelineBeat {
    int index = 0;
    double time = 0.0;
    float strength = 0.0f;      // onset strength at the beat
    float confidence = 0.0f;
    float accent = 0.0f;        // strongest normalised event within the beat
    float bass = 0.0f;
    bool downbeat = false;
    bool extrapolated = false;
};

class MusicalTimeline {
public:
    MusicalTimeline() = default;

    // A synthetic, trusted grid (Motion Lab / manual BPM). There are no audio
    // accents, but the bar structure is known.
    [[nodiscard]] static MusicalTimeline constant(double bpm, double firstBeatTime = 0.0,
                                                  int beatsPerBar = 4);
    // Grid only, with an explicit trust flag. Used when a BeatClock is installed
    // without a full analysis.
    [[nodiscard]] static MusicalTimeline fromClock(const BeatClock& clock,
                                                   bool downbeatTrusted = true);
    // Full timeline from BG Beat: stable grid, metric confidence, real accent
    // timestamps from onsets and bass accents, silence map.
    [[nodiscard]] static MusicalTimeline fromAnalysis(const AnalysisResult& analysis,
                                                      double fallbackBpm = 100.0);

    [[nodiscard]] bool valid() const { return valid_; }
    // True when the timeline was built from BG Beat analysis, so its accents and silences are
    // evidence from the track. A grid-only timeline (Motion Lab, no track) is not analysed.
    [[nodiscard]] bool analyzed() const { return analyzed_; }
    [[nodiscard]] const BeatClock& grid() const { return grid_; }
    [[nodiscard]] double nominalBpm() const { return grid_.nominalBpm(); }
    [[nodiscard]] double duration() const { return duration_; }

    [[nodiscard]] int beatsPerBar() const { return beatsPerBar_; }
    [[nodiscard]] int beatUnit() const { return beatUnit_; }
    [[nodiscard]] bool compound() const { return compound_; }
    [[nodiscard]] int subdivision() const { return subdivision_; }
    [[nodiscard]] float subdivisionConfidence() const { return subdivisionConfidence_; }
    [[nodiscard]] float meterConfidence() const { return meterConfidence_; }
    [[nodiscard]] float downbeatConfidence() const { return downbeatConfidence_; }
    [[nodiscard]] bool downbeatTrusted(float threshold = 0.45f) const {
        return downbeatTrusted_ && downbeatConfidence_ >= threshold &&
               meterConfidence_ >= threshold;
    }
    // Honest, human readable meter. A compound feel with weak metric evidence
    // is labelled as ambiguous instead of pretending it is 6/8 or 12/8.
    [[nodiscard]] std::string meterLabel() const;

    // --- phase -------------------------------------------------------------
    [[nodiscard]] double beatAt(double mediaTime) const { return grid_.beatAt(mediaTime); }
    [[nodiscard]] double timeAtBeat(double beat) const { return grid_.timeAtBeat(beat); }
    [[nodiscard]] double bpmAt(double mediaTime) const { return grid_.bpmAt(mediaTime); }
    [[nodiscard]] int beatIndexAt(double mediaTime) const;
    [[nodiscard]] float beatPhaseAt(double mediaTime) const;
    [[nodiscard]] int beatInBarAt(double mediaTime) const;
    [[nodiscard]] bool downbeatAt(double mediaTime) const;
    [[nodiscard]] int barAt(double mediaTime) const;   // 0-based
    [[nodiscard]] double beatPeriodAt(double mediaTime) const;

    // Next boundary helpers. When the downbeat is not trusted, nextDownbeatTime
    // degrades gracefully to the next beat instead of inventing a bar.
    [[nodiscard]] double nextBeatTime(double mediaTime) const;
    [[nodiscard]] double nextDownbeatTime(double mediaTime) const;
    [[nodiscard]] double nextBarTime(double mediaTime) const;
    [[nodiscard]] double nextPhraseTime(double mediaTime, int phraseBars = 4) const;
    [[nodiscard]] int nextDownbeatIndex(double mediaTime) const;

    // --- events ------------------------------------------------------------
    [[nodiscard]] const std::vector<TimelineBeat>& beats() const { return beats_; }
    [[nodiscard]] const std::vector<AccentEvent>& accents() const { return accents_; }
    [[nodiscard]] const std::vector<BassAccent>& bassEvents() const { return bass_; }
    [[nodiscard]] const std::vector<SilenceRegion>& silences() const { return silences_; }

    [[nodiscard]] float accentAt(double mediaTime, double window = 0.12) const;
    [[nodiscard]] float accentConfidenceAt(double mediaTime, double window = 0.12) const;
    [[nodiscard]] float bassAt(double mediaTime, double window = 0.12) const;
    [[nodiscard]] float beatAccent(int beatIndex) const;
    [[nodiscard]] float beatBass(int beatIndex) const;
    [[nodiscard]] float beatStrength(int beatIndex) const;
    [[nodiscard]] float beatConfidence(int beatIndex) const;
    [[nodiscard]] bool beatIsDownbeat(int beatIndex) const;
    [[nodiscard]] const AccentEvent* nextAccent(double mediaTime,
                                                double lookahead = 2.0) const;
    [[nodiscard]] bool silentAt(double mediaTime) const;
    // 1 outside silence, 0 inside, linear ramps within `fadeSeconds` of each
    // edge. Used to fade the expressive warp instead of hard-switching it.
    [[nodiscard]] float silenceWeightAt(double mediaTime,
                                         double fadeSeconds = 0.12) const;

    [[nodiscard]] std::string describe() const;

private:
    void buildBeatTable(const std::vector<double>& times,
                        const std::vector<std::uint8_t>& downbeatFlags);
    void inferSubdivision(const std::vector<Onset>& onsets);

    bool valid_ = false;
    bool analyzed_ = false;
    BeatClock grid_;
    double duration_ = 1.0e9;
    int beatsPerBar_ = 4;
    int beatUnit_ = 4;
    bool compound_ = false;
    int subdivision_ = 1;
    float subdivisionConfidence_ = 0.0f;
    float meterConfidence_ = 1.0f;
    float downbeatConfidence_ = 1.0f;
    bool downbeatTrusted_ = true;
    std::vector<TimelineBeat> beats_;
    std::vector<AccentEvent> accents_;
    std::vector<BassAccent> bass_;
    std::vector<SilenceRegion> silences_;
};

} // namespace bgmedia::motion
