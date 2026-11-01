#pragma once

// BG Motion 3.3 (issue #11): opt-in thigh/hip spring colliders for VRM 0.x.
//
// Some VRM 0.x models author no collider group on the legs, so a skirt or a
// hanging accessory passes straight through the thighs. This module appends
// sphere colliders along each thigh and assigns them to the lower-body chains
// only. It is OFF by default, changes nothing for models that already have a
// collider on the hips or upper legs, and must run BEFORE a SecondaryDynamics
// is constructed: the simulator reads the collider groups at build time.
//
// The rule, in short (details in docs/BG_MOTION_3_3_THIGH_COLLIDERS.md):
//   * a model is a candidate only if it has humanoid hips, both upper legs and
//     both lower legs, and no non-empty collider group on hips/upper legs;
//   * a spring chain receives the new groups only if its root is NOT under an
//     upper-body bone (spine, chest, upper chest, neck, head, shoulders, arms,
//     eyes) AND every rest joint lies at or below the hips height (plus a 2 %
//     height tolerance). Hair, eyelashes and chest accessories are excluded;
//     skirts, tails and dangling lower-body accessories qualify;
//   * radii are fitted from the model's own skin (see the .cpp), never fixed.
//
// Spring sphere colliders are not cloth simulation. They keep chain points out
// of the thigh spheres; they cannot guarantee that a skirt never penetrates
// the legs.

#include "bgmedia/vrm/VrmModel.hpp"

#include <string>

namespace bgmedia::vrm {

struct ThighColliderOptions {
    // Opt-in. With the default `false` the call returns immediately and the
    // model is not touched.
    bool enabled = false;
    // Spheres per thigh, clamped to 2..3.
    int spheresPerThigh = 3;
};

struct ThighColliderReport {
    bool applied = false;
    // Empty when applied. Otherwise the reason nothing was changed, e.g.
    // "disabled", "already has a collider on hips/upper legs", "missing leg bones".
    std::string skipReason;
    int groupsAdded = 0;       // 0 or 2: one group per thigh, appended at the end
    int spheresAdded = 0;
    int chainsTouched = 0;     // lower-body chains that gained the new references
    int chainsExcluded = 0;    // chains rejected by the placement rule
    float leftRadius = 0.0f;   // metres
    float rightRadius = 0.0f;  // metres
    bool leftRadiusFromMesh = false;   // false: fallback fraction of model height
    bool rightRadiusFromMesh = false;
};

// Appends one collider group per thigh (node = upper leg) and references them
// from the suitable chains. Existing groups and their indices are never
// modified. Calling it again on the result is a no-op, because the generated
// groups now count as an upper-leg collider.
[[nodiscard]] ThighColliderReport synthesizeThighColliders(
    VrmModel& model, const ThighColliderOptions& options = {});

}  // namespace bgmedia::vrm
