#include "ElegooCanvas.hpp"
#include <boost/asio.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <thread>
#include <utility>

namespace Slic3r {
namespace {
class CanvasSocket final : public ElegooCanvasTransport {
    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket{context};
    std::function<bool()> canceled;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool committed = false;
    void check() {
        if ((!committed && canceled()) || std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(L("The printer stopped responding."));
    }
    void transfer(uint8_t* data, size_t length, bool writing) {
        size_t offset = 0;
        while (offset < length) {
            check(); boost::system::error_code ec;
            const auto n = writing ? socket.write_some(boost::asio::buffer(data + offset, length - offset), ec) :
                                     socket.read_some(boost::asio::buffer(data + offset, length - offset), ec);
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue;
            }
            if (ec || n == 0) throw std::runtime_error(L("Lost the connection to the printer. Check its LAN-only settings and address."));
            offset += n;
        }
    }
public:
    CanvasSocket(const std::string& address, std::function<bool()> cancel) : canceled(std::move(cancel)) {
        check(); boost::system::error_code ec;
        const auto ip = boost::asio::ip::make_address_v4(address, ec);
        if (ec) throw std::runtime_error(L("Enter this printer's local IPv4 address."));
        const auto n = ip.to_uint();
        // Deliberately private IPv4 only: this LAN protocol sends the access
        // code unencrypted, so never to a public, link-local or shared address.
        if (!((n >> 24) == 10 || (n >> 20) == 0xac1 || (n >> 16) == 0xc0a8))
            throw std::runtime_error(L("Enter this printer's local IPv4 address."));
        bool done = false; boost::system::error_code connection_error;
        socket.async_connect({ip, 1883}, [&](const boost::system::error_code& error) { connection_error = error; done = true; });
        const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done) {
            check(); context.poll();
            if (std::chrono::steady_clock::now() >= connect_deadline) throw std::runtime_error(L("The printer did not accept a connection."));
            if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (connection_error) throw std::runtime_error(L("Could not connect. Check the printer's LAN-only settings and address."));
        socket.non_blocking(true);
    }
    void commit() override {
        // A sent command gets its own full reply window; cancel no longer applies.
        committed = true;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    void send(uint8_t header, const std::vector<uint8_t>& body) override {
        std::vector<uint8_t> packet{header}; size_t length = body.size();
        do { uint8_t part = uint8_t(length % 128); length /= 128; packet.push_back(part | (length ? 128 : 0)); } while (length);
        packet.insert(packet.end(), body.begin(), body.end());
        transfer(packet.data(), packet.size(), true); std::fill(packet.begin(), packet.end(), 0);
    }
    std::pair<uint8_t, std::vector<uint8_t>> receive() override {
        uint8_t header; transfer(&header, 1, false);
        size_t length = 0, factor = 1;
        for (int i = 0; i < 4; ++i) {
            uint8_t part; transfer(&part, 1, false); length += (part & 127) * factor;
            if (length > 262144) throw std::runtime_error(L("The printer sent an invalid reply."));
            if (!(part & 128)) { std::vector<uint8_t> body(length); transfer(body.data(), body.size(), false); return {header, std::move(body)}; }
            factor *= 128;
        }
        throw std::runtime_error(L("The printer sent an invalid reply."));
    }
};
}
ElegooCanvasResult fetch_elegoo_canvas(const std::string& address, const std::string& serial,
                                      const std::string& secret, const std::function<bool()>& canceled)
{
    try {
        if (canceled()) return {{}, L("Filament sync was canceled.")};
        if (!elegoo_canvas_serial_valid(serial) || secret.size() > 256)
            return {{}, L("Check this printer's serial number and LAN access code.")};
        CanvasSocket socket(address, canceled);
        return read_elegoo_canvas(socket, serial, secret, "porca_" + boost::uuids::to_string(boost::uuids::random_generator()()));
    } catch (const std::exception& e) { return {{}, e.what()}; }
}
ElegooPrintStartResult send_elegoo_print_start(const std::string& address, const std::string& serial, const std::string& secret,
                                               const ElegooPrintJob& job, const std::function<bool()>& canceled)
{
    try {
        if (const char* problem = elegoo_print_job_problem(job)) return {false, problem};
        if (!elegoo_canvas_serial_valid(serial) || secret.size() > 256)
            return {false, L("Check this printer's serial number and LAN access code.")};
        CanvasSocket socket(address, canceled);
        return start_elegoo_print(socket, serial, secret, "porca_" + boost::uuids::to_string(boost::uuids::random_generator()()), job);
    } catch (const std::exception& e) { return {false, e.what()}; }
}
}
