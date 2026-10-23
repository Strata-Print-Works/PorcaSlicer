#include "PrinterScanDialog.hpp"
#include "slic3r/Utils/SavedPrinterConnection.hpp"
#include "slic3r/Utils/ElegooLink.hpp"
#include "slic3r/Utils/PrinterConnectionStatus.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/PrinterDiscovery.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/CrealityHostDiscovery.hpp"
#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/DiscoveryPolicy.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "PresetComboBoxes.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Monitor.hpp"
#include "StatusPanel.hpp"
#include "LazyPage.hpp"
#include "PrinterWebView.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "libslic3r/Utils.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/StateColor.hpp"
#include "DesignTokens.hpp"
#include "Typography.hpp"
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/listctrl.h>
#include <wx/dataview.h>
#include <wx/listbase.h>
#include <wx/toplevel.h>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/splitter.h>
#include <wx/string.h>
#include <wx/variant.h>
#include <wx/vector.h>
#include <wx/gdicmn.h>
#include <nlohmann/json.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <algorithm>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <future>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {
constexpr const char* saved_section = "scanned_printers";
constexpr const char* code_section = "scanned_printer_codes";
// Bambu LAN certificate fingerprints, keyed like the saved device. Local only:
// never written to projects, presets or exported profiles.
constexpr const char* certificate_section = "scanned_printer_certificates";
constexpr const char* destination_prefix = "scanned:";
std::optional<DynamicPrintConfig> control_config;
std::string control_device_id;
std::string normalized_model(std::string_view value)
{
    std::string result;
    for (unsigned char ch : value)
        if (std::isalnum(ch)) result += char(std::tolower(ch));
    return result;
}

bool matches_discovered_model(const DiscoveredPrinter& printer, const std::string& vendor_id,
                              const std::string& model_name, const std::string& model_id)
{
    if (printer.model.empty()) return false;
    if (printer.model == model_name || printer.model == model_id) return true;
    if (printer.vendor != "creality" || vendor_id != "Creality") return false;
    const std::string reported = normalized_model(printer.model);
    if (reported == normalized_model(model_id)) return true;
    std::string named = normalized_model(model_name);
    if (named.rfind("creality", 0) == 0) named.erase(0, 8);
    return reported == named;
}

bool preset_matches_discovered_model(const DiscoveredPrinter& printer, const Preset& preset,
                                     const PresetBundle& bundle)
{
    const std::string model_name = preset.config.opt_string("printer_model");
    for (const auto& vendor : bundle.vendors)
        for (const auto& model : vendor.second.models)
            if (model.name == model_name)
                return matches_discovered_model(printer, vendor.second.id, model.name, model.model_id);
    return false;
}

bool repair_saved_sparkx_preset(DiscoveredPrinter& printer)
{
    if (printer.vendor != "creality" || normalized_model(printer.model) != "sparkxi7" ||
        !wxGetApp().preset_bundle) return false;
    auto& bundle = *wxGetApp().preset_bundle;
    const Preset* old = bundle.printers.find_preset(printer.preset);
    if (old && preset_matches_discovered_model(printer, *old, bundle)) return false;
    const std::string variant = old ? old->config.opt_string("printer_variant") : "0.4";
    const std::string name = "Creality SPARKX i7 " + variant + " nozzle";
    const Preset* replacement = bundle.printers.find_preset(name);
    if (!replacement || !preset_matches_discovered_model(printer, *replacement, bundle)) return false;
    printer.preset = name; // Retain the selected nozzle; discovery cannot determine its size.
    return true;
}

// Parsed saved devices, rebuilt only when the stored section changes (add, edit,
// remove, address repair or any other app-config write). UI thread only.
struct SavedPrinterCache {
    std::map<std::string, std::string> raw;
    std::vector<DiscoveredPrinter> printers;
    std::map<std::string, size_t> by_key;
    bool valid = false;
    bool repaired = false; // Preset repair ran with the profile catalog loaded.
};
SavedPrinterCache saved_printer_cache;

const std::map<std::string, std::string>& saved_section_entries(const AppConfig& config)
{
    static const std::map<std::string, std::string> empty;
    return config.has_section(saved_section) ? config.get_section(saved_section) : empty;
}

// The reference is invalidated by the next write to the saved section; callers
// that may write while iterating use the saved_printers() copy instead.
const std::vector<DiscoveredPrinter>& saved_printer_list()
{
    auto* config = wxGetApp().app_config;
    auto& cache = saved_printer_cache;
    const bool can_repair = wxGetApp().preset_bundle != nullptr;
    if (cache.valid && (cache.repaired || !can_repair) && cache.raw == saved_section_entries(*config)) return cache.printers;
    std::vector<DiscoveredPrinter> result;
    std::vector<std::pair<std::string, std::string>> repaired;
    for (const auto& entry : saved_section_entries(*config))
        if (auto printer = DiscoveredPrinter::deserialize(entry.second)) {
            if (repair_saved_sparkx_preset(*printer)) repaired.emplace_back(entry.first, printer->serialize());
            merge_discovered_printer(result, std::move(*printer));
        }
    for (const auto& entry : repaired) config->set(saved_section, entry.first, entry.second);
    if (!repaired.empty()) config->save();
    cache.raw = saved_section_entries(*config);
    cache.printers = std::move(result);
    cache.by_key.clear();
    for (size_t i = 0; i < cache.printers.size(); ++i) cache.by_key.emplace(cache.printers[i].key(), i);
    cache.valid = true;
    cache.repaired = can_repair;
    return cache.printers;
}

std::vector<DiscoveredPrinter> saved_printers() { return saved_printer_list(); }

std::string project_destination(const DiscoveredPrinter& printer)
{
    return portable_printer_destination(std::string(destination_prefix) + printer.key(), *wxGetApp().app_config);
}

std::optional<DiscoveredPrinter> saved_printer(const std::string& destination)
{
    const auto& printers = saved_printer_list();
    if (boost::starts_with(destination, destination_prefix)) {
        const auto it = saved_printer_cache.by_key.find(destination.substr(8));
        if (it != saved_printer_cache.by_key.end()) return printers[it->second];
    } else if (boost::starts_with(destination, "scanned-id:")) {
        // Resolving a portable ID writes only the alias section, never the saved list.
        for (const auto& printer : printers)
            if (project_destination(printer) == destination) return printer;
    }
    return std::nullopt;
}

std::string device_id(const DiscoveredPrinter& p) { return saved_printer_device_id(p); }

wxString model_label(const DiscoveredPrinter& p)
{
    if (p.vendor == "flashforge" && p.model == "Flashforge") return _L("Model not confirmed");
    for (const auto& vendor : wxGetApp().preset_bundle->vendors)
        for (const auto& model : vendor.second.models)
            if (matches_discovered_model(p, vendor.second.id, model.name, model.model_id)) return from_u8(model.name);
    return p.model.empty() ? _L("Choose printer") : from_u8(p.model);
}

void apply_connection(const DiscoveredPrinter& p, DynamicPrintConfig& config)
{
    apply_saved_printer_connection(p, wxGetApp().app_config->get(code_section, p.key()), config);
}

static DiscoveryScanGate discovery_scan_gate;

struct DeviceLink {
    PrinterLinkResult result{PrinterLinkStatus::unreachable, PrinterLinkFailure::none};
    wxString restore_error;
    bool checking = true;
    unsigned failures = 0;
    std::chrono::steady_clock::time_point next_check{};
    std::string address, code;
};
// Updated only on wx's main thread. Workers return results through queued events.
std::map<std::string, DeviceLink> device_links;
wxDEFINE_EVENT(EVT_DEVICE_LINK, wxThreadEvent);

// Explicitly entering an access code also trusts the certificate the printer
// presents next, e.g. after a reset or replacement, and checks it again now.
void save_printer_code(const std::string& key, const std::string& code)
{
    wxGetApp().app_config->set(code_section, key, code);
    wxGetApp().app_config->erase(certificate_section, key);
    device_links.erase(key);
}
struct LinkUpdate {
    DiscoveredPrinter printer;
    std::string code;
    PrinterLinkResult result;
    std::string serial;
    std::string certificate_pin; // Saved pin when the check started.
    std::string certificate;     // Fingerprint the printer presented.
};

bool start_saved_printer_session(const DiscoveredPrinter& printer, DeviceLink& link)
{
    auto* network = wxGetApp().getAgent();
    auto* devices = wxGetApp().getDeviceManager();
    const auto* preset = wxGetApp().preset_bundle->printers.find_preset(printer.preset);
    if (!network || !devices || !preset || preset->printer_technology() != ptFFF) return false;
    DynamicPrintConfig config = preset->config;
    apply_connection(printer, config);
    const auto backend = config.opt_string("printer_agent");
    if (backend == "orca" || !NetworkAgentFactory::is_printer_agent_registered(backend)) return false;
    if (backend == "bbl" && !NetworkAgent::is_network_module_loaded()) return false;
    const auto id = device_id(printer);
    PrinterConnectionParams params;
    if (!saved_printer_connection_params(id, params)) return false;
    if (backend == "bbl" && params.password.empty()) {
        link.result = {PrinterLinkStatus::refused, PrinterLinkFailure::missing_credentials};
        link.checking = false;
        link.next_check = std::chrono::steady_clock::now() + std::chrono::seconds(300);
        return true;
    }
    // A retry obtains a new SDK handle. A queued callback from the retired
    // attempt retains its invalid token; it cannot inherit new callback storage.
    if (const auto token = network->printer_session_token(id); token && token->valid &&
        !network->expire_printer_session(id)) return false;
    if (auto old = network->printer_session_agent(id)) NetworkAgentFactory::release_printer_agent(old);
    auto transport = NetworkAgentFactory::create_printer_agent_by_id(
        backend, network->get_cloud_agent(backend == "bbl" ? BBL_CLOUD_PROVIDER : ORCA_CLOUD_PROVIDER), data_dir(), id);
    if (!transport) {
        link.restore_error = _L("Could not open a separate connection for this printer. Try reconnecting.");
        link.failures = std::min(link.failures + 1, 7u);
        link.next_check = std::chrono::steady_clock::now() + std::chrono::seconds(discovery_retry_seconds(link.failures, false));
        return true;
    }
    BBLocalMachine machine;
    machine.dev_id = id;
    machine.dev_ip = backend == "bbl" ? printer.address : id;
    machine.dev_name = printer.name;
    machine.printer_type = params.model_id;
    machine.printer_agent_id = backend;
    auto* obj = devices->get_local_machine(id);
    if (obj && obj->printer_agent_id != backend) return false;
    if (!obj) obj = devices->insert_local_device(machine, "lan", "free", "",
                                                   params.password.empty() ? "88888888" : params.password);
    if (!obj) return false;
    obj->set_dev_name(printer.name);
    obj->set_dev_ip(machine.dev_ip);
    obj->set_access_code(params.password.empty() ? "88888888" : params.password, false);
    const int result = network->connect_printer(params, std::move(transport));
    link.restore_error = result == 0 ? wxString() : _L("Could not connect. Check the printer and try reconnecting.");
    link.failures = std::min(link.failures + 1, 7u);
    link.next_check = std::chrono::steady_clock::now() + std::chrono::seconds(discovery_retry_seconds(link.failures, false));
    return true;
}

class PrinterConnections final : public wxPanel
{
    struct Guard { std::mutex mutex; wxEvtHandler* target = nullptr; std::atomic<bool> cancelled{false}; };
    std::shared_ptr<Guard> m_guard = std::make_shared<Guard>();
    wxTimer m_timer;
    std::vector<std::future<void>> m_workers;
    // Pending checks belong to this panel, unlike cached connection outcomes.
    // Recreating the GUI drops canceled ownership with the old panel.
    std::set<std::string> m_in_flight;
    size_t m_cursor = 0;
public:
    explicit PrinterConnections(wxWindow* parent) : wxPanel(parent), m_timer(this)
    {
        Hide();
        m_guard->target = this;
        Bind(EVT_DEVICE_LINK, [this](wxThreadEvent& event) {
            const auto update = event.GetPayload<LinkUpdate>();
            m_in_flight.erase(update.printer.key());
            const auto current = saved_printer(std::string(destination_prefix) + update.printer.key());
            // An edited or removed printer's link was already dropped; never recreate it.
            if (!current) return;
            auto& link = device_links[current->key()];
            const auto pin = wxGetApp().app_config->get(certificate_section, current->key());
            if (current->address != update.printer.address || current->vendor != update.printer.vendor ||
                current->port != update.printer.port || current->model != update.printer.model ||
                current->serial != update.printer.serial || current->local_control != update.printer.local_control ||
                current->lan_mode != update.printer.lan_mode || current->access_code_required != update.printer.access_code_required ||
                wxGetApp().app_config->get(code_section, current->key()) != update.code || pin != update.certificate_pin) {
                link.next_check = {};
                return;
            }
            if (update.result.failure == PrinterLinkFailure::cancelled) return;
            if (current->vendor == "bambu" && should_store_certificate_pin(update.certificate_pin, pin, update.certificate,
                                                                          update.result.status == PrinterLinkStatus::connected)) {
                wxGetApp().app_config->set(certificate_section, current->key(), update.certificate);
                wxGetApp().app_config->save();
            }
            link.result = update.result;
            if (current->vendor == "elegoo" && !update.serial.empty())
                wxGetApp().app_config->set("dev_sn", device_id(*current), update.serial);
            if (update.result.status == PrinterLinkStatus::connected) link.failures = 0;
            else link.failures = std::min(link.failures + 1, 7u);
            link.checking = link.failures > 0 && link.failures < 3 && update.result.status != PrinterLinkStatus::refused;
            link.next_check = std::chrono::steady_clock::now() + std::chrono::seconds(
                update.result.status == PrinterLinkStatus::refused ? 300 : discovery_retry_seconds(link.failures, update.result.status == PrinterLinkStatus::connected));
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh(); });
        m_timer.Start(1000);
        refresh();
    }
    ~PrinterConnections() override
    {
        m_timer.Stop();
        {
            std::lock_guard<std::mutex> lock(m_guard->mutex);
            m_guard->target = nullptr;
            m_guard->cancelled.store(true);
        }
        m_workers.clear(); // Futures join bounded, cancellable checks before the owner disappears.
    }
    void refresh()
    {
        if (wxGetApp().is_closing()) return;
        m_workers.erase(std::remove_if(m_workers.begin(), m_workers.end(), [](auto& worker) {
            return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), m_workers.end());
        auto* devices = wxGetApp().getDeviceManager();
        const bool ready = wxGetApp().initialized() && wxGetApp().getAgent() && devices &&
            !discovery_scan_gate.active() && m_workers.empty();
        if (discovery_scan_gate.active() || m_workers.size() >= 2) return;
        const auto printers = saved_printers();
        for (size_t offset = 0; offset < printers.size(); ++offset) {
            const auto index = (m_cursor + offset) % printers.size();
            const auto& p = printers[index];
            auto& link = device_links[p.key()];
            if (m_in_flight.count(p.key())) continue;
            if (auto* agent = wxGetApp().getAgent()) {
                if (const auto token = agent->printer_session_token(device_id(p))) {
                    if (token->recovery_deferred(std::chrono::steady_clock::now())) continue;
                    auto* machine = devices ? devices->get_local_machine(device_id(p)) : nullptr;
                    if (token->needs_reconnect(machine && machine->is_connected(), std::chrono::steady_clock::now())) {
                        const bool was_connected = token->state == PrinterControlState::connected;
                        if (!agent->expire_printer_session(device_id(p))) continue;
                        link.restore_error = was_connected ? _L("Stopped receiving printer status. Reconnecting shortly.") :
                                                           _L("Connection timed out. Retrying shortly.");
                        link.next_check = std::chrono::steady_clock::now() +
                            std::chrono::seconds(discovery_retry_seconds(link.failures, false));
                        if (auto* obj = devices ? devices->get_local_machine(device_id(p)) : nullptr) {
                            obj->set_online_state(false);
                            obj->set_lan_mode_connection_state(false);
                        }
                        m_cursor = (index + 1) % printers.size();
                        break; // Retire at most one stale transport per timer tick, too.
                    }
                    if (token->state == PrinterControlState::connected) {
                        if (machine && machine->is_connected()) {
                            link.failures = 0;
                            link.restore_error.clear();
                            link.checking = false;
                        }
                        continue;
                    }
                    if (token->state == PrinterControlState::connecting) continue;
                    if (token->state == PrinterControlState::rejected) continue; // Edited credentials or explicit Reconnect.
                }
            }
            if (ready && std::chrono::steady_clock::now() >= link.next_check && start_saved_printer_session(p, link)) {
                m_cursor = (index + 1) % printers.size();
                break; // At most one new/retried control connection per second.
            }
            if (auto* agent = wxGetApp().getAgent(); agent && agent->has_printer_session(device_id(p))) continue;
            if (auto* devices = wxGetApp().getDeviceManager()) {
                auto* machine = devices->get_selected_machine();
                if (machine && machine->get_dev_id() == device_id(p) && machine->is_connected()) {
                    link.result = {PrinterLinkStatus::connected, PrinterLinkFailure::none};
                    link.restore_error.clear();
                    link.checking = false;
                    link.failures = 0;
                    continue; // Reuse live telemetry instead of opening another authenticated connection.
                }
            }
            const auto code = wxGetApp().app_config->get(code_section, p.key());
            if (link.address != p.address || link.code != code) {
                link.checking = true;
                link.failures = 0;
                link.result = {PrinterLinkStatus::unreachable, PrinterLinkFailure::none};
                link.next_check = {};
                link.address = p.address;
                link.code = code;
            }
            if (std::chrono::steady_clock::now() < link.next_check) continue;
            m_in_flight.insert(p.key());
            auto guard = m_guard;
            const auto pin = wxGetApp().app_config->get(certificate_section, p.key());
            m_workers.emplace_back(std::async(std::launch::async, [guard, p, code, pin] {
                PrinterLinkResult result;
                std::string serial, certificate;
                try { result = check_printer_connection(p, code, &guard->cancelled, &serial, pin, &certificate); } catch (const std::exception&) {}
                std::lock_guard<std::mutex> lock(guard->mutex);
                if (!guard->target) return;
                auto* event = new wxThreadEvent(EVT_DEVICE_LINK);
                event->SetPayload(LinkUpdate{p, code, result, serial, pin, certificate});
                wxQueueEvent(guard->target, event);
            }));
            m_cursor = (index + 1) % printers.size();
            break; // Start at most one connection per timer tick, including startup.
        }
    }
};

wxString connection_failure_label(const PrinterLinkResult& result)
{
    switch (result.failure) {
    case PrinterLinkFailure::none: return {};
    case PrinterLinkFailure::cancelled: return _L("Connection check cancelled");
    case PrinterLinkFailure::missing_credentials: return _L("Enter the printer's access code");
    case PrinterLinkFailure::invalid_address: return _L("Check the saved printer address");
    case PrinterLinkFailure::identity_mismatch: return _L("Printer identity changed — check the saved address");
    case PrinterLinkFailure::certificate_changed: return _L("Printer identity changed — check its address. If you reset or replaced it, re-enter its access code in Edit printer.");
    case PrinterLinkFailure::timeout: return _L("Printer did not respond in time");
    case PrinterLinkFailure::connection_refused: return _L("Printer refused the connection");
    case PrinterLinkFailure::unreachable: return _L("Could not reach the printer");
    case PrinterLinkFailure::handshake: return _L("Could not establish a secure connection");
    case PrinterLinkFailure::send: return _L("Could not send the connection request");
    case PrinterLinkFailure::receive: return _L("Connection closed before the printer replied");
    case PrinterLinkFailure::invalid_response: return _L("Unexpected reply from the printer");
    case PrinterLinkFailure::credentials_rejected: return _L("Access denied — check the access code or API key");
    case PrinterLinkFailure::service_rejected: return _L("Printer rejected the connection request");
    case PrinterLinkFailure::printer_not_ready: return _L("Printer service is available, but the printer is not ready");
    case PrinterLinkFailure::http_error: return wxString::Format(_L("Printer returned HTTP %u"), result.http_status);
    case PrinterLinkFailure::request_failed: return _L("Connection check failed — check the address and network");
    }
    return {};
}

wxString connection_label(const DiscoveredPrinter& p)
{
    const auto it = device_links.find(p.key());
    if (it == device_links.end()) return _L("Saved");
    const auto& link = it->second;
    if (!link.restore_error.empty()) return link.restore_error;
    const auto failure = connection_failure_label(link.result);
    if (!failure.empty()) return link.checking ? failure + " · " + _L("Retrying…") : failure;
    if (link.checking) return _L("Checking availability…");
    return link.result.status == PrinterLinkStatus::connected ? _L("Available") : _L("Offline");
}

wxString control_failure_label(const std::string& reason)
{
    if (reason == "bambu_worker_start_failed" || reason == "bambu_worker_missing")
        return _L("Porca could not start this printer's connection. Reinstall Porca, then restart it.");
    if (reason == "bambu_create_null" || reason == "bambu_create_duplicate")
        return _L("Bambu connection support could not start for this printer. Restart Porca. If this continues, reinstall Bambu connection support.");
    if (reason == "bambu_library_load_failed" || reason == "bambu_missing_symbols" || reason == "bambu_unsupported_version")
        return _L("Bambu connection support could not be loaded. Reinstall Bambu connection support, then restart Porca.");
    if (reason == "bambu_config_failed" || reason == "bambu_country_failed" || reason == "bambu_cert_failed")
        return _L("Bambu connection settings could not be opened. Check that Porca can access its local settings and certificates, then restart it.");
    if (reason == "bambu_worker_exited" || reason == "bambu_worker_protocol_failed")
        return _L("The printer connection stopped unexpectedly. Porca will retry. If this continues, restart Porca.");
    if (reason == "bambu_start_failed") return _L("Bambu connection support could not connect. Restart Porca, then check the printer and network.");
    if (reason == "bambu_connection_failed") return _L("Could not connect to this printer. Check the printer and network. Porca will retry.");
    return {};
}

wxString printer_status_label(const DiscoveredPrinter& p)
{
    if (auto* agent = wxGetApp().getAgent()) {
        if (const auto token = agent->printer_session_token(device_id(p))) {
            switch (token->state.load()) {
            case PrinterControlState::connecting: return _L("Connecting controls…");
            case PrinterControlState::rejected: return _L("Access denied — check the saved code");
            case PrinterControlState::offline: {
                const auto reason = control_failure_label(token->error());
                if (!reason.empty()) return reason;
                const auto link = device_links.find(p.key());
                return link != device_links.end() && !link->second.restore_error.empty() ? link->second.restore_error :
                    _L("Connection lost. Porca will retry.");
            }
            case PrinterControlState::connected:
                if (auto* devices = wxGetApp().getDeviceManager()) {
                    auto* obj = devices->get_local_machine(device_id(p));
                    if (!obj || !obj->is_connected()) return _L("Status out of date");
                }
                break;
            }
        }
    }
    if (auto* devices = wxGetApp().getDeviceManager())
        if (auto* obj = devices->get_local_machine(device_id(p)); obj && obj->is_connected()) {
            if (obj->is_in_printing_pause()) return _L("Paused");
            if (obj->is_in_printing()) return wxString::Format(_L("Printing %d%%"), obj->mc_print_percent);
            return obj->is_connecting() ? _L("Connecting…") : _L("Connected");
        }
    return connection_label(p);
}

wxString printer_list_status(const DiscoveredPrinter& printer)
{
    if (auto* agent = wxGetApp().getAgent())
        if (const auto token = agent->printer_session_token(device_id(printer)); token && token->state == PrinterControlState::offline)
            return _L("Connection unavailable");
    const auto link = device_links.find(printer.key());
    if (link != device_links.end() && !link->second.restore_error.empty()) return _L("Connection unavailable");
    return printer_status_label(printer);
}

std::vector<int> device_plates(const DiscoveredPrinter& p)
{
    std::vector<int> result;
    auto* plater = wxGetApp().plater();
    if (!plater) return result;
    const auto destination = project_destination(p);
    auto& plates = plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i)->print_setup().destination == destination) result.push_back(i);
    return result;
}

int device_plate(const DiscoveredPrinter& p)
{
    const auto matches = device_plates(p);
    return matches.empty() ? -1 : matches.front();
}

wxDEFINE_EVENT(EVT_PRINTER_SCAN, wxThreadEvent);
struct ScanUpdate {
    std::optional<DiscoveredPrinter> printer;
    bool complete = false;
    bool available = true;
    unsigned source = 0;
};
struct ScanGuard {
    std::mutex mutex;
    wxEvtHandler* target = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    void post(ScanUpdate update)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!target || cancelled->load()) return;
        auto* event = new wxThreadEvent(EVT_PRINTER_SCAN);
        event->SetPayload(std::move(update));
        wxQueueEvent(target, event);
    }
};

class PrinterScanDialog final : public DPIDialog
{
public:
    explicit PrinterScanDialog(wxWindow* parent)
        : DPIDialog(parent, wxID_ANY, _L("Add a printer"), wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        SetFont(wxGetApp().normal_font());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_status = new wxStaticText(this, wxID_ANY, _L("Searching your network…"));
        sizer->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(16));
        m_list = new wxListView(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(640, 235)), wxLC_REPORT | wxLC_SINGLE_SEL);
        m_list->SetName(_L("Printers"));
        m_list->AppendColumn(_L("Printer"), wxLIST_FORMAT_LEFT, FromDIP(210));
        m_list->AppendColumn(_L("Model"), wxLIST_FORMAT_LEFT, FromDIP(170));
        m_list->AppendColumn(_L("Address"), wxLIST_FORMAT_LEFT, FromDIP(140));
        m_list->AppendColumn(_L("Status"), wxLIST_FORMAT_LEFT, FromDIP(100));
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

        auto* details = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
        details->AddGrowableCol(1);
        details->Add(new wxStaticText(this, wxID_ANY, _L("Slice with")), 0, wxALIGN_CENTER_VERTICAL);
        m_presets = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        m_presets->SetName(_L("Printer and nozzle"));
        details->Add(m_presets, 1, wxEXPAND);
        m_code_label = new wxStaticText(this, wxID_ANY, _L("Access code"));
        details->Add(m_code_label, 0, wxALIGN_CENTER_VERTICAL);
        m_code = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_code->SetName(_L("Access code"));
        details->Add(m_code, 1, wxEXPAND);
        sizer->Add(details, 0, wxEXPAND | wxALL, FromDIP(16));
        m_hint = new wxStaticText(this, wxID_ANY, _L("Select a printer to add it."));
        m_hint->Wrap(FromDIP(640));
        sizer->Add(m_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        m_rescan = new wxButton(this, wxID_ANY, _L("Scan again"));
        actions->Add(m_rescan, 0, wxRIGHT, FromDIP(8));
        auto* manual = new wxButton(this, wxID_ANY, _L("Add by address…"));
        actions->Add(manual, 0);
        actions->AddStretchSpacer();
        auto* close = new wxButton(this, wxID_ANY, _L("Close"));
        actions->Add(close, 0, wxRIGHT, FromDIP(8));
        m_add = new wxButton(this, wxID_ANY, _L("Add printer"));
        m_add->SetDefault();
        actions->Add(m_add, 0);
        sizer->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
        SetSizerAndFit(sizer);
        SetMinSize(GetSize());
        CentreOnParent();
        wxGetApp().UpdateDlgDarkUI(this);

        m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { select(); });
        m_presets->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_add(); });
        m_code->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_add(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        manual->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_by_address(); });
        m_rescan->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_scan(); });
        m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add(); });
        Bind(EVT_PRINTER_SCAN, [this](wxThreadEvent& event) {
            const auto update = event.GetPayload<ScanUpdate>();
            if (update.printer) receive(*update.printer);
            if (update.complete) m_progress.complete(update.source, update.available);
            if (m_progress.finished()) {
                stop_scan();
                m_rescan->Enable();
                m_status->SetLabel(m_seen.empty() ? (m_progress.available() ? _L("No printers answered. Scan again or add one by address.") :
                                                                    _L("Could not search the network. Check your connection and try again.")) :
                                                   wxString::Format(_L("Found %d printers. Select one to add it."), int(m_seen.size())));
            }
        });
        m_printers = saved_printers();
        for (const auto& p : m_printers) append_row(p);
        select();
        start_scan();
    }
    ~PrinterScanDialog() override { stop_scan(); }

private:
    wxListView* m_list = nullptr;
    wxStaticText *m_status = nullptr, *m_hint = nullptr, *m_code_label = nullptr;
    ::ComboBox* m_presets = nullptr;
    wxTextCtrl* m_code = nullptr;
    wxButton *m_rescan = nullptr, *m_add = nullptr;
    std::vector<DiscoveredPrinter> m_printers;
    std::vector<std::string> m_preset_names;
    std::set<std::string> m_seen;
    std::shared_ptr<ScanGuard> m_guard;
    std::vector<Bonjour::Ptr> m_bonjour;
    std::vector<std::future<void>> m_workers;
    DiscoveryScanGate::Lease m_scan_lease;
    DiscoveryScanProgress m_progress;

    void on_dpi_changed(const wxRect&) override
    {
        SetFont(wxGetApp().normal_font());
        Layout();
        GetSizer()->SetSizeHints(this);
    }

    void stop_scan()
    {
        if (m_guard) {
            std::lock_guard<std::mutex> lock(m_guard->mutex);
            m_guard->cancelled->store(true);
            m_guard->target = nullptr;
        }
        for (const auto& lookup : m_bonjour) lookup->cancel();
        for (auto& worker : m_workers) worker.wait();
        for (const auto& lookup : m_bonjour) lookup->wait();
        m_workers.clear();
        m_bonjour.clear();
        m_guard.reset();
        m_scan_lease.reset();
    }
    void append_row(const DiscoveredPrinter& p)
    {
        const long row = m_list->InsertItem(m_list->GetItemCount(), from_u8(p.name));
        update_row(row, p);
    }
    void update_row(long row, const DiscoveredPrinter& p)
    {
        m_list->SetItem(row, 0, from_u8(p.name));
        m_list->SetItem(row, 1, model_label(p));
        m_list->SetItem(row, 2, from_u8(p.address));
        m_list->SetItem(row, 3, p.vendor == "elegoo" && !p.local_control ? _L("Check LAN mode") :
            p.vendor == "elegoo" && discovered_printer_requires_code(p) && wxGetApp().app_config->get(code_section, p.key()).empty() ?
                _L("Access code needed") : p.preset.empty() ? _L("Found") : _L("Added"));
    }
    void receive(DiscoveredPrinter p)
    {
        if (!DiscoveredPrinter::deserialize(p.serialize())) return;
        m_seen.insert(p.key());
        const auto existing = std::find_if(m_printers.begin(), m_printers.end(), [&p](const auto& item) { return same_discovered_printer(item, p); });
        const bool is_new = existing == m_printers.end();
        const bool setup_changed = !is_new && (existing->local_control != p.local_control ||
            existing->access_code_required != p.access_code_required || existing->lan_mode != p.lan_mode);
        const long row = is_new ? long(m_printers.size()) : long(existing - m_printers.begin());
        merge_discovered_printer(m_printers, std::move(p));
        if (is_new) append_row(m_printers.back());
        else {
            const auto& printer = m_printers[size_t(row)];
            update_row(row, printer);
            // Repair an added printer's address after DHCP changes, retaining its profile and code.
            if (!printer.preset.empty()) wxGetApp().app_config->set(saved_section, printer.key(), printer.serialize());
        }
        if (m_list->GetFirstSelected() < 0) { m_list->Select(row); select(); }
        else if (m_list->GetFirstSelected() == row && setup_changed) {
            const auto code = m_code->GetValue();
            const auto chosen = m_presets->GetSelection() >= 0 ? m_preset_names[size_t(m_presets->GetSelection())] : std::string();
            select();
            m_code->ChangeValue(code);
            const auto choice = std::find(m_preset_names.begin(), m_preset_names.end(), chosen);
            if (choice != m_preset_names.end()) m_presets->SetSelection(int(choice - m_preset_names.begin()));
            update_add();
        }
        m_status->SetLabel(wxString::Format(_L("Found %d printers. You can add one while the search continues."), int(m_seen.size())));
    }
    void start_scan()
    {
        stop_scan();
        DeletePendingEvents();
        m_scan_lease = discovery_scan_gate.try_acquire();
        if (!m_scan_lease) {
            m_status->SetLabel(_L("A search is already running. Wait for it to finish, then scan again."));
            return;
        }
        try {
            m_guard = std::make_shared<ScanGuard>();
            m_guard->target = this;
            m_progress.reset(7);
            m_seen.clear();
            m_rescan->Disable();
            m_status->SetLabel(_L("Searching your network…"));
            auto guard = m_guard;
            const PrinterFoundFn found = [guard](DiscoveredPrinter p) { guard->post({std::move(p), false, true}); };
            unsigned next_source = 0;
            auto worker = [this, guard, &next_source](std::function<bool()> scan) {
                const auto source = next_source++;
                m_workers.emplace_back(std::async(std::launch::async, [guard, source, scan = std::move(scan)] {
                    bool available = false;
                    try { available = scan(); } catch (const std::exception&) {}
                    guard->post({std::nullopt, true, available, source});
                }));
            };
            worker([guard, found] { return scan_elegoo_printers(guard->cancelled, found); });
            worker([guard, found] { return scan_bambu_printers(guard->cancelled, found); });
            worker([guard, found] { return scan_snapmaker_printers(guard->cancelled, found); });
            worker([guard, found] {
                std::vector<FlashforgeDiscoveredPrinter> printers;
                wxString error;
                const bool available = Flashforge::discover_printers(printers, error, 6000, 6000, 1, guard->cancelled.get());
                for (const auto& p : printers) {
                    if (guard->cancelled->load()) break;
                    found({"flashforge", p.name, "Flashforge", p.serial_number, p.ip_address, 8899, true, ""});
                }
                return available;
            });
            worker([guard, found] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                for (const auto& p : CrealityHostDiscovery::scan(false, guard->cancelled.get())) {
                    if (guard->cancelled->load() || std::chrono::steady_clock::now() >= deadline) break;
                    DiscoveredPrinter printer{"creality", p.hostname.empty() ? "Creality printer" : p.hostname,
                                              "", p.mac, p.ip, 80, true, ""};
                    // A bounded, read-only identity request; no upload or control command.
                    Http::get("http://" + p.ip + "/info").timeout_connect(1).timeout_max(1).size_limit(65536)
                        .on_progress([guard](Http::Progress, bool& abort) { abort = guard->cancelled->load(); })
                        .on_complete([&printer](std::string body, unsigned) {
                            try {
                                const auto j = nlohmann::json::parse(body);
                                const auto code = j.value("model", std::string());
                                printer.model = CrealityPrint::model_display_name(code);
                                if (printer.model.empty()) printer.model = code;
                                printer.serial = j.value("mac", std::string());
                            } catch (const nlohmann::json::exception&) {}
                        }).perform_sync();
                    found(std::move(printer));
                }
                return true;
            });
            m_bonjour.clear();
            for (const std::string service : {"moonraker", "octoprint"}) {
                const auto source = next_source++;
                auto lookup = Bonjour(service).set_timeout(6).set_retries(1)
                    .set_txt_keys({"sn", "model", "hostname"})
                    .on_reply([guard, found, service](BonjourReply&& reply) {
                        const auto field = [&reply](const std::string& key) {
                            const auto it = reply.txt_data.find(key);
                            return it == reply.txt_data.end() ? std::string() : it->second;
                        };
                        DiscoveredPrinter printer{service, reply.service_name, field("model"), field("sn"),
                                                  reply.ip.to_string(), reply.port, true, ""};
                        if (printer.name.empty()) printer.name = printer.model.empty() ? reply.hostname : printer.model;
                        if (!guard->cancelled->load()) found(std::move(printer));
                    }).on_complete([guard, source] { guard->post({std::nullopt, true, true, source}); }).lookup();
                m_bonjour.push_back(std::move(lookup));
            }
        } catch (const std::exception&) {
            stop_scan();
            m_rescan->Enable();
            m_status->SetLabel(_L("Could not start the search. Try again."));
        }
    }
    void select()
    {
        m_code->ChangeValue("");
        m_presets->Clear();
        m_preset_names.clear();
        const long row = m_list->GetFirstSelected();
        if (row < 0) { m_presets->Disable(); m_code->Disable(); m_add->Disable(); return; }
        const auto& p = m_printers[size_t(row)];
        int selected = -1;
        auto& bundle = *wxGetApp().preset_bundle;
        const bool known_model = std::any_of(bundle.printers.begin(), bundle.printers.end(), [&p, &bundle](const auto& preset) {
            return preset_matches_discovered_model(p, preset, bundle);
        });
        for (auto& preset : bundle.printers) {
            if (preset.is_default || preset.printer_technology() != ptFFF || !preset.is_system) continue;
            const bool match = preset_matches_discovered_model(p, preset, bundle);
            if (!match && known_model && p.vendor != "flashforge") continue;
            // Unidentified machines need an explicit choice; never select the nearest model.
            if (!match && preset.vendor && !boost::icontains(preset.vendor->name, p.vendor) &&
                p.vendor != "moonraker" && p.vendor != "octoprint") continue;
            m_preset_names.push_back(preset.name);
            const int index = int(m_preset_names.size()) - 1;
            m_presets->Append(from_u8(preset.name));
            if (preset.name == p.preset && !(p.vendor == "flashforge" && p.model == "Flashforge")) selected = index;
            else if (selected < 0 && match && p.vendor != "elegoo" && preset.config.opt_string("printer_variant") == "0.4") selected = index;
        }
        m_presets->Enable(!m_preset_names.empty());
        m_presets->SetSelection(selected);
        if (selected < 0) m_presets->SetLabel(_L("Choose printer and nozzle"));
        const bool needs_code = p.vendor == "bambu" || p.vendor == "flashforge" || p.vendor == "elegoo";
        const bool has_code = !wxGetApp().app_config->get(code_section, p.key()).empty();
        const bool code_refused = device_links.count(p.key()) && device_links.at(p.key()).result.status == PrinterLinkStatus::refused;
        m_code_label->SetLabel(p.vendor == "flashforge" ? _L("Check code") :
            p.vendor == "elegoo" && !discovered_printer_requires_code(p) ? _L("Access code (optional)") : _L("Access code"));
        m_code->SetName(m_code_label->GetLabel());
        m_code_label->Show(needs_code && (!has_code || code_refused));
        m_code->Show(needs_code && (!has_code || code_refused));
        m_code->Enable(needs_code && (!has_code || code_refused));
        m_add->SetLabel(p.preset.empty() ? _L("Add printer") : _L("Use printer"));
        m_hint->SetLabel(!p.local_control ? (p.vendor != "elegoo" ? _L("Enable LAN mode on the printer, then scan again.") :
                        !p.lan_mode.has_value() ? _L("Check that LAN-only mode is enabled on the printer, then scan again.") :
                        _L("Enable LAN-only mode on the printer when it is idle, then scan again.")) :
                        m_preset_names.empty() ? _L("Install this printer's profiles from Printer settings, then try again.") :
                        discovered_printer_requires_code(p) && !has_code ? _L("Enter the code shown in the printer's Network settings. Choose the actual printer model and installed nozzle above.") :
                                                 _L("Choose the actual printer model and installed nozzle before adding."));
        m_hint->Wrap(FromDIP(640));
        Layout();
        update_add();
    }
    void update_add()
    {
        const long row = m_list->GetFirstSelected();
        if (row < 0) { m_add->Disable(); return; }
        const auto& p = m_printers[size_t(row)];
        const bool requires_code = discovered_printer_requires_code(p);
        const auto code = boost::trim_copy(into_u8(m_code->GetValue()));
        m_add->Enable(p.local_control && m_presets->GetSelection() >= 0 &&
                      (!requires_code || !wxGetApp().app_config->get(code_section, p.key()).empty() ||
                       (p.vendor == "bambu" ? code.size() == 8 : !code.empty())));
    }
    void add_by_address();
    void add();
    void keep_open_after_add()
    {
        // Loading the printer settings can activate the main frame. Restore
        // this modal after those queued UI updates, so another device can be added.
        CallAfter([this] {
            if (IsModal() && IsShown()) {
                Raise();
                m_list->SetFocus();
            }
        });
    }
};

void PrinterScanDialog::add_by_address()
{
    wxDialog dialog(this, wxID_ANY, _L("Add by address"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(&dialog, wxID_ANY, _L("Printer address")), 0, wxALL, FromDIP(12));
    auto* address = new wxTextCtrl(&dialog, wxID_ANY);
    address->SetName(_L("Printer address"));
    address->SetHint(_L("192.168.1.100"));
    sizer->Add(address, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto* vendor = new wxChoice(&dialog, wxID_ANY);
    for (const auto& label : {_L("Bambu Lab"), _L("Snapmaker"), _L("Flashforge"), _L("Creality"), _L("Klipper / Moonraker"), _L("OctoPrint"), _L("Elegoo Centauri")})
        vendor->Append(label);
    vendor->SetName(_L("Connection"));
    vendor->SetSelection(4);
    sizer->Add(vendor, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* serial = new wxTextCtrl(&dialog, wxID_ANY);
    serial->SetName(_L("Serial number"));
    serial->SetHint(_L("Printer serial number, if available"));
    sizer->Add(serial, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* buttons = dialog.CreateButtonSizer(wxOK | wxCANCEL);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.SetSizerAndFit(sizer);
    dialog.SetMinSize(FromDIP(wxSize(400, -1)));
    dialog.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() != wxID_OK) return;
    static const std::vector<std::string> vendors{"bambu", "snapmaker", "flashforge", "creality", "moonraker", "octoprint", "elegoo"};
    const auto name = vendors[size_t(vendor->GetSelection())];
    DiscoveredPrinter p{name, into_u8(vendor->GetStringSelection()), "", into_u8(serial->GetValue()),
                        boost::trim_copy(into_u8(address->GetValue())), name == "moonraker" ? 7125u : 0u, true, ""};
    if (!DiscoveredPrinter::deserialize(p.serialize()) || (name == "bambu" && p.serial.empty())) {
        m_hint->SetLabel(_L("Enter a valid IP address. Bambu printers also need their serial number."));
        return;
    }
    receive(std::move(p));
}

void PrinterScanDialog::add()
{
    auto* plater = wxGetApp().plater();
    if (!plater || plater->is_background_process_slicing()) {
        m_hint->SetLabel(_L("Wait for slicing to finish, then add the printer."));
        return;
    }
    const long row = m_list->GetFirstSelected();
    const int choice = m_presets->GetSelection();
    if (row < 0 || choice < 0) return;
    auto p = m_printers[size_t(row)];
    const auto code = boost::trim_copy(into_u8(m_code->GetValue()));
    if (!p.local_control || (discovered_printer_requires_code(p) &&
        wxGetApp().app_config->get(code_section, p.key()).empty() &&
        (p.vendor == "bambu" ? code.size() != 8 : code.empty()))) {
        update_add();
        return;
    }
    // Importing a legacy project can reset the plate mode. Adding a device
    // explicitly opts that project into independently configured beds again.
    if (!plater->get_partplate_list().independent_setups()) plater->enable_independent_plates();
    const int own_plate = device_plate(p);
    if (own_plate >= 0) {
        // A portable project can remember this printer before it is saved locally.
        // Restore the connection without replacing that plate's edited print setup.
        p.preset = m_preset_names[size_t(choice)];
        if (p.vendor == "flashforge" || p.vendor == "elegoo") {
            const auto* chosen = wxGetApp().preset_bundle->printers.find_preset(p.preset);
            if (!chosen) return;
            p.model = chosen->config.opt_string("printer_model");
        }
        wxGetApp().app_config->set(saved_section, p.key(), p.serialize());
        if (p.vendor == "elegoo" && !p.serial.empty()) wxGetApp().app_config->set("dev_sn", device_id(p), p.serial);
        wxGetApp().app_config->set("last_scanned_printer", p.key());
        if (!code.empty()) save_printer_code(p.key(), code);
        wxGetApp().app_config->save();
        auto& plates = plater->get_partplate_list();
        for (const int index : device_plates(p))
            plates.get_plate(index)->print_setup().destination_name = p.name;
        if (plates.get_curr_plate_index() == own_plate)
            plater->activate_plate_setup();
        else
            plates.select_plate(own_plate);
        plater->set_plater_dirty(true);
        m_printers[size_t(row)] = p;
        m_list->SetItem(row, 3, _L("Added"));
        m_hint->SetLabel(_L("Printer saved. Check this plate's printer and nozzle, then slice it again before sending."));
        keep_open_after_add();
        return;
    }
    p.preset = m_preset_names[size_t(choice)];
    auto& bundle = *wxGetApp().preset_bundle;
    auto* preset = bundle.printers.find_preset(p.preset);
    if (!preset) return;
    if (p.vendor == "flashforge" || p.vendor == "elegoo") p.model = preset->config.opt_string("printer_model");
    plater->capture_plate_setup();
    plater->take_snapshot(_u8L("Add printer"));
    auto& plates = plater->get_partplate_list();
    const bool new_plate = (plates.get_plate_count() != 1 ||
                            !plates.get_curr_plate()->empty() || !plates.get_curr_plate()->print_setup().destination.empty());
    if (new_plate) {
        const int index = plater->get_partplate_list().create_plate(false);
        if (index < 0) { m_hint->SetLabel(_L("Could not create another plate.")); return; }
        plater->get_partplate_list().select_plate(index);
    }
    // Enable this exact model/nozzle in the existing profile catalog.
    preset->is_visible = true;
    if (preset->vendor)
        wxGetApp().app_config->set_variant(preset->vendor->id, preset->config.opt_string("printer_model"),
                                          preset->config.opt_string("printer_variant"), true);
    bool selected = false;
    plater->update_objects_position_when_select_preset([&] {
        // The outgoing settings already belong to their plate. Choosing a new
        // device must not open the library's save/discard-preset dialog.
        bundle.physical_printers.unselect_printer();
        selected = bundle.printers.select_preset_by_name(p.preset, true);
        if (selected) bundle.update_compatible(PresetSelectCompatibleType::Always);
    });
    if (!selected) { m_hint->SetLabel(_L("Could not select the printer. Try again.")); return; }
    if (!code.empty()) save_printer_code(p.key(), code);
    wxGetApp().app_config->set(saved_section, p.key(), p.serialize());
    if (p.vendor == "elegoo" && !p.serial.empty()) wxGetApp().app_config->set("dev_sn", device_id(p), p.serial);
    apply_connection(p, bundle.printers.get_edited_preset().config);
    wxGetApp().load_current_presets(false, false);
    ensure_scanned_printer(project_destination(p));
    const auto id = device_id(p);
    plater->sidebar().printer_combox()->set_selected_dev_id(id);
    if (auto* devices = wxGetApp().getDeviceManager()) devices->set_selected_machine(id);
    plater->on_config_change(bundle.full_config());
    auto* plate = plater->get_partplate_list().get_curr_plate();
    plate->set_plate_name(p.name);
    plater->capture_plate_setup();
    plater->set_plater_dirty(true);
    wxGetApp().app_config->set("last_scanned_printer", p.key());
    wxGetApp().app_config->save();
    m_printers[size_t(row)] = p;
    m_list->SetItem(row, 3, _L("Added"));
    select();
    m_hint->SetLabel(_L("Added. Its plate is ready. You can add another printer."));
    keep_open_after_add();
}
} // namespace

void show_printer_scan(wxWindow* parent)
{
    PrinterScanDialog dialog(parent);
    dialog.ShowModal();
}

std::vector<SavedPrinterChoice> saved_printer_choices()
{
    std::vector<SavedPrinterChoice> result;
    for (const auto& p : saved_printers())
        result.push_back({project_destination(p), p.name, p.model.empty() ? std::string() : into_u8(model_label(p)), p.preset,
                          into_u8(printer_status_label(p)), p.vendor, p.model});
    return result;
}

bool saved_printer_matches_connection(const std::string& destination, const DynamicPrintConfig& config)
{
    const auto printer = saved_printer(destination);
    return printer && !device_id(*printer).empty() && config.opt_string("print_host") == device_id(*printer);
}

bool saved_printer_available_for_planning(const std::string& destination)
{
    const auto printer = saved_printer(destination);
    if (!printer) return false;
    if (auto* devices = wxGetApp().getDeviceManager()) {
        auto* selected = devices->get_selected_machine();
        auto* machine = selected && selected->get_dev_id() == device_id(*printer)
            ? selected : devices->get_local_machine(device_id(*printer));
        if (machine) {
            if (machine->is_connected())
                return !machine->is_connecting() && !machine->is_in_printing() && !machine->is_in_printing_pause();
            if (machine == selected) return false;
        }
    }
    if (auto* agent = wxGetApp().getAgent(); agent && agent->has_printer_session(device_id(*printer))) return false;
    const auto link = device_links.find(printer->key());
    return link != device_links.end() && !link->second.checking && link->second.restore_error.empty() &&
        link->second.result.status == PrinterLinkStatus::connected;
}

SavedPrinterStatus saved_printer_status(const std::string& destination)
{
    using Connection = SavedPrinterStatus::Connection;
    const auto printer = saved_printer(destination);
    if (!printer) return {into_u8(_L("Offline")), Connection::disconnected};
    if (auto* agent = wxGetApp().getAgent()) {
        if (const auto token = agent->printer_session_token(device_id(*printer))) {
            auto* devices = wxGetApp().getDeviceManager();
            auto* machine = devices ? devices->get_local_machine(device_id(*printer)) : nullptr;
            if (token->state != PrinterControlState::connected || !machine || !machine->is_connected())
                return {into_u8(printer_status_label(*printer)),
                    token->state == PrinterControlState::offline || token->state == PrinterControlState::rejected
                        ? Connection::disconnected : Connection::unknown};
        }
    }
    if (auto* devices = wxGetApp().getDeviceManager()) {
        auto* selected = devices->get_selected_machine();
        auto* machine = selected && selected->get_dev_id() == device_id(*printer)
            ? selected : devices->get_local_machine(device_id(*printer));
        if (machine) {
            if (machine->is_connected()) {
                if (machine->is_connecting())
                    return {into_u8(_L("Connecting…")), Connection::unknown};
                const auto label = machine->is_in_printing_pause() ? _L("Paused") : machine->is_in_printing()
                    ? wxString::Format(_L("Printing %d%%"), machine->mc_print_percent) : _L("Connected");
                return {into_u8(label), Connection::connected};
            }
            // The selected control connection is authoritative, even if an older
            // periodic check succeeded before this connection was lost.
            if (machine == selected) {
                const auto link = device_links.find(printer->key());
                if (link != device_links.end() && link->second.result.failure != PrinterLinkFailure::none)
                    return {into_u8(connection_label(*printer)), Connection::disconnected};
                return {into_u8(_L("Waiting for printer status")), Connection::unknown};
            }
        }
    }
    const auto link = device_links.find(printer->key());
    if (link == device_links.end()) return {into_u8(_L("Saved")), Connection::unknown};
    const auto label = into_u8(connection_label(*printer));
    if (!link->second.restore_error.empty()) return {label, Connection::disconnected};
    if (link->second.checking) return {label, Connection::unknown};
    return {label, link->second.result.status == PrinterLinkStatus::connected ? Connection::unknown : Connection::disconnected};
}

namespace {
bool apply_saved_printer(const DiscoveredPrinter& p)
{
    auto* plater = wxGetApp().plater();
    auto& bundle = *wxGetApp().preset_bundle;
    auto* preset = bundle.printers.find_preset(p.preset);
    if (!plater || !preset || preset->printer_technology() != ptFFF) return false;
    const std::string preset_name = preset->name;
    const auto destination = project_destination(p);
    // Reusing the same device retains edited settings when its model and nozzle still match.
    // An explicit choice of that device also repairs a plate carrying another printer's preset.
    const auto& active_config = bundle.printers.get_edited_preset().config;
    const bool different_setup = active_config.opt_string("printer_model") != preset->config.opt_string("printer_model") ||
                                 active_config.opt_string("printer_variant") != preset->config.opt_string("printer_variant");
    if (plater->get_partplate_list().get_curr_plate()->print_setup().destination != destination || different_setup) {
        preset->is_visible = true;
        if (preset->vendor)
            wxGetApp().app_config->set_variant(preset->vendor->id, preset->config.opt_string("printer_model"),
                                              preset->config.opt_string("printer_variant"), true);
        bool selected = false;
        plater->update_objects_position_when_select_preset([&] {
            bundle.physical_printers.unselect_printer();
            bundle.printers.select_preset_by_name(preset_name, true);
            selected = bundle.printers.get_selected_preset_name() == preset_name;
            if (selected) bundle.update_compatible(PresetSelectCompatibleType::Always);
        });
        if (!selected) return false;
    }
    apply_connection(p, bundle.printers.get_edited_preset().config);
    // Set the intended device before preset-loading callbacks capture the job.
    plater->sidebar().printer_combox()->set_selected_dev_id(device_id(p));
    {
        auto& setup = plater->get_partplate_list().get_curr_plate()->print_setup();
        setup.destination = destination;
        setup.destination_name = p.name;
    }
    wxGetApp().load_current_presets(false, false);
    ensure_scanned_printer(destination);
    plater->sidebar().printer_combox()->set_selected_dev_id(device_id(p));
    if (auto* devices = wxGetApp().getDeviceManager()) devices->set_selected_machine(device_id(p));
    plater->on_config_change(bundle.full_config());
    plater->capture_plate_setup();
    plater->set_plater_dirty(true);
    wxGetApp().app_config->set("last_scanned_printer", p.key());
    wxGetApp().app_config->save();
    return true;
}
}

bool assign_saved_printer(const std::string& destination)
{
    const auto p = saved_printer(destination);
    auto* plater = wxGetApp().plater();
    const auto* preset = p ? wxGetApp().preset_bundle->printers.find_preset(p->preset) : nullptr;
    if (!plater || !preset || preset->printer_technology() != ptFFF ||
        wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || control_config ||
        plater->is_background_process_slicing()) return false;
    Plater::SingleSnapshot single_snapshot(plater);
    if (!plater->get_partplate_list().independent_setups()) plater->enable_independent_plates();
    plater->capture_plate_setup();
    plater->take_snapshot(_u8L("Choose printer"));
    return apply_saved_printer(*p);
}

void clear_saved_printer_assignment()
{
    auto* plater = wxGetApp().plater();
    if (!plater || control_config || plater->is_background_process_slicing()) return;
    plater->sidebar().printer_combox()->set_selected_dev_id("");
    auto& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    for (const auto* key : {"print_host", "print_host_webui", "printhost_port", "printhost_apikey",
                            "printhost_user", "printhost_password", "printhost_cafile", "flashforge_serial_number"})
        config.set_key_value(key, new ConfigOptionString());
    auto& setup = plater->get_partplate_list().get_curr_plate()->print_setup();
    setup.destination.clear();
    setup.destination_name.clear();
    if (auto* devices = wxGetApp().getDeviceManager()) devices->set_selected_machine("");
    plater->capture_plate_setup();
    plater->set_plater_dirty(true);
}

int create_saved_printer_plate(const std::string& destination)
{
    const auto p = saved_printer(destination);
    auto* plater = wxGetApp().plater();
    const auto* preset = p ? wxGetApp().preset_bundle->printers.find_preset(p->preset) : nullptr;
    if (!plater || !preset || preset->printer_technology() != ptFFF ||
        wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || control_config ||
        plater->is_background_process_slicing()) return -1;
    Plater::SingleSnapshot single_snapshot(plater);
    auto& plates = plater->get_partplate_list();
    if (!plates.independent_setups()) plater->enable_independent_plates();
    if (!plater->prepare_plate_setup_switch(plates.get_plate_count())) return -1;
    plater->capture_plate_setup();
    plater->take_snapshot(_u8L("New plate for printer"));
    const int index = plates.create_plate(false);
    if (index < 0) return -1;
    const int previous = plates.get_curr_plate_index();
    if (plates.select_plate(index) < 0 || !apply_saved_printer(*p)) {
        plates.select_plate(previous);
        plates.delete_plate(index);
        return -1;
    }
    plates.get_plate(index)->set_plate_name(p->name);
    return index;
}

std::string scanned_printer_destination(const std::string& id)
{
    for (const auto& p : saved_printers())
        if (device_id(p) == id) return project_destination(p);
    return portable_printer_destination(id, *wxGetApp().app_config);
}

std::string restore_scanned_printer(const std::string& destination, DynamicPrintConfig& config)
{
    if (auto p = saved_printer(destination)) {
        apply_connection(*p, config);
        return device_id(*p);
    }
    if (boost::starts_with(destination, destination_prefix)) return {};
    return local_printer_destination(destination, *wxGetApp().app_config);
}

std::string scanned_printer_name(const std::string& destination)
{
    const auto printer = saved_printer(destination);
    return printer ? printer->name : std::string();
}

bool missing_scanned_printer(const std::string& destination)
{
    if (boost::starts_with(destination, destination_prefix)) return !saved_printer(destination);
    if (!boost::starts_with(destination, "scanned-id:") || saved_printer(destination)) return false;
    const auto local = local_printer_destination(destination, *wxGetApp().app_config);
    auto* devices = wxGetApp().getDeviceManager();
    return local.empty() || !devices || (!devices->get_local_machine(local) && !devices->get_my_machine(local));
}

void ensure_scanned_printer(const std::string& destination)
{
    const auto p = saved_printer(destination);
    auto* devices = wxGetApp().getDeviceManager();
    if (!p || p->vendor != "bambu" || !devices) return;
    BBLocalMachine machine;
    machine.dev_name = p->name;
    machine.dev_id = p->serial;
    machine.dev_ip = p->address;
    machine.printer_type = p->model;
    machine.printer_agent_id = "bbl";
    if (auto* obj = devices->insert_local_device(machine, "lan", "free", "", wxGetApp().app_config->get(code_section, p->key()))) {
        obj->set_dev_ip(p->address);
        obj->set_dev_name(p->name);
    }
}

void start_scanned_printer_connections(wxWindow* owner)
{
    // Parent ownership stops callbacks and polling when the application window closes.
    new PrinterConnections(owner);
}

DynamicPrintConfig* scanned_printer_control_config()
{
    return control_config ? &*control_config : nullptr;
}

std::string scanned_printer_control_device_id()
{
    return control_config ? control_device_id : std::string();
}

bool saved_printer_connection_params(const std::string& id, PrinterConnectionParams& params)
{
    for (const auto& printer : saved_printer_list()) {
        if (device_id(printer) != id) continue;
        const auto* preset = wxGetApp().preset_bundle->printers.find_preset(printer.preset);
        if (!preset) return false;
        params = {};
        params.dev_id = id;
        params.host = printer.address;
        if (printer.vendor != "bambu") params.host = Http::get_host_from_url(device_id(printer), &params.port);
        params.username = printer.vendor == "bambu" ? "bblp" : "";
        // No saved code means no credential: an empty key sends no X-Api-Key header.
        params.password = wxGetApp().app_config->get(code_section, printer.key());
        params.model_name = preset->config.opt_string("printer_model");
        for (const auto& vendor : wxGetApp().preset_bundle->vendors)
            for (const auto& model : vendor.second.models)
                if (model.name == params.model_name) params.model_id = model.model_id;
        return true;
    }
    return false;
}

wxPanel* create_scanned_devices_page(wxWindow* parent, LazyPage<MonitorPanel>* monitor)
{
    class DevicesPage final : public wxPanel {
        wxDataViewListCtrl* m_list;
        wxPanel* m_sidebar;
        wxStaticText *m_summary, *m_name, *m_info, *m_message, *m_connection_detail;
        wxStaticText* m_plate_hint;
        Button *m_open, *m_new_plate, *m_open_controls, *m_add, *m_reconnect, *m_manage, *m_setup;
        ::ComboBox* m_plate_choices;
        std::vector<int> m_plate_indices;
        std::vector<int> m_plate_print_indices;
        ::ComboBox* m_view;
        bool m_native = false;
        int m_native_tabs = 0;
        std::map<std::string, std::pair<PrinterWebView*, std::string>> m_web_views;
        wxPanel *m_detail, *m_empty;
        wxPanel* m_controls;
        LazyPage<MonitorPanel>* m_monitor;
        PrinterWebView* m_web = nullptr;
        wxTimer m_timer;
        std::vector<DiscoveredPrinter> m_printers;
        std::string m_selected, m_loaded, m_open_elegoo;
        bool m_refreshing = false;
        bool m_adding = false;
    public:
        DevicesPage(wxWindow* parent, LazyPage<MonitorPanel>* monitor)
            : wxPanel(parent), m_monitor(monitor), m_timer(this)
        {
            SetFont(wxGetApp().normal_font());
            auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                  wxSP_LIVE_UPDATE | wxBORDER_NONE);
            splitter->SetMinimumPaneSize(FromDIP(260));
            splitter->SetSashGravity(0);
            auto* sidebar = m_sidebar = new wxPanel(splitter);
            auto* left = new wxBoxSizer(wxVERTICAL);
            auto* title = new wxStaticText(sidebar, wxID_ANY, _L("Printers"));
            title->SetFont(DesignSystem::font(DesignSystem::TypeStyle::Title));
            left->Add(title, 0, wxALL, FromDIP(16));
            auto* add = m_add = new Button(sidebar, _L("Add printer…"));
            add->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            left->Add(add, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
            m_list = new wxDataViewListCtrl(sidebar, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                            wxDV_SINGLE | wxBORDER_NONE);
            m_list->SetName(_L("Saved printers"));
            m_list->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, FromDIP(180), wxALIGN_LEFT, 0);
            m_list->AppendTextColumn(_L("Status"), wxDATAVIEW_CELL_INERT, FromDIP(120), wxALIGN_LEFT, 0);
            m_list->SetRowHeight(FromDIP(40));
            m_list->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
                const int width = std::max(FromDIP(120), m_list->GetClientSize().x - FromDIP(136));
                if (m_list->GetColumn(0)->GetWidth() != width) m_list->GetColumn(0)->SetWidth(width);
                event.Skip();
            });
            m_list->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
                wxDataViewItem item;
                wxDataViewColumn* column = nullptr;
                m_list->HitTest(event.GetPosition(), item, column);
                const int row = m_list->ItemToRow(item);
                if (row >= 0 && size_t(row) < m_printers.size())
                    m_list->SetToolTip(from_u8(m_printers[size_t(row)].name) + "\n" + printer_status_label(m_printers[size_t(row)]));
                else m_list->UnsetToolTip();
                event.Skip();
            });
            left->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
            m_summary = new wxStaticText(sidebar, wxID_ANY, "");
            m_summary->SetFont(DesignSystem::font(DesignSystem::TypeStyle::Caption));
            left->Add(m_summary, 0, wxALL, FromDIP(16));
            sidebar->SetSizer(left);

            auto* detail = m_detail = new wxPanel(splitter);
            auto* right = new wxBoxSizer(wxVERTICAL);
            auto* heading = new wxBoxSizer(wxHORIZONTAL);
            auto* identity = new wxBoxSizer(wxVERTICAL);
            m_name = new wxStaticText(detail, wxID_ANY, _L("Select a printer"), wxDefaultPosition,
                                      wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
            m_name->SetMinSize(wxSize(0, -1));
            m_name->SetFont(DesignSystem::font(DesignSystem::TypeStyle::Title));
            identity->Add(m_name, 0, wxEXPAND);
            m_info = new wxStaticText(detail, wxID_ANY, "", wxDefaultPosition,
                                      wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
            m_info->SetMinSize(wxSize(0, -1));
            identity->Add(m_info, 0, wxEXPAND | wxTOP, FromDIP(4));
            heading->Add(identity, 1, wxEXPAND | wxRIGHT, FromDIP(16));
            m_manage = new Button(detail, _L("Manage printer…"));
            m_manage->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            heading->Add(m_manage, 0, wxALIGN_TOP);
            right->Add(heading, 0, wxEXPAND | wxALL, FromDIP(20));
            m_connection_detail = new wxStaticText(detail, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
            m_connection_detail->SetName(_L("Connection status"));
            auto* actions = new wxBoxSizer(wxHORIZONTAL);
            actions->Add(m_connection_detail, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            m_reconnect = new Button(detail, _L("Reconnect"));
            m_reconnect->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            m_view = new ::ComboBox(detail, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                      FromDIP(wxSize(210, -1)), 0, nullptr, wxCB_READONLY);
            m_view->SetName(_L("Printer view"));
            for (const auto& label : {_L("Status and controls"), _L("Storage"), _L("Firmware"), _L("Troubleshooting")})
                m_view->Append(label);
            m_view->SetSelection(0);
            m_view->Hide();
            actions->Add(m_view, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            actions->Add(m_reconnect, 0, wxALIGN_CENTER_VERTICAL);
            right->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
            m_empty = new wxPanel(detail);
            auto* empty = new wxBoxSizer(wxVERTICAL);
            empty->AddStretchSpacer();
            m_message = new wxStaticText(m_empty, wxID_ANY, _L("Select a printer to see its status and controls."));
            empty->Add(m_message, 0, wxALIGN_CENTER | wxALL, FromDIP(16));
            m_setup = new Button(m_empty, _L("Set up controls…"));
            m_setup->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            empty->Add(m_setup, 0, wxALIGN_CENTER | wxALL, FromDIP(8));
            m_open_controls = new Button(m_empty, _L("Open printer controls"));
            m_open_controls->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            m_open_controls->Hide();
            empty->Add(m_open_controls, 0, wxALIGN_CENTER | wxALL, FromDIP(8));
            m_open_controls->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip(); m_open_elegoo = m_selected; inspect();
            });
            empty->AddStretchSpacer();
            m_empty->SetSizer(empty);
            right->Add(m_empty, 1, wxEXPAND);
            // Each native page (and the website) owns its scrolling. Wrapping it
            // in another scroll window traps wheel input and enlarges the page.
            m_controls = new wxPanel(detail);
            auto* controls = new wxBoxSizer(wxVERTICAL);
            m_monitor->Reparent(m_controls);
            controls->Add(m_monitor, 1, wxEXPAND);
            m_controls->SetSizer(controls);
            m_controls->Hide();
            right->Add(m_controls, 1, wxEXPAND);
            auto* jobs = new wxBoxSizer(wxHORIZONTAL);
            m_plate_hint = new wxStaticText(detail, wxID_ANY, _L("No plates for this printer yet."));
            jobs->Add(m_plate_hint, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            m_plate_choices = new ::ComboBox(detail, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                            FromDIP(wxSize(180, -1)), 0, nullptr, wxCB_READONLY);
            m_plate_choices->SetName(_L("Plates for this printer"));
            jobs->Add(m_plate_choices, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            m_open = new Button(detail, _L("Open plate"));
            m_open->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            m_new_plate = new Button(detail, _L("New plate"));
            m_new_plate->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            jobs->Add(m_open, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            jobs->Add(m_new_plate, 0, wxALIGN_CENTER_VERTICAL);
            right->Add(jobs, 0, wxEXPAND | wxALL, FromDIP(20));
            detail->SetSizer(right);
            splitter->SplitVertically(sidebar, detail, FromDIP(320));
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            sizer->Add(splitter, 1, wxEXPAND);
            SetSizer(sizer);

            add->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip();
                m_adding = true;
                stop_inspecting();
                show_printer_scan(this);
                m_adding = false;
                m_selected = wxGetApp().app_config->get("last_scanned_printer");
                refresh();
                inspect();
            });
            m_reconnect->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip();
                if (const auto p = selected()) {
                    auto& link = device_links[p->key()];
                    link.restore_error.clear();
                    if (auto* agent = wxGetApp().getAgent()) agent->forget_printer_session(device_id(*p));
                    link.checking = true;
                    link.next_check = {};
                    forget_web(p->key());
                    m_loaded.clear();
                    inspect();
                }
            });
            m_open->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { event.Skip(); open_plate(); });
            m_new_plate->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip();
                const auto p = selected();
                if (!p) return;
                stop_inspecting();
                if (create_saved_printer_plate(project_destination(*p)) >= 0)
                    wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
                else inspect();
                refresh();
            });
            m_manage->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip();
                wxMenu menu;
                menu.Append(wxID_EDIT, _L("Edit printer…"));
                menu.AppendSeparator();
                menu.Append(wxID_REMOVE, _L("Remove printer…"));
                menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
                    if (e.GetId() == wxID_EDIT) edit(); else remove();
                });
                m_manage->PopupMenu(&menu);
            });
            m_setup->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
                event.Skip();
                wxGetApp().show_network_plugin_download_dialog();
                m_loaded.clear();
                inspect();
            });
            m_view->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { show_view(); });
            m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) {
                const int row = m_list->GetSelectedRow();
                if (m_refreshing || row < 0 || size_t(row) >= m_printers.size()) return;
                m_selected = m_printers[size_t(row)].key();
                wxGetApp().app_config->set("last_scanned_printer", m_selected);
                inspect();
                refresh();
            });
            Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
                refresh();
                if (IsShownOnScreen()) inspect();
            });
            Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
                for (auto* button : {m_add, m_open, m_new_plate, m_open_controls, m_reconnect, m_manage, m_setup})
                    button->Rescale();
                m_list->SetRowHeight(FromDIP(40));
                m_list->GetColumn(1)->SetWidth(FromDIP(120));
                m_plate_choices->Rescale();
                m_view->Rescale();
                m_view->SetMinSize(FromDIP(wxSize(210, -1)));
                m_plate_choices->SetMinSize(FromDIP(wxSize(180, -1)));
                layout_detail();
                event.Skip();
            });
            Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event) {
                apply_theme();
                event.Skip();
            });
            m_detail->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
                wrap_status();
                event.Skip();
            });
            m_selected = wxGetApp().app_config->get("last_scanned_printer");
            apply_theme();
            m_timer.Start(1000);
            refresh();
        }
        ~DevicesPage() override { m_timer.Stop(); control_config.reset(); }
        bool Show(bool show = true) override
        {
            const bool changed = wxPanel::Show(show);
            if (show) { refresh(); inspect(); }
            else { m_open_elegoo.clear(); stop_inspecting(true, false); m_controls->Hide(); }
            return changed;
        }
    private:
        void apply_theme()
        {
            using Role = DesignSystem::ColorRole;
            const auto color = [](Role role) {
                return wxColour(DesignSystem::color_hex(role, wxGetApp().dark_mode()));
            };
            wxGetApp().UpdateDarkUIWin(this);
            SetBackgroundColour(color(Role::Bg));
            m_sidebar->SetBackgroundColour(color(Role::Surface));
            for (auto* panel : {m_detail, m_empty, m_controls}) panel->SetBackgroundColour(color(Role::Bg));
            wxGetApp().UpdateDVCDarkUI(m_list);
            m_list->SetBackgroundColour(color(Role::Surface));
            m_list->SetForegroundColour(color(Role::Ink));
            m_name->SetForegroundColour(color(Role::Ink));
            for (auto* label : {m_summary, m_info, m_plate_hint}) label->SetForegroundColour(color(Role::InkMuted));
            for (auto* label : {m_connection_detail, m_message}) label->SetForegroundColour(color(Role::Ink));
            Refresh();
        }
        void wrap_status()
        {
            const int width = m_detail->GetClientSize().x;
            if (width <= 0) return;
            m_connection_detail->Wrap(-1);
            m_connection_detail->Wrap(std::max(FromDIP(120), width - FromDIP(m_view->IsShown() ? 390 : 170)));
        }
        void layout_detail()
        {
            Layout();
            // Showing a child doesn't resize the splitter pane. Relayout its own
            // sizer so the view selector and previously hidden controls get space.
            wrap_status();
            m_detail->Layout();
            if (m_empty->IsShown()) m_empty->Layout();
            if (m_controls->IsShown()) m_controls->Layout();
        }
        std::optional<DiscoveredPrinter> selected() const
        {
            for (const auto& p : m_printers) if (p.key() == m_selected) return p;
            return std::nullopt;
        }
        void stop_inspecting(bool restore_plate = true, bool forget_view = true)
        {
            m_monitor->Hide();
            m_view->Hide();
            if (m_web) m_web->Hide();
            if (forget_view) m_loaded.clear();
            if (!control_config) return;
            // Change the view while sidebar synchronization is still suppressed.
            // Retained sessions keep their own transports and telemetry.
            if (auto* dev = wxGetApp().getDeviceManager()) dev->set_selected_machine("");
            control_config.reset();
            control_device_id.clear();
            if (!restore_plate || wxGetApp().is_closing()) return;
            wxGetApp().switch_printer_agent();
            const auto id = wxGetApp().sidebar().printer_combox()->get_selected_dev_id();
            const auto destination = scanned_printer_destination(id);
            ensure_scanned_printer(destination);
            if (auto* dev = wxGetApp().getDeviceManager()) {
                if (id.empty() || (dev->get_selected_machine() && dev->get_selected_machine()->get_dev_id() == id)) return;
                dev->set_selected_machine(id);
            }
        }
        void inspect()
        {
            if (!IsShownOnScreen() || m_adding) return;
            m_open_controls->Hide();
            if (m_open_elegoo != m_selected) m_open_elegoo.clear();
            const auto p = selected();
            if (!p) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Hide();
                m_message->SetLabel(_L("Add a printer, then select it to see its status and controls."));
                layout_detail();
                return;
            }
            const auto* preset = wxGetApp().preset_bundle->printers.find_preset(p->preset);
            if (!preset) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Hide();
                m_message->SetLabel(_L("Install this printer's profiles to use its controls."));
                layout_detail();
                return;
            }
            DynamicPrintConfig config = preset->config;
            apply_connection(*p, config);
            const auto agent_id = config.opt_string("printer_agent");
            if (p->vendor == "elegoo" && (!p->local_control ||
                (discovered_printer_requires_code(*p) && wxGetApp().app_config->get(code_section, p->key()).empty()))) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Hide();
                m_message->SetLabel(p->local_control ? _L("Enter the printer's LAN access code in Edit printer to open its controls.") :
                    _L("Check that LAN-only mode is enabled on the printer, then scan again."));
                m_message->Wrap(-1); m_message->Wrap(FromDIP(440)); layout_detail(); return;
            }
            if (p->vendor == "elegoo" && config.opt_string("printer_model") == "Elegoo Centauri Carbon 2" &&
                wxGetApp().app_config->get("dev_sn", device_id(*p)).empty()) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Hide();
                m_message->SetLabel(_L("Waiting for printer identification before opening its controls. Check the saved address and access code if it stays unavailable."));
                m_message->Wrap(-1); m_message->Wrap(FromDIP(440)); layout_detail(); return;
            }
            if (p->vendor == "elegoo" && m_open_elegoo != m_selected) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Hide(); m_open_controls->Show();
                m_message->SetLabel(_L("Open this printer's controls when you need them. Leaving this view stops its camera and control page."));
                m_message->Wrap(-1); m_message->Wrap(FromDIP(440)); layout_detail(); return;
            }
            const bool needs_bambu = p->vendor == "bambu" && !NetworkAgent::is_network_module_loaded();
            const bool native = !needs_bambu && agent_id != "orca" && NetworkAgentFactory::is_printer_agent_registered(agent_id);
            const bool web = (p->vendor == "moonraker" || p->vendor == "octoprint" || p->vendor == "creality" || p->vendor == "snapmaker" || p->vendor == "elegoo");
            if (needs_bambu || (!native && !web)) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Show(needs_bambu);
                m_message->SetLabel(needs_bambu ? _L("Set up Bambu connection support to see the camera and use printer controls.") :
                                               _L("Controls aren't available for this printer yet. You can still open its plate."));
                m_message->Wrap(FromDIP(440));
                layout_detail();
                return;
            }
            // Display-name updates are not a transport or camera change.
            const std::string loaded = nlohmann::json{{"vendor", p->vendor}, {"address", p->address},
                {"port", p->port}, {"serial", p->serial}, {"preset", p->preset}, {"native", native},
                {"code", wxGetApp().app_config->get(code_section, p->key())},
                {"panel_serial", p->vendor == "elegoo" ? wxGetApp().app_config->get("dev_sn", device_id(*p)) : ""}}.dump();
            if (loaded == m_loaded && control_config) return;
            const bool same_view = loaded == m_loaded;
            const int previous_view = same_view ? m_view->GetSelection() : 0;
            stop_inspecting(false, false);
            m_native = native;
            m_native_tabs = native ? (p->vendor == "bambu" ? 4 : 1) : 0;
            control_config = std::move(config);
            control_device_id = device_id(*p);
            if (native) {
                wxGetApp().switch_printer_agent();
                auto* agent = wxGetApp().getAgent();
                if (!agent || !agent->get_printer_agent() || agent->get_printer_agent()->get_agent_info().id != agent_id) {
                    stop_inspecting();
                    m_empty->Show(); m_controls->Hide(); m_setup->Hide();
                    m_message->SetLabel(_L("Could not open this printer's controls. Try reconnecting."));
                    layout_detail();
                    return;
                }
                ensure_scanned_printer(std::string(destination_prefix) + p->key());
                if (auto* dev = wxGetApp().getDeviceManager()) {
                    const auto id = device_id(*p);
                    if (auto* obj = dev->get_local_machine(id)) { obj->set_dev_name(p->name); obj->set_dev_ip(p->vendor == "bambu" ? p->address : id); }
                    if (!dev->get_selected_machine() || dev->get_selected_machine()->get_dev_id() != id) dev->set_selected_machine(id);
                }
                m_monitor->Show();
                if (auto* panel = m_monitor->get()) {
                    if (!same_view) {
                        panel->set_default();
                        panel->get_status_panel()->Scroll(0, 0);
                    }
                    panel->update_all();
                }
            }
            m_web = nullptr;
            if (web) {
                const auto view = m_web_views.find(p->key());
                if (view != m_web_views.end()) m_web = view->second.first;
            }
            m_view->Clear();
            if (native) {
                m_view->Append(_L("Status and controls"));
                // The remaining native tabs use Bambu's storage/firmware/HMS
                // protocols. Other backends expose those operations on their website.
                if (m_native_tabs == 4)
                    for (const auto& label : {_L("Storage"), _L("Firmware"), _L("Troubleshooting")}) m_view->Append(label);
            }
            if (web) m_view->Append(_L("Printer website"));
            m_view->SetSelection(previous_view < int(m_view->GetCount()) ? previous_view : 0);
            m_view->Show(m_view->GetCount() > 1);
            m_empty->Hide(); m_controls->Show();
            m_loaded = loaded;
            show_view();
            layout_detail();
        }
        void forget_web(const std::string& key)
        {
            const auto it = m_web_views.find(key);
            if (it == m_web_views.end()) return;
            if (m_web == it->second.first) m_web = nullptr;
            m_controls->GetSizer()->Detach(it->second.first);
            it->second.first->Destroy();
            m_web_views.erase(it);
        }
        void show_view()
        {
            const int choice = m_view->GetSelection();
            const bool website = !m_native || choice >= m_native_tabs;
            m_monitor->Show(!website);
            if (m_web) m_web->Show(website);
            // Native monitoring must not open a hidden vendor website or camera.
            // Load it only when it is the view the user actually selected.
            if (website && control_config) {
                if (const auto printer = selected()) {
                    auto& view = m_web_views[printer->key()];
                    if (!view.first) {
                        view.first = new PrinterWebView(m_controls);
                        m_controls->GetSizer()->Add(view.first, 1, wxEXPAND);
                        wxGetApp().UpdateDarkUIWin(view.first);
                    }
                    m_web = view.first;
                    m_web->Show();
                    if (view.second != m_loaded) {
                        wxString url = from_u8(printer->vendor == "elegoo" ?
                            ElegooLink::get_print_host_webui(&*control_config) : "http://" +
                            (printer->vendor == "octoprint" ? device_id(*printer) : printer->address));
                        m_web->load_url(url, from_u8(wxGetApp().app_config->get(code_section, printer->key())), &*control_config);
                        view.second = m_loaded;
                    }
                }
            }
            if (!website) {
                if (auto* panel = m_monitor->get(); panel && choice >= 0 && choice < int(panel->get_tabpanel()->GetPageCount()))
                    panel->get_tabpanel()->SetSelection(choice);
            }
            layout_detail();
        }
        void open_plate()
        {
            const auto p = selected();
            if (!p) return;
            auto* plater = wxGetApp().plater();
            const int choice = m_plate_choices->GetSelection();
            const int print_index = choice >= 0 && size_t(choice) < m_plate_print_indices.size() ? m_plate_print_indices[size_t(choice)] : -1;
            const int plate = plater ? plater->get_partplate_list().find_plate_by_print_index(print_index) : -1;
            if (!plater || plate < 0 || plater->is_background_process_slicing()) return;
            if (plater->get_partplate_list().get_plate(plate)->print_setup().destination != project_destination(*p)) return;
            stop_inspecting();
            if (plater->get_partplate_list().select_plate(plate) >= 0)
                wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
        }
        void edit()
        {
            auto p = selected();
            if (!p) return;
            wxDialog dialog(this, wxID_ANY, _L("Edit printer"));
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            auto* fields = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
            fields->AddGrowableCol(1);
            const auto field = [&](const wxString& label, const wxString& value, long style = 0) {
                fields->Add(new wxStaticText(&dialog, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
                auto* input = new ::TextInput(&dialog, value, "", "", wxDefaultPosition, FromDIP(wxSize(270, -1)), style);
                auto* text = input->GetTextCtrl();
                text->SetName(label); fields->Add(input, 1, wxEXPAND); return text;
            };
            auto* name = field(_L("Printer name"), from_u8(p->name));
            fields->Add(new wxStaticText(&dialog, wxID_ANY, _L("Connection")), 0, wxALIGN_CENTER_VERTICAL);
            auto* connection = new ::ComboBox(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                FromDIP(wxSize(270, -1)), 0, nullptr, wxCB_READONLY);
            const std::vector<std::string> vendors{"bambu", "snapmaker", "flashforge", "creality", "moonraker", "octoprint", "elegoo"};
            for (const auto& label : {_L("Bambu Lab"), _L("Snapmaker"), _L("Flashforge"), _L("Creality"),
                                      _L("Klipper / Moonraker"), _L("OctoPrint"), _L("Elegoo Centauri")}) connection->Append(label);
            connection->SetName(_L("Connection"));
            const auto current_vendor = std::find(vendors.begin(), vendors.end(), p->vendor);
            connection->SetSelection(current_vendor == vendors.end() ? 4 : int(current_vendor - vendors.begin()));
            fields->Add(connection, 1, wxEXPAND);
            auto* port = field(_L("Port (blank uses the default)"), p->port ? std::to_string(p->port) : "");
            bool port_edited = false;
            port->Bind(wxEVT_TEXT, [&port_edited](wxCommandEvent& event) { port_edited = true; event.Skip(); });
            connection->Bind(wxEVT_COMBOBOX, [port, connection, vendors, old_vendor = p->vendor](wxCommandEvent&) {
                if (old_vendor == "moonraker" && vendors[size_t(connection->GetSelection())] == "elegoo" && port->GetValue() == "7125")
                    port->ChangeValue("");
            });
            fields->Add(new wxStaticText(&dialog, wxID_ANY, _L("Printer and nozzle")), 0, wxALIGN_CENTER_VERTICAL);
            auto* nozzle = new ::ComboBox(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                           FromDIP(wxSize(270, -1)), 0, nullptr, wxCB_READONLY);
            std::vector<std::string> nozzle_presets;
            const auto* current_preset = wxGetApp().preset_bundle->printers.find_preset(p->preset);
            for (const auto& candidate : wxGetApp().preset_bundle->printers) {
                if (!current_preset || !candidate.is_system || candidate.is_default ||
                    candidate.config.opt_string("printer_model") != current_preset->config.opt_string("printer_model")) continue;
                nozzle->Append(from_u8(candidate.name)); nozzle_presets.push_back(candidate.name);
                if (candidate.name == p->preset) nozzle->SetSelection(int(nozzle_presets.size()) - 1);
            }
            nozzle->SetName(_L("Printer and nozzle"));
            nozzle->Enable(p->vendor == "elegoo" && !nozzle_presets.empty());
            fields->Add(nozzle, 1, wxEXPAND);
            auto* serial = field(_L("Serial number"), from_u8(p->serial));
            auto* address = field(_L("IP address"), from_u8(p->address));
            auto* code = field(p->vendor == "flashforge" ? _L("Check code") : p->vendor == "octoprint" ? _L("API key") : _L("Access code"), "", wxTE_PASSWORD);
            code->SetHint(_L("Leave blank to keep the saved code"));
            sizer->Add(fields, 0, wxEXPAND | wxALL, FromDIP(20));
            auto* error = new wxStaticText(&dialog, wxID_ANY, "");
            sizer->Add(error, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
            sizer->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(20));
            dialog.FindWindow(wxID_OK)->SetLabel(_L("Save"));
            dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                auto updated = *p;
                if (nozzle->GetSelection() >= 0) updated.preset = nozzle_presets[size_t(nozzle->GetSelection())];
                const bool nozzle_changed = updated.preset != p->preset;
                const auto* nozzle_preset = wxGetApp().preset_bundle->printers.find_preset(updated.preset);
                const auto* preset = wxGetApp().preset_bundle->printers.find_preset(p->preset);
                if (!change_discovered_printer_connection(updated, vendors[size_t(connection->GetSelection())],
                        preset ? preset->config.opt_string("printer_model") : "")) {
                    error->SetLabel(_L("Choose a Centauri Carbon profile before using the Elegoo connection.")); dialog.Layout(); return;
                }
                const auto port_text = boost::trim_copy(into_u8(port->GetValue()));
                const auto old_port = p->port ? std::to_string(p->port) : "";
                if (port_edited || port_text != old_port) {
                    unsigned long number = 0;
                    if (!port_text.empty() && (!from_u8(port_text).ToULong(&number) || number > 65535 || number == 0)) {
                        error->SetLabel(_L("Enter a port from 1 to 65535, or leave it blank.")); dialog.Layout(); return;
                    }
                    updated.port = unsigned(number);
                }
                updated.serial = boost::trim_copy(into_u8(serial->GetValue()));
                updated.name = boost::trim_copy(into_u8(name->GetValue()));
                updated.address = boost::trim_copy(into_u8(address->GetValue()));
                const auto new_code = boost::trim_copy(into_u8(code->GetValue()));
                if (updated.name.empty() || !DiscoveredPrinter::deserialize(updated.serialize()) ||
                    (updated.vendor == "bambu" && (updated.serial.empty() ||
                        (new_code.empty() ? wxGetApp().app_config->get(code_section, p->key()) : new_code).size() != 8))) {
                    error->SetLabel(_L("Enter a printer name and valid IP address. Bambu access codes need 8 characters."));
                    dialog.Layout(); return;
                }
                const auto saved = saved_printers();
                const bool duplicate = std::any_of(saved.begin(), saved.end(), [&](const auto& other) {
                    return other.key() != p->key() && same_discovered_printer(updated, other);
                });
                if (duplicate || (updated.key() != p->key() && wxGetApp().app_config->has(saved_section, updated.key()))) {
                    error->SetLabel(_L("A printer with this address is already saved."));
                    dialog.Layout(); return;
                }
                const auto saved_code = wxGetApp().app_config->get(code_section, p->key());
                const bool connection_changed = updated.address != p->address || updated.vendor != p->vendor ||
                    updated.port != p->port || updated.serial != p->serial ||
                    (!new_code.empty() && new_code != saved_code);
                if (nozzle_changed && wxGetApp().plater()->is_background_process_slicing()) {
                    error->SetLabel(_L("Wait for slicing to finish, then change the nozzle.")); dialog.Layout(); return;
                }
                wxGetApp().plater()->capture_plate_setup();
                if (nozzle_changed) wxGetApp().plater()->take_snapshot(_u8L("Change printer nozzle"));
                stop_inspecting(false, false);
                if (connection_changed) {
                    if (auto* agent = wxGetApp().getAgent()) agent->forget_printer_session(device_id(*p));
                    forget_web(p->key());
                }
                const auto old_key = p->key();
                const auto destination = project_destination(*p);
                // Retain the portable identity and the old alias when an address changes.
                if (updated.key() != old_key)
                    wxGetApp().app_config->set("scanned_printer_destinations", std::string(destination_prefix) + updated.key(), destination);
                if (device_id(updated) != device_id(*p)) {
                    wxGetApp().app_config->erase_local_machine(device_id(*p));
                    if (auto* dev = wxGetApp().getDeviceManager()) dev->erase_local_machine(device_id(*p));
                }
                // The certificate pin follows the device across address (DHCP) edits.
                // Another identity, or an explicitly re-entered code, trusts afresh.
                const auto pin = wxGetApp().app_config->get(certificate_section, old_key);
                wxGetApp().app_config->erase(saved_section, old_key);
                wxGetApp().app_config->erase(code_section, old_key);
                wxGetApp().app_config->erase(certificate_section, old_key);
                wxGetApp().app_config->set(saved_section, updated.key(), updated.serialize());
                device_links.erase(old_key);
                if (updated.vendor == "elegoo" && !updated.serial.empty())
                    wxGetApp().app_config->set("dev_sn", device_id(updated), updated.serial);
                wxGetApp().app_config->set(code_section, updated.key(), new_code.empty() ? saved_code : new_code);
                if (!pin.empty() && new_code.empty() && updated.vendor == p->vendor && updated.serial == p->serial)
                    wxGetApp().app_config->set(certificate_section, updated.key(), pin);
                auto& plates = wxGetApp().plater()->get_partplate_list();
                for (int i = 0; i < plates.get_plate_count(); ++i) {
                    auto* plate = plates.get_plate(i);
                    if (plate->print_setup().destination == destination) {
                        if (nozzle_changed && nozzle_preset && apply_confirmed_printer_nozzle(plate->print_setup().config, nozzle_preset->config, updated.preset))
                            plate->update_slice_result_valid_state(false);
                        plate->print_setup().destination_name = updated.name;
                        if (plate->get_plate_name() == p->name) plate->set_plate_name(updated.name);
                    }
                }
                const auto active = plates.get_curr_plate()->print_setup().destination;
                if (active == destination) {
                    if (nozzle_changed) wxGetApp().plater()->activate_plate_setup();
                    apply_connection(updated, wxGetApp().preset_bundle->printers.get_edited_preset().config);
                    wxGetApp().sidebar().printer_combox()->set_selected_dev_id(device_id(updated));
                }
                wxGetApp().plater()->set_plater_dirty(true);
                m_selected = updated.key();
                wxGetApp().app_config->set("last_scanned_printer", m_selected);
                wxGetApp().app_config->save();
                dialog.EndModal(wxID_OK);
            }, wxID_OK);
            dialog.SetSizerAndFit(sizer); dialog.CentreOnParent(); wxGetApp().UpdateDlgDarkUI(&dialog);
            dialog.ShowModal();
            refresh(); inspect();
        }
        void remove()
        {
            const auto p = selected();
            if (!p) return;
            RichMessageDialog confirm(this, format_wxstr(_L("Remove %1% from your printers? Its plates and models will stay in this project."), from_u8(p->name)),
                                    _L("Remove printer"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
            confirm.SetYesNoLabels(_L("Remove"), _L("Keep printer"));
            if (confirm.ShowModal() != wxID_YES) return;
            stop_inspecting(false);
            if (auto* agent = wxGetApp().getAgent()) agent->forget_printer_session(device_id(*p));
            forget_web(p->key());
            if (auto* dev = wxGetApp().getDeviceManager()) {
                if (dev->get_selected_machine() && dev->get_selected_machine()->get_dev_id() == device_id(*p))
                    dev->set_selected_machine("");
                dev->erase_local_machine(device_id(*p));
            }
            wxGetApp().app_config->erase(saved_section, p->key());
            wxGetApp().app_config->erase(code_section, p->key());
            wxGetApp().app_config->erase(certificate_section, p->key());
            wxGetApp().app_config->erase_local_machine(device_id(*p));
            device_links.erase(p->key());
            auto& plates = wxGetApp().plater()->get_partplate_list();
            for (int i = 0; i < plates.get_plate_count(); ++i)
                if (plates.get_plate(i)->print_setup().destination == project_destination(*p))
                {
                    plates.get_plate(i)->print_setup().destination.clear();
                    plates.get_plate(i)->print_setup().destination_name.clear();
                }
            if (wxGetApp().sidebar().printer_combox()->get_selected_dev_id() == device_id(*p)) {
                wxGetApp().sidebar().printer_combox()->set_selected_dev_id("");
                auto& config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
                for (const auto* key : {"print_host", "print_host_webui", "printhost_apikey"})
                    config.set_key_value(key, new ConfigOptionString());
            }
            wxGetApp().plater()->set_plater_dirty(true);
            wxGetApp().app_config->save();
            m_selected.clear();
            refresh(); inspect();
        }
        void refresh()
        {
            const auto printers = saved_printers();
            bool changed = printers.size() != m_printers.size();
            if (!changed) for (size_t i = 0; i < printers.size(); ++i) if (printers[i].key() != m_printers[i].key()) changed = true;
            m_refreshing = true;
            if (changed) m_list->DeleteAllItems();
            m_printers = printers;
            if (!selected() && !printers.empty()) m_selected = printers.front().key();
            int connected = 0;
            for (size_t i = 0; i < printers.size(); ++i) {
                const auto& p = printers[i];
                const auto name = from_u8(p.name);
                const auto status = printer_list_status(p);
                if (changed) {
                    wxVector<wxVariant> values;
                    values.push_back(wxVariant(name));
                    values.push_back(wxVariant(status));
                    m_list->AppendItem(values);
                } else {
                    if (m_list->GetTextValue(unsigned(i), 0) != name) m_list->SetTextValue(name, unsigned(i), 0);
                    if (m_list->GetTextValue(unsigned(i), 1) != status) m_list->SetTextValue(status, unsigned(i), 1);
                }
                if (p.key() == m_selected && m_list->GetSelectedRow() != int(i)) m_list->SelectRow(unsigned(i));
                auto* agent = wxGetApp().getAgent();
                if (agent && agent->has_printer_session(device_id(p))) {
                    auto* devices = wxGetApp().getDeviceManager();
                    auto* machine = devices ? devices->get_local_machine(device_id(p)) : nullptr;
                    if (machine && machine->is_connected() && !machine->is_connecting()) ++connected;
                } else if (device_links.count(p.key()) && device_links.at(p.key()).result.status == PrinterLinkStatus::connected) ++connected;
            }
            m_refreshing = false;
            m_summary->SetLabel(printers.empty() ? _L("Your printers will appear here.") : wxString::Format(_L("%d of %d available"), connected, int(printers.size())));
            const auto p = selected();
            m_name->SetLabelText(p ? from_u8(p->name) : _L("Select a printer"));
            m_name->SetToolTip(p ? from_u8(p->name) : wxString());
            m_info->SetLabelText(p ? model_label(*p) + "  ·  " + from_u8(p->address) : "");
            m_info->SetToolTip(m_info->GetLabel());
            m_connection_detail->SetLabel(p ? printer_status_label(*p) : "");
            m_connection_detail->SetToolTip(m_connection_detail->GetLabel());
            wrap_status();
            const auto indices = p ? device_plates(*p) : std::vector<int>();
            auto& plates = wxGetApp().plater()->get_partplate_list();
            const int previous = m_plate_choices->GetSelection();
            const int previous_print = previous >= 0 && size_t(previous) < m_plate_print_indices.size() ? m_plate_print_indices[size_t(previous)] : -1;
            std::vector<int> print_indices;
            for (const int index : indices) {
                int print_index = -1;
                plates.get_plate(index)->get_print(nullptr, nullptr, &print_index);
                print_indices.push_back(print_index);
            }
            bool plates_changed = indices != m_plate_indices || print_indices != m_plate_print_indices || indices.size() != m_plate_choices->GetCount();
            for (size_t i = 0; !plates_changed && i < indices.size(); ++i) {
                const auto* plate = plates.get_plate(indices[i]);
                const auto label = format_wxstr(_L("Plate %1% — %2%"), indices[i] + 1, from_u8(plate->get_plate_name()));
                plates_changed = m_plate_choices->GetString(unsigned(i)) != label;
            }
            if (plates_changed) {
                m_plate_choices->Clear();
                m_plate_indices = indices;
                m_plate_print_indices = print_indices;
                int choice = 0;
                for (size_t i = 0; i < indices.size(); ++i) {
                    const auto* plate = plates.get_plate(indices[i]);
                    m_plate_choices->Append(format_wxstr(_L("Plate %1% — %2%"), indices[i] + 1, from_u8(plate->get_plate_name())));
                    if (print_indices[i] == previous_print) choice = int(i);
                }
                if (!indices.empty()) m_plate_choices->SetSelection(choice);
            }
            m_plate_choices->Show(!indices.empty());
            m_plate_hint->Show(indices.empty());
            m_open->Show(!indices.empty());
            const bool busy = wxGetApp().plater()->is_background_process_slicing();
            m_plate_choices->Enable(!indices.empty() && !busy);
            m_open->Enable(p && !indices.empty() && !busy);
            m_open->SetToolTip(indices.empty() ? _L("Create a plate for this printer to begin preparing a job.") : _L("Open the selected plate in Prepare."));
            const auto* preset = p ? wxGetApp().preset_bundle->printers.find_preset(p->preset) : nullptr;
            m_new_plate->Enable(preset && preset->printer_technology() == ptFFF &&
                wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF && !busy);
            m_new_plate->SetToolTip(_L("Prepare another job for this printer."));
            m_reconnect->Enable(bool(p)); m_manage->Enable(bool(p)); m_setup->Hide();
            m_detail->Layout();
        }
    };
    return new DevicesPage(parent, monitor);
}
} // namespace Slic3r::GUI
