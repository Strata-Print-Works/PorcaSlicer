#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ProjectSliceQueue.hpp"

TEST_CASE("Projects slice in request order and repeated clicks retain the original request", "[ProjectSliceQueue]")
{
    Slic3r::ProjectSliceQueue queue;
    const auto a = queue.enqueue(11);
    const auto b = queue.enqueue(22);
    REQUIRE(queue.enqueue(11).generation == a.generation);
    REQUIRE(queue.current().project == 11);
    REQUIRE_FALSE(queue.complete(b));
    REQUIRE(queue.complete(a));
    REQUIRE(queue.current().project == 22);
    REQUIRE(queue.complete(b));
    REQUIRE(queue.current().project == 0);
}

TEST_CASE("A completion from cancelled work cannot consume its replacement or another project", "[ProjectSliceQueue]")
{
    Slic3r::ProjectSliceQueue queue;
    const auto old = queue.enqueue(11);
    const auto b = queue.enqueue(22);
    queue.cancel(11);
    const auto replacement = queue.enqueue(11);
    REQUIRE_FALSE(queue.complete(old));
    REQUIRE(queue.current().project == 22);
    REQUIRE(queue.complete(b));
    REQUIRE_FALSE(queue.complete(old));
    REQUIRE(queue.complete(replacement));
}
