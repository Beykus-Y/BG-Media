#include "bgmedia/mix/MixTypes.hpp"

#include <algorithm>

namespace bgmedia::mix {

ImplementationStatus transitionStatus(TransitionType type) noexcept {
    switch (type) {
    case TransitionType::Off:
    case TransitionType::Crossfade:
    case TransitionType::SmartCrossfade:
    case TransitionType::Beatmatched:
    case TransitionType::TempoRamp:
    case TransitionType::PhraseAware:
    case TransitionType::DirectCut:
        return ImplementationStatus::Working;
    case TransitionType::DualTempoMatch:
    case TransitionType::BreakdownBridge:
        return ImplementationStatus::NotImplemented;
    }
    return ImplementationStatus::NotImplemented;
}

const char* transitionTypeName(TransitionType type) noexcept {
    switch (type) {
    case TransitionType::Off: return "Off";
    case TransitionType::Crossfade: return "Crossfade";
    case TransitionType::SmartCrossfade: return "Smart Crossfade";
    case TransitionType::Beatmatched: return "Beatmatched";
    case TransitionType::PhraseAware: return "Phrase-aware";
    case TransitionType::TempoRamp: return "Tempo Ramp";
    case TransitionType::DualTempoMatch: return "Dual Tempo Match";
    case TransitionType::BreakdownBridge: return "Breakdown Bridge";
    case TransitionType::DirectCut: return "Direct Cut";
    }
    return "Unknown";
}

const char* tempoMatchKindName(TempoMatchKind kind) noexcept {
    switch (kind) {
    case TempoMatchKind::None: return "no tempo sync";
    case TempoMatchKind::ExactMetric: return "exact metric match";
    case TempoMatchKind::MetricWithStretch:
        return "metric match + bounded stretch";
    case TempoMatchKind::ExperimentalPolyrhythm:
        return "experimental polyrhythm";
    case TempoMatchKind::FallbackCrossfade: return "fallback to crossfade";
    }
    return "unknown";
}

const char* transitionTypeShortName(TransitionType type) noexcept {
    switch (type) {
    case TransitionType::Off: return "OFF";
    case TransitionType::Crossfade: return "XFADE";
    case TransitionType::SmartCrossfade: return "SMART";
    case TransitionType::Beatmatched: return "BEAT";
    case TransitionType::PhraseAware: return "PHRASE";
    case TransitionType::TempoRamp: return "RAMP";
    case TransitionType::DualTempoMatch: return "DUAL";
    case TransitionType::BreakdownBridge: return "BRIDGE";
    case TransitionType::DirectCut: return "CUT";
    }
    return "?";
}

const char* transitionStatusLabel(TransitionType type) noexcept {
    switch (transitionStatus(type)) {
    case ImplementationStatus::Working: return "ready";
    case ImplementationStatus::Experimental: return "experimental";
    case ImplementationStatus::NotImplemented: return "not implemented";
    }
    return "not implemented";
}

std::vector<TransitionType> selectableTransitionTypes() {
    return {
        TransitionType::Off,
        TransitionType::Crossfade,
        TransitionType::SmartCrossfade,
        TransitionType::Beatmatched,
        TransitionType::TempoRamp,
        TransitionType::PhraseAware,
        TransitionType::DirectCut,
    };
}

PlannerConfig PlannerConfig::forTransition(TransitionType type) {
    PlannerConfig config;
    switch (type) {
    case TransitionType::TempoRamp:
        config.allowTempoRamp = true;
        config.tempoRampMode = TempoRampMode::Manual;
        break;
    case TransitionType::Beatmatched:
        config.allowTempoRamp = true;
        config.tempoRampMode = TempoRampMode::Auto;
        break;
    case TransitionType::PhraseAware:
        config.phraseAlignment = true;
        break;
    case TransitionType::DirectCut:
        config.defaultCrossfadeSeconds = 0.0;
        config.minCrossfadeSeconds = 0.0;
        break;
    default:
        break;
    }
    return config;
}

} // namespace bgmedia::mix
