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
// One-shot session for a read-only request or the explicit print start. The
// transport owns deadlines and cancellation; protocol decisions below are also
// exercised with socket-free packet fixtures.
class ElegooCanvasTransport {
public:
    virtual ~ElegooCanvasTransport() = default;
    virtual void send(uint8_t header, const std::vector<uint8_t>& body) = 0;
    virtual std::pair<uint8_t, std::vector<uint8_t>> receive() = 0;
    // Called once, just before a printer command is sent. From then on only the
    // deadline ends the exchange, so a late cancel cannot hide its outcome.
    virtual void commit() {}
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
            // Empty trays are not unknown spools and need no user decision.
            if (slot.contains("status") && slot["status"].is_number_integer() && slot["status"] == 0) continue;
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
// A registered request sequence for one fresh client, shared by the CANVAS read
// and the print start. The printer's identity is verified before any other
// request; only non-retained replies on this client's response topic with the
// request's id count.
class ElegooMqttSession {
public:
    ElegooMqttSession(ElegooCanvasTransport& io, const std::string& serial, const std::string& secret, const std::string& client)
        : m_io(io), m_base("elegoo/" + serial + "/"), m_request(m_base + client + "/api_request"), m_response(m_base + client + "/api_response")
    {
        if (!elegoo_canvas_serial_valid(serial) ||
            client.empty() || client.size() > 128 || secret.size() > 256)
            throw std::runtime_error(L("Check this printer's serial number and LAN access code."));
        const auto registration_id = client + "_req";
        const auto registration = m_base + registration_id + "/register_response";
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
        for (const auto& topic : {m_response, registration}) { elegoo_mqtt_string(subscribe, topic); subscribe.push_back(0); }
        io.send(130, subscribe);
        // SUBACK may be interleaved with notifications. No retained messages are
        // accepted as a fresh registration, identity, status or material snapshot.
        bool subscribed = false;
        for (int i = 0; i < 64 && !subscribed; ++i) {
            auto packet = io.receive();
            if (packet.first == 144) {
                if (packet.second != std::vector<uint8_t>({0, 1, 0, 0})) throw std::runtime_error(L("The printer refused the connection."));
                subscribed = true;
            }
        }
        if (!subscribed) throw std::runtime_error(L("The printer did not respond."));
        publish(m_base + "api_register", {{"client_id", client}, {"request_id", registration_id}});
        for (;;) {
            const auto message = next();
            if (message.first != registration) continue;
            if (message.second.value("client_id", std::string()) != client || message.second.value("error", std::string()) != "ok")
                throw std::runtime_error(L("The printer did not approve the connection. Close other printer apps and try again."));
            break;
        }
        const auto result = request(1, 1001)["result"];
        if (!result.contains("error_code") || !result["error_code"].is_number_integer() || result["error_code"] != 0 || result.value("sn", std::string()) != serial ||
            (result.value("machine_model", std::string()) != "Centauri Carbon 2" &&
             result.value("machine_model", std::string()) != "Elegoo Centauri Carbon 2" &&
             result.value("machine_model", std::string()) != "Centauri Carbon 2 Combo"))
            throw std::runtime_error(L("Printer identity changed or details are unavailable. Check its address."));
    }
    // Publishes one request and returns its reply, whose "result" is an object.
    nlohmann::json request(int id, int method, const nlohmann::json& params = nlohmann::json::object())
    {
        publish(m_request, {{"id", id}, {"method", method}, {"params", params}});
        for (;;) {
            auto message = next();
            const auto& value = message.second;
            if (message.first == m_response && value.contains("result") && value["result"].is_object() &&
                value.contains("id") && value["id"].is_number_integer() && value["id"] == id)
                return std::move(message.second);
        }
    }

private:
    void publish(const std::string& topic, const nlohmann::json& value)
    {
        std::vector<uint8_t> bytes; elegoo_mqtt_string(bytes, topic);
        const auto text = value.dump(); bytes.insert(bytes.end(), text.begin(), text.end()); m_io.send(48, bytes);
    }
    // The next fresh JSON message within the session's packet budget. QoS 1
    // deliveries are acknowledged; retained messages are never returned.
    std::pair<std::string, nlohmann::json> next()
    {
        while (m_packets < 256) {
            ++m_packets;
            auto packet = m_io.receive(); const auto h = packet.first; const auto& bytes = packet.second;
            if ((h >> 4) != 3) continue;
            if (bytes.size() < 2) throw std::runtime_error(L("The printer sent an invalid reply."));
            const size_t size = (size_t(bytes[0]) << 8) | bytes[1];
            if (size > bytes.size() - 2) throw std::runtime_error(L("The printer sent an invalid reply."));
            size_t offset = 2 + size;
            const unsigned qos = (h >> 1) & 3;
            if (qos == 1) {
                if (bytes.size() < offset + 2) throw std::runtime_error(L("The printer sent an invalid reply."));
                m_io.send(64, {bytes[offset], bytes[offset + 1]}); offset += 2;
            } else if (qos != 0) throw std::runtime_error(L("The printer sent an unsupported reply."));
            if (h & 1) continue;
            auto value = nlohmann::json::parse(bytes.begin() + offset, bytes.end(), nullptr, false);
            if (value.is_object()) return {std::string(bytes.begin() + 2, bytes.begin() + 2 + size), std::move(value)};
        }
        throw std::runtime_error(L("The printer did not respond."));
    }
    ElegooCanvasTransport& m_io;
    const std::string m_base, m_request, m_response;
    int m_packets = 0;
};
inline ElegooCanvasResult read_elegoo_canvas(ElegooCanvasTransport& io, const std::string& serial,
                                             const std::string& secret, const std::string& client)
{
    try {
        ElegooMqttSession session(io, serial, secret, client);
        return {parse_elegoo_canvas(session.request(2, 2005)["result"]), {}};
    } catch (const nlohmann::json::exception&) { return {{}, L("The printer sent an unsupported filament reply.")}; }
    catch (const std::exception& e) { return {{}, e.what()}; }
}
ElegooCanvasResult fetch_elegoo_canvas(const std::string& address, const std::string& serial,
                                      const std::string& secret, const std::function<bool()>& canceled);

// A print the user asked to start right after its upload.
struct ElegooPrintJob {
    std::string filename;     // the uploaded file's name, without a folder
    bool bed_leveling{false};
    bool smooth_plate{false}; // Side B; otherwise the textured Side A
    int filaments{0};         // used by the sliced plate; 0 when unknown
};
struct ElegooPrintStartResult {
    bool started{false};
    std::string error;    // catalog text, translated where it is shown
    bool sent{false};     // the start command reached the transport
    int printer_code{0};  // the printer's refusal code, when it gave one
};
// Why Porca must not start this job, or nullptr. Without a confirmed CANVAS slot
// mapping only a single-filament print can be started remotely.
inline const char* elegoo_print_job_problem(const ElegooPrintJob& job)
{
    if (job.filaments > 1) return L("Start prints that use more than one filament from the printer's screen.");
    if (job.filaments < 1) return L("Couldn't confirm which filaments this print uses. Start it from the printer's screen.");
    if (job.filename.empty() || job.filename.size() > 255 || job.filename.find_first_of("/\\") != std::string::npos ||
        std::any_of(job.filename.begin(), job.filename.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return L("Start this file from the printer's screen.");
    return nullptr;
}
// The vendor's start request for a print without CANVAS: with no slot map the
// printer uses the filament at its print head. Time-lapse needs a camera this
// exchange does not confirm, so it stays off.
inline nlohmann::json elegoo_print_start_params(const ElegooPrintJob& job)
{
    return {{"storage_media", "local"}, {"filename", job.filename},
            {"config", {{"delay_video", false}, {"printer_check", job.bed_leveling}, {"print_layout", job.smooth_plate ? "B" : "A"}}}};
}
// Only the printer's explicit report that no CANVAS is attached counts; a missing
// or unfamiliar value is not proof that the print head feeds by itself.
inline bool elegoo_status_without_canvas(const nlohmann::json& status)
{
    if (!status.contains("external_device") || !status["external_device"].is_object() || !status["external_device"].contains("type"))
        return false;
    const auto& type = status["external_device"]["type"];
    return type.is_number_integer() ? type == 0 : type.is_string() && (type == "" || type == "0");
}
inline const char* elegoo_print_refusal(int code)
{
    switch (code) {
    case 109: return L("Load filament, then start the print from the printer's screen.");
    case 1009: return L("The printer is busy.");
    case 1021: return L("The printer couldn't find the uploaded file.");
    case 1026: return L("The printer has no bed leveling data. Turn on bed leveling and send the print again.");
    default: return L("The printer refused to start the print.");
    }
}
// Sends the start command at most once, after identity verification and only
// when the printer reports no CANVAS. A refused or unconfirmed start is
// reported, never retried.
inline ElegooPrintStartResult start_elegoo_print(ElegooCanvasTransport& io, const std::string& serial, const std::string& secret,
                                                 const std::string& client, const ElegooPrintJob& job)
{
    if (const char* problem = elegoo_print_job_problem(job)) return {false, problem};
    bool sent = false;
    try {
        ElegooMqttSession session(io, serial, secret, client);
        const auto status = session.request(2, 1002)["result"];
        if (!status.contains("error_code") || !status["error_code"].is_number_integer() || status["error_code"] != 0)
            return {false, L("The printer did not report its status.")};
        if (!elegoo_status_without_canvas(status))
            return {false, L("With CANVAS attached, start the print from the printer's screen.")};
        io.commit();
        sent = true;
        const auto reply = session.request(3, 1020, elegoo_print_start_params(job));
        const auto& result = reply["result"];
        if ((!reply.contains("method") || reply["method"] == 1020) && result.contains("error_code") && result["error_code"].is_number_integer()) {
            const int code = result["error_code"];
            if (code == 0) return {true, {}, true};
            return {false, elegoo_print_refusal(code), true, code};
        }
    } catch (const nlohmann::json::exception&) {
        if (!sent) return {false, L("The printer sent an unsupported reply.")};
    } catch (const std::exception& e) {
        if (!sent) return {false, e.what()};
    }
    return {false, L("The printer didn't confirm that the print started. Check the printer before sending it again."), true};
}
// The only route to the start command: the user's Upload and print, after the
// whole upload succeeded. Upload-only and failed uploads never reach it.
template<class Upload, class Start> bool elegoo_upload_then_start(bool print_requested, Upload&& upload, Start&& start)
{
    return upload() && (!print_requested || start());
}
ElegooPrintStartResult send_elegoo_print_start(const std::string& address, const std::string& serial, const std::string& secret,
                                               const ElegooPrintJob& job, const std::function<bool()>& canceled);
} // namespace Slic3r
