#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/PlateLayout.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace Slic3r;

TEST_CASE("Printer planning preserves each assigned filament and color", "[PlateLayout]") {
    DynamicPrintConfig source;
    source.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "TPU"}));
    source.set_key_value("filament_settings_id", new ConfigOptionStrings({"Strong PETG", "Flexible TPU"}));
    source.set_key_value("filament_colour", new ConfigOptionStrings({"#FF7490", "#222222"}));
    DynamicPrintConfig target = source;

    CHECK(same_assigned_filaments(source, target, {1}));
    CHECK(same_assigned_filaments(source, target, {1, 2}));
    CHECK_FALSE(same_assigned_filaments(source, target, {}));
    CHECK_FALSE(same_assigned_filaments(source, target, {3}));

    target.option<ConfigOptionStrings>("filament_type")->values[1] = "PETG";
    CHECK(same_assigned_filaments(source, target, {1}));
    CHECK_FALSE(same_assigned_filaments(source, target, {1, 2}));

    target = source;
    target.option<ConfigOptionStrings>("filament_settings_id")->values[0] = "Other PETG";
    CHECK_FALSE(same_assigned_filaments(source, target, {1}));

    target = source;
    target.option<ConfigOptionStrings>("filament_colour")->values[0] = "#222222";
    CHECK_FALSE(same_assigned_filaments(source, target, {1}));

    source.option<ConfigOptionStrings>("filament_settings_id")->values[0].clear();
    CHECK_FALSE(same_assigned_filaments(source, source, {1}));
}

TEST_CASE("Plate consolidation ignores connection and plate bookkeeping differences", "[PlateLayout]") {
    DynamicPrintConfig source = DynamicPrintConfig::full_print_config();
    source.set_key_value("wall_loops", new ConfigOptionInt(4));
    source.set_key_value("printer_agent", new ConfigOptionString(""));
    source.set_key_value("wipe_tower_x", new ConfigOptionFloats({165.}));
    source.set_key_value("wipe_tower_y", new ConfigOptionFloats({217.497}));
    auto target = source;
    target.set_key_value("printer_agent", new ConfigOptionString("orca"));
    target.set_key_value("print_host", new ConfigOptionString("offline-printer.invalid"));
    target.set_key_value("wipe_tower_x", new ConfigOptionFloats({165., 165., 71.4971}));
    target.set_key_value("wipe_tower_y", new ConfigOptionFloats({217.497, 217.497, 71.4971}));
    target.set_key_value("different_settings_to_system", new ConfigOptionStrings({"wall_loops"}));
    REQUIRE(source != target);
    CHECK(same_plate_layout_settings(source, target));
    CHECK(same_plate_layout_settings(target, source));
    // Comparing must not mutate either job or its connection details.
    CHECK(source.opt_string("printer_agent").empty());
    CHECK(target.opt_string("printer_agent") == "orca");
    CHECK(target.option<ConfigOptionFloats>("wipe_tower_x")->values.size() == 3);
}

TEST_CASE("A job that keeps objects on its plate stays on its own saved printer", "[PlateLayout]") {
    DynamicPrintConfig job;
    job.set_key_value("wall_loops", new ConfigOptionInt(3));
    job.set_key_value("wipe_tower_x", new ConfigOptionFloats({165.}));
    // The group's representative differs only in bookkeeping and connection details.
    auto representative = job;
    representative.set_key_value("wipe_tower_x", new ConfigOptionFloats({165., 71.5}));
    representative.set_key_value("print_host", new ConfigOptionString("other.invalid"));
    REQUIRE(job != representative);
    CHECK(can_keep_fixed_job(job, "printer-a", representative, "printer-a"));
    CHECK_FALSE(can_keep_fixed_job(job, "printer-a", representative, "printer-b"));
    representative.set_key_value("wall_loops", new ConfigOptionInt(4));
    CHECK_FALSE(can_keep_fixed_job(job, "printer-a", representative, "printer-a"));
}

TEST_CASE("Plate consolidation preserves real print setting differences", "[PlateLayout]") {
    const auto source = DynamicPrintConfig::full_print_config();
    CHECK(same_plate_layout_settings(source, source));
    auto target = source;
    target.set_key_value("wall_loops", new ConfigOptionInt(source.opt_int("wall_loops") + 1));
    CHECK_FALSE(same_plate_layout_settings(source, target));
    target = source;
    target.set_key_value("enable_support", new ConfigOptionBool(!source.opt_bool("enable_support")));
    CHECK_FALSE(same_plate_layout_settings(source, target));
    target = source;
    target.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.8}));
    CHECK_FALSE(same_plate_layout_settings(source, target));
    target = source;
    target.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000"}));
    CHECK_FALSE(same_plate_layout_settings(source, target));
    target = source;
    target.set_key_value("enable_prime_tower", new ConfigOptionBool(!source.opt_bool("enable_prime_tower")));
    CHECK_FALSE(same_plate_layout_settings(source, target));
}

TEST_CASE("Plate consolidation compares settings key by key without bookkeeping keys", "[PlateLayout]") {
    DynamicPrintConfig source;
    source.set_key_value("wall_loops", new ConfigOptionInt(3));
    source.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    source.set_key_value("print_host", new ConfigOptionString("a.invalid"));
    // The documented behavior: equal after removing only the ignored keys.
    const auto erased = [](DynamicPrintConfig config) {
        for (const char* key : {"print_host", "print_host_webui", "printhost_port", "printhost_apikey", "printhost_user",
                                "printhost_password", "printhost_cafile", "flashforge_serial_number", "printer_agent",
                                "host_type", "different_settings_to_system", "wipe_tower_x", "wipe_tower_y"})
            config.erase(key);
        return config;
    };
    const auto check = [&](const DynamicPrintConfig& target, bool same) {
        CHECK(same_plate_layout_settings(source, target) == same);
        CHECK(same_plate_layout_settings(target, source) == same);
        CHECK((erased(source) == erased(target)) == same);
    };
    auto target = source;
    target.set_key_value("print_host", new ConfigOptionString("b.invalid"));
    check(target, true);
    target.erase("print_host");
    target.set_key_value("wipe_tower_x", new ConfigOptionFloats({10.}));
    target.set_key_value("host_type", new ConfigOptionString("octoprint"));
    check(target, true);
    target = source;
    target.set_key_value("wall_loops", new ConfigOptionInt(4));
    check(target, false);
    target = source;
    target.set_key_value("sparse_infill_density", new ConfigOptionPercent(20.));
    check(target, false);
    target = source;
    target.erase("layer_height");
    check(target, false);
    check(DynamicPrintConfig{}, false);
    CHECK(same_plate_layout_settings(erased(source), source));
}

namespace {
ExPolygons rectangle(double x, double y, double w, double h) {
    return {ExPolygon(Polygon({Point(scaled(x),scaled(y)),Point(scaled(x+w),scaled(y)),Point(scaled(x+w),scaled(y+h)),Point(scaled(x),scaled(y+h))}))};
}
PlateLayoutInput fixture() {
    PlateLayoutInput in;
    in.bed = rectangle(0,0,45,40);
    in.shapes.push_back({rectangle(0,0,8,5),0});
    in.max_attempts = 100000;
    return in;
}
double area_mm2(const ExPolygons& polygons) {
    double area = 0.;
    for (const auto& polygon : polygons) area += polygon.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}
void verify(const PlateLayoutInput& in, const PlateLayoutResult& result) {
    std::vector<ExPolygons> envelopes;
    for (const auto& p : result.placements) {
        const auto shape = layout_footprint(in.shapes[p.source], p);
        REQUIRE(diff_ex(shape, in.bed).empty());
        REQUIRE(intersection_ex(shape, in.excluded).empty());
        const auto envelope = offset_ex(shape, scaled(in.gap / 2.));
        for (const auto& other : envelopes) REQUIRE(intersection_ex(envelope, other).empty());
        for (const auto& fixed : in.fixed)
            REQUIRE(intersection_ex(envelope, offset_ex(fixed.footprint, scaled(in.gap / 2.))).empty());
        envelopes.push_back(envelope);
        if (!in.allow_rotation) REQUIRE(p.rotation == 0.);
    }
}
}
TEST_CASE("Fill densities place separate pieces inside the physical bed", "[PlateLayout]") {
    auto in = fixture();
    size_t previous = 0;
    for (auto density : {FillDensity::Spacious, FillDensity::Balanced, FillDensity::Tight, FillDensity::Max}) {
        in.gap = fill_density_gap(density);
        const auto result = compute_plate_layout(in);
        REQUIRE(result.placements.size() >= previous);
        previous = result.placements.size();
        REQUIRE(previous > 0);
        verify(in,result);
    }
}
TEST_CASE("Fill honors exclusions fixed parts and concave beds", "[PlateLayout]") {
    auto in = fixture();
    in.bed = diff_ex(in.bed, rectangle(22,20,23,20));
    in.excluded = rectangle(0,0,10,40);
    in.fixed.push_back({rectangle(15,10,8,7),0});
    in.allow_rotation = true;
    const auto result = compute_plate_layout(in);
    REQUIRE_FALSE(result.placements.empty());
    verify(in,result);
}
TEST_CASE("Rotation can fit a part without changing its size", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(-20,-30,8,20);
    in.shapes = {{rectangle(0,0,14,4),0}};
    in.gap = 1.;
    REQUIRE(compute_plate_layout(in).placements.empty());
    in.allow_rotation = true;
    auto result = compute_plate_layout(in);
    REQUIRE_FALSE(result.placements.empty());
    verify(in,result);
}
TEST_CASE("A rotated placement keeps the piece's footprint shape and area", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(-20, -30, 8, 20);
    // An asymmetric outline that only fits this bed after a quarter turn.
    const ExPolygons outline {ExPolygon(Polygon({Point(0, 0), Point(scaled(14.), 0), Point(scaled(14.), scaled(2.)),
        Point(scaled(2.), scaled(2.)), Point(scaled(2.), scaled(4.)), Point(0, scaled(4.))}))};
    in.shapes = {{outline, 0}};
    in.gap = 1.;
    in.allow_rotation = true;
    const auto result = compute_plate_layout(in);
    REQUIRE_FALSE(result.placements.empty());
    verify(in, result);
    for (const auto& placement : result.placements) {
        CHECK(placement.rotation != 0.);
        const auto placed = layout_footprint(in.shapes[placement.source], placement);
        CHECK_THAT(area_mm2(placed), Catch::Matchers::WithinRel(area_mm2(outline), 1e-6));
        // Reversing the placement recovers the original outline.
        ExPolygons restored = placed;
        for (auto& polygon : restored) {
            polygon.translate(-scaled(placement.offset.x()), -scaled(placement.offset.y()));
            polygon.rotate(-placement.rotation);
        }
        CHECK(area_mm2(diff_ex(restored, outline)) < 1e-4);
        CHECK(area_mm2(diff_ex(outline, restored)) < 1e-4);
    }
}
TEST_CASE("Pieces keep the requested gap from towers and exclusions", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0, 0, 40, 20);
    in.excluded = rectangle(0, 0, 10, 20);
    in.gap = 2.;
    const auto result = compute_plate_layout(in);
    REQUIRE_FALSE(result.placements.empty());
    verify(in, result);
    for (const auto& placement : result.placements) {
        const auto piece = layout_footprint(in.shapes[placement.source], placement);
        // Growing a piece by just under the gap still clears the protected area.
        CHECK(intersection_ex(offset_ex(piece, float(scaled(in.gap) - 10)), in.excluded).empty());
    }
}
TEST_CASE("Fill keeps every piece inside a round bed", "[PlateLayout]") {
    auto in = fixture();
    Polygon circle;
    for (int i = 0; i < 64; ++i) {
        const double angle = 2. * PI * i / 64.;
        circle.points.emplace_back(scaled(30. + 30. * std::cos(angle)), scaled(30. + 30. * std::sin(angle)));
    }
    in.bed = {ExPolygon(circle)};
    in.gap = 1.;
    const auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() > 10);
    verify(in, result);
}
TEST_CASE("Reaching the copy limit reports a limited search", "[PlateLayout]") {
    auto in = fixture();
    in.max_copies = 3;
    const auto result = compute_plate_layout(in);
    CHECK(result.placements.size() == 3);
    CHECK(result.search_limited);
    verify(in, result);
}
TEST_CASE("A fill that cannot finish in its time stops and reports a limited search", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0, 0, 300, 300);
    in.shapes = {{rectangle(0, 0, 1, 1), 0}};
    in.gap = fill_density_gap(FillDensity::Max);
    in.max_copies = std::numeric_limits<size_t>::max();
    in.max_attempts = 100000000;
    SECTION("Deadline already passed") {
        in.deadline = std::chrono::steady_clock::now();
        const auto result = compute_plate_layout(in);
        CHECK(result.search_limited);
        CHECK(result.placements.empty());
        CHECK_FALSE(result.canceled);
    }
    SECTION("Tiny time limit") {
        in.time_limit = 1e-6;
        const auto result = compute_plate_layout(in);
        CHECK(result.search_limited);
        CHECK_FALSE(result.canceled);
        verify(in, result);
    }
}
TEST_CASE("Planning past its deadline keeps plates and reports unplaced pieces", "[PlateLayout]") {
    auto plate = fixture();
    plate.bed = rectangle(0, 0, 20, 12);
    plate.shapes = {{rectangle(0, 0, 8, 8), 0}};
    plate.fill = false;
    plate.gap = 1.;
    REQUIRE(consolidate_plate_layouts({{plate, 0, true}, {plate, 0, true}}).plates[1].removed);
    plate.deadline = std::chrono::steady_clock::now();
    const auto consolidated = consolidate_plate_layouts({{plate, 0, true}, {plate, 0, true}});
    CHECK(consolidated.search_limited);
    CHECK_FALSE(consolidated.canceled);
    CHECK_FALSE(consolidated.plates[1].removed);
    CHECK_FALSE(consolidated.plates[0].changed);
    const auto repacked = repack_plate_layouts({{plate, 0, true}});
    CHECK(repacked.search_limited);
    CHECK(repacked.unplaced.size() == 1);
}
TEST_CASE("Fill uses the same checks beyond 100 copies", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0,0,40,40);
    in.shapes = {{rectangle(0,0,2,2),0}};
    in.gap = 1.;
    const auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() > 100);
    verify(in,result);
}
TEST_CASE("Fill repeats complete selected sets and cancellation produces no result", "[PlateLayout]") {
    auto in = fixture();
    in.shapes.push_back({rectangle(0,0,3,4),0});
    auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() % 2 == 0);
    for (size_t i = 0; i < result.placements.size(); ++i) REQUIRE(result.placements[i].source == i % 2);
    verify(in,result);
    result = compute_plate_layout(in, [] { return true; });
    REQUIRE(result.canceled);
    REQUIRE(result.placements.empty());
    in.max_attempts = 10;
    REQUIRE(compute_plate_layout(in).search_limited);
}
TEST_CASE("Arrange never adds copies and clearances override requested gaps", "[PlateLayout]") {
    auto in = fixture();
    in.fill = false;
    in.shapes.push_back(in.shapes.front());
    in.shapes[0].clearance = in.shapes[1].clearance = 4.;
    in.gap = 0.2;
    const auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() == 2);
    auto a = offset_ex(layout_footprint(in.shapes[0],result.placements[0]),scaled(4.));
    auto b = offset_ex(layout_footprint(in.shapes[1],result.placements[1]),scaled(4.));
    REQUIRE(intersection_ex(a,b).empty());
}

TEST_CASE("Reversing tapered pieces nests them without flipping their print face", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0,0,17,13);
    in.shapes = {{ {ExPolygon(Polygon({Point(0,0),Point(scaled(14.),0),Point(0,scaled(10.))}))},0}};
    in.gap = 1.;
    const auto straight = compute_plate_layout(in);
    in.allow_rotation = true;
    const auto rotated = compute_plate_layout(in);
    REQUIRE(rotated.placements.size() > straight.placements.size());
    verify(in, rotated);
}

TEST_CASE("Optimization can use the inside edge of an L-shaped part", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0, 0, 20.6, 20.6);
    const ExPolygons l_shape {ExPolygon(Polygon({Point(0, 0), Point(scaled(15.), 0),
        Point(scaled(15.), scaled(5.)), Point(scaled(5.), scaled(5.)),
        Point(scaled(5.), scaled(15.)), Point(0, scaled(15.))}))};
    in.shapes = {{l_shape, 0}, {l_shape, 0}};
    in.fill = false;
    in.optimize = true;
    in.allow_rotation = true;
    in.gap = 0.2;
    in.max_attempts = 5000;
    const auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() == 2);
    verify(in, result);
}

TEST_CASE("Optimizing compares complete layouts by occupied area", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0, 0, 20, 20);
    in.shapes = {{rectangle(0, 0, 8, 4), 0},
                 {rectangle(0, 0, 8, 4), 0},
                 {rectangle(0, 0, 8, 4), 0}};
    in.fill = false;
    in.allow_rotation = true;
    in.gap = 1.;
    const auto baseline = compute_plate_layout(in);
    in.optimize = true;
    const auto optimized = compute_plate_layout(in);
    REQUIRE(baseline.placements.size() == 3);
    REQUIRE(optimized.placements.size() == 3);
    verify(in, optimized);
    const auto area = [&](const PlateLayoutResult& result) {
        BoundingBox bounds;
        for (const auto& placement : result.placements)
            bounds.merge(get_extents(layout_footprint(in.shapes[placement.source], placement)));
        const auto size = bounds.size();
        return double(size.x()) * double(size.y());
    };
    CHECK(area(optimized) < area(baseline));
}

TEST_CASE("A feasibility search can stop at the first complete safe layout", "[PlateLayout]") {
    auto in = fixture();
    in.bed = rectangle(0, 0, 20, 20);
    in.shapes = {{rectangle(0, 0, 8, 4), 0}, {rectangle(0, 0, 8, 4), 0}};
    in.fill = false;
    in.optimize = true;
    in.allow_rotation = true;
    in.stop_on_fit = true;
    const auto result = compute_plate_layout(in);
    REQUIRE(result.placements.size() == in.shapes.size());
    verify(in, result);
}

TEST_CASE("Consolidating moves a donor's objects onto fewer compatible plates", "[PlateLayout]") {
    auto first = fixture();
    first.bed = rectangle(0, 0, 20, 12);
    first.shapes = {{rectangle(0, 0, 8, 8), 0}};
    first.fill = false;
    first.gap = 1.;
    auto second = first;
    second.shapes.push_back(second.shapes.front());
    const auto result = consolidate_plate_layouts({{first, 0, true}, {first, 0, true}, {second, 0, true}});
    REQUIRE_FALSE(result.canceled);
    REQUIRE(result.plates[2].removed);
    REQUIRE(result.plates[0].placement.placements.size() == 2);
    REQUIRE(result.plates[1].placement.placements.size() == 2);
    CHECK(result.plates[0].sources.back().plate == 2);
    CHECK(result.plates[1].sources.back().plate == 2);
    verify(result.plates[0].layout, result.plates[0].placement);
    verify(result.plates[1].layout, result.plates[1].placement);
}

TEST_CASE("Balanced consolidation uses both compatible printer beds", "[PlateLayout]") {
    auto empty = fixture();
    empty.bed = rectangle(0, 0, 20, 20);
    empty.shapes.clear();
    empty.fill = false;
    empty.gap = 1.;
    auto donor = empty;
    donor.shapes = {{rectangle(0, 0, 6, 6), 0}, {rectangle(0, 0, 6, 6), 0}};
    const auto result = consolidate_plate_layouts(
        {{empty, 0, false}, {empty, 0, false}, {donor, 0, true}}, {}, {}, true);
    REQUIRE(result.plates[2].removed);
    REQUIRE(result.plates[0].sources.size() == 1);
    REQUIRE(result.plates[1].sources.size() == 1);
    verify(result.plates[0].layout, result.plates[0].placement);
    verify(result.plates[1].layout, result.plates[1].placement);
}

TEST_CASE("Printer stages give each eligible printer one job per wave", "[PlateLayout]") {
    const std::vector<PrinterStageJob> jobs {
        {{0, 1}, 0, 60.},
        {{0, 1}, 1, 20.},
        {{0, 1}, std::numeric_limits<size_t>::max(), 30.},
        {{0, 1}, std::numeric_limits<size_t>::max(), 40.},
        {{0, 1}, std::numeric_limits<size_t>::max(), 10.}
    };
    const auto stages = assign_printer_stages(jobs, 2);
    REQUIRE(stages.size() == jobs.size());
    CHECK(stages[0].printer == 0);
    CHECK(stages[0].stage == 1);
    CHECK(stages[1].printer == 1);
    CHECK(stages[1].stage == 1);
    CHECK(stages[2].printer == 1);
    CHECK(stages[2].stage == 2);
    CHECK(stages[3].printer == 0);
    CHECK(stages[3].stage == 2);
    CHECK(stages[4].printer == 1);
    CHECK(stages[4].stage == 3);
    CHECK(assign_printer_stages({{{1}, std::numeric_limits<size_t>::max(), 1.}}, 1).empty());
}

TEST_CASE("A job fixed to an ineligible printer has no printer stage", "[PlateLayout]") {
    // The fixed printer must also be eligible; the job is never moved elsewhere.
    CHECK(assign_printer_stages({{{0, 1}, 2, 1.}}, 3).empty());
    CHECK(assign_printer_stages({{{0}, 1, 1.}}, 2).empty());
    CHECK(assign_printer_stages({{{0, 1}, 5, 1.}}, 2).empty());
    const auto fixed = assign_printer_stages({{{0, 1}, 1, 1.}}, 2);
    REQUIRE(fixed.size() == 1);
    CHECK(fixed[0].printer == 1);
}

TEST_CASE("Twelve compatible jobs use two stages across six printers", "[PlateLayout]") {
    std::vector<PrinterStageJob> jobs(12);
    for (auto& job : jobs) {
        job.eligible_printers = {0, 1, 2, 3, 4, 5};
        job.load = 100.;
    }
    const auto stages = assign_printer_stages(jobs, 6);
    REQUIRE(stages.size() == 12);
    std::array<int, 6> counts {};
    for (const auto& assignment : stages) {
        REQUIRE(assignment.printer < counts.size());
        ++counts[assignment.printer];
        CHECK(assignment.stage <= 2);
    }
    for (int count : counts) CHECK(count == 2);
}

TEST_CASE("Parts from twelve plates can fill two waves of six printer beds", "[PlateLayout]") {
    auto empty = fixture();
    empty.bed = rectangle(0, 0, 12, 12);
    empty.shapes.clear();
    empty.fill = false;
    empty.optimize = true;
    empty.stop_on_fit = true;
    empty.gap = 1.;
    empty.max_attempts = 400;
    auto donor = empty;
    donor.shapes = {{rectangle(0, 0, 9, 9), 0}};
    std::vector<PlateConsolidationPlate> beds;
    for (size_t i = 0; i < 12; ++i) beds.push_back({empty, 0, false});
    for (size_t i = 0; i < 12; ++i) beds.push_back({donor, 0, true});
    const auto packed = consolidate_plate_layouts(beds, {}, {}, true);
    REQUIRE_FALSE(packed.canceled);
    std::vector<PrinterStageJob> jobs;
    for (size_t i = 0; i < 12; ++i) {
        REQUIRE(packed.plates[i].sources.size() == 1);
        verify(packed.plates[i].layout, packed.plates[i].placement);
        jobs.push_back({{i % 6}, std::numeric_limits<size_t>::max(), 81.});
    }
    for (size_t i = 12; i < 24; ++i) REQUIRE(packed.plates[i].removed);
    const auto stages = assign_printer_stages(jobs, 6);
    REQUIRE(stages.size() == 12);
    for (const auto& stage : stages) CHECK(stage.stage <= 2);
}

TEST_CASE("Consolidation leaves plates intact when a complete safe transfer is impossible", "[PlateLayout]") {
    auto plate = fixture();
    plate.bed = rectangle(0, 0, 12, 12);
    plate.shapes = {{rectangle(0, 0, 8, 8), 0}};
    plate.fill = false;
    plate.gap = 1.;
    auto incompatible = consolidate_plate_layouts({{plate, 0, true}, {plate, 1, true}});
    CHECK_FALSE(incompatible.plates[1].removed);
    CHECK_FALSE(incompatible.plates[0].changed);
    auto full = consolidate_plate_layouts({{plate, 0, true}, {plate, 0, true}});
    CHECK_FALSE(full.plates[1].removed);
    CHECK_FALSE(full.plates[0].changed);
    auto locked = consolidate_plate_layouts({{plate, 0, true}, {plate, 0, false}});
    CHECK_FALSE(locked.plates[1].removed);
    CHECK_FALSE(locked.plates[0].changed);
}

TEST_CASE("Consolidation reaches the open part of a plate for interlocking corners", "[PlateLayout]") {
    PlateLayoutInput plate;
    plate.bed = rectangle(0, 0, 45, 45);
    plate.shapes = {{{ExPolygon(Polygon({Point(0, 0), Point(scaled(35.), 0),
        Point(scaled(35.), scaled(6.)), Point(scaled(6.), scaled(6.)),
        Point(scaled(6.), scaled(35.)), Point(0, scaled(35.))}))}, 0.}};
    plate.gap = 1.;
    plate.allow_rotation = true;
    plate.optimize = true;
    plate.fill = false;
    plate.max_attempts = 25000;

    const auto result = consolidate_plate_layouts({{plate, 0, true}, {plate, 0, true}});
    REQUIRE_FALSE(result.canceled);
    REQUIRE(result.plates[1].removed);
    REQUIRE(result.plates[0].placement.placements.size() == 2);
    verify(result.plates[0].layout, result.plates[0].placement);
}

TEST_CASE("Repacking includes every loose piece and creates enough beds", "[PlateLayout]") {
    auto input = fixture();
    input.bed = rectangle(0, 0, 20, 20);
    input.fill = false;
    input.optimize = true;
    input.shapes = std::vector<PlateLayoutShape>(5, {rectangle(0, 0, 12, 12), 0.});
    const auto result = repack_plate_layouts({{input, 0, true}});
    REQUIRE(result.unplaced.empty());
    REQUIRE_FALSE(result.canceled);
    REQUIRE(result.plates.size() == 5);
    std::vector<int> seen(5, 0);
    for (const auto& plate : result.plates) {
        REQUIRE_FALSE(plate.removed);
        REQUIRE(plate.template_plate == 0);
        verify(plate.layout, plate.placement);
        REQUIRE(plate.sources.size() == 1);
        for (const auto& ref : plate.sources) {
            REQUIRE(ref.plate == 0);
            REQUIRE(ref.shape < seen.size());
            ++seen[ref.shape];
        }
    }
    CHECK(seen == std::vector<int>(5, 1));
    CHECK(input.shapes.size() == 5);
}

TEST_CASE("Repacking separates material groups without changing piece identity", "[PlateLayout]") {
    auto input = fixture();
    input.fill = false;
    input.shapes = std::vector<PlateLayoutShape>(4, input.shapes.front());
    const auto result = repack_plate_layouts({{input, 7, true, {11, 12, 11, 12}}});
    REQUIRE(result.unplaced.empty());
    REQUIRE(result.plates.size() == 2);
    for (const auto& plate : result.plates) {
        REQUIRE(plate.sources.size() == 2);
        CHECK(plate.sources[0].shape % 2 == plate.sources[1].shape % 2);
        verify(plate.layout, plate.placement);
    }
}

TEST_CASE("Repacking refuses material groups that do not cover every piece", "[PlateLayout]") {
    auto input = fixture();
    input.fill = false;
    input.shapes = std::vector<PlateLayoutShape>(3, input.shapes.front());
    for (const std::vector<size_t>& groups : {std::vector<size_t>{1}, std::vector<size_t>{1, 2, 1, 2}})
        CHECK_THROWS_AS(repack_plate_layouts({{input, 0, true, groups}}), std::invalid_argument);
    CHECK(repack_plate_layouts({{input, 0, true, {}}}).unplaced.empty());
}

TEST_CASE("Repacking keeps printer setups separate and fixed jobs intact", "[PlateLayout]") {
    auto input = fixture();
    input.fill = false;
    auto fixed = input;
    fixed.fixed = {{rectangle(20, 10, 5, 5), 0.}};
    const auto result = repack_plate_layouts({{input, 0, true}, {input, 1, true}, {fixed, 0, false}});
    REQUIRE(result.unplaced.empty());
    REQUIRE(result.plates.size() == 3);
    CHECK(result.plates[0].sources.front().plate == 0);
    CHECK(result.plates[1].sources.front().plate == 1);
    CHECK(result.plates[2].sources.front().plate == 2);
    CHECK_FALSE(result.plates[2].changed);
    CHECK_FALSE(result.plates[2].removed);
    CHECK(result.plates[2].layout.fixed.size() == 1);
}

TEST_CASE("Repacking can optimize a plate without eliminating another plate", "[PlateLayout]") {
    auto input = fixture();
    input.fill = false;
    input.shapes.push_back(input.shapes.front());
    const auto result = repack_plate_layouts({{input, 0, true}});
    REQUIRE(result.unplaced.empty());
    REQUIRE(result.plates.size() == 1);
    CHECK(result.plates.front().changed);
    CHECK(result.plates.front().sources.size() == 2);
    verify(result.plates.front().layout, result.plates.front().placement);
}

TEST_CASE("Repacking identifies oversized pieces and cancellation without mutating input", "[PlateLayout]") {
    auto input = fixture();
    input.fill = false;
    input.shapes.push_back({rectangle(0, 0, 100, 100), 0.});
    const auto result = repack_plate_layouts({{input, 0, true}});
    REQUIRE(result.unplaced.size() == 1);
    CHECK(result.unplaced.front().plate == 0);
    CHECK(result.unplaced.front().shape == 1);
    CHECK(input.shapes.size() == 2);
    const auto stopped = repack_plate_layouts({{input, 0, true}}, [] { return true; });
    CHECK(stopped.canceled);
    CHECK(input.shapes.size() == 2);
}
