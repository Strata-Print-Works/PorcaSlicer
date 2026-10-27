#include "SnapmakerPrintTask.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#define L(s) s // Mark messages for translation without linking the pure protocol helper to wx.

namespace Slic3r {
using nlohmann::json;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SnapmakerPrintFilament, id, default_toolhead, type, color,
    nozzle_diameter, volume_type, temperature, flow_ratio, max_volumetric_speed, used_mm)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SnapmakerPrintTask, filaments, toolheads, calibrate_flow, calibrate_toolheads, toolhead_colors)

namespace {
std::string material_key(const std::string& value)
{
    std::string result;
    for (unsigned char c : value)
        if (std::isalnum(c)) result += static_cast<char>(std::toupper(c));
    return result;
}

void validate_slice(const SnapmakerPrintTask& task)
{
    if (task.filaments.empty() || task.filaments.size() > 16 || task.toolheads.size() != task.filaments.size())
        throw std::invalid_argument(L("The sliced filament assignments are incomplete. Slice this plate again."));
    std::set<int> ids;
    std::set<int> heads;
    bool calibration_selected = false;
    for (size_t i = 0; i < task.filaments.size(); ++i) {
        const auto& f = task.filaments[i];
        const int head = task.toolheads[i];
        if (f.id < 0 || f.id >= 16 || !ids.insert(f.id).second || head < 0 || head >= 4 || !heads.insert(head).second)
            throw std::invalid_argument(L("Assign each filament to a different U1 toolhead."));
        if (f.type.empty() || f.type.size() > 64 || f.type.find_first_of("\r\n\t';#") != std::string::npos ||
            !std::isfinite(f.nozzle_diameter) || f.nozzle_diameter <= 0. || f.nozzle_diameter > 1.2 ||
            !std::isfinite(f.temperature) || f.temperature <= 0. || f.temperature > 300. ||
            !std::isfinite(f.flow_ratio) || f.flow_ratio <= 0. || f.flow_ratio > 3. ||
            !std::isfinite(f.max_volumetric_speed) || f.max_volumetric_speed < 0. ||
            f.max_volumetric_speed > 1000. ||
            !std::isfinite(f.used_mm) || f.used_mm <= 0. ||
            (f.volume_type != "standard" && f.volume_type != "high_flow"))
            throw std::invalid_argument(L("The sliced filament settings are incomplete or unsupported by the U1. Check this plate's settings and slice again."));
        calibration_selected |= task.calibrate_toolheads[head];
    }
    // Snapmaker's current SET_PRINT_TASK_PARAMETERS checks every used physical
    // head against nozzle_diameter[0]. Do not hide that firmware restriction by
    // omitting known nozzle metadata and bypassing its compatibility check.
    for (const auto& filament : task.filaments)
        if (std::abs(filament.nozzle_diameter - task.filaments.front().nozzle_diameter) >= 0.001)
            throw std::invalid_argument(L("U1 print setup requires the same nozzle diameter for every used toolhead. Use matching nozzles and slice again."));
    if (task.calibrate_flow && !calibration_selected)
        throw std::invalid_argument(L("Choose at least one assigned toolhead for flow calibration."));
}

// Klipper removes a parameter's surrounding quotes. Single quotes preserve the
// double quotes of the Python-compatible JSON lists passed to ast.literal_eval.
std::string parameter(const char* key, const json& value)
{
    return std::string(" ") + key + "='" + value.dump(-1, ' ', true) + "'";
}
} // namespace

bool parse_snapmaker_printer_state(const std::string& status, const std::string& commands,
                                  SnapmakerPrinterState& state)
{
    if (status.size() > 1024 * 1024 || commands.size() > 1024 * 1024) return false;
    try {
        SnapmakerPrinterState parsed;
        const auto objects = json::parse(status).at("result").at("status");
        const auto help = json::parse(commands).at("result");
        if (!help.is_object() || !help.contains("SDCARD_PRINT_FILE_WITH_PARAMETERS")) return false;
        const auto& config = objects.at("print_task_config");
        const auto& loaded = config.at("filament_exist");
        const auto& types = config.at("filament_type");
        if (!loaded.is_array() || !types.is_array() || loaded.size() != 4 || types.size() != 4) return false;
        const std::string print_state = objects.at("print_stats").at("state").get<std::string>();
        parsed.ready = print_state == "standby" || print_state == "complete" || print_state == "cancelled";
        parsed.flow_calibration = config.at("flow_calibrate").get<bool>();
        parsed.supports_flow_calibration = help.contains("FLOW_CALIBRATE");
        if (config.contains("flow_calib_extruders")) {
            if (!config.at("flow_calib_extruders").is_array() || config.at("flow_calib_extruders").size() != 4)
                return false;
            parsed.calibrate_toolheads = config.at("flow_calib_extruders").get<std::array<bool, 4>>();
        }
        for (size_t i = 0; i < 4; ++i) {
            auto& head = parsed.toolheads[i];
            head.loaded = loaded.at(i).get<bool>();
            head.type = types.at(i).get<std::string>();
            if (config.contains("filament_sub_type")) {
                const auto sub = config.at("filament_sub_type").at(i).get<std::string>();
                if (sub == "CF" || sub == "GF") head.type += "-" + sub;
            }
            if (config.contains("filament_color_rgba"))
                head.color = config.at("filament_color_rgba").at(i).get<std::string>();
            const auto& extruder = objects.at(i == 0 ? "extruder" : "extruder" + std::to_string(i));
            head.nozzle_diameter = extruder.at("nozzle_diameter").get<double>();
            head.volume_type = extruder.at("nozzle_volume_type").get<std::string>();
            if (!std::isfinite(head.nozzle_diameter) || head.nozzle_diameter <= 0.) return false;
        }
        state = std::move(parsed);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

bool snapmaker_toolhead_matches(const SnapmakerPrintFilament& f, const SnapmakerToolhead& head)
{
    return head.loaded && !f.type.empty() && material_key(f.type) == material_key(head.type) &&
        std::abs(f.nozzle_diameter - head.nozzle_diameter) < 0.001 &&
        (head.volume_type == f.volume_type || (f.volume_type == "standard" && head.volume_type == "high_flow"));
}

bool snapmaker_colors_match(const std::string& first, const std::string& second)
{
    const auto normalized = [](std::string color) {
        if (!color.empty() && color.front() == '#') color.erase(0, 1);
        if (color.size() != 6 && color.size() != 8) return std::string();
        for (char& c : color) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return std::string();
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (color.size() == 6) color += "FF";
        return color;
    };
    const auto color = normalized(first);
    return !color.empty() && color == normalized(second);
}

bool validate_snapmaker_print_task(const SnapmakerPrintTask& task, const SnapmakerPrinterState& state,
                                   std::string& reason)
{
    try { validate_slice(task); }
    catch (const std::invalid_argument& e) { reason = e.what(); return false; }
    if (!state.ready) { reason = L("The U1 is not ready to start another print."); return false; }
    if (task.calibrate_flow && !state.supports_flow_calibration) {
        reason = L("This U1 does not report automatic flow calibration support."); return false;
    }
    for (size_t i = 0; i < task.filaments.size(); ++i) {
        if (!snapmaker_toolhead_matches(task.filaments[i], state.toolheads[task.toolheads[i]])) {
            reason = L("An assigned toolhead has a different filament, nozzle diameter or flow type. Refresh the printer information and review the assignments.");
            return false;
        }
        const int head = task.toolheads[i];
        if (!task.toolhead_colors[head].empty() &&
            !snapmaker_colors_match(task.toolhead_colors[head], state.toolheads[head].color)) {
            reason = L("An assigned toolhead's filament color changed. Reopen the send dialog and review its assignments.");
            return false;
        }
    }
    reason.clear();
    return true;
}

std::string encode_snapmaker_print_task(const SnapmakerPrintTask& task) { return json(task).dump(); }
SnapmakerPrintTask decode_snapmaker_print_task(const std::string& data) { return json::parse(data).get<SnapmakerPrintTask>(); }

std::string snapmaker_uploaded_path(const std::string& response)
{
    const auto reply = json::parse(response);
    if (reply.contains("error") || reply.at("print_started").get<bool>() || reply.at("print_queued").get<bool>() ||
        reply.at("item").at("root").get<std::string>() != "gcodes")
        throw std::invalid_argument(L("The U1 did not confirm an upload without starting it. Check the printer before trying again."));
    const auto path = reply.at("item").at("path").get<std::string>();
    if (path.empty()) throw std::invalid_argument(L("The U1 did not confirm the uploaded filename."));
    return path;
}

std::string snapmaker_start_command(const std::string& filename, const SnapmakerPrintTask& task)
{
    validate_slice(task);
    if (filename.empty() || filename.size() > 1024 || filename.front() == '/' ||
        filename.find_first_of("\r\n\t\"';#\\") != std::string::npos ||
        std::any_of(filename.begin(), filename.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument(L("Use a filename without quotes, # or semicolons to print with U1 settings."));
    const std::string wrapped = "/" + filename + "/";
    if (wrapped.find("/../") != std::string::npos || wrapped.find("/./") != std::string::npos)
        throw std::invalid_argument(L("Choose a filename inside the printer's G-code storage."));
    std::string extension = filename.size() >= 6 ? filename.substr(filename.size() - 6) : std::string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".gcode")
        throw std::invalid_argument(L("Use a .gcode filename to start this U1 print."));
    size_t count = 0;
    for (const auto& f : task.filaments) count = std::max(count, size_t(f.id + 1));
    json mapping = json::array(), calibration = json::array();
    std::vector<std::string> types(count), volumes(count, "standard");
    std::vector<double> diameters(count, task.filaments.front().nozzle_diameter), temperatures(count, 0.);
    std::vector<double> ratios(count, 1.), speeds(count, 0.), used(count, 0.);
    std::set<int> calibration_heads;
    for (size_t i = 0; i < task.filaments.size(); ++i) {
        const auto& f = task.filaments[i];
        mapping.push_back({f.id, task.toolheads[i]});
        types[f.id] = f.type;
        volumes[f.id] = f.volume_type;
        diameters[f.id] = f.nozzle_diameter;
        temperatures[f.id] = f.temperature;
        ratios[f.id] = f.flow_ratio;
        speeds[f.id] = f.max_volumetric_speed;
        used[f.id] = f.used_mm;
        if (task.calibrate_flow && task.calibrate_toolheads[task.toolheads[i]])
            calibration_heads.insert(task.toolheads[i]);
    }
    for (int head : calibration_heads) calibration.push_back(head);
    return "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"" + filename + "\"" +
        parameter("MAP_TABLE", mapping) + " FLOW_CALIBRATE=" + (task.calibrate_flow ? "1" : "0") +
        parameter("FLOW_CALIBRATE_EXTRUDERS", calibration) + parameter("FILAMENT_TYPE", types) +
        parameter("NOZZLE_DIAMETER_LIST", diameters) + parameter("NOZZLE_TEMP", temperatures) +
        parameter("FILAMENT_FLOW_RATIO", ratios) + parameter("FILAMENT_MAX_VOLUMETRIC_SPEED", speeds) +
        parameter("FILAMENT_VOLUME_TYPE", volumes) + parameter("FILAMENT_USED_MM", used);
}
} // namespace Slic3r

#undef L
