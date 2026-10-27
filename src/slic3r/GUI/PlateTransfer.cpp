#include "PlateTransfer.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "DesignTokens.hpp"
#include "Jobs/Worker.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PlateMaterialTransfer.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include "format.hpp"
#include <wx/colour.h>
#include <wx/control.h>
#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/string.h>
#include <wx/weakref.h>
#include <wx/window.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {
namespace {
struct Move
{
    PlateInstanceTransfer transfer;
    int source = -1;
    int destination = -1;
};
// Identities, content stamps and placements of the model a transfer was planned
// on. Comparing them is much cheaper than copying the model and still catches
// any edit to ids, placements, settings, meshes or painted regions.
struct ModelStamp
{
    std::vector<uint64_t> keys;
    std::vector<double> values;
    std::vector<std::string> production_ids;
    bool operator==(const ModelStamp& rhs) const
    {
        return keys == rhs.keys && values == rhs.values && production_ids == rhs.production_ids;
    }
};

ModelStamp stamp_model(const Model& model)
{
    ModelStamp stamp;
    const auto key = [&stamp](uint64_t value) { stamp.keys.push_back(value); };
    const auto place = [&stamp](const Geometry::Transformation& transformation) {
        const auto& matrix = transformation.get_matrix().matrix();
        stamp.values.insert(stamp.values.end(), matrix.data(), matrix.data() + matrix.size());
    };
    // ModelConfigObject keeps its timestamp accessor private; ModelConfig exposes it.
    const auto config_time = [](const ModelConfig& value) { return value.timestamp(); };
    key(model.id().id);
    for (const ModelObject* object : model.objects) {
        key(object->id().id);
        key(object->printable);
        key(object->auto_drop);
        key(config_time(object->config));
        key(object->layer_height_profile.timestamp());
        key(object->instances.size());
        key(object->volumes.size());
        key(object->layer_config_ranges.size());
        for (const auto& [range, settings] : object->layer_config_ranges) {
            stamp.values.push_back(range.first);
            stamp.values.push_back(range.second);
            key(config_time(settings));
        }
        for (const ModelInstance* instance : object->instances) {
            key(instance->id().id);
            key(instance->printable);
            place(instance->get_transformation());
            stamp.production_ids.push_back(instance->production_id);
        }
        for (const ModelVolume* volume : object->volumes) {
            key(volume->id().id);
            key(uint64_t(reinterpret_cast<std::uintptr_t>(volume->mesh_ptr().get())));
            key(config_time(volume->config));
            key(uint64_t(volume->type()));
            key(volume->mmu_segmentation_facets.timestamp());
            key(volume->supported_facets.timestamp());
            key(volume->seam_facets.timestamp());
            key(volume->fuzzy_skin_facets.timestamp());
            place(volume->get_transformation());
        }
    }
    return stamp;
}

struct Draft
{
    ModelStamp model;
    size_t undo_time = 0;
    std::vector<Move> moves;
    std::vector<PlatePrintSetup> setups;
    std::vector<DynamicPrintConfig> overrides;
    std::vector<Vec3d> origins;
    std::vector<ObjectID> plate_ids;
    int active_plate = -1;
};

wxColour transfer_color(DesignSystem::ColorRole role)
{
    return wxColour(DesignSystem::color_hex(role, wxGetApp().dark_mode()));
}

wxBitmap filament_swatch(wxWindow& window, const wxColour& color, int edge = 28)
{
    wxBitmap bitmap;
    bitmap.CreateWithLogicalSize(window.FromDIP(wxSize(edge, edge)), window.GetDPIScaleFactor(), 32);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(transfer_color(DesignSystem::ColorRole::SurfaceRaised)));
    dc.Clear();
    dc.SetPen(wxPen(transfer_color(DesignSystem::ColorRole::InkMuted)));
    dc.SetBrush(wxBrush(color.IsOk() ? color : transfer_color(DesignSystem::ColorRole::SurfaceRaised)));
    const auto size = bitmap.GetLogicalSize();
    dc.DrawRectangle(0, 0, size.x, size.y);
    if (!color.IsOk()) {
        dc.SetPen(wxPen(transfer_color(DesignSystem::ColorRole::InkMuted)));
        dc.DrawLine(size.x / 4, size.y * 3 / 4, size.x * 3 / 4, size.y / 4);
    }
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

wxString filament_value(const DynamicPrintConfig& config, int slot, const char* key)
{
    const auto* values = config.option<ConfigOptionStrings>(key);
    return values && slot > 0 && size_t(slot) <= values->values.size()
        ? from_u8(values->values[size_t(slot - 1)]) : wxString{};
}

wxColour filament_color(const DynamicPrintConfig& config, int slot)
{
    const auto value = filament_value(config, slot, "filament_colour");
    return value.StartsWith("#") && (value.size() == 7 || value.size() == 9) ? wxColour(value) : wxColour{};
}

wxString filament_profile(const DynamicPrintConfig& config, int slot)
{
    auto name = filament_value(config, slot, "filament_settings_id");
    if (name.EndsWith(" @System")) name.RemoveLast(8);
    return name.empty() ? _L("Unknown filament") : name;
}

wxString filament_details(const DynamicPrintConfig& config, int slot)
{
    auto type = filament_value(config, slot, "filament_type");
    if (type.empty() || type == "(Undefined)") type = _L("Unknown material");
    const auto* diameters = config.option<ConfigOptionFloats>("filament_diameter");
    if (diameters && slot > 0 && size_t(slot) <= diameters->values.size() &&
        std::isfinite(diameters->values[size_t(slot - 1)]) && diameters->values[size_t(slot - 1)] > 0)
        type += L" \u00B7 " + format_wxstr(_L("%1% mm"), wxString::Format("%g", diameters->values[size_t(slot - 1)]));
    const auto color = filament_color(config, slot);
    return type + L" \u00B7 " + (color.IsOk() ? filament_value(config, slot, "filament_colour").Upper() : _L("Unknown color"));
}

wxString candidate_note(const PlateFilamentCandidate& candidate)
{
    if (candidate.exact) return _L("Same filament and settings");
    if (!candidate.material_known) return _L("Material details are incomplete. Check before moving.");
    if (!candidate.same_material) return _L("Material or diameter differs. Check before moving.");
    if (candidate.same_color) return _L("Same color. The destination filament's settings will be used.");
    if (!std::isfinite(candidate.color_distance)) return _L("Color could not be compared. Check before moving.");
    return _L("Color changes. The destination filament's settings will be used.");
}

class TransferDialog final : public DPIDialog
{
    struct Swatch { wxStaticBitmap* view; wxColour color; };
    struct Choice {
        size_t group;
        std::vector<size_t> moves;
        int from;
        ::ComboBox* combo;
        std::vector<PlateFilamentCandidate> candidates;
        std::vector<wxColour> colors;
        std::vector<wxString> details;
        wxStaticBitmap* preview;
        Label* detail;
        Label* note;
    };
    std::vector<Choice> m_choices;
    std::vector<Swatch> m_swatches;
    DialogButtons* m_buttons = nullptr;
    wxScrolledWindow* m_scroll = nullptr;
    Label* m_summary = nullptr;
public:
    TransferDialog(Plater& plater, Draft& draft)
        : DPIDialog(&plater, wxID_ANY, _L("Choose filaments for moved parts"), wxDefaultPosition,
                    wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        SetFont(Label::Body_14);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* intro = new Label(this, _L("Matching materials are listed first. Painted regions and part settings will be kept."), LB_AUTO_WRAP);
        intro->Wrap(FromDIP(720));
        intro->SetMinSize(FromDIP(wxSize(720, -1)));
        outer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(20));
        m_scroll = new wxScrolledWindow(this);
        m_scroll->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        m_scroll->SetScrollRate(0, FromDIP(16));
        auto* content = new wxBoxSizer(wxVERTICAL);
        // The same source/destination pair needs one decision per filament,
        // even when several objects or linked copies move together.
        std::map<std::tuple<int, int, size_t>, std::vector<size_t>> groups;
        for (size_t i = 0; i < draft.moves.size(); ++i) {
            const auto& move = draft.moves[i];
            if (move.source != move.destination && move.destination >= 0)
                groups[{move.source, move.destination, move.source < 0 ? move.transfer.object_idx : 0}].push_back(i);
        }
        const auto plate_name = [&plater](int index) {
            if (index < 0) return _L("Outside the plates");
            const auto name = plater.get_partplate_list().get_plate(index)->get_plate_name();
            return name.empty() ? format_wxstr(_L("Plate %1%"), index + 1) : from_u8(name);
        };
        size_t group_index = 0;
        for (const auto& [key, moves] : groups) {
            const int from_plate = std::get<0>(key), to_plate = std::get<1>(key);
            const auto& destination = draft.setups[size_t(to_plate)].config;
            const DynamicPrintConfig source = from_plate < 0 ? DynamicPrintConfig{} : draft.setups[size_t(from_plate)].config;
            auto* heading = new Label(m_scroll, Label::Head_16,
                wxControl::EscapeMnemonics(plate_name(from_plate) + L" \u2192 " + plate_name(to_plate)), LB_AUTO_WRAP);
            heading->Wrap(FromDIP(720));
            content->Add(heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
            wxString names;
            std::set<size_t> objects;
            std::map<int, std::vector<size_t>> slots;
            for (size_t i : moves) {
                const auto& move = draft.moves[i];
                const auto& object = *plater.model().objects[move.transfer.object_idx];
                if (objects.insert(move.transfer.object_idx).second) {
                    if (!names.empty()) names += ", ";
                    names += from_u8(object.name);
                }
                for (int from : object_filament_slots(object)) slots[from].push_back(i);
            }
            auto* parts = new Label(m_scroll, Label::Body_12, wxControl::EscapeMnemonics(names), LB_AUTO_WRAP);
            parts->Wrap(FromDIP(720));
            content->Add(parts, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
            auto* grid = new wxFlexGridSizer(2, FromDIP(16), FromDIP(24));
            grid->AddGrowableCol(0, 1);
            grid->AddGrowableCol(1, 1);
            grid->Add(new Label(m_scroll, Label::Head_13, _L("Current filament")), 0, wxEXPAND);
            grid->Add(new Label(m_scroll, Label::Head_13, _L("Use after moving")), 0, wxEXPAND);
            for (const auto& [from, affected_moves] : slots) {
                auto* left = new wxBoxSizer(wxHORIZONTAL);
                const auto color = filament_color(source, from);
                auto* swatch = new wxStaticBitmap(m_scroll, wxID_ANY, filament_swatch(*m_scroll, color));
                m_swatches.push_back({swatch, color});
                left->Add(swatch, 0, wxRIGHT | wxTOP, FromDIP(10));
                auto* identity = new wxBoxSizer(wxVERTICAL);
                const auto title = format_wxstr(plate_filament_is_physical(source, from) ||
                    size_t(from) > plate_filament_count(source) ? _L("Filament %1%") : _L("Mixed filament %1%"), from);
                identity->Add(new Label(m_scroll, Label::Head_14, title), 0, wxBOTTOM, FromDIP(4));
                auto* profile = new Label(m_scroll, wxControl::EscapeMnemonics(filament_profile(source, from)), LB_AUTO_WRAP);
                profile->Wrap(FromDIP(270));
                profile->SetMinSize(FromDIP(wxSize(270, -1)));
                identity->Add(profile, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
                identity->Add(new Label(m_scroll, Label::Body_12, filament_details(source, from), LB_AUTO_WRAP), 0, wxEXPAND);
                left->Add(identity, 1, wxEXPAND);
                grid->Add(left, 1, wxEXPAND);

                auto* right = new wxBoxSizer(wxVERTICAL);
                auto* picker = new wxBoxSizer(wxHORIZONTAL);
                auto* preview = new wxStaticBitmap(m_scroll, wxID_ANY, filament_swatch(*m_scroll, wxColour{}));
                picker->Add(preview, 0, wxRIGHT | wxTOP, FromDIP(10));
                auto* combo = new ::ComboBox(m_scroll, wxID_ANY, wxEmptyString, wxDefaultPosition,
                    FromDIP(wxSize(285, -1)), 0, nullptr, wxCB_READONLY);
                combo->SetFont(Label::Body_13);
                combo->SetKeepDropArrow(true);
                combo->GetDropDown().SetAlignIcon(true);
                combo->GetDropDown().SetUseContentWidth(true, true);
                combo->Append(_L("Choose a filament…"));
                auto candidates = rank_plate_filaments(source, from, destination);
                Choice choice{group_index, affected_moves, from, combo, std::move(candidates), {}, {}, preview, nullptr, nullptr};
                for (const auto& candidate : choice.candidates) {
                    const auto color = filament_color(destination, candidate.slot);
                    const auto label = format_wxstr(_L("%1%: %2%"), candidate.slot, filament_profile(destination, candidate.slot));
                    const int index = combo->Append(label, filament_swatch(*combo, color));
                    const auto detail = filament_details(destination, candidate.slot);
                    combo->SetItemTooltip(index, detail + "\n" + candidate_note(candidate));
                    choice.colors.push_back(color);
                    choice.details.push_back(detail);
                }
                const auto match = draft.moves[affected_moves.front()].transfer.filaments.find(from);
                int selected = 0;
                if (match != draft.moves[affected_moves.front()].transfer.filaments.end())
                    for (size_t n = 0; n < choice.candidates.size(); ++n)
                        if (choice.candidates[n].slot == match->second) selected = int(n + 1);
                combo->SetSelection(selected);
                picker->Add(combo, 1, wxEXPAND);
                right->Add(picker, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
                choice.detail = new Label(m_scroll, Label::Body_12, {}, LB_AUTO_WRAP);
                choice.detail->SetMinSize(FromDIP(wxSize(285, -1)));
                right->Add(choice.detail, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(4));
                choice.note = new Label(m_scroll, Label::Body_12, {}, LB_AUTO_WRAP);
                right->Add(choice.note, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
                grid->Add(right, 1, wxEXPAND);
                m_choices.push_back(std::move(choice));
                combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& event) { update_ready(); event.Skip(); });
            }
            content->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));
            ++group_index;
        }
        m_scroll->SetSizer(content);
        set_scroll_size();
        m_scroll->FitInside();
        outer->Add(m_scroll, 1, wxEXPAND);
        m_summary = new Label(this, Label::Body_13, {}, LB_AUTO_WRAP);
        outer->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
        m_buttons = new DialogButtons(this, {"OK", "Cancel"});
        m_buttons->GetOK()->SetLabel(_L("Move parts"));
        m_buttons->GetOK()->Bind(wxEVT_BUTTON, [this, &draft](wxCommandEvent&) {
            if (!ready()) return;
            for (const auto& choice : m_choices)
                for (size_t move : choice.moves)
                    draft.moves[move].transfer.filaments[choice.from] = choice.candidates[size_t(choice.combo->GetSelection() - 1)].slot;
            EndModal(wxID_OK);
        });
        outer->Add(m_buttons, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizerAndFit(outer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        update_ready();
    }
private:
    bool ready() const
    {
        return std::all_of(m_choices.begin(), m_choices.end(), [](const Choice& choice) {
            return choice.combo->GetSelection() > 0 && size_t(choice.combo->GetSelection()) <= choice.candidates.size();
        });
    }
    void update_ready()
    {
        size_t remaining = 0;
        bool merged = false;
        std::map<std::pair<size_t, int>, int> destinations;
        for (auto& choice : m_choices) {
            const int selected = choice.combo->GetSelection();
            const bool chosen = selected > 0 && size_t(selected) <= choice.candidates.size();
            choice.preview->SetBitmap(filament_swatch(*choice.preview, chosen ? choice.colors[size_t(selected - 1)] : wxColour{}));
            choice.detail->SetLabel(chosen ? choice.details[size_t(selected - 1)] : wxString{});
            wxString note;
            auto role = DesignSystem::ColorRole::InkMuted;
            if (chosen) {
                const auto& candidate = choice.candidates[size_t(selected - 1)];
                note = candidate_note(candidate);
                if (!candidate.same_material) role = DesignSystem::ColorRole::Warning;
                const auto inserted = destinations.emplace(std::make_pair(choice.group, candidate.slot), choice.from);
                merged |= !inserted.second && inserted.first->second != choice.from;
            } else {
                ++remaining;
                note = choice.candidates.empty() ? _L("Add a filament to the destination plate, then try again.") :
                    wxString{};
            }
            choice.note->SetLabel(note);
            choice.note->SetForegroundColour(transfer_color(role));
            choice.detail->SetForegroundColour(transfer_color(DesignSystem::ColorRole::InkMuted));
        }
        m_summary->SetLabel(remaining ? format_wxstr(_L_PLURAL("Choose a destination for %1% more filament.",
            "Choose a destination for %1% more filaments.", remaining), remaining) :
            merged ? _L("Some filaments will use the same destination color.") : _L("Ready to move. Review the colors above."));
        m_summary->SetForegroundColour(transfer_color(DesignSystem::ColorRole::Ink));
        m_buttons->GetOK()->Enable(ready());
        m_scroll->Layout();
        m_scroll->FitInside();
        Layout();
    }
    void set_scroll_size()
    {
        wxDisplay display(this);
        const int preferred = FromDIP(std::clamp(int(m_choices.size()) * 110 + 110, 240, 480));
        const int available = display.IsOk() ? display.GetClientArea().height - FromDIP(230) : preferred;
        m_scroll->SetMinSize(wxSize(FromDIP(760), std::min(preferred, std::max(FromDIP(160), available))));
    }
    void on_dpi_changed(const wxRect&) override
    {
        for (const auto& swatch : m_swatches) swatch.view->SetBitmap(filament_swatch(*swatch.view, swatch.color));
        for (const auto& choice : m_choices) {
            choice.combo->Rescale();
            choice.combo->SetFont(Label::Body_13);
            choice.combo->SetMinSize(FromDIP(wxSize(285, -1)));
            for (size_t i = 0; i < choice.colors.size(); ++i)
                choice.combo->SetItemBitmap(unsigned(i + 1), filament_swatch(*choice.combo, choice.colors[i]));
        }
        set_scroll_size();
        m_scroll->SetScrollRate(0, FromDIP(16));
        update_ready();
        GetSizer()->SetSizeHints(this);
    }
    void on_sys_color_changed() override
    {
        wxGetApp().UpdateDlgDarkUI(this);
        for (const auto& swatch : m_swatches) swatch.view->SetBitmap(filament_swatch(*swatch.view, swatch.color));
        update_ready();
        Refresh();
    }
};

enum class DraftState { Current, Busy, Changed };

DraftState draft_state(Plater& plater, const Draft& draft)
{
    if (!plater.background_process().can_switch_print() || plater.is_background_process_slicing() ||
        !plater.get_ui_job_worker().is_idle()) return DraftState::Busy;
    auto& plates = plater.get_partplate_list();
    if (plates.get_plate_count() != int(draft.setups.size()) || plates.get_curr_plate_index() != draft.active_plate)
        return DraftState::Changed;
    // A check must not change the project: read the active plate's live editing
    // context the way a capture would, without storing it on the plate.
    PlatePrintSetup live;
    if (plates.independent_setups()) live.capture(*wxGetApp().preset_bundle);
    for (size_t i = 0; i < draft.setups.size(); ++i) {
        auto* plate = plates.get_plate(int(i));
        const auto& config = int(i) == draft.active_plate && !live.empty() ? live.config : plate->print_setup().config;
        if (plate->id() != draft.plate_ids[i] || plate->get_origin() != draft.origins[i] ||
            config != draft.setups[i].config ||
            plate->print_setup().destination != draft.setups[i].destination ||
            *plate->config() != draft.overrides[i]) return DraftState::Changed;
    }
    return stamp_model(plater.model()) == draft.model ? DraftState::Current : DraftState::Changed;
}

// Explains why a queued move no longer applies. Returns true while it still does.
bool draft_applies(Plater& plater, const Draft& draft, bool after_choices)
{
    const DraftState state = draft_state(plater, draft);
    if (state == DraftState::Current) return true;
    MessageDialog dialog(&plater,
        state == DraftState::Busy ? _L("Wait for slicing or the current task to finish, then move the parts again.") :
        after_choices ? _L("The plates changed while you were choosing filaments. Move the parts again to use the current settings.") :
                        _L("The plates changed before the parts could move. Move them again to use the current settings."),
        _L("Move parts"), wxOK);
    dialog.ShowModal();
    return false;
}

std::vector<BoundingBoxf3> plate_volumes(PartPlateList& plates)
{
    std::vector<BoundingBoxf3> result;
    for (int i = 0; i < plates.get_plate_count(); ++i) result.push_back(plates.get_plate(i)->get_plate_box());
    return result;
}

bool crosses_plates(const std::vector<PlateMoveCandidate>& moves, const std::vector<int>& destinations)
{
    for (size_t i = 0; i < moves.size(); ++i)
        if (destinations[i] >= 0 && destinations[i] != moves[i].source) return true;
    return false;
}
}

std::vector<int> plate_move_destinations(const std::vector<PlateMoveCandidate>& moves,
                                         const std::vector<BoundingBoxf3>& plate_volumes)
{
    std::vector<int> result;
    result.reserve(moves.size());
    for (const auto& move : moves) {
        const auto touches = [&](int plate) { return plate_volumes[size_t(plate)].intersects(move.proposed); };
        int destination = move.source;
        if (destination < 0 || destination >= int(plate_volumes.size()) || !touches(destination)) {
            destination = -1;
            for (int plate = 0; plate < int(plate_volumes.size()); ++plate)
                if (touches(plate)) { destination = plate; break; }
        }
        result.push_back(destination);
    }
    return result;
}

bool defer_plate_transfer(GLCanvas3D& canvas, const std::string& snapshot_name)
{
    auto& plater = *wxGetApp().plater();
    auto& plates = plater.get_partplate_list();
    if (!plates.independent_setups() || canvas.get_canvas_type() != GLCanvas3D::CanvasView3D || !canvas.get_model()) return false;
    if (canvas.get_selection().get_mode() != Selection::Instance) {
        // A part transform is shared by all copies of an object. Transferring it
        // cannot be represented by remapping only this instance's materials.
        std::map<std::pair<int, int>, BoundingBoxf3> proposed;
        std::set<std::pair<int, int>> changed;
        const auto& objects = plater.model().objects;
        for (const auto* volume : canvas.get_volumes().volumes) {
            const int o = volume->object_idx(), n = volume->instance_idx(), v = volume->volume_idx();
            if (o < 0 || size_t(o) >= objects.size() || n < 0 || v < 0 || size_t(v) >= objects[size_t(o)]->volumes.size()) continue;
            if (objects[size_t(o)]->volumes[size_t(v)]->is_model_part())
                proposed[{o, n}].merge(volume->transformed_convex_hull_bounding_box());
            if (volume->get_volume_transformation() != objects[size_t(o)]->volumes[size_t(v)]->get_transformation())
                changed.emplace(o, n);
        }
        if (changed.empty()) return false;
        std::vector<PlateMoveCandidate> candidates;
        for (const auto& key : changed) candidates.push_back({plates.find_instance(key.first, key.second), proposed[key]});
        if (!crosses_plates(candidates, plate_move_destinations(candidates, plate_volumes(plates)))) return false;
        for (auto* volume : canvas.get_volumes().volumes)
            if (changed.count({volume->object_idx(), volume->instance_idx()}) && volume->volume_idx() >= 0)
                volume->set_volume_transformation(objects[size_t(volume->object_idx())]->volumes[size_t(volume->volume_idx())]->get_transformation());
        canvas.get_selection().set_bounding_boxes_dirty();
        canvas.set_as_dirty();
        const wxWeakRef<Plater> owner(&plater);
        plater.CallAfter([owner] {
            if (!owner || wxGetApp().is_closing()) return;
            MessageDialog dialog(owner.get(), _L("Select the whole object to move it to another plate. Its filaments can then be matched to the destination."),
                                 _L("Move to another plate"), wxOK);
            dialog.ShowModal();
        });
        return true;
    }
    auto draft = std::make_shared<Draft>();
    const auto& model = plater.model();
    std::map<std::pair<size_t, size_t>, BoundingBoxf3> bounds;
    std::map<std::pair<size_t, size_t>, Geometry::Transformation> transforms;
    for (const auto* volume : canvas.get_volumes().volumes) {
        const int o = volume->object_idx(), n = volume->instance_idx();
        if (o < 0 || size_t(o) >= model.objects.size() || n < 0 || size_t(n) >= model.objects[size_t(o)]->instances.size() || volume->volume_idx() < 0) continue;
        if (model.objects[size_t(o)]->instances[size_t(n)]->get_transformation() == volume->get_instance_transformation()) continue;
        const auto key = std::make_pair(size_t(o), size_t(n));
        transforms[key] = volume->get_instance_transformation();
        if (model.objects[size_t(o)]->volumes[size_t(volume->volume_idx())]->is_model_part())
            bounds[key].merge(volume->transformed_convex_hull_bounding_box());
    }
    if (transforms.empty()) return false;
    std::vector<PlateMoveCandidate> proposed;
    for (const auto& entry : transforms)
        proposed.push_back({plates.find_instance(int(entry.first.first), int(entry.first.second)), bounds[entry.first]});
    const auto destinations = plate_move_destinations(proposed, plate_volumes(plates));
    if (!crosses_plates(proposed, destinations)) return false;
    size_t index = 0;
    for (const auto& [key, transformation] : transforms) {
        draft->moves.push_back({{key.first, key.second, transformation, {}, 0}, proposed[index].source, destinations[index]});
        ++index;
    }

    plater.capture_plate_setup();
    draft->model = stamp_model(model);
    draft->undo_time = plater.undo_redo_stack_main().active_snapshot_time();
    draft->active_plate = plates.get_curr_plate_index();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        auto* plate = plates.get_plate(i);
        draft->setups.push_back(plate->print_setup());
        draft->overrides.push_back(*plate->config());
        draft->origins.push_back(plate->get_origin());
        draft->plate_ids.push_back(plate->id());
    }
    bool needs_choices = false;
    for (auto& move : draft->moves) {
        const auto& object = *model.objects[move.transfer.object_idx];
        if (move.destination < 0 || move.source == move.destination) {
            for (int slot : object_filament_slots(object)) move.transfer.filaments.emplace(slot, slot);
            move.transfer.destination_filament_count = size_t(*object_filament_slots(object).rbegin());
        } else {
            const auto& destination = draft->setups[size_t(move.destination)].config;
            move.transfer.destination_filament_count = plate_filament_count(destination);
            if (move.source >= 0)
                move.transfer.filaments = match_plate_filaments(object, draft->setups[size_t(move.source)].config, destination);
            needs_choices |= move.transfer.filaments.size() != object_filament_slots(object).size();
            if (move.source >= 0)
                for (const auto& [from, to] : move.transfer.filaments) {
                    const auto candidates = rank_plate_filaments(draft->setups[size_t(move.source)].config, from, destination);
                    const auto selected = std::find_if(candidates.begin(), candidates.end(),
                        [slot = to](const auto& candidate) { return candidate.slot == slot; });
                    needs_choices |= selected == candidates.end() || !selected->exact;
                }
        }
    }
    // End the drag with the original scene. The queued callback owns only a
    // draft; no modal is opened on a mouse stack and Cancel changes nothing.
    for (auto* volume : canvas.get_volumes().volumes) {
        const auto key = std::make_pair(size_t(volume->object_idx()), size_t(volume->instance_idx()));
        if (transforms.count(key)) volume->set_instance_transformation(model.objects[key.first]->instances[key.second]->get_transformation());
    }
    canvas.get_selection().set_bounding_boxes_dirty();
    canvas.set_as_dirty();
    const wxWeakRef<Plater> owner(&plater);
    plater.CallAfter([owner, draft, needs_choices, snapshot_name] {
        if (!owner || wxGetApp().is_closing()) return;
        auto& plater = *owner;
        if (!draft_applies(plater, *draft, false)) return;
        SuppressBackgroundProcessingUpdate suppress;
        if (needs_choices) {
            TransferDialog dialog(plater, *draft);
            if (dialog.ShowModal() != wxID_OK || !owner || wxGetApp().is_closing()) return;
            if (!draft_applies(plater, *draft, true)) return;
        }
        std::vector<PlateInstanceTransfer> transfers;
        for (const auto& move : draft->moves) transfers.push_back(move.transfer);
        try {
            validate_plate_instance_transfers(plater.model(), transfers);
        } catch (const std::invalid_argument&) {
            MessageDialog invalid(&plater, _L("The parts have filament assignments that cannot be moved safely. Check their filament assignments and try again."),
                                  _L("Move parts"), wxOK);
            invalid.ShowModal();
            return;
        }
        // An empty name means the caller recorded the state before this move.
        // That step covers the transfer unless another step was recorded since.
        if (!snapshot_name.empty())
            plater.take_snapshot(snapshot_name);
        else if (plater.undo_redo_stack_main().active_snapshot_time() != draft->undo_time)
            plater.take_snapshot(_u8L("Move to another plate"));
        Plater::SuppressSnapshots snapshots(&plater);
        const auto moved = apply_plate_instance_transfers(plater.model(), transfers);
        auto& plates = plater.get_partplate_list();
        for (const auto& move : draft->moves)
            for (int index : {move.source, move.destination})
                if (index >= 0) plates.get_plate(index)->update_slice_result_valid_state(false);
        auto* canvas = plater.get_view3D_canvas3D();
        auto& selection = canvas->get_selection();
        selection.clear();
        plates.reload_all_objects(false, -1, true);
        canvas->reload_scene(true, true);
        std::vector<std::pair<size_t, size_t>> selected;
        for (const auto& [o, n] : moved)
            for (const auto* volume : plater.model().objects[o]->volumes)
                selected.emplace_back(volume->id().id, plater.model().objects[o]->instances[n]->id().id);
        std::sort(selected.begin(), selected.end());
        selection.set_deserialized(Selection::Instance, selected);
        // The scene reload ran without a selection; refresh the fields that show it.
        canvas->get_gizmos_manager().update_data();
        plater.sidebar().obj_list()->reload_all_plates(false);
        canvas->reset_sequential_print_clearance();
        plates.invalidate_all_imex_ghosts();
        LifecycleEventContext context;
        context.code = LifecycleEvtCode::Ok;
        context.msg = "moved";
        fire_lifecycle_event(LifecycleEvent::ObjectTransformed, context);
        plater.update();
        canvas->set_as_dirty();
    });
    return true;
}
}} // namespace Slic3r::GUI
