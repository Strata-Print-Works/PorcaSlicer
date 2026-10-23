#pragma once

#include "DiscoveryPolicy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <utility>
#include <set>
#include <string>

namespace Slic3r {
class DiscoveryScanGate {
    std::atomic<bool> m_active{false};
public:
    class Lease {
        DiscoveryScanGate* m_gate = nullptr;
        friend class DiscoveryScanGate;
        explicit Lease(DiscoveryScanGate* gate) : m_gate(gate) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : m_gate(std::exchange(other.m_gate, nullptr)) {}
        Lease& operator=(Lease&& other) noexcept {
            reset();
            m_gate = std::exchange(other.m_gate, nullptr);
            return *this;
        }
        ~Lease() { reset(); }
        explicit operator bool() const { return m_gate != nullptr; }
        void reset() { if (m_gate) { m_gate->m_active.store(false); m_gate = nullptr; } }
    };
    Lease try_acquire() {
        bool expected = false;
        return m_active.compare_exchange_strong(expected, true) ? Lease(this) : Lease();
    }
    bool active() const { return m_active.load(); }
};

// A scan receives asynchronous completions from distinct vendor producers.
// Duplicate/late completion notifications must not finish another producer.
class DiscoveryScanProgress {
    unsigned m_count = 0, m_available = 0;
    std::set<unsigned> m_completed;
public:
    void reset(unsigned count) { m_count = count; m_available = 0; m_completed.clear(); }
    void complete(unsigned producer, bool available) {
        if (producer < m_count && m_completed.insert(producer).second && available) ++m_available;
    }
    bool finished() const { return m_completed.size() == m_count; }
    bool available() const { return m_available > 0; }
};

// Advance from actual send time: delayed work must never catch up in a burst.
class DiscoveryProbeSchedule {
    std::chrono::steady_clock::time_point m_next{};
public:
    bool ready(std::chrono::steady_clock::time_point now) const { return now >= m_next; }
    void sent(std::chrono::steady_clock::time_point now) { m_next = now + std::chrono::milliseconds(100); }
};

inline unsigned discovery_retry_seconds(unsigned failures, bool connected)
{
    if (connected) return 30;
    return 5u << std::min(failures > 0 ? failures - 1 : 0, 6u); // 5..320 seconds
}

// Bambu LAN printers present a device-issued certificate that no local CA can
// verify. The first authenticated check pins its SHA-256 fingerprint; later
// checks must present the same certificate before the access code is sent.
enum class CertificatePin { missing, first_use, matched, changed };
inline CertificatePin compare_certificate_pin(const std::string& pinned, const std::string& presented)
{
    if (presented.empty()) return CertificatePin::missing;
    if (pinned.empty()) return CertificatePin::first_use;
    return pinned == presented ? CertificatePin::matched : CertificatePin::changed;
}

// A pin is stored only by an authenticated check that started without one, and
// only while none has been stored or cleared since that check started.
inline bool should_store_certificate_pin(const std::string& pinned_at_start, const std::string& pinned_now,
                                         const std::string& presented, bool authenticated)
{
    return authenticated && pinned_at_start.empty() && pinned_now.empty() && !presented.empty();
}

inline std::string certificate_fingerprint_hex(const unsigned char* digest, size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    if (!digest) return result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += digits[digest[i] >> 4];
        result += digits[digest[i] & 0x0f];
    }
    return result;
}
} // namespace Slic3r
