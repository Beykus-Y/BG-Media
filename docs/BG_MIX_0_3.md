# BG Mix 0.3 — Context-Aware Smart AutoMix

## Overview

BG Mix 0.3 upgrades the transition planner and dual-deck engine from a beat/phrase-aligned crossfader into a context-aware **Smart AutoMix** system. It builds directly on top of BG Beat's `AnalysisResult` without duplicating beat tracking or tempo estimation, while keeping `bgmedia_core` pure C++20 and preserving a 100% lock-free, allocation-free real-time audio callback.

---

## Architecture

### 1. Musical Feature Map (`MixFeatureMap` & `MixFeatureCache`)
- **Headers / Sources**: `include/bgmedia/mix/MixFeatureMap.hpp`, `include/bgmedia/mix/MixFeatureCache.hpp`, `src/mix/MixFeatureMap.cpp`, `src/mix/MixFeatureCache.cpp`
- **BS.1770-4 Loudness**: Full K-weighted multi-channel power summation (`Stage 1` high-shelf + `Stage 2` RLB high-pass), 400 ms momentary and 3.0 s short-term loudness on a 10 ms sub-block grid, two-stage (`-70 LUFS` absolute / `-10 LU` relative) gated integrated LUFS, LRA (`loudnessRangeLu`), and sample peak (`samplePeakDbfs`).
- **Adaptive Audible Bounds (`AudibleBounds`)**: Noise-floor-aware detection of `audibleStart`, `fullBodyStart`, `tailStart`, `audibleEnd`, `hasFadeIn`, `hasFadeOut`, and `hasReverbTail` so transitions never overlap dead air or cut early into an active reverb tail.
- **Time-Local Frames, Bars, Phrases & Cues**:
  - 3-band spectral energy shares (`lowShare`, `midShare`, `highShare`), bass dominance, onset density, and spectral novelty.
  - Bar-level (`BarFeatureSummary`) and phrase-level (`PhraseSpan`) summaries aligned to BG Beat's beat/downbeat grid.
  - Ranked `CuePointCandidate` lists for `MixOut` and `MixIn` roles.
- **Independent `.bgf` Binary Cache (`MixFeatureCache`)**:
  - Keyed by `<contentHash>-<analysisConfigHash>-<featureConfigHash>.bgf` with full schema, extractor, analyzer, vocal model, and harmonic analyzer version validation plus payload FNV-1a checksums.

### 2. Vocal Activity Detection (`VocalDetector`)
- **Headers / Sources**: `include/bgmedia/mix/VocalDetector.hpp`, `src/mix/VocalDetector.cpp`
- **Pluggable `IVocalActivityDetector`**:
  - `MusicVocalDetector` (`VocalDetectorKind::MusicVocalModel`): Harmonic comb pitch salience (`110..950 Hz`) requiring multi-harmonic evidence, vocal formant concentration (`F1/F2/F3`), vibrato/pitch-Contour modulation (`4..8 Hz`), stereo Mid/Side center focus, and percussive transient rejection.
  - `ExternalStemVocalDetector` (`VocalDetectorKind::ExternalStem`): Computes direct vocal-to-accompaniment ratio when a pre-separated vocal stem is available.
  - Honest fallback (`VocalDetectorKind::None` / `SpectralProxy`) with explicit confidence weighting so heuristic proxies are never misrepresented as high-confidence vocal detections.
- **Overlap Vocal Evaluator (`evaluateVocalOverlap`)**:
  - Evaluates simultaneous vocal collision (`vocalClashScore`), vocal-over-instrumental bed support (`vocalOverInstrumentalScore`), mid-phrase vocal cutoffs (`cutsMidVocalPhraseA`, `entersMidVocalPhraseB`), and `cleanVocalHandoff` weighted by actual playback gain and Mid-EQ curves.

### 3. Harmonic & Spectral Compatibility (`HarmonicAnalyzer`)
- **Headers / Sources**: `include/bgmedia/mix/HarmonicAnalyzer.hpp`, `src/mix/HarmonicAnalyzer.cpp`
- **12-Bin Pitch-Class Chroma & Key Estimation**:
  - Log-frequency harmonic-summing pitch-class profiler (`C..B`), Krumhansl-Kessler / Temperley major/minor correlation with Camelot notation (`1A..12B`) as a presentation helper, and `tonalness` (`0..1`) so percussive/drum-heavy material is treated as harmonically neutral (`percussiveNeutral`).
  - Covers both phrase spans and leading/trailing non-phrase boundary regions with local harmonic segments.
- **Pitch-Shift-Aware Overlap Evaluation (`evaluateHarmonicCompatibility`)**:
  - Continuous fractional-semitone chroma rotation when `pitchLock == false` (`12 * log2(rate)`), quarter-tone detune penalty (`detunePenalty`), simultaneous audibility weighting, and mid/high spectral crowding (`spectralCrowdScore`).

### 4. Multi-Candidate Smart AutoMix Planner (`MixPlanner`)
- **Headers / Sources**: `include/bgmedia/mix/MixPlanner.hpp`, `include/bgmedia/mix/MixTypes.hpp`, `src/mix/MixPlanner.cpp`, `src/mix/MixTypes.cpp`
- **Multi-Objective Candidate Search (`planCandidates`, `MixCandidate`, `MixCandidateScore`)**:
  - Evaluates candidate `(Mix-Out, Mix-In, duration, strategy)` combinations across phrase boundary strength, beat/phase alignment, vocal safety, harmonic compatibility, energy continuity, LUFS loudness balance, and bar-aligned duration fit.
  - Surfaces the winning plan alongside ranked `alternatives` (`#1..#6`) and a transparent 7-component `scoreBreakdown`.
- **Musical Transition Strategies (`MixStrategyKind`)**:
  - `SmoothBlend`: Equal-power crossfade with adaptive `bassSwapProgress` (`0.25..0.65`) and optional dynamic `vocalEqCarve`.
  - `VocalHandoff`: Preserves outgoing vocal clarity in the first half while bringing in the incoming instrumental bed, then executes a clean vocal handover.
  - `DropImpactCut`: Builds incoming tension toward the impact point and drops the outgoing deck sharply on the accent.
  - `EnergyBridge`: Lifts the mid-transition plateau across low-energy/breakdown sections to prevent energy holes.
  - `EchoReverbOut`: Fades the dry outgoing deck early while feeding a tempo-synced stereo echo/reverb wash-out tail.
- **Loudness Matching**:
  - Computes per-deck BS.1770-4 LUFS trims (`loudnessTrimDbA`, `loudnessTrimDbB`, `loudnessGainA`, `loudnessGainB`) with `-0.5 dBFS` sample-peak headroom protection.

### 5. Real-Time Engine Execution (`MixEngine`)
- **Headers / Sources**: `include/bgmedia/mix/MixEngine.hpp`, `src/mix/MixEngine.cpp`
- **Strategy Gain Envelopes (`planGain`)**: Executes piecewise strategy gain curves (`gainOut`/`gainIn`) and smoothly reverse-fades from the live strategy level on `cancelTransition()`.
- **Click-Free Loudness Normalization**: Smoothly interpolates `loudnessGain` across transitions and preserves the incoming deck's matched gain after handover.
- **Real-Time Stereo Echo / Reverb Tail (`processTailEcho`)**: Pre-allocated stereo ping-pong delay (`0.50` / `0.75` beat taps) with 22% cross-channel diffusion and 180 Hz high-pass / 4.2 kHz low-pass feedback damping, 100% lock-free and allocation-free on the audio thread.
