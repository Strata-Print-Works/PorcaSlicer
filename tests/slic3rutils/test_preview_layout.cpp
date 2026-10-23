#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PreviewLayout.hpp"

using Slic3r::GUI::preview_layout;
using Catch::Matchers::WithinAbs;

TEST_CASE("Preview panels reserve one nonoverlapping model area at different display scales", "[PreviewLayout][PreviewAllocation]")
{
    const float scale = GENERATE(1.f, 1.5f, 2.f);
    const float width = GENERATE(900.f, 1400.f, 1800.f) * scale;
    const float height = 800.f * scale;
    const bool navigation = GENERATE(false, true);
    const float drawer = GENERATE(0.f, 36.f, 260.f) * scale;
    const auto layout = preview_layout(width, height, scale, navigation, drawer);
    CHECK(layout.navigation_width >= 0.f);
    CHECK(layout.center_left >= layout.navigation_width);
    CHECK(layout.center_width() >= 300.f * scale);
    CHECK(layout.center_right <= width - layout.inspector_width);
    CHECK(layout.scene_bottom > 0.f);
    CHECK_THAT(layout.scene_bottom + layout.drawer_height, WithinAbs(height, .001));
    CHECK_THAT(layout.center_left + layout.center_width() + layout.inspector_width, WithinAbs(width, .001));
    // Expanding source consumes vertical space only; the model's side panels stay put.
    const auto collapsed = preview_layout(width, height, scale, navigation, 36.f * scale);
    CHECK_THAT(layout.center_left, WithinAbs(collapsed.center_left, .001));
    CHECK_THAT(layout.center_right, WithinAbs(collapsed.center_right, .001));
}

TEST_CASE("Preview allocations remain bounded while a window is being resized", "[PreviewLayout][PreviewAllocation]")
{
    const float width = GENERATE(0.f, 100.f, 600.f);
    const float height = GENERATE(0.f, 100.f, 400.f);
    const auto layout = preview_layout(width, height, 2.f, true, 520.f);
    CHECK(layout.center_left >= 0.f);
    CHECK(layout.center_right >= layout.center_left);
    CHECK(layout.center_right <= width);
    CHECK(layout.scene_bottom >= 0.f);
    CHECK(layout.drawer_height <= height);
}
