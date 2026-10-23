#include "PlateLayoutDialog.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "PrinterScanDialog.hpp"
#include "Selection.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PlateLayout.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SelectionAlignment.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/PlateCapacity.hpp"
#include "libslic3r/PrintDeliveryValidation.hpp"
#include <stdexcept>
#include "libslic3r/libslic3r.h"
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {
namespace {
using InstanceKey = std::pair<unsigned int, unsigned int>;

std::vector<InstanceKey> selected_instances(Plater& plater)
{
    std::vector<InstanceKey> result;
    const auto& selection = plater.get_selection();
    if (!selection.is_single_full_instance() && !selection.is_multiple_full_instance() &&
        !selection.is_single_full_object() && !selection.is_multiple_full_object()) return result;
    for (const auto& object : selection.get_content())
        for (unsigned int instance : object.second) result.emplace_back(object.first, instance);
    return result;
}

bool editable_plate(Plater& plater)
{
    auto* plate = plater.get_partplate_list().get_curr_plate();
    return plate && !plate->is_locked() && plater.get_ui_job_worker().is_idle() &&
           !plater.is_background_process_slicing() && plater.printer_technology() == ptFFF;
}

bool on_active_plate(Plater& plater, const std::vector<InstanceKey>& keys)
{
    auto* plate = plater.get_partplate_list().get_curr_plate();
    return plate && !keys.empty() && std::all_of(keys.begin(), keys.end(), [&](const auto& key) {
        return key.first < plater.model().objects.size() &&
               key.second < plater.model().objects[key.first]->instances.size() &&
               plate->contain_instance(key.first, key.second);
    });
}

std::vector<InstanceKey> pieces_outside_plates(Plater& plater)
{
    std::vector<InstanceKey> result;
    auto& plates = plater.get_partplate_list();
    const auto& model = plater.model();
    for (size_t o = 0; o < model.objects.size(); ++o)
        for (size_t copy = 0; copy < model.objects[o]->instances.size(); ++copy) {
            if (!model.objects[o]->instances[copy]->printable) continue;
            bool assigned = false;
            for (int i = 0; i < plates.get_plate_count(); ++i)
                if (plates.get_plate(i)->contain_instance(o, copy)) {
                    assigned = true;
                    break;
                }
            if (!assigned) result.emplace_back(o, copy);
        }
    return result;
}

class LayoutDialog : public DPIDialog {
public:
    explicit LayoutDialog(const wxString& title)
        : DPIDialog(wxGetApp().mainframe, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        SetFont(Label::Body_14);
    }
    void finish(wxSizer* sizer) {
        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }
protected:
    void on_dpi_changed(const wxRect&) override {
        for (auto* child : GetChildren()) {
            if (auto* combo = dynamic_cast<::ComboBox*>(child)) combo->Rescale();
            else if (auto* input = dynamic_cast<::TextInput*>(child)) input->Rescale();
            else if (auto* button = dynamic_cast<Button*>(child)) button->Rescale();
            else if (auto* check = dynamic_cast<::CheckBox*>(child)) check->Rescale();
        }
        GetSizer()->SetSizeHints(this);
        Layout();
        Refresh();
    }
};

::ComboBox* choices(wxWindow* parent, const std::vector<wxString>& labels)
{
    auto* combo = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, parent->FromDIP(wxSize(250, -1)), 0, nullptr, wxCB_READONLY);
    for (const auto& label : labels) combo->Append(label);
    combo->SetSelection(0);
    return combo;
}

void row(wxWindow* parent, wxSizer* sizer, const wxString& label, wxWindow* control)
{
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(16));
    line->Add(control, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, parent->FromDIP(12));
}

bool read_gap(::TextInput* input, double& gap)
{
    return input->GetTextCtrl()->GetValue().ToDouble(&gap) && std::isfinite(gap) && gap >= 0. && gap <= 1000.;
}

// A reason a layout job stopped that the person can act on. The message is
// marked with L() where it is thrown and translated when it is shown.
struct LayoutFailure : std::runtime_error
{
    explicit LayoutFailure(const char* message) : std::runtime_error(message) {}
};

wxString layout_failure_message(const std::exception_ptr& error, const wxString& fallback)
{
    try {
        std::rethrow_exception(error);
    } catch (const LayoutFailure& failure) {
        return _L(failure.what());
    } catch (...) {
        return fallback;
    }
}

struct LayoutTask {
    Model snapshot;
    DynamicPrintConfig config;
    DynamicPrintConfig editing_config;
    DynamicPrintConfig plate_config;
    int plate_index = -1;
    std::string destination;
    Vec3d origin = Vec3d::Zero();
    std::vector<InstanceKey> sources;
    std::vector<InstanceKey> fixed;
    PlateLayoutInput input;
    PlateLayoutResult result;
    bool linked = true;
    bool rearrange = false;
};

bool model_unchanged(const Model& current, const Model& snapshot)
{
    if (current.id() != snapshot.id() || current.objects.size() != snapshot.objects.size()) return false;
    for (size_t i = 0; i < current.objects.size(); ++i) {
        const auto& a = *current.objects[i];
        const auto& b = *snapshot.objects[i];
        if (a.id() != b.id() || a.config.get() != b.config.get() ||
            a.instances.size() != b.instances.size() || a.volumes.size() != b.volumes.size()) return false;
        for (size_t j = 0; j < a.instances.size(); ++j)
            if (a.instances[j]->id() != b.instances[j]->id() ||
                a.instances[j]->get_transformation() != b.instances[j]->get_transformation() ||
                a.instances[j]->printable != b.instances[j]->printable) return false;
        for (size_t j = 0; j < a.volumes.size(); ++j)
            if (a.volumes[j]->id() != b.volumes[j]->id() || &a.volumes[j]->mesh() != &b.volumes[j]->mesh() ||
                a.volumes[j]->get_transformation() != b.volumes[j]->get_transformation() ||
                a.volumes[j]->config.get() != b.volumes[j]->config.get() ||
                a.volumes[j]->mmu_segmentation_facets.timestamp() != b.volumes[j]->mmu_segmentation_facets.timestamp()) return false;
    }
    return true;
}

bool unchanged(Plater& plater, const LayoutTask& task)
{
    if (!editable_plate(plater)) return false;
    auto* plate = plater.get_partplate_list().get_curr_plate();
    if (plate->get_index() != task.plate_index || plate->get_origin() != task.origin ||
        plate->print_setup().destination != task.destination ||
        wxGetApp().preset_bundle->full_config() != task.editing_config || *plate->config() != task.plate_config) return false;
    return model_unchanged(plater.model(), task.snapshot);
}

PlateLayoutShape project_shape(const Model& snapshot, const DynamicPrintConfig& config,
                               const Vec3d& origin, const InstanceKey& key, bool fixed, Job::Ctl& ctl)
{
    auto* object = snapshot.objects[key.first];
    auto* instance = object->instances[key.second];
    TriangleMesh mesh = object->raw_mesh();
    mesh.transform(instance->get_matrix_no_offset());
    PlateLayoutShape shape;
    shape.footprint = union_ex(project_mesh(mesh.its, Transform3d::Identity(), [&ctl] { if (ctl.was_canceled()) throw std::runtime_error("Canceled"); }));
    // Reuse Orca's support allowance; brim and skirt must remain protected even
    // when the user explicitly asks for a smaller spacing.
    const auto ap = get_instance_arrange_poly(instance, config);
    if (instance->get_offset().z() + mesh.bounding_box().max.z() > config.opt_float("printable_height") + 0.001)
        throw LayoutFailure(L("Some objects are too tall for this printer. Scale or rotate them, then try again."));
    const auto* brim = object->get_config_value<ConfigOptionFloat>(config, "brim_width");
    const auto* brim_type = object->get_config_value<ConfigOptionEnum<BrimType>>(config, "brim_type");
    shape.clearance = (brim && brim_type && brim_type->value != btNoBrim) ? brim->value : 0.;
    if (ap.has_tree_support || ap.brim_width > 1.) shape.clearance = std::max(shape.clearance, ap.brim_width);
    if (config.opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject)
        shape.clearance = std::max(shape.clearance, config.opt_float("extruder_clearance_radius") / 2. + 0.01);
    if (fixed) {
        const Vec3d delta = instance->get_offset() - origin;
        for (auto& poly : shape.footprint) poly.translate(scaled(delta.x()), scaled(delta.y()));
    }
    return shape;
}

PlateLayoutShape project_shape(const LayoutTask& task, const InstanceKey& key, bool fixed, Job::Ctl& ctl)
{
    return project_shape(task.snapshot, task.config, task.origin, key, fixed, ctl);
}

void prepare_bed(Plater& plater, PartPlate& plate, const DynamicPrintConfig& config, PlateLayoutInput& input)
{
    const Vec3d origin = plate.get_origin();
    Polygon bed;
    for (const auto& point : plate.get_shape()) bed.points.emplace_back(scaled(Vec2d(point - origin.head<2>())));
    bed.make_counter_clockwise();
    input.bed = {ExPolygon(bed)};
    if (auto zone = plate.imex_primary_zone()) {
        Polygon zone_polygon = scaled(*zone).polygon();
        zone_polygon.translate(scaled(-origin.x()), scaled(-origin.y()));
        input.bed = intersection_ex(input.bed, ExPolygons{ExPolygon(zone_polygon)});
    }
    for (const auto& box : plate.get_exclude_areas()) {
        Polygon exclusion = box.polygon(true);
        exclusion.translate(scaled(-origin.x()), scaled(-origin.y()));
        input.excluded.emplace_back(exclusion);
    }
    for (const auto& box : plate.imex_collision_zones()) {
        auto polygon = box.polygon(true);
        polygon.translate(scaled(-origin.x()), scaled(-origin.y()));
        input.excluded.emplace_back(std::move(polygon));
    }
    if (const auto tower = plater.canvas3D()->get_wipe_tower_info(plate.get_index())) {
        auto polygon = ExPolygon(scaled(tower.bounds()).polygon());
        polygon.translate(scaled(-origin.x()), scaled(-origin.y()));
        input.excluded.push_back(std::move(polygon));
    }
    if (config.opt_bool("enable_wrapping_detection")) {
        const auto points = plater.get_partplate_list().get_wrapping_exclude_area();
        for (size_t i = 0; i + 3 < points.size(); i += 4) {
            Polygon polygon;
            for (size_t j = i; j < i + 4; ++j) polygon.points.push_back(scaled(points[j]));
            polygon.make_counter_clockwise();
            input.excluded.emplace_back(std::move(polygon));
        }
    }
    if (config.opt_bool("scan_first_layer")) {
        arrangement::ArrangePolygons regions;
        plater.get_partplate_list().preprocess_nonprefered_areas(regions, 1, 0);
        for (const auto& region : regions) input.excluded.push_back(region.transformed_poly());
    }
    const double skirt = get_real_skirt_dist(config);
    if (skirt > 0.) input.bed = offset_ex(input.bed, -scaled(skirt));
}

struct WorkspacePlateState {
    ObjectID id;
    Vec3d origin;
    std::string stage_id;
    bool locked;
    std::string destination;
    DynamicPrintConfig setup;
    DynamicPrintConfig overrides;
};

struct WorkspacePlateTask {
    PlatePrintSetup process_setup;
    int index;
    Vec3d origin;
    std::string destination;
    std::string stage_id;
    std::string assigned_destination;
    std::string assigned_name;
    int anchor_printer = -1;
    int wave = 0;
    Vec2d planned_origin = Vec2d::Zero();
    size_t group = 0;
    std::vector<int> used_filaments;
    std::vector<std::vector<int>> source_filaments;
    bool can_repack = false;
    bool can_split = false;
    DynamicPrintConfig config; // Resolved settings used for planning, including plate overrides.
    DynamicPrintConfig plate_overrides; // Kept separate from the setup when the job is applied.
    std::vector<InstanceKey> sources;
    std::vector<InstanceKey> fixed;
    std::vector<PlateConsolidationRef> references;
    PlateLayoutInput input;
    PlateLayoutResult result;
    bool changed = false;
    bool removed = false;
};

struct WorkspacePrinterTarget {
    size_t group = 0;
    size_t printer = 0;
    DynamicPrintConfig config;
    PlateLayoutInput input;
};

struct WorkspacePrinterCandidate {
    size_t printer = 0;
    DynamicPrintConfig config;
    PlateLayoutInput input;
    PlateLayoutResult result;
};

struct WorkspaceLayoutTask {
    Model snapshot;
    DynamicPrintConfig editing_config;
    std::vector<WorkspacePlateState> states;
    std::vector<WorkspaceStage> stages_snapshot;
    std::vector<WorkspacePlateTask> plates;
    std::string stage_id;
    bool search_limited = false;
    int unplanned_plate = -1;
    size_t alternate_jobs = 0;
    int alternate_waves = 0;
    bool arrange_after_optimization = false;
    bool utilize_printers = false;
    bool favor_stages = false;
    size_t baseline_jobs = 0;
    size_t unassigned_instances = 0;
    size_t included_unassigned = 0;
    int unassigned_setup_plate = -1;
    bool pieces_do_not_fit = false;
    std::vector<SavedPrinterChoice> available_printers;
    // Presets copied once for the job, which resolves each saved printer from them.
    std::shared_ptr<const PresetBundle> presets;
    int unprepared_plate = -1;
    std::vector<WorkspacePrinterTarget> printer_targets;
    std::map<int, Vec2d> stage_anchors;
    std::vector<std::pair<int, Vec2d>> workspace_arrangement;
    // Every layout in the plan shares this deadline so the whole search stays bounded.
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool out_of_time = false;
};

bool workspace_unchanged(Plater& plater, const WorkspaceLayoutTask& task)
{
    if (plater.is_background_process_slicing() || !plater.get_ui_job_worker().is_idle() ||
        wxGetApp().preset_bundle->full_config() != task.editing_config ||
        !model_unchanged(plater.model(), task.snapshot)) return false;
    auto& list = plater.get_partplate_list();
    if (list.get_plate_count() != int(task.states.size())) return false;
    const auto& current_stages = list.stages();
    if (current_stages.size() != task.stages_snapshot.size()) return false;
    for (size_t i = 0; i < current_stages.size(); ++i) {
        const auto& a = current_stages[i];
        const auto& b = task.stages_snapshot[i];
        if (a.id != b.id || a.name != b.name || !a.position.isApprox(b.position, 1e-9) ||
            a.frame.defined != b.frame.defined ||
            (a.frame.defined && (!a.frame.min.isApprox(b.frame.min, 1e-9) ||
                                 !a.frame.max.isApprox(b.frame.max, 1e-9)))) return false;
    }
    for (int i = 0; i < list.get_plate_count(); ++i) {
        auto* plate = list.get_plate(i);
        const auto& state = task.states[i];
        if (plate->id() != state.id || plate->get_origin() != state.origin ||
            plate->stage_id() != state.stage_id || plate->is_locked() != state.locked ||
            plate->print_setup().destination != state.destination ||
            plate->print_setup().config != state.setup || *plate->config() != state.overrides) return false;
    }
    return true;
}

// Resolves source settings on another saved printer. Each call works on its own
// copy of presets so earlier resolutions cannot influence later ones; the
// optimize job calls it off the main thread with presets it owns.
std::optional<DynamicPrintConfig> printer_layout_config(const PresetBundle& presets,
                                                        const DynamicPrintConfig& source,
                                                        const SavedPrinterChoice& printer)
{
    PresetBundle bundle(presets);
    PlatePrintSetup source_setup;
    source_setup.config = source;
    source_setup.activate(bundle, "Staged layout preview");
    auto* preset = bundle.printers.find_preset(printer.preset);
    if (!preset || preset->printer_technology() != ptFFF) return std::nullopt;
    preset->is_visible = true;
    bundle.printers.select_preset_by_name(printer.preset, true);
    if (bundle.printers.get_selected_preset_name() != printer.preset) return std::nullopt;
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    auto target = bundle.full_config_secure();
    if (target.opt_string("printer_settings_id") != printer.preset) return std::nullopt;
    return target;
}

std::string config_string(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionString>(key);
    return option ? option->value : std::string{};
}

std::vector<int> used_instance_filaments(ModelInstance* instance, const DynamicPrintConfig& config)
{
    const auto used = get_instance_arrange_poly(instance, config).extrude_ids;
    std::set<int> slots(used.begin(), used.end());
    if (slots.empty()) return {};
    for (const char* option : {"filament_type", "filament_colour"}) {
        const auto* values = config.option<ConfigOptionStrings>(option);
        if (!values) return {};
        for (int slot : slots)
            if (slot <= 0 || size_t(slot) > values->values.size() || values->values[size_t(slot - 1)].empty())
                return {};
    }
    return std::vector<int>(slots.begin(), slots.end());
}

bool prepare_printer_bed(const DynamicPrintConfig& config, PlateLayoutInput& input)
{
    if (config.opt_bool("belt_printer") || config.opt_bool("is_imex") ||
        config.opt_bool("enable_wrapping_detection") || config.opt_bool("scan_first_layer")) return false;
    const auto* area = config.option<ConfigOptionPoints>("printable_area");
    if (!area || area->values.size() < 3) return false;
    Polygon bed;
    for (const auto& point : area->values) bed.points.push_back(scaled(point));
    bed.make_counter_clockwise();
    input.bed = {ExPolygon(bed)};
    if (const auto* zones = config.option<ConfigOptionPointsGroups>("extruder_printable_area"))
        for (const auto& zone : zones->values) {
            if (zone.size() < 3) continue;
            Polygon polygon;
            for (const auto& point : zone) polygon.points.push_back(scaled(point));
            polygon.make_counter_clockwise();
            input.bed = intersection_ex(input.bed, ExPolygons{ExPolygon(polygon)});
        }
    if (const auto* excluded = config.option<ConfigOptionPoints>("bed_exclude_area"))
        for (size_t i = 0; i + 3 < excluded->values.size(); i += 4) {
            Polygon polygon;
            for (size_t j = i; j < i + 4; ++j) polygon.points.push_back(scaled(excluded->values[j]));
            polygon.make_counter_clockwise();
            input.excluded.emplace_back(std::move(polygon));
        }
    const double skirt = get_real_skirt_dist(config);
    if (skirt > 0.) input.bed = offset_ex(input.bed, -scaled(skirt));
    return !input.bed.empty();
}

double plate_layout_load(const PlateLayoutInput& layout)
{
    double area = 0.;
    for (const auto& shape : layout.shapes) {
        const auto size = get_extents(shape.footprint).size();
        area += double(size.x()) * double(size.y());
    }
    return area;
}

class LayoutPreview : public wxPanel {
public:
    LayoutPreview(wxWindow* parent, const PlateLayoutInput& input, const PlateLayoutResult& result,
                  const wxSize& size = wxSize(440, 340)) : wxPanel(parent), m_input(input), m_result(result) {
        SetMinSize(FromDIP(size));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &LayoutPreview::paint, this);
    }
private:
    const PlateLayoutInput& m_input;
    const PlateLayoutResult& m_result;
    void paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#F5F5F3"))));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc) return;
        const auto bounds = get_extents(m_input.bed);
        const Vec2d size = bounds.size().cast<double>() * SCALING_FACTOR;
        if (size.minCoeff() <= 0.) return;
        const double margin = FromDIP(16);
        const double scale = std::min((GetClientSize().x - 2 * margin) / size.x(), (GetClientSize().y - 2 * margin) / size.y());
        const Vec2d center = bounds.center().cast<double>() * SCALING_FACTOR;
        auto draw = [&](const ExPolygons& polygons, wxColour color) {
            auto path = gc->CreatePath();
            auto contour = [&](const Polygon& polygon) {
                bool first = true;
                for (const auto& point : polygon.points) {
                    const double x = GetClientSize().x / 2. + (unscale<double>(point.x()) - center.x()) * scale;
                    const double y = GetClientSize().y / 2. - (unscale<double>(point.y()) - center.y()) * scale;
                    if (first) { path.MoveToPoint(x, y); first = false; } else path.AddLineToPoint(x, y);
                }
                path.CloseSubpath();
            };
            for (const auto& polygon : polygons) { contour(polygon.contour); for (const auto& hole : polygon.holes) contour(hole); }
            gc->SetBrush(wxBrush(StateColor::darkModeColorFor(color)));
            gc->SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#737373")), 1));
            gc->DrawPath(path, wxODDEVEN_RULE);
        };
        draw(m_input.bed, wxColour("#FFFFFF"));
        draw(m_input.excluded, wxColour("#B8B8B8"));
        for (const auto& shape : m_input.fixed) draw(shape.footprint, wxColour("#B8B8B8"));
        for (const auto& placement : m_result.placements)
            draw(layout_footprint(m_input.shapes[placement.source], placement), wxColour("#EC705B"));
    }
};

struct WorkspacePreviewBed {
    int number;
    BoundingBoxf bounds;
    bool moving;
};

class WorkspaceArrangePreview : public wxPanel {
public:
    WorkspaceArrangePreview(wxWindow* parent, std::vector<WorkspacePreviewBed> beds)
        : wxPanel(parent), m_beds(std::move(beds)) {
        SetMinSize(FromDIP(wxSize(440, 180)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WorkspaceArrangePreview::paint, this);
    }
private:
    std::vector<WorkspacePreviewBed> m_beds;
    void paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#F5F5F3"))));
        dc.Clear();
        BoundingBoxf workspace;
        for (const auto& bed : m_beds) workspace.merge(bed.bounds);
        if (!workspace.defined || workspace.size().minCoeff() <= 0.) return;
        const double margin = FromDIP(18);
        const double scale = std::min((GetClientSize().x - 2 * margin) / workspace.size().x(),
                                      (GetClientSize().y - 2 * margin) / workspace.size().y());
        for (const auto& bed : m_beds) {
            const auto& box = bed.bounds;
            const int x = int(margin + (box.min.x() - workspace.min.x()) * scale);
            const int y = int(margin + (workspace.max.y() - box.max.y()) * scale);
            const int width = std::max(1, int(box.size().x() * scale));
            const int height = std::max(1, int(box.size().y() * scale));
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#737373"))));
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour(bed.moving ? "#FF7B7B" : "#B8B8B8"))));
            dc.DrawRectangle(x, y, width, height);
            const wxString number = bed.number > 0 ? wxString::Format("%d", bed.number) : _L("New");
            const auto text_size = dc.GetTextExtent(number);
            if (width > text_size.x + FromDIP(8) && height > text_size.y + FromDIP(8)) {
                dc.SetTextForeground(*wxBLACK);
                dc.DrawText(number, x + (width - text_size.x) / 2, y + (height - text_size.y) / 2);
            }
        }
    }
};

void apply_layout(Plater& plater, const LayoutTask& task)
{
    Plater::TakeSnapshot undo(&plater, task.input.fill ? _u8L("Fill plate") : _u8L("Arrange plate"));
    auto& model = plater.model();
    auto* list = plater.sidebar().obj_list();
    auto& plates = plater.get_partplate_list();
    std::set<size_t> changed_objects;
    // Rearranging moves each selected original to its first placement; the rest are copies.
    std::vector<bool> moved(task.sources.size(), false);
    for (size_t i = 0; i < task.result.placements.size(); ++i) {
        const auto& placement = task.result.placements[i];
        const auto key = task.sources[placement.source];
        auto* object = model.objects[key.first];
        const auto* original = task.snapshot.objects[key.first]->instances[key.second];
        size_t object_index = key.first;
        size_t instance_index = key.second;
        ModelInstance* instance;
        const bool move_original = !task.input.fill || (task.rearrange && !moved[placement.source]);
        if (move_original) {
            moved[placement.source] = true;
            instance = object->instances[key.second];
        } else if (task.linked) {
            instance = object->add_instance(*original);
            instance_index = object->instances.size() - 1;
        } else {
            object = model.add_object(*task.snapshot.objects[key.first]);
            object->clear_instances();
            instance = object->add_instance(*original);
            object_index = model.objects.size() - 1;
            instance_index = 0;
            list->add_object_to_list(object_index, true, true, false);
        }
        // A copy is a new part to produce, not another count of its original.
        if (!move_original) instance->production_id.clear();
        Vec3d offset = original->get_offset();
        offset.x() = placement.offset.x() + task.origin.x();
        offset.y() = placement.offset.y() + task.origin.y();
        instance->set_offset(offset);
        Vec3d rotation = original->get_rotation();
        rotation.z() += placement.rotation;
        instance->set_rotation(rotation);
        object->invalidate_bounding_box();
        plates.notify_instance_update(object_index, instance_index, !move_original);
        changed_objects.insert(object_index);
    }
    for (size_t index : changed_objects) {
        if (index < task.snapshot.objects.size()) {
            const size_t previous = task.snapshot.objects[index]->instances.size();
            const size_t added = model.objects[index]->instances.size() - previous;
            if (added) list->increase_object_instances(index, previous == 1 ? added + 1 : added);
        }
        list->update_info_items(index);
    }
    plater.update();
    list->update_plate_values_for_items();
    plater.mark_plate_toolbar_image_dirty();
}

void review_layout(Plater& plater, const std::shared_ptr<LayoutTask>& task)
{
    if (!unchanged(plater, *task)) { show_info(&plater, _L("The plate changed. Create a new layout preview.")); return; }
    const size_t originals = task->input.fill && task->rearrange ? task->sources.size() : 0;
    if (task->result.placements.size() <= originals ||
        (!task->input.fill && task->result.placements.size() != task->sources.size())) {
        show_info(&plater, task->input.fill ? _L("No additional copies fit at this spacing.") : _L("These objects do not all fit on this plate. Try a smaller gap or allow rotation."));
        return;
    }
    LayoutDialog dialog(task->input.fill ? _L("Fill plate preview") : _L("Arrange plate preview"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const wxString count = task->input.fill ? format_wxstr(_L("Add %1% copies"), task->result.placements.size() - originals) :
                                            format_wxstr(_L("Arrange %1% objects"), task->result.placements.size());
    sizer->Add(new wxStaticText(&dialog, wxID_ANY, count), 0, wxALL, dialog.FromDIP(12));
    sizer->Add(new LayoutPreview(&dialog, task->input, task->result), 1, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(12));
    double clearance = task->input.gap;
    for (const auto& shape : task->input.shapes) clearance = std::max(clearance, 2 * shape.clearance);
    sizer->Add(new wxStaticText(&dialog, wxID_ANY, format_wxstr(_L("Minimum gap: %1% mm"), wxString::Format("%.2f", clearance))), 0, wxALL, dialog.FromDIP(12));
    if (task->result.search_limited)
        sizer->Add(new wxStaticText(&dialog, wxID_ANY, _L("Search limit reached. This preview may leave room for more copies.")), 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* buttons = new DialogButtons(&dialog, {"Apply", "Cancel"});
    buttons->GetAPPLY()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND);
    dialog.finish(sizer);
    if (dialog.ShowModal() == wxID_OK) {
        if (unchanged(plater, *task)) apply_layout(plater, *task);
        else show_info(&plater, _L("The plate changed. Create a new layout preview."));
    }
}

bool plan_printer_stages(Plater& plater, WorkspaceLayoutTask& task, std::vector<WorkspacePreviewBed>& preview)
{
    auto& plates = plater.get_partplate_list();
    std::vector<size_t> jobs;
    for (size_t i = 0; i < task.plates.size(); ++i) {
        const auto& plate = task.plates[i];
        if (!plate.removed && !plate.references.empty() && plate.wave > 0) jobs.push_back(i);
    }
    if (jobs.empty()) return false;

    double right = 0.;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        right = std::max(right, plates.get_plate(i)->get_bounding_box().max.x());
    for (const auto& stage : plates.stages())
        right = std::max(right, plates.stage_bounds(stage.id).max.x());
    right += 220.;
    std::map<int, std::vector<size_t>> waves;
    for (size_t i : jobs) waves[task.plates[i].wave].push_back(i);
    double y = 0.;
    double stage_x = right;
    double row_height = 0.;
    for (const auto& [wave, members] : waves) {
        double stage_width = 140.;
        double height = 0.;
        for (size_t i : members) {
            const auto& plate = task.plates[i];
            const auto* area = plate.config.option<ConfigOptionPoints>("printable_area");
            if (!area || area->values.size() < 3) return false;
            const BoundingBoxf box(area->values);
            stage_width += box.size().x() + 180.;
            height = std::max(height, box.size().y() + 100.);
        }
        stage_width = std::max(400., stage_width);
        if (stage_x > right && stage_x + stage_width > right + 1700.) {
            stage_x = right;
            y -= row_height + 260.;
            row_height = 0.;
        }
        task.stage_anchors[wave] = Vec2d(stage_x, y - 60.);
        double x = stage_x + 60.;
        for (size_t i : members) {
            auto& plate = task.plates[i];
            const auto* area = plate.config.option<ConfigOptionPoints>("printable_area");
            const BoundingBoxf box(area->values);
            plate.planned_origin = Vec2d(x, y) - box.min;
            const BoundingBoxf bounds(box.min + plate.planned_origin, box.max + plate.planned_origin);
            if (bounds.min.cwiseAbs().maxCoeff() > 99000. || bounds.max.cwiseAbs().maxCoeff() > 99000.)
                return false;
            preview.push_back({plate.index + 1, bounds, true});
            x += bounds.size().x() + 180.;
        }
        stage_x += stage_width + 120.;
        row_height = std::max(row_height, height);
    }
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        if (std::any_of(task.plates.begin(), task.plates.end(), [i](const WorkspacePlateTask& plate) {
                return plate.index == i && (plate.removed || plate.wave > 0);
            })) continue;
        const auto& box = plates.get_plate(i)->get_bounding_box();
        preview.push_back({i + 1, BoundingBoxf(box.min.head<2>(), box.max.head<2>()), false});
    }
    return true;
}

void apply_workspace_layout(Plater& plater, const WorkspaceLayoutTask& task)
{
    Plater::TakeSnapshot undo(&plater, _u8L("Optimize plates"));
    auto& model = plater.model();
    auto& plates = plater.get_partplate_list();
    auto* list = plater.sidebar().obj_list();
    std::vector<int> indices(task.plates.size(), -1);
    std::vector<Vec3d> origins(task.plates.size());
    for (size_t i = 0; i < task.plates.size(); ++i) {
        const auto& plate = task.plates[i];
        if (plate.removed || (plate.index < 0 && plate.references.empty())) continue;
        indices[i] = plate.index >= 0 ? plate.index : plates.create_plate(false);
        if (indices[i] < 0) return; // The preview checked the temporary plate count.
        auto* target = plates.get_plate(indices[i]);
        if (!target) { assert(false); return; } // Indices come from the checked snapshot or a new plate.
        origins[i] = target->get_origin();
        if (plate.index >= 0 && (!task.utilize_printers || plate.wave == 0)) continue;
        auto& setup = target->print_setup();
        const bool name_follows_printer = target->get_plate_name().empty() ||
            target->get_plate_name() == setup.destination_name;
        // A job keeps its own setup, cascade and plate overrides. One moved to a
        // different printer takes that printer's resolved settings: its cascade
        // stays when the process preset stayed, and otherwise starts from the new
        // process preset so only real differences become plate edits.
        const auto& source = plate.process_setup;
        const bool reassigned = task.utilize_printers &&
            config_string(plate.config, "printer_settings_id") != config_string(source.config, "printer_settings_id");
        setup.config = reassigned ? plate.config : source.config;
        if (!reassigned || config_string(plate.config, "print_settings_id") == config_string(source.config, "print_settings_id")) {
            setup.process_base = source.process_base;
            setup.process_overrides = source.process_overrides;
        } else {
            setup.process_base.clear();
            setup.process_overrides.clear();
            const auto* preset = wxGetApp().preset_bundle->prints.find_preset(config_string(setup.config, "print_settings_id"));
            initialize_process_cascade(setup, preset ? preset->config : DynamicPrintConfig{}, plate.plate_overrides);
        }
        setup.process_scope_catalog.clear();
        setup.destination = task.utilize_printers ? plate.assigned_destination : plate.destination;
        setup.destination_name = plate.assigned_name;
        if (plate.index < 0 || plate.anchor_printer >= 0 || name_follows_printer)
            target->set_plate_name(plate.assigned_name);
        restore_scanned_printer(setup.destination, setup.config);
        *target->config() = plate.plate_overrides;
        const auto* area = setup.config.option<ConfigOptionPoints>("printable_area");
        if (area && area->values.size() >= 3) {
            // A setup from an older project can lack newer bed keys. Read their
            // defaults without adding them to the plate's saved setup.
            DynamicPrintConfig bed = DynamicPrintConfig::full_print_config();
            bed.apply(setup.config, true);
            const BoundingBoxf bounds(area->values);
            Vec3d position = target->get_origin();
            target->set_pos_and_size(position, int(std::ceil(bounds.size().x())), int(std::ceil(bounds.size().y())),
                                     bed.opt_float("printable_height"), false, false);
            target->set_shape(area->values,
                bed.option<ConfigOptionPoints>("bed_exclude_area")->values,
                bed.option<ConfigOptionPointsGroups>("extruder_printable_area")->values,
                bed.option<ConfigOptionFloatsNullable>("extruder_printable_height")->values,
                position.head<2>(), bed.opt_float("extruder_clearance_height_to_lid"),
                bed.opt_float("extruder_clearance_height_to_rod"));
        }
        if (plate.index < 0) {
            plates.move_plate(indices[i], plate.planned_origin);
            origins[i] = target->get_origin();
            if (!task.utilize_printers && !plate.stage_id.empty())
                plates.set_plate_stage(indices[i], plate.stage_id);
        }
    }
    std::set<size_t> changed_objects;
    for (size_t i = 0; i < task.plates.size(); ++i) {
        const auto& plate = task.plates[i];
        if (!plate.changed || plate.removed) continue;
        for (const auto& placement : plate.result.placements) {
            const auto& ref = plate.references[placement.source];
            const auto key = task.plates[ref.plate].sources[ref.shape];
            const auto* original = task.snapshot.objects[key.first]->instances[key.second];
            auto* object = model.objects[key.first];
            auto* instance = object->instances[key.second];
            Vec3d offset = original->get_offset();
            offset.x() = placement.offset.x() + origins[i].x();
            offset.y() = placement.offset.y() + origins[i].y();
            if ((instance->get_offset() - offset).norm() <= 1e-7 && std::abs(placement.rotation) <= 1e-7) continue;
            instance->set_offset(offset);
            Vec3d rotation = original->get_rotation();
            rotation.z() += placement.rotation;
            instance->set_rotation(rotation);
            object->invalidate_bounding_box();
            plates.notify_instance_update(key.first, key.second);
            changed_objects.insert(key.first);
        }
    }
    for (size_t index : changed_objects) list->update_info_items(index);
    std::vector<int> removed;
    for (const auto& plate : task.plates)
        if (plate.removed && plate.index >= 0) removed.push_back(plate.index);
    std::sort(removed.begin(), removed.end());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    // Removal keeps every plate when any of them still holds parts. Indices then
    // stay valid, but the planned stages no longer match the workspace.
    const std::vector<int> removed_now = plater.remove_empty_plates_in_current_snapshot(removed);
    const bool removal_complete = removed_now.size() == removed.size();
    if (task.utilize_printers && removal_complete) {
        for (size_t i = 0; i < task.plates.size(); ++i) {
            const auto& plan = task.plates[i];
            if (plan.wave == 0 || indices[i] < 0) continue;
            // A planned job's own plate is never one of the emptied plates.
            const int planned = indices[i];
            indices[i] = std::find(removed_now.begin(), removed_now.end(), planned) != removed_now.end() ? -1 :
                planned - int(std::count_if(removed_now.begin(), removed_now.end(),
                                            [planned](int removed_index) { return removed_index < planned; }));
            if (indices[i] < 0 || indices[i] >= plates.get_plate_count()) {
                assert(false);
                indices[i] = -1;
                continue;
            }
            auto* plate = plates.get_plate(indices[i]);
            if (!plate->stage_id().empty()) plates.set_plate_stage(indices[i], "");
        }
        for (size_t i = 0; i < task.plates.size(); ++i) {
            if (task.plates[i].wave == 0 || indices[i] < 0) continue;
            plates.move_plate(indices[i], task.plates[i].planned_origin);
        }
        for (const auto& [wave, anchor] : task.stage_anchors) {
            const auto name = into_u8(format_wxstr(_L("Stage %1%"), task.stages_snapshot.size() + wave));
            const auto stage = plates.create_stage(name, anchor);
            if (stage.empty()) continue;
            std::vector<int> members;
            for (size_t i = 0; i < task.plates.size(); ++i)
                if (task.plates[i].wave == wave && indices[i] >= 0) members.push_back(indices[i]);
            plates.set_plates_stage(members, stage);
        }
    }
    plater.activate_plate_setup();
    if (task.arrange_after_optimization)
        plates.arrange_workspace_plates();
    plater.update();
    list->update_plate_values_for_items();
    plater.mark_plate_toolbar_image_dirty();
    if (task.arrange_after_optimization || task.utilize_printers ||
        std::any_of(task.plates.begin(), task.plates.end(), [](const auto& plate) { return plate.index < 0 && !plate.removed; }))
        if (auto* canvas = plater.get_view3D_canvas3D())
            canvas->zoom_to_plate(REQUIRES_ZOOM_TO_ALL_PLATE);
    if (!removal_complete)
        show_info(&plater, task.utilize_printers
            ? _L("Some plates still hold parts, so they were kept and the planned stages were not created. Use Undo to restore the previous layout.")
            : _L("Some plates still hold parts, so they were kept. Use Undo to restore the previous layout."));
}

void review_workspace_layout(Plater& plater, const std::shared_ptr<WorkspaceLayoutTask>& task)
{
    if (!workspace_unchanged(plater, *task)) {
        show_info(&plater, _L("The workspace changed. Create a new preview."));
        return;
    }
    if (task->unprepared_plate >= 0) {
        show_info(&plater, format_wxstr(_L("No saved printer can safely prepare Plate %1%."), task->unprepared_plate + 1));
        return;
    }
    const wxString too_long = _L("Finding a layout took too long. Optimize one stage or fewer plates at a time, then try again.");
    if (task->out_of_time && (task->pieces_do_not_fit || task->unplanned_plate >= 0)) {
        show_info(&plater, too_long);
        return;
    }
    if (task->pieces_do_not_fit) {
        show_info(&plater, _L("Some pieces cannot fit on their assigned printer at this gap. Try a smaller gap or allow rotation, or choose a larger printer for those pieces."));
        return;
    }
    if (task->unplanned_plate >= 0) {
        show_info(&plater, format_wxstr(_L("Plate %1% could not fit on a compatible saved printer at this gap."),
                                      task->unplanned_plate + 1));
        return;
    }
    const size_t freed = std::count_if(task->plates.begin(), task->plates.end(),
        [](const WorkspacePlateTask& plate) { return plate.removed && plate.index >= 0; });
    const size_t added = std::count_if(task->plates.begin(), task->plates.end(),
        [](const WorkspacePlateTask& plate) { return !plate.removed && plate.index < 0 && !plate.references.empty(); });
    if (task->states.size() + added > max_project_plates) {
        show_info(&plater, _L("This layout needs more plates than the project can hold. Optimize a smaller selection of plates."));
        return;
    }
    if (!task->utilize_printers) {
        double right = 0.;
        for (int i = 0; i < plater.get_partplate_list().get_plate_count(); ++i)
            right = std::max(right, plater.get_partplate_list().get_plate(i)->get_bounding_box().max.x());
        const double start = right + 80.;
        double x = start, y = 0., row_height = 0.;
        for (auto& plate : task->plates) {
            if (plate.removed || plate.index >= 0 || plate.references.empty()) continue;
            const auto bounds = get_extents(plate.input.bed);
            const double width = unscale<double>(bounds.size().x());
            const double height = unscale<double>(bounds.size().y());
            if (x > start && x + width > start + 1700.) {
                x = start;
                y -= row_height + 80.;
                row_height = 0.;
            }
            plate.planned_origin = Vec2d(x - unscale<double>(bounds.min.x()), y - unscale<double>(bounds.min.y()));
            x += width + 80.;
            row_height = std::max(row_height, height);
            if (x > 99000. || std::abs(y) + height > 99000.) {
                show_info(&plater, _L("Move the existing plates closer together before adding more plates."));
                return;
            }
        }
    }
    const bool changed = std::any_of(task->plates.begin(), task->plates.end(),
        [](const auto& plate) { return plate.changed; });
    if (freed == 0 && !task->utilize_printers && !changed) {
        const bool compatible = std::any_of(task->plates.begin(), task->plates.end(), [&](const auto& plate) {
            return std::count_if(task->plates.begin(), task->plates.end(),
                [&](const auto& other) { return plate.group == other.group; }) > 1;
        });
        wxString message;
        if (!compatible) {
            message = _L("These plates cannot be combined without changing a job's printer, print settings, or assigned filaments. Review the plate settings.");
            if (task->stage_id.empty())
                message += "\n\n" + _L("To plan across different printers, turn on Plan across saved printers in stages.");
        } else if (task->out_of_time) {
            message = too_long;
        } else if (task->search_limited) {
            message = _L("The search stopped before it could check every placement. Try a smaller gap or move a few objects closer together.");
        } else {
            message = _L("No complete plate fits onto the other compatible plates at this gap. Try a smaller gap or allow rotation.");
        }
        if (task->unassigned_instances > 0)
            message += "\n\n" + format_wxstr(_L_PLURAL("%1% object is outside all plates and was not included. Move it onto a plate to include it.",
                "%1% objects are outside all plates and were not included. Move them onto plates to include them.", task->unassigned_instances), task->unassigned_instances);
        show_info(&plater, message);
        return;
    }

    std::vector<WorkspacePreviewBed> workspace_beds;
    if (task->utilize_printers) {
        if (!plan_printer_stages(plater, *task, workspace_beds)) {
            show_info(&plater, _L("The planned stages do not fit beside the existing plates. Move the plates closer together, then try again."));
            return;
        }
    } else if (task->arrange_after_optimization) {
        std::vector<int> removed;
        for (const auto& plate : task->plates)
            if (plate.removed) removed.push_back(plate.index);
        auto& plates = plater.get_partplate_list();
        task->workspace_arrangement = plates.workspace_arrangement(removed);
        if (task->workspace_arrangement.empty()) {
            show_info(&plater, _L("The remaining plates cannot be arranged. Turn off Arrange plates after optimization to continue."));
            return;
        }
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            if (std::find(removed.begin(), removed.end(), i) != removed.end()) continue;
            auto* plate = plates.get_plate(i);
            const auto& box = plate->get_bounding_box();
            BoundingBoxf bounds(box.min.head<2>(), box.max.head<2>());
            const auto target = std::find_if(task->workspace_arrangement.begin(), task->workspace_arrangement.end(),
                [i](const auto& entry) { return entry.first == i; });
            const bool moving = target != task->workspace_arrangement.end();
            if (moving) {
                const Vec2d delta = target->second - plate->get_origin().head<2>();
                bounds = BoundingBoxf(bounds.min + delta, bounds.max + delta);
            }
            workspace_beds.push_back({i + 1, bounds, moving});
        }
    }

    LayoutDialog dialog(task->stage_id.empty() ? _L("Optimize workspace preview") : _L("Optimize stage preview"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (task->utilize_printers) {
        const size_t jobs = std::count_if(task->plates.begin(), task->plates.end(),
            [](const WorkspacePlateTask& plate) { return plate.wave > 0; });
        sizer->Add(new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L("Prepare %1% plates in %2% stages"), jobs, task->stage_anchors.size())),
            0, wxALL, dialog.FromDIP(12));
        std::set<std::string> used_printers;
        for (const auto& plate : task->plates)
            if (plate.wave > 0) used_printers.insert(plate.assigned_destination);
        if (used_printers.size() < task->available_printers.size())
            sizer->Add(new wxStaticText(&dialog, wxID_ANY,
                format_wxstr(_L("This plan uses %1% of %2% saved printers. Other printers may need more room or a compatible material."),
                             used_printers.size(), task->available_printers.size())),
                0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        auto* assignments = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxVSCROLL | wxTAB_TRAVERSAL);
        assignments->SetScrollRate(0, dialog.FromDIP(12));
        assignments->SetMinSize(dialog.FromDIP(wxSize(470, 110)));
        auto* assignment_rows = new wxBoxSizer(wxVERTICAL);
        std::vector<const WorkspacePlateTask*> ordered_jobs;
        for (const auto& plate : task->plates)
            if (plate.wave > 0) ordered_jobs.push_back(&plate);
        std::stable_sort(ordered_jobs.begin(), ordered_jobs.end(), [](const auto* a, const auto* b) {
            return a->wave < b->wave || (a->wave == b->wave && a->assigned_name < b->assigned_name);
        });
        for (const auto* plate : ordered_jobs) {
            const size_t stage = task->stages_snapshot.size() + plate->wave;
            const wxString printer = from_u8(plate->assigned_name);
            const wxString assignment = plate->index < 0
                ? format_wxstr(_L("Stage %1%: print a new plate on %2%"), stage, printer)
                : format_wxstr(_L("Stage %1%: print Plate %2% on %3%"), stage, plate->index + 1, printer);
            assignment_rows->Add(new wxStaticText(assignments, wxID_ANY, assignment),
                0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(8));
        }
        assignments->SetSizer(assignment_rows);
        assignments->FitInside();
        sizer->Add(assignments, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    } else {
        const size_t jobs = std::count_if(task->plates.begin(), task->plates.end(),
            [](const auto& plate) { return !plate.removed && !plate.references.empty(); });
        sizer->Add(new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L_PLURAL("Pack pieces onto %1% plate", "Pack pieces onto %1% plates", jobs), jobs)),
            0, wxALL, dialog.FromDIP(12));
        if (added > 0)
            sizer->Add(new wxStaticText(&dialog, wxID_ANY,
                format_wxstr(_L_PLURAL("Add %1% plate for pieces that need more room.", "Add %1% plates for pieces that need more room.", added), added)),
                0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        if (freed > 0)
            sizer->Add(new wxStaticText(&dialog, wxID_ANY,
                format_wxstr(_L_PLURAL("Remove %1% emptied plate.", "Remove %1% emptied plates.", freed), freed)),
                0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    if (task->included_unassigned > 0) {
        auto* included = new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L_PLURAL("Pack %1% piece from outside plates using Plate %2%'s printer and filament settings.",
                "Pack %1% pieces from outside plates using Plate %2%'s printer and filament settings.", task->included_unassigned),
                task->included_unassigned, task->unassigned_setup_plate + 1));
        included->Wrap(dialog.FromDIP(440));
        sizer->Add(included, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    if (task->unassigned_instances > 0) {
        auto* omitted = new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L_PLURAL("%1% object is outside all plates and will stay in place.",
                                   "%1% objects are outside all plates and will stay in place.", task->unassigned_instances), task->unassigned_instances));
        omitted->Wrap(dialog.FromDIP(440));
        sizer->Add(omitted, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    if (task->arrange_after_optimization || task->utilize_printers) {
        sizer->Add(new wxStaticText(&dialog, wxID_ANY, _L("Workspace after optimization")),
            0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        sizer->Add(new WorkspaceArrangePreview(&dialog, std::move(workspace_beds)),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    if (std::any_of(task->plates.begin(), task->plates.end(),
        [](const WorkspacePlateTask& plate) { return plate.changed && !plate.removed; })) {
        auto* scroll = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
        scroll->SetScrollRate(0, dialog.FromDIP(12));
        scroll->SetMinSize(dialog.FromDIP(wxSize(470, 380)));
        auto* content = new wxBoxSizer(wxVERTICAL);
        for (const auto& plate : task->plates) {
            if (!plate.changed || plate.removed) continue;
            content->Add(new wxStaticText(scroll, wxID_ANY,
                plate.index < 0 ? format_wxstr(_L_PLURAL("New plate · %1% object", "New plate · %1% objects", plate.references.size()),
                                               plate.references.size()) :
                format_wxstr(_L_PLURAL("Plate %1% · %2% object", "Plate %1% · %2% objects", plate.references.size()),
                             plate.index + 1, plate.references.size())),
                0, wxTOP | wxLEFT | wxRIGHT, dialog.FromDIP(12));
            content->Add(new LayoutPreview(scroll, plate.input, plate.result, wxSize(440, 220)),
                0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        }
        scroll->SetSizer(content);
        scroll->FitInside();
        sizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(12));
    }
    sizer->Add(new wxStaticText(&dialog, wxID_ANY, task->utilize_printers
        ? _L("Check each printer's material and print quality before printing. Stages organize the jobs; they do not start prints.")
        : _L("Objects move only between plates with matching print setups. Their bed-facing side stays the same.")),
        0, wxALL, dialog.FromDIP(12));
    if (task->search_limited)
        sizer->Add(new wxStaticText(&dialog, wxID_ANY, _L("The search stopped early; another layout may fit more.")),
            0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    if (!task->favor_stages && task->alternate_jobs > 0 &&
        task->alternate_waves > 0 && task->alternate_waves < int(task->stage_anchors.size()))
        sizer->Add(new wxStaticText(&dialog, wxID_ANY,
            format_wxstr(_L("A different layout found %1% plates in %2% stages; this preview keeps fewer plates."),
                         task->alternate_jobs, task->alternate_waves)),
            0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    if (task->favor_stages) {
        const size_t jobs = std::count_if(task->plates.begin(), task->plates.end(),
            [](const WorkspacePlateTask& plate) { return plate.wave > 0; });
        if (jobs > task->baseline_jobs)
            sizer->Add(new wxStaticText(&dialog, wxID_ANY,
                format_wxstr(_L_PLURAL("Use %1% extra plate to finish in fewer stages.",
                                       "Use %1% extra plates to finish in fewer stages.", jobs - task->baseline_jobs),
                             jobs - task->baseline_jobs)),
                0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    auto* buttons = new DialogButtons(&dialog, {"Apply", "Cancel"});
    buttons->GetAPPLY()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND);
    dialog.finish(sizer);
    if (dialog.ShowModal() == wxID_OK) {
        const auto saved = saved_printer_choices();
        const bool printers_unchanged = !task->utilize_printers ||
            std::all_of(task->available_printers.begin(), task->available_printers.end(), [&](const auto& printer) {
                return std::any_of(saved.begin(), saved.end(), [&](const auto& current) {
                    return current.destination == printer.destination && current.preset == printer.preset;
                });
            });
        if (workspace_unchanged(plater, *task) && printers_unchanged)
            apply_workspace_layout(plater, *task);
        else show_info(&plater, _L("The workspace changed. Create a new preview."));
    }
}
} // namespace

bool can_align_selection(Plater& plater)
{
    return editable_plate(plater) && on_active_plate(plater, selected_instances(plater));
}
bool can_fill_plate(Plater& plater)
{
    if (plater.printer_technology() == ptSLA)
        return plater.get_ui_job_worker().is_idle() && !plater.is_background_process_slicing() && plater.can_increase_instances();
    const auto keys = selected_instances(plater);
    return editable_plate(plater) && on_active_plate(plater, keys) &&
        std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return plater.model().objects[key.first]->instances[key.second]->printable; });
}

void show_alignment_dialog(Plater& plater)
{
    if (!can_align_selection(plater)) { show_info(&plater, _L("Select whole objects on the active, unlocked plate to align them.")); return; }
    const auto keys = selected_instances(plater);
    LayoutDialog dialog(_L("Align objects"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(dialog.FromDIP(12));
    auto* reference = choices(&dialog, {_L("Selection"), _L("Plate"), _L("Key object")});
    if (keys.size() == 1) reference->SetSelection(1);
    std::vector<wxString> names;
    for (const auto& key : keys) names.push_back(format_wxstr(_L("%1% · copy %2%"), from_u8(plater.model().objects[key.first]->name), key.second + 1));
    auto* key_object = choices(&dialog, names);
    row(&dialog, sizer, _L("Align to"), reference);
    row(&dialog, sizer, _L("Key object"), key_object);
    key_object->Disable();
    reference->Bind(wxEVT_COMBOBOX, [reference, key_object](wxCommandEvent& e) { key_object->Enable(reference->GetSelection() == 2); e.Skip(); });
    auto* gap = new ::TextInput(&dialog, "3", "mm", "", wxDefaultPosition, dialog.FromDIP(wxSize(150, -1)));
    auto apply = [&](int dimension, AlignmentAction action) {
        double spacing = 0.;
        if (action == AlignmentAction::SetGap && !read_gap(gap, spacing)) { gap->GetTextCtrl()->SetFocus(); return; }
        if (!can_align_selection(plater) || selected_instances(plater) != keys) { dialog.EndModal(wxID_CANCEL); return; }
        std::vector<AlignmentInterval> intervals;
        for (const auto& key : keys) {
            const auto box = plater.model().objects[key.first]->instance_bounding_box(key.second);
            intervals.push_back({box.min[dimension], box.max[dimension]});
        }
        AlignmentInterval selection = intervals.front();
        for (const auto& interval : intervals) { selection.min = std::min(selection.min, interval.min); selection.max = std::max(selection.max, interval.max); }
        AlignmentInterval bounds = selection;
        if (reference->GetSelection() == 1) {
            const auto box = get_extents(plater.get_partplate_list().get_curr_plate()->get_shape());
            bounds = {box.min[dimension], box.max[dimension]};
        } else if (reference->GetSelection() == 2) bounds = intervals[key_object->GetSelection()];
        auto shifts = alignment_offsets(intervals, action, bounds, spacing);
        if (std::any_of(shifts.begin(), shifts.end(), [](double value) { return std::abs(value) > 1e-8; })) {
            Plater::TakeSnapshot undo(&plater, _u8L("Align objects"));
            for (size_t i = 0; i < keys.size(); ++i) {
                if (std::abs(shifts[i]) <= 1e-8) continue;
                Vec3d shift = Vec3d::Zero();
                shift[dimension] = shifts[i];
                plater.model().objects[keys[i].first]->translate_instance(keys[i].second, shift);
                plater.get_partplate_list().notify_instance_update(keys[i].first, keys[i].second);
            }
            plater.update();
            plater.sidebar().obj_list()->update_plate_values_for_items();
        }
        dialog.EndModal(wxID_OK);
    };
    auto* alignment = new wxGridSizer(2, 3, dialog.FromDIP(8), dialog.FromDIP(8));
    auto add_action = [&](wxSizer* target, const wxString& label, int dimension, AlignmentAction action, size_t minimum) {
        auto* button = new Button(&dialog, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->Enable(keys.size() >= minimum);
        button->Bind(wxEVT_BUTTON, [&, dimension, action](wxCommandEvent&) { apply(dimension, action); });
        target->Add(button, 1, wxEXPAND);
    };
    add_action(alignment, _L("Align left"), 0, AlignmentAction::Minimum, 1);
    add_action(alignment, _L("Center horizontally"), 0, AlignmentAction::Center, 1);
    add_action(alignment, _L("Align right"), 0, AlignmentAction::Maximum, 1);
    add_action(alignment, _L("Align front"), 1, AlignmentAction::Minimum, 1);
    add_action(alignment, _L("Center vertically"), 1, AlignmentAction::Center, 1);
    add_action(alignment, _L("Align back"), 1, AlignmentAction::Maximum, 1);
    sizer->Add(alignment, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* distribute = new wxGridSizer(2, 2, dialog.FromDIP(8), dialog.FromDIP(8));
    add_action(distribute, _L("Distribute X centers"), 0, AlignmentAction::DistributeCenters, 3);
    add_action(distribute, _L("Distribute Y centers"), 1, AlignmentAction::DistributeCenters, 3);
    add_action(distribute, _L("Equal horizontal gaps"), 0, AlignmentAction::DistributeGaps, 3);
    add_action(distribute, _L("Equal vertical gaps"), 1, AlignmentAction::DistributeGaps, 3);
    sizer->Add(distribute, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    row(&dialog, sizer, _L("Gap"), gap);
    auto* exact = new wxGridSizer(1, 2, dialog.FromDIP(8), dialog.FromDIP(8));
    add_action(exact, _L("Space horizontally"), 0, AlignmentAction::SetGap, 2);
    add_action(exact, _L("Space vertically"), 1, AlignmentAction::SetGap, 2);
    sizer->Add(exact, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* warning = new wxStaticText(&dialog, wxID_ANY, _L("Directions follow the bed, even when you rotate the view. Check for overlaps after aligning."));
    warning->Wrap(dialog.FromDIP(460));
    sizer->Add(warning, 0, wxALL, dialog.FromDIP(12));
    auto* buttons = new DialogButtons(&dialog, {"Cancel"});
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND);
    dialog.finish(sizer);
    dialog.ShowModal();
}

void show_plate_layout_dialog(Plater& plater, bool fill, bool linked_copies)
{
    if (!editable_plate(plater) || (fill && !can_fill_plate(plater))) {
        show_info(&plater, fill ? _L("Select whole objects on the active, unlocked plate to fill it.") :
                                  _L("Unlock the active plate and wait for slicing or the current task to finish, then arrange it."));
        return;
    }
    LayoutDialog dialog(fill ? _L("Fill plate") : _L("Arrange objects"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(dialog.FromDIP(12));
    auto* density = choices(&dialog, {_L("Spacious · 5 mm"), _L("Balanced · 3 mm"), _L("Tight · 1 mm"), _L("Max · 0.2 mm"), _L("Custom")});
    density->SetSelection(1);
    auto* gap = new ::TextInput(&dialog, "3", "mm", "", wxDefaultPosition, dialog.FromDIP(wxSize(150, -1)));
    auto* rotate = new ::CheckBox(&dialog);
    auto* rearrange = new ::CheckBox(&dialog);
    auto* scope = choices(&dialog, {_L("Selected objects"), _L("All objects on this plate")});
    if (!can_align_selection(plater)) scope->SetSelection(1);
    row(&dialog, sizer, _L("Density"), density);
    row(&dialog, sizer, _L("Gap"), gap);
    row(&dialog, sizer, _L("Allow rotating objects"), rotate);
    if (fill) row(&dialog, sizer, _L("Rearrange selected objects to fit more"), rearrange);
    else row(&dialog, sizer, _L("Arrange"), scope);
    if (fill) scope->Hide(); else rearrange->Hide();
    auto* help = new wxStaticText(&dialog, wxID_ANY, _L("Brims, supports and printer clearance may require more space. Rotation keeps the same face on the bed."));
    help->Wrap(dialog.FromDIP(440));
    sizer->Add(help, 0, wxALL, dialog.FromDIP(12));
    density->Bind(wxEVT_COMBOBOX, [density, gap](wxCommandEvent& e) {
        if (density->GetSelection() < 4) gap->GetTextCtrl()->ChangeValue(wxString::Format("%g", fill_density_gap(static_cast<FillDensity>(density->GetSelection()))));
        e.Skip();
    });
    gap->GetTextCtrl()->Bind(wxEVT_TEXT, [density](wxCommandEvent& e) { density->SetSelection(4); e.Skip(); });
    auto* validation = new wxStaticText(&dialog, wxID_ANY, wxEmptyString);
    sizer->Add(validation, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* buttons = new DialogButtons(&dialog, {L("Preview"), "Cancel"});
    buttons->GetFIRST()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        double value;
        if (!read_gap(gap, value) || value < 0.2) {
            validation->SetLabel(_L("Enter a gap between 0.2 and 1000 mm."));
            dialog.GetSizer()->SetSizeHints(&dialog);
            gap->GetTextCtrl()->SetFocus();
            return;
        }
        dialog.EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND);
    dialog.finish(sizer);
    if (dialog.ShowModal() != wxID_OK || !editable_plate(plater)) return;
    auto task = std::make_shared<LayoutTask>();
    task->snapshot = plater.model();
    task->config = wxGetApp().preset_bundle->full_config();
    task->editing_config = task->config;
    auto* plate = plater.get_partplate_list().get_curr_plate();
    task->plate_config = *plate->config();
    task->plate_index = plate->get_index();
    task->destination = plate->print_setup().destination;
    task->origin = plate->get_origin();
    task->linked = linked_copies;
    task->rearrange = fill && rearrange->GetValue();
    task->input.fill = fill;
    task->input.allow_rotation = rotate->GetValue();
    read_gap(gap, task->input.gap);
    task->sources = selected_instances(plater);
    if (!fill && scope->GetSelection() == 1) task->sources.clear();
    for (size_t o = 0; o < task->snapshot.objects.size(); ++o)
        for (size_t i = 0; i < task->snapshot.objects[o]->instances.size(); ++i)
            if (plate->contain_instance(o, i) || plate->intersect_instance(o, i)) {
                const InstanceKey key(o, i);
                if (!fill && scope->GetSelection() == 1 && plate->contain_instance(o, i) && task->snapshot.objects[o]->instances[i]->printable)
                    task->sources.push_back(key);
                const bool selected = std::find(task->sources.begin(), task->sources.end(), key) != task->sources.end();
                if (!selected || (fill && !task->rearrange)) task->fixed.push_back(key);
            }
    if (task->sources.empty() || !on_active_plate(plater, task->sources)) {
        show_info(&plater, fill ? _L("Select whole objects on the active, unlocked plate to fill it.") :
                           scope->GetSelection() == 1 ? _L("This plate has no printable objects to arrange.") :
                           _L("Select whole objects on the active plate to arrange them, or arrange all objects on this plate."));
        return;
    }
    task->config.apply(task->plate_config);
    task->config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(plate->get_real_print_seq()));
    // Preserve the active full config for the stale-result guard separately;
    // plate overrides are read by project_shape through this resolved snapshot.
    if (task->config.opt_bool("belt_printer")) {
        show_info(&plater, _L("Use the existing Arrange command for belt printers.")); return;
    }
    prepare_bed(plater, *plate, task->config, task->input);
    queue_job(plater.get_ui_job_worker(), [task](Job::Ctl& ctl) {
        const std::string status = _u8L("Finding a layout");
        ctl.update_status(0, status);
        for (const auto& key : task->sources) {
            if (ctl.was_canceled()) return;
            task->input.shapes.push_back(project_shape(*task, key, false, ctl));
        }
        for (const auto& key : task->fixed) {
            if (ctl.was_canceled()) return;
            task->input.fixed.push_back(project_shape(*task, key, true, ctl));
        }
        task->result = compute_plate_layout(task->input, [&ctl] { return ctl.was_canceled(); },
            [&ctl, &status](int value) { ctl.update_status(value, status); });
    }, [task](bool canceled, std::exception_ptr& error) {
        if (canceled || task->result.canceled || wxGetApp().is_closing()) return;
        if (error) {
            const wxString message = layout_failure_message(error, _L("Could not create a layout for these objects."));
            error = nullptr;
            show_info(wxGetApp().plater(), message);
            return;
        }
        wxWeakRef<Plater> owner(wxGetApp().plater());
        wxGetApp().CallAfter([task, owner] {
            if (owner && !wxGetApp().is_closing()) review_layout(*owner, task);
        });
    });
}

bool can_optimize_plates(Plater& plater, const std::string& stage_id)
{
    auto& plates = plater.get_partplate_list();
    if (plater.printer_technology() != ptFFF || !plates.independent_setups() ||
        plater.is_background_process_slicing() || !plater.get_ui_job_worker().is_idle()) return false;
    if (!stage_id.empty() && std::none_of(plates.stages().begin(), plates.stages().end(),
        [&](const WorkspaceStage& stage) { return stage.id == stage_id; })) return false;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        auto* plate = plates.get_plate(i);
        if ((stage_id.empty() || plate->stage_id() == stage_id) && !plate->is_locked() && !plate->empty())
            return true;
    }
    return stage_id.empty() && plates.get_curr_plate() && !plates.get_curr_plate()->is_locked() &&
           !pieces_outside_plates(plater).empty();
}

namespace {
struct OptimizeOptions
{
    std::string stage_id;
    double gap = 0.;
    bool rotate = true;
    bool arrange_after = false;
    bool utilize_printers = false;
    bool favor_stages = false;
    bool pack_outside = false;
    int outside_setup_plate = -1;
    std::vector<InstanceKey> outside;
};

// Asks how to optimize. Returns nothing when the person cancels or no plate can
// lend its settings to pieces outside the plates.
std::optional<OptimizeOptions> ask_optimize_options(Plater& plater, const std::string& stage_id)
{
    OptimizeOptions options;
    options.stage_id = stage_id;
    options.outside = stage_id.empty() ? pieces_outside_plates(plater) : std::vector<InstanceKey>{};
    std::vector<int> outside_setup_indices;
    std::vector<wxString> outside_setup_names;
    if (!options.outside.empty()) {
        auto& plates = plater.get_partplate_list();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            auto* plate = plates.get_plate(i);
            if (plate->is_locked() || plate->print_setup().empty()) continue;
            outside_setup_indices.push_back(i);
            outside_setup_names.push_back(format_wxstr(_L("Plate %1%: %2%"), i + 1,
                from_u8(plate->print_setup().destination_name.empty() ? plate->get_plate_name() : plate->print_setup().destination_name)));
        }
        if (outside_setup_names.empty()) {
            show_info(&plater, _L("To pack the pieces outside the plates, first choose printer settings for an unlocked plate."));
            return std::nullopt;
        }
    }
    LayoutDialog dialog(stage_id.empty() ? _L("Optimize plates") : _L("Optimize stage plates"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(dialog.FromDIP(12));
    auto* gap = new ::TextInput(&dialog, "3", "mm", "", wxDefaultPosition, dialog.FromDIP(wxSize(150, -1)));
    auto* rotate = new ::CheckBox(&dialog);
    rotate->SetValue(true);
    ::CheckBox* arrange_after = nullptr;
    ::CheckBox* utilize_printers = nullptr;
    ::CheckBox* favor_stages = nullptr;
    ::CheckBox* include_outside = nullptr;
    ::ComboBox* outside_setup = nullptr;
    row(&dialog, sizer, _L("Minimum gap"), gap);
    row(&dialog, sizer, _L("Allow rotating objects"), rotate);
    if (!options.outside.empty()) {
        include_outside = new ::CheckBox(&dialog);
        include_outside->SetValue(true);
        row(&dialog, sizer, _L("Include pieces outside plates"), include_outside);
        outside_setup = choices(&dialog, outside_setup_names);
        const auto current = std::find(outside_setup_indices.begin(), outside_setup_indices.end(),
                                       plater.get_partplate_list().get_curr_plate_index());
        if (current != outside_setup_indices.end()) outside_setup->SetSelection(int(current - outside_setup_indices.begin()));
        row(&dialog, sizer, _L("Use settings for those pieces from"), outside_setup);
        include_outside->Bind(wxEVT_TOGGLEBUTTON, [include_outside, outside_setup](wxCommandEvent& event) {
            outside_setup->Enable(include_outside->GetValue());
            event.Skip();
        });
    }
    if (stage_id.empty()) {
        arrange_after = new ::CheckBox(&dialog);
        row(&dialog, sizer, _L("Arrange plates after optimization"), arrange_after);
        utilize_printers = new ::CheckBox(&dialog);
        row(&dialog, sizer, _L("Plan across saved printers in stages"), utilize_printers);
        favor_stages = new ::CheckBox(&dialog);
        favor_stages->Disable();
        row(&dialog, sizer, _L("Favor fewer stages, even if it adds plates"), favor_stages);
        utilize_printers->Bind(wxEVT_TOGGLEBUTTON, [arrange_after, utilize_printers, favor_stages](wxCommandEvent& event) {
            if (utilize_printers->GetValue()) arrange_after->SetValue(false);
            arrange_after->Enable(!utilize_printers->GetValue());
            favor_stages->Enable(utilize_printers->GetValue());
            event.Skip();
        });
    }
    auto* help = new wxStaticText(&dialog, wxID_ANY,
        _L("Pack all printable pieces onto plates. Add plates when needed and keep different filaments separate. Review the layout before applying. Rotation keeps the same face on the bed."));
    help->Wrap(dialog.FromDIP(440));
    sizer->Add(help, 0, wxALL, dialog.FromDIP(12));
    if (utilize_printers) {
        auto* printer_help = new wxStaticText(&dialog, wxID_ANY,
            _L("Plan work across compatible saved printers, including printers that are offline. Check the filament and print quality before printing."));
        printer_help->Wrap(dialog.FromDIP(440));
        sizer->Add(printer_help, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    auto* validation = new wxStaticText(&dialog, wxID_ANY, wxEmptyString);
    sizer->Add(validation, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    auto* buttons = new DialogButtons(&dialog, {L("Preview"), "Cancel"});
    buttons->GetFIRST()->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        double value;
        if (!read_gap(gap, value) || value < 0.2) {
            validation->SetLabel(_L("Enter a gap between 0.2 and 1000 mm."));
            dialog.GetSizer()->SetSizeHints(&dialog);
            gap->GetTextCtrl()->SetFocus();
            return;
        }
        dialog.EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND);
    dialog.finish(sizer);
    if (dialog.ShowModal() != wxID_OK || !read_gap(gap, options.gap)) return std::nullopt;
    options.rotate = rotate->GetValue();
    options.arrange_after = arrange_after && arrange_after->GetValue();
    options.utilize_printers = utilize_printers && utilize_printers->GetValue();
    options.favor_stages = favor_stages && favor_stages->GetValue();
    options.pack_outside = include_outside && include_outside->GetValue();
    if (options.pack_outside) options.outside_setup_plate = outside_setup_indices[size_t(outside_setup->GetSelection())];
    return options;
}

// Resolves a plate's settings, bed and the filaments of the given pieces into a
// planning entry. Explains and returns false when the plate cannot be planned.
bool prepare_workspace_entry(Plater& plater, PartPlate& plate, const OptimizeOptions& options,
                             const Model& snapshot, WorkspacePlateTask& entry)
{
    entry.origin = plate.get_origin();
    entry.destination = plate.print_setup().destination;
    entry.assigned_name = plate.print_setup().destination_name;
    entry.process_setup = plate.print_setup();
    entry.plate_overrides = *plate.config();
    entry.config = plate.print_setup().config;
    entry.config.apply(entry.plate_overrides);
    entry.config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(plate.get_real_print_seq()));
    if (entry.config.opt_bool("belt_printer")) {
        show_info(&plater, _L("Use the existing Arrange command for belt printers."));
        return false;
    }
    entry.input.fill = false;
    entry.input.optimize = true;
    entry.input.allow_rotation = options.rotate;
    entry.input.gap = options.gap;
    entry.input.max_attempts = 25000;
    prepare_bed(plater, plate, entry.config, entry.input);
    std::set<int> slots;
    entry.can_split = !entry.sources.empty();
    for (const auto& key : entry.sources) {
        auto used = used_instance_filaments(snapshot.objects[key.first]->instances[key.second], entry.config);
        if (used.empty()) entry.can_split = false;
        slots.insert(used.begin(), used.end());
        entry.source_filaments.push_back(std::move(used));
    }
    if (entry.can_split) entry.used_filaments.assign(slots.begin(), slots.end());
    entry.can_repack = entry.can_split && entry.used_filaments.size() == 1;
    return true;
}

// Collects the plates to plan, their pieces and the printers they may use.
// Explains and returns nothing when the workspace cannot be planned.
std::shared_ptr<WorkspaceLayoutTask> collect_workspace_task(Plater& plater, const OptimizeOptions& options)
{
    plater.capture_plate_setup();
    auto task = std::make_shared<WorkspaceLayoutTask>();
    task->snapshot = plater.model();
    task->editing_config = wxGetApp().preset_bundle->full_config();
    task->stage_id = options.stage_id;
    task->arrange_after_optimization = options.arrange_after;
    task->utilize_printers = options.utilize_printers;
    task->favor_stages = options.favor_stages;
    task->stages_snapshot = plater.get_partplate_list().stages();
    if (task->utilize_printers) {
        for (const auto& printer : saved_printer_choices()) {
            const auto* preset = wxGetApp().preset_bundle->printers.find_preset(printer.preset);
            if (!preset) continue;
            if (printer.vendor == "flashforge" && !confirmed_flashforge_profile(
                printer.physical_model, preset->config.opt_string("printer_model"))) continue;
            task->available_printers.push_back(printer);
        }
        if (task->available_printers.empty()) {
            show_info(&plater, _L("Add a saved printer with a supported profile to plan stages."));
            return nullptr;
        }
        const auto active_destination = plater.get_partplate_list().get_curr_plate()->print_setup().destination;
        std::stable_sort(task->available_printers.begin(), task->available_printers.end(),
            [&](const SavedPrinterChoice& a, const SavedPrinterChoice& b) {
                return a.destination == active_destination && b.destination != active_destination;
            });
        // One copy for the job; each saved printer is resolved from it off the main thread.
        task->presets = std::make_shared<const PresetBundle>(*wxGetApp().preset_bundle);
    }
    auto& plates = plater.get_partplate_list();
    task->unassigned_instances = options.pack_outside ? 0 : options.outside.size();
    task->included_unassigned = options.pack_outside ? options.outside.size() : 0;
    task->unassigned_setup_plate = options.pack_outside ? options.outside_setup_plate : -1;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        auto* plate = plates.get_plate(i);
        task->states.push_back({plate->id(), plate->get_origin(), plate->stage_id(),
                                plate->is_locked(), plate->print_setup().destination,
                                plate->print_setup().config, *plate->config()});
        if ((!options.stage_id.empty() && plate->stage_id() != options.stage_id) || plate->is_locked() || plate->empty()) continue;
        if (plate->print_setup().config.empty()) {
            show_info(&plater, format_wxstr(_L("Plate %1% has no printer setup."), i + 1));
            return nullptr;
        }
        WorkspacePlateTask entry;
        entry.index = i;
        entry.stage_id = plate->stage_id();
        for (size_t o = 0; o < task->snapshot.objects.size(); ++o)
            for (size_t copy = 0; copy < task->snapshot.objects[o]->instances.size(); ++copy) {
                const InstanceKey key(o, copy);
                if (plate->contain_instance(o, copy) && task->snapshot.objects[o]->instances[copy]->printable)
                    entry.sources.push_back(key);
                else if ((plate->contain_instance(o, copy) || plate->intersect_instance(o, copy)) &&
                         (!options.pack_outside || std::find(options.outside.begin(), options.outside.end(), key) == options.outside.end()))
                    entry.fixed.push_back(key);
            }
        if (!prepare_workspace_entry(plater, *plate, options, task->snapshot, entry)) return nullptr;
        if (!entry.sources.empty()) task->plates.push_back(std::move(entry));
    }
    if (options.pack_outside) {
        WorkspacePlateTask entry;
        entry.index = -1;
        entry.sources = options.outside;
        if (!prepare_workspace_entry(plater, *plates.get_plate(options.outside_setup_plate), options, task->snapshot, entry))
            return nullptr;
        if (!entry.can_split) {
            show_info(&plater, _L("Some pieces use filaments that are missing from the chosen plate. Choose a plate with those filaments before optimizing."));
            return nullptr;
        }
        task->plates.push_back(std::move(entry));
    }
    if (task->plates.empty()) {
        show_info(&plater, _L("There are no unlocked plates with printable objects to plan."));
        return nullptr;
    }
    for (size_t i = 0; i < task->plates.size(); ++i) {
        task->plates[i].group = i;
        for (size_t j = 0; j < i; ++j) {
            const auto& a = task->plates[i];
            const auto& b = task->plates[j];
            const bool compatible = task->utilize_printers
                ? a.can_repack && b.can_repack && a.used_filaments == b.used_filaments &&
                  !a.config.opt_string("printer_settings_id").empty() && same_plate_layout_settings(a.config, b.config)
                : a.can_split && b.can_split && a.stage_id == b.stage_id && a.destination == b.destination &&
                  same_plate_layout_settings(a.config, b.config);
            if (compatible) {
                task->plates[i].group = b.group;
                break;
            }
        }
    }
    return task;
}

// Resolves each compatible group onto every saved printer that can take it.
// Runs on the job thread. Returns false when canceled or when a group has no
// printer, which task.unprepared_plate then names.
bool resolve_printer_targets(WorkspaceLayoutTask& task, Job::Ctl& ctl)
{
    // Copying presets dominates each resolution. Groups that differ only in
    // their filament slots or bookkeeping resolve alike on a printer, so each
    // printer resolves one such set of settings once per job.
    std::map<std::pair<size_t, size_t>, std::optional<DynamicPrintConfig>> resolved;
    for (size_t group = 0; group < task.plates.size(); ++group) {
        if (task.plates[group].group != group) continue;
        const auto& source = task.plates[group];
        size_t settings = group;
        for (size_t earlier = 0; earlier < group; ++earlier)
            if (task.plates[earlier].group == earlier && same_plate_layout_settings(task.plates[earlier].config, source.config)) {
                settings = earlier;
                break;
            }
        for (size_t printer = 0; printer < task.available_printers.size(); ++printer) {
            if (ctl.was_canceled()) return false;
            try {
                const auto& choice = task.available_printers[printer];
                std::optional<DynamicPrintConfig> config;
                if (source.config.opt_string("printer_settings_id") == choice.preset) {
                    config = source.config;
                } else {
                    // A failed resolution is cached too; an exception leaves it empty.
                    const auto [entry, inserted] = resolved.try_emplace({settings, printer});
                    auto& cached = entry->second;
                    if (inserted) cached = printer_layout_config(*task.presets, task.plates[settings].config, choice);
                    if (cached && same_assigned_filaments(source.config, *cached, source.used_filaments)) {
                        config = cached;
                        // The tower positions stay this job's own, as an uncached resolution keeps them.
                        for (const char* key : {"wipe_tower_x", "wipe_tower_y"})
                            if (const auto* option = source.config.option(key)) config->set_key_value(key, option->clone());
                    }
                }
                if (!config) continue;
                PlateLayoutInput input = source.input;
                input.bed.clear();
                input.excluded.clear();
                input.fixed.clear();
                input.shapes.clear();
                if (!prepare_printer_bed(*config, input)) continue;
                task.printer_targets.push_back({group, printer, std::move(*config), std::move(input)});
            } catch (const std::exception&) { /* An incompatible profile cannot receive this job. */ }
        }
        if (std::none_of(task.printer_targets.begin(), task.printer_targets.end(),
                         [group](const auto& target) { return target.group == group; })) {
            task.unprepared_plate = source.index >= 0 ? source.index : task.unassigned_setup_plate;
            return false;
        }
    }
    return true;
}

// Projects each plate's pieces and fixed objects. Returns the packing
// candidates, or nothing when canceled.
std::optional<std::vector<PlateConsolidationPlate>> project_workspace(WorkspaceLayoutTask& task, Job::Ctl& ctl,
                                                                      const std::string& status)
{
    std::vector<PlateConsolidationPlate> candidates;
    std::map<std::pair<size_t, std::vector<int>>, size_t> material_groups;
    for (size_t index = 0; index < task.plates.size(); ++index) {
        auto& plate = task.plates[index];
        if (ctl.was_canceled()) return std::nullopt;
        for (const auto& key : plate.sources)
            plate.input.shapes.push_back(project_shape(task.snapshot, plate.config, plate.origin, key, false, ctl));
        for (const auto& key : plate.fixed)
            plate.input.fixed.push_back(project_shape(task.snapshot, plate.config, plate.origin, key, true, ctl));
        PlateConsolidationPlate candidate{plate.input, plate.group,
            plate.fixed.empty() && (task.utilize_printers ? plate.can_repack : plate.can_split)};
        for (const auto& filaments : plate.source_filaments)
            candidate.shape_groups.push_back(material_groups.try_emplace(std::make_pair(plate.group, filaments), material_groups.size()).first->second);
        candidates.push_back(std::move(candidate));
        ctl.update_status(int(20 * (index + 1) / task.plates.size()), status);
    }
    return candidates;
}

// True when a plate holds pieces that use different filaments.
bool mixes_materials(const std::vector<WorkspacePlateTask>& plates)
{
    return std::any_of(plates.begin(), plates.end(), [](const WorkspacePlateTask& plate) {
        if (!plate.can_split || plate.source_filaments.empty()) return false;
        return std::any_of(plate.source_filaments.begin() + 1, plate.source_filaments.end(),
            [&](const std::vector<int>& used) { return used != plate.source_filaments.front(); });
    });
}

// Packs the projected pieces onto as few compatible plates as fit and records
// the result on task.plates. Returns false when canceled or some pieces do not fit.
bool pack_workspace(WorkspaceLayoutTask& task, const std::vector<PlateConsolidationPlate>& candidates,
                    const std::vector<WorkspacePlateTask>& original_plates, Job::Ctl& ctl, const std::string& status)
{
    auto proposal = task.utilize_printers
        ? consolidate_plate_layouts(candidates, [&ctl] { return ctl.was_canceled(); },
            [&](int value) { ctl.update_status(20 + 30 * value / 100, status); }, true)
        : repack_plate_layouts(candidates, [&ctl] { return ctl.was_canceled(); },
            [&](int value) { ctl.update_status(20 + 80 * value / 100, status); });
    if (proposal.canceled) return false;
    task.search_limited = proposal.search_limited;
    if (!proposal.unplaced.empty()) { task.pieces_do_not_fit = true; return false; }
    for (size_t i = task.plates.size(); i < proposal.plates.size(); ++i) {
        auto entry = original_plates[proposal.plates[i].template_plate];
        entry.index = -1;
        entry.sources.clear();
        task.plates.push_back(std::move(entry));
    }
    for (size_t i = 0; i < task.plates.size(); ++i) {
        auto& plate = task.plates[i];
        auto& result = proposal.plates[i];
        plate.input = std::move(result.layout);
        plate.result = std::move(result.placement);
        plate.references = std::move(result.sources);
        plate.changed = result.changed;
        plate.removed = result.removed;
    }
    return true;
}

// Gives each planned job a saved printer and a stage. Returns false when
// canceled or some job cannot be placed, which task.unplanned_plate then names.
bool schedule_printer_jobs(WorkspaceLayoutTask& task, Job::Ctl& ctl, const std::string& status,
                           int progress_start, int progress_end)
{
    std::vector<std::vector<WorkspacePrinterCandidate>> options(task.plates.size());
    std::vector<size_t> jobs;
    for (size_t i = 0; i < task.plates.size(); ++i) {
        const auto& plate = task.plates[i];
        if (plate.removed || plate.references.empty()) continue;
        jobs.push_back(i);
        for (const auto& target : task.printer_targets) {
            if (ctl.was_canceled()) return false;
            // Projection per printer is costly too; stop offering printers once time is spent.
            if (std::chrono::steady_clock::now() >= task.deadline) { task.search_limited = true; break; }
            if (target.group != plate.group) continue;
            if (plate.anchor_printer >= 0) {
                if (size_t(plate.anchor_printer) == target.printer)
                    options[i].push_back({target.printer, target.config, plate.input, plate.result});
                continue;
            }
            // A job with objects that stay on its plate keeps that plate's bed
            // and its own settings; the group's resolution may differ in bookkeeping.
            const bool keeps_plate = !plate.fixed.empty();
            if (keeps_plate && !can_keep_fixed_job(plate.config, plate.destination, target.config,
                                                   task.available_printers[target.printer].destination)) continue;
            const DynamicPrintConfig& config = keeps_plate ? plate.config : target.config;
            PlateLayoutInput input = keeps_plate ? plate.input : target.input;
            input.shapes.clear();
            input.stop_on_fit = true;
            bool compatible = true;
            for (const auto& ref : plate.references) {
                const auto& source = task.plates[ref.plate];
                const auto key = source.sources[ref.shape];
                try {
                    input.shapes.push_back(project_shape(task.snapshot, config,
                                                         source.origin, key, false, ctl));
                } catch (const std::exception&) {
                    if (ctl.was_canceled()) return false;
                    compatible = false;
                    break;
                }
            }
            if (!compatible) continue;
            auto placement = compute_plate_layout(input, [&ctl] { return ctl.was_canceled(); });
            if (placement.canceled) return false;
            task.search_limited |= placement.search_limited;
            if (placement.placements.size() != input.shapes.size()) continue;
            options[i].push_back({target.printer, config, std::move(input), std::move(placement)});
        }
        if (options[i].empty()) {
            task.unplanned_plate = plate.index >= 0 ? plate.index : task.unassigned_setup_plate;
            return false;
        }
        ctl.update_status(progress_start + int((progress_end - progress_start) * (i + 1) / task.plates.size()), status);
    }
    std::stable_sort(jobs.begin(), jobs.end(), [&](size_t a, size_t b) {
        if ((task.plates[a].anchor_printer >= 0) != (task.plates[b].anchor_printer >= 0))
            return task.plates[a].anchor_printer >= 0;
        if (options[a].size() != options[b].size()) return options[a].size() < options[b].size();
        return plate_layout_load(task.plates[a].input) > plate_layout_load(task.plates[b].input);
    });
    std::vector<PrinterStageJob> requests;
    for (size_t i : jobs) {
        PrinterStageJob request;
        request.load = plate_layout_load(task.plates[i].input);
        for (const auto& option : options[i]) request.eligible_printers.push_back(option.printer);
        requests.push_back(std::move(request));
    }
    const auto assignments = assign_printer_stages(requests, task.available_printers.size());
    if (assignments.size() != jobs.size()) {
        const int index = task.plates[jobs.front()].index;
        task.unplanned_plate = index >= 0 ? index : task.unassigned_setup_plate;
        return false;
    }
    for (size_t order = 0; order < jobs.size(); ++order) {
        auto& plate = task.plates[jobs[order]];
        const auto assigned = assignments[order];
        auto candidate = std::find_if(options[jobs[order]].begin(), options[jobs[order]].end(),
            [assigned](const auto& option) { return option.printer == assigned.printer; });
        const auto& printer = task.available_printers[assigned.printer];
        plate.assigned_destination = printer.destination;
        plate.assigned_name = printer.name;
        plate.wave = int(assigned.stage);
        plate.config = std::move(candidate->config);
        plate.input = std::move(candidate->input);
        plate.result = std::move(candidate->result);
        plate.changed = true;
    }
    return true;
}

// Builds printer jobs from individual objects instead of whole plates. A whole
// job may not fit a smaller printer even though some of its objects do;
// assigning that job first would hide those printers from the stage plan.
// Returns the staged plates followed by the emptied originals, or nothing when
// canceled, an object fits no printer or the plan needs too many plates.
std::optional<std::vector<WorkspacePlateTask>> pack_objects_by_printer(WorkspaceLayoutTask& task,
    const std::vector<WorkspacePlateTask>& original_plates, Job::Ctl& ctl, const std::string& status)
{
    struct ItemOption {
        size_t target;
        PlateLayoutShape shape;
    };
    struct Item {
        size_t source;
        size_t shape;
        double area;
        std::vector<int> filaments;
        std::vector<ItemOption> options;
    };
    std::vector<Item> items;
    for (size_t source = 0; source < original_plates.size(); ++source) {
        const auto& plate = original_plates[source];
        for (size_t shape = 0; shape < plate.sources.size(); ++shape) {
            if (ctl.was_canceled()) return std::nullopt;
            if (std::chrono::steady_clock::now() >= task.deadline) { task.search_limited = true; return std::nullopt; }
            const auto size = get_extents(plate.input.shapes[shape].footprint).size();
            Item item {source, shape, double(size.x()) * double(size.y()),
                       plate.source_filaments[shape], {}};
            for (size_t target = 0; target < task.printer_targets.size(); ++target) {
                const auto& printer = task.printer_targets[target];
                if (printer.group != plate.group) continue;
                try {
                    auto projected = project_shape(task.snapshot, printer.config, plate.origin,
                                                   plate.sources[shape], false, ctl);
                    PlateLayoutInput trial = printer.input;
                    trial.shapes.push_back(projected);
                    trial.stop_on_fit = true;
                    const auto fit = compute_plate_layout(trial, [&ctl] { return ctl.was_canceled(); });
                    if (fit.canceled) return std::nullopt;
                    task.search_limited |= fit.search_limited;
                    if (fit.placements.size() == 1)
                        item.options.push_back({target, std::move(projected)});
                } catch (const std::exception&) {
                    if (ctl.was_canceled()) return std::nullopt;
                }
            }
            if (item.options.empty()) return std::nullopt;
            items.push_back(std::move(item));
        }
        ctl.update_status(70 + int(12 * (source + 1) / original_plates.size()), status);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.options.size() != b.options.size()) return a.options.size() < b.options.size();
        return a.area > b.area;
    });
    struct Bin {
        size_t target;
        std::vector<int> filaments;
        PlateLayoutInput input;
        PlateLayoutResult result;
        std::vector<PlateConsolidationRef> references;
    };
    std::vector<Bin> bins;
    std::vector<size_t> printer_counts(task.available_printers.size(), 0);
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        if (ctl.was_canceled()) return std::nullopt;
        const auto& item = items[item_index];
        bool placed = false;
        for (auto& bin : bins) {
            if (bin.filaments != item.filaments) continue;
            const auto option = std::find_if(item.options.begin(), item.options.end(),
                [&](const ItemOption& entry) { return entry.target == bin.target; });
            if (option == item.options.end()) continue;
            PlateLayoutInput trial = bin.input;
            trial.shapes.push_back(option->shape);
            auto fit = compute_plate_layout(trial, [&ctl] { return ctl.was_canceled(); });
            if (fit.canceled) return std::nullopt;
            task.search_limited |= fit.search_limited;
            if (fit.placements.size() != trial.shapes.size()) continue;
            bin.input = std::move(trial);
            bin.result = std::move(fit);
            bin.references.push_back({item.source, item.shape});
            placed = true;
            break;
        }
        if (!placed) {
            const auto option = std::min_element(item.options.begin(), item.options.end(),
                [&](const ItemOption& a, const ItemOption& b) {
                    const auto& first = task.printer_targets[a.target];
                    const auto& second = task.printer_targets[b.target];
                    if (printer_counts[first.printer] != printer_counts[second.printer])
                        return printer_counts[first.printer] < printer_counts[second.printer];
                    const auto first_size = get_extents(first.input.bed).size();
                    const auto second_size = get_extents(second.input.bed).size();
                    return double(first_size.x()) * double(first_size.y()) >
                           double(second_size.x()) * double(second_size.y());
                });
            const auto& target = task.printer_targets[option->target];
            PlateLayoutInput input = target.input;
            input.shapes.push_back(option->shape);
            input.stop_on_fit = true;
            auto fit = compute_plate_layout(input, [&ctl] { return ctl.was_canceled(); });
            if (fit.canceled) return std::nullopt;
            bins.push_back({option->target, item.filaments, std::move(input), std::move(fit), {{item.source, item.shape}}});
            ++printer_counts[target.printer];
            if (task.states.size() +
                (bins.size() > original_plates.size() ? bins.size() - original_plates.size() : 0) >
                max_project_plates)
                return std::nullopt;
        }
        ctl.update_status(82 + int(12 * (item_index + 1) / items.size()), status);
    }
    // Empty the least needed printer jobs into the others where everything still fits.
    std::vector<size_t> donors(bins.size());
    std::iota(donors.begin(), donors.end(), 0);
    std::stable_sort(donors.begin(), donors.end(), [&](size_t a, size_t b) {
        const auto first = task.printer_targets[bins[a].target].printer;
        const auto second = task.printer_targets[bins[b].target].printer;
        if (printer_counts[first] != printer_counts[second])
            return printer_counts[first] > printer_counts[second];
        return bins[a].references.size() < bins[b].references.size();
    });
    for (size_t order = 0; order < donors.size(); ++order) {
        if (ctl.was_canceled()) return std::nullopt;
        const size_t donor = donors[order];
        if (bins[donor].references.empty()) continue;
        std::map<size_t, Bin> proposals;
        bool complete = true;
        for (const auto& ref : bins[donor].references) {
            const auto item = std::find_if(items.begin(), items.end(), [&](const Item& candidate) {
                return candidate.source == ref.plate && candidate.shape == ref.shape;
            });
            if (item == items.end()) { complete = false; break; }
            bool placed = false;
            for (size_t target = 0; target < bins.size(); ++target) {
                if (target == donor || bins[target].references.empty()) continue;
                if (bins[target].filaments != item->filaments) continue;
                const auto option = std::find_if(item->options.begin(), item->options.end(),
                    [&](const ItemOption& candidate) { return candidate.target == bins[target].target; });
                if (option == item->options.end()) continue;
                auto& proposal = proposals.try_emplace(target, bins[target]).first->second;
                PlateLayoutInput trial = proposal.input;
                trial.shapes.push_back(option->shape);
                auto fit = compute_plate_layout(trial, [&ctl] { return ctl.was_canceled(); });
                if (fit.canceled) return std::nullopt;
                task.search_limited |= fit.search_limited;
                if (fit.placements.size() != trial.shapes.size()) continue;
                proposal.input = std::move(trial);
                proposal.result = std::move(fit);
                proposal.references.push_back(ref);
                placed = true;
                break;
            }
            if (!placed) { complete = false; break; }
        }
        if (complete) {
            for (auto& [target, proposal] : proposals) bins[target] = std::move(proposal);
            bins[donor].references.clear();
            --printer_counts[task.printer_targets[bins[donor].target].printer];
        }
        ctl.update_status(94 + int(3 * (order + 1) / donors.size()), status);
    }
    bins.erase(std::remove_if(bins.begin(), bins.end(),
        [](const Bin& bin) { return bin.references.empty(); }), bins.end());
    std::vector<WorkspacePlateTask> staged;
    staged.reserve(bins.size() + original_plates.size());
    for (size_t index = 0; index < bins.size(); ++index) {
        auto& bin = bins[index];
        const auto& target = task.printer_targets[bin.target];
        WorkspacePlateTask plate;
        plate.index = index < original_plates.size() ? original_plates[index].index : -1;
        plate.group = target.group;
        plate.anchor_printer = int(target.printer);
        plate.config = target.config;
        // Every object in a job shares its settings; keep the first one's setup and plate overrides.
        const auto& origin = original_plates[bin.references.front().plate];
        plate.process_setup = origin.process_setup;
        plate.plate_overrides = origin.plate_overrides;
        plate.input = std::move(bin.input);
        plate.result = std::move(bin.result);
        plate.references = std::move(bin.references);
        for (auto& ref : plate.references) ref.plate += bins.size();
        plate.changed = true;
        staged.push_back(std::move(plate));
    }
    for (size_t index = 0; index < original_plates.size(); ++index) {
        auto donor = original_plates[index];
        if (index < bins.size()) donor.index = -1; // Its plate is reused by a planned job.
        donor.removed = true;
        staged.push_back(std::move(donor));
    }
    return staged;
}

int stage_count(const std::vector<WorkspacePlateTask>& plates)
{
    return std::accumulate(plates.begin(), plates.end(), 0, [](int count, const auto& plate) { return std::max(count, plate.wave); });
}

size_t staged_job_count(const std::vector<WorkspacePlateTask>& plates)
{
    return size_t(std::count_if(plates.begin(), plates.end(), [](const auto& plate) { return plate.wave > 0; }));
}

// Plans stages across saved printers. Keeps the plan built from whole plates
// unless planning objects individually finishes in fewer stages, or is the only
// plan that keeps different filaments apart.
void plan_stage_assignments(WorkspaceLayoutTask& task, const std::vector<WorkspacePlateTask>& original_plates,
                         bool separate_materials, Job::Ctl& ctl, const std::string& status)
{
    const bool baseline_scheduled = schedule_printer_jobs(task, ctl, status, 50, 70);
    const int baseline_unplanned = task.unplanned_plate;
    const auto baseline_plates = task.plates;
    const int baseline_waves = stage_count(baseline_plates);
    const size_t baseline_jobs = staged_job_count(baseline_plates);
    task.baseline_jobs = baseline_jobs;
    if (baseline_scheduled && !separate_materials &&
        (baseline_waves <= 1 || task.printer_targets.size() < 2)) {
        ctl.update_status(100, status);
        return;
    }
    if (!baseline_scheduled) task.plates = original_plates;
    if (std::any_of(original_plates.begin(), original_plates.end(),
        [](const auto& plate) { return !plate.fixed.empty() || !plate.can_split; })) {
        ctl.update_status(100, status);
        return;
    }
    auto staged = pack_objects_by_printer(task, original_plates, ctl, status);
    if (!staged) {
        ctl.update_status(100, status);
        return;
    }
    task.plates = std::move(*staged);
    task.unplanned_plate = -1;
    const bool scheduled = schedule_printer_jobs(task, ctl, status, 97, 99);
    const int staged_waves = stage_count(task.plates);
    const size_t staged_jobs = staged_job_count(task.plates);
    if (baseline_scheduled && scheduled && !task.favor_stages && staged_jobs > baseline_jobs &&
        staged_waves < baseline_waves) {
        task.alternate_jobs = staged_jobs;
        task.alternate_waves = staged_waves;
    }
    const size_t new_plates = std::count_if(task.plates.begin(), task.plates.end(),
        [](const auto& plate) { return plate.index < 0 && plate.wave > 0; });
    if (!scheduled || task.states.size() + new_plates > max_project_plates ||
        (baseline_scheduled && !separate_materials && (staged_waves > baseline_waves ||
        (!task.favor_stages && staged_jobs > baseline_jobs) ||
        (staged_waves == baseline_waves && staged_jobs >= baseline_jobs)))) {
        task.plates = baseline_scheduled ? baseline_plates : original_plates;
        task.unplanned_plate = baseline_unplanned;
    } else if (!baseline_scheduled) {
        task.baseline_jobs = staged_jobs;
    }
    ctl.update_status(100, status);
}

// The optimize-plates job: resolve printers, project pieces, pack them and,
// when planning across saved printers, assign printers and stages.
void plan_workspace(WorkspaceLayoutTask& task, Job::Ctl& ctl, const std::string& status)
{
    if (task.utilize_printers && !resolve_printer_targets(task, ctl)) return;
    const auto candidates = project_workspace(task, ctl, status);
    if (!candidates) return;
    const auto original_plates = task.plates;
    const bool separate_materials = mixes_materials(original_plates);
    if (!pack_workspace(task, *candidates, original_plates, ctl, status) || !task.utilize_printers) return;
    plan_stage_assignments(task, original_plates, separate_materials, ctl, status);
}

// Planning many plates runs many layouts. They share one deadline, after which
// each returns at once and the preview reports that the search stopped early.
constexpr std::chrono::seconds workspace_planning_time{60};

void optimize_workspace(WorkspaceLayoutTask& task, Job::Ctl& ctl)
{
    const std::string status = _u8L("Finding room for all pieces");
    ctl.update_status(0, status);
    task.deadline = std::chrono::steady_clock::now() + workspace_planning_time;
    for (auto& plate : task.plates) plate.input.deadline = task.deadline;
    plan_workspace(task, ctl, status);
    task.out_of_time = std::chrono::steady_clock::now() >= task.deadline;
}
} // namespace

void show_optimize_plates_dialog(Plater& plater, const std::string& stage_id)
{
    if (!can_optimize_plates(plater, stage_id)) return;
    const auto options = ask_optimize_options(plater, stage_id);
    if (!options || !can_optimize_plates(plater, stage_id)) return;
    const auto task = collect_workspace_task(plater, *options);
    if (!task) return;
    queue_job(plater.get_ui_job_worker(), [task](Job::Ctl& ctl) { optimize_workspace(*task, ctl); },
        [task](bool canceled, std::exception_ptr& error) {
            if (canceled || wxGetApp().is_closing()) return;
            if (error) {
                const wxString message = layout_failure_message(error, _L("Could not create a preview for these plates."));
                error = nullptr;
                show_info(wxGetApp().plater(), message);
                return;
            }
            wxWeakRef<Plater> owner(wxGetApp().plater());
            wxGetApp().CallAfter([task, owner] {
                if (owner && !wxGetApp().is_closing()) review_workspace_layout(*owner, task);
            });
        });
}
}} // namespace Slic3r::GUI
