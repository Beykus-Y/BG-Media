# BG Motion 3.3 — Collision-free wrist trajectory planner (issue #5, synthetic-rig scope)

Stacked on the task-space intent bridge (`docs/BG_MOTION_3_3_TASK_SPACE_INTENT.md`,
PR #16). Files: `include/bgmedia/motion3/WristPlanner.hpp`,
`src/motion3/WristPlanner.cpp`, `tests/test_wrist_planner.cpp`.

**Scope honesty.** This is the planner on a SYNTHETIC torso proxy and a sphere hand.
It is NOT the acceptance run from issue #5. That run needs `Model1.vrm` and Nemesis
(private, not in the repository), mesh-level triangle intersection, and before/after
videos. Those remain with the user. Nothing here changes `refineArms`, the authored
clips or the primitives.

## What it does

Input: wrist keys `{beat, wrist, shoulder}` in the chest frame, taken from
`extractClipTaskSpaceIntent` or from a future primitive. Output: a `WristPath` that
keeps the hand sphere clear of the torso and inside the arm's reach.

1. **Reach first.** A key outside the reach sphere (around its own pose shoulder) is
   moved onto the sphere and counted in `reachClamped`.
2. **Swept test, not endpoints.** Each leg is sampled every `sampleStep` (0.01 height
   units). A leg is clear when every sample keeps `torsoSignedDistance - handRadius >=
   margin` and stays in reach. The shoulder moves linearly along the leg, so the pose
   shoulder is respected.
3. **Direct when clear.** Clear legs are kept exactly as authored.
4. **Detour when not.** One waypoint per leg. Candidates: 12 directions (front, sides,
   up, down, back, diagonals) at 1x, 2x and 3x `detourOffset`. Cost = path length +
   0.5 x offset (+ a hysteresis term). Ties break by candidate index, so the choice is
   deterministic.
5. **No flipping.** Changing the detour direction between consecutive legs costs
   `directionHysteresis`, so equally valid detours keep one side.
6. **Attenuate when no detour is clear.** The leg stops at its last clear point
   (bisection) and is flagged `attenuated`. The next leg starts from there. The
   failure is reported, not hidden.
7. **Conflict.** If the start pose itself penetrates, the leg is kept and flagged
   `conflict`. Nothing is moved.

Waypoint beats are split by arc length within their leg, so the beat grid of the
authored keys is preserved. `WristPath::sample(beat)` is piecewise linear. Every leg is
verified by sampling, so the sampled path is clear and reachable, including at corners.

Torso proxy: five bands from waist (`kTorsoBottomY` = -0.12) to shoulders
(`kTorsoTopY` = +0.08), each a superellipse of width and front/back depth. This uses
the same `CollisionProxyInfo` layout as `HumanoidRetargeter`. `syntheticTorsoProxy()`
supplies the test values. A calibrated model proxy is in metres and must be scaled by
1 / height before use.

## Verification

`tests/test_wrist_planner.cpp` (11 cases, all passing; full suite 203/203):

- Signed distance: inside, front, side, above, below, and grows outward.
- Shoulder reference equals the intent rig; the rest wrist is one reach away.
- Across-chest touch (through the sternum) is detoured in front and stays clear; the
  direct line is a real penetration (checked).
- Relaxed swing and overhead raise are kept direct, with no detour and no attenuation.
- Behind-to-front sweep: the result is planned or flagged, never reported clear while
  penetrating.
- Three-key gesture: dense sampling is clear, reachable, with no jumps (< 0.02 per 0.01
  beat). Keys are hit exactly.
- Determinism: identical output on repeated calls.
- Reach clamp moves an out-of-reach key onto the sphere and counts it.
- Repeated crossings keep one detour side (front).
- Authored clips, both arms, 1/8-beat keys: planned clearance is never worse than the
  direct path, and all planned points are reachable.

## QA on authored clips (synthetic torso, hand radius 0.03, margin 0.01)

| Clip | Arm | Legs | Detoured | Attenuated | Clamped | Direct min clearance | Planned min clearance | Plan time |
|---|---|---|---|---|---|---|---|---|
| energetic | L | 128 | 0 | 0 | 0 | +0.110 | +0.110 | 0.33 ms |
| energetic | R | 128 | 0 | 0 | 0 | +0.108 | +0.108 | 0.16 ms |
| groove | L | 128 | 0 | 0 | 0 | +0.105 | +0.105 | 0.10 ms |
| groove | R | 128 | 0 | 0 | 0 | +0.100 | +0.100 | 0.09 ms |
| hiphop | L | 128 | 0 | 0 | 0 | +0.028 | +0.028 | 0.14 ms |
| hiphop | R | 128 | 0 | 0 | 0 | +0.029 | +0.029 | 0.12 ms |
| idle | L/R | 64 | 0 | 0 | 0 | +0.113 | +0.113 | 0.04 ms |
| sidestep | L | 128 | 0 | 0 | 0 | +0.100 | +0.100 | 0.13 ms |
| sidestep | R | 128 | 0 | 0 | 0 | +0.102 | +0.102 | 0.18 ms |

Reading: on the synthetic proxy, no authored clip sweeps through the torso. The planner
is therefore inactive on them, and the lowest clearance (hiphop, about 0.028) is still
clear. This is not the issue's real-model finding. That finding comes from the mesh
(forearms, hair, thighs) and from `refineArms` corrections of up to 126 mm, which this
synthetic setup does not reproduce.

An early version of the planner used the rest-pose shoulder for reach. It produced 6 to
33 spurious clamps per clip. The shoulder is now carried per pose, which fixed this.

## Not done (left for the user or a later pass)

- Acceptance on `Model1.vrm` and Nemesis, with mesh-level triangle intersections and
  swept penetration on the calibrated proxies.
- Before/after videos and the `refineArms` intervention frequency and magnitude.
- Forearm and hand-chain collision. Only the hand sphere is swept.
- Two-hand interaction gestures and hands-above-head special routes. Only the generic
  detour set is implemented.
- Runtime wiring into `DancePlayer`. The planner is a library call only.
- Cross-motion fallback policy beyond attenuation (for example, switching route
  family).
