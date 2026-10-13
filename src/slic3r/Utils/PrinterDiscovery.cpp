#include "PrinterDiscovery.hpp"
#include "Http.hpp"
#include "Bonjour.hpp"
#include "DiscoveryNetwork.hpp"
#include "SystemBonjour.hpp"
#include "libslic3r/DiscoveredPrinter.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#ifndef _WIN32
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace Slic3r {
namespace {
using boost::asio::ip::udp;
using boost::asio::ip::address_v4;

} // namespace

static bool scan_multicast_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found, bool snapmaker)
{
    // Port choices and notices follow MultiSlicer's discovery/bambu.rs. Searches
    // use each interface independently, so VPN routing cannot hide the local LAN.
    boost::asio::io_context io;
    const address_v4 group = address_v4::from_string(snapmaker ? "224.0.0.251" : "239.255.255.250");
    std::vector<address_v4> addresses;
    const auto interfaces = discovery_interfaces();
    for (const auto& interface : interfaces) addresses.emplace_back(interface.address);
    if (addresses.empty() || cancelled->load()) return false;
    std::vector<std::unique_ptr<udp::socket>> sockets;
    for (const auto& address : addresses) {
        boost::system::error_code ec;
        auto socket = std::make_unique<udp::socket>(io);
        socket->open(udp::v4(), ec);
        if (ec) continue;
        socket->bind(udp::endpoint(address, 0), ec);
        if (ec) continue;
        const auto interface = std::find_if(interfaces.begin(), interfaces.end(), [&address](const auto& item) { return item.address == address.to_uint(); });
        if (interface == interfaces.end() || !constrain_discovery_socket(*socket, interface->index)) continue;
        socket->set_option(boost::asio::ip::multicast::outbound_interface(address), ec);
        if (ec) continue;
        socket->set_option(boost::asio::ip::multicast::hops(1), ec);
        if (ec) continue;
        socket->set_option(boost::asio::ip::multicast::enable_loopback(false), ec);
        if (ec) continue;
        socket->non_blocking(true, ec);
        if (ec) continue;
        sockets.push_back(std::move(socket));
    }
    const size_t probes = sockets.size();
    for (const unsigned short port : {1990, 2021}) {
        if (snapmaker) break;
        boost::system::error_code ec;
        auto socket = std::make_unique<udp::socket>(io);
        socket->open(udp::v4(), ec);
        if (ec) continue;
        socket->set_option(udp::socket::reuse_address(true), ec);
#ifdef _WIN32
        socket->bind(udp::endpoint(address_v4::any(), port), ec);
#else
        // BSD permits a shared multicast bind alongside Bambu Studio/Orca.
        socket->bind(udp::endpoint(group, port), ec);
#endif
        if (ec) continue;
        bool joined = false;
        for (const auto& address : addresses) {
            socket->set_option(boost::asio::ip::multicast::join_group(group, address), ec);
            joined = joined || !ec;
        }
        if (!joined) continue;
        socket->non_blocking(true, ec);
        if (!ec) sockets.push_back(std::move(socket));
    }
    if (sockets.empty()) return false;
    const auto started = std::chrono::steady_clock::now();
    auto mdns_query = BonjourRequest::make_PTR("snapmaker", "tcp");
    if (snapmaker && !mdns_query) return false;
    if (mdns_query) {
        // QU: ask for a unicast response to the ephemeral socket. This avoids
        // competing with macOS's mDNSResponder on its port 5353.
        mdns_query->m_data[mdns_query->m_data.size() - 2] = static_cast<char>(0x80);
        mdns_query->m_data.back() = 0x01;
    }
    auto next_probe = started;
    std::array<char, 8192> buffer;
    while (!cancelled->load() && std::chrono::steady_clock::now() - started < std::chrono::seconds(6)) {
        if (std::chrono::steady_clock::now() >= next_probe) {
            for (size_t i = 0; i < probes; ++i) {
                if (snapmaker) {
                    boost::system::error_code ec;
                    if (!wait_for_discovery_probe(cancelled.get())) return true;
                    sockets[i]->send_to(boost::asio::buffer(mdns_query->m_data), udp::endpoint(group, 5353), 0, ec);
                    if (ec) sockets[i]->close(ec);
                    continue;
                }
                for (const unsigned short port : {1900, 1990, 2021}) {
                    const std::string query = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:" + std::to_string(port) +
                        "\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";
                    boost::system::error_code ec;
                    if (!sockets[i]->is_open()) break;
                    if (!wait_for_discovery_probe(cancelled.get())) return true;
                    if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(6)) return true;
                    sockets[i]->send_to(boost::asio::buffer(query), udp::endpoint(group, port), 0, ec);
                    if (ec) { sockets[i]->close(ec); break; }
                }
            }
            next_probe = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        for (const auto& socket : sockets) {
            for (int packet = 0; packet < 32 && !cancelled->load(); ++packet) {
                udp::endpoint sender;
                boost::system::error_code ec;
                const size_t size = socket->receive_from(boost::asio::buffer(buffer), sender, 0, ec);
                if (ec) break;
                if (snapmaker) {
                    const std::vector<unsigned char> packet(buffer.begin(), buffer.begin() + size);
                    for (auto& printer : parse_snapmaker_printer_notice(packet, sender.address().to_string())) found(std::move(printer));
                } else if (auto printer = parse_bambu_printer_notice(std::string(buffer.data(), size), sender.address().to_string()))
                    found(std::move(*printer));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    return true;
}

bool scan_bambu_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found)
{
    return scan_multicast_printers(cancelled, found, false);
}

bool scan_snapmaker_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found)
{
#ifdef __APPLE__
    return system_bonjour_lookup("snapmaker", "tcp", {"machine_type", "device_name", "sn", "link_mode"}, *cancelled, 6,
        [&found](BonjourReply&& reply) {
            const auto field = [&reply](const std::string& key) {
                const auto item = reply.txt_data.find(key);
                return item == reply.txt_data.end() ? std::string() : item->second;
            };
            DiscoveredPrinter p{"snapmaker", field("device_name"), field("machine_type"), field("sn"), reply.ip.to_string(),
                                reply.port, field("link_mode") == "lan", ""};
            if (p.name.empty()) p.name = p.model;
            if (!p.serial.empty()) found(std::move(p));
        });
#else
    return scan_multicast_printers(cancelled, found, true);
#endif
}

PrinterLinkStatus check_printer_connection(const DiscoveredPrinter& p, const std::string& code, const std::atomic<bool>* cancelled)
{
    if (cancelled && cancelled->load()) return PrinterLinkStatus::unreachable;
    // As in MultiSlicer's printer_monitor.rs, each device is checked separately.
    // Authentication proves a connection; accepting a TCP socket alone does not.
    if (p.vendor == "bambu") {
        if (code.empty()) return PrinterLinkStatus::refused;
        boost::asio::io_context io;
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
        // Bambu's LAN endpoint uses its own self-signed certificate, as in
        // MultiSlicer's printer_link.rs. This path only accepts a saved IP.
        context.set_verify_mode(boost::asio::ssl::verify_none);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, context);
        boost::asio::steady_timer timeout(io, std::chrono::seconds(4));
        boost::asio::steady_timer cancellation(io);
        PrinterLinkStatus result = PrinterLinkStatus::unreachable;
        bool finished = false;
        std::array<unsigned char, 4> answer{};
        static std::atomic<uint64_t> client_number{0};
        const std::string client = "orca-plates-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                   "-" + std::to_string(++client_number);
        auto packet = bambu_connect_packet(client, code);
        const auto finish = [&] {
            finished = true;
            boost::system::error_code ignored;
            timeout.cancel(ignored);
            cancellation.cancel(ignored);
            stream.lowest_layer().close(ignored);
        };
        timeout.async_wait([&](const boost::system::error_code& ec) { if (!ec && !finished) finish(); });
        stream.lowest_layer().async_connect({boost::asio::ip::make_address(p.address), static_cast<unsigned short>(p.port ? p.port : 8883)},
            [&](const boost::system::error_code& ec) {
                if (ec) { finish(); return; }
                stream.async_handshake(boost::asio::ssl::stream_base::client, [&](const boost::system::error_code& ec) {
                    if (ec) { finish(); return; }
                    boost::asio::async_write(stream, boost::asio::buffer(packet), [&](const boost::system::error_code& ec, size_t) {
                        if (ec) { finish(); return; }
                        boost::asio::async_read(stream, boost::asio::buffer(answer), [&](const boost::system::error_code& ec, size_t) {
                            if (!ec && answer[0] == 0x20 && answer[1] == 0x02 && (answer[2] & 0xfe) == 0)
                                result = answer[3] == 0 ? PrinterLinkStatus::connected : PrinterLinkStatus::refused;
                            finish();
                        });
                    });
                });
            });
        std::function<void()> check_cancel;
        check_cancel = [&] {
            if (!cancelled || finished) return;
            if (cancelled->load()) { finish(); return; }
            cancellation.expires_after(std::chrono::milliseconds(100));
            cancellation.async_wait([&](const boost::system::error_code& ec) { if (!ec) check_cancel(); });
        };
        check_cancel();
        io.run();
        return result;
    }
    PrinterLinkStatus result = PrinterLinkStatus::unreachable;
    const std::string host = "http://" + p.address;
    auto http = p.vendor == "flashforge" ? Http::post(host + ":8898/detail") :
                Http::get((p.vendor == "snapmaker" || p.vendor == "moonraker") ?
                           host + ":" + std::to_string(p.vendor == "snapmaker" ? 7125 : p.port ? p.port : 7125) + "/server/info" :
                           p.vendor == "creality" ? host + "/info" :
                           host + (p.port && p.port != 80 ? ":" + std::to_string(p.port) : "") + "/api/version");
    if (p.vendor == "flashforge")
        http.header("Content-Type", "application/json").set_post_body(nlohmann::json{{"serialNumber", p.serial}, {"checkCode", code}}.dump());
    else if (!code.empty())
        http.header("X-Api-Key", code);
    http.on_progress([cancelled](Http::Progress, bool& abort) { if (cancelled && cancelled->load()) abort = true; });
    http.timeout_connect(2).timeout_max(4).size_limit(65536)
        .on_complete([&](std::string body, unsigned) {
            try {
                const auto j = nlohmann::json::parse(body);
                const bool verified = p.vendor == "flashforge" ? j.value("code", -1) == 0 && j.contains("detail") && j["detail"].is_object() :
                                      p.vendor == "creality" ? j.contains("model") && j["model"].is_string() :
                                      p.vendor == "octoprint" ? j.contains("server") && j.contains("api") :
                                      j.contains("result") && j["result"].is_object() && j["result"].value("klippy_connected", false);
                if (verified) result = PrinterLinkStatus::connected;
                else if (p.vendor == "flashforge" && j.value("code", 0) != 0) result = PrinterLinkStatus::refused;
            } catch (const nlohmann::json::exception&) {}
        }).on_error([&](std::string, std::string, unsigned status) {
            if (status == 401 || status == 403) result = PrinterLinkStatus::refused;
        }).perform_sync();
    return result;
}
} // namespace Slic3r
