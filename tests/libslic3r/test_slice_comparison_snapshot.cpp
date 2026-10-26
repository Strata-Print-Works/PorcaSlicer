#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/SliceComparison.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;
namespace {
void fill_result(GCodeProcessorResult& r)
{
    r.reset();
    r.id = 1;
    r.print_statistics.modes[0].time = 60;
    r.filament_densities = {1.f, 2.f};
    r.print_statistics.total_volumes_per_extruder = {{0, 1000}, {1, 2000}};
    GCodeProcessorResult::MoveVertex start;
    start.position = Vec3f(110, 220, .2f);
    r.moves.push_back(start);
    auto end = start;
    end.position.x() += 10;
    end.type = EMoveType::Extrude; end.delta_extruder = 2; end.feedrate = 50;
    end.width = .4f; end.height = .2f; end.time[0] = 2;
    r.moves.push_back(end);
}
}
TEST_CASE("Slice comparisons use each filament's density and physical bed coordinates", "[SliceComparisonSnapshot]")
{
    GCodeProcessorResult r; fill_result(r);
    DynamicPrintConfig config;
    config.set_deserialize_strict("filament_density", "1,2");
    const auto s = make_slice_snapshot(r, &config, nullptr, 100, 200, "A");
    REQUIRE(s.grams); CHECK_THAT(*s.grams, Catch::Matchers::WithinAbs(5., 1e-6));
    REQUIRE(s.seconds); CHECK_THAT(*s.seconds, Catch::Matchers::WithinAbs(60., 1e-6));
    REQUIRE(s.layers.size() == 1);
    const auto& p = s.layers.begin()->second.paths.front();
    CHECK_THAT(p.x0, Catch::Matchers::WithinAbs(10., 1e-6));
    CHECK_THAT(p.y0, Catch::Matchers::WithinAbs(20., 1e-6));
    CHECK(s.settings_available);
}
TEST_CASE("Unknown filament mass and omitted paths remain explicitly unavailable", "[SliceComparisonSnapshot]")
{
    GCodeProcessorResult r; fill_result(r); r.filament_densities.pop_back();
    const auto s = make_slice_snapshot(r, nullptr, nullptr, 0, 0, "A", 0);
    CHECK_FALSE(s.grams);
    CHECK_FALSE(s.paths_available);
    REQUIRE(s.layers.size() == 1);
    CHECK(s.layers.begin()->second.paths.empty());
    CHECK_THAT(s.layers.begin()->second.filament_mm, Catch::Matchers::WithinAbs(2., 1e-6));
}
TEST_CASE("Comparison settings are frozen at slice time and exclude connection secrets", "[SliceComparisonSnapshot]")
{
    GCodeProcessorResult r; fill_result(r);
    DynamicPrintConfig config;
    config.set_deserialize_strict("layer_height", ".2");
    config.set_deserialize_strict("printhost_apikey", "private");
    const auto s = make_slice_snapshot(r, &config, nullptr, 0, 0, "A");
    config.set_deserialize_strict("layer_height", ".3");
    CHECK(s.settings.at("layer_height").value == "0.2");
    CHECK(s.settings.count("printhost_apikey") == 0);
}

TEST_CASE("Internal preview points do not duplicate extrusion and material identities survive slot numbering", "[SliceComparisonSnapshot]")
{
    GCodeProcessorResult r; fill_result(r);
    auto synthetic = r.moves.back(); synthetic.internal_only = true; synthetic.position.x() -= 5;
    r.moves.insert(r.moves.begin() + 1, synthetic);
    DynamicPrintConfig config;
    config.set_deserialize_strict("filament_colour", "#FF0000;#FFFFFF");
    const auto a = make_slice_snapshot(r, &config, nullptr, 100, 200, "A");
    REQUIRE(a.layers.size() == 1);
    REQUIRE(a.layers.begin()->second.paths.size() == 1);
    REQUIRE(a.layers.begin()->second.seconds);
    CHECK_THAT(*a.layers.begin()->second.seconds, Catch::Matchers::WithinAbs(2., 1e-6));
    config.set_deserialize_strict("filament_colour", "#0000FF;#FFFFFF");
    const auto b = make_slice_snapshot(r, &config, nullptr, 100, 200, "B");
    const auto diff = compare_slice_paths(&a.layers.begin()->second, &b.layers.begin()->second);
    CHECK(diff.adjusted.size() == 1);
    CHECK(diff.shared.empty());
}

TEST_CASE("Parser fallback densities do not become measured mass for older slices", "[SliceComparisonSnapshot]")
{
    GCodeProcessorResult r; fill_result(r);
    const auto s = make_slice_snapshot(r, nullptr, nullptr, 0, 0, "Loaded");
    CHECK_FALSE(s.grams);
    CHECK(s.seconds);
    CHECK_FALSE(s.settings_available);
}
