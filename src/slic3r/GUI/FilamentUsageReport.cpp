#include "FilamentUsageReport.hpp"

#include "DesignTokens.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Typography.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/FilamentUsage.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <wx/clipbrd.h>
#include <wx/colour.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

namespace {
std::string string_at(const DynamicPrintConfig& config, const char* key, size_t slot)
{
    const auto* option = config.option<ConfigOptionStrings>(key);
    return option && slot < option->values.size() ? option->values[slot] : std::string();
}

bool same_slice_value(const DynamicPrintConfig& config, const char* key, size_t slot, double value)
{
    const auto* option = config.option<ConfigOptionFloats>(key);
    // The parser can supply defaults when source metadata is absent. Require
    // matching configured values instead of treating those defaults as measured.
    return option && slot < option->values.size() && std::isfinite(value) &&
        std::abs(option->values[slot] - value) <= 1e-5 * std::max(1.0, std::abs(value));
}

PlateFilamentUsage collect_plate(PartPlate& plate, const DynamicPrintConfig& config)
{
    PlateFilamentUsage input;
    input.plate_id = plate.id().id;
    auto* result = plate.get_slice_result();
    input.fresh = plate.is_slice_result_valid() && !plate.is_apply_result_invalid() && plate.has_printable_instances() && result;
    if (!input.fresh) return input;
    const auto* mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    if (!result->used_mixed_filaments.empty() && !mixed) {
        input.fresh = false;
        return input;
    }
    for (const auto& [slot, volume] : result->print_statistics.total_volumes_per_extruder) {
        if (volume == 0) continue;
        FilamentUsageMeasurement measurement;
        measurement.slot = slot;
        auto& identity = measurement.identity;
        identity.material = string_at(config, "filament_type", slot);
        identity.finish = string_at(config, "filament_finish", slot);
        identity.brand = string_at(config, "filament_vendor", slot);
        identity.profile = string_at(config, "filament_settings_id", slot);
        identity.color = string_at(config, "filament_colour", slot);
        const std::string multi_color = string_at(config, "filament_multi_colour", slot);
        if (!multi_color.empty()) identity.color += " / " + multi_color;
        const std::string color_type = string_at(config, "filament_colour_type", slot);
        if (color_type == "0") identity.color += " · " + into_u8(_L("Gradient"));
        identity.known = !identity.brand.empty() && identity.brand != "(Undefined)" && identity.brand != into_u8(_L("(Undefined)"));
        identity.diameter_mm = slot < result->filament_diameters.size() ? result->filament_diameters[slot] : 0;
        measurement.density_g_cm3 = slot < result->filament_densities.size() ? result->filament_densities[slot] : 0;
        measurement.volume_mm3 = volume;
        // Slicing resolves virtual colors into physical tool changes. Trust the
        // measured physical totals; never split a virtual total by nominal ratios.
        measurement.physical = !mixed || (slot < mixed->values.size() && !mixed->values[slot]);
        if (!same_slice_value(config, "filament_diameter", slot, identity.diameter_mm) ||
            !same_slice_value(config, "filament_density", slot, measurement.density_g_cm3) ||
            (slot < result->settings_ids.filament.size() && !result->settings_ids.filament[slot].empty() &&
             !identity.profile.empty() && result->settings_ids.filament[slot] != identity.profile)) {
            input.fresh = false;
            input.filaments.clear();
            return input;
        }
        input.filaments.push_back(std::move(measurement));
    }
    return input;
}

std::string csv_field(const std::string& value)
{
    std::string escaped = "\"";
    for (char c : value) {
        if (c == '"') escaped += '"';
        escaped += c;
    }
    return escaped + '"';
}

class FilamentUsageDialog final : public DPIDialog
{
    FilamentUsageReport m_report;
    std::map<size_t, wxString> m_plate_names;
    wxListCtrl* m_table;
    DialogButtons* m_buttons;
    wxStaticText* m_scope;
    wxStaticText* m_note;
    wxStaticText* m_incomplete = nullptr;

    wxString plate_name(size_t id) const
    {
        const auto found = m_plate_names.find(id);
        return found == m_plate_names.end() ? _L("Unavailable plate") : found->second;
    }

    wxString filament_name(const FilamentUsageRow& row) const
    {
        if (row.identity.known && !row.identity.profile.empty() && !row.identity.material.empty() && !row.identity.color.empty())
            return from_u8(row.identity.profile);
        return plate_name(row.fallback_plate_id) + " · " + wxString::Format(_L("Filament %d"), int(row.fallback_slot + 1));
    }

    wxString quantity(double value, const FilamentUsageRow& row) const
    {
        if (!row.has_quantity) return _L("Unavailable");
        const wxString number = wxString::Format("%.2f", value);
        return row.complete && m_report.complete() ? number : wxString::Format(_L("At least %s"), number);
    }

    wxString csv() const
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "Filament,Material,Finish,Color,Brand,Diameter (mm),Grams,Meters,Estimate\r\n" << std::fixed << std::setprecision(4);
        for (const auto& row : m_report.rows) {
            out << csv_field(into_u8(filament_name(row))) << ',' << csv_field(row.identity.material) << ','
                << csv_field(row.identity.finish) << ',' << csv_field(row.identity.color) << ',' << csv_field(row.identity.brand) << ',';
            if (std::isfinite(row.identity.diameter_mm) && row.identity.diameter_mm > 0) out << row.identity.diameter_mm;
            out << ',';
            if (row.has_quantity) out << row.grams;
            out << ',';
            if (row.has_quantity) out << row.meters;
            out << ',' << (row.complete && m_report.complete() ? "Complete" : "Incomplete") << "\r\n";
        }
        for (size_t id : m_report.incomplete_plates)
            out << csv_field(into_u8(plate_name(id))) << ",,,,,,,,Incomplete\r\n";
        return from_u8(out.str());
    }

    void apply_style()
    {
        using namespace DesignSystem;
        const bool dark = wxGetApp().dark_mode();
        const wxColour surface(color_hex(ColorRole::Surface, dark));
        const wxColour ink(color_hex(ColorRole::Ink, dark));
        SetBackgroundColour(surface);
        SetFont(font(TypeStyle::Body));
        for (wxStaticText* label : {m_scope, m_note, m_incomplete}) {
            if (!label) continue;
            label->SetBackgroundColour(surface);
            label->SetForegroundColour(ink);
            label->SetFont(font(label == m_scope ? TypeStyle::Heading : TypeStyle::Body));
        }
        m_note->SetForegroundColour(wxColour(color_hex(ColorRole::InkMuted, dark)));
        if (m_incomplete) {
            m_incomplete->Wrap(-1);
            m_incomplete->Wrap(FromDIP(660));
        }
        m_table->SetBackgroundColour(wxColour(color_hex(ColorRole::Field, dark)));
        m_table->SetTextColour(ink);
        m_table->SetFont(font(TypeStyle::Body));
        const int widths[] = {240, 120, 140, 100, 100, 100};
        for (int column = 0; column < 6; ++column) m_table->SetColumnWidth(column, FromDIP(widths[column]));
        m_table->SetMinSize(FromDIP(wxSize(680, 240)));
        for (int i = 0; i < 2; ++i) m_buttons->GetButtonFromIndex(i)->SetFont(font(TypeStyle::Body));
    }

    void on_dpi_changed(const wxRect&) override
    {
        apply_style();
        for (int i = 0; i < 2; ++i) m_buttons->GetButtonFromIndex(i)->Rescale();
        GetSizer()->SetSizeHints(this);
        Layout();
    }
    void on_sys_color_changed() override { apply_style(); }

public:
    FilamentUsageDialog(wxWindow* parent, FilamentUsageReport report,
                        std::map<size_t, wxString> plate_names, const wxString& scope)
        : DPIDialog(parent, wxID_ANY, _L("Filament needed"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER),
          m_report(std::move(report)), m_plate_names(std::move(plate_names))
    {
        SetBackgroundColour(wxColour(DesignSystem::color_hex(DesignSystem::ColorRole::Surface, wxGetApp().dark_mode())));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        m_scope = new wxStaticText(this, wxID_ANY, scope);
        layout->Add(m_scope, 0, wxEXPAND | wxALL, FromDIP(16));
        if (!m_report.complete()) {
            m_incomplete = new wxStaticText(this, wxID_ANY,
                _L("Estimate incomplete. Slice the remaining plates or check their filament settings."));
            m_incomplete->Wrap(FromDIP(660));
            layout->Add(m_incomplete, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }
        m_table = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        m_table->AppendColumn(_L("Filament"));
        m_table->AppendColumn(_L("Material"));
        m_table->AppendColumn(_L("Color"));
        m_table->AppendColumn(_L("Brand"));
        m_table->AppendColumn(_L("Needed (g)"), wxLIST_FORMAT_RIGHT);
        m_table->AppendColumn(_L("Length (m)"), wxLIST_FORMAT_RIGHT);
        for (const auto& row : m_report.rows) {
            const long index = m_table->InsertItem(m_table->GetItemCount(), filament_name(row));
            wxString material = from_u8(row.identity.material);
            if (!row.identity.finish.empty()) material += " · " + from_u8(row.identity.finish);
            m_table->SetItem(index, 1, material.empty() ? _L("Not specified") : material);
            m_table->SetItem(index, 2, from_u8(row.identity.color));
            m_table->SetItem(index, 3, from_u8(row.identity.brand));
            m_table->SetItem(index, 4, quantity(row.grams, row));
            m_table->SetItem(index, 5, quantity(row.meters, row));
        }
        for (size_t id : m_report.incomplete_plates) {
            const long index = m_table->InsertItem(m_table->GetItemCount(), plate_name(id));
            m_table->SetItem(index, 1, _L("Needs attention"));
            m_table->SetItem(index, 4, _L("Unavailable"));
            m_table->SetItem(index, 5, _L("Unavailable"));
        }
        layout->Add(m_table, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        m_note = new wxStaticText(this, wxID_ANY, m_report.rows.empty()
            ? (m_report.complete() ? _L("No filament is needed for this selection.") : _L("No estimate is available for this selection."))
            : _L("Includes modeled support and purge. Actual use depends on your printer and filament."));
        layout->Add(m_note, 0, wxEXPAND | wxALL, FromDIP(16));
        m_buttons = new DialogButtons(this, {"Copy", "OK"}, "", 1);
        auto* copy = m_buttons->GetButtonFromID(wxID_COPY);
        copy->SetLabel(_L("Copy CSV"));
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            wxClipboardLocker clipboard;
            if (!clipboard) { event.Skip(); return; }
            wxTheClipboard->SetData(new wxTextDataObject(csv()));
            event.Skip();
        });
        m_buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { EndModal(wxID_OK); event.Skip(); });
        layout->Add(m_buttons, 0, wxEXPAND);
        // The dark-mode walk rewrites colours, so the design colours and fonts
        // follow it; fitting afterwards measures the final fonts.
        wxGetApp().UpdateDlgDarkUI(this);
        apply_style();
        SetSizerAndFit(layout);
        CenterOnParent();
    }
};
}

void show_filament_usage_report(wxWindow* parent, PartPlateList& plates,
                                const std::vector<ObjectID>& plate_ids, const wxString& scope_label)
{
    std::vector<PlateFilamentUsage> inputs;
    std::map<size_t, wxString> names;
    DynamicPrintConfig shared;
    if (!plates.independent_setups()) shared = wxGetApp().preset_bundle->full_config_secure();
    for (ObjectID id : plate_ids) {
        const auto& list = plates.get_plate_list();
        const auto found = std::find_if(list.begin(), list.end(), [id](const PartPlate* plate) { return plate && plate->id() == id; });
        if (found == list.end()) {
            inputs.push_back({id.id, false, {}});
            continue;
        }
        PartPlate& plate = **found;
        // get_objects() is the shared model, not membership of this plate.
        // Explicitly excluded models require no filament; printable instances
        // outside the bed still need attention and must not disappear here.
        if (plate.empty() || plate.is_all_instances_unprintable()) continue;
        names[id.id] = from_u8(plate.get_plate_name());
        if (names[id.id].empty()) names[id.id] = wxString::Format(_L("Plate %d"), plate.get_index() + 1);
        inputs.push_back(collect_plate(plate, plates.independent_setups() ? plate.print_setup().config : shared));
    }
    FilamentUsageDialog dialog(parent, aggregate_filament_usage(inputs), std::move(names), scope_label);
    dialog.ShowModal();
}

}} // namespace Slic3r::GUI
