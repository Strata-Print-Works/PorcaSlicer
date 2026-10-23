#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <cctype>
#include <utility>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {
#ifndef L
// Errors keep their untranslated catalog text; the display site translates
// them. L() only marks that text for extraction, without a GUI dependency.
inline const char* L(const char* text) { return text; }
#endif
struct ElegooCanvasSlot {
    unsigned canvas, tray;
    std::string material, color;
};
inline bool elegoo_canvas_material_matches(const ElegooCanvasSlot& slot, const std::string& type)
{
    return !slot.material.empty() && !slot.color.empty() && slot.material.size() == type.size() &&
        std::equal(slot.material.begin(), slot.material.end(), type.begin(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}
struct ElegooSyncTarget {
    int plate;
    std::string setup, connection;
    bool operator==(const ElegooSyncTarget& other) const {
        return plate == other.plate && setup == other.setup && connection == other.connection;
    }
};
inline bool elegoo_canvas_serial_valid(const std::string& serial) {
    return !serial.empty() && serial.size() <= 128 && serial.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}
struct ElegooCanvasResult {
    std::vector<ElegooCanvasSlot> slots;
    std::string error;
};
// One-shot, read-only request. The transport owns deadlines and cancellation;
// protocol decisions below are also exercised with socket-free packet fixtures.
class ElegooCanvasTransport {
public:
    virtual ~ElegooCanvasTransport() = default;
    virtual void send(uint8_t header, const std::vector<uint8_t>& body) = 0;
    virtual std::pair<uint8_t, std::vector<uint8_t>> receive() = 0;
};
inline void elegoo_mqtt_string(std::vector<uint8_t>& bytes, const std::string& value)
{
    if (value.size() > 65535) throw std::runtime_error(L("Invalid printer connection details."));
    bytes.push_back(uint8_t(value.size() >> 8)); bytes.push_back(uint8_t(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
inline std::vector<ElegooCanvasSlot> parse_elegoo_canvas(const nlohmann::json& result)
{
    if (!result.is_object() || !result.contains("error_code") || !result["error_code"].is_number_integer() || result["error_code"] != 0 || !result.contains("canvas_info") ||
        !result["canvas_info"].is_object()) throw std::runtime_error(L("The printer did not report CANVAS filaments."));
    const auto& info = result["canvas_info"];
    if (!info.contains("canvas_list") || !info["canvas_list"].is_array() || info["canvas_list"].size() > 16)
        throw std::runtime_error(L("The printer sent an unsupported CANVAS reply."));
    std::vector<ElegooCanvasSlot> slots;
    std::set<std::pair<unsigned, unsigned>> seen;
    for (const auto& unit : info["canvas_list"]) {
        if (!unit.is_object() || !unit.contains("connected") || (unit["connected"] != 1 && unit["connected"] != true)) continue;
        if (!unit.contains("canvas_id") || !unit["canvas_id"].is_number_integer() || unit["canvas_id"] < 0 || unit["canvas_id"] > 255 ||
            !unit.contains("tray_list") || !unit["tray_list"].is_array() || unit["tray_list"].size() > 16)
            throw std::runtime_error(L("The printer sent an unsupported CANVAS reply."));
        const auto canvas = unit["canvas_id"].get<unsigned>();
        for (const auto& slot : unit["tray_list"]) {
            if (!slot.is_object() || !slot.contains("tray_id") || !slot["tray_id"].is_number_integer() || slot["tray_id"] < 0 || slot["tray_id"] > 255)
                throw std::runtime_error(L("The printer sent an unsupported CANVAS reply."));
            const auto tray = slot["tray_id"].get<unsigned>();
            if (!seen.insert({canvas, tray}).second) throw std::runtime_error(L("The printer reported duplicate filament slots."));
            std::string material, color;
            if (slot.contains("filament_type") && slot["filament_type"].is_string()) material = slot["filament_type"].get<std::string>();
            if (material.size() > 64 || std::any_of(material.begin(), material.end(), [](unsigned char c) { return c < 32; })) material.clear();
            if (slot.contains("filament_color") && slot["filament_color"].is_string()) color = slot["filament_color"].get<std::string>();
            // Match the vendor panel's support for CANVAS 0xRRGGBB colors.
            if (color.size() >= 2 && color[0] == '0' && (color[1] == 'x' || color[1] == 'X')) color.erase(0, 2);
            else if (!color.empty() && color.front() == '#') color.erase(0, 1);
            if (color.size() != 6 || color.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) color.clear();
            else color.insert(0, "#");
            if (slot.contains("status") && (!slot["status"].is_number_integer() ||
                (slot["status"] != 1 && slot["status"] != 2))) { material.clear(); color.clear(); }
            slots.push_back({canvas, tray, material, color});
        }
    }
    return slots;
}
inline ElegooCanvasResult read_elegoo_canvas(ElegooCanvasTransport& io, const std::string& serial,
                                             const std::string& secret, const std::string& client)
{
    try {
        if (!elegoo_canvas_serial_valid(serial) ||
            client.empty() || client.size() > 128 || secret.size() > 256)
            throw std::runtime_error(L("Check this printer's serial number and LAN access code."));
        const auto base = "elegoo/" + serial + "/";
        const auto response = base + client + "/api_response";
        const auto registration_id = client + "_req";
        const auto registration = base + registration_id + "/register_response";
        std::vector<uint8_t> connect;
        elegoo_mqtt_string(connect, "MQTT");
        connect.insert(connect.end(), {4, 194, 0, 20});
        elegoo_mqtt_string(connect, client); elegoo_mqtt_string(connect, "elegoo"); elegoo_mqtt_string(connect, secret);
        io.send(16, connect); std::fill(connect.begin(), connect.end(), 0);
        auto ack = io.receive();
        if (ack.first == 32 && ack.second.size() == 2 && (ack.second[1] == 4 || ack.second[1] == 5))
            throw std::runtime_error(L("The printer rejected the LAN access code. Check its LAN-only settings."));
        if (ack.first != 32 || ack.second != std::vector<uint8_t>({0, 0})) throw std::runtime_error(L("The printer refused the connection."));
        std::vector<uint8_t> subscribe{0, 1};
        for (const auto& topic : {response, registration}) { elegoo_mqtt_string(subscribe, topic); subscribe.push_back(0); }
        io.send(130, subscribe);
        auto publish = [&](const std::string& topic, const nlohmann::json& value) {
            std::vector<uint8_t> bytes; elegoo_mqtt_string(bytes, topic);
            const auto text = value.dump(); bytes.insert(bytes.end(), text.begin(), text.end()); io.send(48, bytes);
        };
        // SUBACK may be interleaved with notifications. No retained messages are
        // accepted as a fresh registration, identity or material snapshot.
        bool subscribed = false;
        for (int i = 0; i < 64 && !subscribed; ++i) {
            auto packet = io.receive();
            if (packet.first == 144) {
                if (packet.second != std::vector<uint8_t>({0, 1, 0, 0})) throw std::runtime_error(L("The printer refused filament updates."));
                subscribed = true;
            }
        }
        if (!subscribed) throw std::runtime_error(L("The printer did not accept filament updates."));
        publish(base + "api_register", {{"client_id", client}, {"request_id", registration_id}});
        bool registered = false, identity = false;
        for (int i = 0; i < 256; ++i) {
            auto packet = io.receive(); const auto h = packet.first; const auto& bytes = packet.second;
            if ((h >> 4) != 3) continue;
            if (bytes.size() < 2) throw std::runtime_error(L("The printer sent an invalid reply."));
            const size_t size = (size_t(bytes[0]) << 8) | bytes[1];
            if (size > bytes.size() - 2) throw std::runtime_error(L("The printer sent an invalid reply."));
            size_t offset = 2 + size;
            const unsigned qos = (h >> 1) & 3;
            if (qos == 1) {
                if (bytes.size() < offset + 2) throw std::runtime_error(L("The printer sent an invalid reply."));
                io.send(64, {bytes[offset], bytes[offset + 1]}); offset += 2;
            } else if (qos != 0) throw std::runtime_error(L("The printer sent an unsupported reply."));
            if (h & 1) continue;
            const std::string topic(bytes.begin() + 2, bytes.begin() + 2 + size);
            auto value = nlohmann::json::parse(bytes.begin() + offset, bytes.end(), nullptr, false);
            if (!value.is_object()) continue;
            if (!registered && topic == registration) {
                if (value.value("client_id", std::string()) != client || value.value("error", std::string()) != "ok")
                    throw std::runtime_error(L("The printer did not approve the connection. Close other printer apps and try again."));
                registered = true;
                publish(base + client + "/api_request", {{"id", 1}, {"method", 1001}, {"params", nlohmann::json::object()}});
            } else if (registered && topic == response && value.contains("result") && value["result"].is_object()) {
                const auto& result = value["result"];
                if (!identity && value.contains("id") && value["id"].is_number_integer() && value["id"] == 1) {
                    if (!result.contains("error_code") || !result["error_code"].is_number_integer() || result["error_code"] != 0 || result.value("sn", std::string()) != serial ||
                        (result.value("machine_model", std::string()) != "Centauri Carbon 2" &&
                         result.value("machine_model", std::string()) != "Elegoo Centauri Carbon 2" &&
                         result.value("machine_model", std::string()) != "Centauri Carbon 2 Combo"))
                        throw std::runtime_error(L("Printer identity changed or details are unavailable. Check its address."));
                    identity = true;
                    publish(base + client + "/api_request", {{"id", 2}, {"method", 2005}, {"params", nlohmann::json::object()}});
                } else if (identity && value.contains("id") && value["id"].is_number_integer() && value["id"] == 2) return {parse_elegoo_canvas(result), {}};
            }
        }
        throw std::runtime_error(L("The printer did not send a fresh filament reply."));
    } catch (const nlohmann::json::exception&) { return {{}, L("The printer sent an unsupported filament reply.")}; }
    catch (const std::exception& e) { return {{}, e.what()}; }
}
ElegooCanvasResult fetch_elegoo_canvas(const std::string& address, const std::string& serial,
                                      const std::string& secret, const std::function<bool()>& canceled);
} // namespace Slic3r
