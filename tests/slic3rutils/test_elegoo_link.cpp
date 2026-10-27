#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ElegooLink.hpp"
#include "slic3r/Utils/ElegooLinkProtocol.hpp"
#include "slic3r/Utils/ElegooCanvasPresets.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
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
    int requests_at_commit = -1;
    void send(uint8_t h, const std::vector<uint8_t>& b) override { sent.emplace_back(h, b); }
    std::pair<uint8_t, std::vector<uint8_t>> receive() override {
        if (replies.empty()) throw std::runtime_error("fixture timeout");
        auto result = replies.front(); replies.pop_front(); return result;
    }
    void commit() override { requests_at_commit = int(requests().size()); }
    // Published requests to the printer, in order.
    std::vector<nlohmann::json> requests() const {
        std::vector<nlohmann::json> result;
        for (const auto& [h, body] : sent) {
            if (h != 48) continue;
            const size_t start = 2 + (size_t(body[0]) << 8) + body[1];
            auto value = nlohmann::json::parse(body.begin() + start, body.end());
            if (value.contains("method")) result.push_back(std::move(value));
        }
        return result;
    }
    std::vector<int> methods() const {
        std::vector<int> result;
        for (const auto& request : requests()) result.push_back(request["method"].get<int>());
        return result;
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
    void status(const nlohmann::json& external_device = {{"type", 0}}) {
        nlohmann::json result{{"error_code", 0}, {"machine_status", {{"status", 1}}}};
        if (!external_device.is_null()) result["external_device"] = external_device;
        message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", result}});
    }
    void start_reply(const nlohmann::json& result, uint8_t header = 48) {
        message("elegoo/fixture-serial/client/api_response", {{"id", 3}, {"method", 1020}, {"result", result}}, header);
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
    CHECK(io.methods() == std::vector<int>{1001, 2005}); // No printer writes or upload/start action.
    CHECK(io.requests_at_commit == -1);
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

namespace {
struct CanvasFilaments : PresetCollection {
    CanvasFilaments() : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                                        static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults())) {}

    void add(const std::string& name, const std::string& type, bool targeted = false, bool compatible = true) {
        DynamicPrintConfig config(default_preset().config);
        config.set_key_value("filament_type", new ConfigOptionStrings({type}));
        config.set_key_value("compatible_printers", new ConfigOptionStrings(targeted
            ? std::vector<std::string>{"Elegoo Centauri Carbon 2 0.4 nozzle"} : std::vector<std::string>{}));
        auto& preset = load_preset("", name, config, false);
        preset.is_system = true;
        preset.is_default = false;
        preset.is_visible = false;
        preset.is_compatible = compatible;
    }
};
}

TEST_CASE("CANVAS PLA automatically selects a hidden printer-specific generic over the library and branded presets", "[ElegooLink]")
{
    CanvasFilaments filaments;
    filaments.add("Generic PLA @System", "PLA");
    filaments.add("Generic PLA @Elegoo Centauri", "PLA", true);
    filaments.add("Bambu PLA Basic @System", "PLA");
    filaments.add("Generic PLA Silk @Elegoo Centauri", "PLA", true);
    const auto* match = match_elegoo_canvas_preset({0, 0, "PLA", "#A03BF7"}, filaments);
    REQUIRE(match);
    CHECK(match->name == "Generic PLA @Elegoo Centauri");
    for (const auto* color : {"#A03BF7", "#7C4C00", "#000000"}) {
        const ElegooCanvasSlot slot{0, 1, "pla", color};
        CHECK(match_elegoo_canvas_preset(slot, filaments) == match);
    }
}

TEST_CASE("CANVAS falls back to the compatible library generic without inventing a filament brand or subtype", "[ElegooLink]")
{
    CanvasFilaments filaments;
    filaments.add("Bambu PLA Basic @System", "PLA");
    filaments.add("Generic PLA Silk @System", "PLA");
    filaments.add("Generic PLA @Elegoo Centauri", "PLA", true, false);
    const ElegooCanvasSlot slot{0, 0, "PLA", "#000000"};
    CHECK_FALSE(match_elegoo_canvas_preset(slot, filaments));
    filaments.add("Generic PLA @System", "PLA");
    const auto* match = match_elegoo_canvas_preset(slot, filaments);
    REQUIRE(match);
    CHECK(match->name == "Generic PLA @System");
    CHECK_FALSE(match_elegoo_canvas_preset({0, 0, "PLA-CF", "#000000"}, filaments));
    CHECK_FALSE(match_elegoo_canvas_preset({0, 0, "", "#000000"}, filaments));
    CHECK_FALSE(match_elegoo_canvas_preset({0, 0, "PLA", ""}, filaments));
}

TEST_CASE("CANVAS leaves equally preferred generic matches for review", "[ElegooLink]")
{
    CanvasFilaments filaments;
    filaments.add("Generic PLA @System", "PLA");
    filaments.add("Generic PLA @Elegoo Centauri", "PLA", true);
    filaments.add("Generic PLA @Another tune", "PLA", true);
    CHECK_FALSE(match_elegoo_canvas_preset({0, 0, "PLA", "#123456"}, filaments));
}

TEST_CASE("CANVAS ignores explicitly empty trays while retaining unknown loaded spools for review", "[ElegooLink]")
{
    auto data = canvas_fixture();
    auto& trays = data["canvas_info"]["canvas_list"][0]["tray_list"];
    trays[0]["status"] = 1;
    trays[1]["status"] = 0;
    trays[2]["status"] = 2;
    const auto slots = parse_elegoo_canvas(data);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].color == "#AABBCC");
    CHECK(slots[1].tray == 2);
    CHECK(slots[1].color.empty());
    CHECK(slots[1].material == "PETG");
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
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].tray == 1);
    CHECK(slots[1].tray == 2);
}

namespace {
ElegooPrintJob single_filament_job() { return {"plate_1.gcode", true, true, 1}; }
}
TEST_CASE("Upload and print sends one start request for the uploaded file with the chosen options", "[ElegooLink]")
{
    CanvasFixture io; io.begin(); io.identity(); io.status();
    io.start_reply({{"error_code", 0}});
    const auto result = start_elegoo_print(io, "fixture-serial", "fixture-code", "client", single_filament_job());
    CHECK(result.started); CHECK(result.sent); CHECK(result.error.empty());
    const auto requests = io.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[1]["method"] == 1002);
    CHECK(io.requests_at_commit == 2); // Cancel stops applying only right before the start request.
    const auto& start = requests[2];
    CHECK(start["method"] == 1020);
    CHECK(start["id"] == 3);
    const auto& params = start["params"];
    CHECK(params["storage_media"] == "local");
    CHECK(params["filename"] == "plate_1.gcode"); // The name the upload used, without a folder.
    CHECK(params["config"]["printer_check"] == true);
    CHECK(params["config"]["print_layout"] == "B");
    CHECK(params["config"]["delay_video"] == false);
    CHECK_FALSE(params["config"].contains("slot_map")); // No CANVAS slot is chosen for the user.
    const auto textured = elegoo_print_start_params({"plate_1.gcode", false, false, 1});
    CHECK(textured["config"]["print_layout"] == "A");
    CHECK(textured["config"]["printer_check"] == false);
}
TEST_CASE("A printer that refuses to start reports why and is not asked again", "[ElegooLink]")
{
    const int code = GENERATE(109, 1009, 1021, 1026, 4242);
    CanvasFixture io; io.begin(); io.identity(); io.status();
    io.start_reply({{"error_code", code}});
    const auto result = start_elegoo_print(io, "fixture-serial", "fixture-code", "client", single_filament_job());
    CHECK_FALSE(result.started);
    CHECK(result.sent);
    CHECK(result.printer_code == code);
    CHECK_FALSE(result.error.empty());
    CHECK(io.methods() == std::vector<int>{1001, 1002, 1020});
}
TEST_CASE("An unconfirmed start is reported as uncertain and never repeated", "[ElegooLink]")
{
    CanvasFixture io; io.begin(); io.identity(); io.status();
    SECTION("No reply before the deadline") {}
    SECTION("Reply to another request") {
        io.message("elegoo/fixture-serial/client/api_response", {{"id", 99}, {"result", {{"error_code", 0}}}});
    }
    SECTION("Reply for another client") {
        io.message("elegoo/fixture-serial/other/api_response", {{"id", 3}, {"result", {{"error_code", 0}}}});
    }
    SECTION("Retained reply") { io.start_reply({{"error_code", 0}}, 49); }
    SECTION("Reply for another action") {
        io.message("elegoo/fixture-serial/client/api_response", {{"id", 3}, {"method", 1021}, {"result", {{"error_code", 0}}}});
    }
    SECTION("Reply without a result code") { io.start_reply({{"message", "accepted"}}); }
    const auto result = start_elegoo_print(io, "fixture-serial", "fixture-code", "client", single_filament_job());
    CHECK_FALSE(result.started);
    CHECK(result.sent);
    CHECK(result.printer_code == 0);
    CHECK_FALSE(result.error.empty());
    CHECK(io.methods() == std::vector<int>{1001, 1002, 1020});
}
TEST_CASE("A CC2 start sends no command without one confirmed filament source", "[ElegooLink]")
{
    CanvasFixture io;
    auto job = single_filament_job();
    std::vector<int> expected{1001, 1002};
    SECTION("Several filaments") { job.filaments = 2; expected.clear(); }
    SECTION("Unknown filaments") { job.filaments = 0; expected.clear(); }
    SECTION("File in a folder") { job.filename = "jobs/plate_1.gcode"; expected.clear(); }
    SECTION("CANVAS attached") { io.begin(); io.identity(); io.status({{"type", 1}}); }
    SECTION("Accessories not reported") { io.begin(); io.identity(); io.status(nullptr); }
    SECTION("Status refused") {
        io.begin(); io.identity();
        io.message("elegoo/fixture-serial/client/api_response", {{"id", 2}, {"result", {{"error_code", 1009}}}});
    }
    SECTION("Different printer") { io.begin(); io.identity("other-serial"); expected = {1001}; }
    const auto result = start_elegoo_print(io, "fixture-serial", "fixture-code", "client", job);
    CHECK_FALSE(result.started);
    CHECK_FALSE(result.sent);
    CHECK_FALSE(result.error.empty());
    CHECK(io.methods() == expected);
    CHECK(io.requests_at_commit == -1);
    if (expected.empty()) CHECK(io.sent.empty());
}
TEST_CASE("Only an explicit report of no CANVAS lets a print start from the print head", "[ElegooLink]")
{
    for (const auto& type : {nlohmann::json(0), nlohmann::json("0"), nlohmann::json("")})
        CHECK(elegoo_status_without_canvas({{"external_device", {{"type", type}}}}));
    for (const auto& type : {nlohmann::json(1), nlohmann::json("canvas"), nlohmann::json(nullptr), nlohmann::json(false)})
        CHECK_FALSE(elegoo_status_without_canvas({{"external_device", {{"type", type}}}}));
    CHECK_FALSE(elegoo_status_without_canvas({{"external_device", nlohmann::json::object()}}));
    CHECK_FALSE(elegoo_status_without_canvas(nlohmann::json::object()));
}
TEST_CASE("Upload only and failed uploads never ask a CC2 to start", "[ElegooLink]")
{
    int starts = 0;
    const auto start = [&] { ++starts; return true; };
    CHECK(elegoo_upload_then_start(false, [] { return true; }, start));
    CHECK_FALSE(elegoo_upload_then_start(false, [] { return false; }, start));
    CHECK_FALSE(elegoo_upload_then_start(true, [] { return false; }, start));
    CHECK(starts == 0);
    CHECK(elegoo_upload_then_start(true, [] { return true; }, start));
    CHECK(starts == 1);
    // Rejected jobs and canceled starts return before any transport is opened.
    CHECK_FALSE(send_elegoo_print_start("", "fixture-serial", "code", {"plate_1.gcode", false, false, 2}, [] { return false; }).sent);
    const auto canceled = send_elegoo_print_start("", "fixture-serial", "code", single_filament_job(), [] { return true; });
    CHECK_FALSE(canceled.started);
    CHECK_FALSE(canceled.sent);
}
