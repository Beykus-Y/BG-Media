# BG Motion 3.3 — natural-motion quality pass (issues #12–#14 follow-up)

Branch `fix/bg-motion-3.3-natural-motion-quality-pass`, based on `dev` `d472c3c`.
No asset was regenerated, no clip format changed, no threshold or global
smoothing was added. Soft IK and the thigh colliders stay off by default.

This document covers the audit of why the dances read as mechanical, the three
defects that were reproduced and fixed, the measurements before/after, and what
is still open. The full experiment log, CSVs, contact sheets and videos live in
`out/quality_pass_3_3/` (git-ignored).

## 1. What the audit measured

Instrumentation already in the repo (`bgmedia_motion_diag`, `bgmedia_qa_matrix`,
`bgmedia_mesh_clip`, `bgmedia_vrm_preview`) was run on both private models,
`models/Model1.vrm` and Nemesis Morrigan, 30/60/120 FPS, 8–40 s per clip.

### 1.1 Overlays, not choreography, produced the angular spikes

`bgmedia_motion_diag` on neonPulse / Model1 / 60 FPS:

| run | LHand max rad/s | Head max rad/s | LHand max m/s² | Head max rad/s² |
|---|---|---|---|---|
| overlay Full | **14.14** | **7.41** | 78.5 | 400 |
| overlay Off | 2.08 | 0.74 | 40.8 | 9 |

The same pattern in every clip: with overlays off the hand bone is essentially
static (0–3 rad/s), because no authored or generated clip rotates the wrist; the
7–16 rad/s hand rotation and the 7–9 rad/s head rotation came entirely from the
overlay primitives. A cue of 0.25–0.5 beat performed `HandFlick`'s 40° wrist
roll / `HeadAccent`'s 12–16° in 25–60 ms, i.e. a twitch at 800–900 °/s, scheduled
on a pseudo-random beat. That is the "резкие" (sharp) part of the complaint and
it is model independent (Model1 and Nemesis differ by <0.3 %).

Root cause: the primitives shaped themselves purely in phase; nothing knew how
long a cue really lasts, so a short cue got the full amplitude in less time.

### 1.2 A hard threshold in the procedural wrist plane

`Choreographer::bothArms` baked
`(elev > 80 ? 22 : 9) * (azim > 45 ? 1 : -1)` degrees of wrist roll. Crossing
either value between two keys left a 13° step in the hand track; the generator's
own summary reported 606 °/s and playback 14.1 rad/s on neonPulse.

### 1.3 The body pulse repeated identically every beat

In the generated clips every beat inside a section had exactly the same hip
depth. Section [8:16] of neonPulse measured
`−70.5 −70.5 −70.5 −70.5 −70.5 −70.5 −70.5 −70.5 mm` — standard deviation
0.00 mm. Accents existed in the clip's accent map but did not shape the body;
only the musical warp read them. That is what makes a loop read as a metronome
regardless of how smooth the interpolation is.

### 1.4 Arms held in one horizontal plane

`authorArms` used `baseElev + 34` (≈74° from straight down, ~16° below
horizontal) as the neutral/return pose of every motif and phrase, so the arms
spent most of the dance held out. Measured elevation of the upper arm (degrees
below horizontal, median): neonPulse left 34.5 → 54.0, streetCurrent 29.1 →
52.5, fluidSignal right 5.7 → 66.1 after the change below. The stills in
`out/quality_pass_3_3/visual/comparisons/*_ab.png` show the difference directly:
before, both arms are out like a scarecrow for seconds; after, they hang with a
bent elbow and travel out only for gestures.

### 1.5 What was *not* the cause

Measured and rejected as explanations, so the next pass does not repeat them:

* **Sampling / interpolation.** Clips are sampled with Catmull–Rom on
  quaternions (C1); the per-key speed of the hand track is 158 °/s at worst.
* **Authored accents are smooth.** The 7.6 m/s energetic right-hand peak is a
  12-frame velocity window (5.0 → 7.6 → 5.3 m/s), not a step. Not limited.
* **The torso is not a board.** Accumulated trunk rotation is 12–26° in
  hiphop/sidestep; the chest follows the hips by 0–0.25 beat. It is not rigid.
* **The retargeter and the collision proxy.** Disabling the arm proxy or the leg
  proxy leaves the neonPulse spike unchanged; mesh triangle counts do not move.
* **Stop-start keying.** Hands have no dead stops (1–5 % of frames below
  0.12 m/s) and peak rise/fall ratios are 0.5–1.35, not the 0/∞ of a paused
  pose-to-pose curve.
* **The renderer.** `bgmedia_skin_check` agreement and the D3D12 path were
  already verified in the 3.2 pass; nothing here pointed at it again.

## 2. Changes

### 2.1 `MotionPrimitive`: a gesture is performed smaller, not faster

`samplePrimitive` takes the cue's real duration (`cueSeconds`) and scales the
gesture amplitude by `clamp(cueSeconds / 0.55, 0.35, 1)`. The phase shape is
untouched, so the peak stays on the authored accent beat — only the size adapts
to the time available. `CompositionOptions::cueSecondsPerBeat` carries the clip
tempo; `DancePlayer` sets it per segment. `0` (the default) keeps the legacy
amplitude bit-identically, so existing callers and tests are unaffected.

### 2.2 `ProceduralChoreography`: continuous wrist plane

`wristRollDeg(elev, azim)` ramps 9°→22° through the 62–96° elevation band and
uses `tanh((azim − 45)/7)` for the direction. Both dependencies are continuous,
so no key can step the hand.

### 2.3 `ProceduralChoreography`: accent-shaped body pulse

`authorAccents()` now runs before `authorBody()`. Per beat: depth
`0.74 + 0.34 · accentStrength` of the section dip, and the release key moves from
0.52 to 0.68 beat, so strong beats land lower and recover later while the down
key stays exactly on the beat. The spine now answers 0.09 beat after the pelvis
instead of stopping with it. Section [8:16] of neonPulse measured
`−71.9 −65.4 −65.4 −65.4 −71.9 −65.4 −65.4 −65.4 mm` (std 2.8 mm) after.

### 2.4 `ProceduralChoreography`: neutral arm is a relaxed arm

Preset 0 (the return pose and the most common motif block) changed from
`{baseElev+34, 16, 12, 28}` to `{24, 30, 10, 46}`: arm low, elbow bent, hand in
front of the hip. The strong planes (overhead, diagonal, point) are unchanged
and still carry the accents.

### 2.5 Test harness: numerically stable pose distance

`2·acos(|dot|)` amplifies one float ULP near identity into 6.9e-4 rad, which made
`motion32.inertial_transition_is_continuous_and_seek_stable` fail at a 1e-6
tolerance on poses that agree to 1e-9. `quatAngle` now uses
`2·atan2(|xyz|, |w|)` of the relative rotation. The tolerance was not loosened.

## 3. Measurements before → after (60 FPS, overlay Full)

Same build flags, model, clip, FPS and duration; before = the `d472c3c` binaries
kept in `out/quality_pass_3_3/baseline_bin`. Model1 and Nemesis give the same
numbers because the overlay layer is model independent.

| clip | LHand rad/s | Head rad/s | RHand rad/s | LHand m/s² | Head m/s² |
|---|---|---|---|---|---|
| neonPulse | 14.14 → 7.42 (−47 %) | 7.41 → 3.03 (−59 %) | 11.94 → 6.55 (−45 %) | 78.5 → 113.7 (**+45 %**) | 45.3 → 35.8 (−21 %) |
| energetic | 15.99 → 6.93 (−57 %) | 9.11 → 3.19 (−65 %) | 8.79 → 4.07 (−54 %) | 87.5 → 61.2 (−30 %) | 54.0 → 37.2 (−31 %) |
| streetCurrent | 10.91 → 6.24 (−43 %) | 7.74 → 3.63 (−53 %) | 7.72 → 4.39 (−43 %) | 68.5 → 62.7 (−8 %) | 36.8 → 31.1 (−15 %) |
| sidestep | 7.36 → 3.77 (−49 %) | 7.53 → 3.03 (−60 %) | 4.07 → 4.07 | 32.5 → 29.3 | 29.0 → 22.5 (−22 %) |
| groove | 4.40 → 2.54 (−42 %) | 7.50 → 3.39 (−55 %) | 5.51 → 3.13 (−43 %) | 44.6 → 29.3 (−34 %) | 27.2 → 27.2 |
| fluidSignal | 5.95 → 5.26 (−12 %) | 7.54 → 2.93 (−61 %) | 4.32 → 4.22 | 71.4 → 64.0 (−10 %) | 34.3 → 25.2 (−27 %) |
| hiphop | 4.01 → 4.01 | 7.59 → 3.63 (−52 %) | 5.92 → 3.51 (−41 %) | 88.3 → 100.1 (**+13 %**) | 58.6 → 56.2 |

Median change: hand angular −42 %, head angular −57 %, hand linear acceleration
−9 %. Two regressions are disclosed rather than tuned away: hiphop's left-hand
linear acceleration (+13 %, from the longer travel of the new accent-shaped
release) and neonPulse's left hand (+45 %) — the left arm now sweeps from a
hanging neutral to the raised gesture, a longer path over the same beats. Both
stay inside the range the existing clips already use (energetic 87–146 m/s²) and
the two clips' angular speeds still fell by 47–57 %.

Mesh level (`bgmedia_mesh_clip`, Model1, 40 s, arm groups): neonPulse and
streetCurrent keep **0** crossing frames; fluidSignal keeps its pre-existing
4 frames (6 → 8 triangles of 400), no new penetration. Leg crossings are
unchanged (`bgmedia_mesh_clip --legs`, see below).

Musical sync is unchanged: `bgmedia_sync_qa` against `docs/sync_qa_3_1.json`
differs only in `analysisMs`, `timelineMs`, `sampleUs*` and `footErrorMm`
(the same field set as the 3.2 pass); every beat, downbeat, accent, phase,
slide and finiteness value is identical.

Composition cost: 1.25 → 1.44 µs/frame (+15 %, 57 cues, 2000 iterations;
`bgmedia_motion_qa`, same machine, baseline binary vs this branch).

## 4. Verification

`ctest --test-dir build/windows` — 21/21 groups, all suites green (including
`quality`, `motion32`, `soft_ik`, `spring`, `triangle`).
New tests:

* `quality.a_short_cue_performs_a_smaller_gesture_not_a_faster_one`
* `quality.overlay_gestures_stay_below_a_twitch_at_60_fps` (≤ 0.20 rad/frame on
  hands and head at 60 FPS; the old code was at 0.24–0.27)
* `quality.generated_clip_wrist_plane_has_no_threshold_step` (≤ 300 °/s per hand
  key; measured 158 °/s)

## 5. Issue #12–#14 numbers (unchanged by this pass, they are the baseline)

`bgmedia_mesh_clip --legs`, 40 s, hiphop+sidestep, 30/60/120 FPS:

| model | fps | frames with crossings | crossing triangles | max depth |
|---|---|---|---|---|
| Model1 | 30 / 60 / 120 | 12 / 20 / 40 of 2400 / 4800 / 9600 | 268 / 544 / 1060 | 5.33–5.46 mm |
| Nemesis | 30 / 60 / 120 | **351 / 711 / 1422** (14.6–14.9 %) | 5457 / 10803 / 21608 | **12.05 mm** |

Geometric cause (knee separation at the worst frame vs 2 × fitted thigh radius):
Model1 Side Step 162 mm vs 126 mm — the knee *joints* clear, the inner-thigh mesh
surfaces do not; Nemesis Side Step **109 mm vs 145 mm** — the knees themselves are
inside the proxy radius, so the penetration is real and deep. The pre-pass gap is
2.5 cm and the proxy radius is a single cylinder fitted to the thigh axis; the
Nemesis inner-thigh mesh is wider than that (12 mm deeper than the envelope), and
both feet are contact-locked there, so the runtime corrector cannot fix it without
sliding a planted foot. Not fixed here: it needs a mesh-derived inner-thigh radius
or a narrower stance per model, which changes choreography.

Issue #13 (ground drag) and #14 (matrix): see `out/quality_pass_3_3/issue13` and
`issue14`. On the synthetic fixture and both real models the reference-grid and
legacy drag figures agree within 0.1 cm for hiphop/sidestep at 30/60/120 FPS
(legacy 0.0 cm at 30 FPS on these clips), so the old 5.9 cm Side Step figure is
not reproduced here; the seek column of the matrix is a measurement of how far a
jump moves the pose, not a pass/fail number.

## 6. Soft IK and thigh colliders (keep both off by default)

Soft IK: 12 scenarios (2 models × 2 clips × 30/60/120 FPS) show peak
acceleration, angular speed, IK error and slide identical to within 0.4 % with
`--soft-ik` on and off; the flag reports bend blends but zero soft-reach solves
because `legSoftReach` is 0 by construction. No benefit is demonstrated on the
real models, so it stays off.

Thigh colliders: the tool adds 2 groups / 6 spheres / 12 chains with mesh-fitted
radii (Model1 81.0/78.5 mm, Nemesis 91.1/88.3 mm). The only available metric
(global maximum spring displacement) is bit-identical with the feature on and
off in every run, and it is saturated at the 5 cm clamp in 1–5 % of frames, so it
cannot judge the feature. A per-chain skirt measurement is needed before turning
it on; it stays off.

## 7. Still open

1. **Choreography vocabulary.** The procedural clips still return to a small set
   of arm planes for 2-beat blocks and the authored clips still hold a pose for
   about a second. That is the largest remaining contributor to the "mechanical"
   impression and it needs a content pass with a human reviewing video, not a
   metric.
2. **Hands have no articulation of their own.** No shipped clip rotates the wrist
   bone; all wrist motion comes from overlays. A per-phrase wrist track would do
   more for naturalness than any solver change.
3. Nemesis thigh crossing (section 5) — needs a mesh-derived stance rule.
4. Spring layer is saturated at the 5 cm cap in 1–5 % of frames; the cap and the
   per-chain tuning were not revisited.
5. Visual assessment of *motion* (as opposed to pose) could not be done in this
   environment: the videos in `out/quality_pass_3_3/visual/videos` were rendered
   but not watched; the pose sheets were reviewed frame by frame.
