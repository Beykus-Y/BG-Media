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
| `bgmedia::MixEngine` adapters | `adapters/miniaudio` | `MiniaudioMixLoader` decodes + analyses + converts; `MiniaudioMixDevice` opens the output, reports latency/load/overruns, and validates the samples its callback produced. |
| BG Mix screen | `apps/desktop` | Adaptive canvas layout, queue, decks, waveforms, plan preview, controls, diagnostics. |

The public mixer headers contain no SDL or miniaudio includes. The desktop app
is a host; the engine can run in another host or be tested headlessly.

## Threading and real-time contract

* The preparation worker decodes, runs BG Beat and converts to the output rate.
* The UI thread calls `MixEngine` control methods and `MixSession::update()`.
* One audio callback calls `MixEngine::render()`. The callback swaps no
  `shared_ptr`s: decks are loaded only while idle and not playing, and the
  engine publishes a double-buffered snapshot with an atomic try-lock.
* A transition plan is control-thread-owned. `startTransition()` refuses to
  overwrite `pendingPlan_` while `planReady_` is set; the callback reads the
  payload under that release/acquire flag, clears the flag after its last read,
  and never writes the payload (including on Stop). This is what lets Stop and
  a subsequent `startTransition()` run concurrently with the callback.
* Track and time-stretch buffers replaced by `loadDeck()`, `clearDeck()` or
  `setTimeStretch()` are not freed on the control thread. They go to a retire
  list and are reclaimed by the control thread only after a render epoch shows
  that at least one callback started after the swap (`reclaimRetired()`).
  `MixSession::update()` pumps this reclamation once per frame.
* `loadDeck()`/`clearDeck()` publish a reset serial instead of writing playback
  state. The callback owns `positionFrames`, `playing`, fade/gain/rate and
  applies a pending seek or reset in a fixed order: hard stop, reset requests,
  pending seeks, plan consumption, cancellation, empty-deck cleanup, pause
  freeze, then normal rendering. There is exactly one writer for live transport
  state.
* The seek payload is a `std::atomic<double>`: `seekSerial` announces a new
  request, but a later `seek()` may overwrite the value while the callback is
  still reading the preceding serial, so the payload itself cannot be a plain
  double guarded only by the serial's release/acquire pair.
* `MixSession::update()` must be pumped every UI frame. When no output device
  exists (headless capture, device failure), the desktop pumps the engine
  silently so the clock and snapshots still advance.

## Transport and failure semantics

* **Stop** is a hard stop: it freezes the transport, clears both play requests
  and drops an active or pending transition. A pause freeze alone is not used,
  because the callback must first settle the deck `playing` flags.
* **Manual transitions** plan from the outgoing deck's current position. Only
  the preview uses the end-of-track window, so pressing Next no longer seeks
  the playing deck.
* **Refilling** the deck that just handed over loads it stopped. It does not
  start at full independent gain next to the new master.
* **EOF before handover** promotes an already prepared deck instead of ignoring
  it because it was not playing yet, and clears the finished deck.
* **Cancel after handover** restores `master` to the outgoing deck and resumes
  it, so the next transition does not start from silence.
* **Pause** ramps an 8 ms envelope per output sample and freezes the transport
  only after the ramp has been rendered. A callback block is typically longer
  than 8 ms (480 frames at 48 kHz), so the ramp cannot be applied per block
  without stepping from full level to silence.
* **EOF** applies its de-click fade over the last playable source sample, not
  over the rest of the callback block, so the final sample approaches silence
  instead of stepping from full amplitude to zero.
* **Direct Cut** is a valid zero-duration plan. An executed cut happens at the
  current position; a preview reports the end of the planned musical window, so
  automatic scheduling cuts before trailing silence. The incoming deck starts
  on its planned entry marker rather than at zero.

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
into the framebuffer, so text stays crisp. The atlas cache is keyed by the baked
pixel size and weight only; logical metrics are derived per frame from the
current scale (`inverseScale = 1 / scale`). Keying by scale instead would reuse
one atlas for 10 px at 100% and 8 px at 125% with the wrong metrics, while adding
the scale to the key retained a texture for every step of a window resize; the
bounded key plus per-frame inverse scale fixes both.

The canvas is clamped to at least 1100x900 logical points; the BG Beat / BG
Motion cards need ~900 points of height, and the BG Mix three-column layout
needs the width. A requested scale that would go below that canvas is reduced,
and the UI scale chip reports the effective value (for example
`UI 200% / 120%` on a 1920x1080 window). A window shorter than 900 points is
scaled slightly below 100% instead of clipping its bottom card row. The
supported window minimum is 1280x860 points.

UI scale can be automatic or fixed at 100/125/150/175/200/225/250%. The choice,
windowed size, maximized/fullscreen state and 3D quality persist in
`desktop.ini` under `SDL_GetPrefPath("BGMedia", "BGMedia")`. While fullscreen,
the saved window geometry is the windowed size, so leaving fullscreen restores
the previous window instead of a screen-sized one.

Fullscreen is SDL borderless fullscreen. `F`, `F11`, `Alt+Enter` toggle it,
`Esc` leaves it (before the screen-specific Esc action). Enter/leave events
mirror the OS state; if `SDL_SetWindowFullscreen` fails the reason is shown in a
toast and the window stays windowed. The capture path fills the display: a
2560x1440 fullscreen capture is 2560x1440, with content laid out across the whole
frame. A first run without saved geometry starts maximized.

The VRM target follows the stage size at runtime, is capped at 3840x2160 for
readback, and is recreated only when the required size changes.

## Review fixes (PR #32)

Each finding from the first review has a reproducing test:

| Finding | Fix | Regression test |
| --- | --- | --- |
| Outgoing deck refilled and auto-played | Refill loads stopped; playing is opt-in | `mix.session_refilled_deck_stays_stopped` |
| Manual Next seeked the playing deck | Execution plans from the current position | `mix.session_next_starts_at_current_position` |
| Stop left decks "playing" / Clear Queue stuck | `MixEngine::stop()` settles state in the callback | `mix.session_stop_and_clear_queue_recover` |
| Heap use-after-free on deck replacement | Deferred retirement by render epoch | `mix.engine_retired_track_lifetime` |
| Cancel after handover left a stopped master | Restore `master` to the outgoing deck | `mix.engine_cancel_after_handover_restores_master` |
| EOF ignored a prepared incoming deck | Promote the prepared deck and clear the ended one | `mix.session_eof_promotes_prepared_deck` |
| Glyph cache key ignored UI scale | Atlas keyed by baked pixels; per-frame inverse scale | `ui.font_cache_key_is_bounded_across_scales` |
| Click at the end of a track | Fade anchored to the last playable sample | `mix.engine_eof_declick_has_no_hard_edge` |
| Load-time seek overrode the transition plan | Reset requests run before plan consumption | `mix.engine_load_seek_does_not_override_plan` |
| Controls overflowed the screen at large scale | Canvas clamp + responsive columns | `ui.effective_scale_keeps_canvas_usable`, `ui.mix_columns_never_overflow` |

The second review found five more issues, fixed in the same way:

| Finding | Fix | Regression test |
| --- | --- | --- |
| `seekSeconds` was written and read without atomics (P1) | Atomic payload, serial still announces requests | `mix.engine_concurrent_controls_stress` (ASan) |
| Cancel re-armed the pending Next request | Clear `nextRequested_` on submission and cancel | `mix.session_cancel_does_not_restart_next` |
| Last 50 ms treated as EOF, promotion while playing | Promote only on `DeckStatus::ended` | `mix.session_promotes_only_at_actual_eof` |
| Automatic fade scheduled from the file end | Start at `preview.transitionStartA - 0.35 s` | `mix.session_auto_transition_uses_planned_window` |
| Atlas cache grew with every UI scale | Bounded key + per-frame inverse scale | `ui.font_cache_key_is_bounded_across_scales` |

The third review found four more issues:

| Finding | Fix | Regression test |
| --- | --- | --- |
| Pause muted a whole 480-frame block instead of ramping (P2) | Per-sample 8 ms envelope, freeze after the ramp | `mix.engine_pause_fade_is_per_sample` |
| Zero-duration Direct Cut never scheduled automatically (P2) | Preview cut point is the window end; zero-duration plans are submitted | `mix.session_auto_direct_cut_uses_marker` |
| Stop and startTransition raced on `pendingPlan_` (P1) | Payload stays control-thread-owned; the callback only clears `planReady_` | `mix.engine_concurrent_controls_stress` (ASan) |
| Real-device stress never measured its samples (P2) | Device callback counts non-finite samples and peak | `bgmedia_mix_qa --stress --device` now reports `samples measured` |

`mix.engine_concurrent_controls_stress` runs a real render thread against a
control thread hammering play/pause/seek/load/clear/next/cancel/stop, and is
meant to be run under AddressSanitizer.

## Verification (2026-10-30)

* `bgmedia_tests mix.`: 29 tests, including equal-power curves, gapless
  transition, EOF/empty decks, pause/resume/cancel, seek during a transition,
  conflicting commands, Direct Cut and automatic direct-cut markers, underrun-
  sized blocks, 44.1/48 kHz, mono/stereo/6-channel downmix, 92/123/163/180 BPM
  alignment, low-confidence and unstable-tempo fallbacks, short incoming track,
  Tempo Ramp phase/mean/slowdown/cap/disabled, render budget, session
  queue/transport, the regressions above, and the concurrent stress test.
* `bgmedia_tests ui.`: 3 tests for scale clamping, column overflow and the
  bounded font cache.
* Full CTest: 25/25 suites. Windows Release test binary: 358 passed, 0 failed.
* AddressSanitizer (MSVC `/fsanitize=address`, RelWithDebInfo): full test
  binary 357 passed, 0 failed, no ASan report, including the concurrent stress
  test and the deferred-reclamation lifetime test.
* Stress, emulated callback thread, real prepared tracks, 2 s: 344 control
  operations (68 loads, 34 transitions, 34 cancels), 573,687 render blocks,
  peak 0.749, 0 non-finite.
* Stress, real WASAPI device (`Наушники (fifine SC9)`), 3 s: 1,600 control
  operations (207 loads, 159 transitions, 160 cancels), 316,416 samples
  validated by the device callback, 32.0 ms latency, 0.03% callback load,
  0 overruns, 0 non-finite.
* Real tracks (BG Beat + decoder, 48 kHz): Smart 123 -> 123, 8 s requested,
  shortened to 3.55 s because the incoming edit is 4 s. Peak 0.443, 0 clipped,
  0 non-finite, 0.1% of one core offline.
* Real Tempo Ramp 92 -> 100 BPM (9%): outgoing A advanced 8.694 s during an 8 s
  output transition (mean rate 1.0867), incoming B advanced exactly 8.000 s.
  Peak 0.793, 0 clipped, 0 non-finite, 0.1% of one core offline.
* Display checks (captures): BG Mix and BG Beat at 1920x1080 and 2560x1440 with
  UI scale 100/125/137/150/175/200%. Content fills the frame, the Mix queue,
  both decks, plan and transport stay on-screen, and the BG Beat bottom card
  row is above the footer. At 1280x860 the canvas falls to 96% and still fits.
  A borderless-fullscreen capture is 2560x1440 with the layout across the whole
  display.

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
