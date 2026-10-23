#include "PrinterConnectionStatus.hpp"
#include <array>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {
PrinterLinkResult printer_mqtt_response(const std::array<unsigned char, 4>& response)
{
    if (response[0] != 0x20 || response[1] != 0x02 || (response[2] & 0xfe) != 0 || response[3] > 5)
        return {PrinterLinkStatus::unreachable, PrinterLinkFailure::invalid_response};
    if (response[3] == 0) return {PrinterLinkStatus::connected, PrinterLinkFailure::none};
    return {PrinterLinkStatus::refused, response[3] >= 4 ? PrinterLinkFailure::credentials_rejected : PrinterLinkFailure::service_rejected};
}

PrinterLinkResult printer_http_response(const std::string& vendor, const std::string& body, unsigned status)
{
    if (status == 401 || status == 403)
        return {PrinterLinkStatus::refused, PrinterLinkFailure::credentials_rejected, status};
    if (status < 200 || status >= 300)
        return {PrinterLinkStatus::unreachable, status == 0 ? PrinterLinkFailure::request_failed : PrinterLinkFailure::http_error, status};
    try {
        const auto j = nlohmann::json::parse(body);
        if (vendor == "flashforge") {
            if (j.contains("code") && j.at("code").is_number_integer() && j.at("code").get<int>() != 0)
                return {PrinterLinkStatus::refused, PrinterLinkFailure::service_rejected};
            if (j.value("code", -1) == 0 && j.contains("detail") && j.at("detail").is_object())
                return {PrinterLinkStatus::connected, PrinterLinkFailure::none};
        } else if (vendor == "creality") {
            if (j.contains("model") && j.at("model").is_string())
                return {PrinterLinkStatus::connected, PrinterLinkFailure::none};
        } else if (vendor == "octoprint") {
            if (j.contains("server") && j.contains("api") && j.at("server").is_string() && j.at("api").is_string())
                return {PrinterLinkStatus::connected, PrinterLinkFailure::none};
        } else if (vendor == "snapmaker" || vendor == "moonraker") {
            if (j.contains("result") && j.at("result").is_object() && j.at("result").contains("klippy_connected") &&
                j.at("result").at("klippy_connected").is_boolean())
                return j.at("result").at("klippy_connected").get<bool>()
                    ? PrinterLinkResult{PrinterLinkStatus::connected, PrinterLinkFailure::none}
                    : PrinterLinkResult{PrinterLinkStatus::not_ready, PrinterLinkFailure::printer_not_ready};
        }
    } catch (const nlohmann::json::exception&) {}
    return {PrinterLinkStatus::unreachable, PrinterLinkFailure::invalid_response};
}
} // namespace Slic3r
