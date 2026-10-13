#pragma once

#include "libslic3r/DiscoveredPrinter.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
using PrinterFoundFn = std::function<void(DiscoveredPrinter)>;
bool scan_bambu_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found);
bool scan_snapmaker_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found);
enum class PrinterLinkStatus { connected, unreachable, refused };
PrinterLinkStatus check_printer_connection(const DiscoveredPrinter& printer, const std::string& code, const std::atomic<bool>* cancelled = nullptr);
} // namespace Slic3r
