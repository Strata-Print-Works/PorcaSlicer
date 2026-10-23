#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PreviewEstimate.hpp"
#include <limits>
#include <optional>

using Slic3r::GUI::PreviewEstimate;
using Catch::Matchers::WithinAbs;

TEST_CASE("Scope estimates use each slice's material measurements", "[PreviewEstimate]")
{
    PreviewEstimate total;
    total.add_time(3600.0);
    total.add_filament(10000.0, 1.2, 20.0); // 12 grams, 0.24 cost
    total.add_time(1200.0);
    total.add_filament(10000.0, 1.5, 30.0); // 15 grams, 0.45 cost
    REQUIRE(total.seconds);
    REQUIRE(total.grams);
    REQUIRE(total.cost);
    CHECK_THAT(*total.seconds, WithinAbs(4800.0, 1e-9));
    CHECK_THAT(*total.grams, WithinAbs(27.0, 1e-9));
    CHECK_THAT(*total.cost, WithinAbs(.69, 1e-9));
}

TEST_CASE("Unusable prices leave measured weight available", "[PreviewEstimate]")
{
    const auto price = GENERATE(std::optional<double>(), std::optional<double>(-1.0),
                               std::optional<double>(std::numeric_limits<double>::quiet_NaN()),
                               std::optional<double>(std::numeric_limits<double>::infinity()));
    PreviewEstimate total;
    total.add_filament(1000.0, 1.2, price);
    total.add_filament(1000.0, 1.2, 20.0);
    REQUIRE(total.grams);
    CHECK_THAT(*total.grams, WithinAbs(2.4, 1e-9));
    CHECK_FALSE(total.cost);
}

TEST_CASE("Invalid extrusion measurements do not become partial scope totals", "[PreviewEstimate]")
{
    const double bad = GENERATE(-1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity());
    const bool invalid_density = GENERATE(false, true);
    PreviewEstimate total;
    total.add_filament(invalid_density ? 1000.0 : bad, invalid_density ? bad : 1.2, 20.0);
    total.add_filament(1000.0, 1.2, 20.0);
    CHECK_FALSE(total.grams);
    CHECK_FALSE(total.cost);
    REQUIRE(total.seconds);
}

TEST_CASE("Unused materials do not make a measured scope incomplete", "[PreviewEstimate]")
{
    PreviewEstimate total;
    total.add_filament(0.0, 0.0, std::nullopt);
    total.add_filament(1000.0, 1.2, 0.0);
    REQUIRE(total.grams);
    REQUIRE(total.cost);
    CHECK_THAT(*total.grams, WithinAbs(1.2, 1e-9));
    CHECK_THAT(*total.cost, WithinAbs(0.0, 1e-9));
}

TEST_CASE("Overflowing scope quantities remain unavailable", "[PreviewEstimate]")
{
    const double huge = std::numeric_limits<double>::max();
    PreviewEstimate total;
    total.add_filament(huge, huge, 20.0);
    CHECK_FALSE(total.grams);
    CHECK_FALSE(total.cost);
    total.add_time(huge);
    total.add_time(huge);
    CHECK_FALSE(total.seconds);
}

TEST_CASE("Overflowing prices do not invalidate measured filament weight", "[PreviewEstimate]")
{
    PreviewEstimate total;
    total.add_filament(1000000.0, 1.2, std::numeric_limits<double>::max());
    REQUIRE(total.grams);
    CHECK_THAT(*total.grams, WithinAbs(1200.0, 1e-9));
    CHECK_FALSE(total.cost);
}

TEST_CASE("Invalid print durations do not become negative or nonfinite estimates", "[PreviewEstimate]")
{
    const double bad = GENERATE(-1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::max());
    PreviewEstimate total;
    total.add_time(bad);
    total.add_time(60.0);
    CHECK_FALSE(total.seconds);
}
