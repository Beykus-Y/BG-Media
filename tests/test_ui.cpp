#include "framework.hpp"

#include "UiMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

namespace {

using bgmedia::desktop::computeMixColumns;
using bgmedia::desktop::effectiveUiScale;
using bgmedia::desktop::fontFaceCacheKey;
using bgmedia::desktop::fontInverseScale;
using bgmedia::desktop::kMinCanvasHeight;
using bgmedia::desktop::kMinCanvasWidth;
using bgmedia::desktop::kUiScaleMin;

struct WindowSize {
    float width;
    float height;
};

constexpr WindowSize kWindows[] = {
    {1280.0f, 860.0f},  {1366.0f, 768.0f},  {1600.0f, 900.0f},
    {1920.0f, 1080.0f}, {2560.0f, 1440.0f}, {3840.0f, 2160.0f},
};

constexpr float kRequests[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f};

} // namespace

TEST_CASE("ui.effective_scale_keeps_canvas_usable") {
    // Regression for the review finding that a large UI scale left too few
    // logical pixels and pushed the transport off-screen.
    for (const WindowSize& window : kWindows) {
        for (float request : kRequests) {
            const float scale =
                effectiveUiScale(window.width, window.height, request);
            CHECK(scale >= kUiScaleMin);
            CHECK(scale <= 2.5f);
            CHECK(scale <= request + 1e-5f);
            const float canvasW = window.width / scale;
            const float canvasH = window.height / scale;
            // Every window in this set is large enough (>= 80% of the minimum
            // canvas) to preserve the full layout, so the bottom card row is
            // never clipped by a large UI scale.
            CHECK(canvasW >= kMinCanvasWidth - 0.5f);
            CHECK(canvasH >= kMinCanvasHeight - 0.5f);
            // A 1080p window must not silently accept 250%: that would leave
            // only 768 logical points and overflow the three-column layout.
            if (window.width <= 1920.0f && request > 2.0f) {
                CHECK(scale < request - 0.05f);
            }
        }
    }

    // A very large (5K) display may use the full 250%.
    const float large = effectiveUiScale(5120.0f, 2880.0f, 2.5f);
    CHECK_NEAR(large, 2.5, 1e-5);

    // 1080p is the common case: with the ~900-point card stack the usable
    // scale tops out at 120%, which is exactly what keeps the bottom card row
    // above the footer instead of clipping it.
    CHECK_NEAR(effectiveUiScale(1920.0f, 1080.0f, 2.0f), 1.2, 1e-5);
    CHECK_NEAR(effectiveUiScale(2560.0f, 1440.0f, 2.0f), 1.6, 1e-5);
}

TEST_CASE("ui.mix_columns_never_overflow") {
    // Regression for computeMixLayout forcing centerW to at least 260 px even
    // when queue + center + right no longer fitted inside the canvas.
    constexpr float margin = 24.0f;
    constexpr float gap = 16.0f;
    for (float canvasW = 320.0f; canvasW <= 4096.0f; canvasW += 37.0f) {
        const auto columns = computeMixColumns(canvasW);
        CHECK(columns.queueW >= 0.0f);
        CHECK(columns.rightW >= 0.0f);
        CHECK(columns.centerW >= 0.0f);
        const float width = std::max(0.0f, canvasW - margin * 2.0f);
        const float used =
            columns.queueW + columns.centerW + columns.rightW + gap * 2.0f;
        CHECK(used <= width + 1e-3f);
        // The right-hand control card ends at canvasW - margin.
        const float rightEdge =
            margin + columns.queueW + gap + columns.centerW + gap + columns.rightW;
        CHECK(rightEdge <= canvasW - margin + 1e-3f);
    }

    // At every supported canvas width the full layout must still be present.
    for (float canvasW : {1100.0f, 1280.0f, 1440.0f, 1920.0f, 2560.0f}) {
        const auto columns = computeMixColumns(canvasW);
        CHECK(columns.queueW >= 210.0f);
        CHECK(columns.rightW >= 300.0f);
        CHECK(columns.centerW >= 420.0f);
    }
}

TEST_CASE("ui.font_cache_key_is_bounded_across_scales") {
    // Regression for two opposite failures. The original cache collapsed 10 px
    // at 100% and 8 px at 125% into one face with wrong logical metrics; then
    // keying by scale made continuous resizing retain a texture per scale step.
    // Now the atlas is keyed by baked pixels only and metrics use the current
    // inverse scale, so both cases are correct and the cache stays bounded.
    const std::uint64_t tenPx = fontFaceCacheKey(10, false);
    CHECK(tenPx == fontFaceCacheKey(10, false));
    CHECK(fontFaceCacheKey(11, false) != tenPx);
    CHECK(fontFaceCacheKey(10, true) != tenPx);
    CHECK_NEAR(10.0f * fontInverseScale(1.0f), 10.0, 1e-5);
    CHECK_NEAR(10.0f * fontInverseScale(1.25f), 8.0, 1e-5);

    // A resize sweeping 1.00 -> 1.50 in 0.25% steps asks for 201 different
    // scales, but a 10-point face bakes into only six pixel sizes.
    std::set<std::uint64_t> keys;
    for (int i = 0; i <= 200; ++i) {
        const float scale = 1.0f + 0.0025f * static_cast<float>(i);
        const int baked = static_cast<int>(
            std::lround(10.0f * scale));
        keys.insert(fontFaceCacheKey(baked, false));
    }
    CHECK(keys.size() <= 6);
}
