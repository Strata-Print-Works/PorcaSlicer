#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ProductionLedger.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Production counts only accepted usable parts", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"first", "First", 2}, {"second", "Second", 1}};
    ledger.runs.push_back({"sent", "Plate 1", "Printer", "today", "porca_send", "sent",
                           {{"first", 1, 0}, {"second", 1, 0}}});
    REQUIRE(ledger.ready_total() == 0);
    ledger.runs.push_back({"manual", "Plate 1", "Printer", "today", "manual", "accepted",
                           {{"first", 1, 1}, {"second", 1, 0}}});
    REQUIRE(ledger.ready_total() == 1);
    REQUIRE(ledger.required_total() == 3);
    REQUIRE(ProductionLedger::deserialize(ledger.serialize()).ready_total() == 1);
    ledger.runs.push_back({"repeat", "Plate 1", "Printer", "tomorrow", "manual", "accepted",
                           {{"first", 1, 1}, {"second", 1, 1}}});
    REQUIRE(ledger.ready_total() == 3);
}

TEST_CASE("Production rejects unsupported or impossible saved results", "[ProductionLedger]")
{
    REQUIRE_THROWS(ProductionLedger::deserialize(R"({"version":2})"));
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"first", "First", 1}};
    ledger.runs.push_back({"run", "Plate", "", "today", "manual", "accepted", {{"first", 1, 2}}});
    REQUIRE_THROWS(ProductionLedger::deserialize(ledger.serialize()));
}

TEST_CASE("Correcting a print keeps the previous review", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"first", "First", 1}};
    ProductionRun run{"run", "Plate", "Printer", "today", "porca_send", "sent", {{"first", 1, 0}}};
    REQUIRE(ledger.ready_total() == 0);
    run.reviews.push_back({"later", {{"first", 1, 1}}});
    ledger.runs.push_back(run);
    REQUIRE(ledger.ready_total() == 1);
    ledger.runs.front().reviews.push_back({"correction", {{"first", 1, 0}}});
    REQUIRE(ledger.ready_total() == 0);
    const auto restored = ProductionLedger::deserialize(ledger.serialize());
    REQUIRE(restored.runs.front().parts.front().accepted == 0);
    REQUIRE(restored.runs.front().reviews.size() == 2);
    REQUIRE(restored.ready_total() == 0);
}

TEST_CASE("Undoing a manual plate completion preserves earlier and unrelated results", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"first", "First", 3}, {"second", "Second", 2}, {"other", "Other plate", 1}};
    ledger.runs.push_back({"earlier", "Plate A", "Printer", "before", "manual", "accepted",
                           {{"first", 1, 1, "old"}}});
    ledger.runs.push_back({"other-run", "Plate B", "Printer", "before", "manual", "accepted",
                           {{"other", 1, 1, "other"}}});
    const std::vector<ProductionRunPart> on_plate{{"first", 1, 0, "position-a"},
                                                   {"second", 1, 0, "position-b"}};
    REQUIRE(ledger.record_plate_done(on_plate, "Plate A", "Printer", "marked", "now") == "marked");
    REQUIRE(ledger.ready_total() == 4);
    ledger.runs.push_back({"later", "Plate B", "Printer", "later", "manual", "accepted",
                           {{"other", 1, 1, "other"}}});
    ledger.parts[0].required = 4; // A later target edit must survive undo.
    ledger = ProductionLedger::deserialize(ledger.serialize());
    REQUIRE(ledger.mark_done_undo_status("marked", {on_plate[1], on_plate[0]}) ==
            ProductionLedger::MarkDoneUndoStatus::Available);
    REQUIRE(ledger.undo_mark_done("marked", on_plate));
    REQUIRE(ledger.ready_total() == 2);
    REQUIRE(ledger.accepted("first") == 1);
    REQUIRE(ledger.accepted("second") == 0);
    REQUIRE(ledger.accepted("other") == 2);
    REQUIRE(ledger.parts[0].required == 4);
    REQUIRE(ledger.runs.size() == 3);
    REQUIRE_FALSE(ledger.undo_mark_done("marked", on_plate));
}

TEST_CASE("Undoing a plate completion restores its pending print", "[ProductionLedger]")
{
    for (const std::string state : {"sent", "printing"}) {
        DYNAMIC_SECTION(state) {
            ProductionLedger ledger;
            ledger.project_id = "project";
            ledger.parts = {{"part", "Part", 2}};
            const std::vector<ProductionRunPart> on_plate{{"part", 1, 0, "original-position"}};
            ledger.runs.push_back({"pending", "Plate A", "Printer", "before", "porca_send", state, on_plate});
            REQUIRE(ledger.record_plate_done(on_plate, "Plate A", "Printer", "unused", "now") == "pending");
            REQUIRE(ledger.runs.size() == 1);
            REQUIRE(ledger.ready_total() == 1);
            ledger.revision = 7;
            auto restored = ProductionLedger::deserialize(ledger.serialize());
            REQUIRE(restored.runs.front().reviews.back().mark_done_previous_state == state);
            REQUIRE(restored.undo_mark_done("pending", on_plate));
            REQUIRE(restored.runs.front().state == state);
            REQUIRE(restored.runs.front().reviews.empty());
            REQUIRE(restored.ready_total() == 0);
            REQUIRE_FALSE(restored.undo_mark_done("pending", on_plate));
            ++restored.revision;
            const auto newer = ProductionLedger::deserialize(restored.serialize());
            REQUIRE(newer.revision > ledger.revision);
            REQUIRE(newer.ready_total() == 0);
            REQUIRE(newer.runs.front().state == state);
        }
    }
}

TEST_CASE("Undoing a mark requires the same current parts and an unchanged result", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 1}};
    const std::vector<ProductionRunPart> on_plate{{"part", 1, 0, "original-position"}};
    ledger.record_plate_done(on_plate, "Plate", "Printer", "marked", "now");
    const std::vector<ProductionRunPart> changed{{"part", 1, 0, "moved-position"}};
    REQUIRE(ledger.mark_done_undo_status("marked", changed) ==
            ProductionLedger::MarkDoneUndoStatus::PlateChanged);
    REQUIRE_FALSE(ledger.undo_mark_done("marked", changed));
    REQUIRE(ledger.ready_total() == 1);
    ledger.runs.front().reviews.push_back({"correction", {{"part", 1, 0, "original-position"}}});
    ledger.runs.front().state = "failed";
    REQUIRE(ledger.mark_done_undo_status("marked", on_plate) ==
            ProductionLedger::MarkDoneUndoStatus::Unavailable);
    REQUIRE_FALSE(ledger.undo_mark_done("marked", on_plate));
    REQUIRE(ledger.ready_total() == 0);
}

TEST_CASE("An undone completion remains undone in newer saved records", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 1}};
    const std::vector<ProductionRunPart> on_plate{{"part", 1, 0, "position"}};
    ledger.record_plate_done(on_plate, "Plate", "Printer", "marked", "now");
    ledger.revision = 10;
    const auto before_undo = ledger.serialize();
    REQUIRE(ledger.undo_mark_done("marked", on_plate));
    ++ledger.revision; // write_ledger journals this revision before embedding it in the project.
    const auto after_undo = ProductionLedger::deserialize(ledger.serialize());
    REQUIRE(after_undo.revision > ProductionLedger::deserialize(before_undo).revision);
    REQUIRE(after_undo.runs.empty());
    REQUIRE(after_undo.ready_total() == 0);
    REQUIRE(ProductionLedger::deserialize(after_undo.serialize()).runs.empty());
}

TEST_CASE("Recording a completion rejects an empty or reused run identity", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"first", "First", 2}, {"other", "Other", 1}};
    const std::vector<ProductionRunPart> on_plate{{"first", 1, 0, "position"}};
    ledger.record_plate_done({{"other", 1, 0, "elsewhere"}}, "Other plate", "Printer", "existing", "before");
    const auto before = ledger.serialize();
    for (const std::string id : {"", "existing"}) {
        DYNAMIC_SECTION("Run identity: " << id) {
            REQUIRE(ledger.record_plate_done(on_plate, "Plate", "Printer", id, "now").empty());
            REQUIRE(ledger.serialize() == before);
            REQUIRE(ledger.accepted("first") == 0);
            REQUIRE(ledger.accepted("other") == 1);
        }
    }
}

TEST_CASE("Saved runs have distinct identities for correction and undo", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 3}};
    const std::vector<ProductionRunPart> on_plate{{"part", 1, 0, "position"}};
    ledger.record_plate_done(on_plate, "Plate", "Printer", "run", "before");
    auto duplicate = ledger.runs.front();
    duplicate.recorded_at = "later";
    ledger.runs.push_back(duplicate);
    REQUIRE_THROWS(ProductionLedger::deserialize(ledger.serialize()));
}

TEST_CASE("A saved print snapshot counts each instance once", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 3}};
    ledger.runs.push_back({"run", "Plate", "Printer", "today", "manual", "accepted",
                           {{"part", 1, 1, "position"}, {"part", 1, 1, "other-position"}}});
    REQUIRE_THROWS(ProductionLedger::deserialize(ledger.serialize()));
}

TEST_CASE("A saved review counts each instance once", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 3}};
    ledger.runs.push_back({"run", "Plate", "Printer", "today", "porca_send", "accepted",
                           {{"part", 1, 0, "position"}},
                           {{"later", {{"part", 1, 1, "position"}, {"part", 1, 1, "position"}}}}});
    REQUIRE_THROWS(ProductionLedger::deserialize(ledger.serialize()));
}

TEST_CASE("Production rejects a saved run with an unknown state", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 1}};
    for (const std::string state : {"sent", "printing", "accepted", "failed"}) {
        DYNAMIC_SECTION("Known state: " << state) {
            ledger.runs = {{"run", "Plate", "Printer", "today", "manual", state, {{"part", 1, 0}}}};
            REQUIRE_NOTHROW(ProductionLedger::deserialize(ledger.serialize()));
        }
    }
    ledger.runs = {{"run", "Plate", "Printer", "today", "manual", "completed", {{"part", 1, 1}}}};
    REQUIRE_THROWS(ProductionLedger::deserialize(ledger.serialize()));
}

TEST_CASE("Marking a changed plate done records a manual run and keeps the sent print pending", "[ProductionLedger]")
{
    ProductionLedger ledger;
    ledger.project_id = "project";
    ledger.parts = {{"part", "Part", 2}};
    ledger.runs.push_back({"pending", "Plate A", "Printer", "before", "porca_send", "sent",
                           {{"part", 1, 0, "sent-position"}}});
    const std::vector<ProductionRunPart> moved{{"part", 1, 0, "moved-position"}};
    REQUIRE(ledger.record_plate_done(moved, "Plate A", "Printer", "manual-run", "now") == "manual-run");
    REQUIRE(ledger.runs.size() == 2);
    CHECK(ledger.runs.front().state == "sent");
    CHECK(ledger.runs.front().reviews.empty());
    CHECK(ledger.runs.back().source == "manual");
    CHECK(ledger.runs.back().state == "accepted");
    CHECK(ledger.accepted("part") == 1);
}

TEST_CASE("A copy saved under another name keeps its own production journal", "[ProductionLedger]")
{
    // Save As keeps the project identity and part keys, so only the saved file
    // tells the original's unsaved progress from the copy's.
    const std::string original = ProductionLedger::journal_location("/projects/brackets.3mf");
    const std::string copy = ProductionLedger::journal_location("/projects/brackets copy.3mf");
    CHECK(original != copy);
    CHECK(original == ProductionLedger::journal_location("/projects/brackets.3mf"));
    REQUIRE(original.size() == 16);
    CHECK(std::all_of(original.begin(), original.end(),
                      [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }));
    CHECK(original.find("brackets") == std::string::npos);
}
