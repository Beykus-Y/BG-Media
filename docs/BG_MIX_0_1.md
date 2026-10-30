# BG Mix 0.1

BG Mix is the first working dual-deck mixer in BG Media. It is deliberately a
0.1: the mixing engine, queue, planning and UI are real and tested, while
several DJ techniques are explicitly marked as not implemented.

## Components

| Layer | Type | Responsibility |
| --- | --- | --- |
| `bgmedia::mix::PcmTrack` | core, UI-free | Prepared interleaved mono/stereo float PCM at the mixer rate. Decoded and resampled once on a worker. |
| `bgmedia::mix::MixEngine` | core, UI-free | Real-time render callback, two decks, equal-power crossfade, correlation headroom, limiter, tempo automation, atomic snapshots. No SDL, miniaudio, locks, allocation or file I/O in `render()`. |
| `bgmedia::mix::MixPlanner` | core, UI-free | Deterministic transition plan from two BG Beat analyses, with reasons and explicit fallbacks. |
| `bgmedia::mix::MixSession` | core, UI-free | Queue, background preparation, deck promotion, automatic/manual transitions. Hosts pump `update()` once per frame. |
| `bgmedia::MixEngine` adapters | `adapters/miniaudio` | `MiniaudioMixLoader` decodes + analyses + converts; `MiniaudioMixDevice` opens the output and reports latency/load/overruns. |
| BG Mix screen | `apps/desktop` | Adaptive canvas layout, queue, decks, waveforms, plan preview, controls, diagnostics. |

The public mixer headers contain no SDL or miniaudio includes. The desktop app
is a host; the engine can run in another host or be tested headlessly.

## Threading and real-time contract

* The preparation worker decodes, runs BG Beat and converts to the output rate.
* The UI thread calls `MixEngine` control methods and `MixSession::update()`.
* One audio callback calls `MixEngine::render()`. The callback swaps no
  `shared_ptr`s: decks are loaded only while idle and not playing, and the
  engine publishes a double-buffered snapshot with an atomic try-lock.
* `MixSession::update()` must be pumped every UI frame. When no output device
  exists (headless capture, device failure), the desktop pumps the engine
  silently so the clock and snapshots still advance.

## Master deck and musical handover

`MixSnapshot::master` is the active musical reference. BG Motion (and any other
view that follows the mix) must read it every frame:

1. Idle: the master is the deck being played (A after the first queue item).
2. During a transition: the outgoing deck stays master until the crossfade
   reaches `handoverProgress` (0.5 by default), then the engine switches
   `master` to the incoming deck at a render-block boundary. The incoming deck
   is already audible and beat-aligned at that point, so the visual beat clock
   does not jump.
3. After the transition: the incoming deck remains master; the outgoing deck is
   cleared and becomes the preparation target.

Use `DeckStatus::bpm` (analysed BPM multiplied by the live rate) and the deck
position for choreography, not the raw analysed BPM, so a Tempo Ramp keeps the
visual pulse honest.

## Transition types

| Type | Status | Behaviour |
| --- | --- | --- |
| Crossfade | working | Equal-power gains, short-term correlation headroom, smoothed output limiter. |
| Smart Crossfade | working | Chooses length from BG Beat sections/confidence, snaps A's start and B's entry to the local grid, shortens if either track runs out. |
| Beatmatched | working | Starts both decks on their local grid; falls back when the grid is missing or low-confidence. |
| Tempo Ramp | experimental | Phase-preserving rate curve on the outgoing deck; linear resampling, so pitch changes by design. |
| Direct Cut | working | Zero-length handover; the incoming deck starts immediately. |
| Phrase-aware, Dual Tempo Match, Breakdown Bridge | not implemented | The planner returns a Crossfade and says why. |

### Tempo Ramp

* The rate curve is continuous, starts at 1.0, ends at `bpmB / localBpmA`, and
  its integral over the ramp equals the target ratio. This is what preserves
  musical phase: the outgoing deck arrives at B's tempo with the same beat
  count, including slowdowns.
* The curve reaches a small plateau above the target and settles back
  (`+7.3%` of the delta at the plateau). A curve from 1.0 straight to the target
  would only advance half the required beats.
* Maximum delta, ramp length (bars), and on/off are configurable. The planner
  rejects a delta above the cap with an explicit reason.
* The engine applies the curve through `ITimeStretchProcessor`. The 0.1
  processor is a linear resampler, so pitch shifts with speed. WSOLA or a phase
  vocoder can replace it without changing the plan or the engine.

## Fallbacks and honesty

The planner never hides a fallback. `MixPlan::fallbackReason` is set when:

* BG Beat confidence is below `minConfidence` (0.45);
* the tempo is unstable or has a tempo change;
* the outgoing track is silent before the planned point;
* the meter/downbeat confidence is too low for a downbeat choice;
* a Tempo Ramp delta exceeds the cap, the tracks already share a tempo, the
  transition is too short, or Tempo Ramp is disabled;
* the remaining audio window is shorter than the requested fade (the duration
  is shortened and the reason is recorded);
* the type is not implemented.

The plan preview overlay draws the real gain/tempo curves and the same reasons;
it does not play audio.

## Display and high-DPI

The desktop reads the real drawable size (`SDL_GetRenderOutputSize`), not the
window points. It picks a UI scale from the display scale and the output size,
applies `SDL_SetRenderScale`, and lays out in a logical canvas
(`physical / scale`). Glyph atlases are baked at `size * scale` and drawn 1:1
into the framebuffer, so text stays crisp. Window minimum is 1280x860 logical
points, which keeps the adaptive layout valid at 100% and 200% DPI.

UI scale can be automatic or fixed at 100/125/150/175/200/225/250%. The choice,
window size, maximized/fullscreen state and 3D quality persist in
`desktop.ini` under `SDL_GetPrefPath("BGMedia", "BGMedia")`.

The VRM target follows the stage size at runtime, is capped at 3840x2160 for
readback, and is recreated only when the required size changes.

## Verification (2026-10-30)

* `bgmedia_tests mix.`: 15 tests, including equal-power curves, gapless
  transition, EOF/empty decks, pause/resume/cancel, seek during a transition,
  conflicting commands, Direct Cut, underrun-sized blocks, 44.1/48 kHz,
  mono/stereo/6-channel downmix, 92/123/163/180 BPM alignment, low-confidence
  and unstable-tempo fallbacks, short incoming track, Tempo Ramp phase/mean/
  slowdown/cap/disabled, render budget, session queue/transport.
* Full CTest: 24/24 suites.
* Real tracks (BG Beat + decoder, 48 kHz): Smart 123 -> 123, 8 s requested,
  shortened to 3.55 s because the incoming edit is 4 s. Peak 0.443, 0 clipped,
  0 non-finite, 0.1% of one core offline.
* Real Tempo Ramp 92 -> 100 BPM (9%): outgoing A advanced 8.694 s during an 8 s
  output transition (mean rate 1.0867), incoming B advanced exactly 8.000 s.
  Peak 0.793, 0 clipped, 0 non-finite, 0.1% of one core offline.
* Real device (`MiniaudioMixDevice`, WASAPI shared): 32.0 ms internal latency,
  0.06% callback load, 0 overruns, transition completed.

## Limitations

* Time-stretch is linear resampling: pitch follows tempo. The
  `ITimeStretchProcessor` seam is ready for WSOLA but no WSOLA/vocoder exists.
* No EQ, bass swap, cue monitoring, loop, hot cue, recording or key lock.
* Phrase-aware, dual-tempo matching and breakdown bridging are not implemented.
* The queue is a preparation queue with one automatic next track, not a full
  library or playlist manager.
* Crossfade headroom is correlation-based; it reduces level slightly when the
  decks are correlated, it does not perform spectral unmasking.
* BG Beat quality bounds the planner. Low-confidence analyses deliberately fall
  back instead of guessing.
