#pragma once

#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <string>
#include <cmath>
#include <initializer_list>

namespace Slic3r {
// An explicit nozzle correction changes only printer identity/nozzle fields.
// Keep the plate's process/material edits and geometry; callers invalidate its slice.
inline bool apply_confirmed_printer_nozzle(DynamicPrintConfig& plate, const DynamicPrintConfig& target,
                                           const std::string& preset_name)
{
    const auto* nozzle = target.option<ConfigOptionFloats>("nozzle_diameter");
    if (plate.opt_string("printer_model") != target.opt_string("printer_model") || !nozzle ||
        nozzle->values.size() != 1 || !std::isfinite(nozzle->values[0]) || nozzle->values[0] <= 0) return false;
    plate.set_key_value("nozzle_diameter", nozzle->clone());
    if (const auto* variant = target.option("printer_variant")) plate.set_key_value("printer_variant", variant->clone());
    plate.set_key_value("printer_settings_id", new ConfigOptionString(preset_name));
    return true;
}
// An IPv6 literal is bracketed wherever a port follows, as in a URL authority.
inline std::string saved_printer_host(const std::string& address)
{
    return address.find(':') == std::string::npos || address.front() == '[' ? address : "[" + address + "]";
}
// Pure connection projection: model/nozzle/material/slice settings are untouched.
inline std::string saved_printer_device_id(const DiscoveredPrinter& p)
{
    if (p.vendor == "bambu") return p.serial;
    if (p.vendor == "snapmaker") return saved_printer_host(p.address) + ":7125";
    if (p.vendor == "moonraker") return saved_printer_host(p.address) + ":" + std::to_string(p.port ? p.port : 7125);
    if ((p.vendor == "elegoo" || p.vendor == "octoprint") && p.port && p.port != 80)
        return saved_printer_host(p.address) + ":" + std::to_string(p.port);
    if (p.vendor == "elegoo") return saved_printer_host(p.address);
    return p.address;
}
inline void apply_saved_printer_connection(const DiscoveredPrinter& p, const std::string& code, DynamicPrintConfig& config)
{
    const auto id = saved_printer_device_id(p);
    config.set_key_value("print_host", new ConfigOptionString(p.vendor == "bambu" ? "" : id));
    config.set_key_value("print_host_webui", new ConfigOptionString(p.vendor == "bambu" ? "" : "http://" + id));
    config.set_key_value("printhost_port", new ConfigOptionString());
    config.set_key_value("printhost_apikey", new ConfigOptionString(code));
    for (const auto* key : {"printhost_user", "printhost_password", "printhost_cafile"})
        config.set_key_value(key, new ConfigOptionString());
    config.set_key_value("flashforge_serial_number", new ConfigOptionString(p.vendor == "flashforge" ? p.serial : ""));
    const std::string agent = p.vendor == "bambu" ? "bbl" : p.vendor == "snapmaker" ? "snapmaker" :
                              p.vendor == "creality" ? "crealityprint" : p.vendor == "moonraker" ? "moonraker" : "orca";
    config.set_key_value("printer_agent", new ConfigOptionString(agent));
    const PrintHostType host = p.vendor == "elegoo" ? htElegooLink : p.vendor == "flashforge" ? htFlashforge :
        p.vendor == "creality" ? htCrealityPrint : (p.vendor == "snapmaker" || p.vendor == "moonraker") ? htMoonraker : htOctoPrint;
    config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(host));
}
} // namespace Slic3r
