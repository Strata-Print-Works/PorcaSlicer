#pragma once

#include "DiscoveredPrinter.hpp"
#include "DiscoveryPolicy.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace Slic3r {
inline constexpr unsigned elegoo_discovery_port = 52700;
inline constexpr char elegoo_discovery_probe[] = R"({"id":0,"method":7000})";

inline bool elegoo_reply_on_interface(uint32_t source, uint32_t address, uint32_t mask)
{
    return discovery_netmask_valid(mask) && discovery_ipv4_allowed(source) &&
        (source & mask) == (address & mask) && source != (address & mask) && source != (address | ~mask);
}

struct ElegooDiscoveryDatagram {
    std::string body, source;
    unsigned port = 0;
    bool on_interface = false;
};
// Narrow I/O seam used by the real owned sockets and socket-free fixtures.
class ElegooDiscoveryTransport {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~ElegooDiscoveryTransport() = default;
    virtual Clock::time_point now() const = 0;
    virtual bool cancelled() const = 0;
    virtual size_t interface_count() const = 0;
    virtual void send_probe(size_t index, Clock::time_point deadline) = 0;
    virtual std::optional<ElegooDiscoveryDatagram> receive(size_t index) = 0;
    virtual void idle() = 0;
};

inline bool run_elegoo_discovery(ElegooDiscoveryTransport& transport,
                                 const std::function<void(DiscoveredPrinter)>& found)
{
    using Clock = ElegooDiscoveryTransport::Clock;
    if (transport.cancelled() || !transport.interface_count()) return false;
    const auto deadline = transport.now() + std::chrono::seconds(6);
    auto next_probe = transport.now();
    const auto count = std::min(transport.interface_count(), size_t(4));
    std::vector<DiscoveredPrinter> seen;
    size_t received = 0;
    const auto active = [&] { return !transport.cancelled() && transport.now() < deadline && received < 1024; };
    while (active()) {
        if (transport.now() >= next_probe) {
            for (size_t i = 0; i < count && active(); ++i) transport.send_probe(i, deadline);
            next_probe = transport.now() + std::chrono::seconds(2); // never catch up after a stall
        }
        for (size_t i = 0; i < count && active(); ++i) {
            for (size_t packet = 0; packet < 32 && active(); ++packet) {
                auto datagram = transport.receive(i);
                if (!datagram) break;
                ++received;
                if (!datagram->on_interface) continue;
                auto printer = parse_elegoo_printer_notice(datagram->body, datagram->source, datagram->port);
                if (!printer) continue;
                const auto previous = std::find_if(seen.begin(), seen.end(), [&](const auto& item) {
                    return same_discovered_printer(item, *printer);
                });
                if (previous == seen.end()) {
                    if (seen.size() >= 256) continue;
                    seen.push_back(*printer);
                } else {
                    if (previous->serialize() == printer->serialize()) continue;
                    *previous = *printer;
                }
                if (active()) found(std::move(*printer));
            }
        }
        if (active()) transport.idle();
    }
    return true;
}
} // namespace Slic3r
