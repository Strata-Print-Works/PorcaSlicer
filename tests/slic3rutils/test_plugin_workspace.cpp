#include <catch2/catch_test_macros.hpp>
#include "slic3r/plugin/host/PluginHostWorkspace.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <fstream>
#include "test_utils.hpp"

TEST_CASE("Copilot setting snapshots preserve every filament value", "[PluginWorkspace]")
{
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new Slic3r::ConfigOptionFloats({0.4, 0.6}));
    config.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.28));
    const auto snapshot = Slic3r::plugin_workspace_config_values(config);
    REQUIRE(snapshot["nozzle_diameter"].is_array());
    CHECK(snapshot["nozzle_diameter"] == nlohmann::json({"0.4", "0.6"}));
    CHECK(snapshot["layer_height"] == "0.28");
}

TEST_CASE("Copilot sees which filament slots a plate prints with", "[PluginWorkspace]")
{
    Slic3r::DynamicPrintConfig config;
    config.set_key_value("filament_settings_id", new Slic3r::ConfigOptionStrings({"Generic PETG", "Bambu PLA Tough"}));
    config.set_key_value("filament_type", new Slic3r::ConfigOptionStrings({"PETG", "PLA"}));
    config.set_key_value("filament_colour", new Slic3r::ConfigOptionStrings({"#0ACC38", "#161616"}));
    const auto filaments = Slic3r::plugin_plate_filaments(config, {2});
    REQUIRE(filaments.size() == 2);
    CHECK(filaments[0] == nlohmann::json({{"slot", 1}, {"preset", "Generic PETG"}, {"type", "PETG"},
                                          {"colour", "#0ACC38"}, {"used", false}}));
    CHECK(filaments[1] == nlohmann::json({{"slot", 2}, {"preset", "Bambu PLA Tough"}, {"type", "PLA"},
                                          {"colour", "#161616"}, {"used", true}}));
}

TEST_CASE("Copilot reads each filament's own value on a printer with extruder variants", "[PluginWorkspace]")
{
    Slic3r::DynamicPrintConfig config;
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4,0.4"},
        {"extruder_variant_list", "\"Direct Drive Standard\";\"Bowden Standard\""},
        {"extruder_type", "Direct Drive,Bowden"},
        {"nozzle_volume_type", "Standard,Standard"},
        {"filament_map", "1,2"},
        {"filament_diameter", "1.75,1.75"},
        {"filament_self_index", "1,1,2,2"},
        {"filament_extruder_variant", "\"Direct Drive Standard\";\"Bowden Standard\";\"Direct Drive Standard\";\"Bowden Standard\""},
        {"nozzle_temperature", "255,250,220,215"},
        {"hot_plate_temp", "70,55"},
        {"layer_height", "0.2"},
        {"compatible_printers_condition", "nozzle_diameter[0]==0.4"},
    });
    const std::vector<std::string> keys{"nozzle_temperature", "hot_plate_temp", "layer_height", "compatible_printers_condition"};
    auto values = Slic3r::plugin_filament_values(config, keys);
    CHECK(values["nozzle_temperature"] == nlohmann::json({"255", "215"}));
    CHECK(values["hot_plate_temp"] == nlohmann::json({"70", "55"}));
    CHECK_FALSE(values.contains("layer_height"));
    // A filament preset's single values have no slot to resolve.
    CHECK_FALSE(values.contains("compatible_printers_condition"));
    // The live configuration is read, never changed.
    CHECK(config.option<Slic3r::ConfigOptionInts>("nozzle_temperature")->values.size() == 4);

    config.set_deserialize_strict({{"filament_map", "1,1"}});
    values = Slic3r::plugin_filament_values(config, keys);
    CHECK(values["nozzle_temperature"] == nlohmann::json({"255", "220"}));
}

TEST_CASE("Copilot setting snapshots omit local printer connection details", "[PluginWorkspace]")
{
    Slic3r::DynamicPrintConfig config;
    for (const auto* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_password", "printhost_cafile"})
        config.set_key_value(key, new Slic3r::ConfigOptionString("local-only"));
    config.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.2));
    const auto snapshot = Slic3r::plugin_workspace_config_values(config);
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot.contains("layer_height"));
}

TEST_CASE("Copilot context tokens change after a setting edit or a plate switch", "[PluginWorkspace]")
{
    Slic3r::PluginWorkspaceRevision revision;
    Slic3r::DynamicPrintConfig print;
    print.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.2));
    Slic3r::DynamicPrintConfig plate;
    const auto first = revision.token(0, 7, {&print, &plate});
    CHECK(revision.token(0, 7, {&print, &plate}) == first);

    print.set_key_value("layer_height", new Slic3r::ConfigOptionFloat(0.28));
    const auto edited = revision.token(0, 7, {&print, &plate});
    CHECK(edited != first);
    plate.set_key_value("skirt_start_angle", new Slic3r::ConfigOptionFloat(10.));
    const auto plate_edited = revision.token(0, 7, {&print, &plate});
    CHECK(plate_edited != edited);

    const auto switched = revision.token(1, 7, {&print, &plate});
    CHECK(switched != plate_edited);
    CHECK(revision.token(1, 8, {&print, &plate}) != switched);
}

TEST_CASE("Photo plugins cannot read stale or incomplete slice files", "[PluginWorkspace]")
{
    for (const auto* state : {"not_sliced", "slicing", "stale"}) {
        const auto snapshot = Slic3r::plugin_sliced_gcode_descriptor(state, 2, "Vase", "/unused.gcode", 42);
        CHECK(snapshot["state"] == state);
        CHECK(snapshot["plate"] == 2);
        CHECK_FALSE(snapshot.contains("gcode_path"));
        CHECK_FALSE(snapshot.contains("revision"));
    }
    const auto missing = Slic3r::plugin_sliced_gcode_descriptor("ready", 1, "", "", 1);
    CHECK(missing["state"] == "missing");
    CHECK_FALSE(missing.contains("gcode_path"));
}

TEST_CASE("Photo plugin slice revisions track replacement G-code", "[PluginWorkspace]")
{
    ScopedTemporaryDir temporary("photo-plugin-slice");
    const auto path = temporary.path() / "print.gcode";
    { std::ofstream out(path.string()); out << "; slice one\n"; }
    const auto first = Slic3r::plugin_sliced_gcode_descriptor("ready", 1, "Vase", path.string(), 1);
    CHECK(first["state"] == "ready");
    CHECK(first["gcode_path"] == path.string());
    CHECK(Slic3r::plugin_sliced_gcode_descriptor("ready", 1, "Vase", path.string(), 1)["revision"] == first["revision"]);
    CHECK(Slic3r::plugin_sliced_gcode_descriptor("ready", 1, "Vase", path.string(), 2)["revision"] != first["revision"]);
    { std::ofstream out(path.string()); out << "; a different longer slice\n"; }
    CHECK(Slic3r::plugin_sliced_gcode_descriptor("ready", 1, "Vase", path.string(), 1)["revision"] != first["revision"]);
}
