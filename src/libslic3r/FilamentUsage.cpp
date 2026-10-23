#include "FilamentUsage.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace Slic3r {

namespace {
bool same_identity(const FilamentUsageIdentity& a, const FilamentUsageIdentity& b)
{
    return std::tie(a.material, a.finish, a.color, a.brand, a.profile, a.diameter_mm) ==
           std::tie(b.material, b.finish, b.color, b.brand, b.profile, b.diameter_mm);
}
bool usable_identity(const FilamentUsageIdentity& identity)
{
    return identity.known && !identity.material.empty() && !identity.color.empty() &&
           !identity.brand.empty() && !identity.profile.empty() &&
           std::isfinite(identity.diameter_mm) && identity.diameter_mm > 0;
}
}

FilamentUsageReport aggregate_filament_usage(const std::vector<PlateFilamentUsage>& plates)
{
    FilamentUsageReport report;
    std::set<size_t> seen;
    for (const auto& plate : plates) {
        // A scoped selection can repeat IDs. Count a plate exactly once.
        if (!seen.insert(plate.plate_id).second) continue;
        bool incomplete = !plate.fresh || plate.filaments.empty();
        if (plate.fresh) {
            for (const auto& filament : plate.filaments) {
                if (filament.volume_mm3 == 0) continue;
                const bool identified = usable_identity(filament.identity);
                auto found = std::find_if(report.rows.begin(), report.rows.end(), [&](const FilamentUsageRow& row) {
                    return identified ? usable_identity(row.identity) && same_identity(row.identity, filament.identity)
                                      : !usable_identity(row.identity) && row.fallback_plate_id == plate.plate_id && row.fallback_slot == filament.slot;
                });
                if (found == report.rows.end()) {
                    report.rows.push_back({filament.identity, plate.plate_id, filament.slot});
                    found = report.rows.end() - 1;
                    found->identity.known = identified;
                }
                const double diameter = filament.identity.diameter_mm;
                const bool valid = filament.physical && std::isfinite(filament.volume_mm3) && filament.volume_mm3 >= 0 &&
                                   std::isfinite(filament.density_g_cm3) && filament.density_g_cm3 > 0 &&
                                   std::isfinite(diameter) && diameter > 0;
                double grams = 0, meters = 0;
                if (valid) {
                    grams = filament.volume_mm3 * filament.density_g_cm3 / 1000.0;
                    meters = filament.volume_mm3 / (std::acos(-1.0) * diameter * diameter / 4.0) / 1000.0;
                }
                if (!valid || !std::isfinite(grams) || !std::isfinite(meters) ||
                    !std::isfinite(found->grams + grams) || !std::isfinite(found->meters + meters)) {
                    found->complete = false;
                    incomplete = true;
                    continue;
                }
                found->grams += grams;
                found->meters += meters;
                found->has_quantity = true;
            }
        }
        if (incomplete) report.incomplete_plates.push_back(plate.plate_id);
    }
    return report;
}

} // namespace Slic3r
