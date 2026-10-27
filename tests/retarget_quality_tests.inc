// BG Motion 3.2 quality pass: spatial correctness (C01, C06, C07, C11).
// Included at the end of test_retarget.cpp (shares its helpers and fixtures).

namespace {

using bgmedia::math::Mat4;
using bgmedia::math::Quat;
using bgmedia::math::Vec3;

// Re-orients one node's local axes by `r` while keeping every joint position
// and the orientation of all children unchanged. The skeleton keeps its shape
// but the bone axes (and therefore bind global rotations) differ, which is
// exactly what differs between exporters.
void reorientNode(VrmModel& model, int nodeIndex, const Quat& r) {
    auto& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
    node.rotation = (node.rotation * r).normalized();
    node.local = Mat4::fromTRS(node.translation, node.rotation, node.scale);
    const Quat inv = r.conjugate();
    for (int child : node.children) {
        auto& c = model.nodes[static_cast<std::size_t>(child)];
        c.translation = bgmedia::math::rotate(inv, c.translation);
        c.rotation = (inv * c.rotation).normalized();
        c.local = Mat4::fromTRS(c.translation, c.rotation, c.scale);
    }
}

void rebuildBind(VrmModel& model) {
    std::vector<int> stack(model.sceneRoots.begin(), model.sceneRoots.end());
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        const auto& node = model.nodes[static_cast<std::size_t>(index)];
        const auto i = static_cast<std::size_t>(index);
        if (node.parent >= 0) {
            const auto p = static_cast<std::size_t>(node.parent);
            model.bindGlobals[i] = model.bindGlobals[p] * node.local;
            model.bindGlobalRotations[i] =
                (model.bindGlobalRotations[p] * node.rotation).normalized();
        } else {
            model.bindGlobals[i] = node.local;
            model.bindGlobalRotations[i] = node.rotation.normalized();
        }
        for (int child : node.children) {
            stack.push_back(child);
        }
    }
}

std::unique_ptr<VrmModel> loadMinimal(bool skewedBind) {
    auto result = VrmLoader::load(fixturePath());
    if (!result.ok()) {
        return nullptr;
    }
    auto model = std::move(result.model);
    if (skewedBind) {
        using bgmedia::math::fromAxisAngle;
        using bgmedia::math::normalize;
        const HumanoidBone legs[] = {HumanoidBone::LeftUpperLeg, HumanoidBone::LeftLowerLeg,
                                     HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg,
                                     HumanoidBone::LeftFoot, HumanoidBone::RightFoot};
        float angle = 0.45f;
        for (HumanoidBone bone : legs) {
            reorientNode(*model, model->nodeFor(bone),
                         fromAxisAngle(normalize(Vec3{1.0f, 0.6f, -0.4f}), angle));
            angle += 0.21f;
        }
        rebuildBind(*model);
    }
    return model;
}

Vec3 clipToModel(const VrmModel& model, Vec3 v) {
    return (-model.right) * v.x + model.up * v.y + model.forward * v.z;
}

bgmedia::motion::ClipPose legReachPose() {
    auto pose = bgmedia::motion::neutralClipPose();
    pose.foot[0].position = {0.05f, 0.09f, 0.06f};
    pose.foot[0].contact = 0.0f;
    pose.foot[1].position = {-0.03f, 0.0f, 0.02f};
    pose.foot[1].contact = 1.0f;
    pose.hips = {0.0f, -0.04f, 0.0f};
    return pose;
}

} // namespace

TEST_CASE(retarget.foot_error_is_measured_on_the_evaluated_skeleton) {
    for (const bool skewed : {false, true}) {
        auto model = loadMinimal(skewed);
        CHECK(model != nullptr);
        HumanoidRetargeter rig(*model);
        CHECK(rig.valid());
        for (const float yaw : {0.0f, 0.25f, -0.6f}) {
            auto clip = legReachPose();
            clip.rootYaw = yaw;
            const auto out = rig.applyClip(clip, 0.0);
            CHECK(out.stats.valid);
            const HumanoidBone feet[2] = {HumanoidBone::LeftFoot, HumanoidBone::RightFoot};
            float worst = 0.0f;
            for (int f = 0; f < 2; ++f) {
                const int node = model->nodeFor(feet[f]);
                const Vec3 expected = model->boneBindPosition(feet[f]) +
                                      clipToModel(*model, clip.foot[f].position) *
                                          out.stats.height;
                const Vec3 actual = HumanoidRetargeter::stagePosition(out, node);
                worst = std::max(worst, (actual - expected).length());
            }
            // The reported number is the real, independently measured error.
            CHECK(worst < 0.003f);
            CHECK_NEAR(out.stats.maxFootError, worst, 0.0015);
            CHECK(out.stats.footSolverError < 0.003f);
            CHECK(out.stats.footReachDeficit < 0.003f);
        }
    }
}

TEST_CASE(retarget.unreachable_foot_target_splits_deficit_from_solver_error) {
    auto model = loadMinimal(true);
    CHECK(model != nullptr);
    HumanoidRetargeter rig(*model);
    auto clip = legReachPose();
    clip.foot[0].position = {0.0f, 1.2f, 0.0f};  // far beyond leg length
    const auto out = rig.applyClip(clip, 0.0);
    CHECK(out.stats.valid);
    CHECK(out.stats.footReachDeficit > 0.2f);
    CHECK(out.stats.maxFootError > 0.2f);
    // The leg still lands on the closest reachable point.
    CHECK(out.stats.footSolverError < 0.003f);
}

TEST_CASE(retarget.leg_pose_is_independent_of_bone_axis_orientation) {
    auto plain = loadMinimal(false);
    auto skew = loadMinimal(true);
    CHECK(plain != nullptr && skew != nullptr);
    HumanoidRetargeter a(*plain);
    HumanoidRetargeter b(*skew);
    const auto clip = legReachPose();
    const auto pa = a.applyClip(clip, 0.0);
    const auto pb = b.applyClip(clip, 0.0);
    for (HumanoidBone bone : {HumanoidBone::LeftLowerLeg, HumanoidBone::RightLowerLeg,
                              HumanoidBone::LeftFoot, HumanoidBone::RightFoot}) {
        const Vec3 x = pa.bonePosition(bone, *plain);
        const Vec3 y = pb.bonePosition(bone, *skew);
        // Knee position proves the calf direction is right, not just the ankle.
        CHECK((x - y).length() < 0.003f);
    }
}

TEST_CASE(retarget.apply_clip_honours_foot_ik_option) {
    auto model = loadMinimal(false);
    CHECK(model != nullptr);
    HumanoidRetargeter rig(*model);
    const auto clip = legReachPose();
    RetargetOptions on;
    RetargetOptions off;
    off.footIK = false;
    const auto withIk = rig.applyClip(clip, 0.0, on);
    const auto noIk = rig.applyClip(clip, 0.0, off);
    CHECK(withIk.stats.footIK);
    CHECK(!noIk.stats.footIK);
    CHECK(noIk.stats.valid);
    for (const auto& m : noIk.nodeGlobals) {
        CHECK(m.finite());
    }
    // Without IK the lifted foot is carried by the hips; it must not reach the
    // stage target that IK reaches.
    const int node = model->nodeFor(HumanoidBone::LeftFoot);
    const Vec3 a = HumanoidRetargeter::stagePosition(withIk, node);
    const Vec3 b = HumanoidRetargeter::stagePosition(noIk, node);
    CHECK((a - b).length() > 0.02f);
    CHECK(noIk.stats.maxFootError > withIk.stats.maxFootError);
}

TEST_CASE(retarget.wrist_axes_are_converted_exactly_once) {
    // The model's own axes differ from the clip axes (character faces +X).
    // Wrist roll/pitch authored in the clip frame must rotate the hand about the
    // model's forward / right axes: the axis is converted exactly once.
    auto model = loadMinimal(false);
    CHECK(model != nullptr);
    model->forward = {1.0f, 0.0f, 0.0f};
    model->right = {0.0f, 0.0f, -1.0f};  // up x forward
    HumanoidRetargeter rig(*model);
    auto base = bgmedia::motion::neutralClipPose();
    base.detailedHands = true;
    const auto a = rig.applyClip(base, 0.0);
    const int hand = model->nodeFor(HumanoidBone::RightHand);
    const auto ra = a.nodeGlobals[static_cast<std::size_t>(hand)].rotationQuat();
    const auto axisOf = [&](int channel) {
        auto rolled = base;
        rolled.handPose[1].wrist[static_cast<std::size_t>(channel)] = 1.0f;
        const auto b = rig.applyClip(rolled, 0.0);
        const auto rb = b.nodeGlobals[static_cast<std::size_t>(hand)].rotationQuat();
        const Quat delta = (rb * ra.conjugate()).normalized();
        return bgmedia::math::normalize(Vec3{delta.x, delta.y, delta.z});
    };
    CHECK(std::abs(bgmedia::math::dot(axisOf(0), model->right)) > 0.99f);
    CHECK(std::abs(bgmedia::math::dot(axisOf(1), model->up)) > 0.99f);
    CHECK(std::abs(bgmedia::math::dot(axisOf(2), model->forward)) > 0.99f);
}
