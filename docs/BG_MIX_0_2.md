# BG Mix 0.2 — musical transitions

Status: **implemented, tested, not merged**. 0.2 supersedes the 0.1 transition
engine and keeps the same public entry points (`MixEngine`, `MixSession`,
`MixPlanner`, `MixTypes`) so the desktop app and future hosts do not need a
rewrite.

0.2 is about *musical* transitions rather than longer fades:

* the outgoing deck reaches the incoming tempo **before** the overlay starts;
* tempo changes preserve pitch through a real streaming WSOLA stretcher;
* beats and downbeats are matched, not just faded;
* the planner can choose a phrase boundary, a bass swap and an adaptive type,
  and always explains a fallback;
* a two-stage engine renders the pre-roll and the crossfade without starting the
  incoming deck early;
* BG Motion can follow the live master tempo through a small adapter, without
  changing BG Motion 3.x.

---

## 1. Components

| Component | Where | Role |
| --- | --- | --- |
| `ITimeStretchStream` / `WsolaStretcher` | `include/bgmedia/mix/ITimeStretchProcessor.hpp`, `src/mix/TimeStretch.cpp` | Per-deck streaming time-stretch. Real-time safe after construction. |
| `LinearResampleStretcher` | same | Explicit performance mode. Pitch follows tempo. |
| `ThreeBandEq` | `include/bgmedia/mix/SpectralEq.hpp`, `src/mix/SpectralEq.cpp` | Complementary low/mid/high split with per-sample gain automation. Unity is a true bypass. |
| `MixPlanner` | `src/mix/MixPlanner.cpp` | Deterministic plan: pre-roll curve, beat/phase cue, phrase cue, EQ curves, adaptive choice, reasons and fallbacks. |
| `MixEngine` | `src/mix/MixEngine.cpp` | Two-stage render state machine, EQ automation, per-deck streams, cancel glide, snapshot/stage publication. |
| `MixSession` | `src/mix/MixSession.cpp` | Settings, queue, scheduling from the overall start, safe quality switching. |
| `MixSync` | `include/bgmedia/mix/MixSync.hpp`, `src/mix/MixSync.cpp` | Maps the live snapshot to master/tempo/phase for BG Motion and diagnostics. |

The DSP core still has no SDL, miniaudio, locks, allocation or file I/O inside
`render()`. Streams are created on the control thread and retired with the same
render-epoch protocol as PCM buffers.

---

## 2. Time-stretch: what 0.2 actually does

### 2.1 WSOLA, not disguised resampling

`WsolaStream` overlaps Hann windows and chooses each synthesis position with a
normalised cross-correlation search on the mid signal. Windows are **never
resampled**, so pitch is preserved. The window is 42 ms (2016 frames at 48 kHz,
clamped to 1024–4096), the hop is half a window, and the search range is
window/10. Sharp transients bypass the search so drum attacks are not smeared.
Stereo uses one shared offset, so the image does not wander.

`latencyFrames()` reports window/2 (≈21 ms at 48 kHz) as algorithmic
look-ahead. The engine's EOF de-click accounts for it.

The one-shot `WsolaStretcher::render()` remains for 0.1 hosts; the engine always
uses `createStream()` so overlap-add history survives callback boundaries.

### 2.2 Quality / latency / CPU / license

| Property | Value |
| --- | --- |
| Pitch at 0.90×, 0.95×, 1.05×, 1.10× | 440.00 Hz measured at all four ratios |
| Largest sample step at 1.10× | 0.0115 (a click would be near full scale) |
| Transient spacing | 17 intervals within ±30 ms of 250 ms |
| Algorithmic latency | ≈21 ms at 48 kHz (window/2) |
| CPU (synthetic 10 s render test) | ≈44 ms, ≈228× real time |
| CPU (real fixtures, QA harness) | 15.5 s of audio in ≈0.06 s, ≈0.4% of one core |
| License | Original implementation in this repository; no third-party DSP code |

These are measurements, not a claim of audible perfection. A mastering-grade
phase vocoder was deliberately not used: WSOLA keeps transients better at the
small tempo deltas a DJ transition needs, with lower latency and no STFT
allocation.

The performance mode (`LinearResampleStretcher`) is retained as an explicit
choice. It is faster and changes pitch with speed; the planner sets
`pitchShiftWarning` and the UI labels it.

---

## 3. Plan semantics

### 3.1 Pre-roll and crossfade

`MixPlan` keeps 0.1 fields and adds:

* `preTransitionSeconds` — real seconds of tempo ramp before the overlay;
* `tempoRampStartA` — outgoing media time where the ramp starts;
* `transitionStartA` / `duration` — the crossfade cue and length, unchanged;
* `overallStartA()` — `tempoRampStartA` when present, otherwise the cue.

Tempo-curve times are relative to the **crossfade start**, so pre-roll points are
negative. This is what lets the engine use one curve for both stages.

For a ramp over `N` bars at the outgoing tempo `bpmA`:

```
media consumed   = N * beatsPerBar * 60 / bpmA
mean rate        = (1 + ratioA) / 2
ramp real time   = media consumed / mean rate
transitionStartA = overallStartA + media consumed
```

The planner emits 65 points from `-rampSeconds` to 0 with the selected shape
(Linear / Smooth / S-curve), then holds `ratioA` through the fade. The first
rate is exactly 1.0 and the last is exactly `ratioA`, so there is no rate step
at either boundary. The numeric integral of the curve equals the media consumed;
the tests check both.

If the full ramp plus the fade does not fit after the current position, the
planner shortens the ramp in whole bars. If even two bars do not fit, it drops
the ramp and says why. It never collapses the crossfade to a few milliseconds
just to keep a ramp.

### 3.2 Tempo and phase matching

`matchTempo()` compares the incoming BPM with the outgoing local BPM. It accepts
the direct ratio when it is inside `maxTempoDeltaPercent`. When the incoming
analysis is octave/metric-ambiguous it also considers `bpmCandidates`, but only
with an incoming ratio in `[0.75, 1.33]`, so a candidate cannot turn into a
2× chipmunk or half-speed drop. `tempoRatioB` is reported and the engine applies
it to the incoming deck.

The outgoing cue is snapped to a beat, and to a downbeat when meter/downbeat
confidence is high. The incoming cue is snapped the same way. Because the ramp
covers an integer number of outgoing bars, the overlay starts on a beat of both
outputs; `beatPhaseErrorMs` is 0 when both cues snapped and half a beat when the
grid was not usable.

Meters are not assumed to be 4/4. `beatsPerBar` from BG Beat drives bar length
and phrase periodicity, so 3/4, 6/8, 7/8 and 12/8 all work as long as BG Beat
reports the meter. Low meter/downbeat confidence degrades to beats only, then to
a plain crossfade, with the reason in `fallbackReason`.

### 3.3 Phrase-aware cue

`PhraseAware` looks for a downbeat whose `barIndex % phraseBars == 0` near the
incoming intro cue. The score combines:

* phrase periodicity (the bar-index modulo);
* section novelty (a section starts at that time);
* section energy;
* beat strength/confidence;
* bass-accent strength as a percussion/low-band cue.

The chosen cue sets `phraseBoundary`, `phraseBars` and `phraseConfidence`. If no
phrase boundary is in range, the planner uses the nearest beat/downbeat and
reports the lower confidence instead of pretending the cue was phrase-aligned.

### 3.4 Bass swap and EQ

When enabled and the fade is at least 2 s, the planner writes complementary
low-band curves:

* outgoing low leaves over the first 40% of the fade;
* incoming low enters from 25% to 70%;
* mid and high stay at unity.

`ThreeBandEq` uses a one-pole low split at 160 Hz and a one-pole high split at
3.2 kHz. `low + (mid + high)` reconstructs the input exactly, so equal gains are
a mathematical bypass. Gain changes are interpolated per sample and coefficients
are derived from the actual sample rate. There is no resonant bump and no level
step when automation starts or ends.

### 3.5 Adaptive choice and fallbacks

`SmartCrossfade` with `PlannerConfig::adaptive` is the "decide for me" mode:

| Material | Choice |
| --- | --- |
| confident grid, compatible tempo, phrase boundary available | Phrase-aware (with ramp when the tempo differs) |
| confident grid, compatible tempo, no phrase evidence | Beatmatched (with ramp when needed) |
| confident grid, tempo outside the cap | Smart Crossfade with bass swap; `fallbackReason` names the BPM difference |
| unstable tempo | Crossfade; reason says the tempo is not stable |
| low confidence or missing grid | Crossfade; reason names the confidence threshold |

Explicit types keep their identity. A requested `TempoRamp` that cannot be used
becomes a Crossfade with a specific reason (disabled, low confidence, unstable
tempo, same tempo, delta cap, not enough room). A requested `Beatmatched` that
cannot sync becomes a Crossfade and says whether the grid or the tempo was the
problem. `DualTempoMatch` and `BreakdownBridge` remain explicitly
`not implemented`; the planner never presents them as working.

---

## 4. Engine state machine

Transport phases stay `Idle / Running / Cancelling`; the finer stage is
published for the UI and BG Sync:

`Idle → Preparing → TempoRamp → BeatAlign → Mixing → Handover → Complete`
and `Cancelled` after a reverse fade.

1. `startTransition()` copies the plan and publishes it.
2. At the next block boundary the engine computes
   `crossfadeStartFrame = now + preTransitionSeconds` and
   `endFrame = crossfadeStartFrame + duration`.
3. The outgoing deck is seeked to `overallStartA()` and plays the pre-roll at
   the curve rate. The incoming deck is loaded at `startB` but **stays stopped**.
4. Chunks never straddle the boundary. On the exact boundary frame the incoming
   deck is activated at `startB`, its stream is reset, and equal-power gains
   begin.
5. The incoming deck holds `tempoRatioB` through the handover, then returns to
   natural speed across the tail of the fade, so the end of the transition has
   no rate step.
6. `finishTransition()` stops the outgoing deck and moves `master` to the
   incoming deck. `cancelTransition()` reverse-fades over 220 ms, glides the
   outgoing rate back to 1.0, returns the EQ to unity, and never starts the
   incoming deck if the cancel happened during pre-roll.

Pause, resume, seek, next, stop, cancel, deck load/clear, decode failure and EOF
are covered by the existing serial-based control protocol; the pre-roll adds no
new shared state. A PCM shortage or EOF still fades to silence; the incoming
deck is activated only at the boundary.

Settings changes are safe mid-transition: a new plan is built for the next
transition, while the running plan is a copy. Changing pitch lock or stretch
quality rebuilds the streams only when the engine is idle; otherwise it is
deferred to the next `update()`.

---

## 5. BG Sync and BG Motion

`MixSync.hpp` is the adapter. `liveMixTempo(snapshot)` returns the master deck,
its audible position (latency-compensated), live BPM (`analysedBpm * rate`),
beat period and fractional phase. During a transition `master` moves at the
handover, so a consumer automatically follows the audible reference.

The desktop app uses it when no video track is playing: it installs a
`BeatClock` through the existing `DancePlayer::setGrid()` API. At natural speed
it uses the analysed grid (downbeats and accents); during a tempo ramp it uses a
constant clock at the live tempo, anchored on the current phase. BG Motion 3.x
is not modified.

---

## 6. Settings

| Setting | Values | Default |
| --- | --- | --- |
| Mode | Auto / Manual / Preview | Auto |
| Type | Off, Crossfade, Smart, Beatmatched, Tempo Ramp, Phrase-aware, Direct Cut | Smart |
| Duration | Auto or 2–16 s | Auto |
| Tempo ramp | Off / Auto / Manual | Auto |
| Ramp length | Auto / 4 / 8 / 16 bars | Auto |
| Curve | Linear / Smooth / S-Curve | S-Curve |
| Max delta | 1–16% | 6% |
| Pitch lock | on / off | on |
| Stretch quality | Quality (WSOLA) / Fast (linear) | Quality |
| Bass swap | on / off | on |
| Phrase length | 2–16 bars | 4 |
| Auto queue | on / off | on |

Auto schedules transitions from `overallStartA()` so the ramp starts in time.
Manual and Preview never auto-start; Preview still builds the full plan for the
timeline. The UI exposes all of the above and shows a Transition Timeline
(pre-roll, crossfade, handover, live playhead) and a Live Transition Monitor
(stage, both live rates, low-band gains, phase error, pitch lock, bass swap).

High-DPI captures are produced at 1920×1080 and 2560×1440 from the same logical
layout; the renderer scales the logical canvas, so text and cards stay sharp.

---

## 7. Verification

### 7.1 Automated tests

`ctest --test-dir build/windows` — 25/25 pass. `bgmedia.mix` contains 39 tests,
including:

* equal-power and correlation headroom;
* gapless transition, EOF de-click, pause envelope, retired-buffer lifetime;
* WSOLA pitch at 0.90/0.95/1.05/1.10, sample-step click check, transient spacing;
* pre-roll before crossfade: incoming deck stays stopped, activates on the
  boundary, A media equals the curve integral;
* cancel during pre-roll: master returns to A, rate returns to 1.0, B never
  starts;
* bass-swap EQ monitor and three-band response/bypass/click checks;
* planner ramp start/end rates, curve smoothness, media integral, slow-down,
  cap/disabled/unstable/low-confidence fallbacks;
* phrase-aware cue and the adaptive compatible/incompatible/unstable cases;
* live-tempo adapter and master handover;
* render budget (10 s of audio in ≈44 ms) and the real-time control stress.

### 7.2 QA harness

`bgmedia_mix_qa` prepares real fixtures through the same BG Beat + decoder path
as the desktop app, renders the planned pre-roll + crossfade, and exports A/B
WAVs:

```
bgmedia_mix_qa --a artifacts/fixtures/beat_123.wav \
               --b artifacts/fixtures/beat_163.wav \
               --type tempo --ramp 10 --duration 8 \
               --export artifacts/bg_mix_0_2 --name ramp_123_163
```

It writes `<name>_mix.wav` (the plan) and `<name>_baseline.wav` (the same window
without the ramp, bass swap or adaptive choice). Generated WAVs live under
`artifacts/`, which is git-ignored: third-party music is never committed.

Representative run (real fixtures, 123.2 → 162.8 BPM, octave candidate at
130.4 BPM, 5.9% delta):

* plan: Tempo Ramp, 7 bars of pre-roll (13.25 s) + 7.94 s fade;
* peak 0.786, 0 clipped samples, 0 non-finite samples;
* 21.2 s of audio rendered in ≈0.08 s (≈0.4% of one core);
* baseline peak 0.787, so the planned mix is not louder;
* the same window with `--type phrase` selects the bar-aligned incoming cue at
  0.448 s and reports `phrase=1 (1.00)`;
* an incompatible pair (92 → 123 BPM, 33.8%) falls back to Smart Crossfade with
  a named reason, snaps the outgoing cue to the nearest beat (246 ms), applies
  the bass swap, and peaks at 0.769 against a 0.798 baseline;
* the harness reports the fixture's own short silent breaks as content silence;
  the baseline shows the same, so they are not engine dropouts.

### 7.3 What is not claimed

* No blind listening test was performed. The tests verify pitch, phase,
  continuity, levels and CPU, not subjective “sounds perfect”.
* WSOLA is not a mastering time-stretcher. Very large tempo changes are capped
  and fall back instead.
* The EQ is a minimum-phase complementary split, not linear phase.
* Beat alignment depends on BG Beat's grid. A wrong grid produces a wrong cue
  and a low-confidence fallback, not a hidden correction.
* The A/B files are local artifacts; this document does not publish third-party
  music.

---

## 8. Migration notes

* `PlannerConfig::allowTempoRamp` still exists; `tempoRampMode = Off` wins.
* `MixPlan::duration` is still the crossfade length, not the total. Use
  `totalDuration()` or `overallStartA()` when scheduling.
* `TempoRamp` is now `Working`; it is no longer experimental and no longer sets
  `pitchShiftWarning` when pitch lock is on.
* The default engine processor is WSOLA. `LinearResampleStretcher` is opt-in
  through `setTimeStretch()` / `MixSession::setQuality(StretchQuality::Performance)`.
* `TransitionStage` and the monitor fields are additive; existing snapshot
  consumers keep working.
