#pragma once

#include "DiscoveryPolicy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

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
} // namespace Slic3r
