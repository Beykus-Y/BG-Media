#pragma once

// Adapter from the live BG Mix snapshot to the musical state BG Motion needs.
// It deliberately does not depend on motion3 or change BG Motion 3.x: a host
// reads the live tempo and master position here and feeds its existing
// BeatClock/DancePlayer through their public API.

#include "bgmedia/mix/MixTypes.hpp"

#include <cstdint>

namespace bgmedia::mix {

struct LiveMixTempo {
    bool valid = false;      // a master deck is loaded and has a usable tempo
    bool playing = false;
    DeckId master = DeckId::A;
    std::uint64_t trackId = 0;
    // Audible position of the master deck, already latency-compensated.
    double positionSeconds = 0.0;
    double bpm = 0.0;              // live BPM = analysedBpm * playback rate
    double analysedBpm = 0.0;
    // Beat spacing in the same source-media domain as positionSeconds. The
    // live BPM is faster by `rate`; using it here would apply the rate twice.
    double sourceBpm = 0.0;
    double rate = 1.0;
    double beatPeriodSeconds = 0.0;
    // Fractional beat phase at positionSeconds, computed in source time. A host
    // with a real downbeat anchor should prefer its own BeatClock; this value
    // is exact when the grid starts at media time zero and is still useful as a
    // sanity check.
    double beatPhase = 0.0;
    double outputLatencySeconds = 0.0;
};

// Reads the master deck from a snapshot. During a transition `master` moves to
// the incoming deck at the handover point, so callers automatically follow the
// audible musical reference rather than the outgoing deck.
[[nodiscard]] LiveMixTempo liveMixTempo(const MixSnapshot& snapshot) noexcept;

} // namespace bgmedia::mix
