#pragma once

#include "Geometry.hpp"
#include <cstddef>
#include <map>
#include <limits>
#include <set>
#include <vector>
#include <utility>

namespace Slic3r {
class DynamicPrintConfig;
class Model;
class ModelObject;

// One-based numbers, including explicit support and height-range assignments.
std::set<int> object_filament_slots(const ModelObject& object);
size_t plate_filament_count(const DynamicPrintConfig& config);
bool plate_filament_is_physical(const DynamicPrintConfig& config, int slot);

struct PlateFilamentCandidate
{
    int slot;
    bool exact = false;
    bool material_known = false;
    bool same_material = false;
    bool same_profile = false;
    bool same_color = false;
    double color_distance = std::numeric_limits<double>::infinity();
};

// Physical destinations, ordered by exact settings, material/diameter, profile,
// then perceptual color distance. Ranking never authorizes an approximate match.
std::vector<PlateFilamentCandidate> rank_plate_filaments(const DynamicPrintConfig& source, int from,
                                                       const DynamicPrintConfig& destination);

// Prefer a unique identical filament, then the same material/diameter and exact
// color (using the profile to disambiguate). Different settings still need review
// before committing the move. Missing entries require a choice; order is irrelevant.
std::map<int, int> match_plate_filaments(const ModelObject& object,
                                       const DynamicPrintConfig& source,
                                       const DynamicPrintConfig& destination);

struct PlateInstanceTransfer
{
    size_t object_idx;
    size_t instance_idx;
    Geometry::Transformation transformation;
    std::map<int, int> filaments;
    size_t destination_filament_count;
};

// Validates the whole batch before mutation. Separates copies only when their
// assignments change, preserving other instances' settings, paint and placement.
// Returns the new object/instance indices in input order.
void validate_plate_instance_transfers(const Model& model, const std::vector<PlateInstanceTransfer>& transfers);
std::vector<std::pair<size_t, size_t>> apply_plate_instance_transfers(
    Model& model, const std::vector<PlateInstanceTransfer>& transfers);
} // namespace Slic3r
