#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

// Palette indices are local to a plate. Only a complete material identity may
// combine quantities across plates; the profile distinguishes unnamed finishes.
struct FilamentUsageIdentity
{
    std::string material;
    std::string finish;
    std::string color;
    std::string brand;
    std::string profile;
    double diameter_mm = 0;
    bool known = false;
};

struct FilamentUsageMeasurement
{
    size_t slot = 0;
    FilamentUsageIdentity identity;
    double volume_mm3 = 0;
    double density_g_cm3 = 0;
    bool physical = true;
};

struct PlateFilamentUsage
{
    size_t plate_id = 0;
    bool fresh = false;
    std::vector<FilamentUsageMeasurement> filaments;
};

struct FilamentUsageRow
{
    FilamentUsageIdentity identity;
    size_t fallback_plate_id = 0;
    size_t fallback_slot = 0;
    double grams = 0;
    double meters = 0;
    bool has_quantity = false;
    bool complete = true;
};

struct FilamentUsageReport
{
    std::vector<FilamentUsageRow> rows;
    std::vector<size_t> incomplete_plates;
    bool complete() const { return incomplete_plates.empty(); }
};

// Uses total modeled extrusion, including support and purge. Stale slices are
// never included, and virtual material quantities are never apportioned by guess.
FilamentUsageReport aggregate_filament_usage(const std::vector<PlateFilamentUsage>& plates);

} // namespace Slic3r
