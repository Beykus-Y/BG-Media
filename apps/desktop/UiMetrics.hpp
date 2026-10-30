#pragma once

// Pure UI math shared by the desktop renderer and the unit tests. It has no SDL
// dependency so the scale and layout rules can be regression-tested without a
// window or a GPU.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bgmedia::desktop {

// The widest screens need at least this many logical points: the BG Mix screen
// needs queue + two deck cards + plan/transport side by side, and the BG Beat /
// BG Motion screens need ~900 points of height for their stacked cards. Keeping
// the logical canvas above this minimum is what stops a large UI scale from
// pushing controls off-screen or clipping the bottom row.
constexpr float kMinCanvasWidth = 1100.0f;
constexpr float kMinCanvasHeight = 900.0f;
// Scale bounds. The lower bound exists only so an unusually small window can
// still fit the layout by shrinking instead of clipping; normal displays never
// go below 100%.
constexpr float kUiScaleMin = 0.8f;
constexpr float kUiScaleMax = 2.5f;

// Honours the requested scale unless it would leave less than the minimum
// canvas. A 1920x1080 window at 200%, for example, is limited to 120%, and a
// window shorter than 900 points is scaled slightly below 100% so the bottom
// card row stays visible.
[[nodiscard]] inline float effectiveUiScale(float drawableWidth,
                                            float drawableHeight,
                                            float requested) {
    const float fit = std::min(drawableWidth / kMinCanvasWidth,
                               drawableHeight / kMinCanvasHeight);
    const float maxScale = std::max(kUiScaleMin, std::min(kUiScaleMax, fit));
    return std::clamp(std::min(requested, maxScale), kUiScaleMin, kUiScaleMax);
}

struct MixColumns {
    float queueW = 0.0f;
    float rightW = 0.0f;
    float centerW = 0.0f;
};

// Column widths for the BG Mix screen. The cards plus the two gaps always fit
// inside the canvas; the old code forced centerW to 260 px even when the total
// exceeded the window, which pushed the right-hand control card off-screen.
[[nodiscard]] inline MixColumns computeMixColumns(float canvasWidth) {
    constexpr float margin = 24.0f;
    constexpr float gap = 16.0f;
    const float width = std::max(0.0f, canvasWidth - margin * 2.0f);

    float queueW = std::clamp(canvasWidth * 0.215f, 250.0f, 340.0f);
    float rightW = std::clamp(canvasWidth * 0.255f, 330.0f, 440.0f);
    float centerW = width - queueW - rightW - gap * 2.0f;
    if (centerW < 420.0f) {
        const float deficit = 420.0f - centerW;
        queueW = std::max(210.0f, queueW - deficit * 0.45f);
        rightW = std::max(300.0f, rightW - deficit * 0.55f);
        centerW = width - queueW - rightW - gap * 2.0f;
    }
    if (centerW < 0.0f) {
        // Even the reduced side columns do not fit. Scale them down so every
        // card stays inside the canvas instead of clipping the right edge.
        const float room = std::max(0.0f, width - gap * 2.0f);
        const float side = queueW + rightW;
        if (side > room && side > 0.0f) {
            const float factor = room / side;
            queueW *= factor;
            rightW *= factor;
        }
        centerW = 0.0f;
    }
    return {queueW, rightW, centerW};
}

// Cache key for a baked glyph atlas. The baked pixel size alone is not unique:
// round(10 px * 1.00) and round(8 px * 1.25) both bake a 10 px face but require
// different logical metrics, so the scale must participate in the key.
[[nodiscard]] inline std::uint64_t fontFaceCacheKey(int bakedSize, float scale,
                                                    bool semibold) {
    const auto scaleKey = static_cast<std::uint32_t>(
        std::lround(std::clamp(scale, 0.5f, 4.0f) * 1000.0f));
    return (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(bakedSize))
            << 32) |
           (static_cast<std::uint64_t>(scaleKey) << 1) |
           (semibold ? 1u : 0u);
}

} // namespace bgmedia::desktop
