#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/PlateSliceBatch.hpp"
#include "libslic3r/ObjectID.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Slic3r;

TEST_CASE("Stage slicing selects only its interleaved plates in workspace order", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(30), "stage-1", true, false}, {ObjectID(8), "stage-2", true, false},
        {ObjectID(55), "stage-1", true, false}, {ObjectID(3), "stage-2", true, false},
        {ObjectID(11), "", true, false}
    };
    PlateSliceBatch batch;
    batch.begin(candidates, SliceScope::Stage, ObjectID(30), "stage-2");
    REQUIRE(batch.total() == 2);
    REQUIRE(batch.completed() == 0);
    REQUIRE(batch.current() == ObjectID(8));
    REQUIRE(batch.complete(batch.generation(), ObjectID(8), true));
    REQUIRE(batch.completed() == 1);
    REQUIRE(batch.current() == ObjectID(3));
    REQUIRE(batch.complete(batch.generation(), ObjectID(3), true));
    REQUIRE(batch.state() == PlateSliceBatch::State::Completed);
    REQUIRE(batch.completed() == 2);
    REQUIRE(batch.current().invalid());
}

TEST_CASE("Plate and project slicing resolve stable identities instead of list positions", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(91), "stage", true, false}, {ObjectID(4), "", true, false},
        {ObjectID(27), "stage", true, false}
    };
    const bool project = GENERATE(false, true);
    PlateSliceBatch batch;
    batch.begin(candidates, project ? SliceScope::Project : SliceScope::Plate, ObjectID(27), "stage");
    const std::vector<ObjectID> expected = project ? std::vector<ObjectID>{ObjectID(91), ObjectID(4), ObjectID(27)} :
                                                   std::vector<ObjectID>{ObjectID(27)};
    REQUIRE(batch.total() == expected.size());
    for (const auto id : expected) {
        REQUIRE(batch.current() == id);
        REQUIRE(batch.complete(batch.generation(), id, true));
    }
    REQUIRE(batch.state() == PlateSliceBatch::State::Completed);
}

TEST_CASE("Batch progress counts only printable jobs that need slicing unless forced", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(1), "stage", true, true}, {ObjectID(2), "stage", false, true},
        {ObjectID(3), "stage", false, false}, {ObjectID(4), "stage", true, false},
        {ObjectID(5), "stage", true, true}, {ObjectID{}, "stage", true, false}
    };
    const bool force = GENERATE(false, true);
    PlateSliceBatch batch;
    batch.begin(candidates, SliceScope::Project, ObjectID(4), "stage", force);
    const std::vector<ObjectID> expected = force ? std::vector<ObjectID>{ObjectID(1), ObjectID(4), ObjectID(5)} :
                                                 std::vector<ObjectID>{ObjectID(4)};
    REQUIRE(batch.total() == expected.size());
    size_t done = 0;
    for (const auto id : expected) {
        REQUIRE(batch.completed() == done);
        REQUIRE(batch.current() == id);
        REQUIRE(batch.complete(batch.generation(), id, true));
        ++done;
    }
    REQUIRE(batch.completed() == expected.size());
}

TEST_CASE("Repeated plate identities use their first candidate without duplicate jobs", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(7), "stage", false, false}, {ObjectID(7), "stage", true, false},
        {ObjectID(9), "stage", true, false}, {ObjectID(9), "stage", true, true},
        {ObjectID(6), "stage", true, true}, {ObjectID(6), "stage", true, false},
        {ObjectID(2), "stage", true, false}, {ObjectID(2), "stage", true, false}
    };
    PlateSliceBatch batch;
    batch.begin(candidates, SliceScope::Project, ObjectID(9), "stage");
    REQUIRE(batch.total() == 2);
    REQUIRE(batch.current() == ObjectID(9));
    REQUIRE(batch.complete(batch.generation(), ObjectID(9), true));
    REQUIRE(batch.current() == ObjectID(2));
    REQUIRE(batch.complete(batch.generation(), ObjectID(2), true));
    REQUIRE(batch.current().invalid());
}

TEST_CASE("An absent plate or stage yields no jobs rather than expanding the scope", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(1), "", true, false}, {ObjectID(2), "stage", true, false}
    };
    PlateSliceBatch batch;
    REQUIRE(batch.state() == PlateSliceBatch::State::Idle);
    batch.begin(candidates, SliceScope::Stage, ObjectID(1), "");
    REQUIRE(batch.total() == 0);
    REQUIRE(batch.state() == PlateSliceBatch::State::Completed);
    batch.begin(candidates, SliceScope::Stage, ObjectID(1), "missing");
    REQUIRE(batch.total() == 0);
    batch.begin(candidates, SliceScope::Plate, ObjectID{}, "stage");
    REQUIRE(batch.total() == 0);
    batch.begin(candidates, SliceScope::Plate, ObjectID(99), "stage");
    REQUIRE(batch.total() == 0);
    batch.begin({}, SliceScope::Project, ObjectID(1), "stage");
    REQUIRE(batch.current().invalid());
    REQUIRE_FALSE(batch.complete(batch.generation(), ObjectID{}, true));
}

TEST_CASE("Only the current plate and batch generation can advance slicing", "[PlateSliceBatch]")
{
    PlateSliceBatch batch;
    batch.begin({{ObjectID(11), "", true, false}, {ObjectID(3), "", true, false}},
                SliceScope::Project, ObjectID(11), "");
    const uint64_t generation = batch.generation();
    REQUIRE_FALSE(batch.complete(generation, ObjectID(3), false));
    REQUIRE_FALSE(batch.complete(generation - 1, ObjectID(11), true));
    REQUIRE(batch.current() == ObjectID(11));
    REQUIRE(batch.completed() == 0);
    REQUIRE(batch.complete(generation, ObjectID(11), true));
    REQUIRE_FALSE(batch.complete(generation, ObjectID(11), true));
    REQUIRE(batch.current() == ObjectID(3));
    REQUIRE(batch.completed() == 1);
    REQUIRE(batch.complete(generation, ObjectID(3), true));
    REQUIRE_FALSE(batch.complete(generation, ObjectID(3), true));
}

TEST_CASE("A plate failure stops the batch before subsequent jobs start", "[PlateSliceBatch]")
{
    PlateSliceBatch batch;
    batch.begin({{ObjectID(1), "", true, false}, {ObjectID(2), "", true, false}, {ObjectID(3), "", true, false}},
                SliceScope::Project, ObjectID(1), "");
    const uint64_t generation = batch.generation();
    REQUIRE(batch.complete(generation, ObjectID(1), true));
    REQUIRE(batch.complete(generation, ObjectID(2), false));
    REQUIRE(batch.state() == PlateSliceBatch::State::Failed);
    REQUIRE(batch.completed() == 1);
    REQUIRE(batch.total() == 3);
    REQUIRE(batch.current().invalid());
    REQUIRE_FALSE(batch.complete(generation, ObjectID(2), true));
    REQUIRE_FALSE(batch.complete(generation, ObjectID(3), true));
}

TEST_CASE("Cancellation clears pending jobs and ignores their late callbacks", "[PlateSliceBatch]")
{
    const std::vector<PlateSliceCandidate> candidates {
        {ObjectID(1), "", true, false}, {ObjectID(2), "", true, false}, {ObjectID(3), "", true, false}
    };
    PlateSliceBatch batch;
    batch.begin(candidates, SliceScope::Project, ObjectID(1), "");
    const uint64_t generation = batch.generation();
    REQUIRE(batch.complete(generation, ObjectID(1), true));
    batch.cancel();
    REQUIRE(batch.state() == PlateSliceBatch::State::Cancelled);
    REQUIRE(batch.current().invalid());
    REQUIRE(batch.completed() == 1);
    REQUIRE(batch.total() == 3);
    REQUIRE_FALSE(batch.complete(generation, ObjectID(2), true));
    batch.begin(candidates, SliceScope::Project, ObjectID(1), "");
    REQUIRE(batch.completed() == 0);
    REQUIRE(batch.current() == ObjectID(1));
    REQUIRE_FALSE(batch.complete(generation, ObjectID(1), true));
    REQUIRE_FALSE(batch.complete(generation, ObjectID(1), false));
    REQUIRE(batch.complete(batch.generation(), ObjectID(1), true));
    REQUIRE(batch.current() == ObjectID(2));
}

TEST_CASE("Starting a replacement batch rejects the previous run even for the same plate", "[PlateSliceBatch]")
{
    PlateSliceBatch batch;
    const std::vector<PlateSliceCandidate> candidates {{ObjectID(8), "stage", true, false}};
    batch.begin(candidates, SliceScope::Project, ObjectID(8), "");
    const uint64_t generation = batch.generation();
    batch.begin(candidates, SliceScope::Plate, ObjectID(8), "");
    REQUIRE_FALSE(batch.complete(generation, ObjectID(8), true));
    REQUIRE(batch.running());
    REQUIRE(batch.current() == ObjectID(8));
    REQUIRE(batch.complete(batch.generation(), ObjectID(8), true));
    REQUIRE(batch.state() == PlateSliceBatch::State::Completed);
}
