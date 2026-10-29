#pragma once

// VRM 1.0 node constraints (VRMC_node_constraint): roll, aim and rotation.
//
// Constraints are authored on helper nodes (twist bones, eye aim targets,
// accessories) and are evaluated after retargeting and before secondary
// dynamics, the order the VRM 1.0 reference runtimes use. A model without
// constraints - every VRM 0.x file - is left untouched.

#include "bgmedia/math/Math.hpp"
#include "bgmedia/vrm/VrmModel.hpp"

#include <vector>

namespace bgmedia::animation {

class NodeConstraintSolver {
public:
    NodeConstraintSolver() = default;
    explicit NodeConstraintSolver(const vrm::VrmModel& model);

    [[nodiscard]] bool empty() const { return order_.empty(); }
    // Constraints dropped because they depend on each other in a cycle.
    [[nodiscard]] int skippedCycles() const { return skipped_; }

    // `nodeGlobals` are character-space globals for every node of the model.
    // Each destination and its subtree are rewritten in dependency order.
    // Returns the number of constraints applied.
    int apply(std::vector<math::Mat4>& nodeGlobals) const;

private:
    const vrm::VrmModel* model_ = nullptr;
    std::vector<int> order_;  // indices into model_->nodeConstraints
    int skipped_ = 0;
};

} // namespace bgmedia::animation
