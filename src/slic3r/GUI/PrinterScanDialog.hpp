#pragma once

#include <string>
#include <vector>

class wxWindow;
class wxPanel;
namespace Slic3r {
class DynamicPrintConfig;
struct PrinterConnectionParams;
namespace GUI {
class MonitorPanel;
template<class Panel> class LazyPage;
void show_printer_scan(wxWindow* parent);
struct SavedPrinterChoice {
    std::string destination, name, model, preset, status;
    std::string vendor, physical_model;
};
struct SavedPrinterStatus {
    enum class Connection { unknown, connected, disconnected };
    std::string label;
    Connection connection = Connection::unknown;
};
// UI-thread snapshot only; a successful periodic check is not a live connection.
SavedPrinterStatus saved_printer_status(const std::string& destination);
// Read local saved devices and cached status; never starts discovery or a check.
std::vector<SavedPrinterChoice> saved_printer_choices();
// Checks the active upload connection against a saved destination without probing it.
bool saved_printer_matches_connection(const std::string& destination, const DynamicPrintConfig& config);
// Cached status only. A periodic check is availability, not a live control connection.
// Busy, connecting, rejected and offline devices are excluded; no probes are started.
bool saved_printer_available_for_planning(const std::string& destination);
// Assigns only the active plate. Other plates and model positions are preserved.
bool assign_saved_printer(const std::string& destination);
// Call after choosing a model preset to remove the active job's device routing.
void clear_saved_printer_assignment();
// Creates an empty independent plate for the destination; -1 if unavailable/busy.
int create_saved_printer_plate(const std::string& destination);
std::string scanned_printer_destination(const std::string& device_id);
// Makes the saved printer with this device ID the one Devices shows next; false if none is saved.
bool select_saved_printer_in_devices(const std::string& device_id);
std::string scanned_printer_name(const std::string& destination);
bool missing_scanned_printer(const std::string& destination);
std::string restore_scanned_printer(const std::string& destination, DynamicPrintConfig& config);
void ensure_scanned_printer(const std::string& destination);
void start_scanned_printer_connections(wxWindow* owner);
// Restored monitoring and device inspection use a copy instead of the active plate's preset.
DynamicPrintConfig* scanned_printer_control_config();
std::string scanned_printer_control_device_id();
bool saved_printer_connection_params(const std::string& device_id, PrinterConnectionParams& params);
wxPanel* create_scanned_devices_page(wxWindow* parent, LazyPage<MonitorPanel>* monitor);
} // namespace GUI
} // namespace Slic3r
