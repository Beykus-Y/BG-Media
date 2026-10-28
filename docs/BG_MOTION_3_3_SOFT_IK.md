# BG Motion 3.3: anatomical joint constraints and contact-aware soft IK

Issue #7. Status: implemented behind options that are OFF by default. With the
defaults the retargeter output is bit-identical to BG Motion 3.2.

Code: `include/bgmedia/animation/TwoBoneSolver.hpp`,
`src/animation/TwoBoneSolver.cpp` (the one two-bone solve),
`src/animation/HumanoidRetargeter.cpp` (its callers),
`include/bgmedia/animation/HumanoidRetargeter.hpp` (`RetargetOptions::softIK` and
the other new fields, `RetargetStats` diagnostics). Tests: `tests/test_soft_ik.cpp`
(suite `soft_ik`). Tool: `bgmedia_motion_diag --soft-ik`.

## 1. Inventory before this change (BG Motion 3.2)

| Where | What it did | Gap |
|---|---|---|
| `solveLeg` (applyClip and apply) | Distance clamped to `[\|l1-l2\|+0.002, l1+l2-0.002]`, then a two-bone solve. Bend side = pole projected onto the plane perpendicular to the reach line; falls back to `cross(right, dir)` then `cross(forward, dir)`. | Hard clamp: C0 only, so a foot target that moves outward at full extension has its velocity step to zero. The pole projection is normalised even when it is near zero, so the side flips when the pole lies on the reach line. |
| `refineLegs` `kneeOf` | A second copy of the same knee maths (one fallback only). | Two copies of the solver that can drift apart. Now one solve. |
| `refineArms` re-solve (armCollision) | Same clamp; pole = elbow target minus shoulder. Breaks out when the pole is degenerate. | The elbow target comes from the collision push. On a nearly straight arm that push's direction flips with the normal, so the elbow flips with it. |
| `applyClip` hip drop | Lowers the pelvis so that planted feet are reachable, using a global factor `0.972 * (L1 + L2)`. | A fixed global reach rule. Left unchanged (defaults must not move). |
| Pole choice, legs | `applyClip`: `normalize(0.6 forward + 0.4 yawed forward)`. `apply`: `forward`. | Fixed directions, not derived from the rig's rest bend. |
| Knee and elbow limits | None. The knee can fold until the reach clamp stops it. | No anatomical flexion range. |
| Arm FK (`apply`, `applyClip`) | Elbow hinge about `up * sideSign` with a fixed angle. Shoulder swing hard-clamped to `[-1.25, 1.48]` rad in `apply`. | The elbow is not limited in the authored pose. The hinge axis assumes the rig's up axis. Not changed here. |
| Contact | `refineLegs` weights the correction by `(1 - contact)^1.5`. `applyClip` uses contact for toe pointing. | No soft reach existed. The planted threshold is different from the one this change uses (see section 3). |

Worked example (a 0.3 m + 0.3 m chain, the one the tests use): the 0.002 m margin at
full extension leaves a knee lift of 0.0245 m, which is 8 % of the bone length. So a
knee that is "straight" still has a 5 cm pole-driven flip when the pole swings across.

## 2. Design

### One solve per limb

`solveTwoBone(root, target, pole, chain, limits)` is the only two-bone solve. Legs
(`solveLeg`, `refineLegs`) and the arm collision re-solve call it. Its inputs are
the bind bone lengths, the rest bend of the chain and the model axes; nothing is
read from the previous frame.

With default `TwoBoneLimits` it reproduces the 3.2 hard clamp and the 3.2 pole
bend, with the same floating point operations in the same order. One exception:
a target exactly on the root used to be divided by `kTiny` (a 1e6-long direction).
It now hangs straight down (`-up`). The fixture never reaches this case.

### Soft reach (C1)

`softReachDistance(d, lo, hi, soft)`, with `s = min(soft, (hi - lo) / 2)`:

- identity for `lo + s <= d <= hi - s`;
- above: `d' = (hi - s) + s * (1 - exp(-(d - (hi - s)) / s))`;
- below: the mirror image around `lo + s`.

The first derivative is 1 at both knees (C1), the map is monotone and it never
reaches `lo` or `hi`. `soft` is `fraction * (upper + lower)` of the limb's bind
length. The fraction is per call, through `RetargetOptions`.

Its second derivative jumps at the knee (from 0 to `-1/s`). That is the C1
trade-off, and it is measured in section 5.

### Stable bend (the pole never flips a straight knee)

The bend side is `normalize(w * poleSide + (1 - w) * restSide)`, where:

- `poleSide` is the pole projected onto the plane perpendicular to the reach
  line; `restSide` is the rest bend projected the same way;
- `w = smoothstep(sin(pole, reach line); 0.15, 0.40) * smoothstep(lift / meanBone; 0.10, 0.25)`.

So the side comes from the rest pose when the pole nears the reach line, and when
the knee is nearly straight. A straight knee (lift about 8 % of the bone) therefore
keeps its rest side whatever the pole does.

The rest bend is derived from the bind pose: the offset of the joint from the bind
reach line, in model space. A straight bind falls back to `forward` for knees and
`-forward` for elbows. Nothing is hardcoded to an axis.

### Flexion range (joint limits)

`kneeFlexMin/Max` and `elbowFlexMin/Max` are angles between the two bones
(0 = straight), in radians. The law of cosines converts them to a window on the
reach distance: `d(f) = sqrt(l1^2 + l2^2 + 2 l1 l2 cos f)`. So the limit depends
only on the bone lengths, not on the rig's axes. The window is combined with the
hard reach, and the soft band (if any) is applied inside it. A window that lies
outside the hard reach (for example an almost-straight `flexMax` that would need
more than `L1 + L2 - 2 mm`) saturates at the nearest hard limit instead of being
dropped. A misordered window (`flexMin > flexMax`) is ignored.

Where the limit is applied:

- Free legs: in the IK, enforced.
- Arms: in the collision re-solve only. The authored arm pose (FK) is not
  clamped. This is a known gap, not a bug.
- Planted legs: measured and reported (`plantedFlexConflicts`), never enforced.
  A joint limit must not move a stance foot.

### Contact-aware legs

`free = clamp(1 - 2 * contact, 0, 1)` per foot (`apply` uses `contact = 1 - lift`).

- The soft band of a leg is `legSoftReach * (L1 + L2) * free`. A planted foot
  (contact >= 0.5) therefore keeps the exact hard reach, and a stance target is
  never shortened.
- The flexion window is enforced only where `free > 0`.
- A planted leg whose target lies beyond the hard reach is counted in
  `plantedReachConflicts`. Its target is still not moved (the hip drop, as before,
  handles reachability).
- The leg collision capsules (shin and foot) end at the solved ankle when `softIK`
  is on, so the clearance is measured where the leg really is. A free target beyond
  the fold clamp is not where the solve puts the foot. With `softIK` off, the capsules
  keep the BG Motion 3.2 geometry, which uses the requested target. Test:
  `leg_capsules_end_at_the_solved_ankle`.

The threshold at contact 0.5 is a hard switch for the flexion window, while the
soft band fades in continuously. This is the one place where a contact change can
still step (see section 5).

### Arms: soft band, collision depth and the pole

The arm soft band applies only in the collision re-solve (section 2). It fades in
with the arm's initial collision depth, computed once per arm from the clearance
before any push:

    w = smoothstep(clamp(-raw / (0.01 * height), 0, 1))

The band is zero at the surface and full at 1 cm of depth. Before this fade, the
band switched on as the arm crossed the surface, so a tiny change of the shoulder
moved the wrist. On the issue #26 reproduction (`minimal.vrm`, left elbow 0.6 rad
about clip Y, left upper arm clip Z near -2.60696 rad), a 1e-4 rad step across the
surface moved the wrist 6.5 mm and the elbow 10.3 mm. With the fade the same step
moves them 0.08 mm and 0.04 mm, the same as with soft IK off. With
`softIK` off the band is zero, so nothing changes. Test:
`arm_soft_band_fades_in_with_collision_depth`.

The stable bend uses the same weight (`TwoBoneLimits::stableBendWeight`): at the
surface the re-solve uses the 3.2 pole rule, the same as the authored arm, and the
rest-bend blend reaches full strength at 1 cm of depth. Without it, the blend
switched on with the first collision solve even with `armSoftReach = 0`: with the
left elbow at 0.4 rad and the upper arm clip Z near -1.43722 rad, a 2e-5 rad step
moved the elbow about 87 mm. Test: `arm_stable_bend_fades_in_with_collision_depth`.

The arm re-solve keeps BG Motion 3.2's pole threshold (squared length 1e-10, set by
`TwoBoneLimits::poleEpsilon`). The legs use 1e-8. A pole 5e-5 m off the reach line
still sets the arm's bend side at the 3.2 threshold. At 1e-8 it would fall back to
the fallback axis, and the elbow would move 4.9 cm on the reproduction chain
(`0.3 / 0.3 m`, target `(0, -0.598, 0)`, pole `(0, -0.3, 0.00005)`). Test:
`arm_pole_threshold_matches_the_legacy_formula`.

### Options and diagnostics

`RetargetOptions` (all default off or neutral):

- `softIK` (false): master switch. When false, none of the fields below is read.
- `armSoftReach` (0.10), `legSoftReach` (0.0): soft band, fraction of limb length.
- `elbowFlexMin` (0), `elbowFlexMax` (2.5), `kneeFlexMin` (0), `kneeFlexMax` (2.4): radians.

`RetargetStats` (zero unless `softIK`): `softReachSolves`, `softReachCompression`
(largest distance the band removed, m), `bendBlends`, `flexLimits`,
`plantedFlexConflicts`, `plantedReachConflicts`.

`softReachCompression` counts both the leg and the arm solves. `footReachDeficit`
and `footSolverError` are measured against the solved (soft) end point, so with
soft IK on a deficit includes any soft shortfall.

## 3. Tests (`tests/test_soft_ik.cpp`, suite `soft_ik`, 17 cases)

- Soft-reach math: identity between the knees; monotone and never reaching the
  limits; C1 at both knees (numerical slopes); zero band equals the hard clamp;
  flexion-to-distance endpoints.
- Solves: a target just inside, just outside and far outside the reach (bone
  lengths kept, pulled in, never past the limit); a deeply folded target and a
  target on the root (valid triangle, no NaN, hangs down).
- Flexion window: the solved angle equals the limit; reported but not enforced
  when the limit does not apply.
- Stable bend: a sweep across the pole's reach line at a nearly straight knee.
  The largest step between consecutive bend directions is 0.004 rad with the
  stable bend, against 3.14 rad (a flip) with the 3.2 rule.
- Rotated rest pose: the solve for rotated axes, rest bend, pole and target equals
  the rotated solve.
- Retargeter: defaults ignore the new fields (bit-identical output with the
  fields changed while off); a planted foot (contact 1) is not moved by the soft
  band, while a free foot is; the same output on repeated calls, independent of
  calls in between and of the media time; the whole rig rotated about its up
  axis yields the rotated output (arm collision off, see section 5).
- Review fixes: the arm pole threshold matches the 3.2 formula
  (`arm_pole_threshold_matches_the_legacy_formula`); the leg capsules follow the solved
  ankle (`leg_capsules_end_at_the_solved_ankle`, a free foot beyond the fold clamp:
  the old geometry differed by 0.146 m of clearance); the arm soft band has no
  discontinuity across the collision surface (`arm_soft_band_fades_in_with_collision_depth`,
  which fails without the depth fade).

## 4. Measurements

Golden check (not committed, a local bit-hash over 1149 retargeter outputs:
`apply` pose path with and without collisions, every bundled dance clip, far-foot
variants): the hashes before and after this change are identical
(`apply=24e6b0210a5727db clip=c1ec35c1617d1ad4 collide=116c99bf02af93af`).

Default-off CSV check against origin/dev's `bgmedia_motion_diag`: `--model
tests/fixtures/minimal.vrm --clip X --to 12 --csv`, for fluidSignal, streetCurrent,
neonPulse, hiphop and sidestep, with and without `--leg-proxy`. All ten CSVs are
byte-identical.

Fixture `tests/fixtures/minimal.vrm`, `bgmedia_motion_diag --clip fluidSignal
--from 0 --to 30 --fps 60`:

| Run | Left knee peak accel / jerk | Soft solves | Bend blends | Ground drag / leg proxy |
|---|---|---|---|---|
| 3.2 (no flag) | 44.3 m/s^2 / 2094 m/s^3 | - | - | 0 cm / 0 frames |
| `--soft-ik` (defaults) | 44.3 m/s^2 / 2090 m/s^3 | 0 | 187 | 0 cm / 0 frames |
| experiment: leg band 0.06 (not a default) | 58.8 m/s^2 / 3255 m/s^3 | 78 | 170 | 0 cm / 0 frames |

The experiment is why the leg band defaults to 0 (section 5). The arm band and the
knee window did not engage on this clip.

## 5. Known limits (stated plainly)

- **C1, not C2.** The soft band removes the velocity step but leaves an
  acceleration step at each knee. On the fixture this raises the left knee's jerk
  peak from 2090 to 3255 m/s^3. So the leg band is off by default, and only the
  arm band and the bend rule are on with `softIK`. A C2 mapping would need a
  different curve than the exponential the issue suggests.
- **A pole-driven bend cannot be continuous everywhere.** A continuous side field
  over all target directions must have singular points. The stable bend moves the
  pole's singularity (the pole on the reach line) to the rest bend, and it smooths
  the transition. But where the pole and the rest bend point in opposite directions
  in the middle of the blend, the blended vector cancels, and there the side is
  chosen by convention. This is measured in the sweep test: the step is 0.004 rad
  with the stable bend, but a bent knee whose pole is genuinely singular still has
  a flip of about twice its lift. The straight-knee case (the one the issue names)
  no longer flips.
- **Arm FK is not limited.** The elbow flexion range is applied only where the
  collision re-solve runs. The authored elbow, the shoulder clamp in `apply`, and
  the elbow hinge about the up axis are unchanged.
- **Contact switch.** The flexion window switches on at contact 0.5 (the soft band
  does not). A target that sits in the folded part of the window can step when a
  foot becomes planted. This is the price of a stance foot that must never move.
- **Arm fade scale.** The 1 cm depth over which the arm band fades in is a tuning
  choice, not derived from the rig. It only sets how far inside the surface the band
  reaches full strength. Shallower or deeper fades keep the continuity property.
- **Rig-level rotation test.** It turns the rest pose and the model axes, but the
  arm collision proxy is calibrated on the unrotated mesh (pre-existing). The test
  therefore runs with arm collision off.
- **Bit-identity is checked locally**, with the scratch golden tool above. The
  committed tests check that the new fields are ignored when off, not bit-equality
  across compilers (MSVC and GCC may differ in `exp` and `acos` in the last bit).
- **Not run on a real VRM model.** The only fixture used is `minimal.vrm`. The
  private `models/Model1.vrm` was not available to this run.
- The hip drop's global `0.972` factor and the arm swing clamp in `apply` are kept
  as they were. Changing either would change the default output.
