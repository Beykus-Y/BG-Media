# BG Motion 3.2 — Quality Pass (stabilization, retargeting, collision, choreography)

Branch `fix/bg-motion-3.2-quality-stabilization`, based on `aacdeeb`. Nothing was
pushed, no PR was created, nothing was merged. The private model
`models/Model1.vrm` was used locally only and is not committed.

Machine-readable numbers: `docs/quality_pass_3_2_metrics.json` (collected by the
new tests) and `docs/motion_qa_3_2_quality_pass.{txt,json}` (output of
`bgmedia_motion_qa`, comparable to the older `docs/motion_qa_3_2.{txt,json}`).

**Scope honesty.** This pass fixed the spatial math, motion composition, spring
dynamics and a footwork bug, and added an arm/torso proxy refinement. It did **not**
implement a goal-driven wrist-trajectory layer, did **not** audit the GPU renderer
and did **not** make a video. Section M lists everything that remains.

---

## A. Executive summary

| Area | What was wrong | State now |
|---|---|---|
| Foot IK diagnostics | Reported an analytic number, not the evaluated skeleton | Measured on the final skeleton; reach deficit and solver error are separate |
| Calf direction | Absolute world rotation applied to a bind *direction* | Fixed; knee position is independent of bone-axis orientation |
| Wrist/finger axes | Axes converted model→clip→model twice | Single conversion; verified on a rig that faces +X |
| `applyClip` | Ignored `RetargetOptions.footIK` | Honoured |
| Motion primitives | Order-dependent, low priority could block high, hard cuts, masks dropped real motion | Deterministic, continuous conflict resolution, masks match what each primitive writes |
| Time warp | Detailed hand pose stayed in the unwarped phase | Warped together with the scalar hand |
| Crossfade | Overlay set flipped at 50 % of the blend | Overlays composed per segment before the blend |
| Spring bones | Wrong space for rotations, dimensionally inconsistent integrator, stale state on OFF→ON, no teleport handling, branches and leaf bones lost, collider index shift | All fixed and covered by tests |
| Footwork | **Foot `z` and `yaw` arguments were swapped; every shipped dance had a 22 cm single-key foot teleport** (not in the audit) | Fixed; peak foot step per key 0.1375 → 0.04–0.05 height units |
| Mesh clipping | Reported arm/hair/clothing overlap with Spring OFF | **Not reproduced** on the frames I sampled (F). An arm↔torso proxy refinement was added and verified on forced gestures; hair/cloth overlap is neither confirmed nor solved |

Tests: 141 before → 173 after (all pass; CTest 16/16 groups). Some of the new tests need the private
`models/Model1.vrm` and return early without it; they are listed in J.

**BG Motion 3.1 did not regress.** `bgmedia_sync_qa` was re-run and diffed against the committed
`docs/sync_qa_3_1.json`: of 693 values only analysis/timeline/sampling *timings* and `footErrorMm`
(0.00015 → 0.00024 mm, now measured on the evaluated skeleton) differ; every beat, downbeat, accent-approach,
phase speed/jerk, slide and NaN metric is identical. In `bgmedia_motion_qa` the five authored clips
(energetic, groove, hiphop, idle, sidestep) are byte-identical to `docs/motion_qa_3_2.txt`.

### Behaviour changes you will see

1. **All three generated dances change** (neonPulse, streetCurrent, fluidSignal): planted feet no longer
   jump 22 cm at pivots, steps carry their real yaw/forward offset, so footwork is visibly different.
2. **Spring bones now actually move** (3–5 cm peak). The previous integrator was scaled by `dt`; hair and
   skirt will look livelier and earlier spring tuning is not comparable.
3. **Wrist roll/pitch and thumb opposition on Model1 change direction.** Model1 faces −Z; the old
   double axis conversion rotated those channels about reversed axes there, while a standard-axes rig was
   already correct. They now follow the same clip-frame contract on every rig (signed tests below). No
   primitive sign was retuned; if a gesture looked right on Model1 *because of* the old behaviour it will
   now be mirrored at the wrist and should be re-checked by eye.
4. Overlay selection is deterministic and continuous; some cues that used to win by array order now lose.
5. Arms may be nudged away from the torso when the proxy is violated (not in any shipped dance).

---

## B. Audit validation (C01–C13)

“Reproduced” means: a new test fails on the original code and passes after the fix.
“Logic-confirmed” means: the defect is certain from reading the code, a regression test
exists and passes, but the failing-on-baseline run was **not** performed.

| # | Audit claim | Verdict | Evidence | Status |
|---|---|---|---|---|
| C01 | Foot IK error is analytic | **Confirmed, reproduced** | `spatial.foot_error_is_measured_on_the_evaluated_skeleton` failed on baseline | Fixed |
| C02 | Spring rotations mix stage/character space | **Confirmed, reproduced** | root yaw 0/90/180/−90: character-space rotation response differed by **2.22 rad** on baseline, 0.0014 rad now | Fixed |
| C03 | Stale state OFF→ON | **Confirmed, reproduced** | first ON frame jumped to the 50 mm clamp on baseline; now only the animation's own frame motion | Fixed |
| C04 | Primitive priority/selection order | **Confirmed (logic)** | priority comparison direction + `std::sort` without tiebreak; 720-permutation test passes | Fixed |
| C05 | Time warp ignores detailed hand pose | **Confirmed, reproduced** | with the two `adaptPose` hand lines reverted the test fails (worst curl/wrist delta 0.0209 vs 1e-4); with the fix 29 warped frames match `sampleClip` exactly | Fixed |
| C06 | Double axis conversion for wrist/thumb | **Confirmed, reproduced** | fails on baseline when model axes ≠ clip axes. Model1 faces −Z, so this was real on the actual model. Signed checks added: pitch/yaw/roll through the detailed path equal the same rotation through the clip hand-bone path (axis **and** sign) on three model bases incl. Model1's; thumb opposition rotates about the same axis and direction as the curl path (6 thumbs on Model1) | Fixed (visible direction change on Model1, see above) |
| C07 | Calf direction with non-identity bind | **Confirmed, reproduced** | knee position differed between a rig and the same rig with re-oriented leg axes | Fixed |
| C08 | Spring unit mixing | **Confirmed, reproduced** | velocity was a per-step displacement, then `+ gravity*dt` (m/s) and `* dt` again; step response never settled (25 mm residual) and the 12 m/s cap compared metres per step | Fixed (Verlet) |
| C09 | First child only | **Conditional** | Model1 has *no* branches and no leaf chains (0/116 nodes missed). Synthetic test (Head → two eyes) failed on baseline | Fixed |
| C10 | Collider group index shift | **Conditional** | Not hit by Model1. Logic-confirmed in the loader; synthetic GLB test added | Fixed |
| C11 | `applyClip` ignores options | **Confirmed, reproduced** | | Fixed |
| C12 | Overlay switches mid-crossfade | **Confirmed, reproduced (emulated)** | per-step bone rotation during a Full-Dance→Full-Dance crossfade at 240 Hz: legacy emulation 0.482 rad → 0.104 rad | Fixed, **residual remains** (see M) |
| C13 | Correction segment overlapped by hold | **Confirmed, latent** | The shipped generator never ends off-start, so no shipped clip is affected; unit test on `Footwork` | Fixed |
| — | **New: `Footwork` z/yaw swapped** | **Confirmed, reproduced** | `step(x=5,z=3,yaw=40)` returned `z=40 yaw=3`; `pivot(22)` never rotated and left a 22 cm offset; all three dances had a 0.1375-height-unit step in one key | Fixed |

---

## C. Spatial mathematics

* **Contract (retargeter).** Every rotation in `applyClip` is authored about clip-frame
  axes (+X left, +Y up, +Z forward) and converted to model space exactly once by
  `quatToModel`. The comment in the code states this. Wrist pitch keeps its established
  sign (clip −X), so the look on a standard rig is unchanged.
* **Calf.** `bindCalfDirection` is a direction; it is now carried by the rotation that
  moved the thigh (`thighWorld · upperBind⁻¹`) instead of the calf's absolute world rotation.
* **Foot IK error.** `RetargetStats` now has `maxFootError` (evaluated foot joint vs. the
  requested target), `footSolverError` (evaluated joint vs. the closest reachable point) and
  `footReachDeficit` (target beyond leg reach). `RetargetOptions.footIK=false` disables IK in
  `applyClip` and applies the clip's own leg rotations.
* **Mirroring.** Primitives author left/right signs themselves; the wrist/finger path is not
  auto-mirrored. This is documented, not changed.

## D. Actual IK QA

Synthetic rigs are built from `minimal.vrm` by re-orienting the bone axes of both legs while
keeping all joint positions (`reorientNode`). Checked at root yaw 0 / 0.25 / −0.6 turns:

* evaluated foot position vs. expected stage target: < 3 mm (both rigs);
* reported `maxFootError` equals the independently measured error within 1.5 mm;
* unreachable target (1.2 height units away): deficit > 0.2, solver error < 3 mm;
* knee/foot positions equal between the plain and the re-oriented rig (< 3 mm).

## E. Motion compositor

* **Selection** is two-phase and order-independent: candidates → sort by priority, then by the
  cue's own data (never by weight or array position, which would flip mid-cue) → greedy
  acceptance. 720 permutations of six cues give identical poses and counters.
* **Conflicts are continuous.** A weaker cue keeps `(1 − winner weight)` of a shared region
  instead of being cut. The upper-body layer cap is a *capacity* that every cue consumes with
  its weight. (A first version used hard cuts; it popped visibly in the crossfade measurement and was replaced.)
* **Options** (`allowHands`, …) remove a region instead of rejecting the whole gesture.
* **Bilateral primitives** (`WeightShift`, `KneeBounce`, `StepTouch`) are no longer narrowed by `side`.
* **Mask audit of all 27 primitives** (`quality.primitive_touched_channels_fit_the_effective_mask`).
  Found and fixed: `ArmWave`/`ArmReach` wrote hands outside their mask; `AlternatingArmSwing`
  overwrote the hand pose; `ShoulderAccent` rotated the upper arm (removed, the arm inherits the
  shoulder); `FingerRipple` bent the elbow 6° under a hand-only mask (removed); `HipSway`,
  `WeightShift`, `SideStep` wrote spine/chest; `HeelTap` rotated both legs for one side. Masks were
  widened only by the bones actually touched, never to the whole body.
* **Contribution test.** All 27 primitives visibly change the composed pose
  (`quality.every_primitive_visibly_changes_the_composed_pose`).
* **Time warp:** `adaptPose` now takes `handPose` and `detailedHands` from the warped sample.
* **Crossfade:** `DanceSchedule` accepts a `PoseDecorator`; `DancePlayer` composes overlays on each
  segment's own pose before the blend, so the outgoing clip keeps its overlays while fading.

## F. Mesh clipping

**Status: the reported mesh clipping was not reproduced on the frames I sampled, and not solved.**

What I did:

1. Rendered neonPulse/streetCurrent on Model1 (Spring OFF, overlays FULL) from several camera yaws and
   noticed forearms and hands that look as if they are inside the chest.
2. Added `--hide-material <substr>` to `bgmedia_vrm_preview` and re-rendered the same frames with hair
   hidden and with hair + clothing hidden (`artifacts/qa/hide_material_discriminator.png`,
   `artifacts/qa/hair_hidden_chest_zoom.png`, not committed). Model1 has `HAIR_01/02`, `CLOTH` (tops, one-piece,
   shoes) and `SKIN` materials.
3. Measured the same frames on the skeleton. At t = 4, 7, 8 s the wrists are 0.28–0.30 m **in front of** the
   hips while the calibrated torso half-depth is 0.116 m; raw proxy clearance at those frames is +0.112,
   +0.167 and +0.087 m. The camera sits in front of the model, so the “hands vanish into the chest” impression
   is occlusion and foreshortening of arms that point at the camera, not interpenetration.
4. Swept three dances × 40 s at 20 Hz (2403 frames, overlays Full): minimum proxy clearance +0.023 m,
   zero violations. (My first proxy was a single wide cylinder that flagged 54 % of frames; those were false
   positives from hanging arms next to the shoulder line, which is why the proxy is banded and ignores the
   upper-arm midpoint. The zero is a property of the redesigned proxy, not a pose change.)

What this does **not** show: whether a hand or sleeve intersects hair, a vest pouch or a skirt in some frame
I did not render. Hair is skinned rigidly to head/neck with Spring OFF, so hair passing through an arm that
reaches toward the face is *plausible* but **unconfirmed**; I did not find such a frame. The earlier statement
that “it is rigid hair” is a hypothesis.

Not investigated (honest): the GPU renderer (skin matrices, inverse-bind order, alpha sorting, depth writes),
CPU-vs-GPU skinning agreement, wireframe/opaque passes. `adapters/sdlgpu` was not reviewed, so a
transparency or depth-sorting explanation for what you saw is **open**. The next useful step is a mesh-level
penetration measurement (CPU skinning of arm-dominant vertices against nearest torso/hair vertices), which was
not built.

## G. Collision-aware refinement

* **Proxy (cached at load, `Impl::calibrateProxies`).** An elliptical torso cylinder with three
  height bands (waist / chest / shoulders) calibrated from the rest mesh (percentile 85 of
  vertices whose dominant joint is Spine/Chest/UpperChest), plus forearm and hand radii from the
  arm-dominant vertices. Model1: half-width 0.108 m, front 0.116 m, back 0.076 m, forearm radius
  0.033 m, hand radius 0.039 m. A model without usable mesh data falls back to skeleton
  proportions (`CollisionProxyInfo::fromMesh=false`).
* **Samples.** Elbow, forearm midpoint, wrist, palm centre. The upper-arm midpoint is deliberately
  excluded: a hanging arm lies beside the torso by construction.
* **Correction.** After the arm bones and before foot IK, only arm bones are changed. A smooth push
  (`softPush`, no on/off switch) moves elbow and wrist targets along the outward normal; ≤ 6
  iterations; a two-bone solve keeps bone lengths; a swing-only update preserves the authored axial
  twist. Moves are clamped to 35 % of arm length (`armCorrectionClamped`).
* **Results.** Forced cross-body sweep: worst clearance −41.2 mm → −2.9 mm (inside the 6.5 mm gap),
  all 34 violating frames improve, wrist step 5.9 mm vs. 5.6 mm unrefined (continuous). Foot
  stage positions unchanged (< 10 µm). `applyClip` costs 0.0187 ms with and without the
  refinement.
* **Limits.** This is a proxy test, **not** a mesh intersection test: it will not detect
  sleeve-vs-vest, hand-vs-hair, or shoulder-vs-chest-strap overlaps, and a 6.5 mm gap is a
  heuristic. Diagnostics: `armProxy`, `armClearanceRaw`, `armClearanceMin`, `armCorrection`,
  `armCorrectionClamped`, `collisionProxy()`. They are **not yet shown in the Desktop UI**.

## H. Spring physics

* **Integrator.** Position Verlet with the fixed 120 Hz step and explicit units: displacement per
  step damped by `exp(−drag·dt)`, spring acceleration `ω²(target − x)` with `ω = 10 + 26·stiffness`
  rad/s, gravity as acceleration, `x += d + a·dt²`; the 12 m/s cap compares real speed.
  `ω²dt² ≤ ~0.09`, so the explicit scheme is stable; length projection keeps chains inextensible.
* **Spaces.** Simulation stays in stage space; the rotation delta is re-expressed in character
  space (`R⁻¹·δ·R`) before it is composed with the animated rotation.
* **Lifecycle.** While OFF the simulation shadows the animated pose; seeks, pauses > 0.25 s,
  negative dt and root teleports (> 0.25 heights/frame) snap to the pose. OFF still means no
  deformation at all (positions unchanged to 1e-7 m in the test).
* **Loader.** Collider groups keep their original index (invalid groups become empty
  placeholders); subtrees become a tree of chains (a branch chain is anchored at the parent's
  simulated branch point and the anchor is written once); leaf spring bones get the VRM 0.x
  7 cm virtual tail.
* **Results.** Yaw invariance 2.22 → 0.0014 rad; step response settles to 1 mm (baseline 25 mm);
  tip spread across 30/60/120/180 FPS 0.72 mm *for a single head-bob instant*; on the neonPulse dance, 60 vs
  120 FPS tip trajectories differ by 2.1 mm on average, but by up to **49 mm** at one instant on one chain and
  by more than 2 cm in 0.26 % of samples (40 of 15 156), i.e. collision contact can amplify tiny phase
  differences (not root-caused; it never exceeds the 5 cm cap); `bgmedia_motion_qa` shows streetCurrent peak
  displacement of 3.0–3.8 cm and neonPulse pinned at the 5.00 cm cap at 120/180 FPS; max stretch ≤ 0.2 %; OFF→ON first-frame displacement
  18.5 mm against 19.5 mm of animation motion in that frame (baseline 50 mm = stale clamp).
  Spring chains now actually move (3–5 cm peak displacement) — the old integrator was
  effectively scaled by `dt`, so earlier visual tuning of spring parameters is **not**
  comparable.
* Spring collision spheres limit single chains only. They are not a full mesh-vs-body collision.

## I. Choreography quality

Delivered:

* **Footwork bug (see B).** Planted feet no longer teleport 22 cm and pivots now actually turn.
  `bgmedia_motion_qa` before → after on the generated dances: foot slide 2.6–3.6 mm → 0.13–0.20 mm;
  peak foot speed 1.8–3.2 m/s → 0.4–0.8 m/s; hip speed 0.54–0.97 → 0.29–0.53 m/s;
  swings per dance 6–8 → 10.
* **C13** loop-closing fix, with the invariant that segments are disjoint.
* Overlay continuity and soft conflict resolution (E).

**Not delivered:** the goal-driven wrist/elbow trajectory layer (anticipation, follow-through,
shoulder compensation), torso lead/follow with counter-rotation timing and a CoM-style weight
transfer. The primitives are still joint-angle gestures. The “mannequin” impression of the arms
is therefore **not addressed** by this pass beyond the foot/transition fixes.

## J. Regression tests

| | before | after |
|---|---|---|
| `bgmedia_tests` | 141 | 173 |
| CTest groups | 13 | 16 |
| New files | | `test_quality.cpp`, `test_spatial.cpp`, `test_spring.cpp` |

Tests that **return early without `models/Model1.vrm`** (so "all pass" includes skips on a clone without it):
`spatial.model1_arm_proxy_clearance_report`, `spatial.arm_refinement_reduces_forced_penetration_smoothly`,
`spatial.thumb_opposition_follows_the_curl_axis_and_sign`, `spring.response_is_independent_of_root_yaw`,
`spring.enabling_after_a_pose_change_does_not_pop`, `spring.teleport_and_hitch_snap_to_the_pose`,
`spring.result_is_frame_rate_independent_and_finite`, `spring.dance_trajectories_agree_between_60_and_120_fps`
(plus the existing `retarget.model1_if_available` and Model1 spring tests). The branching, single-node, collider-index,
step-response, IK, wrist and footwork tests use only the committed fixture.

New tests that fail on the original code: C01, C06, C07, C11 (spatial); C02, C03, C08, C09 (spring);
`footwork_arguments_keep_their_meaning` and `showcase_dances_have_no_foot_teleports`.
`tests/CMakeLists.txt` now also puts `src/motion3` on the test include path (private header
`FootworkTrack.hpp`).

Build-system note: MSVC output here is Russian, so Ninja does not track header dependencies.
After editing a public header, touch the dependent `.cpp` files or rebuild; I hit a real ODR
mismatch (stale test object) once.

## K. Performance

* `applyClip`: 0.0187 ms with and without the arm refinement (2000 runs, Model1).
* Motion composition: 1.72 → 1.25 µs/frame (57 cues, `bgmedia_motion_qa`).
* Spring simulation: 0.05–0.16 ms per frame on Model1 (36 chains, 116 nodes).
* Collision proxies are built once at load (one pass over the skinned vertices).
* `bgmedia_benchmark` measures BG Beat analysis, which this pass did not touch; not re-run.
* GPU cost: not measured.

## L. Visual QA

Tools: `bgmedia_vrm_preview` (SDL_GPU renderer, one still per run) and the image reader. Files are in
`artifacts/qa/` (not committed).

Reviewed: Model1, neonPulse/streetCurrent, overlays off/full, spring off/on, camera yaw ±35–40°, hair
hidden / hair + clothing hidden, at 8 time points. Seen: arms clear of the torso, no folded elbows, no
inverted hands; the hidden-material views make the occlusion explanation in F visible.

**Not done:** video capture/review, Desktop capture, a bind-pose scenario (the preview tool has none), an
explicit root-yaw render, a stress-test sequence, a crossfade render, a **spring-on sequence** (a single cold-start
frame cannot show spring behaviour, so the spring on/off stills prove nothing about spring motion; the evidence for
spring is numerical, section H), a close-up of a roll/point/thumb frame after the C06 sign change, the second real VRM.
Stills do not show temporal artifacts; foot/transition improvements come from the QA tool, not from watching.

## M. Remaining limitations

1. **Crossfade residual.** In the Full→Full crossfade a LeftHand step of 0.104 rad per 4 ms remains
   around progress 0.8 (steady-state overlay step is 0.027). The test asserts ≤ 50 % of the legacy
   figure and prints the rest. Root cause not found.
2. No goal-driven arm layer, no torso lead/follow, no inertialization/velocity-aware crossfade.
3. GPU renderer not audited; no CPU/GPU skinning agreement test; no alpha/depth investigation.
4. Rigid hair/clothing overlap with Spring OFF is not addressable by the arm proxy.
5. Only one real VRM (Model1, which also happens to have no branching spring chains); no second model
   with different proportions/bind orientations. Synthetic rigs cover the math only.
6. Desktop UI does not show the new diagnostics, and `bgmedia_desktop.exe` could not be relinked
   because the running app held the file (it compiles; only the link step failed). Desktop settings were not exercised.
7. `bgmedia_vrm_preview` and `bgmedia_motion_qa` still compose overlays after sampling
   (single-clip use), so they do not exercise the per-segment crossfade path.
8. Wrist roll/pitch sign convention is “as authored per primitive”; there is no automatic L/R mirror.
9. Spring parameter tuning (stiffness → ω, damping → drag) was derived analytically and checked for
   stability, not tuned by eye; hair may now look livelier than before.
10. C09 leaf chains use a virtual tail but only synthetic data exercises it.
11. **ASan:** AddressSanitizer Debug build (`/fsanitize=address`, tests + tools) of the test suite as it stood at
    commit `d7dda88` (170 tests): all passed, no ASan reports. The repo script `scripts/run_asan_windows.ps1`
    itself fails here (`vswhere` is not on PATH), so an equivalent invocation was used. The last three tests
    (two signed wrist/thumb checks and the 60-vs-120-FPS dance test) were added afterwards and have **not**
    run under ASan.
12. **Desktop:** `bgmedia_desktop.exe` compiled but could not be relinked because the running app held the file;
    I did not terminate it. The Desktop UI therefore was not rebuilt or exercised.
13. Spring: dance-level 60 vs 120 FPS divergence of up to 49 mm at isolated instants (H); not root-caused.
14. The crossfade residual (item 1) was not ablated cue by cue.

## N. Git

Branch `fix/bg-motion-3.2-quality-stabilization`. Commits on top of `aacdeeb`:

```
66a2007 Fix spatial math: measured foot IK error, calf bind direction, single wrist axis conversion, applyClip footIK option
b251fa6 Fix motion composition: deterministic selection, soft conflicts, masks, per-segment overlays, warped hands
9db8c38 Add mesh-calibrated arm/torso proxy refinement with clearance diagnostics
e4d324d Fix secondary dynamics: Verlet units, character-space rotations, OFF/ON/teleport, branching, collider indices
a04191f Fix footwork: loop-closing correction, step/pivot z-yaw packing (removes 22 cm foot teleport)
```

A final commit adds this report, the metrics JSON, the QA output and the test renaming. The
working tree otherwise contains only the two pre-existing untracked archives (`BG Media.zip`,
`src.zip`), which were not touched. No push, no PR, no merge.
