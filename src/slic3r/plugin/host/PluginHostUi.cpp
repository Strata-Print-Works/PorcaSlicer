#include "PluginHostUi.hpp"

#include <libslic3r/Preset.hpp>
#include <pybind11/pytypes.h>
#include <pybind11/gil.h>
#include <pybind11/cast.h>
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp" // PythonGILState
#include "slic3r/plugin/PluginFsUtils.hpp"   // json_to_py / py_to_json
#include "slic3r/plugin/PluginUiThread.hpp"  // run_on_ui_blocking

#include <libslic3r/PresetBundle.hpp>

#include <slic3r/GUI/BackgroundSlicingProcess.hpp>
#include <functional>
#include <exception>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/MarkdownTip.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/DockPanel.hpp>
#include <slic3r/GUI/PluginProgressDialog.hpp>
#include <slic3r/GUI/WebDialog.hpp>
#include <slic3r/GUI/NotificationManager.hpp>

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>

#include <boost/log/trivial.hpp>

#include <utility>
#include <string>
#include <wx/app.h>
#include <wx/defs.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/window.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace py = pybind11;
using json   = nlohmann::json;

namespace Slic3r {
namespace {

// --------------------------------------------------------------------------
// GIL-safe holder for a Python callable. A std::function that captured a bare
// py::object could be destroyed on the main thread without the GIL (a dialog
// teardown), which would Py_DECREF unsafely. Wrapping the callable here means
// the GIL is acquired exactly when the last reference is released, on any thread.
// --------------------------------------------------------------------------
struct GilSafeCallable
{
    py::object fn;
    std::atomic_bool active{true};
    explicit GilSafeCallable(py::object f) : fn(std::move(f)) {}
    void disable()
    {
        active.store(false, std::memory_order_release);
        PythonGILState gil;
        if (gil)
            fn = py::object();
        else
            (void) fn.release();
    }
    ~GilSafeCallable()
    {
        disable();
    }
};
using CallablePtr = std::shared_ptr<GilSafeCallable>;

CallablePtr make_holder(py::object obj)
{
    if (!obj || obj.is_none())
        return nullptr;
    return std::make_shared<GilSafeCallable>(std::move(obj));
}

// Adapt a Python callable to a GUI message handler that acquires the GIL and
// swallows/logs exceptions (a raising handler must not escape into wx events).
GUI::WebDialog::MessageHandler make_message_adapter(py::object on_message)
{
    CallablePtr holder = make_holder(std::move(on_message));
    if (!holder)
        return nullptr;
    return [holder](const json& data) {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn(json_to_py(data));
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_message handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

GUI::WebDialog::SubmitHandler make_submit_adapter(py::object on_submit)
{
    CallablePtr holder = make_holder(std::move(on_submit));
    if (!holder)
        return nullptr;
    return [holder](const json& data) {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn(json_to_py(data));
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_submit handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

// The plugin's on_close: fired only on a user/JS-initiated close (not forced teardown), while the
// window is alive. Empty if the plugin passed None.
std::function<void()> make_close_adapter(const CallablePtr& holder)
{
    if (!holder)
        return nullptr;
    return [holder]() {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn();
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_close handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

// --------------------------------------------------------------------------
// Registry of live plugin UI resources. Keyed by an opaque id; tracks the
// owning plugin so all of a plugin's UI can be torn down on unload.
// --------------------------------------------------------------------------
class UiRegistry
{
public:
    static UiRegistry& instance()
    {
        static UiRegistry r;
        return r;
    }

    int reserve_id()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_next_id++;
    }
    void bind(int id, wxWindow* window, const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources[id] = window;
        m_owners[id]    = plugin_key;
    }
    void remove(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources.erase(id);
        m_owners.erase(id);
    }

    template<typename T>
    T* get_as(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_resources.find(id);
        if (it == m_resources.end())
            return nullptr;
        return dynamic_cast<T*>(it->second);
    }
    bool is_open(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_resources.count(id) > 0; // presence only; no pointer deref -> thread-safe
    }
    std::vector<wxWindow*> take_for_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::vector<wxWindow*> out;
        for (auto it = m_owners.begin(); it != m_owners.end();) {
            if (it->second == plugin_key) {
                auto rit = m_resources.find(it->first);
                if (rit != m_resources.end()) {
                    out.push_back(rit->second);
                    m_resources.erase(rit);
                }
                it = m_owners.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }
    void bind_callback(const CallablePtr& callback, const std::string& plugin_key)
    {
        if (!callback)
            return;
        std::lock_guard<std::mutex> lk(m_mtx);
        m_callbacks[plugin_key].push_back(callback);
    }
    std::vector<CallablePtr> take_callbacks_for_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto                        it = m_callbacks.find(plugin_key);
        if (it == m_callbacks.end())
            return {};
        std::vector<CallablePtr> callbacks;
        callbacks.reserve(it->second.size());
        for (const std::weak_ptr<GilSafeCallable>& weak_callback : it->second) {
            if (auto callback = weak_callback.lock())
                callbacks.push_back(std::move(callback));
        }
        m_callbacks.erase(it);
        return callbacks;
    }

private:
    std::mutex                           m_mtx;
    std::unordered_map<int, wxWindow*>   m_resources;
    std::unordered_map<int, std::string> m_owners;
    std::unordered_map<std::string, std::vector<std::weak_ptr<GilSafeCallable>>> m_callbacks;
    int                                  m_next_id{1};
};

wxWindow* ui_parent()
{
    return wxTheApp == nullptr ? nullptr : dynamic_cast<wxWindow*>(GUI::wxGetApp().mainframe);
}

// --------------------------------------------------------------------------
// orca.host.ui.message
// --------------------------------------------------------------------------
long message_style(const std::string& buttons, const std::string& icon)
{
    long style = wxOK;
    if (buttons == "ok_cancel")
        style = wxOK | wxCANCEL;
    else if (buttons == "yes_no")
        style = wxYES_NO;
    else if (buttons == "yes_no_cancel")
        style = wxYES_NO | wxCANCEL;

    if (icon == "warning")
        style |= wxICON_WARNING;
    else if (icon == "error")
        style |= wxICON_ERROR;
    else if (icon == "question")
        style |= wxICON_QUESTION;
    else
        style |= wxICON_INFORMATION;
    return style;
}

std::string button_to_string(int rc)
{
    switch (rc) {
    case wxID_OK:  return "ok";
    case wxID_YES: return "yes";
    case wxID_NO:  return "no";
    default:       return "cancel";
    }
}

std::string ui_message(const std::string& text, const std::string& title,
                       const std::string& buttons, const std::string& icon)
{
    const long style = message_style(buttons, icon);
    return run_on_ui_blocking([&]() -> std::string {
        GUI::MessageDialog dlg(nullptr, wxString::FromUTF8(text), wxString::FromUTF8(title), style);
        return button_to_string(dlg.ShowModal());
    });
}

// --------------------------------------------------------------------------
constexpr long WINDOW_MODELESS  = 0L;
constexpr long WINDOW_MODAL     = 1L << 0;
constexpr long PLUGIN_WX_STYLE = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER;

// orca.host.ui.create_window + UiWindow handle
// --------------------------------------------------------------------------
struct UiWindowHandle
{
    int id{0};
};

struct UiProgressHandle
{
    int id{0};
};

py::object ui_create_window(const std::string& html, const std::string& title, int width, int height,
                            py::object on_message, py::object on_close, long style, py::object on_submit)
{
    auto              msg_adapter    = make_message_adapter(std::move(on_message));
    auto              submit_adapter = make_submit_adapter(std::move(on_submit));
    CallablePtr       close_holder   = make_holder(std::move(on_close));
    const std::string plugin_key     = PluginAuditManager::instance().current_plugin();
    const int         w              = width > 0 ? width : 820;
    const int         h              = height > 0 ? height : 600;

    if ((style & ~WINDOW_MODAL) != 0)
        throw std::invalid_argument("unsupported orca.host.ui.create_window style flags");
    const bool modal = (style & WINDOW_MODAL) != 0;

    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    // The whole wxWindow/webview creation is deferred to a clean main-loop iteration.
    // The WebKit backends deliver script messages synchronously from inside the native
    // callback (GTK: HandleWindowEvent in wxgtk_webview_webkit_script_message_received,
    // wx src/gtk/webview_webkit2.cpp; macOS: ProcessWindowEvent in the
    // WKScriptMessageHandler delegate, src/osx/webview_webkit.mm), so a script-triggered
    // create_window otherwise runs with the plugins-dialog webview's signal/delegate
    // frame still on the stack — creating and presenting a second webview window from
    // there crashed on Linux (observed at a since-removed Raise(): gtk_window_present
    // right after a Show() whose real map can still be deferred behind the X11
    // frame-extents handshake, wx src/gtk/toplevel.cpp). WebView2 already queues script
    // messages (AddPendingEvent), so on Windows this deferral merely matches wx's own
    // delivery model.
    //
    // The id is pre-bound with a null window so the returned handle is live at once:
    // is_open() keys on registry presence, post()/close() are CallAfter-marshaled and
    // FIFO-ordered after the creation below, and a plugin teardown claims the pending
    // id (take_for_plugin), which the creation detects and skips — no orphan window.
    const int new_id = UiRegistry::instance().reserve_id();
    UiRegistry::instance().bind(new_id, nullptr, plugin_key);

    GUI::wxGetApp().CallAfter([new_id, plugin_key, html, title, w, h,
                               msg_adapter = std::move(msg_adapter),
                               submit_adapter = std::move(submit_adapter),
                               close_holder = std::move(close_holder), modal]() mutable {
        // Torn down (plugin unload / app shutdown) before the window materialized.
        if (!UiRegistry::instance().is_open(new_id))
            return;

        GUI::WebDialog::CloseHandler on_close = make_close_adapter(close_holder);
        // Registry cleanup: GIL-free, runs from the dialog destructor on every path.
        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };

        auto* dlg = new GUI::WebDialog(ui_parent(), wxString::FromUTF8(title), html,
                                       wxSize(w, h), std::move(msg_adapter), std::move(submit_adapter),
                                       std::move(on_close), std::move(on_destroyed), PLUGIN_WX_STYLE);
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        if (modal) {
            dlg->ShowModal();
            // Plugin teardown may already have removed and destroyed this dialog
            // while its modal loop was running.
            if (UiRegistry::instance().is_open(new_id))
                dlg->Destroy();
        } else {
            dlg->use_normal_window_type();
            dlg->Show();
        }
    });

    return py::cast(UiWindowHandle{new_id});
}

// --------------------------------------------------------------------------
// orca.host.ui.create_dock_panel + UiDockPanel handle
// --------------------------------------------------------------------------
constexpr const char* DOCK_POSITIONS[] = {"left", "right", "top", "bottom", "float"};

struct UiDockPanelHandle
{
    int id{0};
};

// Polls until the plater is shown on screen, then runs the callback with it (null when the app
// is closing instead) and deletes itself. A plugin loaded at startup opens its panes from
// on_load, before the main window exists, and a pane added before that window is laid out is
// sized against the unsized frame and keeps that width (AuiMgr::track_docked_size).
class PlaterShownWaiter : public wxTimer
{
public:
    explicit PlaterShownWaiter(std::function<void(GUI::Plater*)> fn) : m_fn(std::move(fn)) {}

    // True once the callback ran.
    bool try_run()
    {
        const bool   closing = GUI::wxGetApp().is_closing();
        GUI::Plater* plater  = closing ? nullptr : GUI::wxGetApp().plater();
        if (!closing && (plater == nullptr || !plater->IsShownOnScreen()))
            return false;
        Stop();
        m_fn(plater);
        // Off the timer callback's stack: wxGTK's timeout callback still reads the timer after Notify().
        GUI::wxGetApp().CallAfter([this]() { delete this; });
        return true;
    }

    void Notify() override { try_run(); }

private:
    std::function<void(GUI::Plater*)> m_fn;
};

void run_when_plater_shown(std::function<void(GUI::Plater*)> fn)
{
    GUI::wxGetApp().CallAfter([fn = std::move(fn)]() mutable {
        auto* waiter = new PlaterShownWaiter(std::move(fn));
        if (!waiter->try_run())
            waiter->Start(100);
    });
}

py::object ui_create_dock_panel(const std::string& html, const std::string& title, int width, int height,
                                py::object on_message, py::object on_close, const std::string& dock)
{
    if (std::find(std::begin(DOCK_POSITIONS), std::end(DOCK_POSITIONS), dock) == std::end(DOCK_POSITIONS))
        throw std::invalid_argument("orca.host.ui.create_dock_panel dock must be \"left\", \"right\", \"top\", \"bottom\" or \"float\"");

    auto              msg_adapter  = make_message_adapter(std::move(on_message));
    CallablePtr       close_holder = make_holder(std::move(on_close));
    const std::string plugin_key   = PluginAuditManager::instance().current_plugin();
    const int         w            = width > 0 ? width : 320;
    const int         h            = height > 0 ? height : 480;

    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    // Deferred and pre-bound for the same reasons as create_window().
    const int new_id = UiRegistry::instance().reserve_id();
    UiRegistry::instance().bind(new_id, nullptr, plugin_key);

    run_when_plater_shown([new_id, plugin_key, html, title, dock, w, h,
                           msg_adapter = std::move(msg_adapter),
                           close_holder = std::move(close_holder)](GUI::Plater* plater) mutable {
        if (!UiRegistry::instance().is_open(new_id))
            return;

        if (plater == nullptr) {
            UiRegistry::instance().remove(new_id);
            return;
        }

        auto  on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };
        auto* panel = new GUI::DockPanel(plater, html, std::move(msg_adapter), make_close_adapter(close_holder),
                                         std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, panel, plugin_key);
        plater->add_dock_pane(panel, GUI::plugin_pane_name(plugin_key, title), wxString::FromUTF8(title), dock,
                              wxSize(w, h), [panel]() { panel->fire_close(); });
    });

    return py::cast(UiDockPanelHandle{new_id});
}

// Keep the original Copilot API on the upstream dock-panel implementation.
// Vertical panes used width as their height; preserve that argument convention.
py::object ui_create_docked_panel(const std::string& html, const std::string& title, const std::string& side,
                                  int width, py::object on_message, py::object on_close)
{
    if (side != "left" && side != "right" && side != "top" && side != "bottom")
        throw std::invalid_argument("orca.host.ui.create_docked_panel side must be \"left\", \"right\", \"top\" or \"bottom\"");
    const int span = width > 0 ? width : 380;
    const bool vertical = side == "top" || side == "bottom";
    return ui_create_dock_panel(html, title, vertical ? 320 : span, vertical ? span : 480,
                                std::move(on_message), std::move(on_close), side);
}

void handle_post(int id, py::object data)
{
    if (wxTheApp == nullptr)
        return;
    json j = py_to_json(data); // GIL held (binding body)
    GUI::wxGetApp().CallAfter([id, j = std::move(j)]() {
        auto* window = UiRegistry::instance().get_as<wxWindow>(id);
        if (auto* panel = dynamic_cast<GUI::DockPanel*>(window))
            panel->push_message(j);
        else
            GUI::WebDialog::post_message(dynamic_cast<GUI::WebDialog*>(window), j);
    });
}

void handle_close(int id)
{
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([id]() {
        auto* window = UiRegistry::instance().get_as<wxWindow>(id);
        if (auto* panel = dynamic_cast<GUI::DockPanel*>(window))
            panel->request_close();
        else
            GUI::WebDialog::request_close(dynamic_cast<GUI::WebDialog*>(window));
    });
}

void handle_show(int id, bool show)
{
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([id, show]() {
        auto*        panel  = UiRegistry::instance().get_as<GUI::DockPanel>(id);
        GUI::Plater* plater = GUI::wxGetApp().plater();
        if (panel != nullptr && plater != nullptr)
            plater->show_dock_pane(panel, show);
    });
}

UiProgressHandle ui_create_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    const int         max_value  = maximum > 0 ? maximum : 100;

    return run_on_ui_blocking([&]() -> UiProgressHandle {
        const int new_id = UiRegistry::instance().reserve_id();

        // Registry cleanup: GIL-free, runs from the dialog destructor on every path.
        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };

        auto* dlg = GUI::PluginProgressDialog::create_dialog(ui_parent(), wxString::FromUTF8(title),
                                                             wxString::FromUTF8(message), max_value, style,
                                                             std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        return UiProgressHandle{new_id};
    });
}

UiProgressHandle* new_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    return new UiProgressHandle(ui_create_progress_dialog(title, message, maximum, style));
}

bool progress_is_open(int id)
{
    return UiRegistry::instance().is_open(id);
}

bool progress_pulse(int id, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::pulse(d, wxString::FromUTF8(message));
    });
}

bool progress_update(int id, int value, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::update(d, value, wxString::FromUTF8(message));
    });
}

void progress_start_pulse(int id, int interval_ms, const std::string& message)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::start_pulse(d, interval_ms, wxString::FromUTF8(message));
    });
}

void progress_stop_pulse(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::stop_pulse(d);
    });
}

void progress_close(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::request_close(d);
    });
}

// --------------------------------------------------------------------------
// orca.host.ui.on_setting_ask
// --------------------------------------------------------------------------
// The setting popover has a single ask link, so a single slot; this records who
// filled it so unload can empty it again. Registration is marshaled to the UI
// thread, which is also where the teardown below and MarkdownTip itself run, so
// the slot needs no lock of its own.
std::string ask_owner;

void ui_on_setting_ask(py::object callback)
{
    CallablePtr       holder     = make_holder(std::move(callback));
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();

    run_on_ui_blocking([&]() {
        if (!holder) {
            ask_owner.clear();
            GUI::MarkdownTip::SetAskHandler({});
            return;
        }
        ask_owner = plugin_key;
        GUI::MarkdownTip::SetAskHandler([holder](std::string key) {
            PythonGILState gil;
            if (!gil)
                return;
            try {
                holder->fn(std::move(key));
            } catch (py::error_already_set& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_setting_ask handler raised: " << e.what();
                PyErr_Clear();
            }
        });
    });
}


// --------------------------------------------------------------------------
// orca.host.ui.jump_to_setting
// --------------------------------------------------------------------------
// The counterpart to on_setting_ask: that one lets the user point at a setting
// and ask about it, this one lets the answer point back. A model that can name
// a setting but not show it has to describe where the field is, and a described
// screen position is the thing it most reliably gets wrong.
void ui_jump_to_setting(const std::string& key)
{
    if (key.empty())
        throw std::invalid_argument("setting key must not be empty");

    // Resolving the owning preset happens on the UI thread too: the bundle is
    // the live one and the user can switch presets under us.
    const bool found = run_on_ui_blocking([&key]() -> bool {
        PresetBundle* bundle = GUI::wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return false;

        Preset::Type type;
        if (bundle->prints.get_edited_preset().config.has(key))
            type = Preset::TYPE_PRINT;
        else if (bundle->filaments.get_edited_preset().config.has(key))
            type = Preset::TYPE_FILAMENT;
        else if (bundle->printers.get_edited_preset().config.has(key))
            type = Preset::TYPE_PRINTER;
        else
            return false;

        // The option lives in the sidebar's parameter panel, which is only
        // shown on the 3D editor tab — jumping there first is what makes the
        // blink visible rather than happening on a hidden page.
        if (GUI::wxGetApp().mainframe != nullptr)
            GUI::wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREPARE));
        GUI::wxGetApp().sidebar().jump_to_option(key, type, std::wstring());
        return true;
    });

    if (!found)
        throw std::invalid_argument("unknown setting: " + key);
}

// --------------------------------------------------------------------------
// orca.host.ui.on_slice_complete
// --------------------------------------------------------------------------
// One slot, like on_setting_ask, and for the same reason: a second registration
// replaces the first so unload can reliably empty it.
//
// Fires only on a SUCCESSFUL slice. A cancelled or failed one leaves the plate
// without layers to read, so waking a plugin for it would only produce an
// answer about nothing.
std::string slice_owner;
CallablePtr slice_holder;
bool        slice_bound = false;

void ui_on_slice_complete(py::object callback)
{
    CallablePtr       holder     = make_holder(std::move(callback));
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();

    run_on_ui_blocking([&]() {
        GUI::Plater* plater = GUI::wxGetApp().plater();
        if (plater == nullptr)
            return;

        slice_holder = holder;
        slice_owner  = holder ? plugin_key : std::string();

        // Bound once for the life of the app and switched by the holder above.
        // Unbinding a lambda needs the identical functor back, which we cannot
        // reproduce; keeping one binding and letting it no-op is both simpler
        // and the only version that cannot leave a stale handler behind.
        if (slice_bound)
            return;
        plater->Bind(GUI::EVT_PROCESS_COMPLETED, [](SlicingProcessCompletedEvent& evt) {
            evt.Skip();                  // the slicer's own handlers must still run
            if (!evt.success())
                return;                  // cancelled or failed: no layers to read
            CallablePtr holder = slice_holder;
            if (!holder)
                return;                  // unregistered
            PythonGILState gil;
            if (!gil)
                return;
            try {
                holder->fn();
            } catch (py::error_already_set& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_slice_complete handler raised: " << e.what();
                PyErr_Clear();
            }
        });
        slice_bound = true;
    });
}
void plater_notification(GUI::NotificationManager::NotificationLevel notification_level, const std::string& text,
                         const std::string& hypertext, py::object on_click)
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    CallablePtr        holder    = make_holder(std::move(on_click));
    if (holder)
        UiRegistry::instance().bind_callback(holder, plugin_key);

    std::function<bool(wxEvtHandler*)> callback;
    if (holder) {
        callback = [holder](wxEvtHandler*) -> bool {
            if (!holder->active.load(std::memory_order_acquire))
                return false;

            PythonGILState gil;
            if (!gil)
                return false;
            try {
                py::object result = holder->fn();
                return result.is_none() || result.cast<bool>();
            } catch (py::error_already_set& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised: " << e.what();
                PyErr_Clear();
                return false;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised: " << e.what();
                return false;
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised an unknown exception";
                return false;
            }
        };
    }

    run_on_ui_blocking([notification_level, text, hypertext, callback = std::move(callback)]() mutable {
        GUI::wxGetApp().plater()->get_notification_manager()->push_notification(GUI::NotificationType::CustomNotification, notification_level, text,
                                                                           hypertext, std::move(callback));
    });
}

} // namespace

void PluginHostUi::RegisterBindings(pybind11::module_& host)
{
    auto ui = host.def_submodule(
        "ui",
        "Host UI: native dialogs and interactive HTML windows. Calls run on the main/UI "
        "thread (marshaled from the plugin thread). See the plugin docs for the window.orca bridge. "
        "Do not call these from a slicing pipeline hook (SlicingPipeline capability): that hook runs "
        "on the slicing worker thread, which the UI thread can itself be blocked waiting on, so a "
        "marshaled UI call from there can deadlock the application.");

    ui.def("message", &ui_message, py::arg("text"), py::arg("title") = "OrcaSlicer", py::arg("buttons") = "ok",
           py::arg("icon") = "info",
           "Show a native modal message box; returns the clicked button id "
           "(\"ok\"/\"cancel\"/\"yes\"/\"no\"). buttons: \"ok\"|\"ok_cancel\"|\"yes_no\"|\"yes_no_cancel\"; "
           "icon: \"info\"|\"warning\"|\"error\"|\"question\".");

    ui.attr("PD_APP_MODAL")      = py::int_(wxPD_APP_MODAL);
    ui.attr("PD_AUTO_HIDE")      = py::int_(wxPD_AUTO_HIDE);
    ui.attr("PD_CAN_ABORT")      = py::int_(wxPD_CAN_ABORT);
    ui.attr("PD_CAN_SKIP")       = py::int_(wxPD_CAN_SKIP);
    ui.attr("PD_ELAPSED_TIME")   = py::int_(wxPD_ELAPSED_TIME);
    ui.attr("PD_ESTIMATED_TIME") = py::int_(wxPD_ESTIMATED_TIME);
    ui.attr("PD_REMAINING_TIME") = py::int_(wxPD_REMAINING_TIME);
    ui.attr("WINDOW_MODELESS")   = py::int_(WINDOW_MODELESS);
    ui.attr("WINDOW_MODAL")      = py::int_(WINDOW_MODAL);

    py::class_<UiWindowHandle>(ui, "UiWindow",
                               "Handle to plugin HTML hosted by the app: a window or modal dialog from "
                               "create_window(), or a docked pane from create_docked_panel().")
        .def_property_readonly("id", [](const UiWindowHandle& h) { return h.id; })
        .def(
            "post", [](const UiWindowHandle& h, py::object data) { handle_post(h.id, std::move(data)); },
            py::arg("data"), "Send a payload to the page (delivered to window.orca.onMessage handlers).")
        .def(
            "close", [](const UiWindowHandle& h) { handle_close(h.id); }, "Close the window (fires on_close).")
        .def(
            "is_open", [](const UiWindowHandle& h) { return UiRegistry::instance().is_open(h.id); },
            "Return True while the window is open.");

    ui.def("create_window", &ui_create_window, py::arg("html"), py::arg("title") = "OrcaSlicer", py::arg("width") = 820,
           py::arg("height") = 600, py::arg("on_message") = py::none(), py::arg("on_close") = py::none(),
           py::arg("style") = WINDOW_MODELESS, py::arg("on_submit") = py::none(),
           "Open a persistent HTML window or modal dialog and return a UiWindow. style is WINDOW_MODELESS "
           "or WINDOW_MODAL. on_message(data) is called on the UI thread when the page posts; on_submit(data) "
           "is called when the page submits; offload heavy work to a thread and push results back with window.post().");

    py::class_<UiDockPanelHandle>(ui, "UiDockPanel", "Handle to a dockable plugin HTML panel created by create_dock_panel().")
        .def_property_readonly("id", [](const UiDockPanelHandle& h) { return h.id; })
        .def(
            "post", [](const UiDockPanelHandle& h, py::object data) { handle_post(h.id, std::move(data)); },
            py::arg("data"), "Send a payload to the page (delivered to window.orca.onMessage handlers).")
        .def(
            "show", [](const UiDockPanelHandle& h) { handle_show(h.id, true); }, "Show the panel again after hide().")
        .def(
            "hide", [](const UiDockPanelHandle& h) { handle_show(h.id, false); }, "Hide the panel without closing it.")
        .def(
            "close", [](const UiDockPanelHandle& h) { handle_close(h.id); }, "Close the panel (fires on_close).")
        .def(
            "is_open", [](const UiDockPanelHandle& h) { return UiRegistry::instance().is_open(h.id); },
            "Return True until the panel is closed; a hidden panel is still open.");

    ui.def("create_dock_panel", &ui_create_dock_panel, py::arg("html"), py::arg("title") = "OrcaSlicer",
           py::arg("width") = 320, py::arg("height") = 480, py::arg("on_message") = py::none(),
           py::arg("on_close") = py::none(), py::arg("dock") = "right",
           "Open an HTML panel docked beside the 3D view and return a UiDockPanel. dock is \"left\", \"right\", \"top\", "
           "\"bottom\" or \"float\", and width/height are in DIPs; the user can move and resize the panel, and a panel "
           "opened again comes back where the window layout was last saved. The panel belongs to the Prepare and "
           "Preview tabs. on_message(data) is called on the UI thread when the page posts; window.orca.close() "
           "or the panel's close button closes it and calls on_close(). A post() made before the page has "
           "loaded can be dropped, so have the page request its first data.");
    ui.def("create_docked_panel", &ui_create_docked_panel, py::arg("html"), py::arg("title") = "OrcaSlicer",
           py::arg("side") = "right", py::arg("width") = 380, py::arg("on_message") = py::none(),
           py::arg("on_close") = py::none(),
           "Dock an HTML panel into the plater workspace and return a UiDockPanel. side is "
           "\"left\"|\"right\"|\"top\"|\"bottom\"; width is the initial size along that axis in pixels. "
           "Safe to call from a capability's on_load: the panel waits for the plater to exist "
           "rather than failing. The user can resize it, drag it to another edge, or float it. The page uses the same "
           "window.orca bridge as create_window(), except that submit() is delivered to on_message "
           "(a docked pane has no modal result). Closing the pane fires on_close.");

    py::class_<UiProgressHandle>(ui, "ProgressDialog", "Handle to a native progress dialog.")
        .def(py::init(&new_progress_dialog), py::arg("title"), py::arg("message"), py::arg("maximum") = 100,
             py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
        .def_property_readonly("id", [](const UiProgressHandle& h) { return h.id; })
        .def(
            "pulse", [](const UiProgressHandle& h, const std::string& message) { return progress_pulse(h.id, message); },
            py::arg("message") = "", "Pulse the dialog gauge; returns False if the dialog is closed or cancelled.")
        .def(
            "update",
            [](const UiProgressHandle& h, int value, const std::string& message) {
                return progress_update(h.id, value, message);
            },
            py::arg("value"), py::arg("message") = "",
            "Set the dialog progress value; returns False if the dialog is closed or cancelled.")
        .def(
            "start_pulse",
            [](const UiProgressHandle& h, int interval_ms, const std::string& message) {
                progress_start_pulse(h.id, interval_ms, message);
            },
            py::arg("interval_ms") = 100, py::arg("message") = "", "Start periodic pulsing.")
        .def(
            "stop_pulse", [](const UiProgressHandle& h) { progress_stop_pulse(h.id); }, "Stop periodic pulsing.")
        .def(
            "close", [](const UiProgressHandle& h) { progress_close(h.id); }, "Close the progress dialog.")
        .def(
            "is_open", [](const UiProgressHandle& h) { return progress_is_open(h.id); },
            "Return True while this progress dialog is registered.")
        .def("__enter__", [](UiProgressHandle& h) -> UiProgressHandle& { return h; }, py::return_value_policy::reference_internal)
        .def("__exit__", [](const UiProgressHandle& h, py::object, py::object, py::object) {
            progress_close(h.id);
            return false;
        });

    ui.def("create_progress_dialog", &ui_create_progress_dialog, py::arg("title"), py::arg("message"),
           py::arg("maximum") = 100, py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE,
           "Create a native progress dialog and return a ProgressDialog handle.");

    ui.def("jump_to_setting", &ui_jump_to_setting, py::arg("key"),
           "Show a setting in the slicer's own UI: switch to the 3D editor tab, scroll the "
           "parameter panel to that option and blink it. Raises ValueError if no selected "
           "preset has the key.\n"
           "\n"
           "This is the return path for on_setting_ask. Naming a setting in text leaves the "
           "user to find it; a model that tries to describe where the field is will sooner or "
           "later describe it wrong. Point at it instead.");

    ui.def("on_slice_complete", &ui_on_slice_complete, py::arg("callback"),
           "Call `callback()` on the UI thread each time a slice finishes SUCCESSFULLY. Pass "
           "None to unregister. One slot: registering again replaces the previous callback.\n"
           "\n"
           "Cancelled and failed slices do not fire — they leave no layers to read, so there "
           "would be nothing to say. This is the moment measure_slice becomes meaningful, and "
           "a better trigger than watching the plate: the user has committed to slicing, so "
           "speaking up here interrupts nothing.\n"
           "\n"
           "The callback runs on the UI thread. Do not block in it — hand the work to a thread "
           "of your own and return.");

    ui.def("on_setting_ask", &ui_on_setting_ask, py::arg("callback"),
           "Register callback(setting_key) for the \"ask the copilot\" link in the settings tooltip, or "
           "None to unregister. There is one registration for the whole app: registering replaces any "
           "previous one, and unloading the plugin drops it. The callback runs on the UI thread with the "
           "GIL held, so keep it short — hand slow work to a thread and answer from there.");
    py::enum_<GUI::NotificationManager::NotificationLevel>(ui, "NotificationLevel")
        .value("ProgressBarNotificationLevel", GUI::NotificationManager::NotificationLevel::ProgressBarNotificationLevel)
        .value("HintNotificationLevel", GUI::NotificationManager::NotificationLevel::HintNotificationLevel)
        .value("RegularNotificationLevel", GUI::NotificationManager::NotificationLevel::RegularNotificationLevel)
        .value("PrintInfoNotificationLevel", GUI::NotificationManager::NotificationLevel::PrintInfoNotificationLevel)
        .value("PrintInfoShortNotificationLevel", GUI::NotificationManager::NotificationLevel::PrintInfoShortNotificationLevel)
        .value("ImportantNotificationLevel", GUI::NotificationManager::NotificationLevel::ImportantNotificationLevel)
        .value("WarningNotificationLevel", GUI::NotificationManager::NotificationLevel::WarningNotificationLevel)
        .value("SeriousWarningNotificationLevel", GUI::NotificationManager::NotificationLevel::SeriousWarningNotificationLevel)
        .value("ErrorNotificationLevel", GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel)
        .export_values();

    ui.def("push_notification", &plater_notification, py::arg("notification_level"), py::arg("text"),
           py::arg("hyper_text") = "", py::arg("on_click") = py::none(),
           "Push a plater notification. hyper_text is an underlined label; on_click() is called when it is clicked "
           "and may return True to close the notification.");
}

void PluginHostUi::clear_callbacks()
{
    GUI::MarkdownTip::SetAskHandler({});
    ask_owner.clear();
    slice_holder.reset();
    slice_owner.clear();
}

void PluginHostUi::close_windows_for_plugin(const std::string& plugin_key)
{
    if (wxTheApp == nullptr)
        return;

    auto teardown = [plugin_key]() {
        // Left in place, the handler would call into a module that is being finalized.
        if (ask_owner == plugin_key) {
            ask_owner.clear();
            GUI::MarkdownTip::SetAskHandler({});
        }
        if (slice_owner == plugin_key) {
            slice_owner.clear();
            slice_holder.reset();
        }
        for (auto& callback : UiRegistry::instance().take_callbacks_for_plugin(plugin_key))
            callback->disable();

        // Destroy() bypasses wxEVT_CLOSE, so the plugin's on_close is not fired on
        // forced teardown (intended); the resource destructor still cleans the registry.
        for (auto* window : UiRegistry::instance().take_for_plugin(plugin_key)) {
            if (auto* dialog = dynamic_cast<GUI::WebDialog*>(window))
                GUI::WebDialog::destroy_silently(dialog);
            else if (auto* panel = dynamic_cast<GUI::DockPanel*>(window))
                panel->destroy_silently();
            else if (window != nullptr)
                window->Destroy();
        }
    };

    if (wxIsMainThread())
        teardown();
    else
        GUI::wxGetApp().CallAfter(teardown);
}

} // namespace Slic3r
