#include "PrinterScanDialog.hpp"
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
#include "MainFrame.hpp"
#include "Monitor.hpp"
#include "StatusPanel.hpp"
#include "LazyPage.hpp"
#include "PrinterWebView.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "Widgets/Label.hpp"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/listctrl.h>
#include <wx/listbox.h>
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
#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <nlohmann/json.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <atomic>
#include <chrono>
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
#include <thread>
#include <future>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {
constexpr const char* saved_section = "scanned_printers";
constexpr const char* code_section = "scanned_printer_codes";
constexpr const char* destination_prefix = "scanned:";
std::optional<DynamicPrintConfig> control_config;

std::vector<DiscoveredPrinter> saved_printers()
{
    std::vector<DiscoveredPrinter> result;
    const auto* config = wxGetApp().app_config;
    if (config->has_section(saved_section))
        for (const auto& entry : config->get_section(saved_section))
            if (auto printer = DiscoveredPrinter::deserialize(entry.second))
                merge_discovered_printer(result, std::move(*printer));
    return result;
}

std::string project_destination(const DiscoveredPrinter& printer)
{
    return portable_printer_destination(std::string(destination_prefix) + printer.key(), *wxGetApp().app_config);
}

std::optional<DiscoveredPrinter> saved_printer(const std::string& destination)
{
    if (boost::starts_with(destination, "scanned-id:")) {
        for (const auto& printer : saved_printers())
            if (project_destination(printer) == destination) return printer;
        return std::nullopt;
    }
    if (!boost::starts_with(destination, destination_prefix)) return std::nullopt;
    return DiscoveredPrinter::deserialize(wxGetApp().app_config->get(saved_section, destination.substr(8)));
}

std::string device_id(const DiscoveredPrinter& p)
{
    if (p.vendor == "bambu") return p.serial;
    if (p.vendor == "snapmaker") return p.address + ":7125";
    if (p.vendor == "moonraker") return p.address + ":" + std::to_string(p.port ? p.port : 7125);
    if (p.vendor == "octoprint" && p.port && p.port != 80) return p.address + ":" + std::to_string(p.port);
    return p.address;
}

wxString model_label(const DiscoveredPrinter& p)
{
    for (const auto& vendor : wxGetApp().preset_bundle->vendors)
        for (const auto& model : vendor.second.models)
            if (!p.model.empty() && (model.model_id == p.model || model.name == p.model)) return from_u8(model.name);
    return p.model.empty() ? _L("Choose printer") : from_u8(p.model);
}

void apply_connection(const DiscoveredPrinter& p, DynamicPrintConfig& config)
{
    config.set_key_value("print_host", new ConfigOptionString(p.vendor == "bambu" ? "" : device_id(p)));
    config.set_key_value("print_host_webui", new ConfigOptionString(p.vendor == "bambu" ? "" : "http://" + device_id(p)));
    config.set_key_value("printhost_port", new ConfigOptionString());
    config.set_key_value("printhost_apikey", new ConfigOptionString(wxGetApp().app_config->get(code_section, p.key())));
    config.set_key_value("printhost_user", new ConfigOptionString());
    config.set_key_value("printhost_password", new ConfigOptionString());
    config.set_key_value("printhost_cafile", new ConfigOptionString());
    config.set_key_value("flashforge_serial_number", new ConfigOptionString(p.vendor == "flashforge" ? p.serial : ""));
    const std::string agent = p.vendor == "bambu" ? "bbl" : p.vendor == "snapmaker" ? "snapmaker" :
                              p.vendor == "creality" ? "crealityprint" : p.vendor == "moonraker" ? "moonraker" : "orca";
    config.set_key_value("printer_agent", new ConfigOptionString(agent));
    const PrintHostType host = p.vendor == "flashforge" ? htFlashforge : p.vendor == "creality" ? htCrealityPrint :
                               (p.vendor == "snapmaker" || p.vendor == "moonraker") ? htMoonraker : htOctoPrint;
    config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(host));
}

static DiscoveryScanGate discovery_scan_gate;

struct DeviceLink {
    PrinterLinkStatus status = PrinterLinkStatus::unreachable;
    bool checking = true;
    unsigned failures = 0;
    std::chrono::steady_clock::time_point next_check{};
    std::string address, code;
};
// Updated only on wx's main thread. Workers return results through queued events.
std::map<std::string, DeviceLink> device_links;
wxDEFINE_EVENT(EVT_DEVICE_LINK, wxThreadEvent);
struct LinkUpdate { DiscoveredPrinter printer; std::string code; PrinterLinkStatus status; };

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
            auto& link = device_links[update.printer.key()];
            m_in_flight.erase(update.printer.key());
            const auto current = saved_printer(std::string(destination_prefix) + update.printer.key());
            if (!current || current->address != update.printer.address ||
                wxGetApp().app_config->get(code_section, current->key()) != update.code) {
                link.next_check = {};
                return;
            }
            link.status = update.status;
            if (update.status == PrinterLinkStatus::connected) link.failures = 0;
            else link.failures = std::min(link.failures + 1, 7u);
            link.checking = link.failures > 0 && link.failures < 3 && update.status != PrinterLinkStatus::refused;
            link.next_check = std::chrono::steady_clock::now() + std::chrono::seconds(
                update.status == PrinterLinkStatus::refused ? 300 : discovery_retry_seconds(link.failures, update.status == PrinterLinkStatus::connected));
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
        m_workers.erase(std::remove_if(m_workers.begin(), m_workers.end(), [](auto& worker) {
            return worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), m_workers.end());
        if (discovery_scan_gate.active() || m_workers.size() >= 2) return;
        const auto printers = saved_printers();
        for (size_t offset = 0; offset < printers.size(); ++offset) {
            const auto index = (m_cursor + offset) % printers.size();
            const auto& p = printers[index];
            auto& link = device_links[p.key()];
            if (m_in_flight.count(p.key())) continue;
            if (auto* devices = wxGetApp().getDeviceManager()) {
                auto* machine = devices->get_selected_machine();
                if (machine && machine->get_dev_id() == device_id(p) && machine->is_connected()) {
                    link.status = PrinterLinkStatus::connected;
                    link.checking = false;
                    link.failures = 0;
                    continue; // Reuse live telemetry instead of opening another authenticated connection.
                }
            }
            const auto code = wxGetApp().app_config->get(code_section, p.key());
            if (link.address != p.address || link.code != code) {
                link.checking = true;
                link.failures = 0;
                link.next_check = {};
                link.address = p.address;
                link.code = code;
            }
            if (std::chrono::steady_clock::now() < link.next_check) continue;
            m_in_flight.insert(p.key());
            auto guard = m_guard;
            m_workers.emplace_back(std::async(std::launch::async, [guard, p, code] {
                auto status = PrinterLinkStatus::unreachable;
                try { status = check_printer_connection(p, code, &guard->cancelled); } catch (const std::exception&) {}
                std::lock_guard<std::mutex> lock(guard->mutex);
                if (!guard->target) return;
                auto* event = new wxThreadEvent(EVT_DEVICE_LINK);
                event->SetPayload(LinkUpdate{p, code, status});
                wxQueueEvent(guard->target, event);
            }));
            m_cursor = (index + 1) % printers.size();
            break; // Start at most one connection per timer tick, including startup.
        }
    }
};

wxString connection_label(const DiscoveredPrinter& p)
{
    const auto it = device_links.find(p.key());
    if (it == device_links.end() || it->second.checking) return _L("Connecting…");
    return it->second.status == PrinterLinkStatus::connected ? _L("Connected") :
           it->second.status == PrinterLinkStatus::refused ? _L("Check code") : _L("Offline");
}

wxString printer_status_label(const DiscoveredPrinter& p)
{
    if (auto* devices = wxGetApp().getDeviceManager())
        if (auto* obj = devices->get_local_machine(device_id(p)); obj && obj->is_connected()) {
            if (obj->is_in_printing_pause()) return _L("Paused");
            if (obj->is_in_printing()) return wxString::Format(_L("Printing %d%%"), obj->mc_print_percent);
        }
    return connection_label(p);
}

int device_plate(const DiscoveredPrinter& p)
{
    auto* plater = wxGetApp().plater();
    if (!plater) return -1;
    const auto destination = project_destination(p);
    auto& plates = plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i)->print_setup().destination == destination) return i;
    return -1;
}

wxDEFINE_EVENT(EVT_PRINTER_SCAN, wxThreadEvent);
struct ScanUpdate {
    std::optional<DiscoveredPrinter> printer;
    bool complete = false;
    bool available = true;
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
        m_presets = new wxChoice(this, wxID_ANY);
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
            if (update.complete) { --m_pending; if (update.available) ++m_available; }
            if (m_pending == 0) {
                stop_scan();
                m_rescan->Enable();
                m_status->SetLabel(m_seen.empty() ? (m_available ? _L("No printers answered. Scan again or add one by address.") :
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
    wxChoice* m_presets = nullptr;
    wxTextCtrl* m_code = nullptr;
    wxButton *m_rescan = nullptr, *m_add = nullptr;
    std::vector<DiscoveredPrinter> m_printers;
    std::vector<std::string> m_preset_names;
    std::set<std::string> m_seen;
    std::shared_ptr<ScanGuard> m_guard;
    std::vector<Bonjour::Ptr> m_bonjour;
    std::vector<std::future<void>> m_workers;
    DiscoveryScanGate::Lease m_scan_lease;
    int m_pending = 0, m_available = 0;

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
        m_list->SetItem(row, 1, model_label(p));
        m_list->SetItem(row, 2, from_u8(p.address));
        m_list->SetItem(row, 3, p.preset.empty() ? _L("Found") : _L("Added"));
    }
    void receive(DiscoveredPrinter p)
    {
        if (!DiscoveredPrinter::deserialize(p.serialize())) return;
        m_seen.insert(p.key());
        const auto existing = std::find_if(m_printers.begin(), m_printers.end(), [&p](const auto& item) { return same_discovered_printer(item, p); });
        const bool is_new = existing == m_printers.end();
        const long row = is_new ? long(m_printers.size()) : long(existing - m_printers.begin());
        merge_discovered_printer(m_printers, std::move(p));
        if (is_new) append_row(m_printers.back());
        else {
            const auto& printer = m_printers[size_t(row)];
            m_list->SetItem(row, 0, from_u8(printer.name));
            m_list->SetItem(row, 1, model_label(printer));
            m_list->SetItem(row, 2, from_u8(printer.address));
            // Repair an added printer's address after DHCP changes, retaining its profile and code.
            if (!printer.preset.empty()) wxGetApp().app_config->set(saved_section, printer.key(), printer.serialize());
        }
        if (m_list->GetFirstSelected() < 0) { m_list->Select(row); select(); }
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
            m_pending = 6;
            m_available = 0;
            m_rescan->Disable();
            m_status->SetLabel(_L("Searching your network…"));
            auto guard = m_guard;
            const PrinterFoundFn found = [guard](DiscoveredPrinter p) { guard->post({std::move(p), false, true}); };
            auto worker = [this, guard](std::function<bool()> scan) {
                m_workers.emplace_back(std::async(std::launch::async, [guard, scan = std::move(scan)] {
                    bool available = false;
                    try { available = scan(); } catch (const std::exception&) {}
                    guard->post({std::nullopt, true, available});
                }));
            };
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
                auto lookup = Bonjour(service).set_timeout(6).set_retries(1)
                    .set_txt_keys({"machine_type", "device_name", "sn", "link_mode", "model", "hostname"})
                    .on_reply([guard, found, service](BonjourReply&& reply) {
                        const auto field = [&reply](const std::string& key) {
                            const auto it = reply.txt_data.find(key);
                            return it == reply.txt_data.end() ? std::string() : it->second;
                        };
                        const bool snapmaker = service == "snapmaker";
                        DiscoveredPrinter printer{service, snapmaker ? field("device_name") : reply.service_name,
                                                  snapmaker ? field("machine_type") : field("model"),
                                                  field("sn"), reply.ip.to_string(), reply.port,
                                                  !snapmaker || field("link_mode") == "lan", ""};
                        if (snapmaker && printer.serial.empty()) return;
                        if (printer.name.empty()) printer.name = printer.model.empty() ? reply.hostname : printer.model;
                        if (!guard->cancelled->load()) found(std::move(printer));
                    }).on_complete([guard] { guard->post({std::nullopt, true, true}); }).lookup();
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
        const bool known_model = std::any_of(bundle.printers.begin(), bundle.printers.end(), [&p, &bundle](auto& preset) {
            return !p.model.empty() && (preset.config.opt_string("printer_model") == p.model ||
                                       preset.get_current_printer_type(&bundle) == p.model);
        });
        for (auto& preset : bundle.printers) {
            if (preset.is_default || preset.printer_technology() != ptFFF || !preset.is_system) continue;
            const auto& model = preset.config.opt_string("printer_model");
            const bool match = !p.model.empty() && (model == p.model || preset.get_current_printer_type(&bundle) == p.model);
            if (!match && known_model) continue;
            // Unidentified machines need an explicit choice; never select the nearest model.
            if (!match && preset.vendor && !boost::icontains(preset.vendor->name, p.vendor) &&
                p.vendor != "moonraker" && p.vendor != "octoprint") continue;
            m_preset_names.push_back(preset.name);
            const int index = int(m_preset_names.size()) - 1;
            m_presets->Append(from_u8(preset.name));
            if (preset.name == p.preset) selected = index;
            else if (selected < 0 && match && preset.config.opt_string("printer_variant") == "0.4") selected = index;
        }
        m_presets->Enable(!m_preset_names.empty());
        if (selected >= 0) m_presets->SetSelection(selected);
        const bool needs_code = p.vendor == "bambu" || p.vendor == "flashforge";
        const bool has_code = wxGetApp().app_config->has(code_section, p.key());
        const bool code_refused = device_links.count(p.key()) && device_links.at(p.key()).status == PrinterLinkStatus::refused;
        m_code_label->SetLabel(p.vendor == "flashforge" ? _L("Check code") : _L("Access code"));
        m_code_label->Show(needs_code && (!has_code || code_refused));
        m_code->Show(needs_code && (!has_code || code_refused));
        m_code->Enable(needs_code && (!has_code || code_refused));
        m_add->SetLabel(p.preset.empty() ? _L("Add printer") : _L("Use printer"));
        m_hint->SetLabel(!p.local_control ? _L("Enable LAN mode on the printer, then scan again.") :
                        m_preset_names.empty() ? _L("Install this printer's profiles from Printer settings, then try again.") :
                        needs_code && !has_code ? _L("Enter the code shown in the printer's Network settings. Check the nozzle above before adding.") :
                                                 _L("Check the printer and nozzle above before adding."));
        m_hint->Wrap(FromDIP(640));
        Layout();
        update_add();
    }
    void update_add()
    {
        const long row = m_list->GetFirstSelected();
        if (row < 0) { m_add->Disable(); return; }
        const auto& p = m_printers[size_t(row)];
        const bool requires_code = p.vendor == "bambu";
        const auto code = boost::trim_copy(into_u8(m_code->GetValue()));
        m_add->Enable(p.local_control && m_presets->GetSelection() >= 0 &&
                      (!requires_code || wxGetApp().app_config->has(code_section, p.key()) || code.size() == 8));
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
    for (const auto& label : {_L("Bambu Lab"), _L("Snapmaker"), _L("Flashforge"), _L("Creality"), _L("Klipper / Moonraker"), _L("OctoPrint")})
        vendor->Append(label);
    vendor->SetName(_L("Connection"));
    vendor->SetSelection(4);
    sizer->Add(vendor, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* serial = new wxTextCtrl(&dialog, wxID_ANY);
    serial->SetName(_L("Serial number"));
    serial->SetHint(_L("Bambu serial number, if needed"));
    sizer->Add(serial, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* buttons = dialog.CreateButtonSizer(wxOK | wxCANCEL);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    dialog.SetSizerAndFit(sizer);
    dialog.SetMinSize(FromDIP(wxSize(400, -1)));
    dialog.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() != wxID_OK) return;
    static const std::vector<std::string> vendors{"bambu", "snapmaker", "flashforge", "creality", "moonraker", "octoprint"};
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
    // Importing a legacy project can reset the plate mode. Adding a device
    // explicitly opts that project into independently configured beds again.
    if (!plater->get_partplate_list().independent_setups()) plater->enable_independent_plates();
    auto p = m_printers[size_t(row)];
    const auto code = boost::trim_copy(into_u8(m_code->GetValue()));
    const int own_plate = device_plate(p);
    if (own_plate >= 0) {
        // A portable project can remember this printer before it is saved locally.
        // Restore the connection without replacing that plate's edited print setup.
        p.preset = m_preset_names[size_t(choice)];
        wxGetApp().app_config->set(saved_section, p.key(), p.serialize());
        wxGetApp().app_config->set("last_scanned_printer", p.key());
        if (!code.empty()) wxGetApp().app_config->set(code_section, p.key(), code);
        wxGetApp().app_config->save();
        auto& plates = plater->get_partplate_list();
        plates.get_plate(own_plate)->print_setup().destination_name = p.name;
        if (plates.get_curr_plate_index() == own_plate)
            plater->activate_plate_setup();
        else
            plates.select_plate(own_plate);
        plater->set_plater_dirty(true);
        m_printers[size_t(row)] = p;
        m_list->SetItem(row, 3, _L("Added"));
        m_hint->SetLabel(_L("Printer added. Its existing plate is ready."));
        keep_open_after_add();
        return;
    }
    p.preset = m_preset_names[size_t(choice)];
    auto& bundle = *wxGetApp().preset_bundle;
    auto* preset = bundle.printers.find_preset(p.preset);
    if (!preset) return;
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
    if (!code.empty()) wxGetApp().app_config->set(code_section, p.key(), code);
    wxGetApp().app_config->set(saved_section, p.key(), p.serialize());
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

DynamicPrintConfig* scanned_printer_control_config() { return control_config ? &*control_config : nullptr; }

wxPanel* create_scanned_devices_page(wxWindow* parent, LazyPage<MonitorPanel>* monitor)
{
    class DevicesPage final : public wxPanel {
        wxListBox* m_list;
        wxStaticText *m_summary, *m_name, *m_info, *m_message;
        wxButton *m_open, *m_reconnect, *m_manage, *m_setup;
        wxChoice* m_view;
        wxPanel *m_detail, *m_empty;
        wxScrolledWindow* m_controls;
        LazyPage<MonitorPanel>* m_monitor;
        PrinterWebView* m_web = nullptr;
        wxTimer m_timer;
        std::vector<DiscoveredPrinter> m_printers;
        std::string m_selected, m_loaded;
        bool m_refreshing = false;
        bool m_adding = false;
    public:
        DevicesPage(wxWindow* parent, LazyPage<MonitorPanel>* monitor)
            : wxPanel(parent), m_monitor(monitor), m_timer(this)
        {
            SetFont(wxGetApp().normal_font());
            auto* splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                  wxSP_LIVE_UPDATE | wxSP_3D);
            splitter->SetMinimumPaneSize(FromDIP(220));
            splitter->SetSashGravity(0);
            auto* sidebar = new wxPanel(splitter);
            auto* left = new wxBoxSizer(wxVERTICAL);
            auto* title = new wxStaticText(sidebar, wxID_ANY, _L("Printers"));
            title->SetFont(Label::Head_20);
            left->Add(title, 0, wxALL, FromDIP(16));
            auto* add = new wxButton(sidebar, wxID_ANY, _L("Add printer…"));
            left->Add(add, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
            m_list = new wxListBox(sidebar, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr,
                                   wxLB_SINGLE | wxLB_HSCROLL);
            m_list->SetName(_L("Saved printers"));
            left->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
            m_summary = new wxStaticText(sidebar, wxID_ANY, "");
            left->Add(m_summary, 0, wxALL, FromDIP(16));
            sidebar->SetSizer(left);

            auto* detail = m_detail = new wxPanel(splitter);
            auto* right = new wxBoxSizer(wxVERTICAL);
            m_name = new wxStaticText(detail, wxID_ANY, _L("Select a printer"));
            m_name->SetFont(Label::Head_20);
            right->Add(m_name, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
            m_info = new wxStaticText(detail, wxID_ANY, "");
            right->Add(m_info, 0, wxLEFT | wxRIGHT, FromDIP(20));
            auto* actions = new wxBoxSizer(wxHORIZONTAL);
            m_open = new wxButton(detail, wxID_ANY, _L("Open plate"));
            m_reconnect = new wxButton(detail, wxID_ANY, _L("Reconnect"));
            m_manage = new wxButton(detail, wxID_ANY, _L("Manage…"));
            actions->Add(m_open, 0, wxRIGHT, FromDIP(8));
            actions->Add(m_reconnect, 0, wxRIGHT, FromDIP(8));
            m_view = new wxChoice(detail, wxID_ANY);
            m_view->SetName(_L("Printer view"));
            for (const auto& label : {_L("Status and controls"), _L("Storage"), _L("Firmware"), _L("Troubleshooting")})
                m_view->Append(label);
            m_view->SetSelection(0);
            m_view->Hide();
            actions->Add(m_view, 0, wxRIGHT, FromDIP(8));
            actions->AddStretchSpacer();
            actions->Add(m_manage, 0);
            right->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(12));
            m_empty = new wxPanel(detail);
            auto* empty = new wxBoxSizer(wxVERTICAL);
            empty->AddStretchSpacer();
            m_message = new wxStaticText(m_empty, wxID_ANY, _L("Select a printer to see its status and controls."));
            empty->Add(m_message, 0, wxALIGN_CENTER | wxALL, FromDIP(16));
            m_setup = new wxButton(m_empty, wxID_ANY, _L("Set up controls…"));
            empty->Add(m_setup, 0, wxALIGN_CENTER | wxALL, FromDIP(8));
            empty->AddStretchSpacer();
            m_empty->SetSizer(empty);
            right->Add(m_empty, 1, wxEXPAND);
            m_controls = new wxScrolledWindow(detail);
            m_controls->SetScrollRate(FromDIP(12), FromDIP(12));
            auto* controls = new wxBoxSizer(wxVERTICAL);
            m_monitor->Reparent(m_controls);
            controls->Add(m_monitor, 1, wxEXPAND);
            m_controls->SetSizer(controls);
            m_controls->Hide();
            right->Add(m_controls, 1, wxEXPAND);
            detail->SetSizer(right);
            splitter->SplitVertically(sidebar, detail, FromDIP(250));
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            sizer->Add(splitter, 1, wxEXPAND);
            SetSizer(sizer);

            add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                m_adding = true;
                stop_inspecting();
                show_printer_scan(this);
                m_adding = false;
                m_selected = wxGetApp().app_config->get("last_scanned_printer");
                refresh();
                inspect();
            });
            m_reconnect->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (const auto p = selected()) {
                    auto& link = device_links[p->key()];
                    link.checking = true;
                    link.next_check = {};
                    m_loaded.clear();
                    inspect();
                }
            });
            m_open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_plate(); });
            m_manage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxMenu menu;
                menu.Append(wxID_EDIT, _L("Edit printer…"));
                menu.AppendSeparator();
                menu.Append(wxID_REMOVE, _L("Remove printer…"));
                menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
                    if (e.GetId() == wxID_EDIT) edit(); else remove();
                });
                m_manage->PopupMenu(&menu);
            });
            m_setup->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxGetApp().show_network_plugin_download_dialog();
                m_loaded.clear();
                inspect();
            });
            m_view->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
                if (auto* panel = m_monitor->get()) panel->get_tabpanel()->SetSelection(m_view->GetSelection());
                layout_detail();
            });
            m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) {
                if (m_refreshing || e.GetSelection() < 0 || size_t(e.GetSelection()) >= m_printers.size()) return;
                m_selected = m_printers[size_t(e.GetSelection())].key();
                wxGetApp().app_config->set("last_scanned_printer", m_selected);
                inspect();
                refresh();
            });
            Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
                refresh();
                if (IsShownOnScreen()) inspect();
            });
            m_selected = wxGetApp().app_config->get("last_scanned_printer");
            m_timer.Start(1000);
            refresh();
        }
        ~DevicesPage() override { m_timer.Stop(); control_config.reset(); }
        bool Show(bool show = true) override
        {
            const bool changed = wxPanel::Show(show);
            if (show) { refresh(); inspect(); }
            else { stop_inspecting(); m_controls->Hide(); }
            return changed;
        }
    private:
        void layout_detail()
        {
            Layout();
            // Showing a child doesn't resize the splitter pane. Relayout its own
            // sizer so the view selector and previously hidden controls get space.
            m_detail->Layout();
            if (m_empty->IsShown()) m_empty->Layout();
            if (m_controls->IsShown()) {
                m_controls->Layout();
                m_controls->FitInside();
            }
        }
        std::optional<DiscoveredPrinter> selected() const
        {
            for (const auto& p : m_printers) if (p.key() == m_selected) return p;
            return std::nullopt;
        }
        void stop_inspecting(bool restore_plate = true)
        {
            m_monitor->Hide();
            m_view->Hide();
            if (m_web) m_web->Hide();
            m_loaded.clear();
            if (!control_config) return;
            // Tear down the inspected connection while sidebar synchronization is still suppressed.
            if (auto* dev = wxGetApp().getDeviceManager()) dev->set_selected_machine("");
            control_config.reset();
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
            const bool needs_bambu = p->vendor == "bambu" && !NetworkAgent::is_network_module_loaded();
            const bool native = !needs_bambu && agent_id != "orca" && NetworkAgentFactory::is_printer_agent_registered(agent_id);
            const bool web = !native && (p->vendor == "moonraker" || p->vendor == "octoprint" || p->vendor == "creality" || p->vendor == "snapmaker");
            if (needs_bambu || (!native && !web)) {
                stop_inspecting();
                m_empty->Show(); m_controls->Hide(); m_setup->Show(needs_bambu);
                m_message->SetLabel(needs_bambu ? _L("Set up Bambu connection support to see the camera and use printer controls.") :
                                               _L("Controls aren't available for this printer yet. You can still open its plate."));
                m_message->Wrap(FromDIP(440));
                layout_detail();
                return;
            }
            const std::string loaded = p->serialize() + wxGetApp().app_config->get(code_section, p->key()) + (native ? "native" : "web");
            if (loaded == m_loaded) return;
            stop_inspecting(false);
            control_config = std::move(config);
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
                m_view->Show(); m_view->SetSelection(0);
                if (auto* panel = m_monitor->get()) {
                    panel->get_tabpanel()->SetSelection(0);
                    panel->set_default(); panel->update_all();
                    panel->get_status_panel()->Scroll(0, 0);
                }
            } else {
                if (!m_web) {
                    m_web = new PrinterWebView(m_controls);
                    m_controls->GetSizer()->Add(m_web, 1, wxEXPAND);
                    wxGetApp().UpdateDarkUI(m_web);
                }
                // Moonraker's HTTP API port is distinct from the printer's browser control page.
                wxString url = from_u8("http://" + (p->vendor == "octoprint" ? device_id(*p) : p->address));
                m_web->Show();
                m_web->load_url(url, from_u8(wxGetApp().app_config->get(code_section, p->key())));
            }
            m_empty->Hide(); m_controls->Show();
            m_loaded = loaded;
            layout_detail();
            m_controls->Scroll(0, 0);
        }
        void open_plate()
        {
            const auto p = selected();
            if (!p) return;
            auto* plater = wxGetApp().plater();
            const int plate = device_plate(*p);
            if (!plater || plate < 0 || plater->is_background_process_slicing()) return;
            stop_inspecting();
            plater->get_partplate_list().select_plate(plate);
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
                auto* text = new wxTextCtrl(&dialog, wxID_ANY, value, wxDefaultPosition, FromDIP(wxSize(270, -1)), style);
                text->SetName(label); fields->Add(text, 1, wxEXPAND); return text;
            };
            auto* name = field(_L("Printer name"), from_u8(p->name));
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
                updated.name = boost::trim_copy(into_u8(name->GetValue()));
                updated.address = boost::trim_copy(into_u8(address->GetValue()));
                const auto new_code = boost::trim_copy(into_u8(code->GetValue()));
                if (updated.name.empty() || !DiscoveredPrinter::deserialize(updated.serialize()) ||
                    (p->vendor == "bambu" && !new_code.empty() && new_code.size() != 8)) {
                    error->SetLabel(_L("Enter a printer name and valid IP address. Bambu access codes need 8 characters."));
                    dialog.Layout(); return;
                }
                if (updated.key() != p->key() && wxGetApp().app_config->has(saved_section, updated.key())) {
                    error->SetLabel(_L("A printer with this address is already saved."));
                    dialog.Layout(); return;
                }
                stop_inspecting(false);
                const auto old_key = p->key();
                const auto destination = project_destination(*p);
                // Retain the portable identity and the old alias when an address changes.
                if (updated.key() != old_key)
                    wxGetApp().app_config->set("scanned_printer_destinations", std::string(destination_prefix) + updated.key(), destination);
                if (device_id(updated) != device_id(*p)) {
                    wxGetApp().app_config->erase_local_machine(device_id(*p));
                    if (auto* dev = wxGetApp().getDeviceManager()) dev->erase_local_machine(device_id(*p));
                }
                const auto saved_code = wxGetApp().app_config->get(code_section, old_key);
                wxGetApp().app_config->erase(saved_section, old_key);
                wxGetApp().app_config->erase(code_section, old_key);
                wxGetApp().app_config->set(saved_section, updated.key(), updated.serialize());
                wxGetApp().app_config->set(code_section, updated.key(), new_code.empty() ? saved_code : new_code);
                auto& plates = wxGetApp().plater()->get_partplate_list();
                for (int i = 0; i < plates.get_plate_count(); ++i) {
                    auto* plate = plates.get_plate(i);
                    if (plate->print_setup().destination == destination) {
                        plate->print_setup().destination_name = updated.name;
                        if (plate->get_plate_name() == p->name) plate->set_plate_name(updated.name);
                    }
                }
                const auto active = plates.get_curr_plate()->print_setup().destination;
                if (active == destination) {
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
            wxMessageDialog confirm(this, wxString::Format(_L("Remove %s from your printers? Its plate and models will stay in this project."), from_u8(p->name)),
                                    _L("Remove printer"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
            confirm.SetYesNoLabels(_L("Remove"), _L("Keep printer"));
            if (confirm.ShowModal() != wxID_YES) return;
            stop_inspecting(false);
            if (auto* dev = wxGetApp().getDeviceManager()) {
                if (dev->get_selected_machine() && dev->get_selected_machine()->get_dev_id() == device_id(*p))
                    dev->set_selected_machine("");
                dev->erase_local_machine(device_id(*p));
            }
            wxGetApp().app_config->erase(saved_section, p->key());
            wxGetApp().app_config->erase(code_section, p->key());
            wxGetApp().app_config->erase_local_machine(device_id(*p));
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
            if (changed) m_list->Clear();
            m_printers = printers;
            if (!selected() && !printers.empty()) m_selected = printers.front().key();
            int connected = 0;
            for (size_t i = 0; i < printers.size(); ++i) {
                const auto& p = printers[i];
                const auto label = from_u8(p.name) + "  ·  " + printer_status_label(p);
                if (changed) m_list->Append(label);
                else if (m_list->GetString(unsigned(i)) != label) m_list->SetString(unsigned(i), label);
                if (p.key() == m_selected && m_list->GetSelection() != int(i)) m_list->SetSelection(int(i));
                if (device_links.count(p.key()) && device_links.at(p.key()).status == PrinterLinkStatus::connected) ++connected;
            }
            m_refreshing = false;
            m_summary->SetLabel(printers.empty() ? _L("Your printers will appear here.") : wxString::Format(_L("%d of %d connected"), connected, int(printers.size())));
            const auto p = selected();
            m_name->SetLabel(p ? from_u8(p->name) : _L("Select a printer"));
            m_info->SetLabel(p ? model_label(*p) + "  |  " + from_u8(p->address) + "  |  " + connection_label(*p) : "");
            m_open->Enable(p && device_plate(*p) >= 0 && !wxGetApp().plater()->is_background_process_slicing());
            m_open->SetToolTip(p && device_plate(*p) < 0 ? _L("This printer has no plate in the current project.") : _L("Go to this printer's plate."));
            m_reconnect->Enable(bool(p)); m_manage->Enable(bool(p)); m_setup->Hide();
            m_detail->Layout();
        }
    };
    return new DevicesPage(parent, monitor);
}
} // namespace Slic3r::GUI
