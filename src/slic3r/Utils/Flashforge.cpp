#include "Flashforge.hpp"
#include "DiscoveryNetwork.hpp"
#include "libslic3r/DiscoveryPolicy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <utility>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/error.hpp>
#include <boost/filesystem/operations.hpp>
#include <ctime>
#include <chrono>
#include <string>
#include "libslic3r/Config.hpp"
#include <exception>
#include <ios>
#include <thread>
#include <sstream>
#include <fstream>
#include <set>
#include <map>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>

#include <vector>
#include <utility>
#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include <boost/beast/core/detail/base64.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "Http.hpp"
#include "TCPConsole.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"

namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr unsigned short FLASHFORGE_DISCOVERY_PORT        = 48899;
constexpr unsigned short FLASHFORGE_DISCOVERY_LISTEN_PORT = 18007;

const std::array<unsigned char, 20> FLASHFORGE_DISCOVERY_MESSAGE = {
    0x77, 0x77, 0x77, 0x2e, 0x75, 0x73, 0x72, 0x22,
    0x65, 0x36, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

std::string trim_null_terminated_ascii(const char* data, size_t len)
{
    std::string out(data, data + len);
    const auto  pos = out.find('\0');
    if (pos != std::string::npos)
        out.resize(pos);
    boost::trim(out);
    return out;
}

bool parse_discovery_response(const std::vector<unsigned char>& response, const std::string& ip_address, FlashforgeDiscoveredPrinter& printer)
{
    if (response.size() < 0xC4)
        return false;

    printer.name          = trim_null_terminated_ascii(reinterpret_cast<const char*>(response.data()), 32);
    printer.serial_number = trim_null_terminated_ascii(reinterpret_cast<const char*>(response.data() + 0x92), 32);
    printer.ip_address    = ip_address;
    return !(printer.name.empty() && printer.serial_number.empty());
}


std::string safe_config_string(DynamicPrintConfig* config, const char* key)
{
    if (config == nullptr)
        return {};

    if (const auto* opt = config->option<ConfigOptionString>(key); opt != nullptr)
        return opt->value;

    return {};
}

bool try_parse_json_int(const json& value, int& out)
{
    try {
        if (value.is_number_integer() || value.is_number_unsigned()) {
            out = value.get<int>();
            return true;
        }

        if (value.is_boolean()) {
            out = value.get<bool>() ? 1 : 0;
            return true;
        }

        if (value.is_string()) {
            std::string text = value.get<std::string>();
            boost::trim(text);
            if (text.empty())
                return false;

            size_t pos = 0;
            const long parsed = std::stol(text, &pos, 10);
            if (pos == text.size()) {
                out = static_cast<int>(parsed);
                return true;
            }
        }
    } catch (...) {
    }

    return false;
}

bool validate_local_api_response(const std::string& response_body, wxString& error_msg)
{
    const auto parsed = json::parse(response_body, nullptr, false, true);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error_msg = _(L("Flashforge returned an invalid JSON response."));
        return false;
    }

    int  result_code = 0;
    bool has_code    = false;

    if (parsed.contains("code"))
        has_code = try_parse_json_int(parsed["code"], result_code);
    if (!has_code && parsed.contains("err"))
        has_code = try_parse_json_int(parsed["err"], result_code);

    if (has_code && result_code != 0) {
        std::string message;
        if (parsed.contains("message") && parsed["message"].is_string())
            message = parsed["message"].get<std::string>();
        else if (parsed.contains("msg") && parsed["msg"].is_string())
            message = parsed["msg"].get<std::string>();

        if (message.empty())
            message = "Request failed";

        error_msg = GUI::from_u8((boost::format("Flashforge local API error %1%: %2%") % result_code % message).str());
        return false;
    }

    return true;
}

std::string sanitize_flashforge_filename(const std::string& filename, const std::string& fallback_extension = {})
{
    std::string basename = fs::path(filename).filename().string();
    if (basename.empty()) {
        basename = "print";
        if (!fallback_extension.empty())
            basename += fallback_extension;
    }

    for (char& ch : basename) {
        const bool is_ascii_alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (!is_ascii_alnum && ch != '.' && ch != '_' && ch != '-') {
            ch = '_';
        }
    }

    return basename;
}

} // namespace

Flashforge::Flashforge(DynamicPrintConfig* config)
    : m_host()
    , m_serial_number()
    , m_check_code()
    , m_console_port("8899")
    , m_gcFlavor(gcfMarlinLegacy)
    , m_bufferSize(4096) // 4K buffer size
{
    m_host          = safe_config_string(config, "print_host");
    m_serial_number = safe_config_string(config, "flashforge_serial_number");
    m_check_code    = safe_config_string(config, "printhost_apikey");

    if (config != nullptr) {
        if (const auto* gcode_flavor = config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"); gcode_flavor != nullptr)
        m_gcFlavor = gcode_flavor->value;
    }
}

const char* Flashforge::get_name() const { return "Flashforge"; }

bool Flashforge::discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms,
                                 int idle_timeout_ms, int max_retries, const std::atomic<bool>* cancelled)
{
    printers.clear();
    try {
        using boost::asio::ip::udp;
        struct Probe { std::unique_ptr<udp::socket> socket; udp::endpoint destination; };
        boost::asio::io_context io;
        std::vector<Probe> probes;
        for (const auto& interface : discovery_interfaces()) {
            if (!interface.broadcast || !discovery_netmask_valid(interface.netmask)) continue;
            boost::system::error_code ec;
            auto socket = std::make_unique<udp::socket>(io);
            socket->open(udp::v4(), ec);
            if (ec) continue;
            socket->set_option(boost::asio::socket_base::broadcast(true), ec);
            if (ec) continue;
            socket->set_option(boost::asio::socket_base::reuse_address(true), ec);
            if (ec) continue;
            socket->bind(udp::endpoint(boost::asio::ip::address_v4(interface.address), FLASHFORGE_DISCOVERY_LISTEN_PORT), ec);
            if (ec || !constrain_discovery_socket(*socket, interface.index)) continue;
            socket->non_blocking(true, ec);
            if (ec) continue;
            probes.push_back({std::move(socket), udp::endpoint(boost::asio::ip::address_v4(interface.address | ~interface.netmask), FLASHFORGE_DISCOVERY_PORT)});
        }
        if (probes.empty()) {
            msg = _L("Could not search for Flashforge printers. Check your network connection.");
            return false;
        }
        std::map<std::string, FlashforgeDiscoveredPrinter> by_ip;
        for (int attempt = 0; attempt < std::max(1, max_retries); ++attempt) {
            for (auto& probe : probes) {
                if (!probe.socket->is_open()) continue;
                if (!wait_for_discovery_probe(cancelled)) return false;
                boost::system::error_code ec;
                probe.socket->send_to(boost::asio::buffer(FLASHFORGE_DISCOVERY_MESSAGE), probe.destination, 0, ec);
                if (ec) probe.socket->close(ec);
            }
            const auto start = std::chrono::steady_clock::now();
            auto last_reply = start;
            while ((!cancelled || !cancelled->load()) && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
                if (!by_ip.empty() && std::chrono::steady_clock::now() - last_reply >= std::chrono::milliseconds(idle_timeout_ms)) break;
                bool open = false;
                for (auto& probe : probes) {
                    if (!probe.socket->is_open()) continue;
                    open = true;
                    for (int packet = 0; packet < 32; ++packet) {
                        std::vector<unsigned char> buffer(512);
                        udp::endpoint remote;
                        boost::system::error_code ec;
                        const auto size = probe.socket->receive_from(boost::asio::buffer(buffer), remote, 0, ec);
                        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) break;
                        if (ec) { probe.socket->close(ec); break; }
                        buffer.resize(size);
                        FlashforgeDiscoveredPrinter printer;
                        if (parse_discovery_response(buffer, remote.address().to_string(), printer)) {
                            by_ip[printer.ip_address] = std::move(printer);
                            last_reply = std::chrono::steady_clock::now();
                        }
                    }
                }
                if (!open) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!by_ip.empty() || (cancelled && cancelled->load())) break;
        }
        for (auto& entry : by_ip) printers.push_back(std::move(entry.second));
        std::sort(printers.begin(), printers.end(), [](const auto& a, const auto& b) { return a.name < b.name || (a.name == b.name && a.ip_address < b.ip_address); });
        if (printers.empty()) msg = _L("No Flashforge printers were discovered on the local network.");
        return !printers.empty();
    } catch (const std::exception& error) {
        msg = wxString::FromUTF8(error.what());
        return false;
    }
}

bool Flashforge::test(wxString& msg) const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return test_local_api(msg);

    BOOST_LOG_TRIVIAL(debug) << boost::format("[Flashforge Serial] testing connection");
    // Utils::TCPConsole console(m_host, m_console_port);
    Utils::TCPConsole client(m_host, m_console_port);
    client.enqueue_cmd(controlCommand);
    bool res = client.run_queue();
    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection failed");
    } else {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection success");
    }
    return res;
}

wxString Flashforge::get_test_ok_msg() const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return _(L("Connected to Flashforge local API successfully."));
    return _(L("Serial connection to Flashforge is working correctly."));
}

wxString Flashforge::get_test_failed_msg(wxString& msg) const
{
    const std::string prefix = (!m_serial_number.empty() && !m_check_code.empty()) ?
        _utf8(L("Could not connect to Flashforge local API")) :
        _utf8(L("Could not connect to Flashforge via serial"));
    return GUI::from_u8((boost::format("%s: %s") % prefix % std::string(msg.ToUTF8())).str());
}


bool Flashforge::connect(wxString& msg) const
{
    
    Utils::TCPConsole client(m_host, m_console_port);

    client.enqueue_cmd(controlCommand);
    client.enqueue_cmd(deviceInfoCommand);

    if (m_gcFlavor == gcfKlipper)
        client.enqueue_cmd(connectKlipperCommand);
    else {
        client.enqueue_cmd(connectLegacyCommand);

    }

    client.enqueue_cmd(statusCommand);
    

    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to initiate connection");
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Successfully initiated Connection");

    return res;
}

bool Flashforge::start_print(wxString& msg, const std::string& filename) const
{
    Utils::TCPConsole            client(m_host, m_console_port);
    const std::string            safe_filename = sanitize_flashforge_filename(filename);
    Slic3r::Utils::SerialMessage startPrintCommand = {(boost::format("~M23 0:/user/%1%") % safe_filename).str(), Slic3r::Utils::Command};
    client.enqueue_cmd(startPrintCommand);
    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to start print %1%") % safe_filename;
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Started print %1%") % safe_filename;

    return res;
}

bool Flashforge::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return upload_local_api(std::move(upload_data), std::move(progress_fn), std::move(error_fn));

    bool res = true;
    wxString errormsg;

    Utils::TCPConsole client(m_host, m_console_port);

    try {

        res = connect(errormsg);
        const std::string fallback_extension = upload_data.source_path.extension().string().empty() ? ".gcode" : upload_data.source_path.extension().string();
        const std::string upload_filename = sanitize_flashforge_filename(upload_data.upload_path.string(), fallback_extension);

        std::ifstream newfile;
        newfile.open(upload_data.source_path.c_str(), std::ios::binary); // open a file to perform read operation using file object
        std::string gcodeFile;
        if (newfile.is_open()) {                                         // checking whether the file is open
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...");
            newfile.seekg(0, std::ios::end);
            std::ifstream::pos_type pos = newfile.tellg();

            std::vector<char> result(pos);

            newfile.seekg(0, std::ios::beg);
            newfile.read(&result[0], pos);

            gcodeFile = std::string(result.begin(), result.end()); // TODO: Find more efficient way of breaking ifstream

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...done size is %1%") % gcodeFile.size();

            newfile.close(); // close the file object.
        }
        Slic3r::Utils::SerialMessage fileuploadCommand =
            {(boost::format("~M28 %1% 0:/user/%2%") % gcodeFile.size() % upload_filename).str(),
             Slic3r::Utils::Command};
        client.enqueue_cmd(fileuploadCommand);

        //client.set_tcp_queue_delay(std::chrono::nanoseconds(10000));

        for (int bytePos = 0; bytePos < gcodeFile.size(); bytePos += m_bufferSize) { // TODO: Find more efficient way of breaking ifstream

            int bytePosEnd  = (gcodeFile.size() - bytePos > m_bufferSize - 1) ? m_bufferSize : gcodeFile.size();
            Slic3r::Utils::SerialMessage dataCommand = {gcodeFile.substr(bytePos, bytePosEnd), Slic3r::Utils::Data}; // Break into smaller byte chunks

            client.enqueue_cmd(dataCommand);

        }

        res = client.run_queue();

        if (res)
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sent %1% ") % gcodeFile.size();


        if (!res) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % client.error_message().c_str();
            errormsg = wxString::FromUTF8(client.error_message().c_str());
            error_fn(std::move(errormsg));
        } else {

            client.set_tcp_queue_delay(std::chrono::milliseconds(3000));

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sending file save command ");
            
            client.enqueue_cmd(saveFileCommand);

            res = client.run_queue();

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint)
                res = start_print(errormsg, upload_filename);
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % e.what();
        errormsg = wxString::FromUTF8(e.what());
        error_fn(std::move(errormsg));
    }

    return res;
}

bool Flashforge::test_local_api(wxString& msg) const
{
    std::string body;
    return request_local_api_json("detail", json{{"serialNumber", m_serial_number}, {"checkCode", m_check_code}}.dump(), body, msg);
}

bool Flashforge::fetch_material_slots(std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station, wxString& msg) const
{
    slots.clear();

    if (m_serial_number.empty() || m_check_code.empty()) {
        msg = _(L("Flashforge local API requires both serial number and access code."));
        return false;
    }

    std::string body;
    if (!request_local_api_json("detail", json{{"serialNumber", m_serial_number}, {"checkCode", m_check_code}}.dump(), body, msg))
        return false;

    if (!parse_material_slots(body, slots, supports_material_station)) {
        msg = _(L("Flashforge returned an invalid JSON response."));
        return false;
    }

    return true;
}

bool Flashforge::parse_material_slots(const std::string& body, std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station)
{
    slots.clear();

    const auto parsed = json::parse(body, nullptr, false, true);
    if (parsed.is_discarded())
        return false;

    const auto& detail = parsed.contains("detail") ? parsed["detail"] : parsed;
    const auto& station = detail.contains("matlStationInfo") ? detail["matlStationInfo"] :
                          detail.contains("MatlStationInfo") ? detail["MatlStationInfo"] : json();
    const auto& slot_infos = station.contains("slotInfos") ? station["slotInfos"] :
                             station.contains("SlotInfos") ? station["SlotInfos"] : json::array();

    bool reports_material_station = false;

    int has_material_station_flag = 0;
    if (detail.contains("hasMatlStation") && try_parse_json_int(detail["hasMatlStation"], has_material_station_flag))
        reports_material_station = has_material_station_flag != 0;
    else if (detail.contains("HasMatlStation") && try_parse_json_int(detail["HasMatlStation"], has_material_station_flag))
        reports_material_station = has_material_station_flag != 0;

    int slot_count = 0;
    if (station.contains("slotCnt") && try_parse_json_int(station["slotCnt"], slot_count))
        reports_material_station = reports_material_station || slot_count > 0;
    else if (station.contains("SlotCnt") && try_parse_json_int(station["SlotCnt"], slot_count))
        reports_material_station = reports_material_station || slot_count > 0;

    if (slot_infos.is_array() && !slot_infos.empty())
        reports_material_station = true;

    if (supports_material_station != nullptr)
        *supports_material_station = reports_material_station;

    // Fields are read leniently: firmware may send numbers as strings or flags as numbers.
    for (const auto& slot : slot_infos) {
        if (!slot.is_object())
            continue;
        FlashforgeMaterialSlot info;
        info.slot_id = static_cast<int>(slots.size()) + 1;
        if (const auto it = slot.find("slotId"); it != slot.end())
            try_parse_json_int(*it, info.slot_id);
        int has_filament = 0;
        if (const auto it = slot.find("hasFilament"); it != slot.end() && try_parse_json_int(*it, has_filament))
            info.has_filament = has_filament != 0;
        if (const auto it = slot.find("materialName"); it != slot.end() && it->is_string())
            info.material_name = it->get<std::string>();
        if (const auto it = slot.find("materialColor"); it != slot.end() && it->is_string())
            info.material_color = it->get<std::string>();
        slots.emplace_back(std::move(info));
    }

    return true;
}

bool Flashforge::upload_local_api(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn) const
{
    bool        res            = true;
    std::string material_map_b64;
    std::string material_map_json = "[]";
    auto        leveling_before_print = upload_data.extended_info["levelingBeforePrint"] == "1";
    auto        time_lapse_video      = upload_data.extended_info["timeLapseVideo"] == "1";
    auto        use_material_station  = upload_data.extended_info["useMatlStation"] == "1";

    if (auto it = upload_data.extended_info.find("materialMappings"); it != upload_data.extended_info.end())
        material_map_json = it->second;

    material_map_b64.resize(boost::beast::detail::base64::encoded_size(material_map_json.size()));
    material_map_b64.resize(boost::beast::detail::base64::encode(material_map_b64.data(), material_map_json.data(), material_map_json.size()));

    auto        url      = make_http_url("uploadGcode");
    const std::string fallback_extension = upload_data.source_path.extension().string().empty() ? (upload_data.use_3mf ? ".3mf" : ".gcode") : upload_data.source_path.extension().string();
    auto        filename = sanitize_flashforge_filename(upload_data.upload_path.string(), fallback_extension);
    std::string file_size;
    try {
        file_size = std::to_string(fs::file_size(upload_data.source_path));
    } catch (...) {
        file_size = "0";
    }

    auto http = Http::post(url);
    http.header("serialNumber", m_serial_number)
        .header("checkCode", m_check_code)
        .header("fileSize", file_size)
        .header("printNow", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .header("levelingBeforePrint", leveling_before_print ? "true" : "false")
        .header("flowCalibration", "false")
        .header("firstLayerInspection", "false")
        .header("timeLapseVideo", time_lapse_video ? "true" : "false")
        .header("useMatlStation", use_material_station ? "true" : "false")
        .header("gcodeToolCnt", upload_data.extended_info["gcodeToolCnt"])
        .header("materialMappings", material_map_b64)
        .form_add_file("gcodeFile", upload_data.source_path.string(), filename)
        .on_complete([&](std::string body, unsigned status) {
            wxString msg;
            if (!validate_local_api_response(body, msg)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("[Flashforge HTTP] upload rejected by printer: HTTP %1% body: `%2%`") % status % body;
                error_fn(msg);
                res = false;
            } else {
                BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge HTTP] upload complete: HTTP %1% body: %2%") % status % body;
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("[Flashforge HTTP] upload failed: %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel)
                res = false;
        })
        .perform_sync();

    return res;
}

bool Flashforge::request_local_api_json(const std::string& path, const std::string& body, std::string& response_body, wxString& error_msg) const
{
    bool ok = true;
    auto http = Http::post(make_http_url(path));
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string body_text, unsigned) {
            response_body = std::move(body_text);
            if (!validate_local_api_response(response_body, error_msg))
                ok = false;
        })
        .on_error([&](std::string body_text, std::string error, unsigned status) {
            response_body = std::move(body_text);
            error_msg     = format_error(response_body, error, status);
            ok            = false;
        })
        .perform_sync();
    return ok;
}

std::string Flashforge::make_http_url(const std::string& path) const
{
    return (boost::format("http://%1%:8898/%2%") % extract_host_name() % path).str();
}

std::string Flashforge::extract_host_name() const
{
    std::string host = m_host;
    if (host.find("://") == std::string::npos) {
        const auto slash_pos = host.find('/');
        if (slash_pos != std::string::npos)
            host = host.substr(0, slash_pos);
        return host;
    }

    std::string out = host;
    CURLU*       hurl = curl_url();
    if (!hurl)
        return host;

    const auto rc = curl_url_set(hurl, CURLUPART_URL, host.c_str(), 0);
    if (rc == CURLUE_OK) {
        char* raw_host = nullptr;
        if (curl_url_get(hurl, CURLUPART_HOST, &raw_host, 0) == CURLUE_OK && raw_host != nullptr) {
            out = raw_host;
            curl_free(raw_host);
        }
    }

    curl_url_cleanup(hurl);
    return out;
}

} // namespace Slic3r
