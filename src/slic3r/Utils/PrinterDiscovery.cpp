#include "ElegooLinkProtocol.hpp"
#include "SavedPrinterConnection.hpp"
#include "PrinterDiscovery.hpp"
#include "libslic3r/ElegooDiscovery.hpp"
#include "PrinterConnectionStatus.hpp"
#include <boost/asio/error.hpp>
#include "Http.hpp"
#include "Bonjour.hpp"
#include "DiscoveryNetwork.hpp"
#include "SystemBonjour.hpp"
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/DiscoveryPolicy.hpp"
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
#include <boost/asio/ssl/verify_context.hpp>
#include <boost/asio/ssl/verify_mode.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
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

std::string certificate_sha256(X509* certificate)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (!certificate || X509_digest(certificate, EVP_sha256(), digest, &size) != 1) return {};
    return certificate_fingerprint_hex(digest, size);
}
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

bool scan_elegoo_printers(const std::shared_ptr<std::atomic<bool>>& cancelled, const PrinterFoundFn& found)
{
    if (cancelled->load()) return false; // no interface enumeration or socket when cancelled
    class Sockets final : public ElegooDiscoveryTransport {
        boost::asio::io_context io;
        struct InterfaceSocket { DiscoveryInterface interface; std::unique_ptr<udp::socket> socket; };
        std::vector<InterfaceSocket> sockets;
        const std::atomic<bool>& stop;
    public:
        explicit Sockets(const std::atomic<bool>& stop) : stop(stop)
        {
            for (const auto& interface : discovery_interfaces()) {
                if (stop.load()) break;
                if (!interface.broadcast || !discovery_netmask_valid(interface.netmask)) continue;
                boost::system::error_code error;
                auto socket = std::make_unique<udp::socket>(io);
                socket->open(udp::v4(), error);
                if (error) continue;
                socket->bind(udp::endpoint(address_v4(interface.address), 0), error);
                if (error || !constrain_discovery_socket(*socket, interface.index)) continue;
                socket->set_option(udp::socket::broadcast(true), error);
                if (error) continue;
                socket->non_blocking(true, error);
                if (!error) sockets.push_back({interface, std::move(socket)});
            }
        }
        Clock::time_point now() const override { return Clock::now(); }
        bool cancelled() const override { return stop.load(); }
        size_t interface_count() const override { return sockets.size(); }
        void send_probe(size_t index, Clock::time_point deadline) override
        {
            auto& item = sockets[index];
            if (!item.socket->is_open() || !wait_for_discovery_probe(&stop, deadline) || cancelled() || now() >= deadline) return;
            boost::system::error_code error;
            const auto broadcast = address_v4(item.interface.address | ~item.interface.netmask);
            item.socket->send_to(boost::asio::buffer(elegoo_discovery_probe, sizeof(elegoo_discovery_probe) - 1),
                                 udp::endpoint(broadcast, elegoo_discovery_port), 0, error);
            if (error) item.socket->close(error);
        }
        std::optional<ElegooDiscoveryDatagram> receive(size_t index) override
        {
            std::array<char, 8193> bytes; // one extra byte detects an oversized datagram
            udp::endpoint sender;
            boost::system::error_code error;
            auto& item = sockets[index];
            const auto size = item.socket->receive_from(boost::asio::buffer(bytes), sender, 0, error);
            if (error == boost::asio::error::message_size)
                return ElegooDiscoveryDatagram{std::string(8193, ' '), {}, 0, false}; // count truncated packets in the receive budget
            if (error || !sender.address().is_v4()) return std::nullopt;
            return ElegooDiscoveryDatagram{std::string(bytes.data(), size), sender.address().to_string(), sender.port(),
                elegoo_reply_on_interface(sender.address().to_v4().to_uint(), item.interface.address, item.interface.netmask)};
        }
        void idle() override { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    } transport(*cancelled);
    return run_elegoo_discovery(transport, found);
}

PrinterLinkResult check_printer_connection(const DiscoveredPrinter& p, const std::string& code, const std::atomic<bool>* cancelled,
                                           std::string* serial, const std::string& certificate_pin, std::string* certificate)
{
    if (cancelled && cancelled->load()) return {PrinterLinkStatus::unreachable, PrinterLinkFailure::cancelled};
    // As in MultiSlicer's printer_monitor.rs, each device is checked separately.
    // Authentication proves a connection; accepting a TCP socket alone does not.
    if (p.vendor == "bambu") {
        if (code.empty()) return {PrinterLinkStatus::refused, PrinterLinkFailure::missing_credentials};
        boost::system::error_code address_error;
        const auto address = boost::asio::ip::make_address(p.address, address_error);
        if (address_error) return {PrinterLinkStatus::unreachable, PrinterLinkFailure::invalid_address};
        boost::asio::io_context io;
        boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
        // Bambu's LAN endpoint presents a device-issued certificate that no local
        // CA verifies. Trust comes from the saved fingerprint instead: the leaf
        // certificate is captured here and compared before the access code is sent.
        context.set_verify_mode(boost::asio::ssl::verify_peer);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(io, context);
        std::string presented;
        stream.set_verify_callback([&presented](bool, boost::asio::ssl::verify_context& verify) {
            X509_STORE_CTX* store = verify.native_handle();
            if (X509_STORE_CTX_get_error_depth(store) == 0)
                presented = certificate_sha256(X509_STORE_CTX_get_current_cert(store));
            return true;
        });
        boost::asio::steady_timer timeout(io, std::chrono::seconds(4));
        boost::asio::steady_timer cancellation(io);
        PrinterLinkResult result;
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
        timeout.async_wait([&](const boost::system::error_code& ec) { if (!ec && !finished) { result.failure = PrinterLinkFailure::timeout; finish(); } });
        stream.lowest_layer().async_connect({address, static_cast<unsigned short>(p.port ? p.port : 8883)},
            [&](const boost::system::error_code& ec) {
                if (finished) return;
                if (ec) {
                    result.failure = ec == boost::asio::error::connection_refused ? PrinterLinkFailure::connection_refused : PrinterLinkFailure::unreachable;
                    finish(); return;
                }
                stream.async_handshake(boost::asio::ssl::stream_base::client, [&](const boost::system::error_code& ec) {
                    if (finished) return;
                    if (ec) { result.failure = PrinterLinkFailure::handshake; finish(); return; }
                    if (certificate) *certificate = presented;
                    switch (compare_certificate_pin(certificate_pin, presented)) {
                    case CertificatePin::missing: result.failure = PrinterLinkFailure::handshake; finish(); return;
                    // Another device, or a reset printer, answered at the saved address.
                    // Never send it the access code; the user re-trusts it explicitly.
                    case CertificatePin::changed:
                        result = {PrinterLinkStatus::refused, PrinterLinkFailure::certificate_changed};
                        finish(); return;
                    case CertificatePin::first_use:
                    case CertificatePin::matched: break;
                    }
                    boost::asio::async_write(stream, boost::asio::buffer(packet), [&](const boost::system::error_code& ec, size_t) {
                        if (finished) return;
                        if (ec) { result.failure = PrinterLinkFailure::send; finish(); return; }
                        boost::asio::async_read(stream, boost::asio::buffer(answer), [&](const boost::system::error_code& ec, size_t) {
                            if (finished) return;
                            result = ec ? PrinterLinkResult{PrinterLinkStatus::unreachable, PrinterLinkFailure::receive} : printer_mqtt_response(answer);
                            finish();
                        });
                    });
                });
            });
        std::function<void()> check_cancel;
        check_cancel = [&] {
            if (!cancelled || finished) return;
            if (cancelled->load()) { result.failure = PrinterLinkFailure::cancelled; finish(); return; }
            cancellation.expires_after(std::chrono::milliseconds(100));
            cancellation.async_wait([&](const boost::system::error_code& ec) { if (!ec) check_cancel(); });
        };
        check_cancel();
        io.run();
        return result;
    }
    PrinterLinkResult result;
    if (p.vendor == "elegoo") {
        if (!p.local_control) return {PrinterLinkStatus::unreachable, PrinterLinkFailure::printer_not_ready};
        if (discovered_printer_requires_code(p) && code.empty()) return {PrinterLinkStatus::refused, PrinterLinkFailure::missing_credentials};
        const auto endpoint = saved_printer_device_id(p);
        // The token travels only in the header, which the CC2's authenticated
        // upload endpoint already accepts, so it never lands in URL logs.
        auto request = Http::get("http://" + endpoint +
            (elegoo_printer_type(p.model) == ElegooPrinterType::CC2 ? "/system/info" : "/"));
        request.header("X-Token", elegoo_cc2_token(code));
        request.timeout_connect(2).timeout_max(4).size_limit(65536)
            .on_progress([cancelled](Http::Progress, bool& abort) { abort = cancelled && cancelled->load(); })
            .on_complete([&](std::string body, unsigned status) {
                if (elegoo_printer_type(p.model) == ElegooPrinterType::CC2) {
                    const auto info = parse_elegoo_cc2_info(body, status, p.serial);
                    result = info.connection;
                    if (serial && result.status == PrinterLinkStatus::connected) *serial = info.serial;
                } else if (status >= 200 && status < 300 &&
                           (body.find("ELEGOO") != std::string::npos || body.find("Elegoo") != std::string::npos))
                    // No CC identity reply is specified yet. As in ElegooLink's own
                    // test, this shows only that its upload service answered.
                    result = {PrinterLinkStatus::connected, PrinterLinkFailure::none};
                else result = printer_http_response("elegoo", {}, status);
            }).on_error([&](std::string, std::string, unsigned status) {
                result = printer_http_response("elegoo", {}, status);
            }).perform_sync();
        if (cancelled && cancelled->load()) return {PrinterLinkStatus::unreachable, PrinterLinkFailure::cancelled};
        return result;
    }
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
        .on_complete([&](std::string body, unsigned status) {
            result = printer_http_response(p.vendor, body, status);
        }).on_error([&](std::string, std::string, unsigned status) {
            // Do not retain the raw error or response body: either may contain
            // addresses, access codes or other server-supplied private data.
            result = printer_http_response(p.vendor, {}, status);
        }).perform_sync();
    if (cancelled && cancelled->load())
        return {PrinterLinkStatus::unreachable, PrinterLinkFailure::cancelled};
    return result;
}
} // namespace Slic3r
