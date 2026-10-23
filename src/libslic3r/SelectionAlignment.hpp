#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace Slic3r {

enum class AlignmentAction { Minimum, Center, Maximum, DistributeCenters, DistributeGaps, SetGap };
struct AlignmentInterval { double min, max; };

// Values are measured on one physical bed axis. Return translations, never new
// dimensions, so a layout operation cannot alter scale, orientation or Z.
inline std::vector<double> alignment_offsets(const std::vector<AlignmentInterval>& items,
                                             AlignmentAction action, AlignmentInterval reference,
                                             double gap = 0.)
{
    std::vector<double> result(items.size(), 0.);
    const auto valid = [](const AlignmentInterval& interval) {
        return std::isfinite(interval.min) && std::isfinite(interval.max) && interval.max >= interval.min;
    };
    if (items.empty() || !std::isfinite(gap) || gap < 0. || !std::all_of(items.begin(), items.end(), valid)) return result;
    if (action <= AlignmentAction::Maximum) {
        // Only alignment uses the anchor; an invalid one must not move anything.
        if (!valid(reference)) return result;
        for (size_t i = 0; i < items.size(); ++i)
            result[i] = action == AlignmentAction::Minimum ? reference.min - items[i].min :
                        action == AlignmentAction::Maximum ? reference.max - items[i].max :
                        (reference.min + reference.max - items[i].min - items[i].max) / 2.;
        return result;
    }
    if (items.size() < (action == AlignmentAction::SetGap ? 2u : 3u)) return result;
    std::vector<size_t> order(items.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return action == AlignmentAction::DistributeCenters ?
            items[a].min + items[a].max < items[b].min + items[b].max : items[a].min < items[b].min;
    });
    const auto& first = items[order.front()];
    const auto& last = items[order.back()];
    if (action == AlignmentAction::DistributeCenters) {
        const double start = (first.min + first.max) / 2.;
        const double step = ((last.min + last.max) / 2. - start) / (items.size() - 1);
        for (size_t i = 1; i + 1 < order.size(); ++i)
            result[order[i]] = start + i * step - (items[order[i]].min + items[order[i]].max) / 2.;
    } else {
        if (action == AlignmentAction::DistributeGaps) {
            double widths = 0.;
            for (const auto& item : items) widths += item.max - item.min;
            gap = (last.max - first.min - widths) / (items.size() - 1);
        }
        double next = first.min;
        for (size_t i : order) {
            result[i] = next - items[i].min;
            next += items[i].max - items[i].min + gap;
        }
    }
    return result;
}

} // namespace Slic3r
