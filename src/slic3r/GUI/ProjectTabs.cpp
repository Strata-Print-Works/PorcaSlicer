#include "PartPlate.hpp"
#include "ProjectTabs.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include "Notebook.hpp"
#include "ParamsDialog.hpp"
#include "ParamsPanel.hpp"
#include "Plater.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/AppConfig.hpp"
#include <Eigen/Geometry>
#include "Tab.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StateHandler.hpp"
#include "Typography.hpp"
#include "DesignTokens.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <exception>
#include <ctime>
#include <cstdint>
#include <cmath>
#ifdef SLIC3R_CAD
#include "CAD/DesignPanel.hpp"
#endif

#include <algorithm>
#include <utility>
#include <boost/filesystem/operations.hpp>
#include <wx/menu.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/brush.h>
#include <wx/pen.h>
#include <wx/control.h>
#include <wx/containr.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
namespace {
wxColour tab_color(DesignSystem::ColorRole role)
{
    return wxColour(DesignSystem::color_hex(role, wxGetApp().dark_mode()));
}

void style_tab_action(Button& button, const wxColour& background)
{
    using Role = DesignSystem::ColorRole;
    button.SetCornerRadius(button.FromDIP(5));
    button.SetBorderWidth(button.FromDIP(1));
    button.SetBorderColor(StateColor::resolved(StateColor(
        std::make_pair(tab_color(Role::Focus), int(StateColor::Focused)),
        std::make_pair(background, int(StateColor::Normal)))));
    button.SetBackgroundColor(StateColor::resolved(StateColor(
        std::make_pair(tab_color(Role::SurfaceHover), int(StateColor::Hovered)),
        std::make_pair(background, int(StateColor::Normal)))));
    button.SetTextColor(StateColor::resolved(tab_color(Role::InkMuted)));
}

// Keep Orca's button activation, capture and keyboard behavior. Only the tab's
// paint surface differs: its open bottom edge joins the workspace below it.
class ProjectTab final : public wxNavigationEnabled<Button>
{
public:
    Button* close_button;

    explicit ProjectTab(wxWindow* parent)
    {
        Create(parent, wxEmptyString, wxEmptyString, wxTAB_TRAVERSAL);
        SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        SetBackgroundColour(tab_color(DesignSystem::ColorRole::Bg));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        close_button = new Button(this, wxEmptyString, "project_tab_close", 0, 12);
        close_button->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
        close_button->SetName(_L("Close project"));
        close_button->SetToolTip(_L("Close project"));
        Bind(wxEVT_PAINT, &ProjectTab::paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { position_close(); Refresh(); e.Skip(); });
        rescale();
        update(false, false);
    }

    void SetLabel(const wxString& label) override
    {
        if (label == GetLabel()) return;
        // Measuring the full filename would expand the tab before ellipsizing.
        wxWindow::SetLabel(label);
        Refresh();
    }

    void rescale()
    {
        SetFont(DesignSystem::font(DesignSystem::TypeStyle::Label));
        SetMaxSize(FromDIP(wxSize(205, 38)));
        SetMinSize(FromDIP(wxSize(205, 38)));
        close_button->Rescale();
        close_button->SetMaxSize(FromDIP(wxSize(28, 28)));
        close_button->SetMinSize(FromDIP(wxSize(28, 28)));
        position_close();
    }

    void update(bool selected, bool dirty)
    {
        using Role = DesignSystem::ColorRole;
        const bool dark = wxGetApp().dark_mode();
        if (m_initialized && selected == m_active && dirty == m_dirty && dark == m_dark) return;
        m_initialized = true;
        m_active = selected;
        m_dirty = dirty;
        m_dark = dark;
        const auto background = tab_color(selected ? Role::Surface : Role::Bg);
        SetBackgroundColour(tab_color(Role::Bg));
        close_button->SetBackgroundColour(background);
        style_tab_action(*close_button, background);
        close_button->SetIcon("project_tab_close");
        Refresh();
    }

private:
    bool m_active {false};
    bool m_dirty {false};
    bool m_dark {false};
    bool m_initialized {false};

    void position_close()
    {
        const auto size = GetClientSize();
        close_button->SetSize(size.x - FromDIP(33), (size.y - FromDIP(28)) / 2, FromDIP(28), FromDIP(28));
    }

    void paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        using Role = DesignSystem::ColorRole;
        dc.SetBackground(wxBrush(tab_color(Role::Bg)));
        dc.Clear();
        const auto size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        const bool hovered = (state_handler.states() & StateHandler::Hovered) != 0;
        const auto fill = tab_color(m_active ? Role::Surface : hovered ? Role::SurfaceHover : Role::Bg);
        const int radius = FromDIP(7);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            auto path = gc->CreatePath();
            const double right = size.x - 0.5;
            path.MoveToPoint(0.5, size.y);
            path.AddLineToPoint(0.5, radius);
            path.AddQuadCurveToPoint(0.5, 0.5, radius, 0.5);
            path.AddLineToPoint(right - radius, 0.5);
            path.AddQuadCurveToPoint(right, 0.5, right, radius);
            path.AddLineToPoint(right, size.y);
            gc->SetBrush(wxBrush(fill));
            gc->FillPath(path);
            gc->SetPen(wxPen(tab_color(m_active ? Role::LineControl : Role::Bg), FromDIP(1)));
            gc->StrokePath(path);
            if (m_active) {
                gc->SetPen(wxPen(tab_color(Role::Accent), FromDIP(2)));
                gc->StrokeLine(radius, FromDIP(1), size.x - radius, FromDIP(1));
            }
        }
        gc.reset();
        if (!m_active) {
            dc.SetPen(wxPen(tab_color(Role::Line), FromDIP(1)));
            dc.DrawLine(size.x - 1, FromDIP(10), size.x - 1, size.y - FromDIP(10));
            dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
        }
        dc.SetFont(GetFont());
        dc.SetTextForeground(tab_color(m_active ? Role::Ink : Role::InkMuted));
        const int left = FromDIP(14);
        const int reserve = FromDIP(m_dirty ? 62 : 43);
        const auto label = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END, std::max(0, size.x - left - reserve));
        const auto text_size = dc.GetTextExtent(label);
        dc.DrawText(label, left, (size.y - text_size.y) / 2);
        if (m_dirty) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(tab_color(Role::InkMuted)));
            dc.DrawCircle(size.x - FromDIP(47), size.y / 2, FromDIP(3));
        }
        if (HasFocus()) {
            dc.SetPen(wxPen(tab_color(Role::Focus), FromDIP(1), wxPENSTYLE_DOT));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(FromDIP(5), FromDIP(5), size.x - FromDIP(43), size.y - FromDIP(10), FromDIP(3));
        }
    }
};

bool same_file(const wxString& a, const wxString& b)
{
    if (a.empty() || b.empty()) return false;
    const auto pa = into_path(a), pb = into_path(b);
    boost::system::error_code ec;
    if (boost::filesystem::equivalent(pa, pb, ec) && !ec) return true;
    ec.clear();
    auto ca = boost::filesystem::weakly_canonical(pa, ec);
    if (ec) return false;
    auto cb = boost::filesystem::weakly_canonical(pb, ec);
    return !ec && ca == cb;
}

nlohmann::json view_state(Plater& plater)
{
    auto& camera = plater.get_camera();
    const auto target = camera.get_target();
    const auto rotation = camera.get_view_rotation();
    nlohmann::json selection = nlohmann::json::array();
    for (const auto& [object, instances] : plater.get_selection().get_content())
        for (const auto instance : instances) selection.push_back({object, instance});
    return {{"target", {target.x(), target.y(), target.z()}}, {"zoom", camera.get_zoom()},
        {"rotation", {rotation.w(), rotation.x(), rotation.y(), rotation.z()}},
        {"plate", plater.get_partplate_list().get_curr_plate_index()}, {"selection", std::move(selection)}};
}

void restore_view_state(Plater& plater, const nlohmann::json& state)
{
    if (!state.is_object()) return;
    const int plate = state.value("plate", -1);
    if (plate >= 0 && plate < plater.get_partplate_list().get_plate_count()) plater.select_plate(plate);
    if (state.contains("selection") && state["selection"].is_array()) {
        auto& selection = plater.get_view3D_canvas3D()->get_selection();
        selection.clear();
        const auto& objects = plater.model().objects;
        for (const auto& entry : state["selection"]) {
            if (!entry.is_array() || entry.size() != 2) continue;
            const auto object = entry[0].get<std::size_t>(), instance = entry[1].get<std::size_t>();
            if (object < objects.size() && instance < objects[object]->instances.size())
                selection.add_instance(unsigned(object), unsigned(instance), false);
        }
    }
    if (!state.contains("target") || !state.contains("rotation")) return;
    const auto target = state["target"].get<std::vector<double>>();
    const auto rotation = state["rotation"].get<std::vector<double>>();
    const double zoom = state.value("zoom", 0.0);
    if (target.size() != 3 || rotation.size() != 4 || !std::isfinite(zoom) || zoom <= 0.0 ||
        !std::all_of(target.begin(), target.end(), [](double n) { return std::isfinite(n); }) ||
        !std::all_of(rotation.begin(), rotation.end(), [](double n) { return std::isfinite(n); })) return;
    Eigen::Quaterniond quaternion(rotation[0], rotation[1], rotation[2], rotation[3]);
    if (quaternion.norm() < 1e-6) return;
    Transform3d transform = Transform3d::Identity();
    transform.linear() = quaternion.normalized().toRotationMatrix();
    auto& camera = plater.get_camera();
    camera.set_rotation(transform);
    camera.set_target(Vec3d(target[0], target[1], target[2]));
    camera.set_zoom(zoom);
    camera.requires_zoom_to_bed = false;
    camera.requires_zoom_to_volumes = false;
    camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_PLATE_IDLE;
    plater.get_view3D_canvas3D()->set_as_dirty();
}
}

struct ProjectTabs::Workspace
{
    std::uint64_t id {0};
    Plater* plater {nullptr};
    ParamsPanel* panel {nullptr};
    ParamsDialog* dialog {nullptr};
    std::shared_ptr<PresetBundle> presets;
    std::vector<Tab*> tabs;
    std::vector<Tab*> model_tabs;
    Tab* plate_tab {nullptr};
    wxString view {TAB_ID_PREPARE};
    wxString untitled;
    wxString viewer_path;
    std::string checkpoint;
    wxString stamped_path;
    std::time_t modified {0};
    std::uintmax_t size {0};
    bool stamped {false};
    bool discarded {false};
    bool selected {false};
    bool dark {false};
    double dpi {1.0};
    Button* button {nullptr};
    Button* close_button {nullptr};
    ProjectTab* tab {nullptr};
#ifdef SLIC3R_CAD
    LazyPage<DesignPanel>* design {nullptr};
#endif
};

struct ProjectTabs::UI
{
    wxPanel* panel;
    wxScrolledWindow* scroll;
    wxBoxSizer* tabs;
    wxTimer timer;
    Button* add {nullptr};
    Button* list {nullptr};
    bool dark {false};
    double dpi {0.0};
    PresetBundle* app_presets;
    nlohmann::json previous_session;
    nlohmann::json pending_recovery = nlohmann::json::array();
    std::string last_session;
    bool restore_checked {false};
    bool preserve_manifest {false};
    bool restoring {false};
    explicit UI(MainFrame& frame) : panel(new wxPanel(&frame)),
        scroll(new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL)),
        tabs(new wxBoxSizer(wxHORIZONTAL)), timer(panel), app_presets(wxGetApp().preset_bundle)
    {
        panel->SetBackgroundColour(tab_color(DesignSystem::ColorRole::Bg));
        scroll->SetBackgroundColour(panel->GetBackgroundColour());
        scroll->SetScrollRate(panel->FromDIP(20), 0);
        scroll->SetSizer(tabs);
        scroll->SetMinSize(panel->FromDIP(wxSize(0, 44)));
        scroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(scroll, 0, wxEXPAND | wxLEFT, panel->FromDIP(8));
        panel->SetSizer(sizer);
        const auto manifest = boost::filesystem::path(data_dir()) / "project-session.json";
        std::ifstream input(manifest.string());
        if (input) {
            try {
                input >> previous_session;
                preserve_manifest = !previous_session.is_object() || previous_session.value("version", 0) != 1 ||
                    !previous_session.contains("projects") || !previous_session["projects"].is_array() ||
                    (previous_session.contains("clean") && !previous_session["clean"].is_boolean()) ||
                    (previous_session.contains("recreate") && !previous_session["recreate"].is_boolean()) ||
                    (previous_session.contains("active") && !previous_session["active"].is_number_unsigned() &&
                     !previous_session["active"].is_number_integer());
            } catch (const std::exception&) { preserve_manifest = true; }
        }
    }
};

ProjectTabs::ProjectTabs(MainFrame& frame) : m_frame(frame), m_ui(std::make_unique<UI>(frame))
{
    auto first = std::make_unique<Workspace>();
    first->presets = std::shared_ptr<PresetBundle>(wxGetApp().preset_bundle, [](PresetBundle*) {});
    first->untitled = format_wxstr(_L("Untitled %1%"), m_untitled++);
    capture(*first);
    m_workspaces.push_back(std::move(first));
    auto* add = m_ui->add = new Button(m_ui->panel, wxEmptyString, "project_tab_add", 0, 14);
    add->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    add->SetToolTip(_L("New project"));
    add->SetName(_L("New project"));
    m_ui->panel->GetSizer()->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxALL, m_ui->panel->FromDIP(4));
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        e.Skip();
        m_ui->panel->CallAfter([this] { if (!m_shutdown) new_project(); });
    });
    m_ui->panel->GetSizer()->AddStretchSpacer();
    auto* list = m_ui->list = new Button(m_ui->panel, wxEmptyString, "project_tab_list", 0, 14);
    list->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    list->SetName(_L("Open projects"));
    list->SetToolTip(_L("Open projects"));
    m_ui->panel->GetSizer()->Add(list, 0, wxALIGN_CENTER_VERTICAL | wxALL, m_ui->panel->FromDIP(4));
    list->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        e.Skip();
        m_ui->panel->CallAfter([this] { if (!m_shutdown) project_list(); });
    });
    m_ui->panel->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh(); });
    m_ui->panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { layout_tabs(); e.Skip(); });
    m_ui->timer.Start(250);
    refresh();
}

ProjectTabs::~ProjectTabs() { shutdown(); }
wxWindow* ProjectTabs::window() const { return m_ui->panel; }

void ProjectTabs::capture(Workspace& w)
{
    auto& app = wxGetApp();
    w.plater = m_frame.m_plater;
    w.id = w.plater->project_session_id();
    w.panel = m_frame.m_param_panel;
    w.dialog = m_frame.m_param_dialog;
    w.tabs = app.tabs_list;
    w.model_tabs = app.model_tabs_list;
    w.plate_tab = app.plate_tab;
    w.dark = app.dark_mode();
    w.dpi = m_frame.GetDPIScaleFactor();
#ifdef SLIC3R_CAD
    w.design = m_frame.m_design_page;
#endif
    const auto view = m_frame.m_tabpanel->GetSelectedPageName();
    if (w.checkpoint.empty()) {
        const auto name = std::to_string(get_current_pid()) + "-" + std::to_string(w.plater->model().id().id);
        w.checkpoint = (boost::filesystem::path(data_dir()) / "ProjectSessions" / name / "checkpoint.3mf").string();
    }
    if (view == TAB_ID_PREPARE || view == TAB_ID_PREVIEW) w.view = view;
#ifdef SLIC3R_CAD
    if (view == TAB_ID_DESIGN) w.view = view;
#endif
}

void ProjectTabs::bind(Workspace& w)
{
    auto& app = wxGetApp();
    app.preset_bundle = w.presets.get();
    app.plater_ = m_frame.m_plater = w.plater;
    m_frame.m_param_panel = w.panel;
    m_frame.m_param_dialog = w.dialog;
    app.tabs_list = w.tabs;
    app.model_tabs_list = w.model_tabs;
    app.plate_tab = w.plate_tab;
#ifdef SLIC3R_CAD
    m_frame.m_design_page = w.design;
    if (w.design) w.design->make_current();
#endif
}

void ProjectTabs::attach_pages()
{
    m_frame.m_plater->Reparent(m_frame.m_tabpanel);
    m_frame.m_tabpanel->ReplacePageWindow(TAB_ID_PREPARE, m_frame.m_plater);
    m_frame.m_tabpanel->ReplacePageWindow(TAB_ID_PREVIEW, m_frame.m_plater);
#ifdef SLIC3R_CAD
    if (m_frame.m_design_page) {
        m_frame.m_design_page->Reparent(m_frame.m_tabpanel);
        m_frame.m_tabpanel->ReplacePageWindow(TAB_ID_DESIGN, m_frame.m_design_page);
    }
#endif
    m_frame.m_plater->Show();
}

bool ProjectTabs::activate(Plater* owner)
{
    const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [owner](const auto& w) { return w->plater == owner; });
    return found != m_workspaces.end() && activate(std::distance(m_workspaces.begin(), found));
}

bool ProjectTabs::activate(std::size_t index)
{
    if (m_shutdown || changing() || index >= m_workspaces.size()) return false;
    if (index == m_active) {
        m_frame.select_tab(m_workspaces[index]->view);
        return true;
    }
    auto& outgoing = *m_workspaces[m_active];
    if (!outgoing.plater->prepare_project_switch()) return false;
#ifdef SLIC3R_CAD
    if (outgoing.design && outgoing.design->get()) {
        std::string reason;
        if (outgoing.design->get()->mcp_busy(false, reason)) return false;
        outgoing.design->get()->sync_recipe_to_model();
        outgoing.design->get()->on_tab_hidden();
        outgoing.design->Hide();
    }
#endif
    m_switching = true;
    const auto previous = m_active;
    bool committed = false;
    ScopeGuard restore([&] {
        if (!committed) {
            m_workspaces[index]->plater->wxPanel::Show(false);
            m_active = previous;
            bind(outgoing);
            attach_pages();
            m_frame.select_tab(outgoing.view);
            outgoing.plater->resume_project();
            m_frame.Layout();
        }
        m_switching = false;
    });
    try {
        wxWindowUpdateLocker locker(&m_frame);
        capture(outgoing);
        outgoing.dialog->Hide();
        outgoing.plater->suspend_project();
        outgoing.plater->wxPanel::Show(false);
        m_active = index;
        auto& incoming = *m_workspaces[index];
        bind(incoming);
        if (incoming.dark != wxGetApp().dark_mode()) {
            incoming.plater->sys_color_changed();
            for (auto* tab : incoming.tabs) tab->sys_color_changed();
            for (auto* tab : incoming.model_tabs) tab->sys_color_changed();
            if (incoming.plate_tab) incoming.plate_tab->sys_color_changed();
            incoming.dark = wxGetApp().dark_mode();
        }
        if (incoming.dpi != m_frame.GetDPIScaleFactor()) {
            incoming.plater->msw_rescale();
            incoming.panel->msw_rescale();
            incoming.dpi = m_frame.GetDPIScaleFactor();
        }
        attach_pages();
        m_frame.select_tab(m_workspaces[index]->view);
        m_frame.m_plater->select_view_3D(incoming.view == TAB_ID_PREVIEW ? "Preview" : "3D", true);
        m_frame.m_plater->resume_project();
        m_frame.Layout();
        committed = true;
        m_switching = false;
        refresh();
        return true;
    } catch (const std::exception& error) {
        show_error(&m_frame, from_u8(error.what()));
        return false;
    }
}

ProjectTabs::Workspace* ProjectTabs::create()
try {
    if (m_creating || m_shutdown || !m_frame.m_plater->prepare_project_switch()) return nullptr;
    auto& outgoing = *m_workspaces[m_active];
    capture(outgoing);
    outgoing.dialog->Hide();
    outgoing.plater->suspend_project();
    outgoing.plater->wxPanel::Show(false);
#ifdef SLIC3R_CAD
    if (outgoing.design && outgoing.design->get()) {
        outgoing.design->get()->sync_recipe_to_model();
        outgoing.design->get()->on_tab_hidden();
        outgoing.design->Hide();
    }
#endif
    auto incoming = std::make_unique<Workspace>();
    bool committed = false;
    ScopeGuard rollback([&] {
        m_creating = false;
        if (committed) return;
        if (incoming->plater) {
            incoming->plater->shutdown_project();
            incoming->plater->Destroy();
        }
#ifdef SLIC3R_CAD
        if (incoming->design) {
            auto& pages = m_frame.m_lazy_pages;
            pages.erase(std::remove(pages.begin(), pages.end(), incoming->design), pages.end());
            incoming->design->Destroy();
        }
#endif
        bind(outgoing);
        attach_pages();
        m_frame.select_tab(outgoing.view);
        outgoing.plater->resume_project();
        m_frame.Layout();
    });

    incoming->presets = std::make_shared<PresetBundle>(*wxGetApp().preset_bundle);
    incoming->presets->m_config_maps = wxGetApp().preset_bundle->m_config_maps;
    incoming->presets->m_filament_id_maps = wxGetApp().preset_bundle->m_filament_id_maps;
    incoming->presets->dir_user_presets_local = wxGetApp().preset_bundle->dir_user_presets_local;
    incoming->presets->dir_user_presets_subscribed = wxGetApp().preset_bundle->dir_user_presets_subscribed;
    incoming->presets->filament_ams_list = wxGetApp().preset_bundle->filament_ams_list;
    incoming->presets->ams_multi_color_filment = wxGetApp().preset_bundle->ams_multi_color_filment;
    incoming->presets->extruder_ams_counts = wxGetApp().preset_bundle->extruder_ams_counts;
    incoming->untitled = format_wxstr(_L("Untitled %1%"), m_untitled++);
    m_creating = true;
    auto& app = wxGetApp();
    app.preset_bundle = incoming->presets.get();
    app.tabs_list.clear();
    app.model_tabs_list.clear();
    app.plate_tab = nullptr;
#ifdef SLIC3R_CAD
    if (outgoing.design) {
        m_frame.m_design_page = new LazyPage<DesignPanel>(m_frame.m_tabpanel, TAB_ID_DESIGN, -1);
        m_frame.m_lazy_pages.push_back(m_frame.m_design_page);
        incoming->design = m_frame.m_design_page;
    }
#endif
    m_frame.m_param_panel = new ParamsPanel(m_frame.m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                           wxBK_LEFT | wxTAB_TRAVERSAL);
    app.plater_ = nullptr;
    m_frame.m_plater = nullptr;
    Plater* plater = nullptr;
    {
        // The native child constructors defer preview/scene work until a frame
        // is published. Use that same construction boundary for another document.
        auto* published_frame = app.mainframe;
        app.mainframe = nullptr;
        ScopeGuard restore_frame([&app, published_frame] { app.mainframe = published_frame; });
        plater = new Plater(&m_frame, &m_frame);
    }
    incoming->plater = plater;
    plater->retain_project_presets(incoming->presets);
    plater->wxPanel::Show(false);
    plater->sidebar().obj_list()->init();
    plater->init_notification_manager();
    m_frame.create_preset_tabs();
    app.load_current_presets(false, false);
    plater->new_project(true, true, incoming->untitled, false);
    plater->sidebar().obj_list()->set_min_height();
    capture(*incoming);
    auto* result = incoming.get();
    m_workspaces.push_back(std::move(incoming));
    committed = true;
    m_active = m_workspaces.size() - 1;
    attach_pages();
    m_creating = false;
    m_frame.select_tab(TAB_ID_PREPARE);
    plater->resume_project();
    refresh();
    m_frame.Layout();
    return result;
} catch (const std::exception& error) {
    m_creating = false;
    bind(*m_workspaces[m_active]);
    attach_pages();
    m_frame.select_tab(m_workspaces[m_active]->view);
    show_error(&m_frame, from_u8(error.what()));
    return nullptr;
}

bool ProjectTabs::new_project() { return create() != nullptr; }

bool ProjectTabs::open_project(const wxString& filename, const wxString& origin)
{
    if (filename.empty()) return false;
    for (std::size_t i = 0; i < m_workspaces.size(); ++i)
        if (same_file(filename, m_workspaces[i]->plater->get_project_filename(".3mf"))) return activate(i);
    const auto previous = m_active;
    auto* candidate = create();
    if (!candidate) return false;
    m_creating = true;
    bool loaded = false;
    try {
        candidate->plater->load_project(filename, origin);
        loaded = candidate->plater->last_project_load_succeeded();
    } catch (const std::exception& error) { show_error(&m_frame, from_u8(error.what())); }
    m_creating = false;
    if (loaded) saved(candidate->plater);
    if (!loaded) {
        candidate->plater->set_plater_dirty(false);
        const auto failed = m_active;
        activate(previous);
        auto discarded = std::move(m_workspaces[failed]);
        m_workspaces.erase(m_workspaces.begin() + failed);
        dispose(*discarded);
    }
    refresh();
    return loaded;
}

bool ProjectTabs::open_gcode(const wxString& filename)
{
    for (std::size_t i = 0; i < m_workspaces.size(); ++i)
        if (same_file(filename, m_workspaces[i]->viewer_path)) return activate(i);
    const auto previous = m_active;
    auto* candidate = create();
    if (!candidate) return false;
    m_creating = true;
    bool loaded = false;
    try {
        candidate->plater->load_gcode(filename);
        loaded = candidate->plater->only_gcode_mode();
    } catch (const std::exception& error) { show_error(&m_frame, from_u8(error.what())); }
    m_creating = false;
    if (!loaded) {
        const auto failed = m_active;
        activate(previous);
        auto discarded = std::move(m_workspaces[failed]);
        m_workspaces.erase(m_workspaces.begin() + failed);
        dispose(*discarded);
        refresh();
        return false;
    }
    candidate->viewer_path = filename;
    candidate->view = TAB_ID_PREVIEW;
    m_frame.select_tab(TAB_ID_PREVIEW);
    refresh();
    return true;
}

bool ProjectTabs::confirm_close(Workspace& w)
{
    if (w.plater->is_active_project() && !w.plater->prepare_project_switch()) return false;
    const bool running = w.plater->slice_batch_running() || w.plater->is_background_process_slicing();
    const bool dirty = w.plater->is_project_dirty();
    if (!dirty && !running) return true;
    wxString message = dirty ? _L("Save changes to this project before closing?") : _L("Close this project and cancel slicing?");
    if (dirty && running) message += "\n\n" + _L("Closing this project cancels its slicing work.");
    MessageDialog dialog(&m_frame, message, w.button ? w.button->GetLabel() : _L("Close project"),
                         dirty ? wxYES_NO | wxCANCEL : wxYES_NO | wxNO_DEFAULT);
    dialog.SetButtonLabel(wxID_YES, dirty ? _L("Save") : _L("Close and cancel slicing"));
    dialog.SetButtonLabel(wxID_NO, dirty ? _L("Discard") : _L("Keep open"));
    const int answer = dialog.ShowModal();
    if (answer == wxID_YES) return !dirty || (activate(w.plater) && w.plater->save_project() == wxID_YES);
    w.discarded = dirty && answer == wxID_NO;
    return w.discarded;
}

bool ProjectTabs::close(std::size_t index)
{
    if (index >= m_workspaces.size() || m_creating || m_switching || m_shutdown) return false;
    const auto previous = m_active;
    if (!confirm_close(*m_workspaces[index])) {
        if (previous != m_active) activate(previous);
        return false;
    }
    if (m_workspaces.size() == 1 && !create()) return false;
    const auto next_index = previous != index ? previous :
        index + 1 < m_workspaces.size() ? index + 1 : index - 1;
    if (m_active == index && !activate(next_index)) return false;
    auto closed = std::move(m_workspaces[index]);
    m_workspaces.erase(m_workspaces.begin() + index);
    if (m_active > index) --m_active;
    dispose(*closed);
    refresh();
    return true;
}

void ProjectTabs::dispose(Workspace& workspace)
{
    workspace.plater->shutdown_project();
#ifdef SLIC3R_CAD
    if (workspace.design) {
        if (auto* design = workspace.design->get()) {
            design->unbind_canvas_event_handlers();
            design->reset_canvas_volumes();
            design->shutdown();
        }
        auto& pages = m_frame.m_lazy_pages;
        pages.erase(std::remove(pages.begin(), pages.end(), workspace.design), pages.end());
        workspace.design->Destroy();
    }
#endif

    workspace.plater->model().set_backup_path("");
    if (!workspace.checkpoint.empty()) {
        boost::system::error_code ec;
        boost::filesystem::remove_all(boost::filesystem::path(workspace.checkpoint).parent_path(), ec);
    }
    workspace.plater->Destroy();
    if (workspace.tab) workspace.tab->Destroy();
}

bool ProjectTabs::confirm_quit()
{
    const auto previous = m_active;
    for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
        if (!activate(i) || !confirm_close(*m_workspaces[i])) {
            for (const auto& w : m_workspaces) w->discarded = false;
            activate(previous);
            return false;
        }
    }
    return true;
}

bool ProjectTabs::save_all()
{
    if (!m_frame.m_plater->prepare_project_switch()) return false;
    const auto previous = m_active;
    bool saved = true;
    for (std::size_t i = 0; i < m_workspaces.size(); ++i)
        if (m_workspaces[i]->plater->is_project_dirty() &&
            (!activate(i) || m_workspaces[i]->plater->save_project() != wxID_YES)) { saved = false; break; }
    activate(previous);
    return saved;
}

void ProjectTabs::next(int direction)
{
    if (!m_workspaces.empty()) activate((m_active + m_workspaces.size() + direction) % m_workspaces.size());
}

bool ProjectTabs::path_available(const wxString& path, const Plater* owner) const
{
    return std::none_of(m_workspaces.begin(), m_workspaces.end(), [&](const auto& w) {
        return w->plater != owner && same_file(path, w->plater->get_project_filename(".3mf"));
    });
}

void ProjectTabs::saved(Plater* owner)
{
    for (const auto& w : m_workspaces) {
        if (w->plater != owner) continue;
        w->stamped_path = owner->get_project_filename(".3mf");
        boost::system::error_code ec;
        const auto path = into_path(w->stamped_path);
        w->modified = boost::filesystem::last_write_time(path, ec);
        if (ec) { w->stamped = false; return; }
        w->size = boost::filesystem::file_size(path, ec);
        w->stamped = !ec;
        return;
    }
}

bool ProjectTabs::confirm_save(const wxString& path, const Plater* owner) const
{
    if (!path_available(path, owner)) {
        show_error(&m_frame, _L("This file is already open in another project tab. Choose another name to save this project."));
        return false;
    }
    for (const auto& w : m_workspaces) {
        if (w->plater != owner || !w->stamped || !same_file(path, w->stamped_path)) continue;
        boost::system::error_code ec;
        const auto file = into_path(path);
        const auto modified = boost::filesystem::last_write_time(file, ec);
        if (ec) continue;
        const auto size = boost::filesystem::file_size(file, ec);
        if (ec || (modified == w->modified && size == w->size)) return true;
        MessageDialog dialog(&m_frame, _L("This project file changed outside Porca. Replace it with your open project's changes?"),
                             _L("Save project"), wxYES_NO | wxNO_DEFAULT);
        dialog.SetButtonLabel(wxID_YES, _L("Replace file"));
        dialog.SetButtonLabel(wxID_NO, _L("Keep file"));
        return dialog.ShowModal() == wxID_YES;
    }
    return true;
}

std::vector<Plater*> ProjectTabs::workspaces() const
{
    std::vector<Plater*> result;
    for (const auto& w : m_workspaces) result.push_back(w->plater);
    return result;
}

void ProjectTabs::layout_tabs()
{
    if (m_shutdown || !m_ui->add || !m_ui->list) return;
    const bool dark = wxGetApp().dark_mode();
    const double dpi = m_ui->panel->GetDPIScaleFactor();
    const bool rescale = m_ui->dpi != dpi;
    if (m_ui->dark != dark || rescale) {
        m_ui->dark = dark;
        m_ui->dpi = dpi;
        const auto background = tab_color(DesignSystem::ColorRole::Bg);
        m_ui->panel->SetBackgroundColour(background);
        m_ui->scroll->SetBackgroundColour(background);
        for (auto* button : {m_ui->add, m_ui->list}) {
            button->Rescale();
            button->SetMaxSize(button->FromDIP(wxSize(28, 28)));
            button->SetMinSize(button->FromDIP(wxSize(28, 28)));
            button->SetBackgroundColour(background);
            style_tab_action(*button, background);
        }
        for (const auto& w : m_workspaces)
            if (w->tab && rescale) w->tab->rescale();
        m_ui->panel->Refresh();
    }
    const int available = m_ui->panel->GetClientSize().x;
    if (available <= 20) return; // Not allocated by MainFrame yet.
    const int content = m_ui->tabs->CalcMin().x;
    const int width = std::min(content, std::max(0, available - m_ui->panel->FromDIP(80)));
    const auto size = wxSize(width, m_ui->panel->FromDIP(44));
    if (m_ui->scroll->GetMinSize() != size) {
        m_ui->scroll->SetMinSize(size);
        m_ui->panel->Layout();
        m_ui->scroll->FitInside();
    }
}

void ProjectTabs::refresh()
{
    if (m_shutdown || m_creating || m_switching || wxGetApp().is_closing()) return;
    pump_slice_queue();
    bool layout_changed = false;
    bool selection_changed = false;
    for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
        auto& w = *m_workspaces[i];
        if (!w.tab) {
            layout_changed = true;
            w.tab = new ProjectTab(m_ui->scroll);
            w.button = w.tab;
            w.close_button = w.tab->close_button;
            m_ui->tabs->Add(w.tab, 0, wxALIGN_BOTTOM | wxTOP, w.tab->FromDIP(6));
            auto* target = w.plater;
            const auto find = [this, target]() {
                return std::find_if(m_workspaces.begin(), m_workspaces.end(), [target](const auto& item) { return item->plater == target; });
            };
            w.button->Bind(wxEVT_BUTTON, [this, find, button = w.button](wxCommandEvent& e) {
                e.Skip();
                if (e.GetEventObject() != button) return;
                m_ui->panel->CallAfter([this, find] {
                    if (m_shutdown) return;
                    auto it = find();
                    if (it != m_workspaces.end()) activate(std::distance(m_workspaces.begin(), it));
                });
            });
            w.button->Bind(wxEVT_RIGHT_UP, [this, target](wxMouseEvent& e) {
                e.Skip();
                m_ui->panel->CallAfter([this, target] { if (!m_shutdown) project_menu(target); });
            });
            w.close_button->Bind(wxEVT_BUTTON, [this, find](wxCommandEvent& e) {
                e.Skip();
                m_ui->panel->CallAfter([this, find] {
                    if (m_shutdown) return;
                    auto it = find();
                    if (it != m_workspaces.end()) close(std::distance(m_workspaces.begin(), it));
                });
            });
        }
        const auto path = w.viewer_path.empty() ? w.plater->get_project_filename(".3mf") : w.viewer_path;
        wxString label = path.empty() ? w.untitled : from_path(into_path(path).stem());
        if (!path.empty()) {
            const auto stem = into_path(path).stem();
            const auto duplicates = std::count_if(m_workspaces.begin(), m_workspaces.end(), [&](const auto& other) {
                const auto other_path = other->plater->get_project_filename(".3mf");
                return !other_path.empty() && into_path(other_path).stem() == stem;
            });
            if (duplicates > 1) label += " — " + from_path(into_path(path).parent_path());
        }
        const bool dirty = w.plater->is_project_dirty();
        const auto progress = w.plater->slice_batch_label();
        if (w.plater->slice_batch_running() && m_slice_queue.current().project != w.id) label += " · " + _L("Queued");
        else if (w.plater->is_background_process_slicing()) label += " · " + _L("Slicing");
        else if (!progress.empty()) label += " · " + progress;
        if (w.button->GetLabel() != label) { w.button->SetLabel(label); layout_changed = true; }
        w.button->SetName(dirty ? label + " *" : label);
        wxString tooltip = path.empty() ? label : path;
        if (!progress.empty()) tooltip += "\n" + progress;
        if (dirty) tooltip += "\n" + _L("Unsaved changes");
        w.button->SetToolTip(tooltip);
        if (w.selected != (i == m_active)) {
            selection_changed = true;
            w.selected = i == m_active;
        }
        w.tab->update(w.selected, dirty);
    }
    layout_tabs();
    if (layout_changed) {
        m_ui->scroll->FitInside();
        m_ui->scroll->Layout();
    }
    if ((layout_changed || selection_changed) && !m_workspaces.empty()) {
        const auto rect = m_workspaces[m_active]->tab->GetRect();
        const int width = m_ui->scroll->GetClientSize().x;
        int unit_x = 0, unit_y = 0;
        m_ui->scroll->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int start = m_ui->scroll->GetViewStartPixels().x;
        if (unit_x > 0 && rect.x < 0) m_ui->scroll->Scroll(std::max(0, start + rect.x) / unit_x, 0);
        else if (unit_x > 0 && rect.GetRight() > width)
            m_ui->scroll->Scroll((start + rect.GetRight() - width + unit_x - 1) / unit_x, 0);
    }
    write_session();
}

void ProjectTabs::queue_slice(Plater* owner)
{
    for (const auto& w : m_workspaces)
        if (w->plater == owner) { m_slice_queue.enqueue(w->id); return; }
}

void ProjectTabs::cancel_slice(Plater* owner)
{
    for (const auto& w : m_workspaces) {
        if (w->plater != owner) continue;
        if (m_slice_queue.current().project == w->id) m_slice_started = false;
        m_slice_queue.cancel(w->id);
        return;
    }
}

bool ProjectTabs::can_start_slice(const Plater* owner) const
{
    if (changing() || m_shutdown) return false;
    const auto ticket = m_slice_queue.current();
    for (const auto& w : m_workspaces) {
        if (w->plater != owner && w->plater->slice_worker_running()) return false;
        if (w->plater == owner && ticket.project && ticket.project != w->id) return false;
    }
    return true;
}

void ProjectTabs::pump_slice_queue()
{
    const auto ticket = m_slice_queue.current();
    if (!ticket.project) return;
    const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(),
        [ticket](const auto& w) { return w->id == ticket.project; });
    if (found == m_workspaces.end() || !(*found)->plater->slice_batch_running()) {
        m_slice_queue.complete(ticket);
        m_slice_started = false;
        return;
    }
    if (m_slice_started) return;
    for (const auto& w : m_workspaces)
        if (w->plater->slice_worker_running()) return;
    m_slice_started = true;
    (*found)->plater->continue_queued_slice();
}

void ProjectTabs::project_list()
{
    wxMenu menu;
    for (const auto& w : m_workspaces) {
        auto* owner = w->plater;
        append_menu_item(&menu, wxID_ANY, wxControl::EscapeMnemonics(w->button->GetLabel()), wxEmptyString,
            [this, owner](wxCommandEvent&) {
                const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                    [owner](const auto& w) { return w->plater == owner; });
                if (found != m_workspaces.end()) activate(std::distance(m_workspaces.begin(), found));
            });
    }
    m_ui->panel->PopupMenu(&menu);
}

void ProjectTabs::project_menu(Plater* owner)
{
    const auto find = [this, owner] {
        return std::find_if(m_workspaces.begin(), m_workspaces.end(), [owner](const auto& w) { return w->plater == owner; });
    };
    if (find() == m_workspaces.end()) return;
    wxMenu menu;
    append_menu_item(&menu, wxID_ANY, _L("Save Project"), wxEmptyString, [this, find](wxCommandEvent&) {
        auto found = find();
        if (found != m_workspaces.end() && activate(std::distance(m_workspaces.begin(), found))) m_frame.m_plater->save_project();
    });
    append_menu_item(&menu, wxID_ANY, _L("Close Project"), wxEmptyString, [this, find](wxCommandEvent&) {
        auto found = find();
        if (found != m_workspaces.end()) close(std::distance(m_workspaces.begin(), found));
    });
    menu.AppendSeparator();
    append_menu_item(&menu, wxID_ANY, _L("Move tab left"), wxEmptyString,
        [this, owner](wxCommandEvent&) { move(owner, -1); });
    append_menu_item(&menu, wxID_ANY, _L("Move tab right"), wxEmptyString,
        [this, owner](wxCommandEvent&) { move(owner, 1); });
    m_ui->panel->PopupMenu(&menu);
}

void ProjectTabs::move(Plater* owner, int direction)
{
    const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [owner](const auto& w) { return w->plater == owner; });
    if (found == m_workspaces.end()) return;
    const auto index = std::distance(m_workspaces.begin(), found);
    const auto destination = index + direction;
    if (destination < 0 || destination >= std::ptrdiff_t(m_workspaces.size())) return;
    auto* active = m_workspaces[m_active].get();
    std::swap(m_workspaces[index], m_workspaces[destination]);
    for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
        auto& w = m_workspaces[i];
        if (w.get() == active) m_active = i;
        m_ui->tabs->Detach(w->tab);
    }
    for (const auto& w : m_workspaces)
        m_ui->tabs->Add(w->tab, 0, wxALIGN_BOTTOM | wxTOP, w->tab->FromDIP(6));
    m_ui->scroll->FitInside();
    m_ui->scroll->Layout();
    refresh();
}

bool ProjectTabs::checkpoint(Plater* owner, bool force)
{
    if (!owner || !owner->is_active_project()) return false;
    const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [owner](const auto& w) { return w->plater == owner; });
    if (found == m_workspaces.end()) return false;
    if (!force && (!owner->is_project_dirty() || owner->up_to_date(false, true))) return true;
    const auto path = boost::filesystem::path((*found)->checkpoint);
    try {
        boost::filesystem::create_directories(path.parent_path());
        const auto temporary = path.parent_path() / "checkpoint.tmp.3mf";
        if (!owner->export_3mf_copy(temporary)) return false;
        if (rename_file(temporary.string(), path.string())) return false;
        owner->up_to_date(true, true);
        return true;
    } catch (const std::exception&) { return false; }
}

bool ProjectTabs::prepare_recreation()
{
    const auto previous = m_active;
    for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
        if (!activate(i) || !checkpoint(m_frame.m_plater, true)) {
            activate(previous);
            return false;
        }
    }
    activate(previous);
    m_recreating = true;
    write_session(false);
    return true;
}

void ProjectTabs::write_session(bool clean)
{
    if (m_ui->restoring || m_ui->preserve_manifest || (!m_ui->restore_checked && !m_ui->previous_session.is_null())) return;
    nlohmann::json records = nlohmann::json::array();
    std::size_t active = 0;
    for (const auto& w : m_workspaces) {
        if (clean && w->discarded) continue;
        if (w.get() == m_workspaces[m_active].get()) active = records.size();
        records.push_back({{"source", into_u8(w->plater->get_project_filename(".3mf"))},
            {"checkpoint", w->checkpoint}, {"name", into_u8(w->untitled)},
            {"dirty", w->plater->is_project_dirty()}, {"view", into_u8(w->view)},
            {"workspace_view", view_state(*w->plater)}});
    }
    for (const auto& pending : m_ui->pending_recovery) records.push_back(pending);
    const auto text = nlohmann::json{{"version", 1}, {"clean", clean && m_ui->pending_recovery.empty()},
        {"active", active}, {"recreate", m_recreating}, {"projects", std::move(records)}}.dump(2);
    if (text == m_ui->last_session) return;
    const auto path = boost::filesystem::path(data_dir()) / "project-session.json";
    try {
        save_string_file(path.string() + ".tmp", text);
        if (rename_file(path.string() + ".tmp", path.string())) return;
        m_ui->last_session = text;
    } catch (const std::exception&) {
        // Keep the last complete manifest and all independently owned checkpoints.
    }
}

bool ProjectTabs::restore_session()
{
    if (m_ui->restore_checked) return false;
    m_ui->restore_checked = true;
    if (m_ui->preserve_manifest) {
        show_error(&m_frame, _L("The saved workspace could not be restored. Its recovery files have been kept."));
        return true;
    }
    const auto previous = std::move(m_ui->previous_session);
    m_ui->previous_session = nullptr;
    if (previous.is_null()) return false;
    if (!previous.contains("projects") || !previous["projects"].is_array()) return false;
    const bool clean = previous.value("clean", false);
    if (clean && !wxGetApp().app_config->get_bool("reopen_project_tabs")) return true;
    const bool recreate = previous.value("recreate", false);
    m_ui->restoring = true;
    ScopeGuard finish_restore([this] { m_ui->restoring = false; });
    const auto initial = m_workspaces[m_active]->plater;
    Plater* restored_active = nullptr;
    std::size_t record_index = 0;
    for (const auto& record : previous["projects"]) {
        const auto original_index = record_index++;
        try {
            if (!recreate && !clean && !record.value("dirty", false)) continue;
            auto name = from_u8(record.value("source", std::string()));
            if (name.empty()) name = from_u8(record.value("name", std::string()));
            MessageDialog dialog(&m_frame, format_wxstr(_L("Recover unsaved changes to %1%?"), name),
                                 _L("Recover project"), wxYES_NO | wxCANCEL);
            const auto answer = recreate || clean ? wxID_YES : dialog.ShowModal();
            if (answer == wxID_CANCEL) {
                // Keep unprocessed recovery references available for another attempt.
                for (std::size_t i = original_index; i < previous["projects"].size(); ++i)
                    m_ui->pending_recovery.push_back(previous["projects"][i]);
                return true;
            }
            if (answer != wxID_YES) continue;
            const auto checkpoint = boost::filesystem::path(record.value(clean ? "source" : "checkpoint", std::string()));
            if (clean && checkpoint.empty()) continue;
            if (!boost::filesystem::is_regular_file(checkpoint)) {
                show_error(&m_frame, clean ? format_wxstr(_L("Could not reopen %1%. Other projects can still be opened."), name) :
                    format_wxstr(_L("No complete recovery copy was found for %1%. Other projects can still be recovered."), name));
                m_ui->pending_recovery.push_back(record);
                continue;
            }
            if (open_project(from_path(checkpoint), "<silence>")) {
                m_frame.m_plater->set_project_filename(from_u8(record.value("source", std::string())));
                saved(m_frame.m_plater);
                if (record.value("dirty", false)) m_frame.m_plater->set_plater_dirty(true);
                else m_frame.m_plater->reset_project_dirty_after_save();
                const auto view = from_u8(record.value("view", std::string(TAB_ID_PREPARE)));
                if (view == TAB_ID_PREPARE || view == TAB_ID_PREVIEW) m_workspaces[m_active]->view = view;
                if (record.contains("workspace_view")) {
                    try { restore_view_state(*m_frame.m_plater, record["workspace_view"]); }
                    catch (const std::exception&) { /* A damaged view does not invalidate recovered content. */ }
                }
                if (original_index == previous.value("active", std::size_t(0))) restored_active = m_frame.m_plater;
            } else m_ui->pending_recovery.push_back(record);
        } catch (const std::exception&) {
            show_error(&m_frame, _L("One project could not be recovered. Its recovery files have been kept."));
            m_ui->pending_recovery.push_back(record);
        }
    }
    if (m_workspaces.size() > 1 && initial->model().objects.empty() && !initial->is_project_dirty()) {
        const auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(),
            [initial](const auto& w) { return w->plater == initial; });
        if (found != m_workspaces.end()) close(std::distance(m_workspaces.begin(), found));
    }
    if (restored_active) activate(restored_active);
    m_ui->restoring = false;
    write_session();
    return true;
}

void ProjectTabs::shutdown()
{
    if (m_shutdown) return;
    write_session(!m_recreating);
    m_shutdown = true;
    m_ui->timer.Stop();
    for (const auto& w : m_workspaces) {
        w->plater->shutdown_project();
#ifdef SLIC3R_CAD
        if (w->design && w->design->get()) {
            w->design->get()->unbind_canvas_event_handlers();
            w->design->get()->reset_canvas_volumes();
            w->design->get()->shutdown();
        }
#endif
    }
    wxGetApp().preset_bundle = m_ui->app_presets;
}
}
