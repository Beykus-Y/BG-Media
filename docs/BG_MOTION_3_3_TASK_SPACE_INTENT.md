# BG Motion 3.3 — Task-space gesture intent bridge (issue #4)

Scope of this change: a pure, model-independent extraction of per-arm task-space
intent from a sampled `ClipPose`. It is a library API with tests. It is **not**
wired into `DancePlayer` yet, so the runtime output is unchanged by construction.
The collision-free planner (#5) and the elbow swivel solver (#6) are not part of
this change.

Files: `include/bgmedia/motion3/TaskSpaceIntent.hpp`,
`src/motion3/TaskSpaceIntent.cpp`, `tests/test_intent.cpp`.

## Why

Clips and the 27 primitives store arms as joint rotations. A planner that keeps a
hand away from the torso needs the arm as a task: where the wrist is, how the hand
is oriented, and which way the elbow bends. The bridge derives that task by forward
kinematics, without rewriting any stored clip or primitive.

## Contract

- **Input:** a `ClipPose` (pre-IK, pre-collision). Legacy clips and primitives are
  sampled as they are today.
- **Output:** `TaskSpaceIntent::arm[2]` (`[0]` left, `[1]` right), each an `ArmIntent`:
  - `wrist`: wrist position relative to the chest joint, in the chest frame (height units).
  - `wristOrientation`: hand orientation in the chest frame.
  - `elbowPole`: unit direction the elbow bends, perpendicular to shoulder→wrist, in the chest frame.
    Degenerate (straight or folded) arms fall back to `(0, -1, 0)` with `confidence = 0.5`.
  - `reach`: shoulder→wrist distance / `kReferenceArmReach` (0.32). At most 1.
  - `wristVelocity`: chest-relative, per beat. Filled only by `extractClipTaskSpaceIntent`.
  - `source`: `Extracted`, or `Explicit` when a future primitive states the target.
- **Explicit override:** `resolveArmIntent(explicit, extracted)` returns the explicit
  intent when it is valid and `Explicit`, otherwise the extracted one.

## Frame and reference rig

- The chest joint is the origin. The axes follow the chest's own orientation, so a
  gesture held in front of the sternum stays there when the spine bends.
- Root translation and yaw (`rootX`, `rootZ`, `rootYaw`) are ignored. Hip translation
  (`ClipPose::hips`) is included in the kinematics but cancels out in chest-relative
  values.
- The rig is a fixed canonical skeleton in height units, with every bind rotation
  identity. It is NOT read from a VRM. Offsets: hips 0.53, spine 0.05, chest 0.08,
  upper chest 0.08, shoulder 0.04 out, upper arm 0.06 out, upper arm bone 0.17,
  forearm bone 0.15. The right arm mirrors X.

## Pipeline

```
 authored clip / primitive
          |  sampleClip(clip, beat)            (existing, unchanged)
          v
      ClipPose  ----------------------------------------------+
          |                                                  |
          |  extractTaskSpaceIntent(pose)  (FK, canonical rig)
          v                                                  |
   TaskSpaceIntent  <- resolveArmIntent(explicit?)           |
          |                                                  |
          v                                                  |
 future planner (#5/#6)                                      |
                                                             |
 HumanoidRetargeter::applyClip(pose)  <----------------------+
          (unchanged: the same pose is retargeted, IK and collision run afterwards)
```

The intent never reads the output of the retargeter, so corrected IK poses are not
fed back into intent generation.

## Measured cost

Release build, `g++ -O2`, `groove.json`, 200 000 calls per row, on the build container:

| Operation | Cost per call |
|---|---|
| `sampleClip` alone (existing) | ~0.32 µs |
| `sampleClip` + `extractTaskSpaceIntent` | ~1.07 µs |
| `extractClipTaskSpaceIntent` (3 samples + 3 FK, with velocity) | ~2.33 µs |

These are one-off numbers from a single run. They are not a regression gate.

## Verification

`tests/test_intent.cpp` (8 cases, all passing; the full suite passes at 192/192):

- Reference bind pose: exact wrist position, full reach, fallback pole.
- An arm raise moves the wrist straight up; the right arm is untouched.
- An elbow bend gives a unit pole perpendicular to the shoulder→wrist axis.
- Spine and chest rotation and stage root/hip translation leave the chest-relative intent unchanged.
- Clip extraction is a pure function of the beat: call order does not change the result.
- Every authored clip in `assets/dances` is extractable at 0.25-beat steps, with finite values and `reach <= 1`.
- Explicit intents override the extracted fallback, and extracted intents never count as explicit.
- Direction agreement with `HumanoidRetargeter::applyClip` on `minimal.vrm`. The two
  skeletons differ in proportions, so the test checks cosine > 0.95 between the
  chest-to-wrist directions, not exact positions.

## Not done in this change

- No runtime wiring into `DancePlayer` / `DanceDirector`, so there is no before/after
  diff of motion output yet. The "planner disabled is identical" check is trivially
  met today because nothing consumes the bridge.
- No 30/60/120 FPS, seek, crossfade or phase-warp tests of the intent itself. The
  extraction is a pure function of the beat, so these hold by construction; they
  still need explicit tests once a consumer exists.
- No per-model calibration. The canonical rig is deliberately model-independent. The
  mapping onto a model's bones stays in `HumanoidRetargeter`.
- No explicit-intent primitives yet. Only the `resolveArmIntent` contract exists.
