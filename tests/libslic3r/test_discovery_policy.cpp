#include "libslic3r/DiscoveryPolicy.h"
#include <catch2/catch_test_macros.hpp>
#include "libslic3r/DiscoveryPolicy.hpp"
#include "libslic3r/DiscoveryDns.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Discovery rejects inactive virtual peer-to-peer and link-local interfaces", "[DiscoveryPolicy]")
{
    constexpr uint32_t lan = 0xc0a8029d; // 192.168.2.157
    for (const char* name : {"en0", "eth0", "wlan0", "enp3s0", "wlp2s0"})
        CHECK(discovery_interface_allowed(name, lan, 1, 1, 1, 0, 0));
    for (const char* name : {"lo0", "utun3", "bridge100", "awdl0", "llw0", "ap1", "anpi0", "vmenet0", "vmnet1", "wg0", "docker0", "veth12", "tap0", "tun0", "p2p0"})
        CHECK_FALSE(discovery_interface_allowed(name, lan, 1, 1, 1, 0, 0));
    for (const uint32_t ip : {0u, 0x7f000002u, 0xa9fe0101u, 0xe00000fbu, 0xffffffffu})
        CHECK_FALSE(discovery_interface_allowed("en0", ip, 1, 1, 1, 0, 0));
    CHECK_FALSE(discovery_interface_allowed("en0", lan, 0, 1, 1, 0, 0));
    CHECK_FALSE(discovery_interface_allowed("en0", lan, 1, 0, 1, 0, 0));
    CHECK_FALSE(discovery_interface_allowed("en0", lan, 1, 1, 0, 0, 0));
    CHECK_FALSE(discovery_interface_allowed("en0", lan, 1, 1, 1, 1, 0));
    CHECK_FALSE(discovery_interface_allowed("en0", lan, 1, 1, 1, 0, 1));
    CHECK_FALSE(discovery_interface_allowed(nullptr, lan, 1, 1, 1, 0, 0));
}

TEST_CASE("Broadcast eligibility requires a real contiguous subnet mask", "[DiscoveryPolicy]")
{
    CHECK(discovery_netmask_valid(0xffffff00));
    CHECK(discovery_netmask_valid(0xfffffe00));
    CHECK(discovery_netmask_valid(0xfffff000));
    CHECK_FALSE(discovery_netmask_valid(0));
    CHECK_FALSE(discovery_netmask_valid(0xffff00ff));
    CHECK_FALSE(discovery_netmask_valid(0xffffffff));
    CHECK_FALSE(discovery_netmask_valid(0xfffffffe));
}

TEST_CASE("Competing vendor probes share a send cap without catching up after a stall", "[DiscoveryPolicy]")
{
    DiscoveryProbeSchedule schedule;
    const auto beginning = std::chrono::steady_clock::time_point{};
    std::array<unsigned, 6> fake_transport_sends{};
    for (unsigned ms = 0; ms < 1000; ++ms) {
        const auto now = beginning + std::chrono::milliseconds(ms);
        for (auto& count : fake_transport_sends)
            if (schedule.ready(now)) { ++count; schedule.sent(now); }
    }
    unsigned total = 0;
    for (const auto count : fake_transport_sends) total += count;
    CHECK(total == 10);
    const auto resumed = beginning + std::chrono::seconds(5);
    REQUIRE(schedule.ready(resumed));
    schedule.sent(resumed);
    CHECK_FALSE(schedule.ready(resumed + std::chrono::milliseconds(99)));
    CHECK(schedule.ready(resumed + std::chrono::milliseconds(100)));
}

TEST_CASE("Scan ownership excludes another dialog and releases after worker cleanup", "[DiscoveryPolicy]")
{
    DiscoveryScanGate gate;
    auto scan = gate.try_acquire();
    REQUIRE(bool(scan));
    CHECK_FALSE(bool(gate.try_acquire())); // Same-thread reentrancy is safe too.
    auto worker_owner = std::move(scan);
    CHECK_FALSE(bool(scan));
    CHECK_FALSE(bool(gate.try_acquire()));
    worker_owner.reset();
    CHECK_FALSE(gate.active());
    { auto replacement = gate.try_acquire(); REQUIRE(bool(replacement)); }
    CHECK_FALSE(gate.active());
    CHECK(bool(gate.try_acquire()));
}

TEST_CASE("Offline checks back off exponentially and successful checks reset their pace", "[DiscoveryPolicy]")
{
    const std::array<unsigned, 8> expected{5, 5, 10, 20, 40, 80, 160, 320};
    for (unsigned i = 0; i < expected.size(); ++i) CHECK(discovery_retry_seconds(i, false) == expected[i]);
    CHECK(discovery_retry_seconds(100, false) == 320);
    CHECK(discovery_retry_seconds(100, true) == 30);
    CHECK(discovery_retry_seconds(0, true) == 30);
}

TEST_CASE("System discovery preserves long service types and escaped DNS labels", "[DiscoveryPolicy]")
{
    std::vector<unsigned char> name;
    for (const auto& label : {std::string("_Creality-543324280CDB19"), std::string("_udp"), std::string("local")}) {
        name.push_back(static_cast<unsigned char>(label.size()));
        name.insert(name.end(), label.begin(), label.end());
    }
    name.push_back(0);
    REQUIRE(discovery_dns_name(name.data(), name.size()));
    CHECK(*discovery_dns_name(name.data(), name.size()) == "_Creality-543324280CDB19._udp.local.");
    name.pop_back();
    CHECK_FALSE(discovery_dns_name(name.data(), name.size()));
    const std::array<unsigned char, 8> literal_dot{3, 'K', '.', '2', 2, 'a', 'b', 0};
    REQUIRE(discovery_dns_name(literal_dot.data(), literal_dot.size()));
    CHECK(*discovery_dns_name(literal_dot.data(), literal_dot.size()) == "K\\.2.ab.");
    const std::array<unsigned char, 2> compressed{0xc0, 0};
    CHECK_FALSE(discovery_dns_name(compressed.data(), compressed.size()));
    CHECK_FALSE(discovery_dns_name(nullptr, 10));
}

TEST_CASE("Printer certificate pins are learned once and reject a changed certificate", "[DiscoveryPolicy]")
{
    const std::array<unsigned char, 4> fake_digest{0x00, 0x9f, 0xa0, 0xff};
    const auto first = certificate_fingerprint_hex(fake_digest.data(), fake_digest.size());
    CHECK(first == "009fa0ff");
    CHECK(certificate_fingerprint_hex(nullptr, 4).empty());
    const std::string other = "11111111";

    CHECK(compare_certificate_pin("", "") == CertificatePin::missing);
    CHECK(compare_certificate_pin(first, "") == CertificatePin::missing);
    CHECK(compare_certificate_pin("", first) == CertificatePin::first_use);
    CHECK(compare_certificate_pin(first, first) == CertificatePin::matched);
    CHECK(compare_certificate_pin(first, other) == CertificatePin::changed);

    // Only an authenticated first-use check stores the presented fingerprint.
    CHECK(should_store_certificate_pin("", "", first, true));
    CHECK_FALSE(should_store_certificate_pin("", "", first, false));
    CHECK_FALSE(should_store_certificate_pin("", "", "", true));
    // An existing pin is never replaced by a later check.
    CHECK_FALSE(should_store_certificate_pin(first, first, first, true));
    // A pin stored by another check, or cleared by an explicit credential reset
    // while this check ran, is left as the user or the other check set it.
    CHECK_FALSE(should_store_certificate_pin("", other, first, true));
    CHECK_FALSE(should_store_certificate_pin(other, "", first, true));
}
