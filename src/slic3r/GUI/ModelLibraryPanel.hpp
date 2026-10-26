#pragma once

#include "Lazy.hpp"
#include "slic3r/Utils/ModelLibrary/Library.hpp"
#include "slic3r/Utils/ModelLibrary/Watch.hpp"

#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/bmpbndl.h>
#include <wx/vector.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxDataViewListCtrl;
class wxListCtrl;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class TextInput;

namespace Slic3r::GUI {
class BoostThreadWorker;
class Job;

class ModelLibraryPanel final : public wxPanel, public LazyInstance<ModelLibraryPanel> {
public:
    explicit ModelLibraryPanel(wxWindow* parent);
    ~ModelLibraryPanel() override;
    bool Show(bool show = true) override;
    void msw_rescale();
    void sys_color_changed();

private:
    using Json = ModelLibrary::Json;
    using Id = ModelLibrary::Id;
    std::shared_ptr<ModelLibrary::Library> m_library;
    std::unique_ptr<BoostThreadWorker> m_worker;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    wxTimer m_events;
    wxTimer m_search_timer;
    std::shared_ptr<ModelLibrary::Watch> m_watch = std::make_shared<ModelLibrary::Watch>();
    unsigned m_watch_ticks = 0;
    bool m_online_enabled = false;
    std::string m_api_key;
    std::chrono::steady_clock::time_point m_online_after{};
    wxTreeCtrl* m_navigation = nullptr;
    wxDataViewListCtrl* m_list = nullptr;
    wxListCtrl* m_grid = nullptr;
    wxVector<wxBitmapBundle> m_grid_images;
    bool m_grid_view = true;
    std::string m_group;
    wxDataViewListCtrl* m_files = nullptr;
    wxScrolledWindow* m_inspector = nullptr;
    wxStaticBitmap* m_cover = nullptr;
    wxStaticText* m_title = nullptr;
    wxStaticText* m_details = nullptr;
    wxStaticText* m_status = nullptr;
    ::TextInput* m_search = nullptr;
    Json m_filter = Json::object();
    Json m_rows = Json::array();
    Json m_detail;
    Json m_settings = Json::object();
    Json m_folders = Json::array();
    Json m_collections = Json::array();
    Id m_selected = 0;
    unsigned m_generation = 0;
    bool m_refreshing = false;

    void open_library(const std::string& path);
    void choose_library();
    void refresh_library();
    void show_detail(Id id, bool browse = false);
    void browse_model();
    void show_view_menu();
    void update_inspector();
    void rebuild_navigation(const Json& facets, const Json& collections);
    std::vector<Id> selection() const;
    void operation(const wxString& label, std::function<void(ModelLibrary::Library&)> work);
    void import_files(bool folder, bool organized = false);
    void import_paths(std::vector<ModelLibrary::fs::path> paths);
    void add_to_plate(bool project = false);
    void edit_details();
    void show_actions(const std::string& command = {});
    void show_settings();
    void show_activity();
    void show_attention();
    void edit_tag_rules();
    void online_settings();
    void edit_watch_folders();
    void edit_print_history();
    void file_actions();
    void open_in_slicer(const Json& file);
    void preview();
};
}
