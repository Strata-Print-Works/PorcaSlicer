#pragma once

#include <string>

class wxWindow;
class wxPanel;
namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {
class MonitorPanel;
template<class Panel> class LazyPage;
void show_printer_scan(wxWindow* parent);
std::string scanned_printer_destination(const std::string& device_id);
std::string scanned_printer_name(const std::string& destination);
bool missing_scanned_printer(const std::string& destination);
std::string restore_scanned_printer(const std::string& destination, DynamicPrintConfig& config);
void ensure_scanned_printer(const std::string& destination);
void start_scanned_printer_connections(wxWindow* owner);
// While inspecting a device, networking uses this copy instead of the active plate's preset.
DynamicPrintConfig* scanned_printer_control_config();
wxPanel* create_scanned_devices_page(wxWindow* parent, LazyPage<MonitorPanel>* monitor);
} // namespace GUI
} // namespace Slic3r
