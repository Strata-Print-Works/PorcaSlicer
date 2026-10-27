#pragma once

#include <array>
#include <string>
#include <vector>

namespace Slic3r {

struct SnapmakerToolhead {
    bool loaded = false;
    std::string type;
    std::string color;
    double nozzle_diameter = 0.;
    std::string volume_type;
};

struct SnapmakerPrinterState {
    std::array<SnapmakerToolhead, 4> toolheads;
    bool ready = false;
    bool flow_calibration = false;
    bool supports_flow_calibration = false;
    std::array<bool, 4> calibrate_toolheads {{true, true, true, true}};
};

// IDs are the zero-based logical filament IDs emitted as T<n> in the sliced G-code.
// These values come from the selected plate's completed slice, never the live editor.
struct SnapmakerPrintFilament {
    int id = -1;
    int default_toolhead = -1;
    std::string type;
    std::string color;
    double nozzle_diameter = 0.;
    std::string volume_type;
    double temperature = 0.;
    double flow_ratio = 1.;
    double max_volumetric_speed = 0.;
    double used_mm = 0.;
};

struct SnapmakerPrintTask {
    std::vector<SnapmakerPrintFilament> filaments;
    std::vector<int> toolheads;
    bool calibrate_flow = false;
    std::array<bool, 4> calibrate_toolheads {{true, true, true, true}};
    std::array<std::string, 4> toolhead_colors;
};

bool parse_snapmaker_printer_state(const std::string& status, const std::string& commands,
                                  SnapmakerPrinterState& state);
bool snapmaker_toolhead_matches(const SnapmakerPrintFilament& filament, const SnapmakerToolhead& toolhead);
bool snapmaker_colors_match(const std::string& first, const std::string& second);
bool validate_snapmaker_print_task(const SnapmakerPrintTask& task, const SnapmakerPrinterState& state,
                                   std::string& reason);
std::string encode_snapmaker_print_task(const SnapmakerPrintTask& task);
SnapmakerPrintTask decode_snapmaker_print_task(const std::string& data);
std::string snapmaker_uploaded_path(const std::string& response);
// Throws for invalid IDs, incomplete slice data or a filename unsafe for Klipper's parser.
std::string snapmaker_start_command(const std::string& filename, const SnapmakerPrintTask& task);

} // namespace Slic3r
