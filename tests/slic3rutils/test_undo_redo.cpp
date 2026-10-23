#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "slic3r/Utils/UndoRedo.hpp"

using namespace Slic3r::UndoRedo;

namespace {
// Snapshot::is_topmost() recognizes the uncaptured current state by this name.
const std::string topmost_name = "@@@ Topmost @@@";

Snapshot snapshot(const std::string& name, size_t timestamp, size_t model_id = 1)
{
    SnapshotData data;
    data.snapshot_type = SnapshotType::Action;
    return Snapshot(name, timestamp, model_id, data);
}

// The stack directly after a drag took its snapshot at time 2.
std::vector<Snapshot> after_drag_snapshot()
{
    return {snapshot("New Project", 0), snapshot("Rename plate", 1), snapshot("Move plate", 2), snapshot(topmost_name, 3, 0)};
}
} // namespace

TEST_CASE("A cancelled drag removes its snapshot as if it had never been taken", "[UndoRedo]")
{
    auto snapshots = after_drag_snapshot();
    size_t active = 3, current = 3;
    // A project saved before the drag stays unmodified once the drag is undone in place.
    size_t saved = GENERATE(size_t(1), size_t(2));
    const size_t saved_before = saved;
    REQUIRE(discard_topmost_snapshot(snapshots, active, current, saved, 2));
    REQUIRE(snapshots.size() == 3);
    CHECK(snapshots[1].name == "Rename plate");
    CHECK(snapshots.back().is_topmost());
    CHECK_FALSE(snapshots.back().is_topmost_captured());
    CHECK(snapshots.back().timestamp == 2);
    CHECK(active == 2);
    CHECK(current == 2);
    CHECK(saved == saved_before);
}

TEST_CASE("A save after the drag snapshot no longer matches the restored state", "[UndoRedo]")
{
    auto snapshots = after_drag_snapshot();
    size_t active = 3, current = 3, saved = 3;
    REQUIRE(discard_topmost_snapshot(snapshots, active, current, saved, 2));
    CHECK(saved == size_t(-1));
}

TEST_CASE("A drag snapshot followed by other history is kept", "[UndoRedo]")
{
    size_t active = 3, current = 3, saved = 1, timestamp = 2;
    auto snapshots = after_drag_snapshot();

    SECTION("another snapshot was taken") {
        snapshots.back() = snapshot("Arrange", 3);
        snapshots.push_back(snapshot(topmost_name, 4, 0));
        active = current = 4;
    }
    SECTION("the drag snapshot was undone") {
        snapshots.back().model_id = 7;
        active = 2;
    }
    SECTION("undo and redo captured the current state") {
        snapshots.back().model_id = 7;
    }
    SECTION("the timestamp belongs to an earlier snapshot") {
        timestamp = 1;
    }
    SECTION("only the initial snapshot precedes the current state") {
        snapshots = {snapshot("New Project", 0), snapshot(topmost_name, 1, 0)};
        active = current = 1;
        timestamp = 0;
    }

    const auto before = snapshots;
    const size_t active_before = active, current_before = current;
    CHECK_FALSE(discard_topmost_snapshot(snapshots, active, current, saved, timestamp));
    REQUIRE(snapshots.size() == before.size());
    for (size_t i = 0; i < snapshots.size(); ++i) {
        CHECK(snapshots[i].name == before[i].name);
        CHECK(snapshots[i].timestamp == before[i].timestamp);
    }
    CHECK(active == active_before);
    CHECK(current == current_before);
    CHECK(saved == 1);
}
