#pragma once

#include "ObjectID.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

enum class SliceScope { Plate, Stage, Project };

struct PlateSliceCandidate
{
    ObjectID plate_id;
    std::string stage_id;
    bool printable {false};
    bool result_valid {false};
};

// Pure queue state; the GUI resolves each stable plate ID and owns the worker.
class PlateSliceBatch
{
public:
    enum class State { Idle, Running, Completed, Failed, Cancelled };

    // Candidate order is retained; the first occurrence owns a repeated ID.
    void begin(const std::vector<PlateSliceCandidate>& candidates, SliceScope scope,
               ObjectID active_plate, const std::string& stage_id, bool force = false);
    ObjectID current() const;
    bool complete(uint64_t generation, ObjectID plate_id, bool success);
    void cancel();

    uint64_t generation() const { return m_generation; }
    State state() const { return m_state; }
    bool running() const { return m_state == State::Running; }
    // Counts refer to jobs selected at begin(), including valid results only if forced.
    size_t completed() const { return m_completed; }
    size_t total() const { return m_total; }

private:
    std::vector<ObjectID> m_targets;
    uint64_t m_generation {0};
    State m_state {State::Idle};
    size_t m_completed {0};
    size_t m_total {0};
};

} // namespace Slic3r
