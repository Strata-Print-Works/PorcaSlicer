#include "SliceComparison.hpp"
#include "Config.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "I18N.hpp"
#include "Model.hpp"
#include "Point.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace Slic3r {
namespace {
bool comparison_setting(const std::string& key)
{
    static const std::set<std::string> bookkeeping {
        "different_settings_to_system", "inherits", "printer_settings_id", "print_settings_id",
        "filament_settings_id", "compatible_printers", "compatible_prints", "enable_filament_dynamic_map"
    };
    if (bookkeeping.count(key)) return false;
    for (const char* secret : {"print_host", "printhost", "password", "apikey", "api_key", "access_code", "token", "certificate"})
        if (key.find(secret) != std::string::npos) return false;
    return true;
}
void capture_settings(SliceSnapshot& snapshot, const ConfigBase& config, const std::string& identity = {}, const std::string& scope = {})
{
    for (const auto& key : config.keys()) {
        if (!comparison_setting(key)) continue;
        const auto* def = config.def() ? config.def()->get(key) : nullptr;
        const std::string label = def ? (def->full_label.empty() ? def->label : def->full_label) : key;
        snapshot.settings[identity + key] = {scope + I18N::translate(label.empty() ? key : label), config.opt_serialize(key)};
    }
}
}
SliceSnapshot make_slice_snapshot(const GCodeProcessorResult& result, const DynamicPrintConfig* config,
                                 const Model* sliced_model, double origin_x, double origin_y,
                                 const std::string& label, size_t path_limit)
{
    SliceSnapshot snapshot;
    snapshot.label = label;
    snapshot.revision = result.id;
    const double seconds = result.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    if (std::isfinite(seconds) && seconds > 0) snapshot.seconds = seconds;
    double grams = 0;
    // The parser may fill in generic density defaults for older files. Only a
    // captured slicing configuration can establish what the measurement means.
    const auto* densities = config ? config->option<ConfigOptionFloats>("filament_density") : nullptr;
    bool complete = densities && !result.print_statistics.total_volumes_per_extruder.empty();
    for (const auto& [slot, volume] : result.print_statistics.total_volumes_per_extruder) {
        if (volume == 0) continue;
        if (!std::isfinite(volume) || volume < 0 || slot >= result.filament_densities.size() ||
            !std::isfinite(result.filament_densities[slot]) || result.filament_densities[slot] <= 0 ||
            !densities || slot >= densities->values.size() || !std::isfinite(densities->values[slot]) ||
            std::abs(densities->values[slot] - result.filament_densities[slot]) > .00001) complete = false;
        else grams += volume * result.filament_densities[slot] * .001;
    }
    if (complete) snapshot.grams = grams;
    snapshot.settings_available = config != nullptr;
    if (config) capture_settings(snapshot, *config);
    if (config && sliced_model) {
        for (const auto* object : sliced_model->objects) {
            if (!object->printable || std::none_of(object->instances.begin(), object->instances.end(),
                [](const ModelInstance* instance) { return instance->printable; })) continue;
            const auto identity = "object/" + std::to_string(object->id().id) + "/";
            const auto scope = object->name + " / ";
            capture_settings(snapshot, object->config.get(), identity, scope);
            for (const auto* volume : object->volumes)
                capture_settings(snapshot, volume->config.get(), identity + std::to_string(volume->id().id) + "/", scope + volume->name + " / ");
            for (const auto& [range, overrides] : object->layer_config_ranges) {
                const auto heights = std::to_string(range.first) + "–" + std::to_string(range.second) + " mm / ";
                capture_settings(snapshot, overrides.get(), identity + heights, scope + heights);
            }
        }
    }
    std::map<size_t, uint64_t> materials;
    for (size_t slot = 0; slot < result.filament_densities.size(); ++slot) {
        std::string identity;
        if (config) for (const char* key : {"filament_type", "filament_vendor", "filament_colour", "filament_settings_id"}) {
            const auto* values = config->option<ConfigOptionStrings>(key);
            if (values && slot < values->values.size()) identity += values->values[slot] + "\n";
        }
        if (slot < result.extruder_colors.size()) identity += result.extruder_colors[slot];
        if (slot < result.settings_ids.filament.size()) identity += result.settings_ids.filament[slot];
        if (identity.empty()) identity = "unknown/" + std::to_string(result.id) + "/" + std::to_string(slot);
        identity += "/" + std::to_string(result.filament_densities[slot]);
        if (slot < result.filament_diameters.size()) identity += "/" + std::to_string(result.filament_diameters[slot]);
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : identity) { hash ^= c; hash *= 1099511628211ull; }
        materials[slot] = hash;
    }
    snapshot.paths_available = result.belt_tilt_angle == 0 && !result.machine_frame_transform_active;
    size_t path_count = 0;
    const GCodeProcessorResult::MoveVertex* previous = nullptr;
    for (const auto& move : result.moves) {
        if (move.internal_only) continue;
        if (previous && move.type == EMoveType::Extrude && move.delta_extruder > 0) {
            const double z = move.print_z > 0 ? move.print_z : move.position.z();
            if (!std::isfinite(move.feedrate) || !std::isfinite(move.width) || !std::isfinite(move.height) ||
                !std::isfinite(move.mm3_per_mm) || !std::isfinite(move.delta_extruder) ||
                !std::isfinite(z) || std::abs(z) > 1000000 || !move.position.allFinite() || !previous->position.allFinite() || move.position.cwiseAbs().maxCoeff() > 1000000 || previous->position.cwiseAbs().maxCoeff() > 1000000) {
                snapshot.paths_available = false;
                previous = &move;
                continue;
            }
            auto& layer = snapshot.layers[std::llround(z * 1000.)];
            layer.z = z;
            layer.filament_mm += move.delta_extruder;
            auto& feature = snapshot.features[uint16_t(move.extrusion_role)];
            const double duration = move.time[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
            if (std::isfinite(duration) && duration > 0) feature.seconds += duration;
            else feature.time_available = false;
            // An absent duration makes the whole layer unavailable, not a partial sum.
            if (layer.filament_mm == move.delta_extruder) layer.seconds = 0.;
            if (!std::isfinite(duration) || duration <= 0) layer.seconds.reset();
            else if (layer.seconds) *layer.seconds += duration;
            if (++path_count <= path_limit && snapshot.paths_available)
                layer.paths.push_back({previous->position.x() - float(origin_x), previous->position.y() - float(origin_y),
                    move.position.x() - float(origin_x), move.position.y() - float(origin_y), move.feedrate,
                    move.width, move.height, uint16_t(move.extrusion_role), move.extruder_id, materials.count(move.extruder_id) ? materials.at(move.extruder_id) : uint64_t(result.id), move.mm3_per_mm});
            else snapshot.paths_available = false;
        }
        previous = &move;
    }
    for (auto& [role, feature] : snapshot.features) {
        const auto found = result.print_statistics.used_filaments_per_role.find(ExtrusionRole(role));
        if (complete && found != result.print_statistics.used_filaments_per_role.end() &&
            std::isfinite(found->second.second) && found->second.second >= 0) feature.grams = found->second.second;
    }
    // Never show a sampled or truncated overlay as if it were the full toolpath.
    if (!snapshot.paths_available)
        for (auto& [z, layer] : snapshot.layers) std::vector<ComparedPath>().swap(layer.paths);
    return snapshot;
}
} // namespace Slic3r
