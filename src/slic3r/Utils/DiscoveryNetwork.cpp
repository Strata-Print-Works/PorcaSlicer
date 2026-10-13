#include "libslic3r/DiscoveryPolicy.h"
#include <vector>
#include <atomic>
#include <boost/asio/ip/udp.hpp>
#include "DiscoveryNetwork.hpp"
#include "libslic3r/DiscoveryPolicy.hpp"
#include <algorithm>
#include <string>
#include <cwctype>
#include <chrono>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace Slic3r {
std::vector<DiscoveryInterface> discovery_interfaces()
{
    std::vector<DiscoveryInterface> result;
#ifdef _WIN32
    ULONG length = 16384;
    std::vector<unsigned char> buffer(length);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, adapters, &length);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(length);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                     nullptr, adapters, &length);
    }
    if (status != NO_ERROR) return result;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->PhysicalAddressLength == 0 ||
            (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211) ||
            (adapter->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
        std::wstring description = adapter->Description ? adapter->Description : L"";
        std::transform(description.begin(), description.end(), description.begin(), [](wchar_t c) { return std::towlower(c); });
        bool virtual_adapter = false;
        for (const auto* word : {L"virtual", L"vpn", L"nord", L"wireguard", L"hyper-v", L"vmware", L"docker", L"orbstack"})
            if (description.find(word) != std::wstring::npos) virtual_adapter = true;
        if (virtual_adapter) continue;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            if (item->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(item->Address.lpSockaddr);
            const auto ip = ntohl(addr->sin_addr.s_addr);
            if (!discovery_ipv4_allowed(ip)) continue;
            const uint32_t mask = (item->OnLinkPrefixLength == 0 || item->OnLinkPrefixLength > 32) ? 0 : 0xffffffffu << (32 - item->OnLinkPrefixLength);
            result.push_back({adapter->AdapterName, ip, mask, adapter->IfIndex, true});
        }
    }
#else
    ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) return result;
    for (auto* item = head; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
        const auto* addr = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        const auto ip = ntohl(addr->sin_addr.s_addr);
        const auto flags = item->ifa_flags;
        if (!discovery_interface_allowed(item->ifa_name, ip, flags & IFF_UP, flags & IFF_RUNNING,
                                        flags & IFF_MULTICAST, flags & IFF_LOOPBACK, flags & IFF_POINTOPOINT)) continue;
        const auto* mask = reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
        result.push_back({item->ifa_name, ip, mask ? ntohl(mask->sin_addr.s_addr) : 0,
                          if_nametoindex(item->ifa_name), bool(flags & IFF_BROADCAST)});
    }
    freeifaddrs(head);
#endif
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.address == b.address; }), result.end());
    if (result.size() > 4) result.resize(4);
    return result;
}

bool wait_for_discovery_probe(const std::atomic<bool>* cancelled)
{
    static std::mutex mutex;
    static DiscoveryProbeSchedule schedule;
    while (!cancelled || !cancelled->load()) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            if (schedule.ready(now)) { schedule.sent(now); return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool constrain_discovery_socket(boost::asio::ip::udp::socket& socket, unsigned interface_index)
{
#ifdef __APPLE__
    // Binding a source address alone does not prevent VPN policy routing.
    return interface_index != 0 && setsockopt(socket.native_handle(), IPPROTO_IP, IP_BOUND_IF,
                                            &interface_index, sizeof(interface_index)) == 0;
#else
    return true;
#endif
}
} // namespace Slic3r
