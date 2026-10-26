#include "ModelLibraryPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/ModelLibrary/Extract.hpp"
#include "slic3r/Utils/ModelLibrary/Trash.hpp"
#include "slic3r/Utils/ModelLibrary/Online.hpp"
#include "slic3r/Utils/ModelLibrary/Source.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/dataview.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/listctrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/secretstore.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/variant.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {
using Json = ModelLibrary::Json;
using Id = ModelLibrary::Id;
namespace library_fs = ModelLibrary::fs;

class LibraryDialog : public DPIDialog {
public:
    using DPIDialog::DPIDialog;
protected:
    void on_dpi_changed(const wxRect&) override
    {
        if (GetSizer()) GetSizer()->SetSizeHints(this);
        Layout();
        Refresh();
    }
};

std::string text(const Json& row, const std::string& key)
{
    const auto value = row.find(key);
    return value != row.end() && value->is_string() ? value->get<std::string>() : "";
}

class NavigationData : public wxTreeItemData {
public:
    Json filter;
    explicit NavigationData(Json value) : filter(std::move(value)) {}
};

class LibraryDropTarget : public wxFileDropTarget {
    std::function<void(std::vector<library_fs::path>)> m_drop;
public:
    explicit LibraryDropTarget(std::function<void(std::vector<library_fs::path>)> drop) : m_drop(std::move(drop)) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& names) override
    {
        std::vector<library_fs::path> paths;
        for (const auto& name : names) paths.push_back(library_fs::u8path(into_u8(name)));
        m_drop(std::move(paths));
        return true;
    }
};

class CollectionDropTarget : public wxTextDropTarget {
    std::function<bool(wxCoord,wxCoord,const wxString&)> m_drop;
public:
    explicit CollectionDropTarget(std::function<bool(wxCoord,wxCoord,const wxString&)> drop) : m_drop(std::move(drop)) {}
    bool OnDropText(wxCoord x, wxCoord y, const wxString& value) override { return m_drop(x,y,value); }
};

std::optional<std::vector<std::string>> edit_fields(wxWindow* parent, const wxString& title,
                                                 const std::vector<std::pair<wxString, std::string>>& fields,
                                                 bool multiline = false)
{
    LibraryDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    std::vector<wxTextCtrl*> inputs;
    for (const auto& [label, value] : fields) {
        sizer->Add(new Label(&dialog, label), 0, wxLEFT | wxRIGHT | wxTOP, dialog.FromDIP(16));
        if (multiline) {
            auto* input = new wxTextCtrl(&dialog, wxID_ANY, from_u8(value), wxDefaultPosition, dialog.FromDIP(wxSize(440, 150)), wxTE_MULTILINE);
            inputs.push_back(input);
            sizer->Add(input, 1, wxEXPAND | wxALL, dialog.FromDIP(16));
        } else {
            auto* input = new ::TextInput(&dialog, from_u8(value));
            input->SetMinSize(dialog.FromDIP(wxSize(400, 32)));
            inputs.push_back(input->GetTextCtrl());
            sizer->Add(input, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
        }
    }
    auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    dialog.SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(&dialog);
    dialog.CenterOnParent();
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    std::vector<std::string> values;
    for (const auto* input : inputs) values.push_back(into_u8(input->GetValue()));
    return values;
}

void read_only_text(wxWindow* parent, const wxString& title, const std::string& content)
{
    LibraryDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* view = new wxTextCtrl(&dialog, wxID_ANY, from_u8(content), wxDefaultPosition, dialog.FromDIP(wxSize(700, 450)), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(view, 1, wxEXPAND | wxALL, dialog.FromDIP(16));
    auto* buttons = new DialogButtons(&dialog, {"OK"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    dialog.SetSizerAndFit(sizer);
    wxGetApp().UpdateDlgDarkUI(&dialog);
    dialog.CenterOnParent();
    dialog.ShowModal();
}

std::optional<std::string> choose_outcome(wxWindow& parent)
{
    wxMenu menu;
    menu.Append(1, _L("Successful"));
    menu.Append(2, _L("Partly successful"));
    menu.Append(3, _L("Failed"));
    switch (parent.GetPopupMenuSelectionFromUser(menu)) {
    case 1: return "good";
    case 2: return "meh";
    case 3: return "failed";
    default: return std::nullopt;
    }
}

wxString outcome_label(const std::string& outcome)
{
    if (outcome == "good") return _L("Successful");
    if (outcome == "meh") return _L("Partly successful");
    return _L("Failed");
}

std::optional<std::vector<size_t>> choose_items(wxWindow* parent, const wxString& title,
                                               const std::vector<std::pair<wxString, wxString>>& items,
                                               const std::vector<wxString>& details = {})
{
    LibraryDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* list = new wxDataViewListCtrl(&dialog, wxID_ANY, wxDefaultPosition, dialog.FromDIP(wxSize(760, 440)), wxDV_ROW_LINES);
    list->AppendToggleColumn(_L("Include"), wxDATAVIEW_CELL_ACTIVATABLE, dialog.FromDIP(65));
    list->AppendTextColumn(_L("Name"), wxDATAVIEW_CELL_INERT, dialog.FromDIP(260));
    list->AppendTextColumn(_L("Details"), wxDATAVIEW_CELL_INERT, dialog.FromDIP(420));
    for (const auto& item : items) {
        wxVector<wxVariant> row; row.push_back(true); row.push_back(item.first); row.push_back(item.second); list->AppendItem(row);
    }
    sizer->Add(list, 1, wxEXPAND | wxALL, dialog.FromDIP(16));
    if (!details.empty()) {
        list->SetMinSize(dialog.FromDIP(wxSize(760, 250)));
        auto* preview = new wxTextCtrl(&dialog, wxID_ANY, "", wxDefaultPosition, dialog.FromDIP(wxSize(760, 190)), wxTE_MULTILINE | wxTE_READONLY);
        const auto update = [list, preview, &details] {
            wxString text;
            for (unsigned i = 0; i < details.size(); ++i) {
                wxVariant included; list->GetValue(included,i,0);
                if (included.GetBool()) text += details[i] + "\n";
            }
            preview->ChangeValue(text);
        };
        list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [update](wxDataViewEvent& event) { update(); event.Skip(); });
        update(); sizer->Add(preview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(16));
    }
    auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    std::vector<size_t> selected;
    for (unsigned i = 0; i < items.size(); ++i) { wxVariant value; list->GetValue(value, i, 0); if (value.GetBool()) selected.push_back(i); }
    return selected;
}

class MeshPreview : public wxPanel {
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    std::unique_ptr<BoostThreadWorker> m_worker;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    wxTimer m_timer;
    wxBitmap m_bitmap;
    double m_yaw = 0.785398, m_pitch = 0.55, m_zoom = 1.0;
    wxPoint m_drag, m_pan;
    bool m_dragging = false, m_dirty = true;
    unsigned m_revision = 0;
    void render()
    {
        if (!m_dirty || !m_worker->is_idle()) return;
        m_dirty = false;
        const auto mesh = m_mesh; const auto alive = m_alive;
        const auto yaw = m_yaw, pitch = m_pitch;
        const auto revision = m_revision;
        auto pixels = std::make_shared<std::vector<std::uint8_t>>();
        queue_job(*m_worker, [mesh, pixels, yaw, pitch](Job::Ctl& ctl) {
            if (!ctl.was_canceled()) *pixels = ModelLibrary::render_mesh(*mesh, 512, yaw, pitch, [&ctl] { return ctl.was_canceled(); });
        }, [this, alive, pixels, revision](bool cancelled, std::exception_ptr& error) {
            if (error) { error = nullptr; return; }
            if (!*alive || cancelled || revision != m_revision || pixels->size() != 512 * 512 * 3) return;
            wxImage image(512, 512);
            std::memcpy(image.GetData(), pixels->data(), pixels->size());
            m_bitmap = wxBitmap(image);
            Refresh();
        });
    }
public:
    MeshPreview(wxWindow* parent, indexed_triangle_set mesh)
        : wxPanel(parent), m_mesh(std::make_shared<indexed_triangle_set>(std::move(mesh))),
          m_worker(std::make_unique<BoostThreadWorker>(nullptr, "Library preview")), m_timer(this)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F2F2F2")));
        SetMinSize(FromDIP(wxSize(600, 500)));
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_worker->process_events(); render(); }, m_timer.GetId());
        m_timer.Start(30);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            const auto size = GetClientSize();
            const int edge = std::max(1, int(std::min(size.x, size.y) * m_zoom));
            if (m_bitmap.IsOk()) dc.DrawBitmap(wxBitmap(m_bitmap.ConvertToImage().Scale(edge, edge, wxIMAGE_QUALITY_NORMAL)), (size.x - edge) / 2 + m_pan.x, (size.y - edge) / 2 + m_pan.y);
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { m_drag = event.GetPosition(); m_dragging = true; if (!HasCapture()) CaptureMouse(); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_dragging || !event.LeftIsDown()) { event.Skip(); return; }
            const auto delta = event.GetPosition() - m_drag; m_drag = event.GetPosition();
            if (event.ShiftDown()) { m_pan += delta; Refresh(); }
            else { m_yaw += delta.x * 0.01; m_pitch = std::clamp(m_pitch + delta.y * 0.01, -1.5, 1.5); ++m_revision; m_dirty = true; }
        });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) { m_zoom = std::clamp(m_zoom * (event.GetWheelRotation() > 0 ? 1.1 : 1.0 / 1.1), 0.25, 4.0); Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) { m_dragging = false; if (HasCapture()) ReleaseMouse(); event.Skip(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { m_dragging = false; if (HasCapture()) ReleaseMouse(); event.Skip(); });
        render();
    }
    ~MeshPreview() override { *m_alive = false; m_timer.Stop(); m_worker->cancel_all(); m_worker.reset(); if (HasCapture()) ReleaseMouse(); }
};
} // namespace

ModelLibraryPanel::ModelLibraryPanel(wxWindow* parent)
    : wxPanel(parent), m_events(this), m_search_timer(this)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F4F4F4")));
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    const auto button = [this](wxWindow* owner, wxSizer* sizer, const wxString& label, std::function<void()> action, bool primary = false) {
        auto* control = new Button(owner, label);
        control->SetStyle(primary ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Window);
        control->Bind(wxEVT_BUTTON, [this, action = std::move(action)](wxCommandEvent&) { CallAfter(action); });
        sizer->Add(control, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
        return control;
    };
    button(this, toolbar, _L("Import files"), [this] { import_files(false); }, true);
    button(this, toolbar, _L("Import folder"), [this] { import_files(true); });
    m_search = new ::TextInput(this, "");
    m_search->GetTextCtrl()->SetHint(_L("Search your models"));
    toolbar->Add(m_search, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(12));
    button(this, toolbar, _L("View"), [this] { show_view_menu(); });
    button(this, toolbar, _L("Actions"), [this] { show_actions(); });
    button(this, toolbar, _L("Settings"), [this] { show_settings(); });
    outer->Add(toolbar, 0, wxEXPAND | wxALL, FromDIP(16));
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    m_navigation = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(200, -1)), wxTR_HIDE_ROOT | wxTR_HAS_BUTTONS | wxTR_NO_LINES | wxBORDER_NONE);
    body->Add(m_navigation, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_MULTIPLE | wxDV_ROW_LINES);
    m_list->AppendTextColumn(_L("Name"), wxDATAVIEW_CELL_INERT, FromDIP(280));
    m_list->AppendTextColumn(_L("Designer"), wxDATAVIEW_CELL_INERT, FromDIP(130));
    m_list->AppendTextColumn(_L("Files"), wxDATAVIEW_CELL_INERT, FromDIP(65));
    m_list->AppendTextColumn(_L("Format"), wxDATAVIEW_CELL_INERT, FromDIP(105));
    m_list->AppendTextColumn(_L("Added"), wxDATAVIEW_CELL_INERT, FromDIP(110));
    body->Add(m_list, 1, wxEXPAND | wxRIGHT, FromDIP(12));
    m_grid = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_ICON | wxLC_AUTOARRANGE | wxBORDER_NONE);
    m_grid_images.push_back(wxArtProvider::GetBitmapBundle(wxART_NORMAL_FILE,wxART_OTHER,wxSize(128,128)));
    m_grid->SetNormalImages(m_grid_images);
    body->Add(m_grid, 1, wxEXPAND | wxRIGHT, FromDIP(12));
    m_list->Hide();
    m_inspector = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(320, -1)), wxVSCROLL);
    m_inspector->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_inspector->SetMinSize(FromDIP(wxSize(280, -1)));
    m_inspector->SetScrollRate(0, FromDIP(12));
    auto* inspector = new wxBoxSizer(wxVERTICAL);
    m_cover = new wxStaticBitmap(m_inspector, wxID_ANY, wxNullBitmap);
    m_cover->SetMinSize(FromDIP(wxSize(240, 200)));
    inspector->Add(m_cover, 0, wxALIGN_CENTER | wxALL, FromDIP(12));
    m_title = new Label(m_inspector, _L("Choose a model to see its files"));
    inspector->Add(m_title, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    button(m_inspector, actions, _L("Add to plate"), [this] { add_to_plate(); }, true);
    button(m_inspector, actions, _L("Preview"), [this] { preview(); });
    inspector->Add(actions, 0, wxALL, FromDIP(12));
    m_details = new Label(m_inspector, "");
    inspector->Add(m_details, 0, wxEXPAND | wxALL, FromDIP(12));
    m_files = new wxDataViewListCtrl(m_inspector, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(280, 220)), wxDV_MULTIPLE | wxDV_ROW_LINES);
    m_files->AppendTextColumn(_L("File"), wxDATAVIEW_CELL_INERT, FromDIP(260));
    inspector->Add(m_files, 1, wxEXPAND | wxALL, FromDIP(12));
    button(m_inspector, inspector, _L("Browse files"), [this] { browse_model(); });
    button(m_inspector, inspector, _L("Edit details"), [this] { edit_details(); });
    m_inspector->SetSizer(inspector);
    body->Add(m_inspector, 0, wxEXPAND | wxRIGHT, FromDIP(16));
    outer->Add(body, 1, wxEXPAND);
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    m_status = new Label(this, _L("Choose your library, or create one to start collecting models."));
    footer->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    button(this, footer, _L("Choose library"), [this] { choose_library(); });
    button(this, footer, _L("Activity"), [this] { show_activity(); });
    outer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(outer);
    m_worker = std::make_unique<BoostThreadWorker>(nullptr, "model-library");
    Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
        if (event.GetId() == m_events.GetId()) {
            m_worker->process_events();
            if (m_grid_view && IsShownOnScreen() && m_library) {
                unsigned loaded = 0;
                const auto viewport = m_grid->GetClientRect();
                for (long i = 0; i < m_grid->GetItemCount() && loaded < 2; ++i) {
                    wxListItem item; item.SetId(i); item.SetMask(wxLIST_MASK_IMAGE); m_grid->GetItem(item);
                    if (item.GetImage() != 0) continue;
                    wxRect rect; if (!m_grid->GetItemRect(i, rect) || !viewport.Intersects(rect)) continue;
                    m_grid->SetItemImage(i, -1); ++loaded;
                    const auto relative = text(m_rows[size_t(i)], "thumb");
                    if (relative.empty()) continue;
                    const auto library = m_library; const auto alive = m_alive; const auto generation = m_generation;
                    auto pixels = std::make_shared<ModelLibrary::PreviewPixels>();
                    queue_job(*m_worker, [library, relative, pixels](Job::Ctl& ctl) {
                        if (!ctl.was_canceled()) *pixels = ModelLibrary::read_preview(library->resolve(relative),256);
                    }, [this, alive, library, generation, i, pixels](bool cancelled, std::exception_ptr& error) {
                        if (error) { error = nullptr; return; }
                        if (!*alive || cancelled || library != m_library || generation != m_generation || pixels->rgb.empty()) return;
                        wxImage image(int(pixels->width),int(pixels->height)); std::memcpy(image.GetData(),pixels->rgb.data(),pixels->rgb.size());
                        image.Resize(wxSize(256,256),wxPoint((256 - image.GetWidth()) / 2,(256 - image.GetHeight()) / 2),242,242,242);
                        wxBitmap bitmap(image); bitmap.SetScaleFactor(2.0);
                        const int icon = int(m_grid_images.size());
                        m_grid_images.push_back(wxBitmapBundle::FromBitmap(bitmap));
                        m_grid->SetNormalImages(m_grid_images); m_grid->SetItemImage(i,icon);
                    });
                }
            }
            if (++m_watch_ticks >= 100 && m_library && m_worker->is_idle()) {
                m_watch_ticks = 0;
                const auto library = m_library;
                const auto watch = m_watch;
                const auto alive = m_alive;
                const bool online = m_online_enabled && std::chrono::steady_clock::now() >= m_online_after;
                const auto api_key = m_api_key;
                if (online) m_online_after = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                auto online_error = std::make_shared<std::string>();
                auto changed = std::make_shared<bool>(false);
                queue_job(*m_worker, [library, watch, changed, online, api_key, online_error](Job::Ctl& ctl) {
                    const auto imported = watch->poll(*library,ModelLibrary::extract_file,[&ctl] { return ctl.was_canceled(); });
                    *changed = !imported.empty();
                    if (!ctl.was_canceled()) *changed = library->backfill_geometry(ModelLibrary::extract_file,[&ctl] { return ctl.was_canceled(); }) || *changed;
                    if (library->settings().value("delete_source",false)) for (const auto& row : imported) if (row["model_id"].get<Id>() != 0 && row["status"] != "failed") {
                        const auto path = library_fs::u8path(text(row,"path"));
                        if (library_fs::is_regular_file(path)) try { library->trash_leftovers({path},ModelLibrary::move_to_trash,[&ctl] { return ctl.was_canceled(); }); } catch (const std::exception&) {}
                    }
                    if (online && !ctl.was_canceled()) {
                        try {
                            const auto cancel = [&ctl] { return ctl.was_canceled(); };
                            ModelLibrary::tag_with_ai(*library, api_key, ModelLibrary::request_online, cancel);
                            ModelLibrary::enrich_pages(*library, ModelLibrary::request_online, cancel);
                            *changed = true;
                        } catch (const std::exception& error) { *online_error = error.what(); }
                    }
                }, [this, alive, library, changed, online_error](bool cancelled, std::exception_ptr& error) {
                    error = nullptr;
                    if (*alive && !cancelled && library == m_library) {
                        if (*changed) refresh_library();
                        if (!online_error->empty()) { m_status->SetLabel(from_u8(*online_error)); m_online_after = std::chrono::steady_clock::now() + std::chrono::minutes(15); }
                    }
                });
            }
        }
        else if (event.GetId() == m_search_timer.GetId()) refresh_library();
        else event.Skip();
    });
    m_online_enabled = wxGetApp().app_config->get("model_library_online") == "1";
    if (m_online_enabled) {
        auto store = wxSecretStore::GetDefault(); wxString user; wxSecretValue secret;
        if (store.IsOk() && store.Load("Porca Model Library Anthropic", user, secret)) m_api_key = into_u8(secret.GetAsString());
    }
    m_events.Start(50);
    m_search->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) { m_search_timer.StartOnce(180); event.Skip(); });
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent& event) {
        if (m_refreshing) return;
        const int row = m_list->ItemToRow(event.GetItem());
        if (row >= 0 && size_t(row) < m_rows.size()) show_detail(m_rows[size_t(row)]["id"]);
    });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& event) {
        const int row = m_list->ItemToRow(event.GetItem());
        if (row >= 0 && size_t(row) < m_rows.size()) show_detail(m_rows[size_t(row)]["id"], true);
    });
    m_grid->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event) {
        if (!m_refreshing && event.GetIndex() >= 0 && size_t(event.GetIndex()) < m_rows.size()) show_detail(m_rows[size_t(event.GetIndex())]["id"]);
    });
    m_grid->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
        if (event.GetIndex() >= 0 && size_t(event.GetIndex()) < m_rows.size()) show_detail(m_rows[size_t(event.GetIndex())]["id"], true);
    });
    m_grid->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent&) { CallAfter([this] { show_actions(); }); });
    const auto drag_text = [this] {
        if (!m_library || selection().empty()) return wxString();
        return from_u8(Json{{"porca_library",m_library->root().u8string()},{"models",selection()}}.dump());
    };
    m_grid->Bind(wxEVT_LIST_BEGIN_DRAG, [this,drag_text](wxListEvent&) {
        const auto value = drag_text(); if (value.empty()) return;
        wxTextDataObject data(value); wxDropSource source(m_grid); source.SetData(data); source.DoDragDrop(wxDrag_CopyOnly);
    });
    m_list->EnableDragSource(wxDF_TEXT);
    m_list->Bind(wxEVT_DATAVIEW_ITEM_BEGIN_DRAG, [drag_text](wxDataViewEvent& event) {
        const auto value = drag_text(); if (value.empty()) { event.Veto(); return; }
        event.SetDataObject(new wxTextDataObject(value)); event.SetDragFlags(wxDrag_CopyOnly); event.Allow();
    });
    m_navigation->SetDropTarget(new CollectionDropTarget([this](wxCoord x, wxCoord y, const wxString& value) {
        if (!m_library || value.size() > 1024 * 1024) return false;
        int flags = 0; const auto item = m_navigation->HitTest(wxPoint(x,y),flags);
        if (!item.IsOk()) return false;
        auto* target = dynamic_cast<NavigationData*>(m_navigation->GetItemData(item));
        if (!target || !target->filter.contains("collection")) return false;
        const Id collection = target->filter["collection"];
        for (const auto& row : m_collections) if (row["id"] == collection && !row["query"].is_null()) return false;
        const auto data = Json::parse(into_u8(value),nullptr,false);
        if (!data.is_object() || data.value("porca_library",Json()) != m_library->root().u8string() || !data.contains("models") || !data["models"].is_array()) return false;
        std::vector<Id> ids;
        for (const auto& id : data["models"]) {
            if (!id.is_number_integer()) return false;
            const auto known = std::find_if(m_rows.begin(),m_rows.end(),[&](const Json& row) { return row["id"] == id; });
            if (known == m_rows.end()) return false;
            ids.push_back(id.get<Id>());
        }
        const auto library = m_library;
        CallAfter([this,library,collection,ids] { if (library == m_library) operation(_L("Adding to collection…"),[collection,ids](auto& lib) { lib.collect(collection,ids,true); }); });
        return !ids.empty();
    }));
    m_list->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent&) { CallAfter([this] { show_actions(); }); });
    m_files->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) { CallAfter([this] { preview(); }); });
    m_files->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent&) { CallAfter([this] { file_actions(); }); });
    m_navigation->Bind(wxEVT_TREE_ITEM_MENU, [this](wxTreeEvent& event) {
        if (auto* data = dynamic_cast<NavigationData*>(m_navigation->GetItemData(event.GetItem())); data && data->filter.contains("collection")) {
            const auto id = data->filter["collection"];
            CallAfter([this, id] {
                wxMenu menu; menu.Append(1, _L("Rename collection")); menu.Append(2, _L("Delete collection"));
                const int action = GetPopupMenuSelectionFromUser(menu);
                if (action == 1) {
                    std::string name;
                    for (const auto& row : m_collections) if (row["id"] == id) name = text(row, "name");
                    const auto values = edit_fields(this, _L("Edit collection"), {{_L("Collection name"), name}});
                    if (values) operation(_L("Updating collection…"), [id, name = (*values)[0]](auto& lib) { lib.update_collection(id, name); });
                } else if (action == 2) {
                    MessageDialog confirm(this, _L("Delete this collection? Its models will stay in your library."), _L("Delete collection"), wxYES_NO | wxCANCEL);
                    if (confirm.ShowModal() == wxID_YES) { m_filter = Json::object(); operation(_L("Deleting collection…"), [id](auto& lib) { lib.delete_collection(id); }); }
                }
            });
        }
    });
    m_navigation->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
        if (m_refreshing) return;
        if (auto* data = dynamic_cast<NavigationData*>(m_navigation->GetItemData(event.GetItem()))) { m_filter = data->filter; refresh_library(); }
    });
    SetDropTarget(new LibraryDropTarget([this](auto paths) { CallAfter([this, paths = std::move(paths)]() mutable { import_paths(std::move(paths)); }); }));
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.CmdDown() && event.GetKeyCode() == 'F') { m_search->GetTextCtrl()->SetFocus(); m_search->GetTextCtrl()->SelectAll(); }
        else if (event.CmdDown() && event.GetKeyCode() == 'K') CallAfter([this] { show_actions("palette"); });
        else if (event.CmdDown() && event.GetKeyCode() == 'O') { const bool folder = event.ShiftDown(); CallAfter([this,folder] { import_files(folder); }); }
        else if (event.CmdDown() && event.GetKeyCode() == 'I') { m_inspector->Show(!m_inspector->IsShown()); Layout(); }
        else if (dynamic_cast<wxTextCtrl*>(wxWindow::FindFocus())) event.Skip();
        else if (event.GetKeyCode() == WXK_SPACE) CallAfter([this] { preview(); });
        else if (event.GetKeyCode() == WXK_RETURN && event.CmdDown()) CallAfter([this] { add_to_plate(); });
        else if (event.GetKeyCode() == WXK_RETURN) CallAfter([this] { browse_model(); });
        else if (event.CmdDown() && event.GetKeyCode() >= '1' && event.GetKeyCode() <= '4') {
            m_filter = event.GetKeyCode() == '2' ? Json{{"favorites",true}} : event.GetKeyCode() == '3' ? Json{{"printed",true}} : event.GetKeyCode() == '4' ? Json{{"archived",true}} : Json::object(); refresh_library();
        } else {
            std::string command;
            if (!event.CmdDown() && event.GetKeyCode() == 'T') command = "tag";
            if (event.CmdDown()) {
                if (event.GetKeyCode() == 'D') command = "favorite";
                if (event.GetKeyCode() == 'E') command = event.ShiftDown() ? "rename" : "archive";
                if (event.GetKeyCode() == 'G') command = event.ShiftDown() ? "unstack" : "stack";
                if (event.GetKeyCode() == 'C' && event.ShiftDown()) command = "collection";
                if (event.GetKeyCode() == 'P' && event.ShiftDown()) command = "print";
                if (event.GetKeyCode() == WXK_BACK || event.GetKeyCode() == WXK_DELETE) command = "trash";
            }
            if (!command.empty()) CallAfter([this,command] { show_actions(command); }); else event.Skip();
        }
    });
    sys_color_changed();
    const auto path = wxGetApp().app_config->get("model_library_path");
    if (!path.empty()) open_library(path);
}

ModelLibraryPanel::~ModelLibraryPanel()
{
    *m_alive = false;
    m_events.Stop(); m_search_timer.Stop();
    m_worker->cancel_all();
    m_worker.reset();
}
bool ModelLibraryPanel::Show(bool show) { const bool changed = wxPanel::Show(show); if (show && m_library) refresh_library(); return changed; }
void ModelLibraryPanel::msw_rescale() { m_search->Rescale(); Layout(); }
void ModelLibraryPanel::sys_color_changed()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F4F4F4")));
    m_inspector->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_grid->SetBackgroundColour(GetBackgroundColour());
    m_grid->SetTextColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    wxGetApp().UpdateDVCDarkUI(m_list);
    wxGetApp().UpdateDVCDarkUI(m_files);
    m_navigation->SetBackgroundColour(GetBackgroundColour());
    m_navigation->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    for (auto* label : {m_title, m_details, m_status}) label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    Refresh();
}

void ModelLibraryPanel::open_library(const std::string& path)
{
    m_status->SetLabel(_L("Opening library…"));
    const auto alive = m_alive;
    auto library = std::make_shared<std::shared_ptr<ModelLibrary::Library>>();
    const auto downloads = library_fs::u8path(into_u8(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads)));
#ifdef __APPLE__
    const auto config = library_fs::u8path(into_u8(wxGetHomeDir())) / "Library" / "Application Support";
#elif defined(_WIN32)
    const auto config = library_fs::u8path(into_u8(wxStandardPaths::Get().GetUserConfigDir()));
#else
    wxString xdg; wxGetEnv("XDG_CONFIG_HOME", &xdg);
    const auto config = xdg.empty() ? library_fs::u8path(into_u8(wxGetHomeDir())) / ".config" : library_fs::u8path(into_u8(xdg));
#endif
    queue_job(*m_worker, [path, library, downloads, config](Job::Ctl& ctl) {
        if (ctl.was_canceled()) return;
        const bool existing = library_fs::exists(library_fs::u8path(path) / "library.db");
        *library = std::make_shared<ModelLibrary::Library>(library_fs::u8path(path));
        if (!existing && library_fs::is_directory(downloads)) (*library)->add_watch(downloads, true);
        auto seen = (*library)->settings().value("slicer_watch_paths", Json::array());
        for (const auto& folder : ModelLibrary::slicer_download_folders(config)) {
            const auto value = folder.u8string();
            if (std::find(seen.begin(),seen.end(),value) != seen.end()) continue;
            bool watched = false;
            for (const auto& row : (*library)->watch_folders()) if (row["path"] == value) watched = true;
            if (!watched) { (*library)->add_watch(folder,true); seen.push_back(value); }
        }
        (*library)->set_setting("slicer_watch_paths",seen);
        (*library)->repair_source_links([&ctl] { return ctl.was_canceled(); });
    }, [this, alive, library, path](bool cancelled, std::exception_ptr& error) {
        if (error) {
            try { std::rethrow_exception(error); } catch (const std::exception& e) { if (*alive) m_status->SetLabel(from_u8(e.what())); }
            error = nullptr; return;
        }
        if (!*alive || cancelled || !*library) return;
        m_library = *library; m_watch = std::make_shared<ModelLibrary::Watch>(); m_selected = 0; m_detail = Json();
        wxGetApp().app_config->set("model_library_path", path); wxGetApp().app_config->save();
        refresh_library();
    });
}
void ModelLibraryPanel::choose_library()
{
    wxDirDialog dialog(this, _L("Choose a library folder"), m_library ? from_u8(m_library->root().u8string()) : wxStandardPaths::Get().GetDocumentsDir());
    if (dialog.ShowModal() == wxID_OK) open_library(into_u8(dialog.GetPath()));
}

void ModelLibraryPanel::refresh_library()
{
    if (!m_library) return;
    auto filter = m_filter;
    filter["collapse_stacks"] = true;
    filter["families"] = m_group == "family";
    const auto query = into_u8(m_search->GetTextCtrl()->GetValue());
    if (!query.empty()) filter["search"] = filter.value("search", "") + ' ' + query;
    auto result = std::make_shared<Json>();
    const auto library = m_library; const auto alive = m_alive; const auto generation = ++m_generation;
    queue_job(*m_worker, [library, filter, result](Job::Ctl& ctl) {
        if (ctl.was_canceled()) return;
        *result = {{"models", library->models(filter)}, {"facets", library->facets()}, {"collections", library->collections()}, {"settings", library->settings()}, {"folders", library->watch_folders()}};
    }, [this, alive, result, generation](bool cancelled, std::exception_ptr& error) {
        if (error) { try { std::rethrow_exception(error); } catch (const std::exception& e) { if (*alive) m_status->SetLabel(from_u8(e.what())); } error = nullptr; return; }
        if (!*alive || cancelled || generation != m_generation || result->is_null()) return;
        m_refreshing = true;
        const auto selected = selection();
        m_rows = (*result)["models"];
        if (!m_group.empty()) std::stable_sort(m_rows.begin(), m_rows.end(), [this](const Json& a, const Json& b) {
            const auto key = [this](const Json& row) { return m_group == "month" ? text(row, "added_at").substr(0, 7) : text(row, m_group); };
            return key(a) < key(b);
        });
        m_settings = (*result)["settings"];
        m_folders = (*result)["folders"];
        m_collections = (*result)["collections"];
        m_list->DeleteAllItems(); m_grid->DeleteAllItems();
        m_grid_images.clear();
        m_grid_images.push_back(wxArtProvider::GetBitmapBundle(wxART_NORMAL_FILE,wxART_OTHER,wxSize(128,128)));
        m_grid->SetNormalImages(m_grid_images);
        for (const auto& row : m_rows) {
            wxVector<wxVariant> values;
            const auto versions = row.value("stack_size",Id(0));
            wxString name = from_u8((row.value("favorite",0) ? "★ " : "") + text(row,"name"));
            if (versions > 1 && !m_filter.contains("stack_id")) name += wxString::Format(_L(" (%lld versions)"), static_cast<long long>(versions));
            if (!m_group.empty()) {
                const auto group = m_group == "month" ? text(row,"added_at").substr(0,7) : text(row,m_group);
                if (!group.empty()) name = from_u8(group) + " — " + name;
            }
            values.push_back(name);
            values.push_back(from_u8(text(row, "designer")));
            values.push_back(wxString::Format("%lld", static_cast<long long>(row["file_count"].get<Id>())));
            values.push_back(from_u8(text(row, "formats")));
            values.push_back(from_u8(text(row, "added_at").substr(0, 10)));
            m_list->AppendItem(values);
            const long index = m_grid->InsertItem(m_grid->GetItemCount(), name, 0);
            if (std::find(selected.begin(), selected.end(), row["id"].get<Id>()) != selected.end()) {
                m_grid->SetItemState(index, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
                m_list->SelectRow(unsigned(index));
            }
        }
        // Load a bounded set per idle cycle so opening a large library never
        // decodes all its previews in one UI event.
        rebuild_navigation((*result)["facets"], (*result)["collections"]);
        m_refreshing = false;
        const auto count = static_cast<unsigned>(m_rows.size());
        m_status->SetLabel(wxString::Format(_L_PLURAL("%u model", "%u models", count), count));
        const auto visible = std::find_if(m_rows.begin(),m_rows.end(),[this](const Json& row) { return row["id"] == m_selected; });
        if (m_selected && visible != m_rows.end()) show_detail(m_selected);
        else { m_selected = 0; m_detail = Json(); update_inspector(); }
    });
}

void ModelLibraryPanel::rebuild_navigation(const Json& facets, const Json& collections)
{
    m_navigation->DeleteAllItems();
    const auto root = m_navigation->AddRoot("Library");
    const auto add = [this](const wxTreeItemId& parent, const wxString& label, Json filter) { return m_navigation->AppendItem(parent, label, -1, -1, new NavigationData(std::move(filter))); };
    add(root, _L("All models"), Json::object());
    add(root, _L("Favorites"), {{"favorites", true}});
    add(root, _L("Printed"), {{"printed", true}});
    add(root, _L("Archived"), {{"archived", true}});
    if (!collections.empty()) {
        const auto group = m_navigation->AppendItem(root, _L("Collections"));
        for (const auto& collection : collections) add(group, from_u8(text(collection, "name")), {{"collection", collection["id"]}});
        m_navigation->Expand(group);
    }
    for (const auto& [key, label] : std::vector<std::pair<std::string, wxString>>{{"tags", _L("Tags")}, {"designers", _L("Designers")}, {"sources", _L("Sources")}, {"formats", _L("Formats")}}) {
        const auto group = m_navigation->AppendItem(root, label);
        for (const auto& entry : facets[key]) {
            const auto name = text(entry, "name");
            Json filter;
            if (key == "tags") filter = {{"tag", name}};
            else if (key == "designers") filter = {{"designer", name}};
            else filter = {{"search", (key == "sources" ? "from:\"" : "has:\"") + name + '"'}};
            add(group, from_u8(name), std::move(filter));
        }
        if (key == "tags") m_navigation->Expand(group);
    }
}

std::vector<Id> ModelLibraryPanel::selection() const
{
    wxDataViewItemArray selected; m_list->GetSelections(selected);
    std::vector<Id> ids;
    if (m_grid_view) {
        for (long index = m_grid->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); index != -1; index = m_grid->GetNextItem(index, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
            if (size_t(index) < m_rows.size()) ids.push_back(m_rows[size_t(index)]["id"]);
        selected.clear();
    }
    for (const auto& item : selected) { const int row = m_list->ItemToRow(item); if (row >= 0 && size_t(row) < m_rows.size()) ids.push_back(m_rows[size_t(row)]["id"]); }
    return ids;
}
void ModelLibraryPanel::show_detail(Id id, bool browse)
{
    if (m_selected != id) { m_detail = Json(); update_inspector(); }
    m_selected = id;
    const auto library = m_library; const auto alive = m_alive;
    auto result = std::make_shared<Json>();
    queue_job(*m_worker, [library, id, result](Job::Ctl&) { *result = library->detail(id); }, [this, alive, result, id, library, browse](bool cancelled, std::exception_ptr& error) {
        if (error) { error = nullptr; if (*alive && id == m_selected) { m_detail = Json(); update_inspector(); } return; }
        if (!*alive || cancelled || library != m_library || id != m_selected) return;
        m_detail = *result; update_inspector();
        if (browse) browse_model();
    });
}
void ModelLibraryPanel::update_inspector()
{
    m_files->DeleteAllItems();
    m_cover->SetBitmap(wxNullBitmap);
    if (m_detail.is_null()) { m_title->SetLabel(_L("Choose a model to see its files")); m_details->SetLabel(""); return; }
    m_title->SetLabel(from_u8(text(m_detail, "name")));
    std::string info;
    for (const auto& key : {"designer", "license", "description", "notes"}) {
        const auto value = text(m_detail, key);
        if (!value.empty()) { if (!info.empty()) info += "\n\n"; info += value; }
    }
    if (!m_detail["tags"].empty()) {
        info += "\n\n";
        for (const auto& tag : m_detail["tags"]) info += '#' + text(tag, "name") + ' ';
    }
    m_details->SetLabel(from_u8(info));
    m_details->Wrap(-1); m_details->Wrap(std::max(FromDIP(240), m_inspector->GetClientSize().x - FromDIP(24)));
    for (const auto& file : m_detail["files"]) {
        auto path = text(file, "rel_path");
        const auto prefix = text(m_detail, "dir") + '/';
        if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
        wxVector<wxVariant> values; values.push_back(from_u8(path)); m_files->AppendItem(values);
    }
    const auto thumbnail = text(m_detail, "thumb");
    if (!thumbnail.empty()) {
        const auto library = m_library; const auto alive = m_alive; const auto id = m_selected;
        auto pixels = std::make_shared<ModelLibrary::PreviewPixels>();
        queue_job(*m_worker, [library, thumbnail, pixels](Job::Ctl& ctl) { if (!ctl.was_canceled()) *pixels = ModelLibrary::read_preview(library->resolve(thumbnail),240); },
                  [this, alive, library, id, pixels](bool cancelled, std::exception_ptr& error) {
            if (error) { error = nullptr; return; }
            if (!*alive || cancelled || library != m_library || id != m_selected || pixels->rgb.empty()) return;
            wxImage image(int(pixels->width),int(pixels->height)); std::memcpy(image.GetData(),pixels->rgb.data(),pixels->rgb.size());
            m_cover->SetBitmap(wxBitmap(image)); m_inspector->Layout(); m_inspector->FitInside();
        });
    }
    m_inspector->Layout(); m_inspector->FitInside();
}

void ModelLibraryPanel::operation(const wxString& label, std::function<void(ModelLibrary::Library&)> work)
{
    if (!m_library) { choose_library(); return; }
    const auto library = m_library; const auto alive = m_alive;
    m_status->SetLabel(label);
    queue_job(*m_worker, [library, work = std::move(work)](Job::Ctl& ctl) { if (!ctl.was_canceled()) work(*library); }, [this, alive, library](bool cancelled, std::exception_ptr& error) {
        if (error) { try { std::rethrow_exception(error); } catch (const std::exception& e) { if (*alive) m_status->SetLabel(from_u8(e.what())); } error = nullptr; return; }
        if (*alive && !cancelled && library == m_library) refresh_library();
    });
}

void ModelLibraryPanel::import_files(bool folder, bool organized)
{
    if (!m_library) { choose_library(); return; }
    if (folder) {
        wxDirDialog dialog(this, organized ? _L("Import an organized collection") : _L("Import a model folder"));
        if (dialog.ShowModal() != wxID_OK) return;
        const auto path = library_fs::u8path(into_u8(dialog.GetPath()));
        if (!organized) { import_paths({path}); return; }
        const auto library = m_library; const auto alive = m_alive;
        auto plan = std::make_shared<Json>();
        queue_job(*m_worker, [library, path, plan](Job::Ctl& ctl) { *plan = library->plan_organized(path, [&ctl] { return ctl.was_canceled(); }); }, [this, alive, library, path, plan](bool cancelled, std::exception_ptr& error) {
            if (error) { try { std::rethrow_exception(error); } catch (const std::exception& e) { if (*alive) m_status->SetLabel(from_u8(e.what())); } error = nullptr; return; }
            if (!*alive || cancelled || library != m_library) return;
            std::vector<std::pair<wxString, wxString>> choices;
            std::vector<wxString> details;
            for (const auto& category : *plan) {
                unsigned additions = 0, matches = 0, versions = 0, skipped = 0;
                wxString review = from_u8(text(category,"name")) + "\n";
                for (const auto& item : category["items"]) {
                    if (item["status"] == "new") ++additions;
                    else if (item["status"] == "duplicate") ++matches;
                    else if (item["status"] == "version") ++versions;
                    else ++skipped;
                    const auto label = item["status"] == "new" ? _L("Add") : item["status"] == "duplicate" ? _L("Already saved") : item["status"] == "version" ? _L("Possible version") : _L("Keep in original folder");
                    review += label + ": " + from_u8(text(item,"name")) + "\n";
                    if (!text(item,"reason").empty()) review += "  " + from_u8(text(item,"reason")) + "\n";
                    for (const auto& left : item["left_out"]) { ++skipped; review += "  " + _L("Keep in original folder") + ": " + from_u8(left.get<std::string>()) + "\n"; }
                }
                details.push_back(review);
                choices.emplace_back(text(category, "name").empty() ? _L("Loose files") : from_u8(text(category, "name")),
                    wxString::Format(_L("%u to add, %u saved, %u possible versions, %u left behind"), additions, matches, versions, skipped));
            }
            const auto selected = choose_items(this, _L("Choose folders to import"), choices, details);
            if (!selected || selected->empty()) return;
            std::vector<std::string> categories;
            for (size_t index : *selected) categories.push_back(text((*plan)[index], "name"));
            operation(_L("Importing collection…"), [path, categories](auto& lib) { lib.import_organized(path, categories, ModelLibrary::extract_file); });
        });
    } else {
        wxFileDialog dialog(this, _L("Import models"), "", "", _L("All files") + "|*.*", wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        wxArrayString names; dialog.GetPaths(names);
        std::vector<library_fs::path> paths;
        for (const auto& name : names) paths.push_back(library_fs::u8path(into_u8(name)));
        import_paths(std::move(paths));
    }
}

void ModelLibraryPanel::import_paths(std::vector<library_fs::path> paths)
{
    if (!m_library) { choose_library(); return; }
    const auto library = m_library; const auto alive = m_alive;
    auto results = std::make_shared<std::vector<ModelLibrary::ImportResult>>();
    m_status->SetLabel(_L("Importing models…"));
    queue_job(*m_worker, [library, paths = std::move(paths), results](Job::Ctl& ctl) {
        for (const auto& path : paths) {
            if (ctl.was_canceled()) break;
            auto result = library->import(path,ModelLibrary::extract_file,[&ctl] { return ctl.was_canceled(); });
            if (result.status != "failed" && library->settings().value("delete_source",false) && library_fs::is_regular_file(path)) {
                try { library->trash_leftovers({path},ModelLibrary::move_to_trash,[&ctl] { return ctl.was_canceled(); }); }
                catch (const std::exception&) { result.message += ". Original kept."; }
            }
            results->push_back(std::move(result));
        }
    }, [this, alive, results, library](bool cancelled, std::exception_ptr& error) {
        if (error) { error = nullptr; if (*alive) m_status->SetLabel(_L("Import failed. Your originals have been kept.")); return; }
        if (!*alive || library != m_library) return;
        refresh_library();
        for (const auto& result : *results) if (result.status == "failed") { m_status->SetLabel(from_u8(result.message)); break; }
        if (cancelled) m_status->SetLabel(_L("Import cancelled. Completed imports have been kept."));
    });
}

void ModelLibraryPanel::add_to_plate(bool project)
{
    if (!m_library || m_detail.is_null()) return;
    const auto ids = selection();
    if (!project && ids.size() > 1) {
        const auto library = m_library; const auto alive = m_alive;
        const auto destination = wxGetApp().plater()->project_session_id();
        auto paths = std::make_shared<std::vector<boost::filesystem::path>>();
        queue_job(*m_worker, [library, ids, paths](Job::Ctl& ctl) {
            for (Id id : ids) for (const auto& file : library->files(id)) {
                if (ctl.was_canceled()) return;
                if (file["kind"] == "model" && file["format"] != "gcode" && file["format"] != "bgcode") paths->emplace_back(library->resolve(file["rel_path"]).u8string());
            }
        }, [this, alive, library, paths, destination](bool cancelled, std::exception_ptr& error) {
            if (error) { error = nullptr; return; }
            if (!*alive || cancelled || library != m_library || paths->empty()) return;
            if (!wxGetApp().plater() || wxGetApp().plater()->project_session_id() != destination) {
                m_status->SetLabel(_L("Select the project you started adding these models to and try again."));
                return;
            }
            try {
                auto* plater = wxGetApp().plater(); wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
                Plater::TakeSnapshot snapshot(plater,_u8L("Add models from library"));
                plater->load_files(*paths,LoadStrategy::LoadModel,true);
            } catch (const std::exception& failure) { m_status->SetLabel(from_u8(failure.what())); }
        });
        return;
    }
    wxDataViewItemArray selected; m_files->GetSelections(selected);
    std::vector<boost::filesystem::path> paths;
    const auto append = [&](const Json& file) {
        if (file["kind"] != "model") return;
        paths.emplace_back(m_library->resolve(file["rel_path"]).u8string());
    };
    try {
        if (selected.empty()) for (const auto& file : m_detail["files"]) append(file);
        else for (const auto& item : selected) { const int row = m_files->ItemToRow(item); if (row >= 0 && size_t(row) < m_detail["files"].size()) append(m_detail["files"][size_t(row)]); }
        if (paths.empty()) { m_status->SetLabel(_L("Select a printable file to add to the plate.")); return; }
        auto* plater = wxGetApp().plater();
        if (paths.size() == 1 && (ModelLibrary::Library::format(library_fs::u8path(paths[0].string())) == "gcode" || ModelLibrary::Library::format(library_fs::u8path(paths[0].string())) == "bgcode")) {
            plater->load_gcode(from_path(paths[0])); wxGetApp().mainframe->select_tab(TAB_ID_PREVIEW); return;
        }
        if (project) {
            if (paths.size() != 1 || ModelLibrary::Library::format(library_fs::u8path(paths[0].string())) != "3mf") { m_status->SetLabel(_L("Select one 3MF project to open.")); return; }
            plater->load_project(from_path(paths[0]));
        } else {
            wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
            Plater::TakeSnapshot snapshot(plater,_u8L("Add models from library"));
            plater->load_files(paths, LoadStrategy::LoadModel, true);
        }
        wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    } catch (const std::exception& error) { m_status->SetLabel(from_u8(error.what())); }
}

void ModelLibraryPanel::edit_details()
{
    if (m_detail.is_null()) return;
    const auto values = edit_fields(this, _L("Edit model details"), {{_L("Name"), text(m_detail, "name")}, {_L("Designer"), text(m_detail, "designer")}, {_L("License"), text(m_detail, "license")}, {_L("Source link"), text(m_detail, "source_url")}});
    if (!values) return;
    const Id id = m_selected;
    operation(_L("Saving details…"), [id, values = *values](auto& library) { library.update(id, {{"name", values[0]}, {"designer", values[1]}, {"license", values[2]}, {"source_url", values[3]}}); });
}

void ModelLibraryPanel::show_actions(const std::string& command)
{
    if (!m_library) { choose_library(); return; }
    enum { Add = wxID_HIGHEST + 2310, Open, Edit, Notes, Favorite, Tag, Untag, Archive, Restore, Stack, Unstack, Top, Collection, SavedSearch, Print, History, Rename, Merge, Split, Refresh, Reveal, Source, Organized, Attention, Trash, RemoveCollection, Versions, RestoreFolders };
    wxMenu menu;
    std::vector<std::pair<int, wxString>> commands;
    const auto append = [&menu, &commands](int id, const wxString& label) { menu.Append(id, label); commands.emplace_back(id,label); };
    append(Add, _L("Add to plate")); append(Open, _L("Open project")); append(Edit, _L("Edit details")); append(Notes, _L("Edit notes"));
    menu.AppendSeparator();
    append(Favorite, _L("Toggle favorite")); append(Tag, _L("Add tag")); append(Untag, _L("Remove tag"));
    append(Collection, _L("Add to collection"));
    if (m_filter.contains("collection")) append(RemoveCollection, _L("Remove from this collection"));
    append(SavedSearch, _L("Save this search"));
    append(Versions, _L("Show all versions")); append(Stack, _L("Stack versions")); append(Unstack, _L("Unstack")); append(Top, _L("Show this version first"));
    append(Archive, _L("Archive")); append(Restore, _L("Restore from archive")); append(Trash, _L("Move to Trash"));
    menu.AppendSeparator();
    append(Print, _L("Record a print")); append(History, _L("Print history")); append(Rename, _L("Rename selected models")); append(Merge, _L("Merge selected models")); append(Split, _L("Split a folder into a model"));
    append(RestoreFolders, _L("Restore pack folders from original ZIP")); append(Refresh, _L("Refresh selected models")); append(Reveal, _L("Open model folder")); append(Source, _L("Open source page"));
    menu.AppendSeparator(); append(Organized, _L("Import organized folder")); append(Attention, _L("Needs attention"));
    int action = wxID_NONE;
    if (command == "palette") {
        LibraryDialog dialog(this,wxID_ANY,_L("Library commands"),wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* search = new ::TextInput(&dialog,""); search->GetTextCtrl()->SetHint(_L("Find an action")); sizer->Add(search,0,wxEXPAND | wxALL,FromDIP(12));
        auto* list = new wxDataViewListCtrl(&dialog,wxID_ANY,wxDefaultPosition,FromDIP(wxSize(520,400)),wxDV_NO_HEADER);
        list->AppendTextColumn(_L("Action"),wxDATAVIEW_CELL_INERT,FromDIP(500)); sizer->Add(list,1,wxEXPAND | wxALL,FromDIP(12));
        std::vector<int> visible;
        const auto fill = [&] { list->DeleteAllItems(); visible.clear(); const auto query = search->GetTextCtrl()->GetValue().Lower();
            for (const auto& item : commands) if (query.empty() || item.second.Lower().Find(query) != wxNOT_FOUND) { wxVector<wxVariant> row; row.push_back(item.second); list->AppendItem(row); visible.push_back(item.first); }
            if (!visible.empty()) list->SelectRow(0);
        };
        search->GetTextCtrl()->Bind(wxEVT_TEXT,[&](wxCommandEvent&) { fill(); });
        const auto accept = [&] { const int index = list->ItemToRow(list->GetSelection()); if (index >= 0 && size_t(index) < visible.size()) { action = visible[size_t(index)]; dialog.EndModal(wxID_OK); } };
        list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,[&](wxDataViewEvent&) { accept(); });
        dialog.Bind(wxEVT_CHAR_HOOK,[&](wxKeyEvent& event) { if (event.GetKeyCode() == WXK_RETURN) accept(); else event.Skip(); });
        fill(); dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent(); search->GetTextCtrl()->SetFocus();
        dialog.ShowModal();
    } else if (!command.empty()) {
        const std::map<std::string,int> shortcuts{{"favorite",Favorite},{"archive",Archive},{"tag",Tag},{"collection",Collection},{"print",Print},{"rename",Rename},{"stack",Stack},{"unstack",Unstack},{"trash",Trash}};
        if (const auto found = shortcuts.find(command); found != shortcuts.end()) action = found->second;
    } else action = GetPopupMenuSelectionFromUser(menu);
    const auto ids = selection();
    if (action == Organized) { import_files(true, true); return; }
    if (action == Attention) { show_attention(); return; }
    if (action == SavedSearch) {
        const auto values = edit_fields(this, _L("Save search"), {{_L("Name"), ""}});
        if (values) { const auto query = into_u8(m_search->GetTextCtrl()->GetValue()); operation(_L("Saving search…"), [name = (*values)[0], query](auto& lib) { lib.create_collection(name, &query); }); }
        return;
    }
    if (ids.empty() || action == wxID_NONE) return;
    if (action == Add || action == Open) { add_to_plate(action == Open); return; }
    if (action == Edit) { edit_details(); return; }
    if (action == Versions && !m_detail.is_null() && !m_detail["stack_id"].is_null()) { m_filter = {{"stack_id",m_detail["stack_id"]}}; refresh_library(); return; }
    if (action == Reveal || action == Source) {
        try {
            if (action == Reveal) wxLaunchDefaultApplication(from_u8(m_library->resolve(m_detail["dir"]).u8string()));
            else { const auto url = text(m_detail, "source_url"); if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) wxLaunchDefaultBrowser(from_u8(url)); }
        } catch (const std::exception& error) { m_status->SetLabel(from_u8(error.what())); }
        return;
    }
    if (action == Trash) {
        MessageDialog confirm(this, _L("Move the selected models and their library files to the Trash? Files outside the library will stay where they are."), _L("Move models to Trash"), wxYES_NO | wxCANCEL);
        if (confirm.ShowModal() == wxID_YES) operation(_L("Moving models to Trash…"), [ids](auto& lib) { lib.trash_models(ids, ModelLibrary::move_to_trash); });
    } else if (action == RemoveCollection) {
        const Id collection = m_filter["collection"];
        operation(_L("Updating collection…"), [collection, ids](auto& lib) { lib.collect(collection, ids, false); });
    } else if (action == Notes) {
        const auto values = edit_fields(this, _L("Edit notes"), {{_L("Notes"), text(m_detail, "notes")}}, true);
        if (values) operation(_L("Saving notes…"), [id = m_selected, notes = (*values)[0]](auto& lib) { lib.update(id, {{"notes", notes}}); });
    } else if (action == Favorite) operation(_L("Updating favorites…"), [ids](auto& lib) { for (Id id : ids) lib.update(id, {{"favorite", lib.detail(id).value("favorite", 0) == 0}}); });
    else if (action == Tag || action == Untag) {
        const auto values = edit_fields(this, action == Tag ? _L("Add tag") : _L("Remove tag"), {{_L("Tag"), ""}});
        if (values) operation(_L("Updating tags…"), [ids, name = (*values)[0], on = action == Tag](auto& lib) { lib.tag(ids, name, on); });
    } else if (action == Collection) {
        const auto values = edit_fields(this, _L("Add to collection"), {{_L("Collection name"), ""}});
        if (values) operation(_L("Updating collection…"), [ids, name = (*values)[0]](auto& lib) {
            Id collection = 0; for (const auto& row : lib.collections()) if (row["name"] == name && row["query"].is_null()) collection = row["id"];
            if (!collection) collection = lib.create_collection(name);
            lib.collect(collection, ids, true);
        });
    } else if (action == Stack) operation(_L("Stacking versions…"), [ids](auto& lib) { lib.stack(ids); });
    else if (action == Unstack) operation(_L("Unstacking versions…"), [ids](auto& lib) { lib.unstack(ids); });
    else if (action == Top) operation(_L("Updating version…"), [id = m_selected](auto& lib) { lib.set_stack_top(id); });
    else if (action == Archive || action == Restore) operation(_L("Updating models…"), [ids, archived = action == Archive](auto& lib) { lib.archive(ids, archived); });
    else if (action == Print) {
        const auto outcome = choose_outcome(*this);
        if (!outcome) return;
        const auto values = edit_fields(this, _L("Record a print"), {{_L("Notes"), ""}}, true);
        if (values) operation(_L("Recording print…"), [ids, outcome = *outcome, notes = (*values)[0]](auto& lib) { for (Id id : ids) lib.record_print(id, outcome, notes); });
    } else if (action == History) {
        edit_print_history();
    } else if (action == Rename) {
        const auto values = edit_fields(this, _L("Rename selected models"), {{_L("Find"), ""}, {_L("Replace with"), ""}});
        if (values) operation(_L("Renaming models…"), [ids, values = *values](auto& lib) { lib.rename(ids, values[0], values[1]); });
    } else if (action == Merge && ids.size() > 1) {
        MessageDialog confirm(this, _L("Combine the selected models into the first selected model? Their files and print history will be kept."), _L("Merge models"), wxYES_NO | wxCANCEL);
        if (confirm.ShowModal() == wxID_YES) operation(_L("Merging models…"), [ids](auto& lib) { lib.merge(ids.front(), ids); });
    } else if (action == Split) {
        const auto values = edit_fields(this, _L("Split folder"), {{_L("Folder inside this model"), ""}});
        if (values) operation(_L("Splitting folder…"), [id = m_selected, folder = (*values)[0]](auto& lib) { lib.split(id, folder); });
    } else if (action == RestoreFolders) operation(_L("Restoring pack folders…"), [ids](auto& lib) { for (Id id : ids) lib.restore_pack_layout(id); });
    else if (action == Refresh) operation(_L("Refreshing models…"), [ids](auto& lib) { for (Id id : ids) lib.rescan(id, ModelLibrary::extract_file); });
}

void ModelLibraryPanel::show_settings()
{
    if (!m_library) { choose_library(); return; }
    enum { Choose = wxID_HIGHEST + 2400, Watch, ManageWatch, Scan, Rules, Refresh, Backup, Cancel, Online, AutoStack, ExtractZips, DeleteSource };
    wxMenu menu;
    menu.Append(Choose, _L("Choose library")); menu.Append(Watch, _L("Add watched folder")); menu.Append(ManageWatch, _L("Manage watched folders")); menu.Append(Scan, _L("Scan watched folders"));
    menu.Append(Rules, _L("Tag rules")); menu.Append(Online, _L("Online details and AI tags"));
    menu.AppendCheckItem(AutoStack, _L("Automatically group versions"))->Check(m_settings.value("auto_stack", true)); menu.Append(Refresh, _L("Refresh all models")); menu.Append(Backup, _L("Back up library")); menu.Append(Cancel, _L("Cancel background work"));
    menu.AppendCheckItem(ExtractZips,_L("Unpack ZIP downloads"))->Check(m_settings.value("extract_zips",true));
    menu.AppendCheckItem(DeleteSource,_L("Move fully saved downloads to Trash"))->Check(m_settings.value("delete_source",false));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action == ExtractZips || action == DeleteSource) {
        const auto key = action == ExtractZips ? "extract_zips" : "delete_source";
        const bool enabled = !m_settings.value(key,action == ExtractZips);
        if (enabled && action == DeleteSource) {
            MessageDialog confirm(this,_L("Automatically move downloaded files and ZIPs to Trash after every file is verified in your library? Folders and downloads managed by other slicers will stay in place."),_L("Clean up saved downloads"),wxYES_NO | wxCANCEL);
            if (confirm.ShowModal() != wxID_YES) return;
        }
        operation(_L("Updating import preferences…"),[key,enabled](auto& lib) { lib.set_setting(key,enabled); });
    }
    else if (action == Choose) choose_library();
    else if (action == Watch) {
        wxDirDialog dialog(this, _L("Choose a folder to watch"));
        if (dialog.ShowModal() == wxID_OK) operation(_L("Adding watched folder…"), [path = library_fs::u8path(into_u8(dialog.GetPath()))](auto& lib) { lib.add_watch(path, true); });
    } else if (action == ManageWatch) edit_watch_folders();
    else if (action == Scan) {
        const auto library = m_library; const auto alive = m_alive;
        auto paths = std::make_shared<std::vector<library_fs::path>>();
        queue_job(*m_worker, [library, paths](Job::Ctl&) {
            for (const auto& folder : library->watch_folders()) if (folder.value("enabled", 0)) {
                const auto root = library_fs::u8path(folder["path"].get<std::string>());
                if (!library_fs::is_directory(root)) continue;
                const auto items = library->scan_items(root, folder.value("recursive", 0) != 0); paths->insert(paths->end(), items.begin(), items.end());
            }
        }, [this, alive, paths](bool cancelled, std::exception_ptr& error) { if (error) { error = nullptr; return; } if (*alive && !cancelled) import_paths(std::move(*paths)); });
    } else if (action == Online) online_settings();
    else if (action == AutoStack) { const bool enabled = !m_settings.value("auto_stack", true); operation(_L("Updating organization…"), [enabled](auto& lib) { lib.set_setting("auto_stack", enabled); lib.organize(); }); }
    else if (action == Rules) {
        edit_tag_rules();
    } else if (action == Refresh) operation(_L("Refreshing library…"), [](auto& lib) { for (const auto& row : lib.models()) lib.rescan(row["id"], ModelLibrary::extract_file); });
    else if (action == Backup) operation(_L("Backing up library…"), [](auto& lib) { lib.backup(); });
    else if (action == Cancel) { m_worker->cancel_all(); m_status->SetLabel(_L("Cancelling background work…")); }
}

void ModelLibraryPanel::show_activity()
{
    if (!m_library) return;
    wxMenu menu; menu.Append(1,_L("View import history")); menu.Append(2,_L("Clear import history"));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action == 2) {
        MessageDialog confirm(this,_L("Clear import history? Your models and files will be kept."),_L("Clear import history"),wxYES_NO | wxCANCEL);
        if (confirm.ShowModal() == wxID_YES) operation(_L("Clearing import history…"),[](auto& lib) { lib.clear_activity(); });
        return;
    }
    if (action != 1) return;
    const auto library = m_library; const auto alive = m_alive; auto result = std::make_shared<std::string>();
    queue_job(*m_worker, [library, result](Job::Ctl&) { for (const auto& row : library->activity()) *result += text(row, "at") + "  " + text(row, "status") + "\n" + text(row, "path") + "\n" + text(row, "message") + "\n\n"; },
              [this, alive, result](bool cancelled, std::exception_ptr& error) { if (error) { error = nullptr; return; } if (*alive && !cancelled) read_only_text(this, _L("Import activity"), *result); });
}
void ModelLibraryPanel::show_attention()
{
    if (!m_library) return;
    wxMenu menu; menu.Append(1, _L("Review models")); menu.Append(2, _L("Review originals already saved in the library"));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action != 1 && action != 2) return;
    const auto library = m_library; const auto alive = m_alive; auto result = std::make_shared<Json>();
    m_status->SetLabel(_L("Checking your library…"));
    queue_job(*m_worker, [library, result, action](Job::Ctl& ctl) {
        *result = action == 1 ? library->attention() : library->leftovers([&ctl] { return ctl.was_canceled(); });
    }, [this, alive, library, result, action](bool cancelled, std::exception_ptr& error) {
        if (error) { error = nullptr; return; }
        if (!*alive || cancelled || library != m_library) return;
        if (result->empty()) { m_status->SetLabel(_L("Nothing needs attention.")); return; }
        if (action == 2) {
            std::vector<std::pair<wxString, wxString>> choices;
            for (const auto& row : *result) choices.emplace_back(from_u8(text(row, "name")), from_u8(text(row, "path")));
            const auto selected = choose_items(this, _L("Choose originals to move to Trash"), choices);
            if (!selected || selected->empty()) return;
            std::vector<library_fs::path> paths;
            for (size_t index : *selected) paths.push_back(library_fs::u8path(text((*result)[index], "path")));
            operation(_L("Checking and moving originals to Trash…"), [paths](auto& lib) { lib.trash_leftovers(paths, ModelLibrary::move_to_trash); });
            return;
        }
        LibraryDialog dialog(this, wxID_ANY, _L("Needs attention"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* list = new wxDataViewListCtrl(&dialog, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(720, 420)), wxDV_ROW_LINES);
        list->AppendTextColumn(_L("Model"), wxDATAVIEW_CELL_INERT, FromDIP(260));
        list->AppendTextColumn(_L("Review"), wxDATAVIEW_CELL_INERT, FromDIP(430));
        for (const auto& row : *result) { wxVector<wxVariant> values; values.push_back(from_u8(text(row, "name"))); values.push_back(from_u8(text(row, "suggestion"))); list->AppendItem(values); }
        sizer->Add(list, 1, wxEXPAND | wxALL, FromDIP(16));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        const auto add = [&](const wxString& label, int action_id) {
            auto* button = new Button(&dialog, label); button->SetStyle(ButtonStyle::Regular, ButtonType::Window); actions->Add(button, 0, wxRIGHT, FromDIP(8));
            button->Bind(wxEVT_BUTTON, [&dialog, list, action_id](wxCommandEvent&) { if (list->ItemToRow(list->GetSelection()) >= 0) dialog.EndModal(action_id); });
        };
        add(_L("Review model"), 1); add(_L("Rename"), 2); add(_L("Keep as it is"), 3);
        sizer->Add(actions, 0, wxALL, FromDIP(16)); dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
        const int response = dialog.ShowModal();
        const int index = list->ItemToRow(list->GetSelection());
        if (index < 0 || size_t(index) >= result->size()) return;
        const auto row = (*result)[size_t(index)]; const Id id = row["model_id"];
        if (response == 1) show_detail(id);
        else if (response == 2) {
            const auto values = edit_fields(this, _L("Rename model"), {{_L("Name"), text(row, "kind") == "name" ? text(row, "suggestion") : text(row, "name")}});
            if (values) operation(_L("Renaming model…"), [id, name = (*values)[0]](auto& lib) { lib.update(id, {{"name", name}}); });
        } else if (response == 3) operation(_L("Keeping model…"), [id, kind = text(row, "kind")](auto& lib) { lib.dismiss(id, kind); });
    });
}

void ModelLibraryPanel::online_settings()
{
    wxMenu menu;
    menu.AppendCheckItem(1, _L("Fetch details and photos from Printables and Thingiverse"))->Check(m_online_enabled && m_settings.value("page_details", false));
    menu.AppendCheckItem(2, _L("Suggest tags with Claude"))->Check(m_online_enabled && m_settings.value("ai_tagging", false));
    menu.Append(3, _L("Save Anthropic API key")); menu.Append(4, _L("Remove saved API key")); menu.Append(5, _L("Turn off online library features"));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action == 3) {
        LibraryDialog dialog(this, wxID_ANY, _L("Save Anthropic API key"));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* explanation = new Label(&dialog, _L("Your key is saved in this computer's secure credential store. It stays out of your library and projects."));
        explanation->Wrap(FromDIP(440)); sizer->Add(explanation, 0, wxALL, FromDIP(16));
        auto* input = new wxTextCtrl(&dialog, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(440,32)), wxTE_PASSWORD);
        sizer->Add(input, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* buttons = new DialogButtons(&dialog, {"OK","Cancel"});
        buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
        sizer->Add(buttons,0,wxEXPAND | wxALL,FromDIP(12)); dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
        if (dialog.ShowModal() != wxID_OK || input->GetValue().empty()) return;
        auto store = wxSecretStore::GetDefault();
        if (!store.IsOk() || !store.Save("Porca Model Library Anthropic","api-key",wxSecretValue(input->GetValue()))) { m_status->SetLabel(_L("The secure credential store could not save your key.")); return; }
        m_api_key = into_u8(input->GetValue()); m_status->SetLabel(_L("API key saved."));
    } else if (action == 4) {
        auto store = wxSecretStore::GetDefault();
        if (store.IsOk() && store.Delete("Porca Model Library Anthropic")) { m_api_key.clear(); operation(_L("Turning off AI tags…"), [](auto& lib) { lib.set_setting("ai_tagging",false); }); }
    } else if (action == 5) {
        m_online_enabled = false; m_worker->cancel_all(); wxGetApp().app_config->set("model_library_online","0"); wxGetApp().app_config->save();
    } else if (action == 1 || action == 2) {
        const auto key = action == 1 ? "page_details" : "ai_tagging";
        const bool enabled = !(m_online_enabled && m_settings.value(key, false));
        if (enabled) {
            if (action == 2 && m_api_key.empty()) { m_status->SetLabel(_L("Save an Anthropic API key before turning on AI tags.")); return; }
            MessageDialog confirm(this, action == 1 ? _L("Fetch missing details and photos from your models' Printables and Thingiverse pages?") : _L("Send model names, file names, source links, descriptions and small previews to Anthropic for suggested tags? Your Anthropic account is charged for these requests."), _L("Enable online library feature"), wxYES_NO | wxCANCEL);
            if (confirm.ShowModal() != wxID_YES) return;
            m_online_enabled = true; wxGetApp().app_config->set("model_library_online","1"); wxGetApp().app_config->save();
        }
        operation(_L("Updating online preferences…"), [key, enabled](auto& lib) { lib.set_setting(key,enabled); });
        m_online_after = std::chrono::steady_clock::now();
    }
}

void ModelLibraryPanel::edit_tag_rules()
{
    Json rules = m_settings.value("tag_rules", ModelLibrary::Library::default_tag_rules());
    if (!rules.is_array()) rules = ModelLibrary::Library::default_tag_rules();
    LibraryDialog dialog(this, wxID_ANY, _L("Tag rules"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new Label(&dialog, _L("Automatically tag models whose names or files match a rule. Removed tags stay removed.")), 0, wxALL, FromDIP(16));
    auto* list = new wxDataViewListCtrl(&dialog, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(700, 380)), wxDV_ROW_LINES);
    list->AppendToggleColumn(_L("Enabled"), wxDATAVIEW_CELL_ACTIVATABLE, FromDIP(70));
    list->AppendTextColumn(_L("Tag"), wxDATAVIEW_CELL_EDITABLE, FromDIP(160));
    list->AppendTextColumn(_L("Match names using a regular expression"), wxDATAVIEW_CELL_EDITABLE, FromDIP(440));
    for (const auto& rule : rules) {
        wxVector<wxVariant> values;
        values.push_back(rule.value("enabled", true)); values.push_back(from_u8(text(rule, "tag"))); values.push_back(from_u8(text(rule, "pattern")));
        list->AppendItem(values);
    }
    sizer->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new Button(&dialog, _L("Add rule")); add->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    auto* remove = new Button(&dialog, _L("Remove rule")); remove->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    row->Add(add, 0, wxRIGHT, FromDIP(8)); row->Add(remove);
    add->Bind(wxEVT_BUTTON, [list](wxCommandEvent&) { wxVector<wxVariant> values; values.push_back(true); values.push_back(wxString()); values.push_back(wxString()); list->AppendItem(values); });
    remove->Bind(wxEVT_BUTTON, [list](wxCommandEvent&) { const int index = list->ItemToRow(list->GetSelection()); if (index >= 0) list->DeleteItem(unsigned(index)); });
    sizer->Add(row, 0, wxALL, FromDIP(16));
    auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog, list, &rules](wxCommandEvent&) {
        Json updated = Json::array();
        for (unsigned i = 0; i < list->GetItemCount(); ++i) {
            wxVariant enabled, tag, pattern;
            list->GetValue(enabled, i, 0); list->GetValue(tag, i, 1); list->GetValue(pattern, i, 2);
            const auto expression = into_u8(pattern.GetString());
            if (tag.GetString().empty() || expression.empty()) continue;
            try { std::regex check(expression, std::regex::icase); }
            catch (const std::regex_error&) { MessageDialog message(&dialog, _L("A matching rule is not a valid regular expression. Correct it before saving."), _L("Check tag rule")); message.ShowModal(); return; }
            updated.push_back({{"tag", into_u8(tag.GetString())}, {"pattern", expression}, {"enabled", enabled.GetBool()}});
        }
        rules = std::move(updated); dialog.EndModal(wxID_OK);
    });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
    if (dialog.ShowModal() == wxID_OK) operation(_L("Updating tag rules…"), [rules](auto& library) { library.set_setting("tag_rules", rules); library.organize(); });
}

void ModelLibraryPanel::edit_watch_folders()
{
    LibraryDialog dialog(this, wxID_ANY, _L("Watched folders"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new Label(&dialog, _L("New downloads are imported after they finish saving. Scan to include older files.")), 0, wxALL, FromDIP(16));
    auto* list = new wxDataViewListCtrl(&dialog, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(700, 300)), wxDV_ROW_LINES);
    list->AppendToggleColumn(_L("Watch"), wxDATAVIEW_CELL_ACTIVATABLE, FromDIP(65));
    list->AppendToggleColumn(_L("Include folders"), wxDATAVIEW_CELL_ACTIVATABLE, FromDIP(130));
    list->AppendTextColumn(_L("Folder"), wxDATAVIEW_CELL_INERT, FromDIP(480));
    const auto folders = m_folders;
    std::vector<Id> removed;
    std::vector<Id> row_ids;
    for (const auto& folder : folders) {
        wxVector<wxVariant> values; values.push_back(folder.value("enabled", 0) != 0); values.push_back(folder.value("recursive", 0) != 0); values.push_back(from_u8(text(folder, "path")));
        list->AppendItem(values); row_ids.push_back(folder["id"]);
    }
    sizer->Add(list, 1, wxEXPAND | wxALL, FromDIP(16));
    auto* remove = new Button(&dialog, _L("Stop watching selected folder")); remove->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    remove->Bind(wxEVT_BUTTON, [list, &removed, &row_ids](wxCommandEvent&) {
        const int row = list->ItemToRow(list->GetSelection());
        if (row >= 0 && size_t(row) < row_ids.size()) { removed.push_back(row_ids[size_t(row)]); row_ids.erase(row_ids.begin() + row); list->DeleteItem(unsigned(row)); }
    });
    sizer->Add(remove, 0, wxLEFT | wxRIGHT, FromDIP(16));
    auto* buttons = new DialogButtons(&dialog, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
    if (dialog.ShowModal() != wxID_OK) return;
    Json updates = Json::array();
    for (unsigned i = 0; i < row_ids.size(); ++i) {
        wxVariant enabled, recursive; list->GetValue(enabled, i, 0); list->GetValue(recursive, i, 1);
        updates.push_back({{"id", row_ids[i]}, {"enabled", enabled.GetBool()}, {"recursive", recursive.GetBool()}});
    }
    operation(_L("Updating watched folders…"), [updates, removed](auto& library) {
        for (Id id : removed) library.remove_watch(id);
        for (const auto& update : updates) library.update_watch(update["id"], update["enabled"], update["recursive"]);
    });
}

void ModelLibraryPanel::edit_print_history()
{
    if (m_detail.is_null()) return;
    wxMenu menu;
    const auto entries = m_detail["prints"];
    for (size_t i = 0; i < entries.size(); ++i) menu.Append(int(i + 1), from_u8(text(entries[i], "at")) + " — " + outcome_label(text(entries[i], "outcome")));
    if (entries.empty()) { read_only_text(this, _L("Print history"), into_u8(_L("No prints recorded yet."))); return; }
    const int selected = GetPopupMenuSelectionFromUser(menu);
    if (selected <= 0 || size_t(selected) > entries.size()) return;
    const auto entry = entries[size_t(selected - 1)];
    wxMenu actions;
    actions.Append(1, _L("Correct result")); actions.Append(2, _L("Edit notes")); actions.Append(3, _L("Delete print record"));
    const int action = GetPopupMenuSelectionFromUser(actions);
    if (action == 3) {
        MessageDialog confirm(this, _L("Delete this print record? The model and its files will be kept."), _L("Delete print record"), wxYES_NO | wxCANCEL);
        if (confirm.ShowModal() == wxID_YES) operation(_L("Deleting print record…"), [id = entry["id"].get<Id>()](auto& lib) { lib.delete_print(id); });
    } else if (action == 1 || action == 2) {
        const auto outcome = action == 1 ? choose_outcome(*this) : std::optional<std::string>(text(entry, "outcome"));
        if (!outcome) return;
        const auto values = edit_fields(this, _L("Print notes"), {{_L("Notes"), text(entry, "notes")}}, true);
        if (values) operation(_L("Updating print record…"), [id = entry["id"].get<Id>(), outcome = *outcome, notes = (*values)[0]](auto& lib) { lib.update_print(id, outcome, notes); });
    }
}

void ModelLibraryPanel::file_actions()
{
    if (!m_library || m_detail.is_null()) return;
    const int row = m_files->ItemToRow(m_files->GetSelection());
    if (row < 0 || size_t(row) >= m_detail["files"].size()) return;
    const auto file = m_detail["files"][size_t(row)];
    wxMenu menu;
    menu.Append(1, _L("Add to plate")); menu.Append(2, _L("Preview")); menu.Append(3, _L("Open in default application")); menu.Append(4, _L("Use as cover")); menu.Append(5, _L("File details")); menu.Append(6, _L("Open in another slicer"));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action == 1) add_to_plate();
    else if (action == 2) preview();
    else if (action == 6) open_in_slicer(file);
    else if (action == 3) {
        try { wxLaunchDefaultApplication(from_u8(m_library->resolve(file["rel_path"]).u8string())); }
        catch (const std::exception& e) { m_status->SetLabel(from_u8(e.what())); }
    } else if (action == 4) operation(_L("Updating cover…"), [model = m_selected, id = file["id"].get<Id>()](auto& lib) { lib.set_cover(model, id); });
    else if (action == 5) {
        std::string details = text(file, "original_name") + "\n\n";
        for (const auto& meta : file["meta"]) details += text(meta, "key") + ": " + text(meta, "value") + '\n';
        if (file["size_x"].is_number()) details += "\n" + into_u8(_L("Size")) + ": " + file["size_x"].dump() + " × " + file["size_y"].dump() + " × " + file["size_z"].dump() + " mm\n";
        if (file["triangles"].is_number()) details += into_u8(_L("Triangles")) + ": " + file["triangles"].dump() + '\n';
        if (file["volume"].is_number()) details += into_u8(_L("Volume")) + ": " + file["volume"].dump() + " mm³\n";
        if (!text(file, "error").empty()) details += "\n" + text(file, "error");
        read_only_text(this, _L("File details"), details);
    }
}
void ModelLibraryPanel::open_in_slicer(const Json& file)
{
    if (!m_library) return;
    const auto library = m_library; const auto alive = m_alive; const auto home = library_fs::u8path(into_u8(wxGetHomeDir()));
    auto apps = std::make_shared<std::vector<std::pair<std::string,library_fs::path>>>();
    queue_job(*m_worker,[apps,home](Job::Ctl&) { *apps = ModelLibrary::installed_slicers(home); },[this,alive,library,file,apps](bool cancelled,std::exception_ptr& error) {
        if (error) { error = nullptr; return; }
        if (!*alive || cancelled || library != m_library) return;
        wxMenu menu;
        for (size_t i = 0; i < apps->size(); ++i) menu.Append(int(i + 1),from_u8((*apps)[i].first));
        menu.Append(1000,_L("Choose another application"));
        const int choice = GetPopupMenuSelectionFromUser(menu); library_fs::path app;
        if (choice == 1000) {
            wxFileDialog dialog(this,_L("Choose a slicer application"),"","",_L("All files") + "|*.*",wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK) return; app = library_fs::u8path(into_u8(dialog.GetPath()));
        } else if (choice > 0 && size_t(choice) <= apps->size()) app = (*apps)[size_t(choice - 1)].second;
        else return;
        try {
            std::vector<wxString> values;
#ifdef __APPLE__
            values = {"/usr/bin/open","-a",from_u8(app.u8string()),from_u8(library->resolve(file["rel_path"]).u8string())};
#else
            values = {from_u8(app.u8string()),from_u8(library->resolve(file["rel_path"]).u8string())};
#endif
            std::vector<std::wstring> storage; for (const auto& value : values) storage.push_back(value.ToStdWstring());
            std::vector<const wchar_t*> args; for (const auto& value : storage) args.push_back(value.c_str()); args.push_back(nullptr);
            if (wxExecute(args.data(),wxEXEC_ASYNC) <= 0) m_status->SetLabel(_L("The application could not be opened."));
        } catch (const std::exception& failure) { m_status->SetLabel(from_u8(failure.what())); }
    });
}

void ModelLibraryPanel::show_view_menu()
{
    wxMenu menu;
    menu.AppendRadioItem(1, _L("Grid"))->Check(m_grid_view); menu.AppendRadioItem(2, _L("List"))->Check(!m_grid_view);
    menu.AppendSeparator(); menu.Append(3, _L("Group by source")); menu.Append(4, _L("Group by designer")); menu.Append(5, _L("Group by name family")); menu.Append(6, _L("Group by month")); menu.Append(7, _L("No grouping"));
    menu.AppendSeparator(); menu.Append(8, _L("Newest first")); menu.Append(9, _L("Name A–Z")); menu.Append(10, _L("Largest first"));
    menu.Append(11, m_inspector->IsShown() ? _L("Hide inspector") : _L("Show inspector"));
    const int action = GetPopupMenuSelectionFromUser(menu);
    if (action == 1 || action == 2) {
        const auto ids = selection(); m_refreshing = true;
        m_grid_view = action == 1; m_grid->Show(m_grid_view); m_list->Show(!m_grid_view);
        m_list->UnselectAll();
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const bool selected = std::find(ids.begin(),ids.end(),m_rows[i]["id"].get<Id>()) != ids.end();
            m_grid->SetItemState(long(i),selected ? wxLIST_STATE_SELECTED : 0,wxLIST_STATE_SELECTED);
            if (selected) m_list->SelectRow(unsigned(i));
        }
        m_refreshing = false; Layout();
    }
    else if (action >= 3 && action <= 7) { m_group = action == 3 ? "source_site" : action == 4 ? "designer" : action == 5 ? "family" : action == 6 ? "month" : ""; refresh_library(); }
    else if (action >= 8 && action <= 10) { m_filter["sort"] = action == 8 ? "added" : action == 9 ? "name" : "size"; m_filter["descending"] = action != 9; refresh_library(); }
    else if (action == 11) { m_inspector->Show(!m_inspector->IsShown()); Layout(); }
}

void ModelLibraryPanel::browse_model()
{
    if (m_detail.is_null()) return;
    if (m_detail["files"].size() == 1) { add_to_plate(); return; }
    const Json detail = m_detail;
    LibraryDialog dialog(this, wxID_ANY, from_u8(text(detail, "name")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* search = new ::TextInput(&dialog, ""); search->GetTextCtrl()->SetHint(_L("Find a file in this model")); sizer->Add(search, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* tree = new wxTreeCtrl(&dialog, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(700, 460)), wxTR_HAS_BUTTONS | wxTR_MULTIPLE);
    sizer->Add(tree, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    std::map<wxTreeItemId, size_t> files;
    const auto rebuild = [&] {
        tree->DeleteAllItems(); files.clear();
        const auto root = tree->AddRoot(from_u8(text(detail, "name")));
        std::map<std::string, wxTreeItemId> folders{{"", root}};
        const auto query = search->GetTextCtrl()->GetValue().Lower();
        const auto prefix = text(detail, "dir") + '/';
        for (size_t index = 0; index < detail["files"].size(); ++index) {
            auto path = text(detail["files"][index], "rel_path");
            if (path.rfind(prefix, 0) == 0) path.erase(0, prefix.size());
            if (!query.empty() && from_u8(path).Lower().Find(query) == wxNOT_FOUND) continue;
            auto parent = root; std::string partial;
            const auto file = library_fs::u8path(path);
            for (const auto& component : file.parent_path()) {
                if (!partial.empty()) partial += '/'; partial += component.u8string();
                auto found = folders.find(partial);
                if (found == folders.end()) found = folders.emplace(partial, tree->AppendItem(parent, from_u8(component.u8string()))).first;
                parent = found->second;
            }
            files.emplace(tree->AppendItem(parent, from_u8(file.filename().u8string())), index);
        }
        tree->ExpandAll();
    };
    search->GetTextCtrl()->Bind(wxEVT_TEXT, [&](wxCommandEvent&) { rebuild(); });
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    for (const auto& item : std::vector<std::pair<int, wxString>>{{1, _L("Add to plate")}, {2, _L("Preview")}, {3, _L("Open in default app")}}) {
        auto* button = new Button(&dialog, item.second); button->SetStyle(ButtonStyle::Regular, ButtonType::Window); actions->Add(button, 0, wxRIGHT, FromDIP(8));
        button->Bind(wxEVT_BUTTON, [&dialog, id = item.first](wxCommandEvent&) { dialog.EndModal(id); });
    }
    tree->Bind(wxEVT_TREE_ITEM_ACTIVATED, [&](wxTreeEvent& event) { if (files.count(event.GetItem())) dialog.EndModal(2); else event.Skip(); });
    sizer->Add(actions, 0, wxALL, FromDIP(12)); rebuild();
    dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent();
    const int action = dialog.ShowModal();
    if (action < 1 || action > 3) return;
    wxArrayTreeItemIds selected; tree->GetSelections(selected);
    m_files->UnselectAll();
    for (const auto& item : selected) { const auto found = files.find(item); if (found != files.end()) m_files->SelectRow(unsigned(found->second)); }
    if (m_files->GetSelectedItemsCount() == 0) return;
    if (action == 1) add_to_plate();
    else if (action == 2) preview();
    else for (const auto& item : selected) {
        const auto found = files.find(item); if (found == files.end()) continue;
        try { wxLaunchDefaultApplication(from_u8(m_library->resolve(detail["files"][found->second]["rel_path"]).u8string())); }
        catch (const std::exception& error) { m_status->SetLabel(from_u8(error.what())); }
    }
}

void ModelLibraryPanel::preview()
{
    if (!m_library || m_detail.is_null()) return;
    wxDataViewItemArray selected; m_files->GetSelections(selected);
    Json file;
    if (!selected.empty()) {
        const int row = m_files->ItemToRow(selected.front());
        if (row >= 0 && size_t(row) < m_detail["files"].size()) file = m_detail["files"][size_t(row)];
    } else for (const auto& candidate : m_detail["files"]) if (candidate["kind"] == "model") { file = candidate; break; }
    if (file.is_null()) return;
    library_fs::path path;
    try { path = m_library->resolve(file["rel_path"]); } catch (const std::exception& error) { m_status->SetLabel(from_u8(error.what())); return; }
    if (file["kind"] != "model" || file["format"] == "step") { wxLaunchDefaultApplication(from_u8(path.u8string())); return; }
    if (file["format"] == "gcode" || file["format"] == "bgcode") { wxGetApp().plater()->load_gcode(from_u8(path.u8string())); wxGetApp().mainframe->select_tab(TAB_ID_PREVIEW); return; }
    auto mesh = std::make_shared<TriangleMesh>(); const auto alive = m_alive;
    m_status->SetLabel(_L("Preparing preview…"));
    queue_job(*m_worker, [path, mesh](Job::Ctl& ctl) { if (!ctl.was_canceled()) *mesh = ModelLibrary::read_mesh(path); },
              [this, alive, mesh](bool cancelled, std::exception_ptr& error) {
        if (error) { try { std::rethrow_exception(error); } catch (const std::exception& e) { if (*alive) m_status->SetLabel(from_u8(e.what())); } error = nullptr; return; }
        if (!*alive || cancelled) return;
        const auto count = static_cast<unsigned>(m_rows.size());
        m_status->SetLabel(wxString::Format(_L_PLURAL("%u model", "%u models", count), count));
        LibraryDialog dialog(this, wxID_ANY, _L("Model preview"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new MeshPreview(&dialog, std::move(mesh->its)), 1, wxEXPAND);
        sizer->Add(new Label(&dialog, _L("Drag to rotate. Scroll to zoom. Hold Shift and drag to pan.")), 0, wxALL, FromDIP(12));
        dialog.SetSizerAndFit(sizer); wxGetApp().UpdateDlgDarkUI(&dialog); dialog.CenterOnParent(); dialog.ShowModal();
    });
}
} // namespace Slic3r::GUI
