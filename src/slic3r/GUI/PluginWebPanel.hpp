#ifndef slic3r_GUI_PluginWebPanel_hpp_
#define slic3r_GUI_PluginWebPanel_hpp_

#include <functional>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/panel.h>
#include <wx/webview.h>

class wxAuiManagerEvent;

namespace Slic3r { namespace GUI {

// Plugin HTML hosted in a pane docked into the plater workspace, rather than in a
// floating window (PluginWebDialog). Same page contract — the injected host theme
// variables and the window.orca bridge come from the shared plugin_web/webview_host
// helpers, so a plugin's HTML behaves identically in either host.
//
// Like PluginWebDialog this is deliberately Python-agnostic: it reaches the plugin
// layer only through std::function hooks, which must NOT capture bare pybind11
// objects (the panel can be destroyed on the main thread without the GIL held).
//
// The panel finds its wxAuiManager through the window hierarchy rather than holding
// a Plater reference, so docking, floating, resizing, and teardown all work without
// the plater knowing plugins exist.
class PluginWebPanel : public wxPanel
{
public:
    using MessageHandler = std::function<void(const nlohmann::json& data)>;
    using CloseHandler   = std::function<void()>;

    // Creates the panel as a child of the AUI-managed `host` (the Plater) and docks
    // it there. `side` is "left"/"right"/"top"/"bottom"; `width` is the initial size
    // along the docking axis, in pixels. Returns nullptr when `host` is not managed
    // by a wxAuiManager, which is the case before the plater exists.
    //
    // on_close fires on a user- or page-initiated close (the pane's X, or
    // window.orca.close()); on_destroyed runs from the destructor on every path and
    // must touch host-side state only (no Python).
    static PluginWebPanel* create_docked(wxWindow*          host,
                                         const wxString&    caption,
                                         const std::string& html,
                                         const std::string& side,
                                         int                width,
                                         MessageHandler     on_message,
                                         CloseHandler       on_close,
                                         CloseHandler       on_destroyed);

    static void post_message(PluginWebPanel* panel, const nlohmann::json& data);
    static void request_close(PluginWebPanel* panel);
    static void destroy_for_plugin(PluginWebPanel* panel);

    // Push a payload to the page; delivered to handlers registered via
    // window.orca.onMessage(). MAIN-THREAD ONLY (the plugin layer marshals).
    void push_message(const nlohmann::json& data);

    bool is_open() const { return m_open; }

private:
    PluginWebPanel(wxWindow*          parent,
                   const std::string& html,
                   MessageHandler     on_message,
                   CloseHandler       on_close,
                   CloseHandler       on_destroyed);
    ~PluginWebPanel() override;

    bool create_webview();
    void on_bootstrap_event(wxWebViewEvent& event);
    void on_script_message_event(wxWebViewEvent& event);
    void on_webview_recreated(wxCommandEvent& event);
    void on_pane_close(wxAuiManagerEvent& event);
    void load_plugin_content();
    void close_and_destroy();
    void fire_close();
    void detach_from_manager();

    std::string    m_html;
    wxWebView*     m_browser{nullptr};
    wxWindow*      m_host{nullptr};
    bool           m_content_loaded{false};
    bool           m_open{true};
    bool           m_close_fired{false};
    MessageHandler m_on_message;
    CloseHandler   m_on_close;
    CloseHandler   m_on_destroyed;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginWebPanel_hpp_
