#include "PlateSliceBatch.hpp"
#include "ObjectID.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

void PlateSliceBatch::begin(const std::vector<PlateSliceCandidate>& candidates, SliceScope scope,
                            ObjectID active_plate, const std::string& stage_id, bool force)
{
    ++m_generation;
    m_state = State::Idle;
    m_targets.clear();
    m_completed = 0;
    m_total = 0;
    std::set<ObjectID> seen;
    for (const auto& candidate : candidates) {
        if (candidate.plate_id.invalid() || !seen.insert(candidate.plate_id).second) continue;
        const bool selected = scope == SliceScope::Project ||
            (scope == SliceScope::Plate && candidate.plate_id == active_plate) ||
            (scope == SliceScope::Stage && !stage_id.empty() && candidate.stage_id == stage_id);
        if (selected && candidate.printable && (force || !candidate.result_valid))
            m_targets.push_back(candidate.plate_id);
    }
    m_total = m_targets.size();
    m_state = m_total == 0 ? State::Completed : State::Running;
}

ObjectID PlateSliceBatch::current() const
{
    return running() ? m_targets[m_completed] : ObjectID{};
}

bool PlateSliceBatch::complete(uint64_t generation, ObjectID plate_id, bool success)
{
    if (!running() || generation != m_generation || plate_id != current()) return false;
    if (!success) {
        m_state = State::Failed;
        m_targets.clear();
    } else if (++m_completed == m_total) {
        m_state = State::Completed;
        m_targets.clear();
    }
    return true;
}

void PlateSliceBatch::cancel()
{
    ++m_generation;
    m_state = State::Cancelled;
    m_targets.clear();
}

} // namespace Slic3r
