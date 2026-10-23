#include <catch2/catch_test_macros.hpp>
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/AppConfig.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Address-based destinations migrate to stable local random identities", "[DiscoveredPrinter]")
{
    AppConfig config;
    for (const auto& legacy : {std::string("scanned:moonraker:192.168.1.60:7125"),
                               std::string("scanned:octoprint:fd00::60:80")}) {
        REQUIRE(address_based_printer_destination(legacy));
        const auto portable = portable_printer_destination(legacy, config);
        CHECK(portable.find("scanned-id:") == 0);
        CHECK_FALSE(address_based_printer_destination(portable));
        CHECK(portable_printer_destination(legacy, config) == portable);
        CHECK(portable_printer_destination(portable, config) == portable);
        AppConfig another_computer;
        CHECK(portable_printer_destination(legacy, another_computer) != portable);
        AppConfig reopened = config;
        CHECK(portable_printer_destination(legacy, reopened) == portable);
        // Editing an address retains the portable identity through a local alias.
        const std::string changed = "scanned:moonraker:192.168.1.61:7125";
        config.set("scanned_printer_destinations", changed, portable);
        CHECK(portable_printer_destination(changed, config) == portable);
    }
    const std::string serial = "scanned:bambu:TEST-PRINTER";
    CHECK_FALSE(address_based_printer_destination(serial));
    CHECK(portable_printer_destination(serial, config) == serial);
    for (const auto& native : {std::string("192.168.1.60"), std::string("192.168.1.60:7125"),
                              std::string("fd00::60"), std::string("[fd00::60]:7125"),
                              std::string("printer.local:7125"), std::string("octopi.local"), std::string("printer")}) {
        REQUIRE(address_based_printer_destination(native));
        const auto portable = portable_printer_destination(native, config);
        CHECK(local_printer_destination(portable, config) == native);
        AppConfig another_computer;
        CHECK(local_printer_destination(portable, another_computer).empty());
    }
}

namespace {
const std::string notice = "NOTIFY * HTTP/1.1\r\nNT: urn:bambulab-com:device:3dprinter:1\r\n"
    "DevModel.bambu.com: N9\r\nDevSN.bambu.com: TEST-PRINTER\r\nDevName.bambu.com: Workshop\r\n"
    "DevConnect.bambu.com: lan\r\nLocation: 192.168.1.9\r\n\r\n";

std::vector<unsigned char> snapmaker_notice(const std::string& serial = "SN-TEST")
{
    std::vector<unsigned char> packet{0, 0, 0x84, 0, 0, 0, 0, 3, 0, 0, 0, 0};
    const auto dns_name = [](const std::string& name) {
        std::vector<unsigned char> bytes;
        size_t start = 0;
        while (start < name.size()) {
            const auto dot = name.find('.', start);
            const auto end = dot == std::string::npos ? name.size() : dot;
            bytes.push_back(static_cast<unsigned char>(end - start));
            bytes.insert(bytes.end(), name.begin() + start, name.begin() + end);
            start = end + 1;
        }
        bytes.push_back(0);
        return bytes;
    };
    const auto record = [&packet, &dns_name](const std::string& owner, unsigned kind, const std::vector<unsigned char>& data) {
        const auto name = dns_name(owner);
        packet.insert(packet.end(), name.begin(), name.end());
        packet.insert(packet.end(), {0, static_cast<unsigned char>(kind), 0, 1, 0, 0, 0, 120,
                                    static_cast<unsigned char>(data.size() >> 8), static_cast<unsigned char>(data.size() & 255)});
        packet.insert(packet.end(), data.begin(), data.end());
    };
    std::vector<unsigned char> srv{0, 0, 0, 0, 0x07, 0x5c};
    const auto host = dns_name("U1-test.local");
    srv.insert(srv.end(), host.begin(), host.end());
    record("U1._snapmaker._tcp.local", 33, srv);
    std::vector<unsigned char> txt;
    for (const auto& field : {std::string("machine_type=Snapmaker U1"), std::string("device_name=Workshop U1"),
                              "sn=" + serial, std::string("link_mode=lan")}) {
        txt.push_back(static_cast<unsigned char>(field.size()));
        txt.insert(txt.end(), field.begin(), field.end());
    }
    record("U1._snapmaker._tcp.local", 16, txt);
    record("U1-test.local", 1, {192, 168, 1, 60});
    return packet;
}
}

TEST_CASE("A Snapmaker service keeps its own identity address and local mode", "[DiscoveredPrinter]")
{
    const auto printers = parse_snapmaker_printer_notice(snapmaker_notice(), "192.168.1.61");
    REQUIRE(printers.size() == 1);
    CHECK(printers[0].name == "Workshop U1");
    CHECK(printers[0].model == "Snapmaker U1");
    CHECK(printers[0].serial == "SN-TEST");
    CHECK(printers[0].address == "192.168.1.60");
    CHECK(printers[0].port == 1884);
    CHECK(printers[0].local_control);
}

TEST_CASE("Truncated cyclic or anonymous DNS replies never become printers", "[DiscoveredPrinter]")
{
    auto packet = snapmaker_notice();
    packet.pop_back();
    CHECK(parse_snapmaker_printer_notice(packet, "192.168.1.61").empty());
    CHECK(parse_snapmaker_printer_notice(snapmaker_notice(""), "192.168.1.61").empty());
    packet = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xc0, 12};
    CHECK(parse_snapmaker_printer_notice(packet, "192.168.1.61").empty());
}

TEST_CASE("A Bambu reply identifies its own machine without guessing a nozzle", "[DiscoveredPrinter]")
{
    const auto printer = parse_bambu_printer_notice(notice, "192.168.1.20");
    REQUIRE(printer);
    CHECK(printer->serial == "TEST-PRINTER");
    CHECK(printer->model == "N9");
    CHECK(printer->address == "192.168.1.20");
    CHECK(printer->local_control);
    CHECK(printer->preset.empty());
    const auto reply = parse_bambu_printer_notice("HTTP/1.1 200 OK\r\nST:" + notice.substr(notice.find("urn:")), "192.168.1.20");
    REQUIRE(reply);
    CHECK(reply->key() == printer->key());
}

TEST_CASE("Other network traffic and incomplete notices do not become printers", "[DiscoveredPrinter]")
{
    CHECK_FALSE(parse_bambu_printer_notice("M-SEARCH * HTTP/1.1\r\n" + notice, "192.168.1.20"));
    CHECK_FALSE(parse_bambu_printer_notice("NOTIFY * HTTP/1.1\r\nNT: urn:bambulab-com:device:3dprinter:1\r\n", "192.168.1.20"));
    CHECK_FALSE(parse_bambu_printer_notice(notice, "http://example.com"));
    CHECK_FALSE(parse_bambu_printer_notice(notice, "239.255.255.250"));
    CHECK_FALSE(parse_bambu_printer_notice(std::string(8193, 'x'), "192.168.1.20"));
}

TEST_CASE("A cloud-bound printer is found without promising a local connection", "[DiscoveredPrinter]")
{
    auto cloud = notice;
    cloud.replace(cloud.find("lan"), 3, "cloud");
    const auto printer = parse_bambu_printer_notice(cloud, "192.168.1.20");
    REQUIRE(printer);
    CHECK_FALSE(printer->local_control);
}

TEST_CASE("A saved printer follows its serial to a new address and retains its profile", "[DiscoveredPrinter]")
{
    auto first = *parse_bambu_printer_notice(notice, "192.168.1.20");
    first.preset = "Bambu Lab A2L 0.4 nozzle";
    std::vector<DiscoveredPrinter> printers{first};
    CHECK_FALSE(merge_discovered_printer(printers, *parse_bambu_printer_notice(notice, "192.168.1.21")));
    REQUIRE(printers.size() == 1);
    CHECK(printers[0].address == "192.168.1.21");
    CHECK(printers[0].preset == first.preset);
    CHECK(printers[0].key() == first.key());
}

TEST_CASE("A generic Flashforge rescan retains the explicitly selected model and nozzle", "[DiscoveredPrinter]")
{
    DiscoveredPrinter saved{"flashforge", "Workshop", "Flashforge Adventurer 5M", "SERIAL-TEST",
                            "192.0.2.10", 8899, true, "Flashforge Adventurer 5M 0.4 nozzle"};
    std::vector<DiscoveredPrinter> printers{saved};
    DiscoveredPrinter scanned = saved;
    scanned.model = "Flashforge";
    scanned.preset.clear();
    scanned.address = "192.0.2.11";
    CHECK_FALSE(merge_discovered_printer(printers, scanned));
    REQUIRE(printers.size() == 1);
    CHECK(printers[0].model == saved.model);
    CHECK(printers[0].preset == saved.preset);
    CHECK(printers[0].address == scanned.address);
}

TEST_CASE("Two vendors or serials remain separate devices at the same address", "[DiscoveredPrinter]")
{
    auto first = *parse_bambu_printer_notice(notice, "192.168.1.20");
    auto second = first;
    second.serial = "SECOND";
    std::vector<DiscoveredPrinter> printers{first};
    CHECK(merge_discovered_printer(printers, second));
    second.vendor = "snapmaker";
    CHECK(merge_discovered_printer(printers, second));
    CHECK(printers.size() == 3);
}

TEST_CASE("Saved discovery keeps no credentials and rejects unusable addresses", "[DiscoveredPrinter]")
{
    const auto first = *parse_bambu_printer_notice(notice, "192.168.1.20");
    auto json = nlohmann::json::parse(first.serialize());
    json["access_code"] = "fixture-secret";
    const auto restored = DiscoveredPrinter::deserialize(json.dump());
    REQUIRE(restored);
    CHECK(restored->serialize().find("fixture-secret") == std::string::npos);
    CHECK(restored->key() == first.key());
    json["port"] = 99999;
    CHECK_FALSE(DiscoveredPrinter::deserialize(json.dump()));
    json["port"] = 8883;
    json["address"] = "0.0.0.0";
    CHECK_FALSE(DiscoveredPrinter::deserialize(json.dump()));
    json["address"] = "192.168.1.20";
    json["name"] = "Printer\nInjected";
    CHECK_FALSE(DiscoveredPrinter::deserialize(json.dump()));
}

TEST_CASE("Local MQTT authentication sends a complete clean-session connect packet", "[DiscoveredPrinter]")
{
    const auto packet = bambu_connect_packet("test-client", "12345678");
    REQUIRE(packet.size() > 12);
    CHECK(packet[0] == 0x10);
    CHECK(packet[1] == packet.size() - 2);
    CHECK(packet[9] == 0xc2);
    const std::string body(packet.begin() + 2, packet.end());
    CHECK(body.find("bblp") != std::string::npos);
    CHECK(body.find("test-client") != std::string::npos);
    CHECK(body.substr(body.size() - 8) == "12345678");
    const auto long_packet = bambu_connect_packet(std::string(160, 'x'), "12345678");
    CHECK((long_packet[1] & 0x80) != 0);
    const size_t length = (long_packet[1] & 0x7f) + size_t(long_packet[2]) * 128;
    CHECK(length == long_packet.size() - 3);
}
