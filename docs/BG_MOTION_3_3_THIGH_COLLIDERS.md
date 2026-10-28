# BG Motion 3.3 — Opt-in thigh/hip spring colliders (issue #11)

Files: `include/bgmedia/vrm/ThighColliders.hpp`, `src/vrm/ThighColliders.cpp`,
tests in `tests/test_spring.cpp` (`spring.thigh_colliders_*`), CLI flag
`--thigh-colliders` in `tools/motion_diag`.

## What it is not

SpringBone sphere colliders are **not cloth simulation**. They keep individual chain
points outside a few spheres on the thigh. A skirt is a chain of points with a length
constraint, so this cannot guarantee that the skirt never penetrates the legs. It can
push the hanging points out of the thigh spheres, and that is all the feature claims.

## Why it exists

Some VRM 0.x models have no `colliderGroups` entry on the legs. A skirt or hanging
accessory then passes through the thighs. Attaching colliders to every spring chain
is unsafe: hair, eyelashes and chest accessories would bounce off the legs for no
reason. So the feature is narrow on purpose.

## How to enable

It is off by default and changes nothing unless asked for:

```cpp
vrm::ThighColliderOptions options;
options.enabled = true;          // default is false
options.spheresPerThigh = 3;     // 2 or 3
const vrm::ThighColliderReport report =
    vrm::synthesizeThighColliders(*model, options);   // BEFORE SecondaryDynamics is constructed
```

The simulator reads the collider groups when it is constructed, so call the function
first. Per-model opt-out is the caller's decision: a model that should not get the
spheres is simply not passed through the function, or is passed with `enabled = false`.

Tool: `bgmedia_motion_diag ... --spring 1 --thigh-colliders 1`. The flag prints one line
with the result (`added` or the skip reason, group and sphere counts, radii).

## The rule

**Model gate.** The model must have humanoid Hips, both UpperLeg and both LowerLeg
bones. It must not already have a non-empty collider group on the Hips or on either
UpperLeg. Any such group means the author handled the legs, and the model is left
unchanged. The model must also have at least one spring chain.

**Chain gate.** A spring chain is lower-body, and only then does it receive the new
groups, when both hold:

1. its root (first node) is not in the subtree of an upper-body bone: Spine, Chest,
   UpperChest, Neck, Head, both Shoulders, UpperArms, LowerArms, Hands or Eyes;
2. every rest joint of the chain is at or below the hips height plus 2 % of model
   height.

So hair, eyelashes and chest or arm accessories are excluded, and so is any chain with a
joint above the hips. Skirts, tails and dangling lower-body accessories are included.
If no chain passes, nothing is added.

**Groups.** Two groups are appended at the end of `springColliderGroups`, one per
thigh, with `node` = UpperLeg. Each has 2 or 3 spheres placed along the thigh axis (hip
joint to knee joint) at fractions 0.25/0.75 (two) or 0.2/0.5/0.8 (three). Sphere offsets
are in the upper-leg node's local frame, so they follow the animated leg.

The new group indices are appended to the `colliderGroups` of each suitable chain.
Existing groups, their indices and all existing references are never changed.
Authored `boneGroups` lists are not touched either.

**Radius.** Fitted from the model's own skin. Vertices whose dominant joint (weight at
least 0.5) is the UpperLeg are placed in bind pose, then measured to the thigh axis. The
radius is the 90th percentile of those distances, clamped to 2 %..9 % of model height.
The same percentile and bounds the retargeter uses for its thigh proxy. With fewer than
50 samples (no skinned mesh) the radius falls back to 4.5 % of model height. The report
says which path was taken for each side.

## Idempotency and safety

- Applying it twice is a no-op on the second call. The first call's groups count as an
  upper-leg collider, so there are no duplicates.
- Missing leg or hips bones, a degenerate thigh, or chains pointing at nodes that do not
  exist are a no-op with a `skipReason`. Nothing is modified on any skip, because the
  groups are fully built before the model is touched.
- The work is bounded: two groups of at most three spheres. The only per-vertex work is
  one pass over skinned vertices to fit each radius, once per call.

## Parameters

| Name | Default | Meaning |
| --- | --- | --- |
| `enabled` | `false` | Opt-in switch. |
| `spheresPerThigh` | `3` | Clamped to 2..3. |
| Radius bounds | 2 %..9 % of height | Fitted radius is clamped to this. |
| Radius fallback | 4.5 % of height | Used without at least 50 skinned samples. |
| Hips tolerance | 2 % of height | Rest joints may sit this far above the hips. |

## Limitations

- Sphere colliders only. A skirt can still clip through the legs between spheres, at
  the knee and in fast motion.
- Radii are fitted from thigh-dominant vertices. A loose skirt that stands well away from
  the thigh gets no extra clearance from this, and a model with no skinned mesh gets the
  fixed fallback.
- The rule is a heuristic on the rest pose. A non-standard rig (for example a
  hair chain rooted outside the head subtree whose joints all sit below the hips) can be
  misclassified. Check the report, or leave the option off for that model.
- VRM 0.x `secondaryAnimation` only. The VRM 1.0 spring bone path is not covered.
- Not wired into the desktop application. It is available to tools and to future
  integration, and it stays off there until someone turns it on deliberately.

## Verification

`tests/test_spring.cpp` covers: default off; models with leg or hips colliders unchanged;
skirt receives the groups and hair never does; hair output bit-identical with and
without the feature at runtime; idempotency; existing indices and references survive;
broken input is a safe no-op; radius fitted from skin; a thigh-axis skirt is pushed
out to about radius plus hit radius; and 30/60/120 FPS stays finite and stable with
the feature on.
