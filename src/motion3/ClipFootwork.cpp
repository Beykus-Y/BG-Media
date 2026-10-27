#include "bgmedia/motion3/ClipFootwork.hpp"

#include "FootworkTrack.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace bgmedia::motion {

namespace {

using math::Vec3;
using detail::footSmootherstep;

constexpr float kRefCm = 160.0f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;

// One step of one foot: the swing that carries it, followed by the stance it lands in.
// All key indices are cyclic positions in [0, M).
struct Step {
    int foot = 0;
    std::vector<int> swing;
    std::vector<int> stance;
    // Free parameters (the stance offset is in the character frame at the landing key).
    float dx = 0.0f, dz = 0.0f;  // cm
    float bend = 0.0f;           // sideways bend of the swing path, cm (forward/back)
    float lift = 0.0f;           // extra lift, cm
    float landingYaw = 0.0f;     // turns
};

class Planner {
public:
    Planner(const HumanoidClip& clip, const FootworkPlanOptions& options)
        : clip_(clip), opt_(options) {
        body_.hipHalf = options.legs.hipHalf;
        body_.hipY = options.legs.hipY;
        body_.ankleHalf = options.legs.ankleHalf;
        body_.ankleY = options.legs.ankleY;
        body_.thighR = options.legs.thighR;
        body_.kneeR = options.legs.kneeR;
        body_.ankleR = options.legs.ankleR;
        body_.footLen = options.legs.footLen;
        body_.footR = options.legs.footR;
        body_.legLen = options.legs.legLen;
    }

    HumanoidClip run(FootworkPlanReport* report);

private:
    const HumanoidClip& clip_;
    FootworkPlanOptions opt_;
    detail::LegProxy body_;
    int M_ = 0;
    std::vector<Step> steps_;
    // Stage-space delta (cm) per foot and key; recomputed from the steps.
    std::vector<Vec3> delta_[2];
    std::vector<Vec3> hipAdjust_;  // cm, character frame (x left, z forward)
    std::vector<char> planted_[2];

    float yawAt(int i) const {
        return clip_.root.size() > static_cast<std::size_t>(i) ? clip_.root[static_cast<std::size_t>(i)].z : 0.0f;
    }

    // Character frame -> stage frame and back (rotation about the up axis).
    static void toStage(float yawTurns, float xc, float zc, float& xs, float& zs) {
        const float a = yawTurns * kTwoPi;
        const float c = std::cos(a), s = std::sin(a);
        xs = xc * c + zc * s;
        zs = -xc * s + zc * c;
    }

    float clearanceAt(int i) const {
        const std::size_t k = static_cast<std::size_t>(i);
        float rx = 0.0f, rz = 0.0f, yaw = 0.0f;
        if (clip_.root.size() > k) {
            rx = clip_.root[k].x;
            rz = clip_.root[k].y;
            yaw = clip_.root[k].z;
        }
        Vec3 hips{};
        if (clip_.hips.size() > k) {
            hips = clip_.hips[k];
        }
        const float a = yaw * kTwoPi;
        const float c = std::cos(a), s = std::sin(a);
        detail::LegPose pose[2];
        for (int f = 0; f < 2; ++f) {
            const Vec3 p = clip_.feet[static_cast<std::size_t>(f)].position[k];
            const Vec3 d = delta_[f][k];
            const float x = (p.x - rx) * kRefCm + d.x;
            const float z = (p.z - rz) * kRefCm + d.z;
            pose[f].x = x * c - z * s;
            pose[f].z = x * s + z * c;
            pose[f].lift = std::max(0.0f, p.y * kRefCm + d.y);
        }
        Vec3 hipShift{hips.x * kRefCm, hips.y * kRefCm, hips.z * kRefCm};
        if (hipAdjust_.size() > k) {
            hipShift.x += hipAdjust_[k].x;
            hipShift.z += hipAdjust_[k].z;
        }
        return detail::legClearanceCm(pose[0], pose[1], body_, hipShift);
    }

    float minClearance(const std::vector<int>& keys, float stopBelow) const {
        float best = 1.0e9f;
        for (int i : keys) {
            best = std::min(best, clearanceAt(i));
            if (best < stopBelow) {
                return best;
            }
        }
        return best;
    }

    void buildSteps();
    void apply(const Step& step, const Step* previous);
    void applyAll();
    std::vector<int> affectedKeys(std::size_t index) const;
    int stepAt(int foot, int key, bool* swinging) const;
    bool solve(std::size_t index);
    int countViolations(float* minClear) const;

    std::vector<std::size_t> stepsOfFoot_[2];
};

void Planner::buildSteps() {
    const int K = std::max(1, clip_.keysPerBeat);
    for (int f = 0; f < 2; ++f) {
        std::vector<char>& planted = planted_[f];
        planted.assign(static_cast<std::size_t>(M_), 0);
        for (const ContactSegment& seg : clip_.contacts) {
            if (seg.foot != f) continue;
            const int a = static_cast<int>(std::ceil(seg.start * static_cast<float>(K) - 1.0e-3f));
            const int b = static_cast<int>(std::floor(seg.end * static_cast<float>(K) + 1.0e-3f));
            for (int i = a; i <= b; ++i) {
                planted[static_cast<std::size_t>(((i % M_) + M_) % M_)] = 1;
            }
        }
        int firstSwing = -1;
        for (int i = 0; i < M_; ++i) {
            if (!planted[static_cast<std::size_t>(i)] &&
                planted[static_cast<std::size_t>((i + M_ - 1) % M_)]) {
                firstSwing = i;
                break;
            }
        }
        if (firstSwing < 0) {
            continue;  // always planted or always in the air: nothing to plan
        }
        int i = 0;
        while (i < M_) {
            Step step;
            step.foot = f;
            int pos = (firstSwing + i) % M_;
            while (i < M_ && !planted[static_cast<std::size_t>(pos)]) {
                step.swing.push_back(pos);
                ++i;
                pos = (firstSwing + i) % M_;
            }
            while (i < M_ && planted[static_cast<std::size_t>(pos)]) {
                step.stance.push_back(pos);
                ++i;
                pos = (firstSwing + i) % M_;
            }
            if (!step.stance.empty()) {
                step.landingYaw = yawAt(step.stance.front());
                stepsOfFoot_[f].push_back(steps_.size());
                steps_.push_back(std::move(step));
            }
        }
    }
}

// Writes one step's delta into the per-key arrays. The swing blends from the previous
// stance's offset to this one, so the stance offsets are the only held values.
void Planner::apply(const Step& step, const Step* previous) {
    const std::size_t f = static_cast<std::size_t>(step.foot);
    float sx = 0.0f, sz = 0.0f;
    toStage(step.landingYaw, step.dx, step.dz, sx, sz);
    float px = 0.0f, pz = 0.0f;
    if (previous) {
        toStage(previous->landingYaw, previous->dx, previous->dz, px, pz);
    }
    for (int key : step.stance) {
        delta_[f][static_cast<std::size_t>(key)] = {sx, 0.0f, sz};
    }
    const int n = static_cast<int>(step.swing.size());
    for (int j = 0; j < n; ++j) {
        const float u = static_cast<float>(j + 1) / static_cast<float>(n + 1);
        const float blend = footSmootherstep(u);
        const float bump = std::sin(kPi * u);
        const float bump2 = bump * bump;
        const int key = step.swing[static_cast<std::size_t>(j)];
        float bx = 0.0f, bz = 0.0f;
        toStage(yawAt(key), 0.0f, step.bend * bump2, bx, bz);
        delta_[f][static_cast<std::size_t>(key)] = {px + (sx - px) * blend + bx,
                                                    step.lift * bump2,
                                                    pz + (sz - pz) * blend + bz};
    }
}

void Planner::applyAll() {
    for (std::size_t k = 0; k < steps_.size(); ++k) {
        const auto& list = stepsOfFoot_[steps_[k].foot];
        const auto it = std::find(list.begin(), list.end(), k);
        const std::size_t at = static_cast<std::size_t>(it - list.begin());
        const Step* prev = list.size() > 0 ? &steps_[list[(at + list.size() - 1) % list.size()]] : nullptr;
        apply(steps_[k], prev);
    }
}

std::vector<int> Planner::affectedKeys(std::size_t index) const {
    const Step& step = steps_[index];
    const auto& list = stepsOfFoot_[step.foot];
    const auto it = std::find(list.begin(), list.end(), index);
    const std::size_t at = static_cast<std::size_t>(it - list.begin());
    const Step& next = steps_[list[(at + 1) % list.size()]];
    std::vector<int> keys = step.swing;
    keys.insert(keys.end(), step.stance.begin(), step.stance.end());
    keys.insert(keys.end(), next.swing.begin(), next.swing.end());
    return keys;
}

// The step of `foot` that owns `key`, and whether the foot is in the air there.
int Planner::stepAt(int foot, int key, bool* swinging) const {
    for (std::size_t index : stepsOfFoot_[foot]) {
        const Step& step = steps_[index];
        if (std::find(step.swing.begin(), step.swing.end(), key) != step.swing.end()) {
            if (swinging) *swinging = true;
            return static_cast<int>(index);
        }
        if (std::find(step.stance.begin(), step.stance.end(), key) != step.stance.end()) {
            if (swinging) *swinging = false;
            return static_cast<int>(index);
        }
    }
    return -1;
}

int Planner::countViolations(float* minClear) const {
    int count = 0;
    float best = 1.0e9f;
    for (int i = 0; i < M_; ++i) {
        const float c = clearanceAt(i);
        best = std::min(best, c);
        if (c < opt_.minGapCm) ++count;
    }
    if (minClear) *minClear = best;
    return count;
}

// Re-plans one step from the authored data: smallest landing change, then the smallest
// side bend and lift that make the whole swing and stance clear of the other leg.
bool Planner::solve(std::size_t index) {
    Step& step = steps_[index];
    const auto& list = stepsOfFoot_[step.foot];
    const std::size_t at = static_cast<std::size_t>(std::find(list.begin(), list.end(), index) - list.begin());
    const Step* previous = &steps_[list[(at + list.size() - 1) % list.size()]];
    const std::vector<int> keys = affectedKeys(index);
    const auto setAndApply = [&](float dx, float dz, float bend, float lift) {
        step.dx = dx;
        step.dz = dz;
        step.bend = bend;
        step.lift = lift;
        apply(step, previous);
        // The next swing blends away from this stance: refresh it too.
        const std::size_t nextIndex = list[(at + 1) % list.size()];
        apply(steps_[nextIndex], &step);
    };
    const float gap = opt_.minGapCm;

    setAndApply(0.0f, 0.0f, 0.0f, 0.0f);
    float current = minClearance(keys, -1.0e9f);
    if (current >= gap) {
        return true;
    }
    struct Landing { float dx, dz, cost; };
    std::vector<Landing> landings;
    static constexpr float kDx[] = {0, 2, -2, 4, -4, 6, -6, 8, -8, 10, -10, 13, -13, 16, -16, 19, -19, 22, -22, 26, -26};
    static constexpr float kDz[] = {0, 3, -3, 6, -6, 9, -9, 12, -12, 16, -16, 20, -20, 26, -26};
    const float sideSign = step.foot == 0 ? 1.0f : -1.0f;
    for (float ddx : kDx) {
        for (float ddz : kDz) {
            if (std::max(std::abs(ddx), std::abs(ddz)) > opt_.maxShiftCm) continue;
            // Pulling the foot towards the body's midline is the usual cure for a cross-over;
            // reaching further out is allowed but costs more.
            const float inward = -ddx * sideSign;
            const float cost = 1.2f * std::abs(ddx) + std::abs(ddz) + (inward < 0.0f ? 0.0f : 0.2f) +
                               (ddz < 0.0f ? 0.5f : 0.0f);
            landings.push_back({ddx, ddz, cost});
        }
    }
    std::sort(landings.begin(), landings.end(), [](const Landing& a, const Landing& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.dx != b.dx) return a.dx < b.dx;
        return a.dz < b.dz;
    });
    struct Path { float bend, lift, cost; };
    std::vector<Path> paths;
    static constexpr float kBend[] = {0, 6, -6, 12, -12};
    static constexpr float kLift[] = {0, 3, 6, 9};
    for (float b : kBend) {
        for (float l : kLift) {
            paths.push_back({b, l, 0.6f * std::abs(b) + 0.5f * l + (b < 0.0f ? 0.3f : 0.0f)});
        }
    }
    std::sort(paths.begin(), paths.end(), [](const Path& a, const Path& b) { return a.cost < b.cost; });

    float bestClear = current;
    Landing bestLanding{0, 0, 0};
    Path bestPath{0, 0, 0};
    int tried = 0;
    for (const Landing& landing : landings) {
        // Stance first: a landing that is inside the other leg can never work.
        setAndApply(landing.dx, landing.dz, 0.0f, 0.0f);
        if (minClearance(step.stance, gap) < gap) {
            continue;
        }
        for (const Path& path : paths) {
            setAndApply(landing.dx, landing.dz, path.bend, path.lift);
            const float clear = minClearance(keys, -1.0e9f);
            if (clear >= gap) {
                return true;
            }
            if (clear > bestClear) {
                bestClear = clear;
                bestLanding = landing;
                bestPath = path;
            }
        }
        if (++tried >= 60) break;
    }
    // Nothing clears the gap: keep the least bad option, but only if it is an improvement.
    if (bestClear > current) {
        setAndApply(bestLanding.dx, bestLanding.dz, bestPath.bend, bestPath.lift);
    } else {
        setAndApply(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return false;
}

HumanoidClip Planner::run(FootworkPlanReport* report) {
    FootworkPlanReport local;
    FootworkPlanReport& rep = report ? *report : local;
    rep = FootworkPlanReport{};
    HumanoidClip out = clip_;
    const std::size_t keyCount = static_cast<std::size_t>(clip_.keyCount);
    rep.keys = clip_.keyCount;
    if (!clip_.loop || !clip_.seamless || clip_.keyCount < 8 ||
        clip_.feet[0].position.size() != keyCount || clip_.feet[1].position.size() != keyCount) {
        return out;
    }
    M_ = clip_.keyCount - 1;
    for (int f = 0; f < 2; ++f) {
        delta_[f].assign(keyCount, Vec3{});
    }
    buildSteps();
    rep.stances = static_cast<int>(steps_.size());
    for (const Step& s : steps_) rep.swings += s.swing.empty() ? 0 : 1;

    float before = 0.0f;
    rep.violationKeysBefore = countViolations(&before);
    rep.minClearanceBeforeCm = before;
    rep.violationKeysAfter = rep.violationKeysBefore;
    rep.minClearanceAfterCm = before;
    if (rep.violationKeysBefore == 0) {
        return out;
    }

    // Sweep until clean: each violation is handed to the foot that landed last (it chose its
    // landing with the other leg already in place); if that cannot clear it, the other foot
    // is re-planned instead.
    std::vector<Step> bestSteps = steps_;
    int bestCount = rep.violationKeysBefore;
    for (int sweep = 0; sweep < std::max(1, opt_.maxSweeps); ++sweep) {
        bool progressed = false;
        for (int i = 0; i < M_; ++i) {
            if (clearanceAt(i) >= opt_.minGapCm) continue;
            int owner[2];
            bool air[2];
            for (int f = 0; f < 2; ++f) {
                owner[f] = stepAt(f, i, &air[f]);
            }
            int order[2] = {0, 1};
            const auto age = [&](int f) {
                // Keys since this step's swing began (smaller = landed more recently).
                if (owner[f] < 0) return 1 << 20;
                const Step& s = steps_[static_cast<std::size_t>(owner[f])];
                const auto& all = air[f] ? s.swing : s.stance;
                const int idx = static_cast<int>(std::find(all.begin(), all.end(), i) - all.begin());
                return (air[f] ? 0 : static_cast<int>(s.swing.size())) + idx;
            };
            if (air[0] != air[1]) {
                if (!air[0]) std::swap(order[0], order[1]);  // the swinging foot moves first
            } else if (age(0) > age(1)) {
                std::swap(order[0], order[1]);
            }
            for (int o : order) {
                if (owner[o] < 0) continue;
                solve(static_cast<std::size_t>(owner[o]));
                progressed = true;
                if (clearanceAt(i) >= opt_.minGapCm) break;
            }
        }
        float mc = 0.0f;
        const int count = countViolations(&mc);
        if (count < bestCount) {
            bestCount = count;
            bestSteps = steps_;
        }
        if (count == 0 || !progressed) break;
    }
    steps_ = bestSteps;
    applyAll();

    float after = 0.0f;
    rep.violationKeysAfter = countViolations(&after);
    rep.minClearanceAfterCm = after;
    rep.unresolved = rep.violationKeysAfter;
    for (int i = 0; i < M_; ++i) {
        if (clearanceAt(i) < opt_.minGapCm) rep.unresolvedKeys.push_back(i);
    }

    // Weight transfer: a stance that was moved leaves the pelvis off its supporting foot.
    // The pelvis follows the moved feet in proportion to how much weight each carries (the
    // share is smoothed over half a beat, so the weight is on the standing leg BEFORE the
    // other foot lifts), bounded and smooth. Kept only if it does not hurt the clearance.
    if (rep.violationKeysAfter == 0 && !planted_[0].empty() && !planted_[1].empty() &&
        out.hips.size() == keyCount) {
        const int K = std::max(1, clip_.keysPerBeat);
        const int radius = std::max(1, K / 2);
        std::vector<float> share[2];
        for (int f = 0; f < 2; ++f) {
            share[f].assign(static_cast<std::size_t>(M_), 0.0f);
            for (int i = 0; i < M_; ++i) {
                float sum = 0.0f, wsum = 0.0f;
                for (int d = -radius; d <= radius; ++d) {
                    const float w = 1.0f - static_cast<float>(std::abs(d)) / static_cast<float>(radius + 1);
                    sum += w * static_cast<float>(planted_[f][static_cast<std::size_t>(((i + d) % M_ + M_) % M_)]);
                    wsum += w;
                }
                share[f][static_cast<std::size_t>(i)] = sum / wsum;
            }
        }
        std::vector<Vec3> adjust(static_cast<std::size_t>(clip_.keyCount), Vec3{});
        float biggest = 0.0f;
        for (int i = 0; i < M_; ++i) {
            const float a = yawAt(i) * kTwoPi;
            const float c = std::cos(a), s = std::sin(a);
            const float s0 = share[0][static_cast<std::size_t>(i)], s1 = share[1][static_cast<std::size_t>(i)];
            const float total = std::max(1.0e-3f, s0 + s1);
            float ax = 0.0f, az = 0.0f;
            for (int f = 0; f < 2; ++f) {
                const Vec3 d = delta_[f][static_cast<std::size_t>(i)];
                const float w = share[f][static_cast<std::size_t>(i)] / total;
                // stance offset of the foot, as the pelvis sees it (the swing blend already fades it)
                ax += w * (d.x * c - d.z * s);
                az += w * (d.x * s + d.z * c);
            }
            ax = std::clamp(0.35f * ax, -6.0f, 6.0f);
            az = std::clamp(0.35f * az, -6.0f, 6.0f);
            adjust[static_cast<std::size_t>(i)] = {ax, 0.0f, az};
            biggest = std::max(biggest, std::max(std::abs(ax), std::abs(az)));
        }
        adjust[static_cast<std::size_t>(M_)] = adjust[0];
        if (biggest > 0.05f) {
            hipAdjust_ = adjust;
            float mc = 0.0f;
            const int viol = countViolations(&mc);
            if (viol == 0) {
                rep.minClearanceAfterCm = mc;
                for (int i = 0; i <= M_; ++i) {
                    Vec3& h = out.hips[static_cast<std::size_t>(i)];
                    h.x += adjust[static_cast<std::size_t>(i)].x / kRefCm;
                    h.z += adjust[static_cast<std::size_t>(i)].z / kRefCm;
                }
                rep.maxPelvisShiftCm = biggest;
            } else {
                hipAdjust_.clear();
            }
        }
    }

    // Write the planned tracks. The duplicated loop key follows key 0.
    for (int f = 0; f < 2; ++f) {
        auto& track = out.feet[static_cast<std::size_t>(f)].position;
        for (int i = 0; i < M_; ++i) {
            const Vec3 d = delta_[f][static_cast<std::size_t>(i)];
            Vec3& p = track[static_cast<std::size_t>(i)];
            p.x += d.x / kRefCm;
            p.y = std::max(0.0f, p.y + d.y / kRefCm);
            p.z += d.z / kRefCm;
        }
        track[static_cast<std::size_t>(M_)] = track[0];
    }
    for (const Step& s : steps_) {
        const bool moved = std::abs(s.dx) + std::abs(s.dz) + std::abs(s.bend) + s.lift > 1.0e-4f;
        if (moved) ++rep.stancesMoved;
        rep.maxLandingShiftCm = std::max(rep.maxLandingShiftCm, std::sqrt(s.dx * s.dx + s.dz * s.dz));
        rep.maxSideBendCm = std::max(rep.maxSideBendCm, std::abs(s.bend));
        rep.maxExtraLiftCm = std::max(rep.maxExtraLiftCm, s.lift);
    }
    rep.changed = rep.stancesMoved > 0;
    return out;
}

}  // namespace

HumanoidClip planClipFootwork(const HumanoidClip& clip, const FootworkPlanOptions& options,
                              FootworkPlanReport* report) {
    return Planner(clip, options).run(report);
}

}  // namespace bgmedia::motion
