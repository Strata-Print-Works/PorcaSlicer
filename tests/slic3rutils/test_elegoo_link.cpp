#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ElegooLink.hpp"
#include "slic3r/Utils/ElegooLinkProtocol.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/SavedPrinterConnection.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace {
DiscoveredPrinter wrong_connection()
{
    return {"moonraker", "Printer", "", "fixture-serial", "192.0.2.10", 7125, true,
            "Elegoo Centauri Carbon 2 0.4 nozzle"};
}
DynamicPrintConfig configuration(const std::string& model)
{
    DynamicPrintConfig config;
    config.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    config.set_key_value("printer_model", new ConfigOptionString(model));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    return config;
}
}

TEST_CASE("Correcting a saved CC2 connection retains its identity and portable setup", "[ElegooLink]")
{
    auto printer = wrong_connection();
    const auto key = printer.key();
    const auto preset = printer.preset;
    const auto serial = printer.serial;
    auto config = configuration("Elegoo Centauri Carbon 2");
    // Project model/nozzle settings remain independent of the connection projection.
    const auto model = config.opt_string("printer_model");
    const auto nozzle = config.option("nozzle_diameter")->serialize();
    apply_saved_printer_connection(printer, "fixture-code", config);
    CHECK(config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htMoonraker);
    CHECK(config.opt_string("print_host") == "192.0.2.10:7125");
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", model));
    CHECK(printer.key() == key);
    CHECK(printer.port == 0);
    CHECK(printer.serial == serial);
    CHECK(printer.preset == preset);
    apply_saved_printer_connection(printer, "fixture-code", config);
    CHECK(config.opt_string("print_host") == "192.0.2.10");
    CHECK(config.opt_string("printhost_apikey") == "fixture-code");
    CHECK(config.opt_string("printer_model") == model);
    CHECK(config.option("nozzle_diameter")->serialize() == nozzle);
    CHECK(config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htElegooLink);
    const std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config)); // construction only; no socket or request
    REQUIRE(host);
    CHECK(std::string(host->get_name()) == "ElegooLink");
    CHECK(host->get_host() == "192.0.2.10");
    const auto serialized = printer.serialize();
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", model));
    CHECK(printer.serialize() == serialized);
    const auto restored = DiscoveredPrinter::deserialize(serialized);
    REQUIRE(restored);
    CHECK(restored->key() == key);
    CHECK(restored->serial == serial);
}

TEST_CASE("A slicing preset alone never reclassifies a generic Moonraker connection", "[ElegooLink]")
{
    const auto printer = wrong_connection();
    auto config = configuration("Elegoo Centauri Carbon 2");
    apply_saved_printer_connection(printer, "saved-code", config);
    CHECK(printer.vendor == "moonraker");
    CHECK(config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htMoonraker);
    auto invalid = printer;
    CHECK_FALSE(change_discovered_printer_connection(invalid, "elegoo", "Unidentified printer"));
    CHECK(invalid.serialize() == printer.serialize());
}

TEST_CASE("Explicit connection correction preserves custom ports and unrelated saved printers", "[ElegooLink]")
{
    std::vector<DiscoveredPrinter> saved{wrong_connection(), {"bambu", "Other", "N1", "other-serial", "192.0.2.11", 0, true, "A1 mini"}};
    const auto other = saved[1].serialize();
    saved[0].port = 8088;
    REQUIRE(change_discovered_printer_connection(saved[0], "elegoo", "Elegoo Centauri Carbon 2"));
    CHECK(saved_printer_device_id(saved[0]) == "192.0.2.10:8088");
    CHECK(saved[1].serialize() == other);
}

TEST_CASE("CC and CC2 use the existing Elegoo uploader without changing their model", "[ElegooLink]")
{
    for (const auto* model : {"Elegoo Centauri Carbon", "Elegoo Centauri Carbon 2"}) {
        auto printer = wrong_connection();
        REQUIRE(change_discovered_printer_connection(printer, "elegoo", model));
        auto config = configuration(model);
        apply_saved_printer_connection(printer, "", config);
        const std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config));
        REQUIRE(host);
        CHECK(std::string(host->get_name()) == "ElegooLink");
        CHECK(printer.model == model);
    }
    CHECK(elegoo_printer_type("Elegoo Centauri Carbon") == ElegooPrinterType::CC);
    CHECK(elegoo_printer_type("Elegoo Centauri Carbon 2") == ElegooPrinterType::CC2);
    CHECK(elegoo_printer_type("Elegoo Centauri Carbon 20") == ElegooPrinterType::Other);
}

TEST_CASE("An Elegoo host snapshot keeps its target when another printer is selected", "[ElegooLink]")
{
    auto printer = wrong_connection();
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", "Elegoo Centauri Carbon 2"));
    auto selected = configuration(printer.model);
    apply_saved_printer_connection(printer, "first-code", selected);
    auto snapshot = selected;
    const std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&snapshot));
    printer.address = "192.0.2.20";
    apply_saved_printer_connection(printer, "second-code", selected);
    REQUIRE(host);
    CHECK(host->get_host() == "192.0.2.10");
    CHECK(snapshot.opt_string("printhost_apikey") == "first-code");
    CHECK(selected.opt_string("print_host") == "192.0.2.20");
}

TEST_CASE("CC2 availability requires valid identification and distinguishes rejection from unavailable service", "[ElegooLink]")
{
    const auto ok = parse_elegoo_cc2_info(R"({"error_code":0,"system_info":{"sn":"fixture-serial"}})", 200);
    CHECK(ok.connection.status == PrinterLinkStatus::connected); // HTTP availability, not a registered control session
    CHECK(ok.serial == "fixture-serial");
    for (unsigned status : {401u, 403u}) {
        const auto rejected = parse_elegoo_cc2_info("", status);
        CHECK(rejected.connection.failure == PrinterLinkFailure::credentials_rejected);
        CHECK(rejected.serial.empty());
    }
    CHECK(parse_elegoo_cc2_info("", 0).connection.failure == PrinterLinkFailure::request_failed);
    CHECK(parse_elegoo_cc2_info("", 503).connection.failure == PrinterLinkFailure::http_error);
    CHECK(parse_elegoo_cc2_info(R"({"error_code":17,"message":"firmware refusal"})", 200).connection.failure == PrinterLinkFailure::service_rejected);
    for (const auto* body : {"{}", "<html>login</html>", "", R"({"error_code":0})", R"({"error_code":0,"system_info":{"sn":"bad/#"}})"}) {
        const auto invalid = parse_elegoo_cc2_info(body, 200);
        CHECK(invalid.connection.failure == PrinterLinkFailure::invalid_response);
        CHECK(invalid.serial.empty());
    }
    CHECK(elegoo_cc2_token("") == "123456"); // documented policy in the existing adapter; never invent auth success
    CHECK(elegoo_cc2_token("saved-code") == "saved-code");
}

TEST_CASE("Building the CC2 control URL encodes credentials without fetching identification", "[ElegooLink]")
{
    auto config = configuration("Elegoo Centauri Carbon 2");
    auto printer = wrong_connection();
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", "Elegoo Centauri Carbon 2"));
    printer.port = 8088;
    apply_saved_printer_connection(printer, "a&b?#", config);
    // This used to perform synchronous HTTP when the serial cache was empty.
    const auto url = ElegooLink::get_print_host_webui(&config);
    CHECK(url.find("file:") == 0);
    CHECK(url.find("access_code=a%26b%3F%23") != std::string::npos);
    CHECK(url.find("&ip=192.0.2.10") != std::string::npos);
    CHECK(url.find("&ip=192.0.2.10%3A8088") == std::string::npos); // MQTT uses its own vendor port
    CHECK(url.find("developer_mode") == std::string::npos);
}

TEST_CASE("Discovery merges retain a corrected saved identity", "[ElegooLink]")
{
    auto printer = wrong_connection();
    const auto key = printer.key();
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", "Elegoo Centauri Carbon 2"));
    std::vector<DiscoveredPrinter> saved{printer};
    auto discovered = printer;
    discovered.identity_key.clear();
    discovered.preset.clear();
    discovered.name = "Updated name";
    CHECK_FALSE(merge_discovered_printer(saved, discovered)); // false means the existing entry was updated
    REQUIRE(saved.size() == 1);
    CHECK(saved.front().key() == key);
    CHECK(saved.front().serial == printer.serial);
    CHECK(saved.front().preset == printer.preset);
}

TEST_CASE("Elegoo custom endpoints keep the HTTP port and bracket IPv6 addresses", "[ElegooLink]")
{
    auto printer = wrong_connection();
    REQUIRE(change_discovered_printer_connection(printer, "elegoo", "Elegoo Centauri Carbon 2"));
    printer.address = "2001:db8::10";
    printer.port = 8088;
    CHECK(saved_printer_device_id(printer) == "[2001:db8::10]:8088");
    auto config = configuration(printer.model);
    apply_saved_printer_connection(printer, "fixture", config);
    CHECK(config.opt_string("print_host_webui") == "http://[2001:db8::10]:8088");
}

TEST_CASE("Saved device ids bracket IPv6 hosts before every port", "[ElegooLink]")
{
    const auto id = [](std::string vendor, std::string address, unsigned port) {
        DiscoveredPrinter printer;
        printer.vendor = std::move(vendor);
        printer.address = std::move(address);
        printer.port = port;
        return saved_printer_device_id(printer);
    };
    CHECK(id("moonraker", "2001:db8::10", 0) == "[2001:db8::10]:7125");
    CHECK(id("moonraker", "2001:db8::10", 7126) == "[2001:db8::10]:7126");
    CHECK(id("snapmaker", "2001:db8::10", 0) == "[2001:db8::10]:7125");
    CHECK(id("octoprint", "2001:db8::10", 5000) == "[2001:db8::10]:5000");
    CHECK(id("elegoo", "[2001:db8::10]", 8088) == "[2001:db8::10]:8088");
    CHECK(id("moonraker", "192.0.2.10", 0) == "192.0.2.10:7125");
    CHECK(id("octoprint", "printer.local", 80) == "printer.local");
}

TEST_CASE("CC2 identification cannot overwrite another saved printer identity", "[ElegooLink]")
{
    const std::string response = R"({"error_code":0,"system_info":{"sn":"current-serial"}})";
    const auto mismatch = parse_elegoo_cc2_info(response, 200, "saved-serial");
    CHECK(mismatch.connection.status == PrinterLinkStatus::unreachable);
    CHECK(mismatch.connection.failure == PrinterLinkFailure::identity_mismatch);
    CHECK(mismatch.serial.empty());
    const auto same = parse_elegoo_cc2_info(response, 200, "current-serial");
    CHECK(same.connection.status == PrinterLinkStatus::connected);
    CHECK(same.serial == "current-serial");
}

#include "slic3r/Utils/ElegooCanvas.hpp"
#include <deque>

namespace {
class CanvasFixture final : public ElegooCanvasTransport {
public:
    std::deque<std::pair<uint8_t, std::vector<uint8_t>>> replies;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sent;
    void send(uint8_t h, const std::vector<uint8_t>& b) override { sent.emplace_back(h, b); }
    std::pair<uint8_t, std::vector<uint8_t>> receive() override {
        if (replies.empty()) throw std::runtime_error("fixture timeout");
        auto result = replies.front(); replies.pop_front(); return result;
    }
    void message(const std::string& topic, const nlohmann::json& value, uint8_t header = 48) {
        std::vector<uint8_t> body; elegoo_mqtt_string(body, topic);
        const auto text = value.dump(); body.insert(body.end(), text.begin(), text.end()); replies.emplace_back(header, body);
    }
    void begin() {
        replies.push_back({32, {0, 0}}); replies.push_back({144, {0, 1, 0, 0}});
        message("elegoo/fixture-serial/client_req/register_response", {{"client_id", "client"}, {"error", "ok"}});
    }
    void identity(const std::string& serial = "fixture-serial") {
        message("elegoo/fixture-serial/client/api_response", {{"id", 1}, {"result", {{"error_code", 0}, {"sn", serial}, {"machine_model", "Centauri Carbon 2"}}}});
    }
};
nlohmann::json canvas_fixture() {
    return {{"error_code", 0}, {"canvas_info", {{"canvas_list", nlohmann::json::array({
        {{"canvas_id", 1}, {"connected", 1}, {"tray_list", nlohmann::json::array({
            {{"tray_id", 0}, {"filament_type", "PLA"}, {"filament_color", "AABBCC"}},
            {{"tray_id", 1}}, {{"tray_id", 2}, {"filament_type", "PETG"}, {"filament_color", "unknown"}}})}},
        {{"canvas_id", 2}, {"connected", 0}, {"tray_list", nlohmann::json::array({{{"tray_id", 0}, {"filament_type", "ABS"}}})}}
    })}}}};
}
}
TEST_CASE("CANVAS sync accepts only correlated fresh readings after identity verification", "[ElegooLink]")
{
    CanvasFixture io; io.begin();
    io.message("elegoo/other/client/api_response", {{"id", 2}, {"result", canvas_fixture()}});
    io.message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", canvas_fixture()}}, 49); // retained
    io.identity();
    io.message("elegoo/fixture-serial/client/api_response", {{"id", 99}, {"result", canvas_fixture()}});
    io.message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", canvas_fixture()}});
    const auto result = read_elegoo_canvas(io, "fixture-serial", "fixture-code", "client");
    REQUIRE(result.error.empty()); REQUIRE(result.slots.size() == 3);
    CHECK(result.slots[0].color == "#AABBCC"); CHECK(result.slots[0].material == "PLA");
    CHECK(result.slots[1].material.empty()); CHECK(result.slots[1].color.empty()); CHECK(result.slots[2].color.empty());
    std::vector<int> methods;
    for (const auto& [h, body] : io.sent) {
        if (h != 48) continue;
        const size_t start = 2 + (size_t(body[0]) << 8) + body[1];
        const auto value = nlohmann::json::parse(body.begin() + start, body.end());
        if (value.contains("method")) methods.push_back(value["method"].get<int>());
    }
    CHECK(methods == std::vector<int>{1001, 2005}); // No printer writes or upload/start action.
}
TEST_CASE("CANVAS sync rejects credentials changed identity and registration refusal without importing slots", "[ElegooLink]")
{
    SECTION("Credentials") {
        CanvasFixture io; io.replies.push_back({32, {0, 5}});
        const auto result = read_elegoo_canvas(io, "fixture-serial", "bad", "client");
        CHECK_FALSE(result.error.empty()); CHECK(result.slots.empty()); CHECK(io.sent.size() == 1);
    }
    SECTION("Wrong device") {
        CanvasFixture io; io.begin(); io.identity("other-serial");
        const auto result = read_elegoo_canvas(io, "fixture-serial", "code", "client");
        CHECK_FALSE(result.error.empty()); CHECK(result.slots.empty()); CHECK(io.sent.size() == 4);
    }
    SECTION("Registration refused") {
        CanvasFixture io; io.replies.push_back({32, {0, 0}}); io.replies.push_back({144, {0, 1, 0, 0}});
        io.message("elegoo/fixture-serial/client_req/register_response", {{"client_id", "client"}, {"error", "denied"}});
        const auto result = read_elegoo_canvas(io, "fixture-serial", "code", "client");
        CHECK_FALSE(result.error.empty()); CHECK(result.slots.empty()); CHECK(io.sent.size() == 3);
    }
    SECTION("Missing serial") {
        CanvasFixture io; const auto result = read_elegoo_canvas(io, "", "code", "client");
        CHECK_FALSE(result.error.empty()); CHECK(io.sent.empty());
    }
    SECTION("Firmware unsupported") {
        CanvasFixture io; io.begin(); io.identity();
        io.message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", {{"error_code", 7}}}});
        const auto result = read_elegoo_canvas(io, "fixture-serial", "code", "client");
        CHECK_FALSE(result.error.empty()); CHECK(result.slots.empty());
    }
}
TEST_CASE("Confirmed nozzle correction preserves plate edits materials and connection settings", "[ElegooLink]")
{
    auto plate = configuration("Elegoo Centauri Carbon 2");
    plate.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.2}));
    plate.set_key_value("layer_height", new ConfigOptionFloat(0.12));
    plate.set_key_value("filament_colour", new ConfigOptionStrings({"#AABBCC", "#112233"}));
    plate.set_key_value("print_host", new ConfigOptionString("fixture-local"));
    const auto unrelated = plate;
    const auto target = configuration("Elegoo Centauri Carbon 2");
    REQUIRE(apply_confirmed_printer_nozzle(plate, target, "Elegoo Centauri Carbon 2 0.4 nozzle"));
    CHECK_THAT(plate.opt_float("nozzle_diameter", 0u), Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK_THAT(plate.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    CHECK(plate.option("filament_colour")->serialize() == unrelated.option("filament_colour")->serialize());
    CHECK(plate.opt_string("print_host") == unrelated.opt_string("print_host"));
    CHECK_THAT(unrelated.opt_float("nozzle_diameter", 0u), Catch::Matchers::WithinAbs(0.2, 1e-9));
    const auto corrected = plate;
    REQUIRE(apply_confirmed_printer_nozzle(plate, target, "Elegoo Centauri Carbon 2 0.4 nozzle")); CHECK(plate == corrected);
    CHECK_FALSE(apply_confirmed_printer_nozzle(plate, configuration("Other printer"), "Other")); CHECK(plate == corrected);
}
TEST_CASE("CANVAS review excludes unknown materials and rejects stale plate or route results", "[ElegooLink]")
{
    const ElegooCanvasSlot known{0, 1, "PLA", "#123456"};
    CHECK(elegoo_canvas_material_matches(known, "PLA"));
    CHECK(elegoo_canvas_material_matches(known, "pla"));
    CHECK_FALSE(elegoo_canvas_material_matches(known, "PLA-CF"));
    CHECK_FALSE(elegoo_canvas_material_matches({0, 1, "", "#123456"}, "PLA"));
    CHECK_FALSE(elegoo_canvas_material_matches({0, 1, "PLA", ""}, "PLA"));
    const ElegooSyncTarget request{12, "edited-plate-a", "printer-a"};
    CHECK(request == ElegooSyncTarget{12, "edited-plate-a", "printer-a"});
    CHECK_FALSE(request == ElegooSyncTarget{13, "edited-plate-a", "printer-a"});
    CHECK_FALSE(request == ElegooSyncTarget{12, "new-material-edits", "printer-a"});
    CHECK_FALSE(request == ElegooSyncTarget{12, "edited-plate-a", "printer-b"});
}
TEST_CASE("CANVAS RGB color formats remain selectable without accepting malformed colors", "[ElegooLink]")
{
    for (const auto* color : {"0xAABBCC", "0XaAbBcC", "#AABBCC", "aabbcc"}) {
        auto data = canvas_fixture();
        data["canvas_info"]["canvas_list"][0]["tray_list"][0]["filament_color"] = color;
        const auto slots = parse_elegoo_canvas(data);
        REQUIRE_FALSE(slots.empty());
        CHECK(slots.front().color.size() == 7);
        CHECK(slots.front().color.front() == '#');
        CHECK(elegoo_canvas_material_matches(slots.front(), "PLA"));
    }
    for (const auto* color : {"0x", "0x#AABBCC", "0xAABBCG", "0xAABBCCDD", "#AABB", "unknown"}) {
        auto data = canvas_fixture();
        data["canvas_info"]["canvas_list"][0]["tray_list"][0]["filament_color"] = color;
        const auto slots = parse_elegoo_canvas(data);
        REQUIRE_FALSE(slots.empty());
        CHECK(slots.front().color.empty());
        CHECK_FALSE(elegoo_canvas_material_matches(slots.front(), "PLA"));
    }
}

TEST_CASE("Malformed empty and duplicate CANVAS replies do not invent loaded filaments", "[ElegooLink]")
{
    auto data = canvas_fixture();
    data["canvas_info"]["canvas_list"] = nlohmann::json::array();
    CHECK(parse_elegoo_canvas(data).empty());
    data = canvas_fixture();
    data["canvas_info"]["canvas_list"][0]["tray_list"].push_back(data["canvas_info"]["canvas_list"][0]["tray_list"][0]);
    CHECK_THROWS(parse_elegoo_canvas(nlohmann::json::parse(data.dump())));
    CanvasFixture io; io.begin(); io.identity();
    io.message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", {{"error_code", "wrong-type"}}}});
    const auto result = read_elegoo_canvas(io, "fixture-serial", "code", "client");
    CHECK_FALSE(result.error.empty()); CHECK(result.slots.empty());
}
TEST_CASE("Canceled CANVAS requests open no transport and empty slots retain no stale material", "[ElegooLink]")
{
    const auto canceled = fetch_elegoo_canvas("", "fixture-serial", "code", [] { return true; });
    CHECK_FALSE(canceled.error.empty()); CHECK(canceled.slots.empty());
    const auto no_serial = fetch_elegoo_canvas("", "", "code", [] { return false; });
    CHECK_FALSE(no_serial.error.empty()); CHECK(no_serial.slots.empty());
    auto data = canvas_fixture();
    data["canvas_info"]["canvas_list"][0]["tray_list"][0]["status"] = 0;
    const auto slots = parse_elegoo_canvas(nlohmann::json::parse(data.dump()));
    REQUIRE(slots.size() == 3); CHECK(slots[0].material.empty()); CHECK(slots[0].color.empty());
}
