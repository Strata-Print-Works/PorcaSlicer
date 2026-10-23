#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "libslic3r/SelectionAlignment.hpp"
#include <limits>
using namespace Slic3r;
using Catch::Approx;
TEST_CASE("Alignment uses selected extents and explicit anchors", "[SelectionAlignment]") {
    const std::vector<AlignmentInterval> boxes{{10,20}, {35,55}, {80,90}};
    REQUIRE(alignment_offsets(boxes, AlignmentAction::Minimum, {10,90}) == std::vector<double>{0,-25,-70});
    REQUIRE(alignment_offsets(boxes, AlignmentAction::Maximum, {10,90}) == std::vector<double>{70,35,0});
    REQUIRE(alignment_offsets(boxes, AlignmentAction::Center, boxes[1]) == std::vector<double>{30,0,-40});
    REQUIRE(alignment_offsets(boxes, AlignmentAction::Center, {-100,100}) == std::vector<double>{-15,-45,-85});
}
TEST_CASE("Distribution preserves endpoints and accounts for unequal widths", "[SelectionAlignment]") {
    const std::vector<AlignmentInterval> boxes{{80,90},{10,20},{35,55}};
    REQUIRE(alignment_offsets(boxes, AlignmentAction::DistributeCenters, {10,90}) == std::vector<double>{0,0,5});
    REQUIRE(alignment_offsets(boxes, AlignmentAction::DistributeGaps, {10,90}) == std::vector<double>{0,0,5});
    REQUIRE(alignment_offsets(boxes, AlignmentAction::SetGap, {10,90}, 1.) == std::vector<double>{-38,0,-14});
}
TEST_CASE("Alignment handles small and invalid selections without motion", "[SelectionAlignment]") {
    REQUIRE(alignment_offsets({}, AlignmentAction::Minimum, {0,1}).empty());
    REQUIRE(alignment_offsets({{0,1},{5,6}}, AlignmentAction::DistributeGaps, {0,6}) == std::vector<double>{0,0});
    REQUIRE(alignment_offsets({{0,1}}, AlignmentAction::SetGap, {0,1}, -1.) == std::vector<double>{0});
    REQUIRE(alignment_offsets({{0,std::numeric_limits<double>::infinity()}}, AlignmentAction::Minimum, {0,1}) == std::vector<double>{0});
}
TEST_CASE("Alignment to an invalid anchor leaves the selection in place", "[SelectionAlignment]") {
    const std::vector<AlignmentInterval> boxes{{10,20}, {35,55}};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (auto action : {AlignmentAction::Minimum, AlignmentAction::Center, AlignmentAction::Maximum})
        for (const AlignmentInterval reference : {AlignmentInterval{nan, 1}, AlignmentInterval{0, inf}, AlignmentInterval{-inf, inf}, AlignmentInterval{5, 1}})
            REQUIRE(alignment_offsets(boxes, action, reference) == std::vector<double>{0, 0});
    // Distribution never reads the anchor.
    REQUIRE(alignment_offsets(boxes, AlignmentAction::SetGap, {nan, nan}, 1.) == std::vector<double>{0, -14});
}
