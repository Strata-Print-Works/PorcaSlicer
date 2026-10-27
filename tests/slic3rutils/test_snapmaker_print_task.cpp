#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/Utils/SnapmakerPrintTask.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace Slic3r;
using nlohmann::json;

namespace {
SnapmakerPrintTask task()
{
    SnapmakerPrintFilament f;
    f.id = 2; // Sparse logical IDs must not be compacted to T0.
    f.default_toolhead = 1;
    f.type = "PLA";
    f.color = "#FF8800";
    f.nozzle_diameter = .4;
    f.volume_type = "standard";
    f.temperature = 215.;
    f.flow_ratio = .98;
    f.max_volumetric_speed = 18.;
    f.used_mm = 1234.;
    SnapmakerPrintTask t;
    t.filaments = {f};
    t.toolheads = {3};
    t.calibrate_flow = true;
    t.toolhead_colors[3] = "FF8800FF";
    return t;
}

json status()
{
    json objects;
    objects["print_task_config"] = {
        {"filament_exist", {true, true, false, true}},
        {"filament_type", {"PLA", "PETG", "NONE", "PLA"}},
        {"filament_color_rgba", {"FFFFFFFF", "FFFFFFFF", "FFFFFFFF", "FF8800FF"}},
        {"flow_calibrate", false}, {"flow_calib_extruders", {true, true, true, true}}};
    objects["print_stats"] = {{"state", "standby"}};
    for (int i = 0; i < 4; ++i)
        objects[i == 0 ? "extruder" : "extruder" + std::to_string(i)] = {
            {"nozzle_diameter", .4}, {"nozzle_volume_type", "standard"}};
    return {{"result", {{"status", objects}}}};
}

json commands()
{
    return {{"result", {{"SDCARD_PRINT_FILE_WITH_PARAMETERS", "Start with settings"},
                         {"FLOW_CALIBRATE", "Calibrate flow"}}}};
}

SnapmakerPrinterState printer()
{
    SnapmakerPrinterState result;
    REQUIRE(parse_snapmaker_printer_state(status().dump(), commands().dump(), result));
    return result;
}
} // namespace

TEST_CASE("U1 print setup preserves sparse logical filament IDs and physical calibration heads", "[SnapmakerPrintTask]")
{
    const auto t = task();
    const std::string command = snapmaker_start_command("folder/part one.gcode", t);
    CHECK(command.find("SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"folder/part one.gcode\"") == 0);
    CHECK(command.find("MAP_TABLE='[[2,3]]'") != std::string::npos);
    CHECK(command.find("FLOW_CALIBRATE=1") != std::string::npos);
    CHECK(command.find("FLOW_CALIBRATE_EXTRUDERS='[3]'") != std::string::npos);
    CHECK(command.find("FILAMENT_TYPE='[\"\",\"\",\"PLA\"]'") != std::string::npos);
    CHECK(command.find("FILAMENT_USED_MM='[0.0,0.0,1234.0]'") != std::string::npos);
    CHECK(command.find('\n') == std::string::npos);
}

TEST_CASE("Queued U1 print choices own their sliced settings", "[SnapmakerPrintTask]")
{
    auto original = task();
    const auto serialized = encode_snapmaker_print_task(original);
    original.filaments[0].temperature = 290.;
    original.toolheads[0] = 0;
    const auto queued = decode_snapmaker_print_task(serialized);
    CHECK_THAT(queued.filaments[0].temperature, Catch::Matchers::WithinAbs(215., 0.0001));
    CHECK(queued.toolheads == std::vector<int>{3});
    CHECK(encode_snapmaker_print_task(queued) == serialized);
}

TEST_CASE("U1 assignments reject changed materials nozzles and unavailable toolheads", "[SnapmakerPrintTask]")
{
    const auto t = task();
    const auto current = printer();
    std::string reason;
    REQUIRE(validate_snapmaker_print_task(t, current, reason));
    for (int change = 0; change < 7; ++change) {
        auto changed = current;
        if (change == 0) changed.toolheads[3].loaded = false;
        if (change == 1) changed.toolheads[3].type = "PETG";
        if (change == 2) changed.toolheads[3].nozzle_diameter = .6;
        if (change == 3) changed.toolheads[3].volume_type.clear();
        if (change == 4) changed.ready = false;
        if (change == 5) changed.supports_flow_calibration = false;
        if (change == 6) changed.toolheads[3].color = "00FF00FF";
        CHECK_FALSE(validate_snapmaker_print_task(t, changed, reason));
        CHECK_FALSE(reason.empty());
    }
}

TEST_CASE("U1 calibration is limited to selected assigned physical toolheads", "[SnapmakerPrintTask]")
{
    auto t = task();
    t.calibrate_toolheads = {{true, true, true, false}};
    CHECK_THROWS_AS(snapmaker_start_command("part.gcode", t), std::invalid_argument);
    t.calibrate_flow = false;
    const auto command = snapmaker_start_command("part.gcode", t);
    CHECK(command.find("FLOW_CALIBRATE=0") != std::string::npos);
    CHECK(command.find("FLOW_CALIBRATE_EXTRUDERS='[]'") != std::string::npos);
    t.calibrate_flow = true;
    t.calibrate_toolheads[3] = true;
    CHECK(snapmaker_start_command("part.gcode", t).find("FLOW_CALIBRATE_EXTRUDERS='[3]'") != std::string::npos);
}

TEST_CASE("U1 setup rejects missing capabilities malformed snapshots and unknown nozzles", "[SnapmakerPrintTask]")
{
    SnapmakerPrinterState result;
    CHECK_FALSE(parse_snapmaker_printer_state("{}", commands().dump(), result));
    CHECK_FALSE(parse_snapmaker_printer_state(status().dump(), "{}", result));
    auto incomplete = status();
    incomplete["result"]["status"]["extruder3"].erase("nozzle_diameter");
    CHECK_FALSE(parse_snapmaker_printer_state(incomplete.dump(), commands().dump(), result));
    incomplete = status();
    incomplete["result"]["status"]["print_task_config"]["flow_calib_extruders"] = {true, true};
    CHECK_FALSE(parse_snapmaker_printer_state(incomplete.dump(), commands().dump(), result));
}

TEST_CASE("U1 start never guesses an upload path or accepts an already started upload", "[SnapmakerPrintTask]")
{
    json reply = {{"item", {{"root", "gcodes"}, {"path", "renamed.gcode"}}},
                  {"print_started", false}, {"print_queued", false}};
    CHECK(snapmaker_uploaded_path(reply.dump()) == "renamed.gcode");
    auto started = reply;
    started["print_started"] = true;
    CHECK_THROWS(snapmaker_uploaded_path(started.dump()));
    started = reply;
    started["print_queued"] = true;
    CHECK_THROWS(snapmaker_uploaded_path(started.dump()));
    reply["item"].erase("path");
    CHECK_THROWS(snapmaker_uploaded_path(reply.dump()));
    CHECK_THROWS(snapmaker_uploaded_path("{\"error\":\"busy\"}"));
    CHECK_THROWS(snapmaker_uploaded_path("<html>Login</html>"));
}

TEST_CASE("U1 print requests reject command injection and incomplete slicing data", "[SnapmakerPrintTask]")
{
    for (const std::string filename : {"part.gcode\nG28", "part.gcode;G28", "a\" b.gcode", "a'b.gcode", "../part.gcode", "a#b.gcode", "part.3mf"})
        CHECK_THROWS_AS(snapmaker_start_command(filename, task()), std::invalid_argument);
    auto invalid = task();
    invalid.toolheads[0] = -1;
    CHECK_THROWS_AS(snapmaker_start_command("part.gcode", invalid), std::invalid_argument);
    invalid = task();
    invalid.filaments[0].temperature = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(snapmaker_start_command("part.gcode", invalid), std::invalid_argument);
    invalid = task();
    invalid.filaments.push_back(invalid.filaments.front());
    invalid.filaments.back().id = 1;
    invalid.toolheads.push_back(3);
    CHECK_THROWS_AS(snapmaker_start_command("part.gcode", invalid), std::invalid_argument);
}

TEST_CASE("U1 nozzle compatibility allows standard jobs on high flow heads without guessing hardware", "[SnapmakerPrintTask]")
{
    auto f = task().filaments.front();
    auto head = printer().toolheads[3];
    head.volume_type = "high_flow";
    CHECK(snapmaker_toolhead_matches(f, head));
    head.volume_type = "standard";
    f.volume_type = "high_flow";
    CHECK_FALSE(snapmaker_toolhead_matches(f, head));
    head.volume_type.clear();
    CHECK_FALSE(snapmaker_toolhead_matches(f, head));
    CHECK(snapmaker_colors_match("#ff8800", "FF8800FF"));
    CHECK_FALSE(snapmaker_colors_match("", "FFFFFFFF"));
    CHECK_FALSE(snapmaker_colors_match("#FF8800", "FF880000"));
    auto mixed = task();
    mixed.filaments.push_back(mixed.filaments.front());
    mixed.filaments.back().id = 1;
    mixed.filaments.back().nozzle_diameter = .6;
    mixed.toolheads.push_back(1);
    CHECK_THROWS_AS(snapmaker_start_command("part.gcode", mixed), std::invalid_argument);
}
