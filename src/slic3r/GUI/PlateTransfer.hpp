#pragma once
#include "libslic3r/BoundingBox.hpp"
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {
class GLCanvas3D;

// An instance whose proposed placement differs from the model.
struct PlateMoveCandidate
{
    int source = -1;        // Plate that holds the instance now; -1 outside every plate.
    BoundingBoxf3 proposed; // Model parts at the proposed placement.
};

// The plate each candidate lands on, or -1 outside every plate. An instance
// stays on its plate while its proposed bounds still touch that plate's volume;
// otherwise it joins the first plate it touches.
std::vector<int> plate_move_destinations(const std::vector<PlateMoveCandidate>& moves,
                                         const std::vector<BoundingBoxf3>& plate_volumes);

// Intercepts cross-plate instance moves before geometry, membership or undo is
// changed. Returns true when the deferred transaction owns the move. The
// transaction records snapshot_name as its undo step when it applies. An empty
// name means the caller already recorded the state before the move; that step
// then also covers the transfer.
bool defer_plate_transfer(GLCanvas3D& canvas, const std::string& snapshot_name);
}}
