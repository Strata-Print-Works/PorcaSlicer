#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class AppConfig;

// Discovery identifies a machine, but never supplies credentials or a nozzle size.
struct DiscoveredPrinter
{
    std::string vendor, name, model, serial, address;
    unsigned port = 0;
    bool local_control = false;
    std::string preset;
    // Local saved identity survives an explicit correction of the connection
    // type. Plate destinations and credentials remain associated with this key.
    std::string identity_key;
    // Discovery setup hints are tri-state, never authentication evidence.
    std::optional<bool> lan_mode, access_code_required;

    std::string key() const;
    std::string serialize() const;
    static std::optional<DiscoveredPrinter> deserialize(const std::string& value);
};

std::optional<DiscoveredPrinter> parse_elegoo_printer_notice(const std::string& notice, const std::string& sender, unsigned port);
bool discovered_printer_requires_code(const DiscoveredPrinter& printer);

bool change_discovered_printer_connection(DiscoveredPrinter& printer, const std::string& vendor,
                                          const std::string& profile_model);

std::optional<DiscoveredPrinter> parse_bambu_printer_notice(const std::string& notice, const std::string& sender);
std::vector<DiscoveredPrinter> parse_snapmaker_printer_notice(const std::vector<unsigned char>& packet, const std::string& sender);
bool merge_discovered_printer(std::vector<DiscoveredPrinter>& printers, DiscoveredPrinter printer);
bool same_discovered_printer(const DiscoveredPrinter& a, const DiscoveredPrinter& b);
// Legacy address-based project destinations resolve through a local, random ID.
bool address_based_printer_destination(const std::string& destination);
std::string portable_printer_destination(const std::string& destination, AppConfig& config);
std::string local_printer_destination(const std::string& destination, const AppConfig& config);
std::vector<unsigned char> bambu_connect_packet(const std::string& client, const std::string& code);

} // namespace Slic3r
