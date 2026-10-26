#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SliceComparison.hpp"

#include <algorithm>
#include <string>

using namespace Slic3r;
TEST_CASE("A settings change can be compared with the actual preceding slice without replacing it", "[SliceComparisonPrint]")
{
    auto config = Test::multifilament_config(1, {
        {"layer_height", ".2"}, {"initial_layer_print_height", ".2"}, {"gcode_comments", "1"},
        {"filament_nozzle_map", "0"}, {"filament_density", "1.24"}, {"filament_diameter", "1.75"}, {"enable_arc_fitting", "0"}, {"enable_support", "0"},
        {"enable_prime_tower", "0"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"}, {"wall_loops", "2"}
    });
    Print first; Model first_model;
    Test::init_print({Test::cube(6)}, first, first_model, config);
    GCodeProcessorResult first_result;
    REQUIRE_FALSE(Test::gcode(first, &first_result).empty());
    auto before = make_slice_snapshot(first_result, &first.full_print_config(), &first.model(), 0, 0, "Before");
    REQUIRE(before.seconds);
    REQUIRE(before.grams);
    REQUIRE_FALSE(before.layers.empty());
    const auto original_setting = before.settings.at("wall_loops").value;

    config.set_deserialize_strict("wall_loops", "4");
    Print second; Model second_model;
    Test::init_print({Test::cube(6)}, second, second_model, config);
    GCodeProcessorResult second_result;
    REQUIRE_FALSE(Test::gcode(second, &second_result).empty());
    auto after = make_slice_snapshot(second_result, &second.full_print_config(), &second.model(), 0, 0, "After");
    const auto changes = compare_slice_settings(before, after);
    CHECK(std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
        return change.before == original_setting && change.after == "4";
    }));
    bool changed_paths = false;
    for (const auto& [z, layer] : before.layers) {
        const auto it = after.layers.find(z);
        const auto diff = compare_slice_paths(&layer, it == after.layers.end() ? nullptr : &it->second);
        changed_paths |= !diff.before_only.empty() || !diff.after_only.empty() || !diff.adjusted.empty();
    }
    CHECK(changed_paths);
    CHECK(before.settings.at("wall_loops").value == original_setting);
    CHECK(first.full_print_config().opt_int("wall_loops") == 2);
}
