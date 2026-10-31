undefined
TEST_CASE(timeline.bass_index_survives_missing_source_beat) {
    AnalysisResult analysis = makeGrid(120.0, 0.4, 24, 4);
    // The original beat #5 was dropped by BG Beat. BeatClock must insert a
    // virtual slot, so source index 9 no longer means musical beat #9.
    analysis.beats.erase(analysis.beats.begin() + 5);
    BassAccent bass;
    bass.time = 0.4 + 10 * 0.5;
    bass.strength = 0.95f;
    bass.lowBandRatio = 0.85f;
    bass.confidence = 0.90f;
    bass.nearestBeat = 9;
    analysis.bassAccents.push_back(bass);
    const MusicalTimeline timeline = MusicalTimeline::fromAnalysis(analysis);
    bool found = false;
    for (const motion::AccentEvent& event : timeline.accents()) {
        if (event.bass && std::abs(event.time - bass.time) < 0.01) {
            found = true;
            CHECK(event.beatIndex == 10);
            CHECK_NEAR(event.beatOffset, 0.0, 0.02);
        }
    }
    CHECK(found);
}
