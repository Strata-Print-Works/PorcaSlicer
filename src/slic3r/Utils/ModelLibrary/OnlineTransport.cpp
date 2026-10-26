#include "Online.hpp"
#include "slic3r/Utils/Http.hpp"
#include <stdexcept>

namespace Slic3r::ModelLibrary {
WebReply request_online(const std::string& url, const Json& body,
                        const std::map<std::string, std::string>& headers, const Cancel& cancel)
{
    if (cancel && cancel()) throw std::runtime_error("Cancelled");
    auto request = body.is_null() ? Http::get(url) : Http::post(url);
    request.headers_reset().tls_verify(true).timeout_connect(10).timeout_max(45).size_limit(16 * 1024 * 1024);
    request.header("User-Agent", "Porca Model Library");
    if (!body.is_null()) request.header("Content-Type", "application/json").set_post_body(body.dump());
    for (const auto& [name, value] : headers) request.header(name, value);
    WebReply reply;
    request.on_progress([cancel](Http::Progress, bool& stop) { stop = cancel && cancel(); });
    request.on_complete([&reply](std::string bytes, unsigned status) { reply = {status,std::move(bytes)}; });
    request.on_error([&reply](std::string, std::string, unsigned status) { reply.status = status; });
    request.perform_sync();
    if (cancel && cancel()) throw std::runtime_error("Cancelled");
    if (!reply.status) throw std::runtime_error("Could not reach the service. The library has been kept.");
    return reply;
}
}
