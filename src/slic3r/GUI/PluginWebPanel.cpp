#include "PluginWebPanel.hpp"

#include "Widgets/WebView.hpp"
#include "PluginWebDialog.hpp"
#include "Widgets/WebViewHostDialog.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/aui/aui.h>
#include <wx/sizer.h>

#include <utility>

namespace Slic3r { namespace GUI {

namespace {

// The same tiny bundled bootstrap page the dialog uses: it brings the webview up so
// the real plugin HTML can be swapped in with SetPage once loading has settled.
wxString bootstrap_url()
{
    const std::string path = (boost::filesystem::path(resources_dir()) / "web/dialog/PluginWebDialog/blank.html")
                                 .make_preferred()
                                 .string();
    return wxString("file://") + from_u8(path);
}

// Pane names must be unique within a wxAuiManager and are what SavePerspective
// records, so they are generated rather than taken from the (user-visible) caption.
std::string next_pane_name()
{
    static int counter = 0;
    return "orca-plugin-pane-" + std::to_string(++counter);
}

wxAuiPaneInfo pane_info(const std::string& name, const wxString& caption, const std::string& side, int width)
{
    wxAuiPaneInfo info;
    info.Name(wxString::FromUTF8(name)).Caption(caption).CloseButton(true).MaximizeButton(false).PaneBorder(false);

    // Sized along the docking axis; the cross-axis figure is ignored for a docked
    // pane and only matters if the user floats it.
    const int em = wxGetApp().em_unit();
    if (side == "bottom" || side == "top") {
        (side == "top") ? info.Top() : info.Bottom();
        info.BestSize(wxSize(60 * em, width));
    } else {
        (side == "left") ? info.Left() : info.Right();
        info.BestSize(wxSize(width, 60 * em));
    }
    return info;
}

} // namespace

PluginWebPanel* PluginWebPanel::create_docked(wxWindow*          host,
                                              const wxString&    caption,
                                              const std::string& html,
                                              const std::string& side,
                                              int                width,
                                              MessageHandler     on_message,
                                              CloseHandler       on_close,
                                              CloseHandler       on_destroyed)
{
    if (host == nullptr)
        return nullptr;
    wxAuiManager* mgr = wxAuiManager::GetManager(host);
    if (mgr == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "PluginWebPanel: no wxAuiManager for the host window";
        return nullptr;
    }

    auto* panel = new PluginWebPanel(host, html, std::move(on_message), std::move(on_close),
                                     std::move(on_destroyed));
    if (!panel->create_webview()) {
        // No on_destroyed yet from the caller's point of view: the panel never docked.
        panel->m_on_destroyed = nullptr;
        panel->Destroy();
        return nullptr;
    }

    mgr->AddPane(panel, pane_info(next_pane_name(), caption, side, width));
    mgr->Update();
    return panel;
}

PluginWebPanel::PluginWebPanel(wxWindow*          parent,
                               const std::string& html,
                               MessageHandler     on_message,
                               CloseHandler       on_close,
                               CloseHandler       on_destroyed)
    : wxPanel(parent, wxID_ANY)
    , m_html(html)
    , m_host(parent)
    , m_on_message(std::move(on_message))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    // Paint in the themed background so there is no white flash before the
    // (transparent) bootstrap page and the plugin HTML render.
    SetBackgroundColour(wxGetApp().get_window_default_clr());

    // The pane's own close button only hides the pane by default, which would leave the
    // plugin believing its panel is still open. Handled on the managed window, where
    // wxAuiManager delivers manager events, and filtered to this pane.
    m_host->Bind(wxEVT_AUI_PANE_CLOSE, &PluginWebPanel::on_pane_close, this);
}

bool PluginWebPanel::create_webview()
{
    const wxString url = bootstrap_url();

    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "PluginWebPanel: could not create the webview";
        return false;
    }
    m_browser->SetBackgroundColour(wxGetApp().get_window_default_clr());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Document-start scripts, in the same order the dialog registers them: host theme
    // contract first (defines the --orca-* variables), then the defaults built on
    // them, then the bridge.
    m_browser->AddUserScript(wxString::FromUTF8(WebViewHostDialog::theme_user_script()));
    m_browser->AddUserScript(wxString::FromUTF8(WebViewHostDialog::plugin_defaults_user_script()));
    m_browser->AddUserScript(PluginWebDialog::bridge_js());

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PluginWebPanel::on_script_message_event, this, m_browser->GetId());
    // Bind ERROR too, so a missing or blocked bootstrap resource still triggers the swap.
    Bind(wxEVT_WEBVIEW_LOADED, &PluginWebPanel::on_bootstrap_event, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &PluginWebPanel::on_bootstrap_event, this, m_browser->GetId());
    m_browser->Bind(EVT_WEBVIEW_RECREATED, &PluginWebPanel::on_webview_recreated, this);

    WebView::LoadUrl(m_browser, url);
    return true;
}

PluginWebPanel::~PluginWebPanel()
{
    // Every destruction path passes here, including the plater tearing down its
    // children at shutdown, so the manager must not be left holding this pane.
    if (m_host != nullptr)
        m_host->Unbind(wxEVT_AUI_PANE_CLOSE, &PluginWebPanel::on_pane_close, this);
    detach_from_manager();

    // Host-side state only (no Python): the members above are still alive here, and
    // the callback merely drops this panel from the plugin UI registry.
    if (m_on_destroyed)
        m_on_destroyed();
}

void PluginWebPanel::post_message(PluginWebPanel* panel, const nlohmann::json& data)
{
    if (panel != nullptr && panel->is_open())
        panel->push_message(data);
}

void PluginWebPanel::request_close(PluginWebPanel* panel)
{
    if (panel != nullptr)
        panel->close_and_destroy();
}

void PluginWebPanel::destroy_for_plugin(PluginWebPanel* panel)
{
    if (panel == nullptr)
        return;
    // Forced teardown (plugin unload / app shutdown) must not call back into Python.
    panel->m_open      = false;
    panel->m_on_close  = nullptr;
    panel->detach_from_manager();
    panel->Destroy();
}

void PluginWebPanel::push_message(const nlohmann::json& data)
{
    if (!m_open || m_browser == nullptr)
        return;
    nlohmann::json envelope;
    envelope["data"] = data;
    const wxString payload = wxString::FromUTF8(
        envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
    const wxString script = wxT("__orcaDispatch(") + payload + wxT(")");
    wxGetApp().CallAfter([this, script] {
        if (m_open && m_browser != nullptr)
            WebView::RunScript(m_browser, script);
    });
}

void PluginWebPanel::on_bootstrap_event(wxWebViewEvent& event)
{
    // The first bootstrap load (or its error) triggers the swap to plugin HTML; the
    // resulting plugin-page load is ignored (guarded by m_content_loaded).
    load_plugin_content();
    event.Skip();
}

void PluginWebPanel::load_plugin_content()
{
    if (m_content_loaded || m_browser == nullptr)
        return;
    m_content_loaded = true;
    m_browser->SetPage(wxString::FromUTF8(m_html), PluginWebDialog::base_url());
}

void PluginWebPanel::on_script_message_event(wxWebViewEvent& event)
{
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(event.GetString().utf8_string());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << "PluginWebPanel: unparseable script message: " << e.what();
        return;
    }

    if (payload.value("channel", std::string()) != "orca")
        return;

    const std::string    kind = payload.value("kind", std::string());
    const nlohmann::json data = payload.contains("data") ? payload["data"] : nlohmann::json();
    if (kind == "close") {
        close_and_destroy();
        return;
    }
    // A docked pane has no modal result, so submit() is delivered as an ordinary
    // message rather than ending anything — pages written for either host still work.
    if ((kind == "message" || kind == "submit") && m_on_message)
        m_on_message(data);
}

void PluginWebPanel::on_webview_recreated(wxCommandEvent&)
{
    // Handled: do NOT Skip(), so WebView::RecreateAll skips the redundant reload.
    if (m_browser != nullptr)
        WebView::RunScript(m_browser, wxString::FromUTF8(WebViewHostDialog::theme_apply_js()));
}

void PluginWebPanel::on_pane_close(wxAuiManagerEvent& event)
{
    if (event.GetPane() == nullptr || event.GetPane()->window != this) {
        event.Skip();
        return;
    }
    // Veto the manager's default (hide the pane) and tear the panel down instead.
    // Deferred: the manager is still walking its own event when this returns.
    event.Veto();
    wxGetApp().CallAfter([this]() { close_and_destroy(); });
}

void PluginWebPanel::close_and_destroy()
{
    if (!m_open)
        return;
    m_open = false;
    fire_close();
    detach_from_manager();
    Destroy();
}

void PluginWebPanel::fire_close()
{
    if (m_close_fired)
        return;
    m_close_fired = true;
    if (m_on_close) {
        CloseHandler cb = m_on_close;
        m_on_close      = nullptr;
        cb();
    }
}

void PluginWebPanel::detach_from_manager()
{
    // Null once the plater has torn its manager down, which is the ordinary path at
    // application shutdown — the pane is already gone with it.
    wxAuiManager* mgr = m_host != nullptr ? wxAuiManager::GetManager(m_host) : nullptr;
    if (mgr == nullptr || !mgr->GetPane(this).IsOk())
        return;
    mgr->DetachPane(this);
    mgr->Update();
}

}} // namespace Slic3r::GUI
