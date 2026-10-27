#include "PluginHostBindings.hpp"
#include "PluginHostWorkspace.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include <nlohmann/json.hpp>
#include <libslic3r/LocalesUtils.hpp>
#include <libslic3r/Config.hpp>
#include <exception>
#include "PluginHostUi.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <memory>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include "pybind11/gil.h"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/PluginUiThread.hpp"

#include <cstddef>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/PresetComboBoxes.hpp> // TabPresetComboBox::set_filament_idx
#include <slic3r/GUI/Tab.hpp>

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
#include <wx/string.h>

#include <set>
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

// Handle to one category of currently edited preset. It is a category, not a snapshot: it always
// resolves the preset the matching tab edits now, so it stays valid across preset switches.
struct PresetEditor
{
    Preset::Type type = Preset::TYPE_INVALID;
};

const char* preset_tab_name(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return "process";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER:  return "printer";
    default:                    return "preset";
    }
}

PresetCollection& collection_for(PresetBundle& bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT:    return bundle.prints;
    case Preset::TYPE_FILAMENT: return bundle.filaments;
    case Preset::TYPE_PRINTER:  return printer_presets(bundle);
    default:                    throw std::invalid_argument("Unsupported preset category");
    }
}

// Marker for a user-declined selection: Tab::select_preset() returned false. It derives from
// std::runtime_error (pybind11 maps the base to RuntimeError), so callers see it as an ordinary
// failed transaction and no staged setting or slot assignment is created.
struct SelectionCancelled : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// --------------------------------------------------------------------------
// The staged preset update lifecycle described in docs/HLSD/preset-plugin-api.md.
//
// begin_update() resolves and binds one target (selecting it through the normal UI flow when
// the caller asked for a preset that is not already active), returns a PresetTransaction that
// stages changes with set(), and applies them only on commit(). The transaction holds names,
// never preset pointers, so it stays valid across reloads; a target or filament slot that changed
// between begin_update() and commit() is rejected instead of silently editing the new mapping.
//
// begin_update() is synchronous: it runs on the background Script worker, marshals the target
// selection to the UI thread with PluginHostUi::run_on_ui_thread(), and blocks that worker until
// the selection and any unsaved-edit dialog settle. The UI thread stays responsive because the
// Script capability owns its worker thread. No GUI or preset pointer outlives the call; the
// returned transaction holds copied category/name/slot values. The update() shorthand shares the
// same target resolution and is safe only from that worker, never from the UI thread or a slicing
// pipeline hook.
//
// Known limits of this implementation, kept explicit rather than hidden:
//   * A requested preset_name is resolved by the exact name its dropdown entry shows: the item's
//     display label or its stored item alias (the internal preset name). A name two entries carry
//     is rejected as ambiguous instead of being resolved by position. Process and printer resolve
//     against the tab dropdown; a filament preset_name resolves against the requested slot's own
//     sidebar selector, not the tab's, so its compatibility filter is the one that applies. A
//     target the selector does not offer fails before the flow's unsaved-edit prompt. The flow
//     itself is unchanged and the target is still confirmed as the edited preset afterwards.
//   * A filament preset_name that names a different preset for a slot selects it through
//     Tab::select_preset() (the normal flow, honouring unsaved-edit prompts) and then updates the
//     slot mapping with PresetBundle::set_filament_preset() plus every dependent side effect the
//     sidebar selection path performs: the project-dirty/selection refresh, the dynamic filament
//     list, the slot's own combo (re-run so it lists and selects the assigned target), the
//     conditional support-filament flushing recalc and the slot's AMS sync badge (read from the
//     slot combo's own flag). The private Plater::priv::on_select_preset() path cannot be reused
//     from here, so this mirrors its filament branch rather than calling it.
//   * A user-declined selection does not undo decisions the user already accepted in an earlier
//     dialog. It aborts before any slot assignment or transaction is created, so no staged
//     setting is applied.
// --------------------------------------------------------------------------

// Outcome of one committed transaction. Identifies the target, whether selecting it switched
// anything, and for filament (whose preset may be shared) which slots share the edited preset.
struct PresetUpdateResult
{
    Preset::Type             type                    = Preset::TYPE_INVALID;
    std::string              target_name;
    bool                     selection_changed       = false;
    int                      slot_number             = -1; // 1-based for filament, -1 otherwise
    bool                     slot_assignment_changed = false;
    std::vector<int>         shared_slots;
    std::vector<std::string> keys;
    bool                     dirty                   = false;
};

struct PresetTransaction
{
    Preset::Type  type                    = Preset::TYPE_INVALID;
    std::string   target_name;
    bool          filament                = false;
    int           slot_number             = 0; // 1-based, filament only
    std::string   slot_assignment_snapshot;    // slot's assigned preset as of begin_update()
    bool          selection_changed       = false;
    bool          slot_assignment_changed = false;

    std::vector<std::pair<std::string, std::string>> staged;
    std::set<std::string>                            staged_keys;
    bool                                             committed = false;

    void               set(const std::string& key, const std::string& value);
    PresetUpdateResult commit();
};

GUI::GUI_App& require_gui_app()
{
    GUI::GUI_App& app = GUI::wxGetApp();
    if (app.is_closing())
        throw std::runtime_error("OrcaSlicer is closing");
    if (app.preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");
    if (app.plater() == nullptr)
        throw std::runtime_error("OrcaSlicer main window is not available");
    return app;
}

GUI::Tab* require_tab(GUI::GUI_App& app, Preset::Type type)
{
    GUI::Tab* tab = app.get_tab(type);
    if (tab == nullptr)
        throw std::runtime_error(std::string("The ") + preset_tab_name(type) + " preset tab is not available");
    // The tab for this preset type exists, but the selected printer's technology may not use it
    // (the print tab no-ops under SLA); refuse instead of writing a value the UI cannot apply.
    if (!tab->supports_printer_technology(app.preset_bundle->printers.get_selected_preset().printer_technology()))
        throw std::runtime_error(std::string("The ") + preset_tab_name(type)
                                 + " preset is not available for the selected printer technology");
    return tab;
}

// True for a combo row that is a group header, a marker or a disabled row: it carries a marker in
// its client data and is not a selectable preset.
bool combo_item_is_marker(GUI::PresetComboBox& combo, int item)
{
    const auto marker = reinterpret_cast<std::size_t>(combo.GetClientData(item));
    return marker >= GUI::PresetComboBox::LABEL_ITEM_DISABLED && marker < GUI::PresetComboBox::LABEL_ITEM_MAX;
}

// How a caller-supplied name resolved against a combo's actual items.
struct ComboPresetMatch
{
    int         item          = -1;    // first matching item, or -1
    bool        ambiguous     = false; // two distinct items carry this name
    std::string internal_name;         // internal preset name of the match (empty when none)
};

// Resolve `requested` by the name each combo item shows: its display label, or its stored item
// alias (the internal preset name, or the printer model for a system printer preset). Names are
// compared with the modified-dirty suffix removed, i.e. exactly what the dropdown shows. Reading
// the combo's own items means this respects the combo's real selectable model (visibility, its
// show-incompatible state and, for a slot combo, that slot's compatibility). A name that two
// distinct items carry is reported ambiguous instead of being resolved by position.
ComboPresetMatch resolve_combo_preset(GUI::PresetComboBox& combo, const std::string& requested)
{
    ComboPresetMatch  match;
    const std::string want       = Preset::remove_suffix_modified(requested);
    const int         item_count = combo.GetCount();
    for (int item = 0; item < item_count; ++item) {
        if (combo_item_is_marker(combo, item))
            continue;
        const wxString    alias      = combo.GetItemAlias(item);
        const std::string alias_name = alias.empty() ? std::string()
                                                     : Preset::remove_suffix_modified(alias.ToUTF8().data());
        const std::string label_name = Preset::remove_suffix_modified(combo.GetString(item).ToUTF8().data());
        if ((alias_name.empty() || alias_name != want) && label_name != want)
            continue;
        if (match.item >= 0 && match.item != item) {
            match.ambiguous = true;
            return match;
        }
        match.item          = item;
        match.internal_name = alias_name.empty() ? label_name : alias_name;
    }
    return match;
}

// Index of the item whose internal name is `preset_name`, or -1. Used for per-item flags (the AMS
// sync badge); the internal name is unique, so no ambiguity handling is needed here.
int combo_item_for_preset(GUI::PresetComboBox& combo, const std::string& preset_name)
{
    const std::string want       = Preset::remove_suffix_modified(preset_name);
    const int         item_count = combo.GetCount();
    for (int item = 0; item < item_count; ++item) {
        if (combo_item_is_marker(combo, item))
            continue;
        const wxString    alias = combo.GetItemAlias(item);
        const std::string alias_name = alias.empty() ? std::string()
                                                     : Preset::remove_suffix_modified(alias.ToUTF8().data());
        if (!alias_name.empty() && alias_name == want)
            return item;
    }
    return -1;
}

// Resolve `requested` to the internal preset name its dropdown entry names, or throw. `category`
// and `selector` are used only for the error text. A name the selector does not offer, or that two
// entries share, raises std::invalid_argument.
std::string resolve_preset_in_combo(GUI::PresetComboBox& combo, const std::string& requested,
                                    const std::string& category, const std::string& selector)
{
    const ComboPresetMatch match = resolve_combo_preset(combo, requested);
    if (match.ambiguous)
        throw std::invalid_argument("The " + category + " name \"" + requested + "\" is ambiguous in the " + selector
                                    + "; more than one entry shows it, so name the preset by its own entry");
    if (match.item < 0)
        throw std::invalid_argument("The " + category + " preset \"" + requested + "\" is not offered by the " + selector);
    return match.internal_name;
}

// The preset a filament slot currently resolves to, by its internal name. A slot whose stored
// name is empty or does not resolve yields an error identifying the slot.
std::string resolved_slot_preset(PresetBundle& bundle, int slot_number)
{
    if (slot_number < 1 || static_cast<size_t>(slot_number) > bundle.filament_presets.size())
        throw std::out_of_range("Filament slot " + std::to_string(slot_number)
                                + " is out of range; slot numbers are 1-based and this printer has "
                                + std::to_string(bundle.filament_presets.size()) + " filament slot(s)");
    const std::string& assigned_name = bundle.filament_presets[static_cast<size_t>(slot_number) - 1];
    if (assigned_name.empty())
        throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " has no assigned filament preset");
    Preset* preset = bundle.filaments.find_preset(assigned_name);
    if (preset == nullptr)
        throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " is assigned \"" + assigned_name
                                 + "\", which does not resolve to a filament preset");
    return preset->name;
}

void PresetTransaction::set(const std::string& key, const std::string& value)
{
    if (committed)
        throw std::runtime_error("This preset transaction has already been committed; start a new begin_update()");
    if (key.empty())
        throw std::invalid_argument("A setting key is required");
    // Conflicting duplicate writes to one setting are rejected rather than resolved by order.
    if (!staged_keys.insert(key).second)
        throw std::invalid_argument("Setting \"" + key + "\" is already staged in this transaction; "
                                    "conflicting duplicate writes to one setting are rejected");
    staged.emplace_back(key, value);
}

PresetUpdateResult PresetTransaction::commit()
{
    if (committed)
        throw std::runtime_error("This preset transaction has already been committed; start a new begin_update()");
    if (type == Preset::TYPE_INVALID)
        throw std::runtime_error("This preset transaction has no target");

    PresetUpdateResult result;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App&     app        = require_gui_app();
        GUI::Tab*         tab        = require_tab(app, type);
        PresetBundle&     bundle     = *app.preset_bundle;
        PresetCollection& collection = collection_for(bundle, type);

        // The target must still be the edited preset: the user may have switched away while the
        // transaction was staged, and editing a different preset would break "explicit target".
        const std::string edited = collection.get_edited_preset().name;
        if (edited != target_name)
            throw std::runtime_error("The edited " + std::string(preset_tab_name(type)) + " preset is \"" + edited
                                     + "\", not this transaction's target \"" + target_name
                                     + "\"; nothing was applied");

        if (filament) {
            // Reject a stale slot mapping instead of editing whatever the slot now points at.
            const std::string now_assigned = resolved_slot_preset(bundle, slot_number);
            if (now_assigned != slot_assignment_snapshot)
                throw std::runtime_error("Filament slot " + std::to_string(slot_number) + " now holds \"" + now_assigned
                                         + "\", not \"" + slot_assignment_snapshot
                                         + "\" as when this transaction was created; the slot mapping changed, so nothing was applied");
        }

        // Apply the whole batch through the tab's batch path: it validates every key and value before
        // touching the preset, rejects duplicate keys, runs the same per-setting commit path a hand
        // edit uses (validation, on_change, dirty state) and refreshes once for the batch. A failure
        // restores the edited preset, so no staged setting survives a failed commit.
        tab->apply_preset_config_values(staged);

        result.type                    = type;
        result.target_name             = target_name;
        result.selection_changed       = selection_changed;
        result.slot_number             = filament ? slot_number : -1;
        result.slot_assignment_changed = slot_assignment_changed;
        result.keys.reserve(staged.size());
        for (const auto& entry : staged)
            result.keys.push_back(entry.first);
        result.dirty = collection.get_edited_preset().is_dirty;

        if (filament) {
            // Every slot whose assigned preset resolves to the edited one is affected: the preset
            // is shared, not a per-slot override.
            for (std::size_t index = 0; index < bundle.filament_presets.size(); ++index) {
                Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[index]);
                if (preset != nullptr && preset->name == target_name)
                    result.shared_slots.push_back(static_cast<int>(index) + 1);
            }
        }
    });

    committed = true;
    return result;
}

// Process/printer target resolution: the active preset when no name is given, otherwise the named
// preset selected through the normal tab-selection flow (which enforces the dropdown/compatibility
// rules and the unsaved-edit prompt). Binds only after confirming the requested preset is edited.
// Runs on the UI thread with the GIL released, called from begin_preset_update().
void bind_preset_target(Preset::Type type, const std::string& preset_name, PresetTransaction& transaction)
{
    GUI::GUI_App&     app        = require_gui_app();
    GUI::Tab*         tab        = require_tab(app, type);
    PresetCollection& collection = collection_for(*app.preset_bundle, type);

    const std::string current = collection.get_edited_preset().name;
    std::string       target  = current;
    bool              selection_changed = false;

    if (!preset_name.empty()) {
        // Resolve by the name the tab's dropdown actually shows (display label or item alias), so a
        // plugin can pass exactly what the user sees. With no populated dropdown there is nothing
        // to resolve against, so fall back to collection internal names.
        GUI::TabPresetComboBox* combo = tab->get_combo_box();
        std::string             resolved;
        if (combo != nullptr && combo->GetCount() > 0)
            resolved = resolve_preset_in_combo(*combo, preset_name, preset_tab_name(type),
                                               "preset selector for the selected printer");
        else
            resolved = preset_name;

        Preset* requested = collection.find_preset(resolved);
        if (requested == nullptr)
            throw std::invalid_argument("No " + std::string(preset_tab_name(type)) + " preset named \"" + resolved
                                        + "\" is available to select");
        target = requested->name;
        if (target != current) {
            // Normal UI selection: honours the unsaved-edit prompt; a declined prompt returns false
            // and is a user cancellation, distinct from any other resolution failure.
            if (!tab->select_preset(target))
                throw SelectionCancelled("Switching the edited " + std::string(preset_tab_name(type)) + " preset to \""
                                         + target + "\" was canceled; nothing was staged");
            selection_changed = true;
        }
    }

    // select_preset() can still leave a different preset edited (e.g. the target is not an
    // allowed dropdown choice), so confirm before binding.
    if (collection.get_edited_preset().name != target)
        throw std::runtime_error("The edited " + std::string(preset_tab_name(type)) + " preset is \""
                                 + collection.get_edited_preset().name + "\", not \"" + target
                                 + "\"; the transaction was not created");

    transaction.type              = type;
    transaction.target_name       = target;
    transaction.selection_changed = selection_changed;
}

// Blocking form used by begin_update() and update().
PresetTransaction begin_preset_update(Preset::Type type, const std::string& preset_name)
{
    PresetTransaction transaction;
    PluginHostUi::run_on_ui_thread([&] { bind_preset_target(type, preset_name, transaction); });
    return transaction;
}

// Filament target resolution: slot_number identifies the slot; an omitted preset_name keeps the
// preset assigned to it. A supplied preset_name selects that preset for the slot through the normal
// tab-selection flow, then records the slot assignment and its dependent Plater side effects (the
// ones the sidebar selection path performs) only after that flow succeeds, so a canceled prompt
// changes neither. A mixed-color virtual slot is rejected before any of this, with or without a
// preset_name. A requested preset the requested slot's own dropdown does not offer is rejected
// before the selection flow, so no dialog is raised for it; the one exception is the slot's own
// current assignment, which stays targetable when the dropdown hides it. A different preset the
// tab happens to edit is a globally edited target, not an offered choice for this slot, and is
// rejected.
// Runs on the UI thread with the GIL released, called from begin_filament_update().
void bind_filament_target(int slot_number, const std::string& preset_name, PresetTransaction& transaction)
{
    GUI::GUI_App& app    = require_gui_app();
    PresetBundle& bundle = *app.preset_bundle;
    GUI::Plater*  plater = app.plater();

    // A mixed-color virtual slot has no single filament preset behind it, so every form of this
    // call is rejected: with or without a preset_name there is no one preset to edit or assign.
    // Range-guarded so an out-of-range slot still gets resolved_slot_preset()'s clear error.
    if (slot_number >= 1 && static_cast<size_t>(slot_number) <= bundle.filament_presets.size()
        && bundle.is_mixed_filament(static_cast<size_t>(slot_number) - 1))
        throw std::invalid_argument("Filament slot " + std::to_string(slot_number)
                                    + " is a mixed-color virtual slot; it has no single filament preset to edit or assign");

    // Resolve and validate before any selection, so a bad slot fails without a prompt.
    const std::string assigned_internal = resolved_slot_preset(bundle, slot_number);

    GUI::Tab* tab = require_tab(app, Preset::TYPE_FILAMENT);

    const size_t slot_index = static_cast<size_t>(slot_number) - 1;

    std::string target = assigned_internal;
    if (!preset_name.empty()) {
        // Resolve against the requested sidebar slot's own selector, not the tab's: that is the
        // model the user picks this slot's filament from, including its compatibility filter. The
        // selector matches the exact name shown (display label or item alias).
        std::vector<GUI::PlaterPresetComboBox*>& slot_combos = plater->sidebar().combos_filament();
        GUI::PlaterPresetComboBox* slot_combo = slot_index < slot_combos.size() ? slot_combos[slot_index] : nullptr;
        std::string                resolved;
        if (slot_combo != nullptr && slot_combo->GetCount() > 0) {
            const ComboPresetMatch match = resolve_combo_preset(*slot_combo, preset_name);
            if (match.ambiguous)
                throw std::invalid_argument("The filament name \"" + preset_name
                                            + "\" is ambiguous in the filament preset selector for filament slot "
                                            + std::to_string(slot_number)
                                            + "; more than one entry shows it, so name the preset by its own entry");
            if (match.item >= 0) {
                resolved = match.internal_name;
            } else {
                // Not shown by this slot's selector. Only the slot's current assignment stays
                // targetable (a hidden or incompatible-but-selected preset the dropdown omits);
                // resolve it by internal name or by display alias and accept only that preset. A
                // different preset the tab happens to edit is a globally edited target, not an
                // offered choice for this slot, and is rejected.
                Preset* by_name = bundle.filaments.find_preset(preset_name);
                if (by_name == nullptr) {
                    const std::string& alias_name = bundle.filaments.get_preset_name_by_alias(preset_name);
                    by_name                       = bundle.filaments.find_preset(alias_name);
                }
                if (by_name == nullptr || by_name->name != assigned_internal)
                    throw std::invalid_argument("Filament preset \"" + preset_name
                                                + "\" is not offered by the filament preset selector for filament slot "
                                                + std::to_string(slot_number));
                resolved = by_name->name;
            }
        } else {
            // The slot selector is not populated; fall back to the tab selector, then to collection
            // internal names, so the call still works when the sidebar has not been laid out.
            GUI::TabPresetComboBox* tab_combo = tab->get_combo_box();
            if (tab_combo != nullptr && tab_combo->GetCount() > 0)
                resolved = resolve_preset_in_combo(*tab_combo, preset_name, "filament",
                                                   "filament preset selector for the selected printer");
            else
                resolved = preset_name;
        }

        Preset* requested = bundle.filaments.find_preset(resolved);
        if (requested == nullptr)
            throw std::invalid_argument("No filament preset named \"" + resolved + "\" is available to select");
        target = requested->name;
    }

    bool selection_changed = false;
    if (bundle.filaments.get_edited_preset().name != target) {
        // A declined prompt is a user cancellation: it is thrown before any slot-assignment change,
        // so neither the mapping nor a setting is touched.
        if (!tab->select_preset(target))
            throw SelectionCancelled("Switching the edited filament preset to \"" + target + "\" for filament slot "
                                     + std::to_string(slot_number)
                                     + " was canceled; neither the slot assignment nor any setting was changed");
        if (bundle.filaments.get_edited_preset().name != target)
            throw std::runtime_error("The edited filament preset is \"" + bundle.filaments.get_edited_preset().name
                                     + "\", not \"" + target + "\"; the transaction was not created");
        selection_changed = true;
    }

    bool assignment_changed = false;
    if (!preset_name.empty() && target != assigned_internal) {
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");
        // Mirror the filament branch of Plater::priv::on_select_preset() (the sidebar's normal
        // preset selection): update the mapping, then every dependent project/UI side effect,
        // rather than the vector entry alone.
        const bool was_support = GUI::is_support_filament(static_cast<int>(slot_index));
        bundle.set_filament_preset(slot_index, target);
        plater->update_project_dirty_from_presets();
        bundle.export_selections(*app.app_config);
        plater->sidebar().update_dynamic_filament_list();
        // Assigning a support filament (or moving away from one) changes which filaments the
        // flush rules apply to, so the normal flow recalculates the flushing volumes when
        // auto-calc covers every filament.
        if (GUI::is_support_filament(static_cast<int>(slot_index)) != was_support
            && app.app_config->get("auto_calculate_flush") == "all")
            plater->sidebar().auto_calc_flushing_volumes(static_cast<int>(slot_index));
        // Refresh the requested slot's own sidebar combo so its list and current selection show the
        // newly assigned target (the list may not have contained it), matching the sidebar
        // selection path. Fetch the pointer after update_dynamic_filament_list(), which may
        // rebuild the combo vector.
        std::vector<GUI::PlaterPresetComboBox*>& slot_combos = plater->sidebar().combos_filament();
        if (slot_index < slot_combos.size() && slot_combos[slot_index] != nullptr) {
            GUI::PlaterPresetComboBox* slot_combo = slot_combos[slot_index];
            slot_combo->update();
            // Keep the slot's AMS sync badge truthful, reading the item's own flag, the same source
            // on_select_preset() reads, so no UI state is invented here.
            const int item = combo_item_for_preset(*slot_combo, target);
            if (item >= 0)
                slot_combo->set_sync_badge(slot_combo->GetFlag(item)
                                           == static_cast<int>(GUI::PresetComboBox::FilamentAMSType::FROM_AMS));
        }
        plater->on_filament_change(slot_index);
        assignment_changed = true;
    }

    // Match PlaterPresetComboBox::switch_to_tab(): record which 0-based filament slot this tab
    // combobox now edits, so the GUI shows the slot the value was written for.
    if (GUI::TabPresetComboBox* combo = tab->get_combo_box())
        combo->set_filament_idx(slot_number - 1);

    transaction.type                     = Preset::TYPE_FILAMENT;
    transaction.filament                 = true;
    transaction.slot_number              = slot_number;
    transaction.target_name              = target;
    transaction.selection_changed        = selection_changed;
    transaction.slot_assignment_changed  = assignment_changed;
    // Snapshot the mapping after the explicit assignment, as the slot's resolved internal name,
    // so commit() compares like with like (a stored alias resolves to the same internal preset).
    transaction.slot_assignment_snapshot = resolved_slot_preset(bundle, slot_number);
}

// Blocking form used by begin_update() and update().
PresetTransaction begin_filament_update(int slot_number, const std::string& preset_name)
{
    PresetTransaction transaction;
    PluginHostUi::run_on_ui_thread([&] { bind_filament_target(slot_number, preset_name, transaction); });
    return transaction;
}

// Compatibility shorthand: one staged setting, committed immediately through the same
// begin_update()/set()/commit() path, so targeting, selection, validation and commit are shared.
void update_edited_preset(Preset::Type type, const std::string& key, const std::string& value)
{
    PresetTransaction transaction = begin_preset_update(type, std::string());
    transaction.set(key, value);
    transaction.commit();
}

void update_filament_slot(int slot_number, const std::string& key, const std::string& value)
{
    PresetTransaction transaction = begin_filament_update(slot_number, std::string());
    transaction.set(key, value);
    transaction.commit();
}

void reload_local_bundle(const std::string& bundle_id)
{
    bool        reloaded = false;
    std::string error;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App& app = GUI::wxGetApp();
        if (app.is_closing())
            throw std::runtime_error("OrcaSlicer is closing");
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");
        if (app.mainframe == nullptr)
            throw std::runtime_error("OrcaSlicer main window is not available");

        reloaded = app.preset_bundle->reload_local_bundle(app.app_config->get("preset_folder"), bundle_id, &error);
        if (!reloaded)
            return;

        app.mainframe->update_side_preset_ui();
        for (GUI::Tab* tab : app.tabs_list) {
            tab->reload_config();
            tab->update_changed_ui();
        }
        if (GUI::Plater* plater = app.plater())
            plater->sidebar().update_all_preset_comboboxes();
    });
    if (!reloaded)
        throw std::runtime_error(error.empty() ? "Local bundle was not reloaded" : error);
}

std::vector<std::string> list_bundle_ids()
{
    std::vector<std::string> ids;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::GUI_App& app = GUI::wxGetApp();
        if (app.is_closing())
            throw std::runtime_error("OrcaSlicer is closing");
        if (app.preset_bundle == nullptr)
            throw std::runtime_error("Preset bundle is not available");
        if (app.app_config == nullptr)
            throw std::runtime_error("Application configuration is not available");

        ids = app.preset_bundle->list_local_bundle_ids(app.app_config->get("preset_folder"));
    });
    return ids;
}

} // namespace

void host_bindings::register_presets(py::module_& host)
{
    auto preset  = host.def_submodule("preset", "Preset operations");
    auto bundles = preset.def_submodule("bundles", "Local preset bundle operations");
    bundles.def("reload_local", &reload_local_bundle, py::arg("bundle_id"),
                "Reload one local preset bundle from the active user's _local directory.");
    bundles.def("list_ids", &list_bundle_ids,
                "IDs of local preset bundles in the active user's _local directory that reload_local() accepts, "
                "including bundles added after startup.");

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

    py::class_<PresetUpdateResult>(preset, "PresetUpdateResult",
                                   "Outcome of one committed preset transaction. target_name is the preset that was "
                                   "edited; selection_changed says whether begin_update() switched the active preset; "
                                   "for filament, slot_number is the 1-based target slot, slot_assignment_changed "
                                   "says whether selecting the requested preset reassigned that slot, and shared_slots "
                                   "lists every 1-based slot whose assigned preset is the edited (shared) one. keys "
                                   "lists the settings applied, in order, and dirty says whether the edited preset "
                                   "now differs from its saved state.")
        .def_readonly("type", &PresetUpdateResult::type)
        .def_readonly("target_name", &PresetUpdateResult::target_name)
        .def_readonly("selection_changed", &PresetUpdateResult::selection_changed)
        .def_readonly("slot_number", &PresetUpdateResult::slot_number)
        .def_readonly("slot_assignment_changed", &PresetUpdateResult::slot_assignment_changed)
        .def_readonly("shared_slots", &PresetUpdateResult::shared_slots)
        .def_readonly("keys", &PresetUpdateResult::keys)
        .def_readonly("dirty", &PresetUpdateResult::dirty)
        .def_property_readonly("category", [](const PresetUpdateResult& result) { return preset_tab_name(result.type); })
        .def("__repr__", [](const PresetUpdateResult& result) {
            return std::string("<PresetUpdateResult ") + preset_tab_name(result.type) + " \"" + result.target_name
                   + "\" keys=" + std::to_string(result.keys.size()) + ">";
        });

    py::class_<PresetTransaction>(preset, "PresetTransaction",
                                  "A staged, single-target preset edit returned by begin_update(). set() stages a "
                                  "serialized value without mutating the preset; commit() validates and applies the "
                                  "whole batch through the normal setting-change path. begin_update() blocks the "
                                  "background Script worker until any selection or unsaved-edit dialog is answered, "
                                  "while the UI thread stays responsive; set() and commit() run synchronously on the "
                                  "same worker. Nothing is saved to disk: the edited preset stays dirty until the user "
                                  "saves it.")
        .def_readonly("type", &PresetTransaction::type)
        .def_readonly("target_name", &PresetTransaction::target_name)
        .def_readonly("selection_changed", &PresetTransaction::selection_changed)
        .def_property_readonly("slot_number", [](const PresetTransaction& transaction) {
            return transaction.filament ? transaction.slot_number : -1;
        })
        .def_property_readonly("category", [](const PresetTransaction& transaction) { return preset_tab_name(transaction.type); })
        .def_property_readonly("staged_keys", [](const PresetTransaction& transaction) {
            std::vector<std::string> keys;
            keys.reserve(transaction.staged.size());
            for (const auto& entry : transaction.staged)
                keys.push_back(entry.first);
            return keys;
        })
        .def("set", &PresetTransaction::set, py::arg("key"), py::arg("value"),
             "Stage one setting change. key must be a setting of the target preset category and value its "
             "serialized string form; staging validates neither, so commit() is where an invalid key or value "
             "fails. Staging the same key twice in one transaction raises ValueError (conflicting duplicate "
             "writes are rejected). After commit() the transaction is closed.")
        .def("commit", &PresetTransaction::commit,
             "Validate and apply every staged setting through the same path as editing it in the GUI, then return "
             "a PresetUpdateResult. An invalid key or value raises ValueError; a changed target or filament slot "
             "mapping raises RuntimeError; a failure leaves none of the staged settings applied. The edited preset "
             "becomes dirty until the user saves it.");

    py::class_<PresetEditor>(preset, "PresetEditor",
                             "Handle to a category of currently edited preset. It is not a snapshot: it always "
                             "resolves the preset the matching tab edits now, so it stays usable across preset "
                             "switches. update() edits the preset in place; the change is not saved to disk.")
        .def_readonly("type", &PresetEditor::type)
        .def(
            "begin_update",
            [](const PresetEditor& editor, const std::string& preset_name) {
                if (editor.type == Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("begin_update on the filament editor takes "
                                                "(slot_number, preset_name=\"\"); use orca.host.preset.filament.begin_update(slot_number, ...)");
                return begin_preset_update(editor.type, preset_name);
            },
            py::arg("preset_name") = "",
            "Resolve and bind this category's target preset and return a PresetTransaction. With no preset_name the "
            "currently active preset is targeted; a preset_name must be a preset this category can select, and is "
            "selected through the normal preset-selection flow (which prompts about unsaved edits). The call blocks "
            "the background Script worker until any such dialog is answered, leaving the UI thread responsive. A "
            "selection the user cancels raises RuntimeError; an unavailable or invalid target raises ValueError. "
            "Either failure creates no transaction and stages no setting. Call it from the background Script worker, "
            "never from the UI thread. This form is for process and printer; filament takes "
            "begin_update(slot_number, preset_name=\"\").")
        .def(
            "begin_update",
            [](const PresetEditor& editor, int slot_number, const std::string& preset_name) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of begin_update() is only available on "
                                                "orca.host.preset.filament; call begin_update(preset_name=\"\") on this editor");
                return begin_filament_update(slot_number, preset_name);
            },
            py::arg("slot_number"), py::arg("preset_name") = "",
            "Resolve and bind a filament preset by 1-based slot and return a PresetTransaction. Omitting preset_name "
            "targets the preset assigned to the slot; supplying a different name selects it for the slot through the "
            "normal flow (prompting about unsaved edits) and, once that succeeds, records the new slot assignment with "
            "the same Plater side effects the sidebar selection performs (project/selection refresh, dynamic filament "
            "list, conditional support-filament flushing recalc and the slot's AMS sync badge). A requested preset "
            "this slot's dropdown does not offer is rejected before that flow, so it raises no prompt; the slot's own "
            "current assignment stays targetable even when the dropdown hides it. A selection the user cancels raises "
            "RuntimeError and changes neither the slot assignment nor any setting. An out-of-range, empty or "
            "unresolved slot is rejected, and a mixed-color virtual slot is rejected whether or not preset_name is "
            "given. The transaction edits the shared preset, so every slot assigned to it is affected. The call "
            "blocks the background Script worker until the selection settles, leaving the UI thread responsive; call "
            "it from that worker, never from the UI thread.")
        .def(
            "update",
            [](const PresetEditor& editor, const std::string& key, const std::string& value) {
                update_edited_preset(editor.type, key, value);
            },
            py::arg("key"), py::arg("value"),
            "Set one setting of the currently edited preset and run the same committed-change path as "
            "editing the setting in the GUI; the edited preset becomes dirty until saved/exported. "
            "key must be a setting of this preset category and value its serialized string form, "
            "otherwise ValueError is raised and nothing is written. A setting edited through a field "
            "runs that field's validation even before its page has been opened; one the GUI edits "
            "outside a field (such as compatible printers) also recomputes preset compatibility. "
            "Equivalent to begin_update() with the active preset as target, then set(key, value) "
            "and commit(); like begin_update() this call blocks the background Script worker.")
        .def(
            "update",
            [](const PresetEditor& editor, int slot_number, const std::string& key, const std::string& value) {
                if (editor.type != Preset::TYPE_FILAMENT)
                    throw std::invalid_argument("The slot_number form of update() is only available on "
                                                "orca.host.preset.filament; call update(key, value) on this editor");
                update_filament_slot(slot_number, key, value);
            },
            py::arg("slot_number"), py::arg("key"), py::arg("value"),
            "Set one setting of the filament preset assigned to a 1-based filament slot. When that preset "
            "is not the one currently edited the call switches to it through the normal preset-selection "
            "flow, so an unsaved edited preset still prompts the user and a declined prompt cancels the "
            "call with an error and no change. An out-of-range, empty or unresolved slot is rejected with "
            "a clear error, as is a mixed-color virtual slot, which has no single preset to edit. Other "
            "preset editors accept only update(key, value); this form raises "
            "ValueError on them. Equivalent to begin_update(slot_number) with an omitted preset_name "
            "(the slot's current assignment is kept), then set(key, value) and commit(); like "
            "begin_update() this call blocks the background Script worker.");

    // The three preset categories edited by the main tabs. `process` is the print/process preset,
    // `printer` the machine preset and `filament` the currently edited filament preset (its
    // 3-argument form addresses a 1-based filament slot); each only accepts its own settings, so a
    // printer setting passed to `process` is rejected instead of written silently.
    preset.attr("process")  = py::cast(PresetEditor{Preset::TYPE_PRINT});
    preset.attr("printer")  = py::cast(PresetEditor{Preset::TYPE_PRINTER});
    preset.attr("filament") = py::cast(PresetEditor{Preset::TYPE_FILAMENT});

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
