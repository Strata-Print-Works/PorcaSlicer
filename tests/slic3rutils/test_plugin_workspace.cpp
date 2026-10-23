#include <catch2/catch_test_macros.hpp>
#include "slic3r/plugin/host/PluginHostWorkspace.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include <nlohmann/json.hpp>
#include <string>

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
