#pragma once

#include "PrinterConnectionStatus.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>

namespace Slic3r {
enum class ElegooPrinterType { Other, CC, CC2 };
inline ElegooPrinterType elegoo_printer_type(const std::string& model)
{
    if (model == "Elegoo Centauri Carbon 2") return ElegooPrinterType::CC2;
    if (model == "Elegoo Centauri Carbon") return ElegooPrinterType::CC;
    return ElegooPrinterType::Other;
}
inline std::string elegoo_cc2_token(const std::string& saved)
{
    // Vendor default for machines on which an access code has not been enabled.
    return saved.empty() ? "123456" : saved;
}
struct ElegooDeviceInfo {
    PrinterLinkResult connection;
    std::string serial;
};
inline ElegooDeviceInfo parse_elegoo_cc2_info(const std::string& body, unsigned status, const std::string& expected_serial = {})
{
    if (status == 401 || status == 403)
        return {{PrinterLinkStatus::refused, PrinterLinkFailure::credentials_rejected, status}, {}};
    if (status < 200 || status >= 300)
        return {{PrinterLinkStatus::unreachable, status ? PrinterLinkFailure::http_error : PrinterLinkFailure::request_failed, status}, {}};
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_object() && json.contains("error_code") && json["error_code"].is_number_integer()) {
        if (json["error_code"] != 0)
            return {{PrinterLinkStatus::refused, PrinterLinkFailure::service_rejected, status}, {}};
        if (json.contains("system_info") && json["system_info"].is_object()) {
            const auto& system = json["system_info"];
            if (system.contains("sn") && system["sn"].is_string()) {
                const auto serial = system["sn"].get<std::string>();
                if (!serial.empty() && serial.size() <= 256 &&
                    std::none_of(serial.begin(), serial.end(), [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '+' || c == '#'; }))
                {
                    if (!expected_serial.empty() && serial != expected_serial)
                        return {{PrinterLinkStatus::unreachable, PrinterLinkFailure::identity_mismatch, status}, {}};
                    return {{PrinterLinkStatus::connected, PrinterLinkFailure::none}, serial};
                }
            }
        }
    }
    return {{PrinterLinkStatus::unreachable, PrinterLinkFailure::invalid_response, status}, {}};
}
} // namespace Slic3r
