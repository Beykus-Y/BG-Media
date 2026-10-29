#include "bgmedia/animation/NodeConstraints.hpp"

#include <cstddef>
#include <vector>

namespace bgmedia::animation {
namespace {

using math::Mat4;
using math::Quat;
using math::Vec3;

bool isAncestorOrSelf(const vrm::VrmModel& model, int ancestor, int node) {
    for (int current = node, guard = 0;
         current >= 0 && guard <= static_cast<int>(model.nodes.size());
         current = model.nodes[static_cast<std::size_t>(current)].parent, ++guard) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

Mat4 parentGlobal(const vrm::VrmModel& model, const std::vector<Mat4>& globals, int node) {
    const int parent = model.nodes[static_cast<std::size_t>(node)].parent;
    return parent >= 0 ? globals[static_cast<std::size_t>(parent)] : Mat4::identity();
}

Quat localRotation(const vrm::VrmModel& model, const std::vector<Mat4>& globals, int node) {
    return (parentGlobal(model, globals, node).inverseAffine() *
            globals[static_cast<std::size_t>(node)])
        .rotationQuat();
}

bool isSceneRoot(const vrm::VrmModel& model, int node) {
    for (int root : model.sceneRoots) {
        if (root == node) {
            return true;
        }
    }
    return false;
}

// Rewrites `node`'s local rotation and carries its whole subtree along.
void setLocalRotation(const vrm::VrmModel& model, std::vector<Mat4>& globals, int node,
                      Quat rotation) {
    const Mat4 parent = parentGlobal(model, globals, node);
    const Mat4 oldGlobal = globals[static_cast<std::size_t>(node)];
    const Mat4 local = parent.inverseAffine() * oldGlobal;
    const Vec3 scale{local.axisX().length(), local.axisY().length(), local.axisZ().length()};
    const Mat4 newGlobal =
        parent * Mat4::fromTRS(local.translationPart(), rotation.normalized(), scale);
    const Mat4 delta = newGlobal * oldGlobal.inverseAffine();
    globals[static_cast<std::size_t>(node)] = newGlobal;
    std::vector<int> stack(model.nodes[static_cast<std::size_t>(node)].children);
    while (!stack.empty()) {
        const int child = stack.back();
        stack.pop_back();
        if (child < 0 || static_cast<std::size_t>(child) >= globals.size()) {
            continue;
        }
        globals[static_cast<std::size_t>(child)] = delta * globals[static_cast<std::size_t>(child)];
        const auto& grandChildren = model.nodes[static_cast<std::size_t>(child)].children;
        stack.insert(stack.end(), grandChildren.begin(), grandChildren.end());
    }
}

} // namespace

NodeConstraintSolver::NodeConstraintSolver(const vrm::VrmModel& model) : model_(&model) {
    const auto& constraints = model.nodeConstraints;
    const std::size_t count = constraints.size();
    // B depends on A when A moves anything B reads: B's source (its local
    // rotation, or for an aim its world position) or, for an aim, B's own
    // world frame.
    std::vector<std::vector<int>> dependents(count);
    std::vector<int> pending(count, 0);
    for (std::size_t a = 0; a < count; ++a) {
        for (std::size_t b = 0; b < count; ++b) {
            if (a == b) {
                continue;
            }
            const vrm::NodeConstraint& first = constraints[a];
            const vrm::NodeConstraint& second = constraints[b];
            const bool aim = second.type == vrm::NodeConstraintType::Aim;
            const bool moves =
                first.destination == second.source ||
                (aim && (isAncestorOrSelf(model, first.destination, second.source) ||
                         isAncestorOrSelf(model, first.destination, second.destination)));
            if (moves) {
                dependents[a].push_back(static_cast<int>(b));
                ++pending[b];
            }
        }
    }
    std::vector<int> ready;
    for (std::size_t i = 0; i < count; ++i) {
        if (pending[i] == 0) {
            ready.push_back(static_cast<int>(i));
        }
    }
    // Kahn's algorithm in file order, so independent constraints keep the
    // order they were authored in.
    while (!ready.empty()) {
        std::size_t pick = 0;
        for (std::size_t i = 1; i < ready.size(); ++i) {
            if (ready[i] < ready[pick]) {
                pick = i;
            }
        }
        const int current = ready[pick];
        ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick));
        order_.push_back(current);
        for (int next : dependents[static_cast<std::size_t>(current)]) {
            if (--pending[static_cast<std::size_t>(next)] == 0) {
                ready.push_back(next);
            }
        }
    }
    skipped_ = static_cast<int>(count - order_.size());
}

int NodeConstraintSolver::apply(std::vector<Mat4>& globals) const {
    if (!model_ || order_.empty() || globals.size() != model_->nodes.size()) {
        return 0;
    }
    const vrm::VrmModel& model = *model_;
    int applied = 0;
    for (int index : order_) {
        const vrm::NodeConstraint& c = model.nodeConstraints[static_cast<std::size_t>(index)];
        if (c.weight <= 0.0f) {
            continue;
        }
        // Constraints are authored against the file's local frames. The loader
        // turned the scene roots by sourceToModel, so take that turn back off
        // a root's local rotation here and put it back on when writing.
        const Quat srcTurn = isSceneRoot(model, c.source) ? model.sourceToModel : Quat::identity();
        const Quat dstTurn =
            isSceneRoot(model, c.destination) ? model.sourceToModel : Quat::identity();
        const auto authoredLocal = [&](int node, Quat turn) {
            return turn.conjugate() * localRotation(model, globals, node);
        };
        const Quat dstRest =
            dstTurn.conjugate() * model.nodes[static_cast<std::size_t>(c.destination)].rotation;
        const Quat srcRest =
            srcTurn.conjugate() * model.nodes[static_cast<std::size_t>(c.source)].rotation;
        Quat target = dstRest;
        switch (c.type) {
        case vrm::NodeConstraintType::Rotation: {
            // The source's rotation away from its rest, applied on top of the
            // destination's rest.
            const Quat delta = srcRest.conjugate() * authoredLocal(c.source, srcTurn);
            target = dstRest * delta;
            break;
        }
        case vrm::NodeConstraintType::Roll: {
            // Only the twist of the source's delta about the roll axis, after
            // moving the delta into the destination's rest frame.
            const Quat src = authoredLocal(c.source, srcTurn);
            const Quat deltaInParent = src * srcRest.conjugate();
            const Quat deltaInDst = dstRest.conjugate() * deltaInParent * dstRest;
            const Vec3 to = math::rotate(deltaInDst, c.axis);
            const Quat swing = math::fromTo(c.axis, to);
            target = dstRest * swing.conjugate() * deltaInDst;
            break;
        }
        case vrm::NodeConstraintType::Aim: {
            // Turn the destination's aim axis toward the source.
            const Quat parentRot =
                parentGlobal(model, globals, c.destination).rotationQuat() * dstTurn;
            const Vec3 from = math::rotate(parentRot * dstRest, c.axis);
            const Vec3 to = globals[static_cast<std::size_t>(c.source)].translationPart() -
                            globals[static_cast<std::size_t>(c.destination)].translationPart();
            if (to.lengthSquared() < 1.0e-12f) {
                continue;
            }
            const Quat turn = math::fromTo(from, to);
            target = parentRot.conjugate() * turn * parentRot * dstRest;
            break;
        }
        }
        setLocalRotation(model, globals, c.destination,
                         dstTurn * math::slerp(dstRest, target, c.weight));
        ++applied;
    }
    return applied;
}

} // namespace bgmedia::animation
