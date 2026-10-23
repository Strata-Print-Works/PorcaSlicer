#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/PluginAgentOwner.hpp"

TEST_CASE("A singleton SDK handle cannot acquire a second owner or be destroyed by it", "[PluginAgentOwner]")
{
    int opaque = 0, destroys = 0;
    auto destroy = [&](void* handle) { CHECK(handle == &opaque); ++destroys; return 0; };
    Slic3r::PluginAgentOwner account, device;
    REQUIRE(account.acquire(&opaque, destroy));
    CHECK_FALSE(device.acquire(&opaque, destroy));
    device.reset();
    CHECK(destroys == 0);
    CHECK(account.get() == &opaque);
    account.reset();
    CHECK(destroys == 1);
    CHECK(device.acquire(&opaque, destroy));
    device.reset();
    CHECK(destroys == 2);
}
