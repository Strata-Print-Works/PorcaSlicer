#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>
#include <string>
#include <boost/asio/ip/udp.hpp>

namespace Slic3r {
struct DiscoveryInterface {
    std::string name;
    uint32_t address;
    uint32_t netmask;
    unsigned index;
    bool broadcast;
};

// Fail closed: no hostname resolution, wildcard fallback, or virtual adapters.
std::vector<DiscoveryInterface> discovery_interfaces();
bool constrain_discovery_socket(boost::asio::ip::udp::socket& socket, unsigned interface_index);
bool wait_for_discovery_probe(const std::atomic<bool>* cancelled = nullptr,
                              std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
} // namespace Slic3r
