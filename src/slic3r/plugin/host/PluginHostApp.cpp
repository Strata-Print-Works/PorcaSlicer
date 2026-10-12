#include "PluginHostBindings.hpp"
#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/calib.hpp>

#include "slic3r/plugin/PluginUiThread.hpp"
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/Semver.hpp>
#include <libslic3r_version.h>
#include <slic3r/GUI/BuildCommit.hpp>
#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/OpenGLManager.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/Utils/CloudProvider.hpp>
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/bambu_networking.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

#include <memory>
#include <stdexcept>
#include <wx/app.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <string>
#include <utility>

namespace py = pybind11;

namespace Slic3r {
namespace {

GUI::Plater* current_plater()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    return plater;
}

PresetBundle* current_preset_bundle()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    return preset_bundle;
}


// --------------------------------------------------------------------------
// orca.host.calibrate
// --------------------------------------------------------------------------
// The Calibration menu's test-tower generators, reachable from a plugin. Being
// able to explain how to tune pressure advance is worth much less than being
// able to put the tower on the plate — the explanation is the part the user can
// already find, the setup is the part they get wrong.
//
// DESTRUCTIVE: each of these starts a new project, exactly as the menu items
// do, so whatever is on the plate is discarded. The caller is responsible for
// asking first; there is no undo past this point.
void calibrate(const std::string& test, py::object start, py::object end, py::object step)
{
    // Read the optional overrides before marshaling: these are Python objects
    // and the UI-thread call below runs with the GIL released.
    const bool   has_start = !start.is_none();
    const bool   has_end   = !end.is_none();
    const bool   has_step  = !step.is_none();
    const double v_start   = has_start ? start.cast<double>() : 0.0;
    const double v_end     = has_end ? end.cast<double>() : 0.0;
    const double v_step    = has_step ? step.cast<double>() : 0.0;

    // Defaults are the ones the calibration dialogs open with, so an
    // unparameterised call gives the same tower the menu would.
    Calib_Params params;
    bool         is_flowrate = false;
    if (test == "temp") {
        params.mode  = CalibMode::Calib_Temp_Tower;
        params.start = 230.0; params.end = 190.0; params.step = 5.0;
    } else if (test == "pa") {
        params.mode  = CalibMode::Calib_PA_Tower;
        params.start = 0.0; params.end = 0.1; params.step = 0.002;
    } else if (test == "retraction") {
        params.mode  = CalibMode::Calib_Retraction_tower;
        params.start = 0.0; params.end = 2.0; params.step = 0.1;
    } else if (test == "max_volumetric_speed") {
        params.mode  = CalibMode::Calib_Vol_speed_Tower;
        params.start = 5.0; params.end = 20.0; params.step = 0.5;
    } else if (test == "vfa") {
        params.mode  = CalibMode::Calib_VFA_Tower;
        params.start = 40.0; params.end = 200.0; params.step = 10.0;
    } else if (test == "flow") {
        is_flowrate = true;              // no tower params; its own two-pass plate
    } else {
        throw std::invalid_argument(
            "unknown calibration test: " + test +
            " (expected temp, pa, retraction, max_volumetric_speed, vfa or flow)");
    }

    if (is_flowrate && (has_start || has_end || has_step))
        throw std::invalid_argument("the flow test takes no start/end/step");

    if (has_start) params.start = v_start;
    if (has_end)   params.end   = v_end;
    if (has_step)  params.step  = v_step;

    // A zero or negative step never terminates the tower generator, and a range
    // that cannot be walked in whole steps produces an empty plate. Both are
    // easier to reject here than to explain afterwards. Temperature towers run
    // downwards, so compare against the direction the caller actually asked for.
    if (!is_flowrate) {
        if (!(params.step > 0.0))
            throw std::invalid_argument("calibration step must be greater than zero");
        if (params.start == params.end)
            throw std::invalid_argument("calibration start and end must differ");
    }

    run_on_ui_blocking([&params, is_flowrate]() {
        GUI::Plater* plater = current_plater();
        if (is_flowrate)
            plater->calib_flowrate(true, 1);   // linear, first pass — the dialog's default
        else
            switch (params.mode) {
            case CalibMode::Calib_Temp_Tower:      plater->calib_temp(params); break;
            case CalibMode::Calib_PA_Tower:        plater->calib_pa(params); break;
            case CalibMode::Calib_Retraction_tower: plater->calib_retraction(params); break;
            case CalibMode::Calib_Vol_speed_Tower: plater->calib_max_vol_speed(params); break;
            case CalibMode::Calib_VFA_Tower:       plater->calib_VFA(params); break;
            default: break;
            }
        // Slice it, so what lands is a plate ready to print rather than one
        // more step for the user to work out.
        plater->reslice();
    });
}

GUI::GUI_App& current_app()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return GUI::wxGetApp();
}

// The plater and the device list are only safe to touch from the UI thread, and the 3MF export
// renders thumbnails, which needs the UI thread's GL context.
void require_main_thread(const char* function_name)
{
    if (!wxIsMainThread())
        throw std::runtime_error(std::string(function_name) + "() must be called from the UI thread");
}

void export_3mf_copy(GUI::Plater& plater, const std::string& path)
{
    require_main_thread("export_3mf_copy");
    if (!boost::iends_with(path, ".3mf"))
        throw py::value_error("export_3mf_copy() needs a path ending in .3mf");
    // The thumbnails are rendered by the 3D view, which may not exist yet while plugins load.
    if (!GUI::OpenGLManager::get_gl_info().is_detected())
        throw std::runtime_error("export_3mf_copy() needs the 3D view, which is not ready yet");
    // The file is written from C++, so raise the audit event Python's open(path, "w") raises: the
    // plugin gets the same deny list, permissions and prompt as for writing the file itself.
    if (PySys_Audit("open", "ssi", path.c_str(), "w", 0) < 0)
        throw py::error_already_set();
    if (!plater.export_3mf_copy(GUI::into_path(GUI::from_u8(path))))
        throw std::runtime_error("Failed to write " + path);
}

py::object gl_info()
{
    const GUI::OpenGLManager::GLInfo& info = GUI::OpenGLManager::get_gl_info();
    if (!info.is_detected())
        return py::none();
    py::dict out;
    out["vendor"]       = info.get_vendor();
    out["renderer"]     = info.get_renderer();
    out["version"]      = info.get_version();
    out["glsl_version"] = info.get_glsl_version();
    out["core_profile"] = info.is_core_profile();
    return std::move(out);
}

py::dict app_info()
{
    GUI::GUI_App& app = current_app();
    // The app config has no lock of its own; the UI thread is where it is written.
    require_main_thread("app_info");
    py::dict out;
    out["version"]  = SoftFever_VERSION;
    out["build"]    = GUI::build_commit_label;
    out["mode"]     = app.is_editor() ? "editor" : "gcode viewer";
    out["language"] = GUI::into_u8(app.current_language_code_safe());
    // The app config file is deny-listed for plugins; these are the parts a bug report needs.
    const boost::optional<Semver>& config_version = app.last_config_version();
    out["app_config_version"] = config_version && config_version->valid() ? config_version->to_string_sf() : std::string();
    out["stealth_mode"]       = app.app_config != nullptr && app.app_config->get_stealth_mode();
    out["is_signed_in"]       = app.is_user_login(ORCA_CLOUD_PROVIDER);
    out["is_bambu_signed_in"] = app.is_user_login(BBL_CLOUD_PROVIDER);
    out["network_plugin_version"] = NetworkAgent::is_network_module_loaded() ? NetworkAgent::get_version() : std::string();
    const NetworkLibraryLoadError load_error = NetworkAgent::get_load_error();
    out["network_plugin_error"] = load_error.has_error ? load_error.message : std::string();
    return out;
}

py::object selected_printer()
{
    GUI::GUI_App& app = current_app();
    require_main_thread("selected_printer");
    DeviceManager* devices = app.getDeviceManager();
    MachineObject* machine = devices != nullptr ? devices->get_selected_machine() : nullptr;
    if (machine == nullptr)
        return py::none();
    // Agent, model, link and firmware only: no name, serial, address or access code. Other agents
    // fill the model and firmware with placeholders, so those come from Bambu printers only.
    const bool bambu = machine->printer_agent_id == BBL_PRINTER_AGENT_ID;
    py::dict   out;
    out["agent"]      = machine->printer_agent_id;
    out["model_id"]   = bambu ? machine->printer_type : std::string();
    out["connection"] = machine->connection_type();
    out["online"]     = machine->is_online();
    out["connected"]  = machine->is_connected();
    out["firmware"]   = bambu ? machine->get_ota_version() : std::string();
    return std::move(out);
}

} // namespace

// Access to the live GUI application: the Plater and the module-level
// plater()/model()/preset_bundle() accessors. Everything here is owned by the
// app and only reachable once the GUI is up (the accessors throw before that).
void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater")
        .def("model", static_cast<Model& (GUI::Plater::*)()>(&GUI::Plater::model), py::return_value_policy::reference_internal)
        .def("is_project_dirty", &GUI::Plater::is_project_dirty)
        .def("is_presets_dirty", &GUI::Plater::is_presets_dirty)
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture)
        // Path the project was last opened from or saved to; empty while it has never been saved.
        .def("project_path", [](GUI::Plater& plater) {
            require_main_thread("project_path");
            return GUI::into_u8(plater.get_project_filename(".3mf"));
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Write the current project, unsaved changes included, to path as a 3MF copy. The project's "
             "file name and saved state are not changed, and the signed-in account is not written as the designer.");

    host.def("calibrate", &calibrate, py::arg("test"),
             py::arg("start") = py::none(), py::arg("end") = py::none(), py::arg("step") = py::none(),
             "Generate and slice one of the built-in calibration towers, the same ones the "
             "Calibration menu produces. test is one of \"temp\", \"pa\", \"retraction\", "
             "\"max_volumetric_speed\", \"vfa\" or \"flow\". start/end/step override the "
             "dialog defaults; the flow test takes none of them and raises if given any.\n"
             "\n"
             "DESTRUCTIVE — like the menu items, this starts a NEW PROJECT: everything on the "
             "current plate is discarded and there is no undo. Ask the user before calling it, "
             "and check plater().is_project_dirty() first if you want to know what they stand "
             "to lose.\n"
             "\n"
             "Raises ValueError for an unknown test, a step of zero or less, or a start equal "
             "to its end — each of those produces an empty or non-terminating tower.");

    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", []() -> Model& {
        return current_plater()->model();
    }, py::return_value_policy::reference);
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference);
    host.def("app_info", &app_info,
             "The running app: version, build, mode, language, app_config_version (the version that wrote the app config "
             "before this run, empty when there was none), stealth_mode, is_signed_in (Orca Cloud), is_bambu_signed_in (Bambu Cloud), network_plugin_version (empty "
             "when the Bambu network plugin is not loaded) and network_plugin_error (empty unless it failed to load).");
    host.def("gl_info", &gl_info,
             "OpenGL vendor, renderer, version, glsl_version and core_profile of the 3D view; None before it has been created.");
    host.def("selected_printer", &selected_printer,
             "Agent, model_id, connection ('lan' or 'cloud'), online, connected and firmware of the printer selected in the "
             "device list; model_id and firmware are empty for non-Bambu agents. None when none is selected. Print hosts "
             "configured in the printer preset (OctoPrint and the like) are not in the device list.");
}

} // namespace Slic3r
