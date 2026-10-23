#include "PlatePrintSetup.hpp"
#include "PresetBundle.hpp"
#include "Config.hpp"
#include "Exception.hpp"
#include "Preset.hpp"
#include "DiscoveredPrinter.hpp"
#include "BoundingBox.hpp"
#include "I18N.hpp"
#include "Point.hpp"
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstddef>
#include <limits>
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace Slic3r {

Vec2d workspace_plate_stride(double width, double depth)
{
    return {std::max({width * 1.2, width + workspace_controls_clearance, 204.}),
            depth + std::max(depth * .2, workspace_title_clearance)};
}

Vec2d orca_plate_origin(int index, int count, double width, double depth)
{
    // PartPlateList::m_plate_width/m_plate_depth and the 3MF exporter use whole
    // millimetres. Matching them keeps fractional beds on the file's grid.
    width = std::trunc(width);
    depth = std::trunc(depth);
    const int columns = std::max(1, int(std::ceil(std::sqrt(double(count)))));
    return {index % columns * (width * 1.2), -(index / columns) * (depth * 1.2)};
}

namespace {
bool local_connection_setting(const std::string& key)
{
    static constexpr std::array<const char*, 8> keys {
        "print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
        "printhost_user", "printhost_password", "printhost_port", "enable_filament_dynamic_map"
    };
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

[[noreturn]] void unrestorable_plate_setting(const std::string& key)
{
    BOOST_LOG_TRIVIAL(error) << "Cannot restore plate setting " << key;
    throw RuntimeError(_u8L("This plate's printing setup can't be restored by this version of Porca."));
}
}

void PlatePrintSetup::capture(const PresetBundle& bundle)
{
    config = bundle.full_config_secure();
    // Live routing must be refreshed from the destination, not replayed from a file.
    config.erase("enable_filament_dynamic_map");
}

void PlatePrintSetup::activate(PresetBundle& bundle, const std::string& plate_name) const
{
    if (empty())
        throw RuntimeError("The plate has no printing setup.");
    bundle.load_config_model(plate_name, config);
}

const std::vector<std::string>& plate_process_edit_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> result;
        for (const auto& key : Preset::print_options()) {
            const auto* def = print_config_def.get(key);
            // Slot numbers and tool variants have different meanings on different
            // printers. Text, lists, scripts and plugin controls stay plate-local.
            if (!def || def->label.empty() || def->category.empty() || def->is_code || def->readonly ||
                key.find("filament") != std::string::npos || key.find("extruder") != std::string::npos)
                continue;
            if (key == "enable_wrapping_detection" || key == "wipe_tower_wall_type" ||
                key == "enable_mixed_color_sublayer" || key == "timelapse_type")
                continue; // These require printer capability or palette-specific UI.
            switch (def->type) {
            case coBool: case coInt: case coFloat: case coPercent: case coFloatOrPercent: case coEnum:
                result.push_back(key);
                break;
            default: break;
            }
        }
        return result;
    }();
    return keys;
}


namespace {
bool plate_process_edit_key(const std::string& key)
{
    static const std::set<std::string> keys(plate_process_edit_keys().begin(), plate_process_edit_keys().end());
    return keys.count(key) > 0;
}

nlohmann::json process_config_json(const DynamicPrintConfig& config)
{
    auto result = nlohmann::json::object();
    for (const auto& key : config.keys()) {
        const auto* option = config.option(key);
        if (!plate_process_edit_key(key)) {
            BOOST_LOG_TRIVIAL(error) << "Project or stage print settings contain unsupported key " << key;
            throw RuntimeError(_u8L("Some of this project's print settings can't be saved."));
        }
        result[key] = {{"value", option->serialize()}};
        if (option->type() == coFloat || option->type() == coPercent || option->type() == coFloatOrPercent)
            result[key]["number"] = option->getFloat();
    }
    return result;
}
[[noreturn]] void unsupported_process_setting(const std::string& key)
{
    BOOST_LOG_TRIVIAL(error) << "Cannot restore project or stage print setting " << key;
    throw RuntimeError(_u8L("This project's print settings can't be restored by this version of Porca."));
}

DynamicPrintConfig process_config_from_json(const nlohmann::json& values)
{
    if (!values.is_object()) unsupported_process_setting("(not an object)");
    DynamicPrintConfig result;
    for (const auto& item : values.items()) {
        if (!plate_process_edit_key(item.key())) unsupported_process_setting(item.key());
        const auto& entry = item.value();
        if (!entry.is_object() || !entry.contains("value") || !entry.at("value").is_string() ||
            (entry.contains("number") && !entry.at("number").is_number()))
            unsupported_process_setting(item.key());
        result.set_deserialize_strict({{item.key(), entry.at("value").get<std::string>()}});
        auto* option = result.option(item.key());
        if (entry.contains("number")) {
            const double number = entry.at("number").get<double>();
            if (!std::isfinite(number)) unsupported_process_setting(item.key());
            if (option->type() == coFloatOrPercent) static_cast<ConfigOptionFloatOrPercent*>(option)->value = number;
            else if (option->type() == coFloat || option->type() == coPercent) static_cast<ConfigOptionFloat*>(option)->value = number;
            else unsupported_process_setting(item.key());
        }
    }
    return result;
}

// Every setting read by the non-interactive dependency checks below.
constexpr const char* process_edit_dependency_keys[] = {
    "ironing_spacing", "support_ironing_spacing", "xy_hole_compensation", "xy_contour_compensation",
    "elefant_foot_compensation", "spiral_mode", "detect_thin_wall", "overhang_reverse", "timelapse_type",
    "enable_wrapping_detection", "alternate_extra_wall", "ensure_vertical_shell_thickness", "enable_support",
    "detect_overhang_wall", "support_style", "support_type", "seam_slope_type", "seam_slope_start_height",
    "layer_height", "initial_layer_print_height", "infill_lock_depth", "skin_infill_depth", "fuzzy_skin_mode",
    "wall_generator", "belt_printer", "raft_layers", "draft_shield", "brim_type"
};
}

std::string ProcessSettingsScopes::serialize() const
{
    auto stage_values = nlohmann::json::object();
    for (const auto& [stage, config] : stages) stage_values[stage] = process_config_json(config);
    return nlohmann::json{{"version", 1}, {"project", process_config_json(project)}, {"stages", stage_values}}.dump();
}
ProcessSettingsScopes ProcessSettingsScopes::deserialize(const std::string& value)
{
    if (value.empty()) return {};
    const auto data = nlohmann::json::parse(value);
    if (data.at("version") != 1 || !data.at("stages").is_object() || data.at("stages").size() > 256)
        throw RuntimeError(_u8L("This project's print settings require a newer version of Porca."));
    ProcessSettingsScopes result;
    result.project = process_config_from_json(data.at("project"));
    for (const auto& item : data.at("stages").items()) result.stages[item.key()] = process_config_from_json(item.value());
    return result;
}

void initialize_process_cascade(PlatePrintSetup& setup, const DynamicPrintConfig& preset,
                                const DynamicPrintConfig& plate_options)
{
    if (!setup.process_base.empty()) return;
    for (const auto& key : plate_process_edit_keys()) {
        const auto* current = plate_options.option(key);
        if (!current) current = setup.config.option(key);
        if (!current) continue;
        const auto* base = preset.option(key);
        setup.process_base.set_key_value(key, (base ? base : current)->clone());
        // Missing presets cannot establish which old values were intentional edits.
        if (!base || plate_options.has(key) || *base != *current)
            setup.process_overrides.set_key_value(key, current->clone());
    }
}
DynamicPrintConfig inherited_process_settings(const PlatePrintSetup& setup,
                                             const ProcessSettingsScopes& scopes, const std::string& stage)
{
    auto result = setup.process_base;
    result.apply(scopes.project);
    const auto found = scopes.stages.find(stage);
    if (found != scopes.stages.end()) result.apply(found->second);
    return result;
}
DynamicPrintConfig resolved_process_settings(const PlatePrintSetup& setup,
                                            const ProcessSettingsScopes& scopes, const std::string& stage)
{
    auto result = inherited_process_settings(setup, scopes, stage);
    result.apply(setup.process_overrides);
    return result;
}

PlateProcessEdit prepare_plate_process_edit(const DynamicPrintConfig& config,
                                          const DynamicPrintConfig& overrides,
                                          const DynamicPrintConfig& changes)
{
    PlateProcessEdit result;
    result.config = config;
    result.overrides = overrides;
    for (const auto& key : changes.keys()) {
        const auto* value = changes.option(key);
        const auto* original = config.option(key);
        if (!plate_process_edit_key(key) || !original || original->type() != value->type()) {
            result.errors.emplace(key, L("Change this setting on each plate."));
            continue;
        }
        result.config.set_key_value(key, value->clone());
        if (overrides.has(key)) result.overrides.set_key_value(key, value->clone());
    }
    if (!result.errors.empty()) return result;
    DynamicPrintConfig effective = result.config;
    effective.apply(result.overrides);
    // The checks below read these settings directly. A partial plate config
    // cannot be checked, so report it rather than dereferencing a missing option.
    for (const char* key : process_edit_dependency_keys) {
        const auto* option = effective.option(key);
        const auto* def = print_config_def.get(key);
        if (!option || !def || option->type() != def->type)
            result.errors[key] = L("Choose compatible print settings for this plate before applying.");
    }
    if (!result.errors.empty()) return result;
    // CLI validation reports dependent settings instead of silently correcting
    // them in a GUI prompt (for example, vase mode's walls, infill and supports).
    result.errors = effective.validate(true);
    // These are the non-interactive counterpart of the dependency corrections in
    // ConfigManipulation::update_print_fff_config. Bulk edits must report the
    // conflict; opening a plate later must not silently change a reviewed value.
    auto conflict = [&](const char* key) {
        result.errors[key] = L("Choose compatible print settings for this plate before applying.");
    };
    for (const char* key : {"ironing_spacing", "support_ironing_spacing"})
        if (effective.opt_float(key) < IRONING_SPACING_MIN) conflict(key);
    for (const char* key : {"xy_hole_compensation", "xy_contour_compensation"})
        if (std::abs(effective.opt_float(key)) > 2.) conflict(key);
    if (effective.opt_float("elefant_foot_compensation") > 1.) conflict("elefant_foot_compensation");
    if (effective.opt_bool("spiral_mode") && (effective.opt_bool("detect_thin_wall") ||
        effective.opt_bool("overhang_reverse") || effective.opt_enum<TimelapseType>("timelapse_type") != tlTraditional ||
        effective.opt_bool("enable_wrapping_detection"))) conflict("spiral_mode");
    if (effective.opt_bool("alternate_extra_wall") &&
        effective.opt_enum<EnsureVerticalShellThickness>("ensure_vertical_shell_thickness") == evstAll)
        conflict("alternate_extra_wall");
    if (effective.opt_bool("enable_support")) {
        if (!effective.opt_bool("detect_overhang_wall")) conflict("detect_overhang_wall");
        const auto style = effective.opt_enum<SupportMaterialStyle>("support_style");
        const bool tree = is_tree(effective.opt_enum<SupportType>("support_type"));
        if (tree ? !(style == smsDefault || style == smsTreeSlim || style == smsTreeStrong || style == smsTreeHybrid || style == smsTreeOrganic) :
                   !(style == smsDefault || style == smsGrid || style == smsSnug)) conflict("support_style");
    }
    if (effective.opt_enum<SeamScarfType>("seam_slope_type") != SeamScarfType::None &&
        effective.get_abs_value("seam_slope_start_height") >= effective.opt_float("layer_height"))
        conflict("seam_slope_start_height");
    if (effective.opt_float("infill_lock_depth") > effective.opt_float("skin_infill_depth")) conflict("infill_lock_depth");
    if (effective.opt_enum<FuzzySkinMode>("fuzzy_skin_mode") != FuzzySkinMode::Displacement &&
        effective.opt_enum<PerimeterGeneratorType>("wall_generator") != PerimeterGeneratorType::Arachne)
        conflict("fuzzy_skin_mode");
    if (effective.opt_bool("belt_printer")) {
        if (effective.opt_int("raft_layers") > 0) conflict("raft_layers");
        if (effective.opt_enum<DraftShield>("draft_shield") != dsDisabled) conflict("draft_shield");
    } else if (effective.opt_enum<BrimType>("brim_type") == btLeadingEdgeOnly) conflict("brim_type");
    for (const char* key : {"layer_height", "initial_layer_print_height"}) {
        if (!changes.has(key)) continue;
        const double height = effective.opt_float(key);
        const auto* nozzles = effective.option<ConfigOptionFloats>("nozzle_diameter");
        const auto* minimum = effective.option<ConfigOptionFloats>("min_layer_height");
        const auto* maximum = effective.option<ConfigOptionFloats>("max_layer_height");
        if (!nozzles || nozzles->values.empty()) {
            result.errors[key] = L("Choose a nozzle for this plate first.");
            continue;
        }
        for (size_t i = 0; i < nozzles->values.size(); ++i) {
            const double nozzle = nozzles->values[i];
            const double low = minimum && !minimum->values.empty() ? minimum->get_at(i) : 0.;
            const double high = maximum && !maximum->values.empty() && maximum->get_at(i) > 0. ?
                maximum->get_at(i) : nozzle * 0.75;
            if (!std::isfinite(height) || nozzle <= 0. || height < low - 1e-6 || height > high + 1e-6) {
                result.errors[key] = L("The layer height is outside this printer's nozzle limits.");
                break;
            }
        }
    }
    for (const auto& key : changes.keys()) {
        const auto* value = changes.option(key);
        if (value->type() == coEnum) {
            const auto* def = print_config_def.get(key);
            if (!def->enum_keys_map || std::none_of(def->enum_keys_map->begin(), def->enum_keys_map->end(),
                [&](const auto& entry) { return entry.second == value->getInt(); }))
                result.errors[key] = L("Choose a supported value for this setting.");
        }
        if ((value->type() == coFloat || value->type() == coPercent || value->type() == coFloatOrPercent) &&
            !std::isfinite(value->getFloat()))
            result.errors[key] = L("Enter a finite number.");
        result.changed = result.changed || *config.option(key) != *result.config.option(key) ||
            (overrides.has(key) && *overrides.option(key) != *result.overrides.option(key));
    }
    return result;
}

PlateProcessEdit resolve_plate_process(const PlateProcessCascade& plate, const ProcessSettingsScopes& scopes)
{
    const auto values = resolved_process_settings(plate.setup, scopes, plate.stage);
    DynamicPrintConfig changes;
    for (const auto& key : values.keys()) {
        const auto* current = plate.options.option(key);
        if (!current) current = plate.setup.config.option(key);
        if (!current || *current != *values.option(key)) changes.set_key_value(key, values.option(key)->clone());
    }
    auto result = prepare_plate_process_edit(plate.setup.config, plate.options, changes);
    // A plate keeping its values is not checked; an older conflict is not this edit's.
    if (changes.empty()) result.errors.clear();
    return result;
}

ProcessCascadeEdit resolve_process_cascade(const std::vector<PlateProcessCascade>& plates,
                                           const ProcessSettingsScopes& scopes)
{
    ProcessCascadeEdit result;
    for (const auto& plate : plates) {
        result.plates.push_back(resolve_plate_process(plate, scopes));
        if (!result.plates.back().errors.empty()) {
            result.failed = int(result.plates.size()) - 1;
            break;
        }
    }
    return result;
}

ProcessScopeReach process_scope_reach(const std::vector<PlateProcessCascade>& plates,
                                      const ProcessSettingsScopes& scopes, const std::string& stage,
                                      const std::string& key)
{
    ProcessScopeReach result;
    for (const auto& plate : plates) {
        if (!stage.empty() && plate.stage != stage) continue;
        const auto lower = scopes.stages.find(plate.stage);
        const bool own = plate.setup.process_overrides.has(key) ||
            (stage.empty() && lower != scopes.stages.end() && lower->second.has(key));
        if (own) ++result.overridden;
        else ++result.inheriting;
    }
    return result;
}

ProcessScopeView process_scope_view(const std::vector<PlateProcessCascade>& plates,
                                    const ProcessSettingsScopes& scopes, const std::string& stage)
{
    // The Project editor shows project defaults, not any stage's; its baseline is
    // each plate's preset. A Stage editor's baseline includes the project defaults.
    auto shown = scopes;
    auto parent = scopes;
    if (stage.empty()) {
        shown.stages.clear();
        parent = {};
    } else
        parent.stages.erase(stage);
    // Only the first plate's complete settings are shown; the others are compared.
    struct Member
    {
        const DynamicPrintConfig* config;
        DynamicPrintConfig values;
        DynamicPrintConfig inherited;
    };
    std::vector<Member> members;
    for (const auto& plate : plates)
        if (stage.empty() || plate.stage == stage)
            members.push_back({&plate.setup.config, inherited_process_settings(plate.setup, shown, stage),
                               inherited_process_settings(plate.setup, parent, stage)});
    ProcessScopeView result;
    if (members.empty()) return result;
    auto mixed = [&members](bool baseline) {
        // A setting outside a plate's cascade keeps the plate's configured value.
        auto option = [baseline](const Member& member, const std::string& key) {
            const auto* value = (baseline ? member.inherited : member.values).option(key);
            return value ? value : member.config->option(key);
        };
        std::vector<std::string> keys;
        for (const auto& key : plate_process_edit_keys()) {
            const auto* first = option(members.front(), key);
            if (first && std::any_of(members.begin(), members.end(), [&](const Member& member) {
                const auto* value = option(member, key);
                return !value || *value != *first;
            })) keys.push_back(key);
        }
        return keys;
    };
    result.mixed = mixed(false);
    result.mixed_inherited = mixed(true);
    result.values = *members.front().config;
    result.values.apply(members.front().values);
    result.inherited = *members.front().config;
    result.inherited.apply(members.front().inherited);
    return result;
}

void record_plate_process_edits(PlatePrintSetup& setup, const DynamicPrintConfig& edited,
                                const ProcessSettingsScopes& scopes, const std::string& stage)
{
    const auto parent = inherited_process_settings(setup, scopes, stage);
    for (const auto& key : plate_process_edit_keys()) {
        const auto* next = edited.option(key);
        const auto* old = setup.config.option(key);
        if (!next || !old || *next == *old) continue;
        if (parent.has(key) && *next == *parent.option(key)) setup.process_overrides.erase(key);
        else setup.process_overrides.set_key_value(key, next->clone());
    }
}

void select_plate_process_preset(PlatePrintSetup& setup, const DynamicPrintConfig& preset)
{
    for (const auto& key : plate_process_edit_keys())
        if (const auto* value = preset.option(key)) setup.process_base.set_key_value(key, value->clone());
}

std::string PlatePrintSetup::serialize() const
{
    // GUI import/capture migrates legacy destinations through the local ID map.
    // Reject unmigrated callers too, rather than publishing a connection address.
    if (address_based_printer_destination(destination))
        throw RuntimeError("This printer's project destination must be updated before saving.");
    nlohmann::json values = nlohmann::json::object();
    nlohmann::json exact = nlohmann::json::object();
    for (const auto& key : config.keys()) {
        // Saving must be safe even when a caller supplied a config directly.
        if (local_connection_setting(key)) continue;
        const ConfigOption* option = config.option(key);
        values[key] = option->serialize();
        // Orca's display serialization rounds floats, and some generic enums have
        // no name table. Retain numeric values as well as the readable config.
        switch (option->type()) {
        case coEnum: exact[key] = option->getInt(); break;
        case coEnums: exact[key] = static_cast<const ConfigOptionEnumsGeneric*>(option)->values; break;
        case coFloat:
        case coPercent:
        case coFloatOrPercent: exact[key] = option->getFloat(); break;
        case coFloats:
        case coPercents: exact[key] = static_cast<const ConfigOptionVector<double>*>(option)->values; break;
        case coFloatsOrPercents: {
            exact[key] = nlohmann::json::array();
            for (const auto& value : static_cast<const ConfigOptionVector<FloatOrPercent>*>(option)->values)
                exact[key].push_back(value.value);
            break;
        }
        case coPoints: {
            exact[key] = nlohmann::json::array();
            for (const auto& point : static_cast<const ConfigOptionPoints*>(option)->values)
                exact[key].push_back({point.x(), point.y()});
            break;
        }
        default: break;
        }
    }
    // Version 1 readers ignore every process field, so any of them requires version 2.
    const bool cascade = !process_base.empty() || !process_overrides.empty() || !process_scope_catalog.empty();
    return nlohmann::json{{"version", cascade ? 2 : 1},
                          {"process_base", process_config_json(process_base)}, {"process_overrides", process_config_json(process_overrides)},
                          {"process_scope_catalog", process_scope_catalog}, {"config", std::move(values)}, {"exact", std::move(exact)},
                          {"destination", destination}, {"destination_name", destination_name}}.dump();
}

PlatePrintSetup PlatePrintSetup::deserialize(const std::string& value)
{
    const auto data = nlohmann::json::parse(value);
    if ((data.at("version") != 1 && data.at("version") != 2) || !data.at("config").is_object())
        throw RuntimeError(_u8L("This plate's printing setup requires a newer version of Porca."));
    PlatePrintSetup setup;
    const auto exact = data.value("exact", nlohmann::json::object());
    if (!exact.is_object()) throw RuntimeError("Cannot restore this plate's printing setup.");
    for (const auto& item : data.at("config").items()) {
        // Project files describe print intent; connection details come from this computer.
        if (local_connection_setting(item.key())) continue;
        ConfigOption* option = setup.config.option(item.key(), true);
        if (!option) unrestorable_plate_setting(item.key());
        const bool numeric_enum = exact.contains(item.key()) && (option->type() == coEnum || option->type() == coEnums);
        if (!numeric_enum && !option->deserialize(item.value().get<std::string>()))
            unrestorable_plate_setting(item.key());
        if (!exact.contains(item.key())) continue;
        const auto& value = exact.at(item.key());
        switch (option->type()) {
        case coEnum: option->setInt(value.get<int>()); break;
        case coEnums: static_cast<ConfigOptionEnumsGeneric*>(option)->values = value.get<std::vector<int>>(); break;
        case coFloat:
        case coPercent:
        case coFloatOrPercent: static_cast<ConfigOptionFloat*>(option)->value = value.get<double>(); break;
        case coFloats:
        case coPercents: {
            // Per-extruder and per-filament vectors keep their readable length. An empty
            // vector is the exception: its readable form is "", which Orca reads as one zero.
            auto& values = static_cast<ConfigOptionVector<double>*>(option)->values;
            if (!value.is_array()) unrestorable_plate_setting(item.key());
            if (value.empty() && item.value().get<std::string>().empty()) values.clear();
            if (values.size() != value.size()) unrestorable_plate_setting(item.key());
            for (size_t i = 0; i < values.size(); ++i) {
                if (!value[i].is_null() && !value[i].is_number()) unrestorable_plate_setting(item.key());
                values[i] = value[i].is_null() ? std::numeric_limits<double>::quiet_NaN() : value[i].get<double>();
            }
            break;
        }
        case coFloatsOrPercents: {
            auto& values = static_cast<ConfigOptionVector<FloatOrPercent>*>(option)->values;
            if (values.size() != value.size()) unrestorable_plate_setting(item.key());
            for (size_t i = 0; i < values.size(); ++i)
                values[i].value = value[i].is_null() ? std::numeric_limits<double>::quiet_NaN() : value[i].get<double>();
            break;
        }
        case coPoints: {
            auto& points = static_cast<ConfigOptionPoints*>(option)->values;
            points.clear();
            for (const auto& point : value) points.emplace_back(point.at(0).get<double>(), point.at(1).get<double>());
            break;
        }
        default: break;
        }
    }
    if (data.at("version") == 2) {
        setup.process_base = process_config_from_json(data.at("process_base"));
        setup.process_overrides = process_config_from_json(data.at("process_overrides"));
        setup.process_scope_catalog = data.value("process_scope_catalog", std::string());
        ProcessSettingsScopes::deserialize(setup.process_scope_catalog);
    }
    setup.destination = data.value("destination", std::string());
    setup.destination_name = data.value("destination_name", std::string());
    // Version 1 inherits Orca's implicit FFF technology when the field is absent.
    for (const char* key : {"printable_area", "printable_height", "nozzle_diameter",
                            "layer_height", "filament_diameter", "filament_type", "filament_colour"})
        if (!setup.config.has(key)) unrestorable_plate_setting(key);
    const auto& area = setup.config.option<ConfigOptionPoints>("printable_area")->values;
    const auto& nozzles = setup.config.option<ConfigOptionFloats>("nozzle_diameter")->values;
    const double height = setup.config.opt_float("printable_height");
    if (Preset::printer_technology(setup.config) != ptFFF || area.size() < 3 ||
        std::any_of(area.begin(), area.end(), [](const Vec2d& point) { return !point.allFinite(); }) ||
        !std::isfinite(height) || height <= 0 || nozzles.empty() ||
        std::any_of(nozzles.begin(), nozzles.end(), [](double nozzle) { return !std::isfinite(nozzle) || nozzle <= 0; }))
        throw RuntimeError("This plate's printing setup is not supported.");
    const auto bed_size = BoundingBoxf(area).size();
    if (bed_size.x() <= 0 || bed_size.y() <= 0 ||
        bed_size.x() > std::numeric_limits<int>::max() || bed_size.y() > std::numeric_limits<int>::max())
        throw RuntimeError("Cannot restore this plate's printable area.");
    return setup;
}

namespace {
bool valid_workspace_frame(const BoundingBoxf& bounds)
{
    return bounds.defined && bounds.min.allFinite() && bounds.max.allFinite() &&
        bounds.min.cwiseAbs().maxCoeff() <= 100000.0 && bounds.max.cwiseAbs().maxCoeff() <= 100000.0 &&
        bounds.size().minCoeff() >= 200.0;
}

bool valid_workspace_stage(const WorkspaceStage& stage)
{
    return !stage.id.empty() && stage.id.size() <= 64 &&
        std::all_of(stage.id.begin(), stage.id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-';
        }) && !stage.name.empty() && stage.name.size() <= 1024 &&
        stage.name.find_first_not_of(" \t\r\n") != std::string::npos &&
        stage.name.find('\0') == std::string::npos && stage.position.allFinite() &&
        stage.position.cwiseAbs().maxCoeff() <= 100000.0 &&
        (!stage.frame.defined || (valid_workspace_frame(stage.frame) &&
            valid_workspace_frame(BoundingBoxf(stage.position + stage.frame.min, stage.position + stage.frame.max))));
}
}

std::string serialize_workspace_stages(const std::vector<WorkspaceStage>& stages)
{
    if (stages.size() > 256) throw RuntimeError("Cannot save this project's stages.");
    auto entries = nlohmann::json::array();
    std::set<std::string> ids;
    bool has_frames = false;
    for (const auto& stage : stages) {
        if (!valid_workspace_stage(stage) || !ids.insert(stage.id).second)
            throw RuntimeError("Cannot save this project's stages.");
        entries.push_back({{"id", stage.id}, {"name", stage.name},
                           {"position", {stage.position.x(), stage.position.y()}}});
        if (stage.frame.defined) {
            entries.back()["frame"] = {{stage.frame.min.x(), stage.frame.min.y()},
                                        {stage.frame.max.x(), stage.frame.max.y()}};
            has_frames = true;
        }
    }
    return nlohmann::json{{"version", has_frames ? 2 : 1}, {"stages", std::move(entries)}}.dump();
}

std::vector<WorkspaceStage> deserialize_workspace_stages(const std::string& value)
{
    const auto data = nlohmann::json::parse(value);
    if (!data.is_object() || !data.at("version").is_number_integer() ||
        (data.at("version") != 1 && data.at("version") != 2))
        throw RuntimeError("This project's stages require a newer version of Porca.");
    const auto& entries = data.at("stages");
    if (!entries.is_array() || entries.size() > 256)
        throw RuntimeError("Cannot restore this project's stages.");
    std::vector<WorkspaceStage> stages;
    std::set<std::string> ids;
    for (const auto& entry : entries) {
        const auto& position = entry.at("position");
        if (!position.is_array() || position.size() != 2)
            throw RuntimeError("Cannot restore this project's stages.");
        WorkspaceStage stage{entry.at("id").get<std::string>(), entry.at("name").get<std::string>(),
                             Vec2d(position.at(0).get<double>(), position.at(1).get<double>())};
        if (entry.contains("frame")) {
            const auto& frame = entry.at("frame");
            if (data.at("version") != 2 || !frame.is_array() || frame.size() != 2 ||
                !frame[0].is_array() || frame[0].size() != 2 || !frame[1].is_array() || frame[1].size() != 2)
                throw RuntimeError("Cannot restore this project's stage area.");
            stage.frame = BoundingBoxf(Vec2d(frame[0][0].get<double>(), frame[0][1].get<double>()),
                                      Vec2d(frame[1][0].get<double>(), frame[1][1].get<double>()));
            if (!valid_workspace_frame(stage.frame))
                throw RuntimeError("Cannot restore this project's stage area.");
        }
        if (!valid_workspace_stage(stage) || !ids.insert(stage.id).second)
            throw RuntimeError("Cannot restore this project's stages.");
        stages.push_back(std::move(stage));
    }
    return stages;
}

BoundingBoxf workspace_stage_minimum_bounds(const WorkspaceStage& stage,
                                            const std::vector<WorkspacePlateBounds>& plates)
{
    BoundingBoxf bounds;
    for (const auto& plate : plates)
        if (plate.stage_id == stage.id) bounds.merge(plate.bounds);
    if (!bounds.defined) return BoundingBoxf(stage.position, stage.position + Vec2d(200, 200));
    bounds = BoundingBoxf(bounds.min - Vec2d::Constant(workspace_stage_margin),
                          bounds.max + Vec2d(workspace_controls_clearance, workspace_title_clearance));
    const Vec2d extra = (Vec2d(200, 200) - bounds.size()).cwiseMax(0.0) * 0.5;
    return BoundingBoxf(bounds.min - extra, bounds.max + extra);
}

BoundingBoxf workspace_stage_bounds(const WorkspaceStage& stage,
                                    const std::vector<WorkspacePlateBounds>& plates)
{
    const bool has_members = std::any_of(plates.begin(), plates.end(), [&](const WorkspacePlateBounds& plate) {
        return plate.stage_id == stage.id;
    });
    if (!has_members)
        return stage.frame.defined ? BoundingBoxf(stage.position + stage.frame.min, stage.position + stage.frame.max) :
                                    BoundingBoxf(stage.position, stage.position + Vec2d(400, 300));
    auto bounds = workspace_stage_minimum_bounds(stage, plates);
    if (stage.frame.defined) bounds.merge(BoundingBoxf(stage.position + stage.frame.min, stage.position + stage.frame.max));
    return bounds;
}

bool can_resize_workspace_stage(const WorkspaceStage& stage,
                                const std::vector<WorkspacePlateBounds>& plates,
                                const BoundingBoxf& bounds)
{
    if (!valid_workspace_stage(stage) || !valid_workspace_frame(bounds) ||
        !valid_workspace_frame(BoundingBoxf(bounds.min - stage.position, bounds.max - stage.position)))
        return false;
    const bool has_members = std::any_of(plates.begin(), plates.end(), [&](const WorkspacePlateBounds& plate) {
        return plate.stage_id == stage.id;
    });
    return !has_members || bounds.contains(workspace_stage_minimum_bounds(stage, plates));
}

bool can_move_workspace_stage(const std::vector<WorkspacePlateBounds>& plates,
                              const std::string& stage_id, const Vec2d& delta)
{
    if (stage_id.empty() || !delta.allFinite() || delta.squaredNorm() < 1e-12) return false;
    for (const auto& plate : plates) {
        if (plate.stage_id != stage_id) continue;
        const Vec2d origin = plate.origin + delta;
        if (plate.locked || !origin.allFinite() || origin.cwiseAbs().maxCoeff() > 100000.0 ||
            !plate.bounds.defined || !plate.bounds.min.allFinite() || !plate.bounds.max.allFinite()) return false;
        const Vec2d min = plate.bounds.min + delta;
        const Vec2d max = plate.bounds.max + delta;
        for (const auto& other : plates) {
            if (other.stage_id == stage_id) continue;
            if (!other.bounds.defined || !other.bounds.min.allFinite() || !other.bounds.max.allFinite()) return false;
            if (min.x() < other.bounds.max.x() + 5.0 && max.x() > other.bounds.min.x() - 5.0 &&
                min.y() < other.bounds.max.y() + 5.0 && max.y() > other.bounds.min.y() - 5.0) return false;
        }
    }
    return true;
}

bool can_change_workspace_stage_area(const BoundingBoxf& current, const BoundingBoxf& proposed,
                                     const std::vector<BoundingBoxf>& others)
{
    const auto valid = [](const BoundingBoxf& bounds) {
        return bounds.defined && bounds.min.allFinite() && bounds.max.allFinite() &&
            bounds.size().allFinite() && bounds.size().minCoeff() > 0.;
    };
    if (!valid(current) || !valid(proposed)) return false;
    const auto penetration = [](const BoundingBoxf& area, const BoundingBoxf& neighbor) {
        // Minimum translation needed to separate the rectangles. Unlike their
        // intersection area, this decreases when leaving a containing stage.
        return std::max(0., std::min({area.max.x() - neighbor.min.x(), neighbor.max.x() - area.min.x(),
                                     area.max.y() - neighbor.min.y(), neighbor.max.y() - area.min.y()}));
    };
    for (const auto& other : others) {
        if (!valid(other)) return false;
        const BoundingBoxf clearance(other.min - Vec2d::Constant(workspace_stage_gap),
                                     other.max + Vec2d::Constant(workspace_stage_gap));
        if (!valid(clearance)) return false;
        const double before = penetration(current, clearance);
        const double after = penetration(proposed, clearance);
        if (after > 0. && (before <= 0. || after >= before)) return false;
    }
    return true;
}

} // namespace Slic3r
