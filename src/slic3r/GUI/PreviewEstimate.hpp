#pragma once

#include <cmath>
#include <limits>
#include <optional>

namespace Slic3r::GUI {

// A scope sums measurements from each valid slice, never the active preset.
// Missing or unusable quantities remain unavailable after further additions.
struct PreviewEstimate
{
    std::optional<double> seconds = 0.0;
    std::optional<double> grams = 0.0;
    std::optional<double> cost = 0.0;

    void add_time(double value)
    {
        add(seconds, value);
        // Keep even damaged metadata safe for the integer-based time formatter.
        if (seconds && *seconds > double(std::numeric_limits<int>::max()))
            seconds.reset();
    }

    void add_filament(double volume_mm3, double density_g_cm3, std::optional<double> cost_per_kg)
    {
        if (volume_mm3 == 0.0)
            return;
        if (!std::isfinite(volume_mm3) || volume_mm3 < 0.0 ||
            !std::isfinite(density_g_cm3) || density_g_cm3 <= 0.0) {
            grams.reset();
            cost.reset();
            return;
        }
        const double weight = volume_mm3 * .001 * density_g_cm3;
        add(grams, weight);
        if (!cost_per_kg || !std::isfinite(*cost_per_kg) || *cost_per_kg < 0.0)
            cost.reset();
        else
            add(cost, weight * .001 * *cost_per_kg);
    }

private:
    static void add(std::optional<double>& total, double value)
    {
        if (total) {
            const double sum = *total + value;
            if (std::isfinite(value) && value >= 0.0 && std::isfinite(sum))
                *total = sum;
            else
                total.reset();
        }
    }
};

} // namespace Slic3r::GUI
