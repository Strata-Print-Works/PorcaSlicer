#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r {
// `not_ready`: the printer's service answered, but the printer cannot take
// control yet, such as while its firmware host is starting or disconnected.
enum class PrinterLinkStatus { connected, unreachable, refused, not_ready };
enum class PrinterLinkFailure {
    none, cancelled, missing_credentials, invalid_address, identity_mismatch, timeout, connection_refused,
    unreachable, handshake, send, receive, invalid_response, credentials_rejected,
    service_rejected, printer_not_ready, http_error, request_failed, certificate_changed
};
struct PrinterLinkResult {
    PrinterLinkStatus status = PrinterLinkStatus::unreachable;
    PrinterLinkFailure failure = PrinterLinkFailure::request_failed;
    unsigned http_status = 0;
};

// Decode bounded responses without retaining server text, URLs or credentials.
PrinterLinkResult printer_mqtt_response(const std::array<unsigned char, 4>& response);
PrinterLinkResult printer_http_response(const std::string& vendor, const std::string& body, unsigned status);

// Independent jobs and an inspector own their settings; a retained connection
// must never coerce an imported project's model, nozzle or filament setup.
inline bool may_sync_connected_printer_after_project_load(bool load_config, bool independent_setups, bool inspecting)
{
    return load_config && !independent_setups && !inspecting;
}

// Startup restoration runs once, after dependencies are ready. An explicit
// selection or shutdown cancels it, so delayed startup never takes over the UI.
class PrinterConnectionRestore {
    std::string m_target;
public:
    explicit PrinterConnectionRestore(std::string target) : m_target(std::move(target)) {}
    std::optional<std::string> take_target(bool ready, bool superseded, bool closing)
    {
        if (superseded || closing) m_target.clear();
        if (!ready || m_target.empty()) return std::nullopt;
        return std::exchange(m_target, {});
    }
};
} // namespace Slic3r
