#pragma once
#include "PrinterConnectionStatus.hpp"

#include "libslic3r/DiscoveredPrinter.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
using PrinterFoundFn = std::function<void(DiscoveredPrinter)>;
bool scan_elegoo_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found);
bool scan_bambu_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found);
bool scan_snapmaker_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found);
// `certificate_pin` is the saved Bambu certificate fingerprint (empty before first use).
// `certificate` receives the fingerprint the printer presented; the caller persists it.
PrinterLinkResult check_printer_connection(const DiscoveredPrinter& printer, const std::string& code, const std::atomic<bool>* cancelled = nullptr,
                                           std::string* serial = nullptr, const std::string& certificate_pin = {},
                                           std::string* certificate = nullptr);
} // namespace Slic3r
