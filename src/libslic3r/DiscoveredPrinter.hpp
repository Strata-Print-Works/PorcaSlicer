#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Discovery identifies a machine, but never supplies credentials or a nozzle size.
struct DiscoveredPrinter
{
    std::string vendor, name, model, serial, address;
    unsigned port = 0;
    bool local_control = false;
    std::string preset;

    std::string key() const;
    std::string serialize() const;
    static std::optional<DiscoveredPrinter> deserialize(const std::string& value);
};

std::optional<DiscoveredPrinter> parse_bambu_printer_notice(const std::string& notice, const std::string& sender);
std::vector<DiscoveredPrinter> parse_snapmaker_printer_notice(const std::vector<unsigned char>& packet, const std::string& sender);
bool merge_discovered_printer(std::vector<DiscoveredPrinter>& printers, DiscoveredPrinter printer);
bool same_discovered_printer(const DiscoveredPrinter& a, const DiscoveredPrinter& b);
std::vector<unsigned char> bambu_connect_packet(const std::string& client, const std::string& code);

} // namespace Slic3r
