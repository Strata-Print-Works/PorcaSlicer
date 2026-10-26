#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ProjectBackupState.hpp"

TEST_CASE("Saving or discarding one project retains another project's recovery edits", "[ProjectBackupState]")
{
    Slic3r::ProjectBackupState state;
    state.changed(11);
    state.changed(22);
    state.clear(11, false);
    REQUIRE_FALSE(state.dirty(11, false));
    REQUIRE(state.dirty(11, true));
    REQUIRE(state.dirty(22, false));
    REQUIRE(state.dirty(22, true));
    state.forget(11);
    REQUIRE_FALSE(state.dirty(11, true));
    REQUIRE(state.dirty(22, true));
}

TEST_CASE("A recovery checkpoint preserves unsaved changes and a new edit needs another checkpoint", "[ProjectBackupState]")
{
    Slic3r::ProjectBackupState state;
    state.changed(11);
    state.clear(11, true);
    REQUIRE(state.dirty(11, false));
    REQUIRE_FALSE(state.dirty(11, true));
    state.changed(11);
    REQUIRE(state.dirty(11, true));
    REQUIRE_FALSE(state.dirty(99, true));
}
