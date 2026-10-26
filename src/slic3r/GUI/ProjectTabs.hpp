#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "libslic3r/ProjectSliceQueue.hpp"
#include <wx/string.h>

class wxWindow;

namespace Slic3r::GUI {
class MainFrame;
class Plater;

// Owns document lifetimes. Shared mode pages stay on MainFrame; each document
// retains its workspace and settings controls so switching never reloads a file.
class ProjectTabs
{
public:
    explicit ProjectTabs(MainFrame& frame);
    ~ProjectTabs();
    wxWindow* window() const;
    bool creating() const { return m_creating; }
    bool changing() const { return m_creating || m_switching; }
    bool new_project();
    bool open_project(const wxString& filename, const wxString& origin = "-");
    bool open_gcode(const wxString& filename);
    bool activate(std::size_t index);
    bool activate(Plater* owner);
    bool close(std::size_t index);
    bool close_active() { return close(m_active); }
    bool confirm_quit();
    bool save_all();
    void next(int direction);
    void refresh();
    void shutdown();
    bool path_available(const wxString& path, const Plater* owner) const;
    bool confirm_save(const wxString& path, const Plater* owner) const;
    void saved(Plater* owner);
    std::vector<Plater*> workspaces() const;
    void queue_slice(Plater* owner);
    void cancel_slice(Plater* owner);
    bool can_start_slice(const Plater* owner) const;
    bool restore_session();
    bool checkpoint(Plater* owner, bool force = false);
    bool prepare_recreation();

private:
    struct Workspace;
    struct UI;
    MainFrame& m_frame;
    std::unique_ptr<UI> m_ui;
    std::vector<std::unique_ptr<Workspace>> m_workspaces;
    std::size_t m_active {0};
    unsigned int m_untitled {1};
    bool m_creating {false};
    bool m_switching {false};
    bool m_shutdown {false};
    bool m_recreating {false};
    Slic3r::ProjectSliceQueue m_slice_queue;
    bool m_slice_started {false};
    void pump_slice_queue();
    void write_session(bool clean = false);
    void project_menu(Plater* owner);
    void project_list();
    void layout_tabs();
    void move(Plater* owner, int direction);
    void dispose(Workspace& workspace);
    void capture(Workspace& workspace);
    void bind(Workspace& workspace);
    void attach_pages();
    bool confirm_close(Workspace& workspace);
    Workspace* create();
};
}
