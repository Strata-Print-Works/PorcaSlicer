#include "DiscoveredPrinter.hpp"
#include "AppConfig.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace {
bool valid_address(const std::string& address)
{
    boost::system::error_code ec;
    const auto ip = boost::asio::ip::make_address(address, ec);
    return !ec && !ip.is_unspecified() && !ip.is_multicast();
}
bool valid(const DiscoveredPrinter& p)
{
    for (const auto* field : {&p.name, &p.model, &p.serial, &p.preset})
        if (std::any_of(field->begin(), field->end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    return (p.vendor == "bambu" || p.vendor == "snapmaker" || p.vendor == "flashforge" ||
            p.vendor == "creality" || p.vendor == "moonraker" || p.vendor == "octoprint") &&
           !p.name.empty() && p.name.size() <= 256 && p.model.size() <= 256 && p.serial.size() <= 256 &&
           p.preset.size() <= 512 && p.port <= 65535 && valid_address(p.address);
}
} // namespace

std::string DiscoveredPrinter::key() const
{
    return vendor + ":" + (serial.empty() ? address + ":" + std::to_string(port) : serial);
}

bool address_based_printer_destination(const std::string& destination)
{
    if (destination.compare(0, 11, "scanned-id:") == 0) return false;
    auto address = destination;
    const bool scanned = address.compare(0, 8, "scanned:") == 0;
    if (scanned) {
        const auto vendor_end = address.find(':', 8);
        if (vendor_end == std::string::npos) return false;
        address.erase(0, vendor_end + 1);
    }
    if (valid_address(address)) return true;
    const auto port_start = address.rfind(':');
    if (port_start != std::string::npos) {
        if (port_start + 1 == address.size() ||
            !std::all_of(address.begin() + port_start + 1, address.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        address.resize(port_start);
    }
    if (address.size() > 2 && address.front() == '[' && address.back() == ']')
        address = address.substr(1, address.size() - 2);
    if (valid_address(address)) return true;
    // Scanned serials are explicitly identified. Bare native IDs can also be
    // DNS hosts (including single-label names), so keep those tokens local.
    return !scanned && !address.empty() && std::all_of(address.begin(), address.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    });
}

std::string portable_printer_destination(const std::string& destination, AppConfig& config)
{
    if (!address_based_printer_destination(destination)) return destination;
    constexpr const char* section = "scanned_printer_destinations";
    auto portable = config.get(section, destination);
    if (portable.empty()) {
        portable = "scanned-id:" + boost::uuids::to_string(boost::uuids::random_generator()());
        config.set(section, destination, portable);
    }
    return portable;
}

std::string local_printer_destination(const std::string& destination, const AppConfig& config)
{
    if (destination.compare(0, 11, "scanned-id:") != 0) return destination;
    if (config.has_section("scanned_printer_destinations"))
        for (const auto& alias : config.get_section("scanned_printer_destinations"))
            if (alias.second == destination && alias.first.compare(0, 8, "scanned:") != 0) return alias.first;
    return {};
}

std::string DiscoveredPrinter::serialize() const
{
    return nlohmann::json{{"vendor", vendor}, {"name", name}, {"model", model}, {"serial", serial},
                          {"address", address}, {"port", port}, {"local_control", local_control}, {"preset", preset}}.dump();
}

std::optional<DiscoveredPrinter> DiscoveredPrinter::deserialize(const std::string& value)
{
    try {
        if (value.size() > 8192) return std::nullopt;
        const auto j = nlohmann::json::parse(value);
        DiscoveredPrinter p{j.at("vendor").get<std::string>(), j.at("name").get<std::string>(),
                            j.at("model").get<std::string>(), j.value("serial", std::string()),
                            j.at("address").get<std::string>(), j.value("port", 0u),
                            j.value("local_control", false), j.value("preset", std::string())};
        if (valid(p)) return p;
    } catch (const nlohmann::json::exception&) {}
    return std::nullopt;
}

std::optional<DiscoveredPrinter> parse_bambu_printer_notice(const std::string& notice, const std::string& sender)
{
    if (notice.size() > 8192 || !valid_address(sender)) return std::nullopt;
    std::istringstream lines(notice);
    std::string line;
    std::getline(lines, line);
    boost::trim(line);
    if (line != "NOTIFY * HTTP/1.1" && line != "HTTP/1.1 200 OK") return std::nullopt;
    std::map<std::string, std::string> fields;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        fields[boost::to_lower_copy(boost::trim_copy(line.substr(0, colon)))] = boost::trim_copy(line.substr(colon + 1));
    }
    const auto kind = fields.count("nt") ? fields["nt"] : fields["st"];
    if (kind != "urn:bambulab-com:device:3dprinter:1") return std::nullopt;
    DiscoveredPrinter p;
    p.vendor = "bambu";
    p.model = fields["devmodel.bambu.com"];
    p.serial = fields["devsn.bambu.com"];
    if (p.serial.empty()) p.serial = fields["usn"];
    if (p.serial.empty() || p.model.empty()) return std::nullopt;
    p.name = fields["devname.bambu.com"];
    if (p.name.empty()) p.name = "Bambu " + p.model;
    // The sender is authoritative; an advertised location may be stale or a URL.
    p.address = sender;
    p.port = 8883;
    p.local_control = fields["devbind.bambu.com"] == "free" || fields["devconnect.bambu.com"] == "lan";
    return valid(p) ? std::optional<DiscoveredPrinter>(std::move(p)) : std::nullopt;
}

bool merge_discovered_printer(std::vector<DiscoveredPrinter>& printers, DiscoveredPrinter printer)
{
    if (!valid(printer)) return false;
    const auto found = std::find_if(printers.begin(), printers.end(), [&printer](const auto& saved) {
        return same_discovered_printer(saved, printer);
    });
    if (found == printers.end()) {
        printers.push_back(std::move(printer));
        return true;
    }
    if (printer.preset.empty()) printer.preset = found->preset;
    if (printer.serial.empty()) printer.serial = found->serial;
    *found = std::move(printer);
    return false;
}

bool same_discovered_printer(const DiscoveredPrinter& a, const DiscoveredPrinter& b)
{
    if (a.vendor != b.vendor) return false;
    if (!a.serial.empty() && !b.serial.empty()) return a.serial == b.serial;
    return a.address == b.address && a.port == b.port;
}

std::vector<unsigned char> bambu_connect_packet(const std::string& client, const std::string& code)
{
    // MQTT 3.1.1 CONNECT: clean session, username/password, five-second keepalive.
    std::vector<unsigned char> body{0, 4, 'M', 'Q', 'T', 'T', 4, 0xc2, 0, 5};
    const auto append = [&body](const std::string& value) {
        body.push_back(static_cast<unsigned char>(value.size() >> 8));
        body.push_back(static_cast<unsigned char>(value.size() & 0xff));
        body.insert(body.end(), value.begin(), value.end());
    };
    append(client);
    append("bblp");
    append(code);
    std::vector<unsigned char> packet{0x10};
    size_t remaining = body.size();
    do {
        const auto digit = static_cast<unsigned char>(remaining % 128);
        remaining /= 128;
        packet.push_back(digit | (remaining ? 0x80 : 0));
    } while (remaining);
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<DiscoveredPrinter> parse_snapmaker_printer_notice(const std::vector<unsigned char>& bytes, const std::string& sender)
{
    // Ported from MultiSlicer's discovery/snapmaker.rs. Keep DNS compression
    // bounded and correlate TXT, SRV and A records by service/host identity.
    if (bytes.size() < 12 || bytes.size() > 65536 || !(bytes[2] & 0x80)) return {};
    const auto word = [&bytes](size_t at) -> std::optional<unsigned> {
        if (at + 2 > bytes.size()) return std::nullopt;
        return (unsigned(bytes[at]) << 8) | bytes[at + 1];
    };
    const auto name = [&bytes](size_t& cursor) -> std::optional<std::string> {
        size_t at = cursor, resume = 0;
        unsigned hops = 0;
        std::string result;
        while (at < bytes.size()) {
            const unsigned length = bytes[at++];
            if ((length & 0xc0) == 0xc0) {
                if (at >= bytes.size() || ++hops > 32) return std::nullopt;
                if (!resume) resume = at + 1;
                at = ((length & 0x3f) << 8) | bytes[at];
            } else if (length == 0) {
                cursor = resume ? resume : at;
                return result;
            } else {
                if (length > 63 || at + length > bytes.size() || result.size() + length > 255) return std::nullopt;
                if (!result.empty()) result += '.';
                result.append(reinterpret_cast<const char*>(bytes.data() + at), length);
                at += length;
            }
        }
        return std::nullopt;
    };
    struct Service { unsigned port = 0; std::string host; std::map<std::string, std::string> fields; };
    std::map<std::string, Service> services;
    std::map<std::string, std::string> addresses;
    size_t cursor = 12;
    const unsigned questions = *word(4);
    const unsigned records = *word(6) + *word(8) + *word(10);
    if (questions > 256 || records > 256) return {};
    for (unsigned i = 0; i < questions; ++i) {
        if (!name(cursor) || cursor + 4 > bytes.size()) return {};
        cursor += 4;
    }
    for (unsigned i = 0; i < records; ++i) {
        auto owner = name(cursor);
        if (!owner || cursor + 10 > bytes.size()) return {};
        const unsigned kind = *word(cursor);
        const unsigned length = *word(cursor + 8);
        cursor += 10;
        const size_t end = cursor + length;
        if (end > bytes.size()) return {};
        boost::to_lower(*owner);
        if (kind == 1 && length == 4) {
            addresses[*owner] = std::to_string(bytes[cursor]) + "." + std::to_string(bytes[cursor + 1]) + "." +
                                std::to_string(bytes[cursor + 2]) + "." + std::to_string(bytes[cursor + 3]);
        } else if (owner->size() > 22 && owner->substr(owner->size() - 22) == "._snapmaker._tcp.local") {
            auto& service = services[*owner];
            if (kind == 33 && length >= 7) {
                service.port = *word(cursor + 4);
                size_t host_cursor = cursor + 6;
                const auto host = name(host_cursor);
                if (!host || host_cursor > end) return {};
                service.host = boost::to_lower_copy(*host);
            } else if (kind == 16) {
                size_t at = cursor;
                while (at < end) {
                    const unsigned count = bytes[at++];
                    if (at + count > end) return {};
                    const std::string pair(reinterpret_cast<const char*>(bytes.data() + at), count);
                    const auto equal = pair.find('=');
                    if (equal != std::string::npos) service.fields[boost::to_lower_copy(pair.substr(0, equal))] = pair.substr(equal + 1);
                    at += count;
                }
            }
        }
        cursor = end;
    }
    std::vector<DiscoveredPrinter> result;
    for (const auto& entry : services) {
        auto fields = entry.second.fields;
        if (fields["sn"].empty() || fields["machine_type"].empty() || !entry.second.port) continue;
        DiscoveredPrinter p{"snapmaker", fields["device_name"].empty() ? fields["machine_type"] : fields["device_name"],
                            fields["machine_type"], fields["sn"], addresses.count(entry.second.host) ? addresses[entry.second.host] : sender,
                            entry.second.port, fields["link_mode"] == "lan", ""};
        if (valid(p)) result.push_back(std::move(p));
    }
    return result;
}
} // namespace Slic3r
