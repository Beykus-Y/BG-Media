# BG Motion 3.2 вЂ” Procedural Choreography & Character Dynamics

Status: implemented, tested and visually verified on the private local model
`models/Model1.vrm` (VRM 0.x, UniGLTF 2.44.0). The model itself is not part of
the repository.

BG Motion 3.2 keeps the BG Beat в†’ BG Sync в†’ BG Motion 3.1 musical chain and adds
four things on top of it:

1. a **procedural choreography generator** that composes full dances from
   motifs, phrases and sections;
2. a **motion primitive library + compositor** that layers independent gestures
   (arms, hands, torso, shoulders, head, footwork) onto a full dance;
3. a **detailed hand/finger layer** with per-finger, per-joint articulation and
   a graceful fallback when a VRM has no finger bones;
4. a **VRM 0.x secondary-dynamics layer** for spring bones, colliders and the
   supported soft-tissue chains of the model.

The implementation is local C++20. It does not use motion capture, Mixamo, ready
made dance files, an external LLM or an AI motion generator. Randomness is
limited to deterministic choice inside the generator; identical inputs produce
identical output.

---

## 1. Honest audit of BG Motion 3.1

Before 3.2 the animation path was:

```
HumanoidClip (baked quaternion tracks per core humanoid bone)
  в†’ DanceLibrary
  в†’ DanceDirector / DanceSchedule (musical grid + time warp)
  в†’ ClipPose (24 core bones + stage-space feet + scalar hand curl)
  в†’ HumanoidRetargeter::applyClip (foot IK, root motion, toe/hip solve)
  в†’ RetargetedPose::nodeGlobals
  в†’ SDL_GPU skinning
```

What was already good:

- 24 core VRM humanoid bones were animated.
- Foot targets were stage-space and IK-solved, so planted feet did not skate.
- Root motion and yaw were applied outside the pelvis, so contacts stayed valid.
- Clips were model-independent and loop/contact validated.
- Musical timing, downbeats, accents and time warp were in place.

What was missing or visibly weak:

| Defect | 3.1 reality | 3.2 response |
|---|---|---|
| Repeated movement | Clips were authored loops of 8вЂ“16 beats; long arm holds looked mechanical | Phrase generator with rotating motifs, per-beat arm planes and four sections |
| Pelvis / weight | Hips followed feet, but only a small authored amount | Per-foot load smearing, auto balance, root travel follow, pelvis roll |
| Rigid chest | Spine/chest/upperChest were keyed but amplitudes were small | Independent pelvis/spine/chest/upperChest rotation, opposition, wave lag |
| Hands | One scalar curl per hand | Per-finger/per-joint pose, wrist, spread and thumb opposition |
| Fingers | Finger bones were discovered by the retargeter but only moved as one block | Detailed hand tracks + overlay layer; truthful fallback |
| Hair / cloth | Not simulated at all | VRM 0.x spring bone chains + collider spheres |
| Motion variety | Five JSON clips with fixed keys | 27 primitives + 3 deterministic generated dances + overlay planner |
| Transitions | Slerp crossfade of two sampled poses | Same crossfade, plus contact-safe composition and phase-preserving overlays |

### Model capability audit

The local test model is not a generic assumption. Its real capabilities are:

| Capability | Count / detail |
|---|---|
| Nodes | 175 |
| Meshes / triangles | 3 / 35 881 |
| VRM humanoid entries | 54 (24 core + 30 finger bones) |
| Core humanoid bones | 24, including `upperChest` and toes |
| Finger bones | 30 (thumb, index, middle, ring, little; 3 joints each, both hands) |
| Spring bone groups | 25 (`Hair`, `TopsUpperArm`/coat skirt, `Bust`) |
| Linear spring chains | 36 (22 hair, 12 coat/skirt, 2 bust) |
| Spring-simulated bones | 116 |
| Collider groups / sphere colliders | 10 / 22 |
| Blend shape groups | 14 (visemes, blink, emotions) |
| Secondary animation schema | `extensions.VRM.secondaryAnimation` (VRM 0.x), not `VRMC_springBone` |

The model has **no dedicated skirt deformable mesh** beyond the coat/skirt spring
chains and no separate вЂњsoft tissueвЂќ bones outside the two `Bust` chains. Those
two chains are simulated, but they are one-node chains, so the result is an
inertial tilt, not a full soft-body deformation. No claim is made beyond that.

The synthetic `tests/fixtures/minimal.vrm` has none of these optional features.
That path is used to test the fallback behaviour.

---

## 2. Data model changes

### 2.1 Clip format v2

`bgmotion.clip` version 2 is a **backward-compatible extension** of v1:

- v1 readers still get the same core tracks.
- v2 adds optional `tracks.footLeft/footRight` 5th lane (`toe` roll, -1..1).
- v2 adds optional `tracks.handPoseLeft/handPoseRight`:
  - 15 floats: `circle[finger][joint]` curl, 0 open .. 1 closed;
  - 5 floats: per-finger spread, -1..1;
  - 1 float: thumb opposition, 0..1;
  - 3 floats: wrist pitch/yaw/roll, -1..1.
- `formatVersion` is 2 for newly written clips; the loader accepts 1 and 2.

The writer (`clipToJson` / `saveClipFile`) nests bone tracks under
`tracks.bones`, matching the v1 authoring format. The round-trip is covered by
`motion32.generated_clip_roundtrips_through_json_v2`.

### 2.2 Hand pose

`motion::HandPose` is the runtime hand state. It is blended with linear
interpolation and clamped to joint ranges. The retargeter applies it to the
VRM finger nodes:

- proximal/intermediate/distal curl per finger;
- fan/spread on the proximal joint;
- thumb opposition;
- wrist pitch/yaw/roll on the hand bone.

If the model has zero finger bones, `RetargetStats::detailedHands` stays false
and no fake finger motion is produced.

### 2.3 Spring rig data

`VrmModel` now stores:

- `fingerNodes[2][5][3]`;
- `springGroups`, `springColliderGroups`, `springChains`, `springSource`;
- diagnostics counts for all of the above.

The loader parses `extensions.VRM.secondaryAnimation` (VRM 0.x), including the
spec's `stiffiness` spelling, `gravityPower`, `gravityDir`, `dragForce`,
`hitRadius`, `center`, `boneGroups`, `colliderGroups`, offsets and radii.
Malformed references are warnings, never crashes. Groups are expanded into
linear chains; a node that is a root of another group starts a new chain, so a
transform is never simulated twice.

---

## 3. Motion primitive library

A primitive is a small procedural motion, not a pose preset. Every primitive has
metadata:

```cpp
struct PrimitiveMetadata {
    PrimitiveKind kind;
    const char* id;
    const char* name;
    MotionCategory category;   // Arm / Hand / Torso / Shoulder / Head / Footwork / FullBody
    BodyMask mask;             // Pelvis, Spine, Chest, Shoulders, Head, arms, hands, legs, feet
    BlendMode blend;           // Additive, Override, Masked
    float defaultBeats, minBeats, maxBeats;
    float minIntensity, maxIntensity;
    int priority;
    bool repeatable;
    bool accentAffinity;
    bool needsSupport;
    bool altersFeet;
    bool needsFingers;
    const char* description;
};
```

The shipped library contains **27 primitives**:

| Category | Primitives |
|---|---|
| Arm | `armWave`, `armSwing`, `armReach`, `pointGesture` |
| Hand | `handFlick`, `wristRoll`, `fingerRipple`, `fingerSpread`, `fistPulse` |
| Torso | `chestIsolation`, `torsoTwist`, `bodyWave`, `sideLean`, `upperBodyBounce`, `hipSway` |
| Shoulder | `shoulderAccent`, `shoulderRoll` |
| Head | `headNod`, `headTilt`, `lookDirection`, `headAccent` |
| Footwork | `sideStep`, `stepTouch`, `heelTap`, `toeTap`, `kneeBounce` |
| Full body | `weightShift` |

They differ mechanically, not only by amplitude:

- `armWave` travels shoulder в†’ elbow в†’ wrist with a per-side phase lag;
- `armSwing` counter-swings the arms with elbow follow-through;
- `handFlick` snaps the wrist and fans the fingers, then relaxes;
- `wristRoll` rotates the hand around the forearm axis;
- `chestIsolation` moves the ribcage against a stable pelvis;
- `torsoTwist` counter-rotates pelvis and chest;
- `bodyWave` delays each vertebra group;
- `weightShift` transfers the body over the loaded leg without moving a foot;
- `sideStep` lifts and places a swing foot; it is rejected if that foot is planted;
- `heelTap` / `toeTap` use the authored toe lane;
- `fingerRipple` gives each finger a different phase;
- `fingerSpread` opens from the middle outward;
- `fistPulse` closes on the hit and releases.

### 3.1 Composition and conflict resolution

`composeMotion()` is the only layer that writes a final pose from a base
choreography plus cues. It:

1. resolves the cue time on the looped clip beat; a cue at beat 15.75 on a
   16-beat loop is still active at beat 0.1;
2. computes an attack/release envelope, so gestures fade in and out;
3. narrows a two-sided cue to the requested side;
4. rejects a cue whose mask is empty or whose category is disabled by the UI
   mode;
5. rejects footwork that would move a planted foot
   (`rejectedByContact`);
6. rejects `needsSupport` gestures when neither foot is planted;
7. resolves same-region overlaps by priority; lower priority loses, equal
   priority keeps the earlier scheduled cue, which makes the plan deterministic;
8. caps the number of upper-body layers;
9. applies rotations with quaternion composition
   `q = slerp(identity, delta, weight) * q`, never by adding Euler angles;
10. adds hip offsets and only lifts a foot whose base contact is below the
    planted threshold.

The result carries diagnostics: active cues, accepted/rejected counts, max
weight and per-bone layer counts. These are surfaced in the desktop debug panel
(`LYR accepted/total`).

### 3.2 Overlay planning

`planPrimitiveOverlays(clip, seed, density, includeHands)` is deterministic from
the clip id, its accents, its bar structure and the seed. It does not place a
gesture on every beat:

- bar starts get a shoulder or chest accent;
- off-beats get hand gestures and occasional head answers;
- every other bar gets an arm wave or reach;
- authored accents with strength в‰Ґ 0.55 get a short head/fist accent exactly on
  the accent.

The generated showcase clips have their own richer overlay plan (46вЂ“57 cues for
32 beats); the authored clips get a lighter plan (в‰€20вЂ“30 cues).

---

## 4. Procedural choreography generator

Header: `include/bgmedia/motion3/ProceduralChoreography.hpp`.

Input:

```cpp
ChoreographyRequest {
    id, name, style, profile;     // neon / street / fluid
    bpm; beatsPerBar; bars;       // default 8 bars = 32 beats
    seed; intensity; keysPerBeat;
    includeOverlays; loop;
}
```

Output:

```cpp
GeneratedDance {
    HumanoidClip clip;            // baked, model independent, JSON-serialisable
    std::vector<PrimitiveCue> overlays;
    std::vector<std::string> structure;
    ChoreographyStats stats;
}
```

### 4.1 Structure

`Motif в†’ Phrase в†’ Section в†’ Full Dance`.

The default 8-bar dance uses:

| Section | Bars | Content |
|---|---|---|
| Intro | 1 | weight shift, open/fold arm motif, no travel |
| Development A | 2 | side-touch footwork, counter-swinging arms |
| Development B | 2 | cross-step, close, diagonal reaches |
| Build | 1 | pivot turn on both loaded feet, overhead arms |
| Resolve | 2 | heel/toe taps, low pumps, settle |

For a 4-bar request the generator only emits the sections that fit; for 6 bars
it stops before the pivot/heel phrase. Every foot pattern is authored to return
to its start, and the last key is forced to equal the first, so the loop seam is
exactly zero.

### 4.2 Body mechanics in the baked clip

The generator is a port and extension of the authoring DSL in
`tools/dancegen/bgdance.py`, now running inside the engine:

- **Footwork**: hold / step / pivot / orbit segments, stage-space targets,
  contact segments with pivot flags, lifted swing feet and toe roll.
- **Weight transfer**: per-foot swing masks are smoothed with a Gaussian kernel
  (`Пѓ = 0.22` beat). The hips follow the loaded foot (`com_x/com_z`), the pelvis
  rolls toward the loaded leg, and the shoulders counter-tilt.
- **Root travel**: the root follows 24% of the travelled centre of mass in X and
  18% in Z, so a side step moves the character instead of stretching both legs.
  This is kinematic approximation, not a physics simulation.
- **Pelvis / spine / chest**: independent rotations with weights 0.30/0.34/0.36,
  pelvisвЂ“chest opposition, lateral ribcage isolation, and a three-group body wave
  with phase lag.
- **Shoulders**: independent lift/forward keys, opposite the lead side.
- **Head**: nod on the beat, delayed off-beat release, look/tilt per phrase.
- **Arms**: elevation/azimuth/twist/elbow/wrist controls. Keys select planes
  (open, overhead, folded, low/back, diagonal, sweep, point) and swap sides.
  Curves are shape-preserving monotone cubics, so a hold stays a hold and an
  accent lands on its key.
- **Pivot turn**: both feet pivot in place (contact kind `pivot`), the root yaw
  turns up to 22В° and returns to zero before the loop.

### 4.3 Determinism

The only randomness is a seeded 32-bit LCG used for:

- side-touch / cross-step direction,
- arm jitter (В±5В°),
- profile variation.

No random choice is made per frame. `motion32.generator_is_deterministic` checks
that two requests with the same fields produce byte-identical JSON and identical
sampled poses. `motion32.different_seeds_and_profiles_differ` checks that two
seeds produce a different dance (peak difference > 0.035 rad).

### 4.4 Save / load / replay

```cpp
GeneratedDance dance = ProceduralChoreography::generate(request);
ProceduralChoreography::save(dance, "assets/generated/neonPulse.json");
// or:
motion::saveClipFile(dance.clip, path);
```

The runtime never calls a model or a service. It can replay a generated clip
through `DanceLibrary` exactly like an authored one. The desktop app generates
three dances at startup; the `GEN` button cycles them. The authoring tool
`bgmedia_procedural_dance --write <dir>` writes them to disk for inspection or
for a build that wants frozen assets.

### 4.5 Shipped procedural content

| Id | Name | BPM | Beats | Profile | Seed | Overlay cues |
|---|---|---|---|---|---|---|
| `neonPulse` | Neon Pulse | 104 | 32 | neon | 7 | 57 |
| `streetCurrent` | Street Current | 96 | 32 | street | 23 | 49 |
| `fluidSignal` | Fluid Signal | 112 | 32 | fluid | 41 | 46 |

`neonPulse` is the reference full dance: 4 phrases, side-touch, cross-step,
pivot turn, heel/toe, four arm planes, 17 accents and 57 overlay gestures.

---

## 5. Fingers and hands

The retargeter already discovered finger nodes from `humanoidEntries`; 3.2 uses
them fully:

- detailed hand pose is applied per finger and per joint;
- the wrist gets pitch/yaw/roll on top of the clip's hand rotation;
- spread fans the fingers around the palm normal;
- thumb opposition brings the thumb across the palm;
- each joint is clamped; no hyper-extension is generated;
- bind-pose local axes are still used for retargeting, so a different VRM with
  different bone orientations is handled by the same code;
- if a finger node is missing, only that joint is skipped;
- if all are missing, `RetargetStats::detailedHands` is false and no hand
  animation is claimed.

The desktop debug panel reports the finger bone count. The test model has 30.

---

## 6. Secondary dynamics (VRM spring bones)

Header: `include/bgmedia/animation/SecondaryDynamics.hpp`.

### 6.1 Scope

Supported: VRM 0.x `extensions.VRM.secondaryAnimation` вЂ” bone groups, collider
groups, chain roots, stiffness, gravity, drag, hit radius.

Not supported: VRM 1.0 `VRMC_springBone` (this build's importer is VRM 0.x only),
cloth meshes without spring bones, and full FEM / mass-spring cloth.

### 6.2 Solver

The solver is a position-based chain with a fixed 120 Hz timestep:

1. read the **final** animated node globals from `RetargetedPose`;
2. convert them to stage space using `rootOffset`, `rootPivot` and
   `modelTransform`, so root motion and root yaw actually drive the springs;
3. for each node, semi-implicit integrate:
   `v += (animated - current) * stiffness * h; v *= exp(-drag * h); v += gravity * h; current += v * h`;
4. clamp velocity and displacement from the animated target;
5. project segment lengths while keeping the root anchored;
6. push nodes out of every authored collider sphere (with `hitRadius`);
7. convert back to character space, write node globals and a direction-derived
   rotation back into the pose.

The displacement limit is `min(settings.maxDisplacement, 0.45 Г— chain rest
length)`. This is essential: a 4 cm hair chain cannot be allowed to move 12 cm
without reading as an explosion.

### 6.3 Stability controls

- fixed substep 1/120 s;
- max 8 substeps per frame;
- accumulated time clamped to 0.1 s;
- velocity clamp 12 m/s;
- exponential drag from `dragForce` + the user damping preset;
- per-chain displacement clamp;
- NaN/Inf detection; on failure the chain is reset to its animated pose;
- reset on seek, teleport, pause/resume discontinuity, model reload and clip
  change;
- when disabled, the pose is left untouched and the accumulator is zeroed.

The tests cover 30/60/120/180 FPS, 3-second runs, frame-rate comparison,
collision overlap, reset, disabled, no-spring-rig fallback and NaN checks.

### 6.4 What is simulated on the test model

- 22 hair chains: visible lag and return after head/body rotation and root turns;
- 12 coat/skirt chains: follow the hip/leg motion with their authored stiffness;
- 2 bust chains: one-node inertial tilt, moderate stiffness;
- 22 collider spheres on spine, upper chest, neck, head, upper arms, forearms and
  hands.

No global mesh deformation is done. The skinned mesh follows the bone transforms
through the existing renderer.

---

## 7. Update order

The implemented order is:

1. musical time (BeatClock / MusicalTimeline / time warp);
2. DanceSchedule sampling of the base clip;
3. motion overlay composition (`composeMotion`);
4. humanoid retargeting (`applyClip`), foot IK and root motion;
5. secondary dynamics (`SecondaryDynamics::update`) on the final node globals;
6. skinning matrices in the renderer;
7. render.

Secondary dynamics is deliberately after retargeting: it must react to the pose
that is actually rendered, and no later stage is allowed to overwrite it. The
renderer consumes `RetargetedPose::nodeGlobals` for both humanoid and spring
joints, so no extra upload path is needed.

---

## 8. Musical integration and time warp

- The generator and overlay planner work in clip beats; the existing
  `DanceSchedule` maps music time to clip beats through the BG Beat grid.
- `frame.clipBeat` is the warped beat. Overlays use it, so a gesture stays on
  the musical accent instead of drifting with the warp.
- Foot targets, root motion, yaw and contact segments come from the stable
  unwarped grid (`stable` / `warped` split in `poseOf`). This is unchanged from
  3.1 and keeps foot contacts valid under `Off / Soft / Expressive`.
- Upper-body-only warp still avoids double-warping the lower body.
- Overlays are evaluated after the warp, with their own envelope, so a cue
  cannot accelerate a planted foot or move the root.
- Authored accents are used by the planner; the overlay planner also uses bar
  boundaries and clip energy.

---

## 9. Transitions

The existing crossfade is retained:

- `blendPoses` uses shortest-arc slerp for bone rotations, arc interpolation for
  root and foot yaw, and linear interpolation for positions;
- `smootherstep` on the crossfade parameter gives zero velocity at both ends,
  which suppresses the worst вЂњsnapвЂќ without a full inertialization solver;
- overlays are evaluated on the destination clip's phase, so they do not jump
  when the base changes;
- planted-foot contact is preserved by the compositor; a footwork overlay is
  rejected while that foot is planted;
- seek/rebase/reseed are still pure functions of time and do not accumulate
  hidden state.

No claim of full inertialization (velocity-matched offset decay) is made. This
is listed as a remaining limitation.

---

## 10. Desktop UI

The existing panel was extended, not redesigned:

- `LAYER` cycles `OFF в†’ LIGHT в†’ FULL в†’ HANDS`;
- `GEN` cycles the three procedural dances (and off);
- `SPRING` toggles VRM secondary dynamics; `S-` / `S+` adjust intensity,
  `DAMP` cycles a damping preset and `RESET SPRING` clears the simulation;
- the existing debug views (`F5` feet, `F6` skeleton, `F7` root) remain; with
  `F6` active the spring chains are drawn in amber;
- the debug panel reports `LYR active/total`, spring chains, max displacement,
  simulation time and finger bone count;
- the clip state shows the generated dance name and its phase/accent markers;
- keyboard shortcuts: `Y` layer, `J` generated, `K` spring;
- layer mode, spring enable/intensity/damping are persisted to
  `motion32-profile.cfg` next to the existing warp profile and restored on the
  next launch.

The settings are not decorative: the overlay mode changes composition, the
spring toggle changes `SecondaryDynamics`, and the generated selector changes the
DanceDirector schedule.

---

## 11. Tests and QA

### Unit / integration tests

`build/windows/bin/bgmedia_tests.exe` в†’ **141 passed, 0 failed** (Release).

The BG Motion 3.2 group (`motion32.*`) covers:

- generator determinism, seed/profile difference, phrase length, loop seam;
- JSON v2 round-trip (hands, toe, contacts, accents);
- QA bounds (speed, acceleration, root, foot, slide);
- primitive metadata completeness, 27 entries, unique ids;
- body masks and conflict rules;
- planted-foot rejection;
- arm + hand layer composition with normalized quaternions;
- loop-wrapped cues;
- overlay plan determinism and downbeat presence;
- model capability counts (30 fingers, 36 spring chains, 116 spring bones);
- detailed finger motion vs. fallback;
- spring stability at 30/60/120/180 FPS, collision, reset, disabled, no rig;
- fixed-timestep near-invariance;
- hand pose clamping;
- generated-to-generated transitions;
- player installation of three generated dances;
- overlay changes pose but not musical phase/root;
- seek/rebase finiteness.

### ASan

`build/asan/bin/bgmedia_tests.exe` with `/fsanitize=address` (MSVC):
**141 passed, 0 failed**, no ASan report. Run under the VS environment so the
ASan runtime DLL is on `PATH`.

### Motion QA

`docs/motion_qa_3_2.txt` and `docs/motion_qa_3_2.json` are generated by:

```powershell
bgmedia_motion_qa --dances assets/dances --model models/Model1.vrm `
  --json docs/motion_qa_3_2.json
```

Key numbers for the generated reference dance at its authored 104 BPM:

| Metric | neonPulse |
|---|---|
| Loop rotation seam | 0.000В° |
| Loop position seam | 0.0000 height units |
| Max angular speed | 607 В°/s |
| Max angular acceleration | 15 081 В°/sВІ |
| Max hips speed | 0.72 heights/s |
| Max foot speed | 2.38 heights/s |
| Planted ankle slide | 3.43 mm |
| Foot IK error | 0.00 mm |
| Reach drop | 0.06 cm |
| Foot swings | 6 |
| Swing path length | 1.77 height units |
| Root excursion | 0.04 height units |
| Root yaw | 22В° (returns to 0) |

The slide is measured on the retargeted ankle over contact segments; it is below
the 4 mm validator threshold.

### Spring QA

`docs/motion_qa_3_2.json` contains, for each generated dance and FPS:

| Clip | FPS | Max displacement | Max stretch | Sim time | Substeps |
|---|---:|---:|---:|---:|---:|
| neonPulse | 30 | 5.00 cm | 0.000% | 0.19 ms | 4 |
| neonPulse | 60 | 5.00 cm | 0.009% | 0.12 ms | 2 |
| neonPulse | 120 | 5.00 cm | 0.197% | 0.09 ms | 1 |
| neonPulse | 180 | 5.00 cm | 0.233% | 0.09 ms | 1 |
| streetCurrent | 30/60/120/180 | 5.00 cm | в‰¤ 0.200% | в‰¤ 0.15 ms | 4/2/1/1 |
| fluidSignal | 30/60/120/180 | 5.00 cm | в‰¤ 0.199% | в‰¤ 0.15 ms | 4/2/1/1 |

All frames finite, all 36 chains, 116 bones, no NaN. вЂњ5.00 cmвЂќ is the configured
per-chain displacement cap, not an unbounded result.

### Performance

Measured on the local Windows desktop build (Release):

| Operation | Measured |
|---|---|
| Full generated dance (32 beats, 257 keys, 14 bone tracks) | в‰€ 7.5вЂ“9 ms |
| Overlay composition, 57 cues | в‰€ 1.3 Вµs/frame |
| Spring simulation, 116 bones / 36 chains | 0.06вЂ“0.15 ms/frame |
| Retargeting | в‰€ 0.01 ms/frame |
| 3D capture render (single frame, measured in desktop capture) | в‰€ 0.6 ms GPU, 461 FPS reported |

The generator runs once at library load (or in the authoring tool), not per
frame. Runtime sampling is a pure function of beat/time. No heavy physics engine
or new third-party dependency was added.

### Visual verification

Frames and a demo reel were rendered with `bgmedia_vrm_preview`:

- `artifacts/reel/bg_motion_3_2_demo.mp4` (в‰€52 s, 720Г—450, 30 FPS):
  baseline Groove в†’ Neon Pulse в†’ generated transition в†’ hand close-up в†’ spring
  off в†’ spring on.
- contact sheets and stills in `artifacts/motion32/` and `artifacts/reel/`
  (local only; not committed because the VRM model is private).

The frames were inspected for foot skating, elbow inversion, hand/body
penetration, hair explosion, balance loss and mechanical repetition. The
remaining issues are listed below.

---

## 12. Compatibility

- **Old clips**: version 1 clips load unchanged. Their scalar hand curl is used;
  no detailed hand pose is invented. Existing Groove / Hip-Hop / Side Step /
  Energetic / Idle choreography keeps its exact key tracks.
- **DanceDirector / DancePlayer**: existing API remains; new methods are
  additive (`setOverlayMode`, `selectClip`, `overlaysFor`, `generatedDances`).
- **BG Motion 3.1**: MusicalTimeline, BeatClock, accent map and warp are
  untouched. The overlay layer reads the already-warped clip beat.
- **CharacterPose 2.0**: the legacy 2D path is unchanged. Secondary dynamics is
  applied to both legacy and clip retargeting in the desktop scene, but the
  generated choreography is a clip feature.
- **Models**: any VRM 0.x with the required core humanoid bones can play the
  generated clips. Fingers and spring bones are optional; absence is reported,
  not faked.

---

## 13. Known limitations

1. **No full inertialization.** Crossfades use zero-velocity smoothing, not a
   velocity-matched offset decay. A deliberately hard cut still produces a
   visible pose change.
2. **Foot contact during a crossfade** relies on entry/exit beats that match the
   footwork. If two clips are forced together at arbitrary foot phases, the
   blend can move a planted target for a few frames. The compositor protects
   overlays, but the base crossfade itself is a pose blend.
3. **Spring reset on seek.** A seek resets the spring state to the current pose;
   the history before the seek is not replayed. This is stable and explicit, but
   not a bit-exact reconstruction of the pre-seek motion.
4. **Hair/cloth is bone-level**, not mesh deformation. The test model has 36
   chains and 22 collider spheres; another model with no spring bones gets no
   secondary motion.
5. **Bust dynamics is a one-node tilt** on this model, not soft-body tissue.
6. **No facial animation.** The model has 14 blend shapes; they are not driven
   by the dance layer.
7. **The dance style set is still small.** The generator has three profiles and
   a finite motif set. It produces meaningful variation, but not an unlimited
   dance vocabulary.
8. **Upper body dominates visually.** Footwork is real and contact-safe, but the
   generated patterns are side-touch / cross-step / pivot / heel-toe, not
   advanced floorwork or partner work.
9. **Root travel is kinematic.** It follows the centre of mass by a fixed gain;
   there is no true support-polygon or centre-of-pressure solver.
10. **VRM 1.0 `VRMC_springBone` is not supported** by this importer.
11. **Visual QA is frame-based.** A 52 s reel and contact sheets were inspected,
    but there is no automated perceptual critic.

---

## 14. Commands

```powershell
# Build (Release, desktop + tests + tools)
cmake --preset release
cmake --build build/release --parallel

# Tests
build/release/bin/bgmedia_tests.exe

# ASan (MSVC)
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DBGMEDIA_BUILD_DESKTOP=OFF -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi" `
  -DCMAKE_EXE_LINKER_FLAGS="/fsanitize=address"
cmake --build build/asan --target bgmedia_tests --parallel

# Authoring: generate and validate the three showcase dances
build/windows/bin/bgmedia_procedural_dance.exe
build/windows/bin/bgmedia_procedural_dance.exe --write assets/generated

# QA (text + machine readable)
build/windows/bin/bgmedia_motion_qa.exe --dances assets/dances `
  --model models/Model1.vrm --json docs/motion_qa_3_2.json

# Desktop, generated dance and secondary dynamics
build/windows/bin/bgmedia_desktop.exe --vrm models/Model1.vrm --3d `
  --dance neonPulse --dance-bpm 104 --debug-skeleton

# Headless frames / demo
build/windows/bin/bgmedia_vrm_preview.exe --model models/Model1.vrm `
  --player neonPulse --overlay 1 --spring 1 --time 0 --frames 420 --fps 30 `
  --framedir artifacts/reel/neon
```

