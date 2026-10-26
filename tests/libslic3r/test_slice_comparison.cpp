#include <catch2/catch_test_macros.hpp>
#include "libslic3r/SliceComparison.hpp"
#include <utility>

using namespace Slic3r;
namespace {
ComparedPath path(float x = 0) { return {x, 0, x + 10, 0, 50, .4f, .2f, 1, 0}; }
SliceSnapshot snapshot(unsigned revision, const char* label)
{
    SliceSnapshot s;
    s.revision = revision; s.label = label;
    s.settings_available = true;
    s.settings["speed"] = {"Speed", std::to_string(revision)};
    return s;
}
}
TEST_CASE("A previous slice remains immutable and belongs to its stable plate", "[SliceComparison]")
{
    SliceComparisonHistory history;
    history.record(10, snapshot(1, "First"));
    history.record(20, snapshot(2, "Other plate"));
    history.record(10, snapshot(3, "Second"));
    REQUIRE(history.previous(10));
    CHECK(history.previous(10)->revision == 1);
    CHECK(history.current(10)->revision == 3);
    CHECK(history.current(20)->revision == 2);
    CHECK_FALSE(history.previous(20));
    history.record(10, snapshot(3, "Repeated completion"));
    CHECK(history.previous(10)->revision == 1);
    const auto retained = history.previous(10);
    history.clear();
    CHECK_FALSE(history.current(10));
    CHECK(retained->revision == 1);
}
TEST_CASE("Loaded slice settings can be enriched without creating a previous result", "[SliceComparison]")
{
    SliceComparisonHistory history;
    auto loaded = snapshot(1, "Loaded"); loaded.settings_available = false;
    history.record(10, std::move(loaded));
    history.record(10, snapshot(1, "Completed"));
    CHECK(history.current(10)->settings_available);
    CHECK_FALSE(history.previous(10));
}
TEST_CASE("Comparison retains repeated toolpaths and recognizes reversed lines", "[SliceComparison]")
{
    ComparedLayer a, b;
    a.paths = {path(), path()};
    auto reversed = path(); std::swap(reversed.x0, reversed.x1);
    b.paths = {reversed};
    const auto diff = compare_slice_paths(&a, &b);
    CHECK(diff.shared.size() == 1);
    CHECK(diff.before_only.size() == 1);
    CHECK(diff.after_only.empty());
}
TEST_CASE("Changes to speed material role or bead dimensions are visible on shared geometry", "[SliceComparison]")
{
    const ComparedLayer a{.2, 1, {path()}};
    for (int change = 0; change != 6; ++change) {
        auto b = a;
        if (change == 0) b.paths[0].speed += 10;
        if (change == 1) b.paths[0].material_id = 1;
        if (change == 2) b.paths[0].role = 2;
        if (change == 3) b.paths[0].width += .1f;
        if (change == 4) b.paths[0].height += .1f;
        if (change == 5) b.paths[0].flow += .1f;
        const auto diff = compare_slice_paths(&a, &b);
        CHECK(diff.adjusted.size() == 1);
        CHECK(diff.shared.empty());
    }
}
TEST_CASE("Unmatched heights stay separate and moved geometry is not aligned automatically", "[SliceComparison]")
{
    auto a = snapshot(1, "A"), b = snapshot(2, "B");
    a.layers[200] = {.2, 1, {path()}};
    b.layers[300] = {.3, 1, {path(20)}};
    CHECK(compared_layer_heights(a, b) == std::vector<int64_t>{200, 300});
    const auto diff = compare_slice_paths(&a.layers.at(200), &b.layers.at(300));
    CHECK(diff.shared.empty());
    CHECK(diff.before_only.size() == 1);
    CHECK(diff.after_only.size() == 1);
    CHECK(compare_slice_paths(nullptr, &b.layers.at(300)).after_only.size() == 1);
}
TEST_CASE("Unavailable settings are not reported as removed and changed values retain their labels", "[SliceComparison]")
{
    auto a = snapshot(1, "A"), b = snapshot(2, "B");
    const auto changes = compare_slice_settings(a, b);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].label == "Speed");
    CHECK(changes[0].before == "1");
    CHECK(changes[0].after == "2");
    b.settings_available = false;
    CHECK(compare_slice_settings(a, b).empty());
}
TEST_CASE("Session history evicts older plates within its memory budget", "[SliceComparison]")
{
    const auto s = snapshot(1, "A");
    SliceComparisonHistory history(s.memory_bytes() * 2 + 64);
    history.record(10, s); history.record(20, s); history.record(30, s);
    CHECK_FALSE(history.current(10));
    CHECK(history.current(20));
    CHECK(history.current(30));
    SliceComparisonHistory small(1024);
    auto large = snapshot(2, "Large");
    large.layers[200].paths.resize(1000, path());
    small.record(1, std::move(large));
    CHECK_FALSE(small.current(1)->paths_available);
    CHECK(small.current(1)->layers.at(200).paths.empty());
}

TEST_CASE("An adjusted duplicate does not consume a later exact toolpath match", "[SliceComparison]")
{
    ComparedLayer a, b;
    auto slow = path(), fast = path(), changed = path();
    slow.speed = 10; fast.speed = 20; changed.speed = 30;
    a.paths = {slow, fast}; b.paths = {changed, slow};
    const auto diff = compare_slice_paths(&a, &b);
    CHECK(diff.shared.size() == 1);
    CHECK(diff.adjusted.size() == 1);
    CHECK(diff.before_only.empty());
    CHECK(diff.after_only.empty());
}
