#include "framework.hpp"

#include "UiMetrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

using bgmedia::desktop::computeMixColumns;
using bgmedia::desktop::effectiveUiScale;
using bgmedia::desktop::fontFaceCacheKey;
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

TEST_CASE("ui.font_cache_key_includes_scale") {
    // Regression for reusing a 10 px face at both 100% and 125%: the atlas is
    // the same but inverseScale, and therefore every logical metric, differs.
    const std::uint64_t at100 = fontFaceCacheKey(10, 1.0f, false);
    const std::uint64_t at125 = fontFaceCacheKey(10, 1.25f, false);
    CHECK(at100 != at125);
    CHECK(fontFaceCacheKey(10, 1.0f, false) == at100);
    CHECK(fontFaceCacheKey(10, 1.0f, true) != at100);
    // 8 px at 125% also bakes 10 px, so the baked size alone cannot be the key.
    CHECK(fontFaceCacheKey(10, 1.25f, false) == at125);
    CHECK(fontFaceCacheKey(8, 1.25f, false) != at125);
    CHECK(fontFaceCacheKey(10, 1.5f, false) != at100);
    CHECK(fontFaceCacheKey(10, 2.0f, false) != at100);
}
