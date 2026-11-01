#include "bgmedia/mix/MixSync.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::mix {

LiveMixTempo liveMixTempo(const MixSnapshot& snapshot) noexcept {
    LiveMixTempo state;
    state.master = snapshot.master;
    state.outputLatencySeconds = snapshot.outputLatencySeconds;
    const DeckStatus& deck = snapshot.decks[deckIndex(snapshot.master)];
    state.trackId = deck.trackId;
    state.playing = deck.playing;
    state.rate = deck.rate;
    state.analysedBpm = deck.analysedBpm;
    state.bpm = deck.bpm > 0.0 ? deck.bpm : deck.analysedBpm * deck.rate;
    if (!deck.loaded || state.bpm <= 0.0) {
        return state;
    }
    state.valid = true;
    state.sourceBpm = deck.analysedBpm > 0.0
                          ? deck.analysedBpm
                          : (state.rate > 1e-9 ? state.bpm / state.rate
                                               : state.bpm);
    if (state.sourceBpm <= 0.0) {
        state.sourceBpm = state.bpm;
    }
    state.beatPeriodSeconds = 60.0 / state.sourceBpm;
    state.positionSeconds =
        std::max(0.0, deck.positionSeconds - state.outputLatencySeconds);
    const double beats = state.positionSeconds / state.beatPeriodSeconds;
    state.beatPhase = beats - std::floor(beats);
    return state;
}

} // namespace bgmedia::mix
