#pragma once

#include "ExPolygon.hpp"
#include "Point.hpp"
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

// Printer changes may not substitute the material or color used by any printed slot.
bool same_assigned_filaments(const DynamicPrintConfig& source, const DynamicPrintConfig& target,
                             const std::vector<int>& used_filaments);

// Connection details and per-plate tower positions do not change a job's
// resolved print settings. Tower footprints are checked separately by packing.
bool same_plate_layout_settings(const DynamicPrintConfig& source, const DynamicPrintConfig& target);

// A job with objects that stay on its plate keeps its printer. A saved printer
// can take it only when it is the same destination with the same print settings.
bool can_keep_fixed_job(const DynamicPrintConfig& job, const std::string& job_destination,
                        const DynamicPrintConfig& target, const std::string& target_destination);

enum class FillDensity { Spacious, Balanced, Tight, Max };
double fill_density_gap(FillDensity density);

struct PlateLayoutShape {
    ExPolygons footprint; // Already scaled/tilted; XY relative to the instance origin.
    double clearance = 0.; // Per-side brim/support/tool clearance, in mm.
};
struct PlateLayoutPlacement {
    size_t source = 0;
    Vec2d offset = Vec2d::Zero(); // Physical bed coordinates, mm.
    double rotation = 0.; // Additional rotation about Z, radians.
};
struct PlateLayoutInput {
    ExPolygons bed;
    ExPolygons excluded;
    std::vector<PlateLayoutShape> fixed; // Footprints in physical bed coordinates.
    std::vector<PlateLayoutShape> shapes;
    double gap = 3.;
    bool allow_rotation = false;
    bool optimize = false; // Compare complete layouts by occupied footprint after searching orientations.
    bool stop_on_fit = false; // Feasibility checks need no later area comparison once every shape fits.
    bool fill = true; // Repeat complete sets, or place each shape once.
    size_t max_copies = 2000;
    size_t max_attempts = 600000; // Per pass; enabling rotation retains the complete baseline pass.
    // Wall-clock bounds. Reaching either keeps the best layout found so far and
    // reports search_limited. A plan that runs many layouts shares one deadline.
    double time_limit = 10.; // Seconds for this layout; zero or less disables it.
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};
struct PlateLayoutResult {
    std::vector<PlateLayoutPlacement> placements;
    bool canceled = false;
    bool search_limited = false;
};

ExPolygons layout_footprint(const PlateLayoutShape& shape, const PlateLayoutPlacement& placement);
PlateLayoutResult compute_plate_layout(const PlateLayoutInput& input,
                                      const std::function<bool()>& canceled = {},
                                      const std::function<void(int)>& progress = {});

struct PlateConsolidationPlate {
    PlateLayoutInput layout;
    size_t group = 0; // Only plates with a compatible resolved setup share a group.
    bool can_remove = true;
    std::vector<size_t> shape_groups; // Optional per-piece material group for a complete repack.
};
struct PlateConsolidationRef {
    size_t plate = 0;
    size_t shape = 0;
};
struct PlateConsolidationPlateResult {
    PlateLayoutInput layout;
    PlateLayoutResult placement;
    std::vector<PlateConsolidationRef> sources;
    bool changed = false;
    bool removed = false;
    size_t template_plate = 0; // Setup/bed to use if this result needs a new physical plate.
};
struct PlateConsolidationResult {
    std::vector<PlateConsolidationPlateResult> plates;
    bool canceled = false;
    bool search_limited = false;
    std::vector<PlateConsolidationRef> unplaced;
};

PlateConsolidationResult consolidate_plate_layouts(const std::vector<PlateConsolidationPlate>& plates,
                                                   const std::function<bool()>& canceled = {},
                                                   const std::function<void(int)>& progress = {},
                                                   bool balance_targets = false);

// Pack every eligible piece, creating additional beds when necessary. Fixed
// jobs remain intact; setup and material groups never mix. No input is mutated.
PlateConsolidationResult repack_plate_layouts(const std::vector<PlateConsolidationPlate>& plates,
                                             const std::function<bool()>& canceled = {},
                                             const std::function<void(int)>& progress = {});

struct PrinterStageJob {
    std::vector<size_t> eligible_printers;
    size_t fixed_printer = std::numeric_limits<size_t>::max();
    double load = 0.; // Printable footprint area; used only to break equal job-count ties.
};
struct PrinterStageAssignment {
    size_t printer = 0;
    size_t stage = 0; // One-based wave for this printer.
};
// Jobs are supplied in scheduling order. An empty result means some job has no valid printer.
std::vector<PrinterStageAssignment> assign_printer_stages(const std::vector<PrinterStageJob>& jobs,
                                                          size_t printer_count);

} // namespace Slic3r
