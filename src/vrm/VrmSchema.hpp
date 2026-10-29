#pragma once

// Internal to src/vrm: readers that adapt the VRM 0.x and VRM 1.0 extension
// schemas into the shared VrmModel. The loader owns the glTF core (nodes,
// meshes, skins, textures) and calls these once the parts they reference are
// in place. Every reader range-checks indices and records a warning instead
// of failing the import.

#include "VrmJson.hpp"

#include "bgmedia/vrm/VrmModel.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bgmedia::vrm::schema {

// VRM 1.0 humanoid bone name -> the VRM 0.x name of the same bone. Only the
// thumb chain was renamed; every other name is returned unchanged.
[[nodiscard]] std::string vrm0BoneName(std::string_view vrm1Name);

// VRMC_vrm.meta. Fills the 1.0 fields and maps the 0.x fields from them.
void readMetaV1(const json::JsonValue* meta, VrmModel& model);

// extensions.VRMC_springBone: colliders (spheres; capsules become a row of
// spheres), collider groups and springs, expanded straight into chains.
void readSpringBoneV1(const json::JsonValue* springBone, VrmModel& model);

// nodes[].extensions.VRMC_node_constraint.
void readNodeConstraintsV1(const json::JsonValue* root, VrmModel& model);

// materials[].extensions.VRMC_materials_mtoon (the glTF material itself has
// already been read by the loader).
void readMToonV1(const json::JsonValue* root, VrmModel& model);

// Expressions. `morphTargetCounts[mesh]` is the number of morph targets the
// mesh's primitives share (the largest primitive count).
void readExpressionsV0(const json::JsonValue* vrm,
                       const std::vector<int>& morphTargetCounts, VrmModel& model);
void readExpressionsV1(const json::JsonValue* vrmc,
                       const std::vector<int>& morphTargetCounts, VrmModel& model);

// Look-at parameters and first-person mesh annotations.
void readFirstPersonV0(const json::JsonValue* vrm, VrmModel& model);
void readFirstPersonV1(const json::JsonValue* vrmc, VrmModel& model);

// Spring counters in model.diagnostics, shared by both generations.
void finishSpringDiagnostics(VrmModel& model);

} // namespace bgmedia::vrm::schema
