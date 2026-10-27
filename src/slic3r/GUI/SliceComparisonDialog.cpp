#include "SliceComparisonDialog.hpp"
#include "DesignTokens.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "Typography.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"

#include <wx/colour.h>
#include <wx/brush.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace Slic3r::GUI {
namespace {
wxColour color(DesignSystem::ColorRole role)
{
    return wxColour(DesignSystem::color_hex(role, wxGetApp().dark_mode()));
}
class ComparisonPlot final : public wxPanel {
    ComparedPaths m_paths;
    double m_x0 = 0, m_y0 = 0, m_x1 = 1, m_y1 = 1, m_zoom = 1, m_center_x = .5, m_center_y = .5;
    bool m_available = false;
    double scale() const
    {
        const auto size = GetClientSize();
        return std::max(0., std::min((size.x - FromDIP(32)) / std::max(1., m_x1 - m_x0),
                                      (size.y - FromDIP(32)) / std::max(1., m_y1 - m_y0))) * m_zoom;
    }
public:
    explicit ComparisonPlot(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            const double old_scale = scale();
            m_zoom = std::clamp(m_zoom * (event.GetWheelRotation() > 0 ? 1.2 : 1. / 1.2), 1., 30.);
            const double new_scale = scale();
            if (old_scale > 0. && new_scale > 0.) {
                const auto size = GetClientSize();
                // Keep the point under the cursor fixed, including small parts
                // far from a purge line in the same slice.
                m_center_x += (event.GetX() - size.x / 2.) * (1. / old_scale - 1. / new_scale);
                m_center_y -= (event.GetY() - size.y / 2.) * (1. / old_scale - 1. / new_scale);
            }
            if (m_zoom == 1.) {
                m_center_x = (m_x0 + m_x1) / 2.; m_center_y = (m_y0 + m_y1) / 2.;
            }
            Refresh();
        });
    }
    bool AcceptsFocus() const override { return false; }
    void set_comparison(const SliceSnapshot& a, const SliceSnapshot& b)
    {
        m_available = a.paths_available && b.paths_available;
        m_x0 = m_y0 = std::numeric_limits<double>::max();
        m_x1 = m_y1 = std::numeric_limits<double>::lowest();
        for (const auto* snapshot : {&a, &b}) for (const auto& [z, layer] : snapshot->layers) for (const auto& p : layer.paths) {
            m_x0 = std::min({m_x0, double(p.x0), double(p.x1)});
            m_x1 = std::max({m_x1, double(p.x0), double(p.x1)});
            m_y0 = std::min({m_y0, double(p.y0), double(p.y1)});
            m_y1 = std::max({m_y1, double(p.y0), double(p.y1)});
        }
        m_zoom = 1;
        m_center_x = (m_x0 + m_x1) / 2.; m_center_y = (m_y0 + m_y1) / 2.;
    }
    void clear()
    {
        m_available = false;
        m_paths = {};
        Refresh();
    }
    void set_layer(const ComparedLayer* a, const ComparedLayer* b)
    {
        m_paths = m_available ? compare_slice_paths(a, b) : ComparedPaths{};
        Refresh();
    }
    void paint()
    {
        wxAutoBufferedPaintDC buffer(this);
        wxGCDC dc(buffer);
        dc.SetBackground(wxBrush(color(DesignSystem::ColorRole::Field))); dc.Clear();
        dc.SetFont(GetFont()); dc.SetTextForeground(color(DesignSystem::ColorRole::InkMuted));
        if (!m_available || m_x0 > m_x1) {
            dc.DrawText(_L("Individual toolpaths are unavailable for this comparison."), FromDIP(16), FromDIP(16));
            return;
        }
        const auto size = GetClientSize();
        const double display_scale = scale();
        const auto draw = [&](const std::vector<ComparedPath>& paths, DesignSystem::ColorRole role, int width) {
            dc.SetPen(wxPen(color(role), FromDIP(width)));
            for (const auto& p : paths) {
                const auto point = [&](float x, float y) {
                    return wxPoint(int(std::lround(size.x / 2. + (x - m_center_x) * display_scale)),
                                   int(std::lround(size.y / 2. - (y - m_center_y) * display_scale)));
                };
                dc.DrawLine(point(p.x0, p.y0), point(p.x1, p.y1));
            }
        };
        draw(m_paths.shared, DesignSystem::ColorRole::InkMuted, 1);
        draw(m_paths.before_only, DesignSystem::ColorRole::Info, 2);
        draw(m_paths.after_only, DesignSystem::ColorRole::Accent, 2);
        draw(m_paths.adjusted, DesignSystem::ColorRole::Warning, 2);
    }
};
wxString quantity(const std::optional<double>& value, const wxString& unit)
{
    return value ? wxString::Format("%.1f %s", *value, unit) : _L("Unavailable");
}
class SliceComparisonDialog final : public DPIDialog {
    SliceSnapshotPtr m_current, m_compared;
    std::vector<SliceComparisonSource> m_sources;
    ::ComboBox* m_source;
    wxListCtrl *m_totals, *m_settings, *m_features;
    wxNotebook* m_tabs;
    ComparisonPlot* m_plot;
    wxSlider* m_layer;
    Button* m_largest;
    wxStaticText *m_heading, *m_settings_note, *m_layer_note, *m_session_note;
    std::vector<wxStaticText*> m_labels;
    std::vector<std::pair<wxStaticText*, DesignSystem::ColorRole>> m_legend;
    DialogButtons* m_buttons;
    std::vector<int64_t> m_heights;
    void metric(const wxString& name, std::optional<double> a, std::optional<double> b, const wxString& unit)
    {
        const auto row = m_totals->InsertItem(m_totals->GetItemCount(), name);
        m_totals->SetItem(row, 1, quantity(a, unit)); m_totals->SetItem(row, 2, quantity(b, unit));
        m_totals->SetItem(row, 3, a && b ? wxString::Format("%+.1f %s", *b - *a, unit) : _L("Unavailable"));
    }
    void update_layer()
    {
        if (!m_compared || m_heights.empty()) {
            m_plot->set_layer(nullptr, nullptr);
            m_layer_note->SetLabel(_L("No printable layers are available."));
            return;
        }
        const auto z = m_heights.at(size_t(m_layer->GetValue()));
        const auto a = m_compared->layers.find(z), b = m_current->layers.find(z);
        m_plot->set_layer(a == m_compared->layers.end() ? nullptr : &a->second,
                          b == m_current->layers.end() ? nullptr : &b->second);
        wxString note = wxString::Format(_L("Height: %.3f mm"), z / 1000.);
        if (a == m_compared->layers.end()) note += " — " + _L("Only in the current slice");
        if (b == m_current->layers.end()) note += " — " + _L("Only in the compared slice");
        if (a != m_compared->layers.end() && b != m_current->layers.end() && a->second.seconds && b->second.seconds)
            note += wxString::Format(_L(" — Extrusion time change: %+.1f s"), *b->second.seconds - *a->second.seconds);
        m_layer_note->SetLabel(note);
    }
    void select_source()
    {
        const int selected = m_source->GetSelection();
        if (selected < 0 || size_t(selected) >= m_sources.size()) return;
        m_compared = m_sources[size_t(selected)].load();
        m_totals->DeleteAllItems(); m_settings->DeleteAllItems(); m_features->DeleteAllItems();
        if (!m_compared) {
            m_settings_note->SetLabel(_L("This slice is no longer available. Close this window and choose a completed slice."));
            m_heights.clear();
            m_layer->Enable(false);
            m_largest->Enable(false);
            m_layer_note->SetLabel(_L("This slice is no longer available."));
            m_plot->clear();
            return;
        }
        metric(_L("Estimated time"), m_compared->seconds ? std::optional<double>(*m_compared->seconds / 60.) : std::nullopt,
               m_current->seconds ? std::optional<double>(*m_current->seconds / 60.) : std::nullopt, _L("min"));
        metric(_L("Filament"), m_compared->grams, m_current->grams, "g");
        metric(_L("Layers"), double(m_compared->layers.size()), double(m_current->layers.size()), wxEmptyString);
        std::set<uint16_t> roles;
        for (const auto& [role, feature] : m_compared->features) roles.insert(role);
        for (const auto& [role, feature] : m_current->features) roles.insert(role);
        for (const auto role : roles) {
            const auto row = m_features->InsertItem(m_features->GetItemCount(),
                from_u8(ExtrusionEntity::role_to_string(ExtrusionRole(role))));
            for (int side = 0; side < 2; ++side) {
                const auto& features = side == 0 ? m_compared->features : m_current->features;
                const auto found = features.find(role);
                const std::optional<double> seconds = found == features.end() ? std::optional<double>(0.) :
                    found->second.time_available ? std::optional<double>(found->second.seconds / 60.) : std::nullopt;
                m_features->SetItem(row, 1 + side, quantity(seconds, _L("min")));
                m_features->SetItem(row, 3 + side, quantity(found == features.end() ? std::optional<double>(0.) : found->second.grams, "g"));
            }
        }
        const auto changes = compare_slice_settings(*m_compared, *m_current);
        for (const auto& change : changes) {
            const auto row = m_settings->InsertItem(m_settings->GetItemCount(), from_u8(change.label));
            m_settings->SetItem(row, 1, from_u8(change.before)); m_settings->SetItem(row, 2, from_u8(change.after));
        }
        m_settings_note->SetLabel(!m_compared->settings_available || !m_current->settings_available ?
            _L("Print settings weren't retained with one of these slices. Its toolpaths and available estimates can still be compared.") :
            changes.empty() ? _L("The retained print settings match.") : _L("These print settings changed between the two slices."));
        m_settings_note->Wrap(-1); m_settings_note->Wrap(FromDIP(760));
        m_heights = compared_layer_heights(*m_compared, *m_current);
        m_layer->SetRange(0, int(std::max(size_t(2), m_heights.size()) - 1));
        m_layer->SetValue(0); m_layer->Enable(m_heights.size() > 1);
        m_largest->Enable(std::any_of(m_heights.begin(), m_heights.end(), [&](int64_t z) {
            const auto a = m_compared->layers.find(z), b = m_current->layers.find(z);
            return a != m_compared->layers.end() && b != m_current->layers.end() && a->second.seconds && b->second.seconds;
        }));
        m_plot->set_comparison(*m_compared, *m_current);
        update_layer(); Layout();
    }
    void apply_style()
    {
        const auto bg = color(DesignSystem::ColorRole::Surface), ink = color(DesignSystem::ColorRole::Ink);
        SetBackgroundColour(bg); SetFont(DesignSystem::font(DesignSystem::TypeStyle::Body));
        for (auto* label : m_labels) {
            label->SetBackgroundColour(bg); label->SetForegroundColour(ink); label->SetFont(GetFont());
        }
        for (const auto& [label, role] : m_legend) label->SetForegroundColour(color(role));
        m_heading->SetFont(DesignSystem::font(DesignSystem::TypeStyle::Heading));
        m_session_note->SetForegroundColour(color(DesignSystem::ColorRole::InkMuted));
        for (auto* table : {m_totals, m_settings, m_features}) {
            table->SetBackgroundColour(color(DesignSystem::ColorRole::Field)); table->SetTextColour(ink); table->SetFont(GetFont());
        }
        const int totals[] = {220, 190, 190, 150};
        for (int i = 0; i < 4; ++i) m_totals->SetColumnWidth(i, FromDIP(totals[i]));
        const int settings[] = {310, 220, 220};
        for (int i = 0; i < 3; ++i) m_settings->SetColumnWidth(i, FromDIP(settings[i]));
        const int features[] = {260, 125, 125, 125, 125};
        for (int i = 0; i < 5; ++i) m_features->SetColumnWidth(i, FromDIP(features[i]));
        m_totals->SetMinSize(FromDIP(wxSize(760, 110)));
        m_tabs->SetMinSize(FromDIP(wxSize(760, 330)));
        m_source->SetMinSize(FromDIP(wxSize(500, 32)));
        m_plot->SetFont(GetFont()); m_plot->Refresh();
    }
    void on_dpi_changed(const wxRect&) override
    {
        m_source->Rescale(); m_largest->Rescale(); m_buttons->GetLAST()->Rescale(); apply_style(); GetSizer()->SetSizeHints(this); Layout();
    }
    void on_sys_color_changed() override { apply_style(); }
public:
    SliceComparisonDialog(wxWindow* parent, SliceSnapshotPtr current, std::vector<SliceComparisonSource> sources)
        : DPIDialog(parent, wxID_ANY, _L("Compare slices"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER),
          m_current(std::move(current)), m_sources(std::move(sources))
    {
        SetBackgroundColour(color(DesignSystem::ColorRole::Surface));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        const auto label = [&](wxWindow* owner, const wxString& text) {
            auto* value = new wxStaticText(owner, wxID_ANY, text); m_labels.push_back(value); return value;
        };
        m_heading = label(this, format_wxstr(_L("Current slice: %1%"), from_u8(m_current->label)));
        layout->Add(m_heading, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* source_row = new wxBoxSizer(wxHORIZONTAL);
        source_row->Add(label(this, _L("Compare with")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        m_source = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        for (const auto& source : m_sources) m_source->Append(from_u8(source.label));
        m_source->SetSelection(0); m_source->SetName(_L("Compare with"));
        m_source->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& event) { select_source(); event.Skip(); });
        source_row->Add(m_source, 1, wxEXPAND);
        layout->Add(source_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_totals = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        for (const auto& title : {_L("Estimate"), _L("Compared slice"), _L("Current slice"), _L("Change")}) m_totals->AppendColumn(title);
        layout->Add(m_totals, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_tabs = new wxNotebook(this, wxID_ANY);
        auto* paths = new wxPanel(m_tabs);
        paths->SetBackgroundColour(GetBackgroundColour());
        auto* path_layout = new wxBoxSizer(wxVERTICAL);
        auto* legend = new wxBoxSizer(wxHORIZONTAL);
        const std::pair<wxString, DesignSystem::ColorRole> items[] = {
            {_L("Shared"), DesignSystem::ColorRole::InkMuted}, {_L("Compared only"), DesignSystem::ColorRole::Info},
            {_L("Current only"), DesignSystem::ColorRole::Accent}, {_L("Changed printing"), DesignSystem::ColorRole::Warning}};
        for (const auto& [text, role] : items) {
            auto* item = label(paths, text); item->SetForegroundColour(color(role)); m_legend.emplace_back(item, role); legend->Add(item, 0, wxRIGHT, FromDIP(20));
        }
        legend->AddStretchSpacer();
        m_largest = new Button(paths, _L("Largest time change"));
        m_largest->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        m_largest->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            double largest = -1.; size_t selected = 0;
            for (size_t i = 0; i < m_heights.size(); ++i) {
                const auto a = m_compared->layers.find(m_heights[i]), b = m_current->layers.find(m_heights[i]);
                if (a == m_compared->layers.end() || b == m_current->layers.end() || !a->second.seconds || !b->second.seconds) continue;
                const double change = std::abs(*b->second.seconds - *a->second.seconds);
                if (change > largest) { largest = change; selected = i; }
            }
            m_layer->SetValue(int(selected)); update_layer();
            event.Skip();
        });
        legend->Add(m_largest, 0, wxALIGN_CENTER_VERTICAL);
        path_layout->Add(legend, 0, wxEXPAND | wxALL, FromDIP(12));
        m_plot = new ComparisonPlot(paths);
        m_plot->SetToolTip(_L("Scroll to zoom the toolpaths."));
        path_layout->Add(m_plot, 1, wxEXPAND);
        m_layer_note = label(paths, wxEmptyString); path_layout->Add(m_layer_note, 0, wxALL, FromDIP(8));
        m_layer = new wxSlider(paths, wxID_ANY, 0, 0, 1); m_layer->SetName(_L("Comparison height"));
        m_layer->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_layer(); });
        path_layout->Add(m_layer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        paths->SetSizer(path_layout); m_tabs->AddPage(paths, _L("Toolpaths"));
        auto* settings = new wxPanel(m_tabs); settings->SetBackgroundColour(GetBackgroundColour());
        auto* settings_layout = new wxBoxSizer(wxVERTICAL);
        m_settings_note = label(settings, wxEmptyString); settings_layout->Add(m_settings_note, 0, wxEXPAND | wxALL, FromDIP(12));
        m_settings = new wxListCtrl(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        for (const auto& title : {_L("Setting"), _L("Compared slice"), _L("Current slice")}) m_settings->AppendColumn(title);
        settings_layout->Add(m_settings, 1, wxEXPAND); settings->SetSizer(settings_layout); m_tabs->AddPage(settings, _L("Settings"));
        auto* features = new wxPanel(m_tabs); features->SetBackgroundColour(GetBackgroundColour());
        auto* features_layout = new wxBoxSizer(wxVERTICAL);
        features_layout->Add(label(features, _L("Extrusion time excludes travel, heating and printer preparation.")), 0, wxALL, FromDIP(12));
        m_features = new wxListCtrl(features, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        for (const auto& title : {_L("Print feature"), _L("Compared time"), _L("Current time"), _L("Compared filament"), _L("Current filament")})
            m_features->AppendColumn(title);
        features_layout->Add(m_features, 1, wxEXPAND); features->SetSizer(features_layout); m_tabs->AddPage(features, _L("Print features"));
        layout->Add(m_tabs, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        m_session_note = label(this, _L("Comparisons are kept for this session."));
        layout->Add(m_session_note, 0, wxEXPAND | wxALL, FromDIP(16));
        m_buttons = new DialogButtons(this, {"Close"});
        m_buttons->GetLAST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { EndModal(wxID_CLOSE); event.Skip(); });
        layout->Add(m_buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        wxGetApp().UpdateDlgDarkUI(this); apply_style(); select_source();
        SetSizerAndFit(layout); CenterOnParent();
    }
};
}
void show_slice_comparison(wxWindow* parent, SliceSnapshotPtr current, std::vector<SliceComparisonSource> sources)
{
    if (!current || sources.empty()) return;
    SliceComparisonDialog dialog(parent, std::move(current), std::move(sources));
    dialog.ShowModal();
}
} // namespace Slic3r::GUI
