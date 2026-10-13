#include "Bonjour.hpp"
#include <string>
#include <atomic>
#include <utility>
#include <boost/asio/ip/address_v4.hpp>
#include "SystemBonjour.hpp"
#ifdef __APPLE__
#include "DiscoveryNetwork.hpp"
#include "libslic3r/DiscoveryPolicy.h"
#include "libslic3r/DiscoveryDns.hpp"
#include <dns_sd.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

namespace Slic3r {
namespace {
struct DnsReference {
    DNSServiceRef ref = nullptr;
    DnsReference() = default;
    DnsReference(const DnsReference&) = delete;
    DnsReference& operator=(const DnsReference&) = delete;
    ~DnsReference() { if (ref) DNSServiceRefDeallocate(ref); }
};

class SystemBrowser {
    struct Query : DnsReference {
        SystemBrowser* owner;
        std::string name, host;
        BonjourReply::TxtData txt;
        uint16_t port = 0;
        bool enumerate = false;
    };
    DnsReference m_connection;
    std::vector<std::unique_ptr<Query>> m_queries;
    std::set<std::string> m_seen;
    const Bonjour::TxtKeys& m_keys;
    const Bonjour::ReplyFn& m_reply;
public:
    SystemBrowser(const Bonjour::TxtKeys& keys, const Bonjour::ReplyFn& reply) : m_keys(keys), m_reply(reply)
    { DNSServiceCreateConnection(&m_connection.ref); }

    bool browse(const std::string& type, unsigned index, bool enumerate)
    {
        if (enumerate) return record("_services._dns-sd._udp.local.", kDNSServiceType_PTR, {}, index, true);
        if (!m_connection.ref || m_queries.size() >= 128 || !m_seen.insert(type + ":" + std::to_string(index)).second) return false;
        auto query = std::make_unique<Query>();
        query->owner = this;
        query->enumerate = enumerate;
        auto ref = m_connection.ref;
        if (DNSServiceBrowse(&ref, kDNSServiceFlagsShareConnection, index, type.c_str(), "local.", browse_reply, query.get()) != kDNSServiceErr_NoError)
            return false;
        query->ref = ref;
        m_queries.push_back(std::move(query));
        return true;
    }

    void address(std::string name, std::string host, uint16_t port, BonjourReply::TxtData txt, unsigned index)
    {
        if (!m_connection.ref || m_queries.size() >= 128) return;
        auto query = std::make_unique<Query>();
        query->owner = this;
        query->name = std::move(name);
        query->host = std::move(host);
        query->port = port;
        query->txt = std::move(txt);
        auto ref = m_connection.ref;
        if (DNSServiceGetAddrInfo(&ref, kDNSServiceFlagsShareConnection, index, kDNSServiceProtocol_IPv4,
                                 query->host.c_str(), address_reply, query.get()) != kDNSServiceErr_NoError) return;
        query->ref = ref;
        m_queries.push_back(std::move(query));
    }

    void run(const std::atomic<bool>& cancelled, unsigned timeout)
    {
        if (!m_connection.ref) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        const int fd = DNSServiceRefSockFD(m_connection.ref);
        if (fd < 0 || fd >= FD_SETSIZE) return;
        while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
            fd_set descriptors;
            FD_ZERO(&descriptors);
            FD_SET(fd, &descriptors);
            timeval wait{0, 100000};
            const int ready = select(fd + 1, &descriptors, nullptr, nullptr, &wait);
            if (ready < 0) break;
            if (ready > 0 && DNSServiceProcessResult(m_connection.ref) != kDNSServiceErr_NoError) break;
        }
    }
private:
    static void browse_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error,
                             const char* name, const char* type, const char* domain, void* context)
    {
        if (error || !(flags & kDNSServiceFlagsAdd)) return;
        auto* query = static_cast<Query*>(context);
        auto* owner = query->owner;
        if (owner->m_queries.size() >= 128 ||
            !owner->m_seen.insert(std::string(name) + ":" + type + ":" + std::to_string(index)).second) return;
        auto resolve = std::make_unique<Query>();
        resolve->owner = owner;
        resolve->name = std::string(type).find("Creality") != std::string::npos || std::string(type).find("creality") != std::string::npos ? std::string(type) + "local." : name;
        auto ref = owner->m_connection.ref;
        if (DNSServiceResolve(&ref, kDNSServiceFlagsShareConnection, index, name, type, domain, resolve_reply, resolve.get()) != kDNSServiceErr_NoError)
            return;
        resolve->ref = ref;
        owner->m_queries.push_back(std::move(resolve));
    }

    bool record(const std::string& fullname, uint16_t kind, const std::string& service, unsigned index, bool enumerate = false)
    {
        // Some Creality service types exceed Browse/Resolve's conventional
        // service-name length. Query their PTR/SRV records directly through
        // mDNSResponder instead of falling back to raw multicast.
        if (!m_connection.ref || m_queries.size() >= 128 || !m_seen.insert(fullname + ":" + std::to_string(kind) + ":" + std::to_string(index)).second) return false;
        auto query = std::make_unique<Query>();
        query->owner = this;
        query->name = service;
        query->enumerate = enumerate;
        auto ref = m_connection.ref;
        if (DNSServiceQueryRecord(&ref, kDNSServiceFlagsShareConnection, index, fullname.c_str(), kind,
                                  kDNSServiceClass_IN, record_reply, query.get()) != kDNSServiceErr_NoError) return false;
        query->ref = ref;
        m_queries.push_back(std::move(query));
        return true;
    }

    static void record_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType error,
                             const char*, uint16_t kind, uint16_t, uint16_t length, const void* rdata, uint32_t, void* context)
    {
        if (error || !(flags & kDNSServiceFlagsAdd) || !rdata) return;
        auto* query = static_cast<Query*>(context);
        const auto* data = static_cast<const unsigned char*>(rdata);
        if (kind == kDNSServiceType_PTR) {
            if (const auto name = discovery_dns_name(data, length)) {
                if (query->enumerate) {
                    if (name->find("Creality") != std::string::npos || name->find("creality") != std::string::npos)
                        query->owner->record(*name, kDNSServiceType_PTR, *name, index);
                } else query->owner->record(*name, kDNSServiceType_SRV, query->name, index);
            }
        } else if (kind == kDNSServiceType_SRV && length > 6) {
            if (const auto host = discovery_dns_name(data + 6, length - 6))
                query->owner->address(query->name, *host, (uint16_t(data[4]) << 8) | data[5], {}, index);
        }
    }

    static void resolve_reply(DNSServiceRef, DNSServiceFlags, uint32_t index, DNSServiceErrorType error,
                              const char*, const char* host, uint16_t port, uint16_t length,
                              const unsigned char* txt, void* context)
    {
        if (error) return;
        auto* query = static_cast<Query*>(context);
        BonjourReply::TxtData fields;
        for (const auto& key : query->owner->m_keys) {
            uint8_t size = 0;
            const auto* value = static_cast<const char*>(TXTRecordGetValuePtr(length, txt, key.c_str(), &size));
            if (value) fields.emplace(key, std::string(value, size));
        }
        query->owner->address(query->name, host, ntohs(port), std::move(fields), index);
    }

    static void address_reply(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType error,
                              const char*, const sockaddr* address, uint32_t, void* context)
    {
        if (error || !(flags & kDNSServiceFlagsAdd) || !address || address->sa_family != AF_INET) return;
        auto* query = static_cast<Query*>(context);
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        const auto ip = ntohl(ipv4->sin_addr.s_addr);
        if (!discovery_ipv4_allowed(ip)) return;
        if (query->owner->m_reply)
            query->owner->m_reply(BonjourReply(boost::asio::ip::address_v4(ip), query->port, query->name, query->host, query->txt));
    }
};
} // namespace

bool system_bonjour_lookup(const std::string& service, const std::string& protocol, const Bonjour::TxtKeys& keys,
                           const std::atomic<bool>& cancelled, unsigned timeout, const Bonjour::ReplyFn& reply, bool enumerate_types)
{
    SystemBrowser browser(keys, reply);
    bool started = false;
    for (const auto& interface : discovery_interfaces()) {
        if (cancelled.load()) break;
        started = browser.browse(enumerate_types ? "_services._dns-sd._udp." : "_" + service + "._" + protocol + ".",
                                 interface.index, enumerate_types) || started;
    }
    if (started) browser.run(cancelled, timeout);
    return started;
}

void system_bonjour_resolve(const std::string& hostname, const std::atomic<bool>& cancelled,
                            unsigned timeout, const Bonjour::ResolveFn& reply)
{
    std::vector<BonjourReply> replies;
    const Bonjour::TxtKeys keys;
    const Bonjour::ReplyFn collect = [&replies](BonjourReply&& result) {
        if (std::find(replies.begin(), replies.end(), result) == replies.end()) replies.push_back(std::move(result));
    };
    SystemBrowser browser(keys, collect);
    const std::string host = hostname.find('.') == std::string::npos ? hostname + ".local." : hostname;
    for (const auto& interface : discovery_interfaces()) browser.address({}, host, 0, {}, interface.index);
    browser.run(cancelled, timeout);
    if (!cancelled.load() && reply) reply(replies);
}
} // namespace Slic3r
#endif
