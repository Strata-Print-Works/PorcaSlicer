#include <initializer_list>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/WorkspaceStyle.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Thin display sheets preserve physical XY and rest on the workspace", "[WorkspaceStyle]")
{
    const BoundingBoxf3 bounds(Vec3d(-140., -146.5, -12.95), Vec3d(140., 146.5, 1.352));
    const Vec3d offset(1570., -925., -0.45);
    const Transform3d display = WorkspaceStyle::sheet_transform(offset, bounds);
    const Vec3d low = display * bounds.min;
    const Vec3d high = display * bounds.max;
    CHECK_THAT(low.x(), WithinAbs(offset.x() + bounds.min.x(), 1e-8));
    CHECK_THAT(low.y(), WithinAbs(offset.y() + bounds.min.y(), 1e-8));
    CHECK_THAT(high.x() - low.x(), WithinAbs(bounds.size().x(), 1e-8));
    CHECK_THAT(high.y() - low.y(), WithinAbs(bounds.size().y(), 1e-8));
    CHECK_THAT(low.z(), WithinAbs(WorkspaceStyle::floor_z, 1e-6));
    CHECK_THAT(high.z(), WithinAbs(WorkspaceStyle::sheet_top_z, 1e-8));
    CHECK_THAT(high.z() - low.z(), WithinAbs(1.0, 1e-8));
    CHECK(WorkspaceStyle::chrome_z > WorkspaceStyle::floor_z);
    CHECK(WorkspaceStyle::chrome_z < WorkspaceStyle::sheet_top_z);
    CHECK_THAT(bounds.min.z(), WithinAbs(-12.95, 1e-8));
}

TEST_CASE("Inactive sheets are darker in both application themes", "[WorkspaceStyle]")
{
    for (bool dark : {false, true}) {
        const auto active = WorkspaceStyle::bed_color(dark, true);
        const auto inactive = WorkspaceStyle::bed_color(dark, false);
        CHECK(inactive.r() < active.r());
        CHECK(inactive.g() < active.g());
        CHECK(inactive.b() < active.b());
        CHECK_THAT(inactive.a(), WithinAbs(1.0, 1e-6));
        CHECK_THAT(active.a(), WithinAbs(1.0, 1e-6));
    }
}
