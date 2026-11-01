#pragma once

// BG Mix Transition Planner: a deterministic, explainable mapping from two
// BG Beat analysis results to one transition plan. The planner never "decides"
// silently: every recommendation carries reasons and every fallback carries a
// fallbackReason.

#include "bgmedia/analysis/AnalysisResult.hpp"
#include "bgmedia/mix/MixTypes.hpp"

#include <string>

namespace bgmedia::mix {

struct PlanRequest {
    TransitionType type = TransitionType::SmartCrossfade;
    // 0 means "choose automatically"; otherwise the manual crossfade length.
    double requestedDuration = 0.0;
    // Current outgoing media position. When negative the planner plans the kind
    // of ending transition an automatic queue advance would use.
    double currentPositionA = -1.0;
    bool autoStart = false;
    std::string trackNameB;
};

class MixPlanner {
public:
    explicit MixPlanner(PlannerConfig config = {});

    [[nodiscard]] const PlannerConfig& config() const noexcept { return config_; }
    void setConfig(PlannerConfig config) { config_ = config; }

    // Plans a transition from A to B. `a` and `b` must outlive the call.
    [[nodiscard]] MixPlan plan(const AnalysisResult& a, const AnalysisResult& b,
                               const PlanRequest& request) const;

    // Chooses the default crossfade length from BG Beat sections/confidence.
    [[nodiscard]] double suggestDuration(const AnalysisResult& a,
                                         const AnalysisResult& b) const;
    // Nearest beat (and downbeat when trusted) in the incoming track at/after
    // `time`. Empty when the analysis has no usable grid.
    [[nodiscard]] static double nearestBeat(const AnalysisResult& analysis,
                                            double time, bool* downbeat,
                                            double* errorMs);

private:
    PlannerConfig config_;
};

} // namespace bgmedia::mix
