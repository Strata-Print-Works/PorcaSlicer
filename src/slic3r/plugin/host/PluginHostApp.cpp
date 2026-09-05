#include "PluginHostBindings.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/calib.hpp>

#include "slic3r/plugin/PluginUiThread.hpp"
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/Plater.hpp>

#include <memory>
#include <stdexcept>

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
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture);

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
    // UI language of the running app ("en_US", "ru_RU", ...), so plugins can
    // localize their own dialogs. The app config file that stores this value
    // is deny-listed by the audit hook (it sits next to cloud secrets), so a
    // read-only accessor is the supported way to get just the language.
    host.def("app_language", []() -> std::string {
        if (wxTheApp == nullptr)
            throw std::runtime_error("OrcaSlicer application is not initialized");
        return GUI::into_u8(GUI::wxGetApp().current_language_code_safe());
    });
}

} // namespace Slic3r
