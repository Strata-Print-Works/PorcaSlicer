#include "PlateLayout.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <set>
#include <vector>

namespace Slic3r {

bool same_plate_layout_settings(const DynamicPrintConfig& source, const DynamicPrintConfig& target)
{
    // Sorted for binary search. Compared in place: the planner calls this for
    // every pair of plates, so copying both full configs dominated the check.
    static constexpr std::array<std::string_view, 13> ignored{
        "different_settings_to_system", "flashforge_serial_number", "host_type", "print_host", "print_host_webui",
        "printer_agent", "printhost_apikey", "printhost_cafile", "printhost_password", "printhost_port",
        "printhost_user", "wipe_tower_x", "wipe_tower_y"};
    assert(std::is_sorted(ignored.begin(), ignored.end()));
    const auto compared = [](auto it, auto end) {
        while (it != end && std::binary_search(ignored.begin(), ignored.end(), std::string_view(it->first))) ++it;
        return it;
    };
    auto a = compared(source.cbegin(), source.cend());
    auto b = compared(target.cbegin(), target.cend());
    while (a != source.cend() && b != target.cend()) {
        if (a->first != b->first || *a->second != *b->second) return false;
        a = compared(std::next(a), source.cend());
        b = compared(std::next(b), target.cend());
    }
    return a == source.cend() && b == target.cend();
}

bool can_keep_fixed_job(const DynamicPrintConfig& job, const std::string& job_destination,
                        const DynamicPrintConfig& target, const std::string& target_destination)
{
    // Grouping ignores connection and tower bookkeeping, so a job may differ
    // from its group's representative in exactly those keys.
    return job_destination == target_destination && same_plate_layout_settings(job, target);
}

bool same_assigned_filaments(const DynamicPrintConfig& source, const DynamicPrintConfig& target,
                             const std::vector<int>& used_filaments)
{
    if (used_filaments.empty()) return false;
    for (const char* key : {"filament_type", "filament_settings_id", "filament_colour"}) {
        const auto* source_values = source.option<ConfigOptionStrings>(key);
        const auto* target_values = target.option<ConfigOptionStrings>(key);
        if (!source_values || !target_values) return false;
        for (int slot : used_filaments) {
            if (slot <= 0 || size_t(slot) > source_values->values.size() ||
                size_t(slot) > target_values->values.size() ||
                source_values->values[size_t(slot - 1)].empty() ||
                source_values->values[size_t(slot - 1)] != target_values->values[size_t(slot - 1)])
                return false;
        }
    }
    return true;
}

double fill_density_gap(FillDensity density)
{
    switch (density) {
    case FillDensity::Spacious: return 5.;
    case FillDensity::Balanced: return 3.;
    case FillDensity::Tight: return 1.;
    case FillDensity::Max: return 0.2; // Keep separate pieces from touching/fusing.
    }
    return 3.;
}

ExPolygons layout_footprint(const PlateLayoutShape& shape, const PlateLayoutPlacement& placement)
{
    ExPolygons result = shape.footprint;
    for (auto& poly : result) {
        poly.rotate(placement.rotation);
        poly.translate(scaled(placement.offset.x()), scaled(placement.offset.y()));
    }
    return result;
}

namespace {
struct Occupied {
    ExPolygons polygons;
    BoundingBox bounds;
    explicit Occupied(ExPolygons p) : polygons(std::move(p)), bounds(get_extents(polygons)) {}
};
struct Orientation {
    double angle;
    ExPolygons envelope;
    BoundingBox bounds;
};

// Uniform buckets over the bed. A candidate is tested only against envelopes
// sharing a bucket with its bounds, rather than every copy placed so far.
// Overlapping bounds always share a bucket, so results are unchanged.
class OccupiedIndex
{
public:
    OccupiedIndex(const BoundingBox& area, coord_t step)
        : m_origin(area.min), m_cell(std::max(step, area.size().maxCoeff() / 64 + 1)),
          m_columns(size_t(area.size().x() / m_cell) + 1), m_cells(m_columns * (size_t(area.size().y() / m_cell) + 1))
    {}

    void add(const BoundingBox& bounds)
    {
        const auto [x0, x1] = span(bounds.min.x(), bounds.max.x(), m_origin.x(), m_columns);
        const auto [y0, y1] = span(bounds.min.y(), bounds.max.y(), m_origin.y(), m_cells.size() / m_columns);
        for (size_t y = y0; y <= y1; ++y)
            for (size_t x = x0; x <= x1; ++x) m_cells[y * m_columns + x].push_back(m_seen.size());
        m_seen.push_back(0);
    }

    // Visits each nearby envelope once; stops when visit returns true.
    template<class Visit> bool any_near(const BoundingBox& bounds, Visit&& visit)
    {
        ++m_query;
        const auto [x0, x1] = span(bounds.min.x(), bounds.max.x(), m_origin.x(), m_columns);
        const auto [y0, y1] = span(bounds.min.y(), bounds.max.y(), m_origin.y(), m_cells.size() / m_columns);
        for (size_t y = y0; y <= y1; ++y)
            for (size_t x = x0; x <= x1; ++x)
                for (size_t index : m_cells[y * m_columns + x]) {
                    if (m_seen[index] == m_query) continue;
                    m_seen[index] = m_query;
                    if (visit(index)) return true;
                }
        return false;
    }

private:
    std::pair<size_t, size_t> span(coord_t low, coord_t high, coord_t origin, size_t count) const
    {
        const auto bucket = [&](coord_t value) {
            return value <= origin ? size_t(0) : std::min(count - 1, size_t((value - origin) / m_cell));
        };
        return {bucket(low), bucket(high)};
    }

    Point m_origin;
    coord_t m_cell;
    size_t m_columns;
    std::vector<std::vector<size_t>> m_cells;
    std::vector<size_t> m_seen;
    size_t m_query = 0;
};
}

PlateLayoutResult compute_plate_layout(const PlateLayoutInput& input,
                                      const std::function<bool()>& canceled,
                                      const std::function<void(int)>& progress)
{
    PlateLayoutResult best;
    if (input.max_copies == 0 || input.bed.empty() || input.shapes.empty() || !std::isfinite(input.gap) || input.gap < 0.) return best;
    const BoundingBox bed_bounds = get_extents(input.bed);
    if (!bed_bounds.defined || bed_bounds.size().minCoeff() <= 0) return best;
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline = input.deadline;
    if (input.time_limit > 0. && std::isfinite(input.time_limit))
        deadline = std::min(deadline, Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                         std::chrono::duration<double>(std::min(input.time_limit, 86400.))));
    if (Clock::now() >= deadline) { best.search_limited = true; return best; }
    // A convex bed contains every envelope whose bounding box corners it contains,
    // so most candidates skip the polygon difference against the bed.
    const bool convex_bed = input.bed.size() == 1 && input.bed.front().holes.empty() &&
                            polygon_is_convex(input.bed.front().contour);
    const auto inside_bed = [&](const ExPolygons& envelope, const BoundingBox& bounds) {
        const Polygon& contour = input.bed.front().contour;
        if (convex_bed && contour.contains(bounds.min) && contour.contains(bounds.max) &&
            contour.contains(Point(bounds.min.x(), bounds.max.y())) && contour.contains(Point(bounds.max.x(), bounds.min.y())))
            return true;
        return diff_ex(envelope, input.bed).empty();
    };
    // Consolidation must scan the whole bed within its bounded budget. A fine
    // 1 mm raster spent the entire trial on the first few rows of large beds.
    const double step_mm = input.optimize && !input.fill
        ? std::max(1., unscale<double>(bed_bounds.size().maxCoeff()) / 48.)
        : input.gap <= 0.2 ? 0.5 : 1.;
    const coord_t step = std::max(scaled(step_mm), bed_bounds.size().maxCoeff() / 2000);
    std::vector<std::vector<Orientation>> orientations;
    for (const auto& shape : input.shapes) {
        if (shape.footprint.empty() || !std::isfinite(shape.clearance) || shape.clearance < 0.) return best;
        std::vector<Orientation> variants;
        // 180 degrees is deliberately tried before quarter turns: alternating
        // directions can interlock tapered/flexi silhouettes without tilting them.
        for (double angle : {0., PI, PI / 2., 3. * PI / 2., PI / 4., 5. * PI / 4., 3. * PI / 4., 7. * PI / 4.}) {
            ExPolygons poly = layout_footprint(shape, {0, Vec2d::Zero(), angle});
            poly = offset_ex(poly, scaled(std::max(input.gap / 2., shape.clearance) + 0.005));
            if (poly.empty()) return best;
            variants.push_back({angle, poly, get_extents(poly)});
            if (!input.allow_rotation) break;
        }
        orientations.push_back(std::move(variants));
    }
    const bool optimize = input.optimize && !input.fill;
    std::vector<Occupied> fixed;
    // Towers, exclusions and collision zones keep the requested gap like fixed parts do.
    if (!input.excluded.empty()) fixed.emplace_back(offset_ex(input.excluded, scaled(input.gap / 2. + 0.005)));
    for (const auto& shape : input.fixed)
        if (!shape.footprint.empty())
            fixed.emplace_back(offset_ex(shape.footprint, scaled(std::max(input.gap / 2., shape.clearance) + 0.005)));
    // CAD parts often have rectangular recesses. Their axis-aligned edges are
    // useful placement candidates that a coarse bed-wide raster can miss.
    const auto add_edge_coordinates = [](const ExPolygons& polygons, std::set<coord_t>* xs,
                                         std::set<coord_t>* ys) {
        const auto add_contour = [&](const Polygon& contour) {
            for (size_t i = 0; i < contour.points.size(); ++i) {
                const auto& a = contour.points[i];
                const auto& b = contour.points[(i + 1) % contour.points.size()];
                if (xs && a.x() == b.x()) xs->insert(a.x());
                if (ys && a.y() == b.y()) ys->insert(a.y());
            }
        };
        for (const auto& polygon : polygons) {
            add_contour(polygon.contour);
            for (const auto& hole : polygon.holes) add_contour(hole);
        }
    };

    // Compare an unchanged-direction pass with alternating-row and freely
    // rotated passes. All passes use the same polygon checks and budget; no
    // count threshold switches to unchecked bounding-box placement.
    const int passes = optimize ? (input.allow_rotation ? 5 : 3) : input.allow_rotation ? 3 : 1;
    size_t attempts = 0;
    bool timed_out = false;
    double best_area = std::numeric_limits<double>::infinity();
    for (int pass = 0; pass < passes && !timed_out; ++pass) {
        std::vector<size_t> order(input.shapes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        if (optimize && pass == (input.allow_rotation ? 3 : 1))
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const auto area = [&](size_t source) {
                    const auto size = get_extents(input.shapes[source].footprint).size();
                    return double(size.x()) * double(size.y());
                };
                return area(a) > area(b);
            });
        else if (optimize && pass == (input.allow_rotation ? 4 : 2))
            std::reverse(order.begin(), order.end());
        auto occupied = fixed;
        OccupiedIndex nearby(bed_bounds, step);
        for (const auto& obstacle : occupied) nearby.add(obstacle.bounds);
        PlateLayoutResult trial;
        size_t next_source = 0;
        size_t row = 0;
        bool exhausted = false;
        const size_t pass_budget = input.max_attempts;
        size_t pass_attempts = 0;
        std::set<coord_t> rows;
        for (coord_t y = bed_bounds.min.y(); y <= bed_bounds.max.y(); y += step) rows.insert(y);
        // Every row starts from the raster plus each obstacle's right edge (and,
        // when optimizing, its axis-aligned edges). Maintain that set for the pass
        // instead of rebuilding it from every obstacle on each row.
        std::set<coord_t> pass_columns;
        for (coord_t x = bed_bounds.min.x(); x <= bed_bounds.max.x(); x += step) pass_columns.insert(x);
        const auto add_obstacle_columns = [&](const Occupied& obstacle) {
            if (obstacle.bounds.max.x() >= bed_bounds.min.x() && obstacle.bounds.max.x() <= bed_bounds.max.x())
                pass_columns.insert(obstacle.bounds.max.x());
            if (optimize) add_edge_coordinates(obstacle.polygons, &pass_columns, nullptr);
        };
        for (const auto& obstacle : occupied) {
            add_obstacle_columns(obstacle);
            if (optimize) add_edge_coordinates(obstacle.polygons, nullptr, &rows);
        }
        // Columns added while a row is scanned are new obstacles' edges. They are
        // candidates for the rest of this row and every later one, so each row
        // walks the pass set directly; std::set insertion keeps the iterator valid.
        for (auto yi = rows.begin(); yi != rows.end() && !exhausted; ++yi, ++row) {
            const coord_t y = *yi;
            if (y < bed_bounds.min.y() || y > bed_bounds.max.y()) continue;
            for (auto xi = pass_columns.begin(); xi != pass_columns.end() && !exhausted; ++xi) {
                const coord_t x = *xi;
                if (x < bed_bounds.min.x() || x > bed_bounds.max.x()) continue;
                if (canceled && canceled()) { best.canceled = true; best.placements.clear(); return best; }
                const size_t source = order[next_source];
                auto& variants = orientations[source];
                const size_t choices = pass == 0 ? 1 : pass == 1 && input.allow_rotation ? 2 : variants.size();
                bool placed = false;
                for (size_t k = 0; k < choices; ++k) {
                    ++attempts;
                    if (++pass_attempts > pass_budget) { trial.search_limited = true; exhausted = true; break; }
                    if ((attempts & 63) == 0 && Clock::now() >= deadline) {
                        trial.search_limited = timed_out = exhausted = true;
                        break;
                    }
                    const size_t index = pass == 1 && input.allow_rotation ? (k + row % 2) % 2 :
                                         pass >= 3 ? (k + pass - 2) % variants.size() : k;
                    const auto& variant = variants[index];
                    if (x + variant.bounds.size().x() > bed_bounds.max.x() || y + variant.bounds.size().y() > bed_bounds.max.y()) continue;
                    const Point delta(x - variant.bounds.min.x(), y - variant.bounds.min.y());
                    ExPolygons envelope = variant.envelope;
                    for (auto& poly : envelope) poly.translate(delta);
                    const BoundingBox bounds = get_extents(envelope);
                    const bool collision = nearby.any_near(bounds, [&](size_t nearby_index) {
                        const auto& obstacle = occupied[nearby_index];
                        return bounds.overlap(obstacle.bounds) && !intersection_ex(envelope, obstacle.polygons).empty();
                    });
                    if (collision || !inside_bed(envelope, bounds)) continue;
                    trial.placements.push_back({source, delta.cast<double>() * SCALING_FACTOR, variant.angle});
                    occupied.emplace_back(std::move(envelope));
                    nearby.add(occupied.back().bounds);
                    // Exact envelope-edge candidates avoid rounding a 0.2 mm
                    // gap up to the search grid, even for thousands of copies.
                    add_obstacle_columns(occupied.back());
                    if (optimize) add_edge_coordinates(occupied.back().polygons, nullptr, &rows);
                    if (bounds.max.y() > y && bounds.max.y() <= bed_bounds.max.y()) rows.insert(bounds.max.y());
                    next_source = (next_source + 1) % input.shapes.size();
                    placed = true;
                    if ((!input.fill && next_source == 0) || trial.placements.size() >= input.max_copies) {
                        trial.search_limited = input.fill && trial.placements.size() >= input.max_copies;
                        exhausted = true;
                    }
                    break;
                }
                if (placed && progress) progress(std::min(99, int(100 * attempts / std::max(size_t(1), input.max_attempts * passes))));
            }
        }
        // Filling several selected objects repeats complete sets only.
        if (input.fill) trial.placements.resize(trial.placements.size() - next_source);
        const bool limited = best.search_limited || trial.search_limited;
        double area = std::numeric_limits<double>::infinity();
        if (input.optimize && !input.fill && !trial.placements.empty()) {
            BoundingBox occupied_bounds;
            for (const auto& placement : trial.placements)
                occupied_bounds.merge(get_extents(layout_footprint(input.shapes[placement.source], placement)));
            const auto size = occupied_bounds.size();
            area = double(size.x()) * double(size.y());
        }
        if (trial.placements.size() > best.placements.size() ||
            (input.optimize && !input.fill && trial.placements.size() == best.placements.size() && area < best_area)) {
            best_area = area;
            best = std::move(trial);
        }
        best.search_limited = limited;

        if (input.stop_on_fit && !input.fill && best.placements.size() == input.shapes.size()) {
            if (progress) progress(100);
            return best;
        }

    }
    if (progress) progress(100);
    return best;
}

PlateConsolidationResult consolidate_plate_layouts(const std::vector<PlateConsolidationPlate>& plates,
                                                   const std::function<bool()>& canceled,
                                                   const std::function<void(int)>& progress,
                                                   bool balance_targets)
{
    PlateConsolidationResult result;
    result.plates.resize(plates.size());
    for (size_t i = 0; i < plates.size(); ++i) {
        auto& state = result.plates[i];
        state.layout = plates[i].layout;
        state.layout.fill = false;
        for (size_t shape = 0; shape < state.layout.shapes.size(); ++shape)
            state.sources.push_back({i, shape});
    }

    // Empty later plates into earlier compatible ones. Trial placements are kept
    // separate until every object on the donor has a destination; a failed trial
    // never causes a partial transfer that leaves the plate in use.
    for (size_t donor = plates.size(); donor-- > 0;) {
        if (canceled && canceled()) { result.canceled = true; return result; }
        // Plates not yet emptied stay as they are once the plan's time is spent.
        if (std::chrono::steady_clock::now() >= plates[donor].layout.deadline) { result.search_limited = true; break; }
        if (!plates[donor].can_remove || !plates[donor].layout.fixed.empty() ||
            result.plates[donor].sources.empty()) continue;

        std::map<size_t, PlateConsolidationPlateResult> proposals;
        bool complete = true;
        for (size_t shape = 0; shape < result.plates[donor].sources.size(); ++shape) {
            bool placed = false;
            std::vector<size_t> targets;
            targets.reserve(donor);
            for (size_t target = 0; target < donor; ++target)
                if (plates[target].group == plates[donor].group && !result.plates[target].removed)
                    targets.push_back(target);
            if (balance_targets) {
                // Measure each target once rather than on every sort comparison.
                std::vector<std::pair<double, size_t>> loads;
                loads.reserve(targets.size());
                for (size_t target : targets) {
                    const auto proposal = proposals.find(target);
                    const auto& layout = proposal == proposals.end() ? result.plates[target].layout : proposal->second.layout;
                    double area = 0.;
                    for (const auto& item : layout.shapes) {
                        const auto size = get_extents(item.footprint).size();
                        area += double(size.x()) * double(size.y());
                    }
                    loads.emplace_back(area, target);
                }
                std::stable_sort(loads.begin(), loads.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
                for (size_t i = 0; i < loads.size(); ++i) targets[i] = loads[i].second;
            }
            for (size_t target : targets) {
                if (plates[target].group != plates[donor].group || result.plates[target].removed) continue;
                const auto source = result.plates[donor].sources[shape];
                auto& trial = proposals.try_emplace(target, result.plates[target]).first->second;
                PlateLayoutInput input = trial.layout;
                input.shapes.push_back(result.plates[donor].layout.shapes[shape]);
                input.max_copies = std::max(input.max_copies, input.shapes.size());
                const auto placement = compute_plate_layout(input, canceled);
                if (placement.canceled) { result.canceled = true; return result; }
                result.search_limited |= placement.search_limited;
                if (placement.placements.size() != input.shapes.size()) continue;
                trial.layout = std::move(input);
                trial.placement = placement;
                trial.sources.push_back(result.plates[donor].sources[shape]);
                trial.changed = true;
                placed = true;
                break;
            }
            if (!placed) { complete = false; break; }
        }
        if (complete) {
            for (auto& [target, trial] : proposals) result.plates[target] = std::move(trial);
            result.plates[donor].removed = true;
        }
        if (progress) progress(int(100 * (plates.size() - donor) / std::max(size_t(1), plates.size())));
    }
    return result;
}

PlateConsolidationResult repack_plate_layouts(const std::vector<PlateConsolidationPlate>& plates,
                                             const std::function<bool()>& canceled,
                                             const std::function<void(int)>& progress)
{
    // Material groups keep pieces with different assignments apart. Without one
    // group per piece the plan could mix them, so refuse before planning.
    for (const auto& plate : plates)
        if (!plate.shape_groups.empty() && plate.shape_groups.size() != plate.layout.shapes.size())
            throw std::invalid_argument("Some pieces are missing their material assignments.");
    PlateConsolidationResult result;
    struct Item { PlateConsolidationRef source; size_t material; double area; };
    std::vector<Item> items;
    std::vector<size_t> materials(plates.size(), std::numeric_limits<size_t>::max());
    for (size_t i = 0; i < plates.size(); ++i) {
        const auto& source = plates[i];
        PlateConsolidationPlateResult bin;
        bin.layout = source.layout;
        bin.layout.fill = false;
        bin.layout.stop_on_fit = true;
        bin.template_plate = i;
        if (source.can_remove && source.layout.fixed.empty()) {
            bin.layout.shapes.clear();
            for (size_t j = 0; j < source.layout.shapes.size(); ++j) {
                const auto size = get_extents(source.layout.shapes[j].footprint).size();
                items.push_back({{i, j}, source.shape_groups.empty() ? 0 : source.shape_groups[j],
                                 double(size.x()) * double(size.y())});
            }
        } else {
            for (size_t j = 0; j < source.layout.shapes.size(); ++j) bin.sources.push_back({i, j});
        }
        result.plates.push_back(std::move(bin));
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.area > b.area; });
    for (size_t n = 0; n < items.size(); ++n) {
        if (canceled && canceled()) { result.canceled = true; return result; }
        const auto& item = items[n];
        const auto& source = plates[item.source.plate];
        if (std::chrono::steady_clock::now() >= source.layout.deadline) {
            // Pieces without a place are reported rather than left off their plates.
            result.search_limited = true;
            for (size_t rest = n; rest < items.size(); ++rest) result.unplaced.push_back(items[rest].source);
            break;
        }
        bool placed = false;
        for (size_t i = 0; i <= result.plates.size(); ++i) {
            const bool fresh = i == result.plates.size();
            if (!fresh) {
                const auto& original = plates[result.plates[i].template_plate];
                if (!original.can_remove || !original.layout.fixed.empty() || original.group != source.group ||
                    (!result.plates[i].sources.empty() && materials[i] != item.material)) continue;
            }
            auto input = fresh ? source.layout : result.plates[i].layout;
            if (fresh) input.shapes.clear();
            input.fill = false;
            input.stop_on_fit = true;
            input.shapes.push_back(source.layout.shapes[item.source.shape]);
            input.max_copies = std::max(input.max_copies, input.shapes.size());
            auto placement = compute_plate_layout(input, canceled);
            if (placement.canceled) { result.canceled = true; return result; }
            result.search_limited |= placement.search_limited;
            if (placement.placements.size() != input.shapes.size()) {
                if (fresh) break;
                continue;
            }
            if (fresh) {
                PlateConsolidationPlateResult bin;
                bin.template_plate = item.source.plate;
                result.plates.push_back(std::move(bin));
                materials.push_back(item.material);
            }
            auto& bin = result.plates[i];
            bin.layout = std::move(input);
            bin.placement = std::move(placement);
            bin.sources.push_back(item.source);
            bin.changed = true;
            materials[i] = item.material;
            placed = true;
            break;
        }
        if (!placed) result.unplaced.push_back(item.source);
        if (progress) progress(int(100 * (n + 1) / std::max(size_t(1), items.size())));
    }
    for (auto& bin : result.plates)
        if (plates[bin.template_plate].can_remove && plates[bin.template_plate].layout.fixed.empty())
            bin.removed = bin.sources.empty();
    return result;
}

std::vector<PrinterStageAssignment> assign_printer_stages(const std::vector<PrinterStageJob>& jobs,
                                                          size_t printer_count)
{
    if (printer_count == 0) return {};
    std::vector<size_t> count(printer_count, 0);
    std::vector<double> load(printer_count, 0.);
    std::vector<PrinterStageAssignment> result;
    result.reserve(jobs.size());
    for (const auto& job : jobs) {
        if (!std::isfinite(job.load) || job.load < 0.) return {};
        size_t chosen = printer_count;
        for (size_t candidate : job.eligible_printers) {
            if (candidate >= printer_count ||
                (job.fixed_printer != std::numeric_limits<size_t>::max() && job.fixed_printer != candidate)) continue;
            if (chosen == printer_count || count[candidate] < count[chosen] ||
                (count[candidate] == count[chosen] && load[candidate] < load[chosen])) chosen = candidate;
        }
        if (chosen == printer_count) return {};
        result.push_back({chosen, ++count[chosen]});
        load[chosen] += job.load;
    }
    return result;
}

} // namespace Slic3r
