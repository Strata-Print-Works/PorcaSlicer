#include "PluginHostWorkspace.hpp"
#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginUiThread.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include <cstdint>
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include <boost/functional/hash.hpp>
#include <nlohmann/json.hpp>
#include <pybind11/stl.h>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <system_error>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include <string>
#include <cstddef>
#include <utility>
#include <pybind11/pytypes.h>
#include "libslic3r/Point.hpp"

namespace Slic3r {
namespace {
using Json = nlohmann::json;
GUI::Plater& plater()
{
    auto* p = GUI::wxGetApp().plater();
    if (!p) throw std::runtime_error("The workspace is not available yet.");
    return *p;
}
bool private_setting(const std::string& key)
{
    return key == "print_host" || key == "print_host_webui" || key.compare(0, 10, "printhost_") == 0;
}
Json value(const ConfigOption& option)
{
    if (const auto* list = dynamic_cast<const ConfigOptionVectorBase*>(&option)) return list->vserialize();
    return option.serialize();
}
Json vec(const Vec3d& v) { return {v.x(), v.y(), v.z()}; }
Json orientation(const Geometry::Transformation& transform)
{
    const Vec3d rotation = transform.get_rotation() * (180. / PI);
    return {{"position_mm", vec(transform.get_offset())}, {"rotation_deg", vec(rotation)},
        {"scale", vec(transform.get_scaling_factor())}, {"mirror", vec(transform.get_mirror())}};
}
Json matrix(const Transform3d& m)
{
    Json rows = Json::array();
    for (int row = 0; row != 4; ++row)
        rows.push_back({m.matrix()(row, 0), m.matrix()(row, 1), m.matrix()(row, 2), m.matrix()(row, 3)});
    return rows;
}
DynamicPrintConfig plate_config(int index)
{
    auto& plates = plater().get_partplate_list();
    if (index < 0 || index >= plates.get_plate_count()) throw std::invalid_argument("Unknown plate.");
    auto* plate = plates.get_plate(index);
    DynamicPrintConfig config;
    if (index == plates.get_curr_plate_index()) config = GUI::wxGetApp().preset_bundle->full_config_secure();
    else {
        if (plate->print_setup().empty()) throw std::runtime_error("This plate has no independent settings snapshot.");
        config = plate->print_setup().config;
    }
    config.apply(*plate->config());
    return config;
}
Json slice_status(GUI::Plater& p, GUI::PartPlate& plate)
{
    const bool slicing_this_plate = p.background_process().running() &&
        p.background_process().get_current_plate() == &plate;
    const auto* result = plate.get_slice_result();
    const bool has_toolpaths = !slicing_this_plate && result && !result->moves.empty();
    const bool valid = has_toolpaths && plate.is_slice_result_valid();
    Json out = {{"sliced", valid}, {"state", slicing_this_plate ? "slicing" : valid ? "ready" :
        has_toolpaths ? "stale" : "not_sliced"}};
    if (valid) {
        out["estimated_time_seconds"] = result->print_statistics.modes[0].time;
        out["moves_total"] = result->moves.size();
        out["warnings_count"] = result->warnings.size();
        out["ready_for_print"] = plate.is_slice_result_ready_for_print();
        out["toolpath_outside"] = result->toolpath_outside;
        out["validation_error_code"] = result->gcode_check_result.error_code;
    }
    return out;
}
Json sliced_gcode()
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    const int index = plates.get_curr_plate_index();
    if (index < 0 || index >= plates.get_plate_count())
        return {{"state", "not_sliced"}, {"plate", 0}, {"name", ""}};
    auto* plate = plates.get_plate(index);
    const Json status = slice_status(p, *plate);
    const auto* result = plate->get_slice_result();
    return plugin_sliced_gcode_descriptor(status.at("state").get<std::string>(), index + 1,
        plate->get_plate_name(), result ? result->filename : std::string(), result ? result->id : 0);
}
Json scene(int requested)
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    const int target = requested == -1 ? plates.get_curr_plate_index() : requested - 1;
    if (requested != 0 && (target < 0 || target >= plates.get_plate_count())) throw std::invalid_argument("Unknown plate.");
    const auto& selected = p.get_selection().get_content();
    Json objects = Json::array();
    for (size_t oi = 0; oi < p.model().objects.size(); ++oi) {
        const auto* object = p.model().objects[oi];
        Json instances = Json::array();
        for (size_t ii = 0; ii < object->instances.size(); ++ii) {
            const int pi = plates.find_instance_belongs(int(oi), int(ii));
            if (requested != 0 && pi != target) continue;
            const auto* instance = object->instances[ii];
            auto found = selected.find(unsigned(oi));
            const auto box = object->instance_bounding_box(*instance);
            instances.push_back({{"instance", ii}, {"plate", pi + 1}, {"printable", instance->printable},
                {"selected", found != selected.end() && found->second.count(unsigned(ii)) != 0},
                {"transform", matrix(instance->get_matrix())},
                {"orientation", orientation(instance->get_transformation())},
                {"bounds_min_mm", vec(box.min)}, {"bounds_max_mm", vec(box.max)}});
        }
        if (instances.empty()) continue;
        Json parts = Json::array();
        for (const auto* volume : object->volumes)
            parts.push_back({{"id", volume->id().id}, {"name", volume->name}, {"type", int(volume->type())},
                {"filament", volume->extruder_id()},
                {"transform", matrix(volume->get_matrix())}, {"orientation", orientation(volume->get_transformation())},
                {"settings", plugin_workspace_config_values(volume->config.get())}});
        Json layer_ranges = Json::array();
        for (const auto& range : object->layer_config_ranges)
            layer_ranges.push_back({{"z_min_mm", range.first.first}, {"z_max_mm", range.first.second},
                {"settings", plugin_workspace_config_values(range.second.get())}});
        const auto* extruder = object->config.option("extruder");
        objects.push_back({{"id", object->id().id}, {"object_index", oi}, {"name", object->name},
            {"filament", extruder && extruder->getInt() > 0 ? extruder->getInt() : 1},
            {"printable", object->printable}, {"settings", plugin_workspace_config_values(object->config.get())},
            {"support_painted", object->is_fdm_support_painted()}, {"seam_painted", object->is_seam_painted()},
            {"custom_layering", object->has_custom_layering()}, {"layer_ranges", layer_ranges},
            {"layer_height_profile", object->layer_height_profile.get()}, {"parts", parts}, {"instances", instances}});
    }
    return objects;
}
Json context()
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    const auto& bundle = *GUI::wxGetApp().preset_bundle;
    Json list = Json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        auto* plate = plates.get_plate(i);
        Json shape = Json::array();
        for (const auto& point : plate->get_shape()) shape.push_back({point.x(), point.y()});
        list.push_back({{"plate", i + 1}, {"name", plate->get_plate_name()}, {"origin_mm", vec(plate->get_origin())},
            {"bed_shape", shape}, {"locked", plate->is_locked()}, {"slice_valid", plate->is_slice_result_valid()},
            {"slice", slice_status(p, *plate)},
            {"destination_name", plate->print_setup().destination_name}});
        // Each plate loads its own filaments; a plate without its own setup has
        // no list to report, as with its settings.
        try {
            const auto config = plate_config(i);
            list.back()["filaments"] = plugin_plate_filaments(config, plate->get_extruders(false, config, config));
        } catch (const std::runtime_error&) {}
    }
    auto& camera = p.get_camera();
    return {{"source", "live"}, {"context_token", plugin_workspace_token()},
        {"plate", plates.get_curr_plate_index() + 1}, {"plate_count", plates.get_plate_count()}, {"plates", list},
        {"machine", bundle.printers.get_edited_preset().name}, {"process", bundle.prints.get_edited_preset().name},
        {"filament", bundle.filaments.get_edited_preset().name}, {"filaments", bundle.filament_presets},
        {"view", p.is_preview_shown() ? "preview" : "prepare"}, {"slicing", p.is_background_process_slicing()},
        {"camera", {{"projection", camera.get_type_as_string()}, {"position_mm", vec(camera.get_position())},
                    {"target_mm", vec(camera.get_target())}, {"zoom", camera.get_zoom()}}}};
}
Json settings(const std::vector<std::string>& requested, int offset, int limit, int plate_number)
{
    auto& plates = plater().get_partplate_list();
    const int index = plate_number == -1 ? plates.get_curr_plate_index() : plate_number - 1;
    const auto config = plate_config(index);
    auto keys = requested.empty() ? config.keys() : requested;
    keys.erase(std::remove_if(keys.begin(), keys.end(), private_setting), keys.end());
    offset = std::max(0, offset); limit = std::clamp(limit, 1, 100);
    Json entries = Json::object();
    const size_t end = std::min(keys.size(), size_t(offset) + size_t(limit));
    const auto per_filament = plugin_filament_values(config,
        std::vector<std::string>(keys.begin() + std::min(keys.size(), size_t(offset)), keys.begin() + end));
    for (size_t i = size_t(offset); i < end; ++i) {
        const auto& key = keys[i];
        const auto* option = config.option(key);
        if (!option) { entries[key] = {{"error", "Unknown setting"}}; continue; }
        const auto* def = config.def()->get(key);
        entries[key] = {{"value", value(*option)}};
        if (const auto found = per_filament.find(key); found != per_filament.end()) entries[key]["per_filament"] = *found;
        if (def) {
            entries[key]["label"] = def->label;
            entries[key]["category"] = def->category;
            entries[key]["tooltip"] = def->tooltip;
            entries[key]["units"] = def->sidetext;
            entries[key]["allowed_values"] = def->enum_values;
        }
    }
    return {{"source", "live"}, {"plate", index + 1}, {"context_token", plugin_workspace_token()},
        {"settings", entries}, {"total", keys.size()}, {"next_offset", end < keys.size() ? Json(end) : Json(nullptr)}};
}
Json preview(int offset, int limit, int requested)
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    const int index = requested == -1 ? plates.get_curr_plate_index() : requested - 1;
    if (index < 0 || index >= plates.get_plate_count()) throw std::invalid_argument("Unknown plate.");
    auto* plate = plates.get_plate(index);
    Json out = {{"plate", index + 1}, {"source", "live"},
        {"sliced", false}, {"context_token", plugin_workspace_token()}};
    out["slice"] = slice_status(p, *plate);
    if (!out["slice"]["sliced"].get<bool>()) return out;
    const auto* result = plate->get_slice_result();
    out["sliced"] = true;
    out["toolpath_outside"] = result->toolpath_outside;
    out["moves_total"] = result->moves.size();
    out["warnings"] = Json::array();
    for (const auto& warning : result->warnings)
        out["warnings"].push_back({{"severity", warning.level}, {"message", warning.msg}});
    out["estimated_time_seconds"] = result->print_statistics.modes[0].time;
    out["prepare_time_seconds"] = result->print_statistics.modes[0].prepare_time;
    out["filament_by_extruder"] = Json::array();
    for (const auto& [extruder, volume] : result->print_statistics.total_volumes_per_extruder) {
        Json item = {{"extruder", extruder}, {"volume_mm3", volume}};
        if (extruder < result->filament_densities.size())
            item["mass_g"] = volume * result->filament_densities[extruder] / 1000.;
        out["filament_by_extruder"].push_back(std::move(item));
    }
    out["filament_changes"] = result->print_statistics.total_filament_changes;
    out["extruder_changes"] = result->print_statistics.total_extruder_changes;
    out["travel_distance_mm"] = result->print_statistics.total_travel_distance;
    out["note"] = "Toolpaths are bed-relative. Layer indices and move offsets are zero-based. Overhang area alone does not prove an unsupported print.";
    if (index == plates.get_curr_plate_index()) {
        if (auto* canvas = p.get_preview_canvas3D(); canvas && p.is_preview_loaded()) {
            out["visible_layer_range"] = canvas->get_gcode_viewer().get_layers_z_range();
            out["view_type"] = int(canvas->get_gcode_view_type());
        }
    }
    offset = std::max(0, offset); limit = std::clamp(limit, 1, 200);
    const size_t end = std::min(result->moves.size(), size_t(offset) + size_t(limit));
    out["moves"] = Json::array();
    for (size_t i = size_t(offset); i < end; ++i) {
        const auto& m = result->moves[i];
        out["moves"].push_back({{"offset", i}, {"position_mm", vec(m.position.cast<double>())},
            {"layer", m.layer_id}, {"role", ExtrusionEntity::role_to_string(m.extrusion_role)}, {"type", int(m.type)},
            {"extruder", m.extruder_id}, {"width_mm", m.width}, {"height_mm", m.height},
            {"speed_mm_s", m.feedrate}, {"flow_mm3_s", m.volumetric_rate()}, {"temperature_c", m.temperature},
            {"fan_percent", m.fan_speed}, {"gcode_line", m.gcode_id}});
    }
    out["next_offset"] = end < result->moves.size() ? Json(end) : Json(nullptr);
    return out;
}
Json preview_layers(int offset, int limit, int requested)
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    const int index = requested == -1 ? plates.get_curr_plate_index() : requested - 1;
    if (index < 0 || index >= plates.get_plate_count()) throw std::invalid_argument("Unknown plate.");
    auto* plate = plates.get_plate(index);
    Json out = {{"source", "live"}, {"plate", index + 1},
        {"context_token", plugin_workspace_token()}, {"slice", slice_status(p, *plate)},
        {"sliced", false}, {"layers", Json::array()}, {"next_offset", nullptr}};
    if (!out["slice"]["sliced"].get<bool>()) return out;
    out["sliced"] = true;

    struct LayerSummary {
        double z_mm = 0.;
        size_t first_move = 0;
        size_t last_move = 0;
        size_t move_count = 0;
        size_t extrusion_moves = 0;
        std::map<std::string, size_t> roles;
        std::set<unsigned int> extruders;
    };
    std::map<unsigned int, LayerSummary> layers;
    const auto& moves = plate->get_slice_result()->moves;
    for (size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        auto& layer = layers[move.layer_id];
        if (layer.move_count++ == 0) layer.first_move = i;
        layer.last_move = i;
        layer.z_mm = std::max(layer.z_mm, double(move.print_z > 0.f ? move.print_z : move.position.z()));
        if (move.type == EMoveType::Extrude) {
            ++layer.extrusion_moves;
            ++layer.roles[ExtrusionEntity::role_to_string(move.extrusion_role)];
            layer.extruders.insert(move.extruder_id);
        }
    }
    offset = std::max(0, offset); limit = std::clamp(limit, 1, 200);
    out["layers_total"] = layers.size();
    auto it = layers.begin();
    std::advance(it, std::min(size_t(offset), layers.size()));
    for (int count = 0; count < limit && it != layers.end(); ++count, ++it) {
        const auto& layer = it->second;
        out["layers"].push_back({{"layer", it->first}, {"z_mm", layer.z_mm},
            {"first_move_offset", layer.first_move}, {"last_move_offset", layer.last_move},
            {"move_count", layer.move_count}, {"extrusion_moves", layer.extrusion_moves},
            {"roles", layer.roles}, {"extruders", layer.extruders}});
    }
    const size_t end = std::min(layers.size(), size_t(offset) + size_t(limit));
    out["next_offset"] = end < layers.size() ? Json(end) : Json(nullptr);
    return out;
}
pybind11::object python_json(const Json& value)
{
    return pybind11::module_::import("json").attr("loads")(value.dump());
}
} // namespace

bool plugin_workspace_public_setting(const std::string& key) { return !private_setting(key); }

nlohmann::json plugin_workspace_config_values(const DynamicPrintConfig& config)
{
    Json out = Json::object();
    for (const auto& key : config.keys())
        if (!private_setting(key)) out[key] = value(*config.option(key));
    return out;
}

std::string PluginWorkspaceRevision::token(std::size_t plate, std::size_t state,
                                           const std::vector<const DynamicPrintConfig*>& configs)
{
    bool same = m_configs.size() == configs.size();
    for (std::size_t i = 0; same && i < configs.size(); ++i) same = m_configs[i] == *configs[i];
    if (!same) {
        m_configs.clear();
        for (const auto* config : configs) m_configs.push_back(*config);
        ++m_config_revision;
    }
    std::size_t seed = plate;
    boost::hash_combine(seed, state);
    boost::hash_combine(seed, m_config_revision);
    return std::to_string(seed);
}

std::string plugin_workspace_token()
{
    auto& p = plater();
    auto& plates = p.get_partplate_list();
    auto* plate = plates.get_curr_plate();
    const auto& bundle = *GUI::wxGetApp().preset_bundle;
    // A plate switch, an undoable edit, a moved object or a changed setting
    // invalidates an old suggestion. Edits are snapshotted, objects carry
    // revisions, and settings are compared rather than serialized per call.
    std::size_t state = p.model().id().id;
    boost::hash_combine(state, p.project_session_id());
    boost::hash_combine(state, p.undo_redo_stack_main().active_snapshot_time());
    boost::hash_combine(state, bundle.filament_presets);
    auto hash_matrix = [&state](const Transform3d& transform) {
        const double* values = transform.matrix().data();
        for (int i = 0; i < 16; ++i) boost::hash_combine(state, values[i]);
    };
    for (const auto* object : p.model().objects) {
        boost::hash_combine(state, object->id().id);
        boost::hash_combine(state, static_cast<const ModelConfig&>(object->config).timestamp());
        boost::hash_combine(state, object->layer_height_profile.timestamp());
        for (const auto& range : object->layer_config_ranges) {
            boost::hash_combine(state, range.first.first);
            boost::hash_combine(state, range.first.second);
            boost::hash_combine(state, range.second.timestamp());
        }
        for (const auto* volume : object->volumes) {
            boost::hash_combine(state, volume->id().id);
            boost::hash_combine(state, static_cast<const ModelConfig&>(volume->config).timestamp());
            for (const auto* facets : {&volume->supported_facets, &volume->seam_facets,
                                       &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets})
                boost::hash_combine(state, facets->timestamp());
            hash_matrix(volume->get_matrix());
        }
        for (const auto* instance : object->instances) {
            boost::hash_combine(state, instance->printable);
            hash_matrix(instance->get_matrix());
        }
    }
    static PluginWorkspaceRevision revision;
    std::size_t plate_id = std::size_t(plates.get_curr_plate_index());
    boost::hash_combine(plate_id, reinterpret_cast<std::uintptr_t>(plate));
    return revision.token(plate_id, state, {&bundle.prints.get_edited_preset().config,
        &bundle.printers.get_edited_preset().config, &bundle.filaments.get_edited_preset().config,
        &bundle.project_config, plate->config()});
}

nlohmann::json plugin_plate_filaments(const DynamicPrintConfig& config, const std::vector<int>& used)
{
    Json out = Json::array();
    const auto* presets = config.option<ConfigOptionStrings>("filament_settings_id");
    if (!presets) return out;
    const auto text = [&config](const char* key, size_t index) {
        const auto* option = config.option<ConfigOptionStrings>(key);
        return option && index < option->size() ? option->get_at(index) : std::string();
    };
    for (size_t i = 0; i < presets->size(); ++i)
        out.push_back({{"slot", i + 1}, {"preset", presets->get_at(i)}, {"type", text("filament_type", i)},
            {"colour", text("filament_colour", i)},
            {"used", std::find(used.begin(), used.end(), int(i + 1)) != used.end()}});
    return out;
}

nlohmann::json plugin_filament_values(const DynamicPrintConfig& config, const std::vector<std::string>& keys)
{
    static const std::set<std::string> filament_keys(Preset::filament_options().begin(), Preset::filament_options().end());
    // Printers with several extruder variants store one value per filament and
    // variant; slicing keeps the variant each filament's extruder uses (PrintApply).
    DynamicPrintConfig resolved = config;
    int extruder_count = 1;
    const bool different_extruder = resolved.support_different_extruders(extruder_count);
    if (extruder_count > 1 || different_extruder || resolved.has_multi_variant_filament()) {
        std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
        const int volume_count = resolved.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);
        std::set<std::string> variant_keys = filament_options_with_variant;
        variant_keys.insert("filament_self_index");
        resolved.update_values_to_printer_extruders_for_multiple_filaments(resolved, extruder_count, volume_count, variant_keys,
                                                                         "filament_self_index", "filament_extruder_variant");
    }
    Json out = Json::object();
    for (const auto& key : keys)
        if (filament_keys.count(key) != 0)
            if (const auto* option = dynamic_cast<const ConfigOptionVectorBase*>(resolved.option(key))) out[key] = option->vserialize();
    return out;
}

nlohmann::json plugin_sliced_gcode_descriptor(const std::string& state, int plate,
    const std::string& name, const std::string& filename, unsigned int result_id)
{
    Json out = {{"state", state}, {"plate", plate}, {"name", name}};
    // Never publish a path to a stale result or a file still being written.
    if (state != "ready") return out;
    std::error_code error;
    const auto path = std::filesystem::u8path(filename);
    if (!std::filesystem::is_regular_file(path, error)) {
        out["state"] = "missing";
        return out;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) { out["state"] = "missing"; return out; }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) { out["state"] = "missing"; return out; }
    out["gcode_path"] = filename;
    out["size"] = size;
    // File identity also distinguishes slices restored from a different project.
    out["revision"] = Json::array({filename, result_id, size, modified.time_since_epoch().count()}).dump();
    return out;
}

void host_bindings::register_workspace(pybind11::module_& host)
{
    host.def("workspace_context", [] { return python_json(run_on_ui_blocking(context)); });
    host.def("sliced_gcode_snapshot", [] { return python_json(run_on_ui_blocking(sliced_gcode)); },
        "Read the active plate's slice state and completed G-code path. No export or post-processing. "
        "Only ready results include gcode_path and revision; reading the file remains subject to plugin permissions.");
    host.def("scene_snapshot", [](int plate) { return python_json(run_on_ui_blocking([=] { return scene(plate); })); },
             pybind11::arg("plate") = -1);
    host.def("settings_snapshot", [](const std::vector<std::string>& keys, int offset, int limit, int plate) {
        return python_json(run_on_ui_blocking([&] { return settings(keys, offset, limit, plate); }));
    }, pybind11::arg("keys") = std::vector<std::string>{}, pybind11::arg("offset") = 0,
       pybind11::arg("limit") = 80, pybind11::arg("plate") = -1);
    host.def("preview_snapshot", [](int offset, int limit, int plate) {
        return python_json(run_on_ui_blocking([=] { return preview(offset, limit, plate); }));
    }, pybind11::arg("offset") = 0, pybind11::arg("limit") = 100, pybind11::arg("plate") = -1);
    host.def("preview_layers_snapshot", [](int offset, int limit, int plate) {
        return python_json(run_on_ui_blocking([=] { return preview_layers(offset, limit, plate); }));
    }, pybind11::arg("offset") = 0, pybind11::arg("limit") = 60, pybind11::arg("plate") = -1);
    host.def("selection", [] {
        return python_json(run_on_ui_blocking([] {
            Json out = context(); out["objects"] = Json::array();
            for (const auto& object : scene(0)) for (const auto& instance : object["instances"])
                if (instance["selected"].get<bool>()) out["objects"].push_back({{"name", object["name"]},
                    {"object_index", object["object_index"]}, {"instance_index", instance["instance"]},
                    {"instances", 1}, {"plate", instance["plate"]}});
            return out;
        }));
    });
}
} // namespace Slic3r
