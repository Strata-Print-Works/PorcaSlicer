#include "PluginHostBindings.hpp"
#include "PluginHostWorkspace.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include <nlohmann/json.hpp>
#include <libslic3r/LocalesUtils.hpp>
#include <libslic3r/Config.hpp>
#include <exception>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include "pybind11/gil.h"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/PluginUiThread.hpp"

#include <cstddef>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/Tab.hpp>

#include <memory>
#include <pybind11/stl.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

py::list current_filament_presets(PresetBundle& bundle)
{
    py::list presets;
    for (const std::string& preset_name : bundle.filament_presets) {
        Preset* preset = bundle.filaments.find_preset(preset_name);
        if (preset == nullptr)
            presets.append(py::none());
        else
            presets.append(py::cast(preset, py::return_value_policy::reference));
    }
    return presets;
}

PresetCollection& printer_presets(PresetBundle& bundle)
{
    return static_cast<PresetCollection&>(bundle.printers);
}

// --------------------------------------------------------------------------
// Live setting edits.
//
// Values are written into the *edited* preset held by the matching settings tab
// and the UI is reloaded from it, which is the same route MainFrame::load_config
// takes for a config loaded from a file. So a change lands the way a change typed
// into the settings tab does: fields update, the preset shows as modified, and
// the plate re-slices. Nothing is written to disk until save() is called.
// --------------------------------------------------------------------------

// wx is not thread-safe and plugins run on their own thread, so every function
// below does its work inside run_on_ui_blocking.
GUI::Tab* tab_for_type(Preset::Type type)
{
    GUI::Tab* tab = GUI::wxGetApp().get_tab(type);
    if (tab == nullptr || tab->get_config() == nullptr)
        throw std::invalid_argument("no settings tab for that preset type; is the GUI up?");
    return tab;
}

// Settings are addressed by key alone, as they are in the preset files and in the
// slicer's own UI — the caller should not have to know that `layer_height` is a
// process setting and `nozzle_diameter` a printer one.
GUI::Tab* tab_owning(const std::string& key)
{
    for (GUI::Tab* tab : GUI::wxGetApp().tabs_list)
        if (tab != nullptr && tab->get_config() != nullptr && tab->get_config()->has(key))
            return tab;
    return nullptr;
}

// Config values are strings everywhere in Orca (preset JSON, .ini, the option
// serializers), so that is the contract here too. Python numbers and bools are
// accepted as a convenience and rendered the way the serializers write them;
// sequences become the comma-separated form used for per-extruder settings.
std::string to_config_string(const py::handle& value)
{
    if (py::isinstance<py::str>(value))
        return value.cast<std::string>();
    if (py::isinstance<py::bool_>(value))
        return value.cast<bool>() ? "1" : "0";
    if (py::isinstance<py::int_>(value))
        return std::to_string(value.cast<long long>());
    // The slicer's own formatter: a plain stream would emit a locale's decimal
    // comma, which the option parsers reject.
    if (py::isinstance<py::float_>(value))
        return float_to_string_decimal_point(value.cast<double>());
    if (py::isinstance<py::sequence>(value) && !py::isinstance<py::bytes>(value)) {
        std::vector<std::string> parts;
        for (const py::handle& item : value)
            parts.push_back(to_config_string(item));
        return boost::algorithm::join(parts, ",");
    }
    throw py::type_error("setting values must be a string, number, bool, or a sequence of those");
}

bool is_numeric_option(ConfigOptionType type)
{
    switch (type) {
    case coFloat: case coFloats: case coInt: case coInts:
    case coPercent: case coPercents: case coFloatOrPercent: case coFloatsOrPercents: return true;
    default: return false;
    }
}

bool allows_percent(ConfigOptionType type)
{
    switch (type) {
    case coPercent: case coPercents: case coFloatOrPercent: case coFloatsOrPercents: return true;
    default: return false;
    }
}

std::string range_text(const ConfigOptionDef& def)
{
    const bool has_min = def.min > -FLT_MAX;
    const bool has_max = def.max < FLT_MAX;
    if (has_min && has_max)
        return float_to_string_decimal_point(def.min) + " to " + float_to_string_decimal_point(def.max);
    if (has_min)
        return "at least " + float_to_string_decimal_point(def.min);
    return "at most " + float_to_string_decimal_point(def.max);
}

// The option parsers are forgiving in a way a caller must not rely on:
// ConfigOptionFloat::deserialize turns "fast" into 0 without complaint, and the
// declared <min,max> is enforced by the input fields rather than by the parser.
// Left alone, a typo would silently zero a setting.
void check_number(const std::string& key, const ConfigOptionDef& def, const std::string& text)
{
    if (text.empty() || (def.nullable && text == "nil"))
        return;

    std::string number  = text;
    const bool  percent = allows_percent(def.type) && number.back() == '%';
    if (percent)
        number.pop_back();

    const char*  begin = number.c_str();
    char*        end   = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || *end != '\0' || !std::isfinite(parsed))
        throw std::invalid_argument(key + ": \"" + text + "\" is not a number");

    // A percentage is relative to another setting, so the absolute range does not apply to it.
    if (percent)
        return;
    if (parsed < double(def.min) || parsed > double(def.max))
        throw std::invalid_argument(key + ": " + text + " is out of range (" + range_text(def) + ")");
}

// Returns the string to deserialize, which is not always the one passed in.
std::string validated_value(const std::string&     key,
                            const ConfigOptionDef& def,
                            const ConfigOption*    current,
                            const std::string&     value)
{
    // Only numeric options are split on commas: a string option's own value may
    // contain them, and its serializer is the authority on quoting.
    if (!is_numeric_option(def.type))
        return value;

    std::vector<std::string> parts;
    boost::split(parts, value, boost::is_any_of(","));
    for (std::string& part : parts) {
        boost::trim(part);
        check_number(key, def, part);
    }

    const auto* vector = current == nullptr ? nullptr : dynamic_cast<const ConfigOptionVectorBase*>(current);
    if (def.is_scalar() || vector == nullptr)
        return boost::algorithm::join(parts, ",");

    // Per-extruder settings hold one value per extruder. A single number means
    // "all of them": writing it through unchanged would instead shorten the vector
    // and silently drop the other extruders' values.
    const size_t have = vector->size();
    if (parts.size() == 1 && have > 1)
        return boost::algorithm::join(std::vector<std::string>(have, parts.front()), ",");
    if (have > 0 && parts.size() != have)
        throw std::invalid_argument(key + " is a per-extruder setting with " + std::to_string(have) +
                                    " values; give one value for all of them, or " + std::to_string(have) +
                                    " values");
    return boost::algorithm::join(parts, ",");
}

// Applies every value or none of them: a half-applied batch would leave the
// slicer in a state the caller never asked for and cannot easily describe.
py::dict set_values(const py::dict& values, const std::string& expected_context = {})
{
    // Converted with the GIL held, before anything is marshaled to the UI thread.
    std::map<std::string, std::string> wanted;
    for (const auto& item : values)
        wanted.emplace(item.first.cast<std::string>(), to_config_string(item.second));

    auto applied = run_on_ui_blocking([&wanted, &expected_context]() -> std::map<std::string, std::pair<std::string, std::string>> {
        if (!expected_context.empty() && plugin_workspace_token() != expected_context)
            throw std::runtime_error("The plate or its settings changed. Ask Copilot for an updated suggestion.");
        auto* active_plate = GUI::wxGetApp().plater()->get_partplate_list().get_curr_plate();
        for (const auto& [key, value] : wanted)
            if (!expected_context.empty() && active_plate->config()->has(key))
                throw std::invalid_argument("This setting has a plate override. Edit it in the plate settings: " + key);
        std::map<GUI::Tab*, std::vector<std::string>> by_tab;
        std::vector<std::string>                      unknown;
        for (const auto& [key, value] : wanted) {
            if (GUI::Tab* tab = tab_owning(key))
                by_tab[tab].push_back(key);
            else
                unknown.push_back(key);
        }
        if (!unknown.empty())
            throw std::invalid_argument("unknown settings: " + boost::algorithm::join(unknown, ", "));

        // Validate and normalize against a copy first, so a value the option will
        // not accept is reported before any of the batch has touched the live config.
        std::map<std::string, std::string> ready;
        for (const auto& [tab, keys] : by_tab) {
            DynamicPrintConfig  probe  = *tab->get_config();
            const ConfigDef*    config_def = tab->get_config()->def();
            for (const std::string& key : keys) {
                const ConfigOptionDef* def = config_def == nullptr ? nullptr : config_def->get(key);
                if (def == nullptr)
                    throw std::invalid_argument("no definition for setting " + key);
                const std::string value = validated_value(key, *def, tab->get_config()->option(key),
                                                          wanted.at(key));
                try {
                    probe.set_deserialize_strict(key, value);
                } catch (const std::exception& e) {
                    throw std::invalid_argument("cannot set " + key + " to \"" + value + "\": " + e.what());
                }
                ready.emplace(key, value);
            }
        }

        std::map<std::string, std::pair<std::string, std::string>> changed;
        for (const auto& [tab, keys] : by_tab) {
            DynamicPrintConfig* config = tab->get_config();
            for (const std::string& key : keys) {
                const std::string before = config->opt_serialize(key);
                config->set_deserialize_strict(key, ready.at(key));
                const std::string after = config->opt_serialize(key);
                if (before != after)
                    changed.emplace(key, std::make_pair(before, after));
            }
        }

        // Pushes the edited presets back into the tabs, re-runs the compatibility
        // and dirty-state bookkeeping, and invalidates the slice.
        if (!changed.empty())
            GUI::wxGetApp().load_current_presets();
        return changed;
    });

    py::dict out;
    for (const auto& [key, values] : applied)
        out[py::str(key)] = py::make_tuple(values.first, values.second);
    return out;
}

py::object edited_value(const std::string& key)
{
    return run_on_ui_blocking([&key]() -> py::object {
        GUI::Tab* tab = tab_owning(key);
        if (tab == nullptr)
            return py::none();
        // The GIL is released around this call; take it back to build the result.
        py::gil_scoped_acquire gil;
        return py::cast(tab->get_config()->opt_serialize(key));
    });
}

std::vector<std::string> dirty_keys(Preset::Type type)
{
    return run_on_ui_blocking([type]() { return tab_for_type(type)->get_presets()->current_dirty_options(); });
}

void save_preset(Preset::Type type, const std::string& name)
{
    run_on_ui_blocking([type, &name]() {
        GUI::Tab*     tab    = tab_for_type(type);
        // An empty name makes Tab::save_preset open its Save dialog, which a plugin
        // call must never do — resolve it to the edited preset instead.
        std::string   target = name;
        if (target.empty()) {
            const Preset& edited = tab->get_presets()->get_edited_preset();
            if (!edited.is_user())
                throw std::invalid_argument("\"" + edited.name +
                                            "\" is a system preset and cannot be overwritten; "
                                            "pass a name to save a copy");
            target = edited.name;
        }
        tab->save_preset(target);
    });
}

void select_preset(Preset::Type type, const std::string& name)
{
    run_on_ui_blocking([type, &name]() {
        GUI::Tab* tab = tab_for_type(type);
        if (tab->get_presets()->find_preset(name) == nullptr)
            throw std::invalid_argument("no preset named \"" + name + "\"");
        // With unsaved edits, Tab::select_preset asks the user what to do with them.
        // That dialog runs a nested event loop on this thread, which never returns to
        // the plugin waiting on us — so refuse instead of hanging.
        if (tab->get_presets()->current_is_dirty())
            throw std::invalid_argument("\"" + tab->get_presets()->get_edited_preset().name +
                                        "\" has unsaved changes; save or discard them before selecting "
                                        "another preset");
        tab->select_preset(name);
    });
}

void discard_changes(Preset::Type type)
{
    run_on_ui_blocking([type]() {
        GUI::Tab* tab = tab_for_type(type);
        // Deliberately not Tab::select_preset(current): that route prompts about the
        // changes it is being asked to drop, and the dialog would deadlock the caller.
        tab->get_presets()->discard_current_changes();
        GUI::wxGetApp().load_current_presets();
    });
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    py::enum_<Preset::Type>(host, "PresetType")
        .value("Invalid", Preset::TYPE_INVALID)
        .value("Print", Preset::TYPE_PRINT)
        .value("SlaPrint", Preset::TYPE_SLA_PRINT)
        .value("Filament", Preset::TYPE_FILAMENT)
        .value("SlaMaterial", Preset::TYPE_SLA_MATERIAL)
        .value("Printer", Preset::TYPE_PRINTER)
        .value("PhysicalPrinter", Preset::TYPE_PHYSICAL_PRINTER)
        .value("Plate", Preset::TYPE_PLATE)
        .value("Model", Preset::TYPE_MODEL);

    py::class_<Preset, std::unique_ptr<Preset, py::nodelete>>(host, "Preset")
        .def_readonly("type", &Preset::type)
        .def_readonly("name", &Preset::name)
        .def_readonly("alias", &Preset::alias)
        .def_readonly("file", &Preset::file)
        .def_readonly("is_default", &Preset::is_default)
        .def_readonly("is_external", &Preset::is_external)
        .def_readonly("is_system", &Preset::is_system)
        .def_readonly("is_visible", &Preset::is_visible)
        .def_readonly("is_dirty", &Preset::is_dirty)
        .def_readonly("is_compatible", &Preset::is_compatible)
        .def_readonly("is_project_embedded", &Preset::is_project_embedded)
        .def_readonly("bundle_id", &Preset::bundle_id)
        .def("is_user", &Preset::is_user)
        .def("is_from_bundle", &Preset::is_from_bundle)
        .def("label", &Preset::label, py::arg("no_alias") = false)
        .def("config_keys", [](const Preset& preset) { return preset.config.keys(); })
        .def("config_value", [](const Preset& preset, const std::string& key) {
            return config_value_or_none(preset.config, key);
        });

    py::class_<PresetCollection, std::unique_ptr<PresetCollection, py::nodelete>>(host, "PresetCollection")
        .def("size", &PresetCollection::size)
        .def("get_selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("selected_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_selected_preset();
        }, py::return_value_policy::reference_internal)
        .def("get_selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("selected_preset_name", &PresetCollection::get_selected_preset_name)
        .def("get_edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("edited_preset", [](PresetCollection& collection) -> Preset& {
            return collection.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("preset", [](PresetCollection& collection, size_t index) -> Preset& {
            if (index >= collection.size())
                throw py::index_error("preset index out of range");
            return collection.preset(index);
        }, py::return_value_policy::reference_internal)
        .def("find_preset", [](PresetCollection& collection, const std::string& name) -> Preset* {
            return collection.find_preset(name);
        }, py::return_value_policy::reference_internal)
        .def("preset_names", [](const PresetCollection& collection) {
            std::vector<std::string> names;
            names.reserve(collection.get_presets().size());
            for (const Preset& preset : collection.get_presets())
                names.push_back(preset.name);
            return names;
        });

    py::class_<PresetBundle, std::unique_ptr<PresetBundle, py::nodelete>>(host, "PresetBundle")
        .def_property_readonly("prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("printers", &printer_presets, py::return_value_policy::reference_internal)
        .def_property_readonly("filaments", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.filaments;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_prints", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_prints;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("sla_materials", [](PresetBundle& bundle) -> PresetCollection& {
            return bundle.sla_materials;
        }, py::return_value_policy::reference_internal)
        .def("current_process_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_print_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.prints.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_printer_preset", [](PresetBundle& bundle) -> Preset& {
            return bundle.printers.get_edited_preset();
        }, py::return_value_policy::reference_internal)
        .def("current_filament_preset_names", [](PresetBundle& bundle) {
            return bundle.filament_presets;
        })
        .def("current_filament_presets", &current_filament_presets)
        .def("full_config_keys", [](const PresetBundle& bundle) {
            return bundle.full_config().keys();
        })
        .def("full_config_value", [](const PresetBundle& bundle, const std::string& key) {
            return config_value_or_none(bundle.full_config(), key);
        });

    auto presets = host.def_submodule(
        "presets",
        "Change slicer settings live. Edits land in the preset the settings tabs are editing, "
        "exactly as if they had been typed there: fields update, the preset shows as modified, "
        "and the plate re-slices. Nothing reaches disk until save(). Calls run on the main/UI "
        "thread (marshaled from the plugin thread); do not call them from a SlicingPipeline hook, "
        "which runs on the slicing worker the UI thread can be waiting on.");

    presets.def("proposal_snapshot", [](const std::vector<std::string>& keys) {
        const auto snapshot = run_on_ui_blocking([&keys] {
            nlohmann::json values = nlohmann::json::object();
            for (const auto& key : keys) {
                if (!plugin_workspace_public_setting(key)) continue;
                if (auto* tab = tab_owning(key)) {
                    const auto* def = tab->get_config()->def()->get(key);
                    values[key] = {{"value", tab->get_config()->opt_serialize(key)},
                        {"label", def ? def->label : key}};
                }
            }
            return nlohmann::json{{"settings", values}, {"context_token", plugin_workspace_token()}};
        });
        return py::module_::import("json").attr("loads")(snapshot.dump());
    }, py::arg("keys"));

    presets.def("set_values", &set_values, py::arg("values"), py::arg("expected_context") = "",
                "Apply {setting_key: value} to the presets currently being edited and refresh the UI. "
                "Settings are addressed by key alone — the owning preset (process/filament/printer) is "
                "found for you. Values are strings, as in the preset files; numbers, bools and sequences "
                "are accepted and converted. Raises without applying any of the batch if a key is "
                "unknown, a number will not parse, or a number is outside the setting's declared "
                "range. A single value given for a per-extruder setting is applied to every extruder "
                "rather than shortening the list. Returns {key: (old, new)} for what actually changed.");

    presets.def("value", &edited_value, py::arg("key"),
                "The value a setting currently has in the preset being edited, including unsaved "
                "changes, or None if no preset defines that key.");

    presets.def("dirty_keys", &dirty_keys, py::arg("preset_type"),
                "Settings changed since this preset was last saved.");

    presets.def("save", &save_preset, py::arg("preset_type"), py::arg("name") = "",
                "Write the edited preset to disk. With no name, overwrites the current preset, which "
                "fails for a system preset — pass a name to save a copy under it instead.");

    presets.def("select", &select_preset, py::arg("preset_type"), py::arg("name"),
                "Select a preset by name. Unsaved changes make this prompt the user, the same way "
                "the preset dropdown does; save() or discard_changes() first to avoid that.");

    presets.def("discard_changes", &discard_changes, py::arg("preset_type"),
                "Drop unsaved changes and reload the preset as last saved.");
}

} // namespace Slic3r
