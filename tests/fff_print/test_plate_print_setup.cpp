#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include <sstream>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

TEST_CASE("Editing another plate does not invalidate a completed plate slice", "[PlatePrintSetup]")
{
    PresetBundle bundle;
    PlatePrintSetup first;
    first.config = Test::multifilament_config(1, {{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"gcode_comments", "1"}, {"filament_nozzle_map", "0"}});
    PlatePrintSetup second = first;
    second.config.set_deserialize_strict("layer_height", "0.3");
    second.config.set_deserialize_strict("initial_layer_print_height", "0.3");

    Print first_print, second_print;
    Model first_model, second_model;
    first.activate(bundle, "Plate 1");
    first.capture(bundle);
    auto first_input = bundle.full_config(false);
    first_input.erase("filament_map_2");
    Test::init_print({Test::cube(6)}, first_print, first_model, first_input);
    const std::string first_gcode = Test::gcode(first_print);
    REQUIRE(first_print.is_step_done(psGCodeExport));
    first.activate(bundle, "Plate 1");
    second.activate(bundle, "Plate 2");
    auto second_input = bundle.full_config(false);
    second_input.erase("filament_map_2");
    Test::init_print({Test::cube(6)}, second_print, second_model, second_input);
    const std::string second_gcode = Test::gcode(second_print);
    REQUIRE(second_print.is_step_done(psGCodeExport));
    REQUIRE(first_print.is_step_done(psGCodeExport));
    REQUIRE(first_gcode != second_gcode);

    first.activate(bundle, "Plate 1");
    auto restored = bundle.full_config(false);
    restored.erase("filament_map_2");
    std::ostringstream differences;
    for (const auto& key : first_print.config().diff(restored))
        differences << key << ": print=" << first_print.config().option(key)->serialize() << " restored=" << restored.option(key)->serialize() << "\n";
    INFO(differences.str());
    REQUIRE(first_print.apply(first_model, restored) == Print::APPLY_STATUS_UNCHANGED);
    REQUIRE(first_print.is_step_done(psGCodeExport));
    REQUIRE_THAT(first_print.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.2, 1e-9));
    REQUIRE_THAT(second_print.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.3, 1e-9));
}
