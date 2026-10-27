# BG Motion 3.3 — Continuous elbow swivel solver (issue #6, synthetic-rig scope)

Stacked on the wrist planner (PR for #5), which is stacked on the intent bridge (PR #16).
Files: `include/bgmedia/motion3/ElbowSolver.hpp`, `src/motion3/ElbowSolver.cpp`,
`tests/test_elbow_solver.cpp`.

**Scope honesty.** This solver runs on the SYNTHETIC torso proxy and the reference arm.
It is NOT the acceptance run from issue #6. That run needs `Model1.vrm` and Nemesis
(private, not in the repository), calibrated mesh proxies, and before/after videos.
Those remain with the user. Nothing here changes `refineArms`, the authored clips or the
primitives, and nothing applies the solved curve to a model yet.

## What it does

For a shoulder S and a wrist W, the elbow lies on a circle around the S-W axis. Its
angle psi is the free parameter. psi = 0 is the bend the intent asks for
(`ArmIntent::elbowPole`). The solver works offline over a list of samples with
known beats:

- **Locus.** `makeLocus` computes the circle from the two bone lengths. The wrist is
  clamped to the reach sphere, and the degenerate cases are flagged, not hidden: a
  straight arm (circle radius zero), a folded arm (wrist closer than |upper - fore|),
  a pole collinear with the axis (a helper axis is used), and a zero pole.
- **Candidates.** 16 swivel angles per sample, `2 pi i / 16`. Candidate 0 is the
  pole-only elbow, which is the baseline.
- **Cost per candidate.** `poleWeight * (1 - cos psi)` for preference, plus
  `clearanceWeight * max(0, margin - gap)`, where `gap` is the smallest clearance of the
  upper arm (shoulder joint excluded) and the forearm from the torso proxy, minus the
  arm radius.
- **Viterbi.** Dynamic programming over the samples minimises frame cost plus the
  elbow direction change between neighbouring samples (`smoothWeight` per radian,
  `flipWeight` extra above pi/2). Ties break by candidate index, so the result is
  deterministic.
- **Curve.** Stores only the chosen bend direction per sample (unit). `bendDirection(beat)`
  interpolates it on the sphere; neighbours that point in opposite directions turn along a
  fixed rotation path, so the direction never collapses. `elbow(beat, shoulder, wrist, ...)`
  rebuilds the elbow on the two-bone locus for the wrist the consumer plans at that beat,
  so both bones keep their length at every frame, not only at the stored samples.

The solver never reads a previously rendered frame. Its output is a pure function of
the samples, so it does not depend on the frame rate, seek position or call order.

## Verification

`tests/test_elbow_solver.cpp` (7 cases, all passing; full suite in the stacked branch):

- Every swivel angle keeps both bone lengths exactly, and the locus is not degenerate.
- Swivel 0 hangs the elbow towards the pole; swivel pi lifts it.
- Pole pointing into the torso, wrist out to the side: the pole-only elbow penetrates
  (-0.015), and the solved elbow clears the margin (+0.010).
- A 33-sample sweep through the front has zero flips and a largest step below pi/2.
- Sampling is order-free: forward and backward reads of the same beats are identical.
- Degenerate inputs (fully extended, beyond reach, collinear pole, zero pole) are flagged,
  clamped where needed, and stay finite with exact bone length.
- All five authored clips, both arms: zero flips, and the solved curve never has less
  clearance than the pole-only elbow.

## QA on authored clips (synthetic torso, arm radius 0.03, margin 0.01, 16 candidates)

Clearance is the minimum over the curve of the upper arm and forearm (height units).
"greedy" is the same solver with the temporal terms off, so each sample is chosen
independently.

| Clip | Arm | Pole-only | Solved (DP) | Greedy | DP max step (rad) | Greedy max step (rad) | Flips |
|---|---|---|---|---|---|---|---|
| energetic | L | -0.008 | +0.005 | +0.005 | 0.446 | 0.446 | 0 |
| energetic | R | -0.011 | +0.003 | +0.003 | 0.412 | 0.412 | 0 |
| groove | L | -0.014 | +0.003 | +0.003 | 0.136 | 0.168 | 0 |
| groove | R | -0.015 | +0.002 | +0.002 | 0.164 | 0.237 | 0 |
| hiphop | L | -0.008 | +0.004 | +0.001 | 0.301 | 0.441 | 0 |
| hiphop | R | -0.011 | -0.007 | -0.008 | 0.287 | 0.346 | 0 |
| idle | L/R | -0.011 | -0.007 | -0.007 | 0.007 | 0.073 | 0 |
| sidestep | L | -0.008 | +0.006 | +0.008 | 0.284 | 0.328 | 0 |
| sidestep | R | -0.007 | +0.008 | +0.008 | 0.295 | 0.294 | 0 |

Solve time: 0.7 to 1.6 ms per clip and arm (about 130 samples, 16 candidates).

Reading:
- The solver reduces the pole-only penetration on every clip, and the flip count is zero
  everywhere. The DP curve has a smaller largest step than the greedy choice on groove,
  hiphop, idle and sidestep-L, and about the same elsewhere.
- Clearance is NOT fully fixed. Every row stays below the 0.01 margin, and idle and
  hiphop-R still penetrate by 0.007 to 0.008. The wrist itself is clear on every clip
  (minimum +0.028), so the residual comes from the elbow geometry.
- The cause is the synthetic proxy near the shoulder. The shoulder joint sits 0.01
  outside the torso half-width (0.10 against 0.09), and the upper arm has a 0.03 radius,
  so the upper arm close to the shoulder cannot clear the flank unless the elbow swings
  out. A calibrated mesh proxy with a real deltoid will differ. These numbers are not
  the issue's acceptance numbers.

## Not done (left for the user or a later pass)

- Acceptance on `Model1.vrm` and Nemesis: calibrated elbow-torso and forearm-torso
  clearance on real meshes, and before/after videos.
- Comparison against the current `refineArms` two-bone correction. Only the pole-only
  baseline and the greedy variant are measured here.
- Applying the curve in `HumanoidRetargeter` or `DancePlayer`. The solver is a library
  call only, so there is no fighting between two elbow solvers yet.
- Left/right symmetry cost (the mirrored poles already give symmetric curves, but this is
  not enforced).
- A finer candidate grid or a continuous optimiser. The 16-candidate grid is a starting
  point, not a claim of optimality.
- Explicit preplanned swivel curves per gesture. The DP over samples gives the same
  determinism, but the per-gesture curve authoring is not built.
