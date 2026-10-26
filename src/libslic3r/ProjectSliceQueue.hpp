#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace Slic3r {
class ProjectSliceQueue
{
public:
    struct Ticket {
        std::uint64_t project {0};
        std::uint64_t generation {0};
    };
    Ticket enqueue(std::uint64_t project)
    {
        const auto found = std::find_if(m_requests.begin(), m_requests.end(),
            [project](Ticket t) { return t.project == project; });
        if (found != m_requests.end()) return *found;
        m_requests.push_back({project, ++m_generation});
        return m_requests.back();
    }
    Ticket current() const { return m_requests.empty() ? Ticket{} : m_requests.front(); }
    bool complete(Ticket ticket)
    {
        const auto active = current();
        if (!active.project || active.project != ticket.project || active.generation != ticket.generation) return false;
        m_requests.pop_front();
        return true;
    }
    void cancel(std::uint64_t project)
    {
        m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(),
            [project](Ticket t) { return t.project == project; }), m_requests.end());
    }
private:
    std::uint64_t m_generation {0};
    std::deque<Ticket> m_requests;
};
}
