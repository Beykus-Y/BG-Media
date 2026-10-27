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
| Eyes through the back of the head | Blended face layers were drawn with a depth compare of ALWAYS, i.e. over everything | **Fixed** (G3): one-line renderer change plus a GPU regression check |
| Mesh clipping | Confirmed at mesh level with Spring OFF | Confirmed at mesh level on two of the three dances (F.1: hands/fingers and a forearm cross the torso by 1.6–2.9 cm; CPU and GPU skinning agree). After the targeted proxy fix (G2) the confirmed triangle crossings drop from 16 to 0 frames (streetCurrent) and from 11 to 2 (fluidSignal), neonPulse control stays 0; hair overlap unconfirmed |

Tests: 141 before → 175 after (all pass; CTest 16/16 groups). Some of the new tests need the private
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

**Status: not reproduced by the proxy or on my first sampled frames; a later mesh-level tool (F.1) does find candidate overlap frames. Not solved.**

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
4. ~~Swept three dances × 40 s: minimum proxy clearance +0.023 m, zero violations.~~ **This claim was wrong.**
   The sweep test reused one `DancePlayer` for the three clips; re-selecting at t = 0 keeps the first schedule, so it
   measured neonPulse three times. With a fresh player per clip (fixed) the sweep over 2403 frames gives **20 frames
   with proxy penetration, raw minimum −0.046 m**, which the refinement brings to ≈ 0. See F.1 for why that is still
   not enough at mesh level. (My first proxy was a single wide cylinder that flagged 54 % of frames; those were false
   positives from hanging arms, which is why the proxy is banded and ignores the upper-arm midpoint.)

Caveat on items 1–3: the stills and the skeleton numbers for "t = 4/7/8 s" were produced with a direct jump to that time
(`--time`, `update(t)`), which makes the dance player seek and re-align its schedule, so those poses are not the ones a
sequential playback reaches at the same time. They are internally consistent with each other but not representative.

What this does **not** show: whether a hand or sleeve intersects hair, a vest pouch or a skirt in some frame
I did not render. Hair is skinned rigidly to head/neck with Spring OFF, so hair passing through an arm that
reaches toward the face is *plausible* but **unconfirmed**; I did not find such a frame. The earlier statement
that “it is rigid hair” is a hypothesis.

Not investigated (honest): the GPU renderer (skin matrices, inverse-bind order, alpha sorting, depth writes),
CPU-vs-GPU skinning agreement, wireframe/opaque passes. `adapters/sdlgpu` was not reviewed, so a
transparency or depth-sorting explanation for what you saw is **open**. The next useful step is a mesh-level
penetration measurement (CPU skinning of arm-dominant vertices against nearest torso/hair vertices), which was
not built.

### F.1 Mesh-level diagnostic (follow-up): `bgmedia_mesh_clip`

Built after the first pass, diagnosis only; no new collision system. It CPU-skins all 100 816 vertices of Model1
with the retargeted pose and counts forearm/hand/finger vertices behind the nearest torso vertex's normal
(vertex-vs-nearest-vertex, so an approximation that can over- or under-report), and arm vertices within 4 mm of hair.
Upper-arm skin is only tested against hair, because a hanging upper arm lies against the torso by construction
(a first version without that rule flagged 400/400 frames). Command: `bgmedia_mesh_clip [--clips a,b] [--depth-mm 8]`.

Result, 3 dances × 40 s at 10 Hz, overlays Full, spring off, depth threshold 8 mm, "frame" = at least 5 vertices:

| dance | frames with forearm/hand behind torso | worst depth | frames with arm within 4 mm of hair |
|---|---|---|---|
| neonPulse | 79 / 400 | 24 mm | 4 |
| streetCurrent | 90 / 400 | 29 mm | 6 (up to 70 vertices at t = 9.6 s) |
| fluidSignal | 31 / 400 | 16 mm | 0 |

Worst frames (renders in `artifacts/qa/mesh_clip_candidates.png`): streetCurrent t = 5.1 s (left thumb/hand, up to 690
vertices), neonPulse t = 7.3 s (right forearm), fluidSignal t = 4.8 s. In the stills the hand rests on the opposite
shoulder / upper chest with the sleeve pressed into the vest, which is consistent with a real, shallow (1.5–3 cm)
overlap. At streetCurrent t = 9.6 s a raised arm lies behind the hanging hair strands.

**Method caveat found while doing the three checks below:** an earlier version of this tool re-requested poses with a
backwards `update(t)`, which seeks and re-aligns the schedule (poses differ from the sweep; its first "explain" output was
meaningless). The tool now reuses the poses of the sweep itself, and the renders in
`artifacts/qa/mesh_clip_confirmed_frames.png` were made by stepping sequentially from t = 0 at 10 FPS
(`bgmedia_vrm_preview --time 0 --frames N --fps 10`).

#### The three checks

**1. Why the proxy reported a positive clearance while the mesh crossed.** Per worst frame the tool prints the proxy's
own clearance and where the deepest vertex lies relative to the proxy samples (elbow, forearm midpoint, wrist, palm
centre; spheres of the calibrated radii):

| frame | proxy raw clearance | deepest vertex | nearest proxy sample | covered? |
|---|---|---|---|---|
| streetCurrent 5.1 s | −0.020 m (flagged; refinement corrected it) | 29 mm, spine position 0.84 | palm, 9.0 cm away (radius 3.9 cm) | **no** (a finger) |
| fluidSignal 4.8 s | +0.020 m | 16 mm, spine position 0.57 | forearm-mid, 3.4 cm (radius 3.3 cm) | borderline |
| neonPulse 7.3 s | +0.038 m | 22 mm, spine position 0.59 | elbow/forearm, 5.2 cm (radius 3.3 cm) | **no** |

So the earlier hypothesis ("the proxy fades above the neck base") is **wrong**: the offending vertices lie at spine
position 0.57–0.86, inside the torso span. Two real gaps: (a) the hand is a single sphere at the palm centre although the
hand extends about 9 cm further, so fingers and thumbs are not covered; (b) the torso surface is the 85th percentile of the
rest vertices, so pouches and straps stick out up to ~2 cm beyond it.

**2. Real triangle crossings vs close contact.** Arm-skinned triangles (forearm, hand, fingers, sleeve, glove) against torso-skinned
triangles, all-edges-vs-triangle test with a grid broad phase, crossings that already exist in the neutral pose masked
(there were none):

* streetCurrent 5.1 s and 24.3 s: **57 arm triangles cross torso triangles** (45 at 24.4 s), spine position 0.76–0.86. Confirmed
  penetration: in the render the hand/fingers and sleeve sink into the chest rig.
* fluidSignal 4.8 / 15.6 / 1.2 s: **6 crossing triangles**, forearm into the mid-torso, 16 mm.
* neonPulse 7.3 / 7.4 / 26.6 s: **0 crossings** although the vertex test says 22 mm. That is near contact or a fold (or a hand fully
  inside the shell, which this test cannot see); it is not confirmed clipping.

Limits: triangles are checked only against triangles of the torso-skinned group (not hair, not other clothing layers); an arm
that is entirely inside a closed surface gives no crossing; sleeve-over-vest layering authored by the artist counts as a
crossing.

**3. Is the CPU geometry the geometry that is drawn?** `bgmedia_skin_check` renders the SKIN and CLOTH materials on the GPU (second
render with everything hidden gives the background), skins the same vertices on the CPU with `model · nodeGlobals[joint] ·
inverseBind` (the shader's palette formula), projects them with a copy of the renderer's camera (including that the camera
ignores root translation: my first copy used the model matrix and produced IoU 0.55 on two root-moving frames, a harness error,
not a skinning error) and compares silhouettes:

| pose / camera | IoU neutral | IoU dance | GPU-only px (dance) | CPU-only px |
|---|---|---|---|---|
| neonPulse 7.3, front | 0.973 | 0.974 | 2 (0.01 %) | 2.7 % |
| streetCurrent 5.1, +35° | 0.952 | 0.956 | 0 | 4.6 % |
| neonPulse 7.3, −35° | 0.962 | 0.958 | 0 | 4.4 % |
| fluidSignal 4.8, +60° | 0.936 | 0.931 | 0 | 7.4 % |
| streetCurrent 24.3, front | 0.973 | 0.969 | 1 | 3.2 % |

The dance pose agrees as well as the neutral pose, GPU-only pixels are ≈ 0, i.e. everything the GPU draws is covered by the CPU
geometry; the CPU-only 3–7 % are alpha-cut-out regions that the GPU discards and my rasteriser does not. Skins have at most 171
joints (palette limit 192). Limits: silhouette level (about 3 mm per pixel at this distance), not per-vertex; only SKIN/CLOTH
materials (hair not checked); depth ordering and alpha blending are not examined.

#### What this changes

* The renderer is **not** the cause of the measured penetration: CPU and GPU skinning agree.
* Penetration is **real** for streetCurrent (hand/fingers into the upper chest) and fluidSignal (forearm into mid-torso), shallow
  (1.6–2.9 cm), and **not confirmed** for neonPulse.
* The refinement does what it was designed for (proxy clearance ≈ 0 after correction) but the proxy is too coarse: hand
  modelled as one sphere, torso at P85. No fix was made here; candidates are more hand samples (fingertips, thumb) and a larger
  torso percentile, to be judged with this tool.
* Not done: arm-vs-hair triangle test (hair sheets), arm-vs-skirt/one-piece layers, a second model.

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

## G2. Targeted fix of the confirmed arm/torso crossings

Scope: improve the existing proxy refinement, no new collision engine. All measurements use `bgmedia_mesh_clip`: one fresh
`DancePlayer` per clip, sequential playback from t = 0 (10 Hz for penetration, 60 Hz for smoothness), whole dances (40 s),
Spring OFF, overlays Full. "Crossing" = an arm-skinned triangle (forearm, hand, fingers, sleeve, glove) crossing a torso-skinned
triangle (rest-pose crossings masked: none).

**What was wrong, per the diagnostics (F.1):** (a) the hand was one sphere at the palm centre, so fingers and thumbs were not
covered; (b) the torso cross-section was an ellipse, but the vest is boxy: its front corners (lateral 9 cm, forward 7 cm at
mid-torso) lie outside the ellipse yet inside the mesh, which is exactly where a forearm folded across the body ends up;
(c) the torso envelope was P85 of the vertices (pouches and straps stick out); (d) the refinement rotated the hand with the forearm.

**Changes (`HumanoidRetargeter.cpp`, all calibrated from the model's own mesh, with the skeleton fallback kept):**

* **Hand control points:** palm centre, index and little knuckles, middle joint, distal joint and extended tip of every finger
  (including the thumb), carried rigidly with the wrist. Finger radius is the P90 distance of finger-skinned vertices to their bone
  segment (Model1: measured, smaller than the palm radius).
* **Torso shape fitted to the mesh:** 5 height bands with correct band centres (the old 3-band interpolation put band centres at the
  wrong heights), P95 per band for half-width / front / back, and a cross-section exponent `p` (|x/a|^p + |z/b|^p = 1) chosen as the
  smallest value in 2…4 for which at most 6 % of the torso vertices lie outside. The general radius was **not** increased blindly:
  the forearm radius went from P85 to P95 (3.3 → 3.5 cm), the torso extents are shape-fitted.
* **Minimal correction:** a forearm point is cleared by translating elbow and wrist together (the old rule pivoted about a fixed
  elbow and needed twice the wrist move); the wrist follows the deepest hand point's normal; the **world orientation of the hand is
  preserved** (its local rotation is re-derived), as are the twist of the upper arm and forearm.
* **Cost guard:** the finger samples are skipped when the wrist is farther from the torso than the whole hand can reach.

**Results** (before = previous proxy, after = this change; same sequential playback):

| dance | frames with triangle crossings | max / total crossing triangles | proxy violations (raw) | max correction | max acceleration of hands/elbows @60 Hz |
|---|---|---|---|---|---|
| neonPulse (**negative control**) | 0 → 0 | 0 → 0 | 0 → 32 | 0 → 9.6 mm | 800 → 800 m/s² (unchanged) |
| streetCurrent | **16 → 0** of 400 | 57 → 0 / 700 → 0 | 10 → 48 | 49 → 126 mm | 67.8 → 67.8 m/s² |
| fluidSignal | **11 → 2** of 400 | 39 → 6 / 132 → 12 | 0 → 18 | 0 → 24 mm | 84.9 → 84.9 m/s² |

* Peak hand/elbow speed and the maximum hand-orientation step per frame are unchanged in all three dances (3.63 / 3.51 m/s,
  0.381 / 0.231 rad); the correction itself changes by at most 13.5 mm (streetCurrent) between 60 Hz frames, with a smooth onset.
* After the correction the proxy clearance is ≥ −1 mm in every frame of every dance (it was already ≥ 0 after correction before,
  so the metric alone showed no problem: the proxy was too coarse, not the solver).
* Forced cross-body test gesture: 42 of 201 frames violate the proxy, all improve, worst −45.7 mm → −2.3 mm; the wrist step of that
  deliberately extreme sweep grew from 5.9 mm to 15.2 mm per 1/200 sweep (limit in the test: 3× the unrefined step + 10 mm).
* Sequential before/after stills of the same four poses (streetCurrent 5.1 s and 24.5 s, fluidSignal 10.8 s, neonPulse 7.3 s):
  `artifacts/qa/arm_fix_before_after.png` (not committed). The change is small by eye (arm moved a few cm out of the chest rig, hand
  orientation and pose character kept); neonPulse looks the same.
* Cost: `applyClip` on the worst-case cross-body pose 0.019 ms without → 0.042 ms with the refinement (before this change 0.019 → 0.019);
  frames with the arm away from the torso take the early out.

**Honest caveats**

* The *vertex*-behind-surface count (nearest-vertex normal) barely moved (streetCurrent 90 → 86 frames, depth 29 → 24 mm;
  neonPulse and fluidSignal unchanged). That metric measures near contact and folds (it reports 79 frames on neonPulse where no
  triangle crosses), so the confirmed measure is the triangle test, not this count.
* **Residual:** fluidSignal still has 2 frames (t = 10.8 s, 30.0 s) with 6 crossing triangles, 16 mm. The forearm sphere touches the
  proxy there (clearance ≈ 0) but the mesh has a local protrusion beyond the P95 envelope; growing the envelope further would push
  arms off the body in ordinary poses (neonPulse already receives 32 small, unneeded corrections of up to 9.6 mm). Not pursued.
* **Large corrections:** streetCurrent has about 5 frames (around t = 24.5 s) where the authored hand is buried 3–4.6 cm in the chest
  and the wrist moves up to 126 mm (p90 115 mm) although the deficit is 46 mm. I tried three alternative solvers for these frames
  (a push direction frozen at the first iteration, the push-weighted mean normal of all violating hand points, and a least-norm
  solution of all violated half-spaces by alternating projections). None beat the plain deepest-point normal (the last gave 148 mm
  and a 4× higher acceleration), so I kept the simplest one and stopped rather than add iterations or radius. The reason is
  geometric: a hand buried near the medial axis of the torso has no consistent exit normal, so any linearised push either detours or
  oscillates. Whether 126 mm for 0.5 s is acceptable artistically is open; the 35 %-of-arm-length clamp was not triggered.
* neonPulse (hand/elbow max speed 13.4 m/s, acceleration 800 m/s², hand orientation step 0.67 rad/frame) is unrelated to this change
  (it is identical with the refinement off) and points at a pre-existing discontinuity in that clip's playback; not investigated.
* Only arm-vs-torso triangles were tested, not hair or other clothing layers, and only one model.

## G3. Eyes visible through the back of the head (separate defect)

**Reproduced** with a neutral pose (so it is not a head turn) and identical cameras. From behind (`bgmedia_vrm_preview --player idle
--yaw 0`) the eyes, irises and brows were drawn on top of the back of the head and the hair, also with the hair hidden; with every material
forced opaque (`--opaque`) they disappeared. Images: `artifacts/qa/eyes_idle_front_back_BEFORE.png` and `..._AFTER.png`
(rows: normal, hair hidden, opaque override, opaque + hair hidden; columns: camera behind / in front), `eyes_front_face_before_after.png`.

Investigation, in the order requested:

| item | finding |
|---|---|
| Head/eye orientation | Not the cause: the defect is identical in the neutral pose; the front/back cameras differ by exactly 180° |
| Skinning | Not the cause: eye, brow, lash, eye-line and skin layers share the face vertex buffer (4419 vertices), 99 %+ weighted to Head/Neck and, for the lids, the eye bones; hair 19 664 of 19 664 vertices on head/neck/hair chains (test) |
| Materials / alpha | Eye layers (iris, highlight, brows, lashes, eye line) are BLEND; face skin, eye white and hair are MASK. Forcing everything opaque removed the defect, which points at the blended layers, not at an alpha-cutout hole in the head |
| Depth test | **Cause.** `makeMeshPipeline` used `compare_op = SDL_GPU_COMPAREOP_ALWAYS` for blended pipelines (depth test "enabled" but never rejecting), so those layers ignore the opaque depth buffer entirely. The code comment claimed the opposite |
| Depth write | Correctly off for blended layers (kept) |
| Render order | Opaque pass first, then blended primitives sorted back-to-front by bind centroid (fine; kept) |
| Culling | `CULLMODE_NONE` for all pipelines; the material's `doubleSided` flag is ignored (unused-parameter warning), so single-sided materials are drawn double-sided. Not the cause here; noted, not changed |

**Fix (local):** blended pipelines now use `COMPAREOP_LESS_OR_EQUAL` with depth write still off (`SdlGpuVrmRenderer.cpp`). Nothing else changed, no
depth bias.

**Check, permanent:** `bgmedia_skin_check --eye-occlusion` renders the neutral pose with and without the eye/brow/lash/eye-line layers from seven camera
yaws and counts changed pixels. Behind the head the count must be 0, from the front it must be > 100.

| camera | before fix (px changed when eye layers hidden) | after fix |
|---|---|---|
| front 180° / 160° | 500 / 491 | 220 / 221 |
| behind 0° / +20° / −20° | **387 / 386 / 382 (visible through the head)** | **0 / 0 / 0** |
| side +90° / −90° | 157 / 148 | 0 / 0 |

Side effects seen: from the front the eyes, irises, lashes and brows are still fully visible, with no z-fighting; the faint green rims below the
eyes that the old renderer drew over the eyelid skin are gone (they were part of the same artifact); some other blended clothing/face layers are
now correctly occluded too (about 1 000 pixels differ in the idle view). The IoU check (G of the CPU/GPU section) did not cover this defect, as
expected: it compared silhouettes of skin and clothing, not depth or transparency.

Not done: other blended materials in other models (a model with a legitimately see-through blended layer behind opaque geometry would now
be occluded, which is the correct behaviour but unverified), GPU timing before/after (one frame at ~4 ms on this machine, not compared), the
Desktop app was rebuilt but not launched.

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
| `bgmedia_tests` | 141 | 175 |
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

## P. Follow-up pass: Nemesis Morrigan, real MP3, leg crossings, seam fix

Tooling: `bgmedia_motion_diag` (instrumented playback: per-joint speed/acceleration/jerk, spike attribution to clip step / foot-target step / overlay / warp / clip switch, foot slide and ground drag, mesh leg and arm crossings through `tools/support/MeshProbe.hpp`). All numbers: Desktop mode (crossfade from the previous clip), 0-44 s of the real MP3, first 4 s skipped.

### P.1 Fixed and committed (7961a67)
- Overlay yank at the 32-beat loop seam: a cue that ended exactly at the seam dropped its weight in one frame. Cue `influence` now ramps over `kYieldBeats = 0.5`; the regression test fails on the old code (34-82 rad/s non-hand steps), passes now (<= 20 rad/s at 30/60/120 FPS, 80/136/170 BPM).
- Procedural leg-through-leg: footwork planner (`planClearance`, per-leg proxy `legClearanceCm`, `Footwork::setOther`) picks shrink/shift/bump per step; side-touch returns the trailing foot first. Procedural dances on Model1: 0 leg crossing frames. On Nemesis only 7-121 frames with <= 28 triangles (thigh skin contact, not penetration through the leg).

### P.2 Not solved: authored Hip-Hop and Side Step
Mesh crossings are unchanged from the baseline: Nemesis hiphop 110 frames / 601 tris, sidestep 73 / 340; Model1 hiphop 44 / 531, sidestep 34 / 350.
Root cause (measured, Nemesis hiphop 6.0-6.5 s): the authored data plants the right foot at +16.3 cm, across the body midline, while the left foot swings through it. In 51 (hiphop) and 189 (sidestep) sampled frames BOTH feet are planted and overlapping, so moving a free foot cannot help.
An experimental runtime corrector (`RetargetOptions::legCollision`, capsule thigh/shin/foot calibrated from the mesh, free-foot push) was built and evaluated in four variants. Best result: crossings 601 -> 238 tris but ankle speed 1.2 -> 9-25 m/s and ground drag 0 -> 170-720 cm. It fails the gates (corrected frames about equal to crossing frames; ankle speed <= 1.5x baseline; ground drag not above baseline), so it ships DISABLED (`legCollision = false`); `bgmedia_motion_diag --leg-proxy` enables it for experiments.
Needed next: a stance-level pre-pass over the authored contact schedule (one lateral offset per landing, held for the planted run, ramped during the swing with a small lift, 0 if not clearable). Out of scope for this pass.

### P.3 Hand jerks (Neon Pulse, Energetic)
After the seam fix the remaining acceleration peaks are periodic (once per 16 beats at a fixed clip phase: Energetic phase 0.75, Neon Pulse 0.118), with no overlay, crossfade or warp involved and zero angular step: they are authored accent gestures (Energetic RHand 10 m/s peak, Neon LHand 4.8 m/s), not discontinuities. No limiter added.

### P.4 Verification
- Full build incl. `bgmedia_desktop.exe`, 178 unit tests, CTest 16/16, ASan 178/178 (exit 0).
- BG Motion 3.1 sync QA re-run: identical to `docs/sync_qa_3_1.json` except timing fields.
- Not done in this pass: video / frame-sequence review of the final build.

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
11. **ASan:** AddressSanitizer Debug build (`/fsanitize=address`, tests + tools; the Desktop/SDL targets are not part of that build, so
    `bgmedia_skin_check` and the renderer change were not run under ASan) of the full suite at commit `1f7ab4c`: 175 tests passed, no ASan
    reports. `scripts/run_asan_windows.ps1` itself fails here (`vswhere` not on PATH), so an equivalent invocation was used.
12. **Desktop:** `bgmedia_desktop.exe` now builds and links (rebuilt after the app was closed); it was not launched or exercised.
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
d7dda88 Document the quality pass; add QA metrics; rename spatial tests for CTest filtering
c952f61 Follow-ups: signed wrist/thumb checks, sync_qa regression diff, dance-level spring FPS test,
        --hide-material preview aid, corrected mesh-clipping findings
```

The working tree otherwise contains only the two pre-existing untracked archives (`BG Media.zip`,
`src.zip`), which were not touched. No push, no PR, no merge.
