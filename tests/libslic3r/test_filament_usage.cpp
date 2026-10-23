#include "libslic3r/FilamentUsage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {
FilamentUsageMeasurement filament(size_t slot, double volume)
{
    return {slot, {"PLA", "Matte", "#FFFFFF", "Example", "Example PLA Matte", 1.75, true}, volume, 1.24, true};
}
}

TEST_CASE("The same filament combines across different plate slots", "[FilamentUsage]")
{
    const double area = std::acos(-1.0) * 1.75 * 1.75 / 4;
    const double first_volume = area * 1000, second_volume = area * 2000;
    const auto result = aggregate_filament_usage({{1, true, {filament(0, first_volume)}}, {2, true, {filament(4, second_volume)}}});
    REQUIRE(result.complete());
    REQUIRE(result.rows.size() == 1);
    REQUIRE_THAT(result.rows[0].meters, WithinRel(3.0));
    REQUIRE_THAT(result.rows[0].grams, WithinRel((first_volume + second_volume) * 1.24 / 1000));
}

TEST_CASE("An empty scope is complete while a fresh job without measurements is incomplete", "[FilamentUsage]")
{
    const auto empty = aggregate_filament_usage({});
    REQUIRE(empty.complete());
    REQUIRE(empty.rows.empty());
    const auto missing = aggregate_filament_usage({{1, true, {}}});
    REQUIRE_FALSE(missing.complete());
    REQUIRE(missing.rows.empty());
}

TEST_CASE("Unused zero volume slots do not add needed filaments", "[FilamentUsage]")
{
    auto unused = filament(4, 0);
    unused.physical = false;
    unused.density_g_cm3 = 0;
    unused.identity.profile = "Unused virtual filament";
    const auto result = aggregate_filament_usage({{1, true, {filament(0, 1000), unused}}});
    REQUIRE(result.complete());
    REQUIRE(result.rows.size() == 1);
    REQUIRE_THAT(result.rows[0].grams, WithinRel(1.24));
}

TEST_CASE("Different colors finishes profiles and brands remain separate", "[FilamentUsage]")
{
    auto color = filament(0, 1000), finish = color, profile = color, brand = color, diameter = color;
    color.identity.color = "#000000";
    finish.identity.finish = "Silk";
    profile.identity.profile = "Custom PLA";
    brand.identity.brand = "Another brand";
    diameter.identity.diameter_mm = 2.85;
    const auto result = aggregate_filament_usage({{1, true, {filament(0, 1000)}}, {2, true, {color}},
        {3, true, {finish}}, {4, true, {profile}}, {5, true, {brand}}, {6, true, {diameter}}});
    REQUIRE(result.complete());
    REQUIRE(result.rows.size() == 6);
}

TEST_CASE("Unidentified filaments remain local to their plate and slot", "[FilamentUsage]")
{
    auto first = filament(0, 1000), second = filament(1, 1000);
    first.identity.known = second.identity.known = false;
    const auto result = aggregate_filament_usage({{1, true, {first, second}}, {2, true, {first}}});
    REQUIRE(result.rows.size() == 3);
    REQUIRE(result.rows[0].fallback_plate_id == 1);
    REQUIRE(result.rows[2].fallback_plate_id == 2);
}

TEST_CASE("Stale and missing slices do not contribute outdated quantities", "[FilamentUsage]")
{
    const auto result = aggregate_filament_usage({{1, true, {filament(0, 1000)}},
        {2, false, {filament(0, 9000)}}, {3, true, {}}});
    REQUIRE_FALSE(result.complete());
    const std::vector<size_t> incomplete {2, 3};
    REQUIRE(result.incomplete_plates == incomplete);
    REQUIRE(result.rows.size() == 1);
    REQUIRE_THAT(result.rows[0].grams, WithinRel(1.24));
}

TEST_CASE("Each measured contribution uses its own density and modeled volume", "[FilamentUsage]")
{
    auto first = filament(0, 1000), second = filament(4, 3000);
    first.density_g_cm3 = 1.1;
    second.density_g_cm3 = 1.3;
    const auto result = aggregate_filament_usage({{1, true, {first}}, {2, true, {second}}});
    REQUIRE(result.complete());
    REQUIRE(result.rows.size() == 1);
    REQUIRE_THAT(result.rows[0].grams, WithinRel(1.1 + 3.9));
    const double area = std::acos(-1.0) * 1.75 * 1.75 / 4;
    REQUIRE_THAT(result.rows[0].meters, WithinRel(4000.0 / area / 1000.0));
}

TEST_CASE("Invalid measurements and unresolved mixed quantities remain unavailable", "[FilamentUsage]")
{
    auto invalid = filament(0, 1000), virtual_filament = filament(1, 1000);
    invalid.density_g_cm3 = std::numeric_limits<double>::quiet_NaN();
    virtual_filament.physical = false;
    virtual_filament.identity.profile = "Mixed PLA";
    const auto result = aggregate_filament_usage({{1, true, {invalid, virtual_filament}}});
    REQUIRE_FALSE(result.complete());
    REQUIRE(result.rows.size() == 2);
    REQUIRE_FALSE(result.rows[0].has_quantity);
    REQUIRE_FALSE(result.rows[1].has_quantity);
    REQUIRE_FALSE(result.rows[0].complete);
}

TEST_CASE("Partial totals retain their incompleteness and each plate is counted once", "[FilamentUsage]")
{
    auto invalid = filament(0, -1);
    const auto result = aggregate_filament_usage({{1, true, {filament(0, 1000)}},
        {2, true, {invalid}}, {1, true, {filament(0, 1000)}}});
    REQUIRE_FALSE(result.complete());
    REQUIRE(result.rows.size() == 1);
    REQUIRE(result.rows[0].has_quantity);
    REQUIRE_FALSE(result.rows[0].complete);
    REQUIRE_THAT(result.rows[0].grams, WithinRel(1.24));
}
