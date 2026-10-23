#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <cstddef>
#include <string>
#include <vector>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <sstream>

using namespace Slic3r;

TEST_CASE("Stage names identities and empty-stage anchors survive serialization", "[WorkspaceStages]")
{
    const std::vector<WorkspaceStage> stages {
        {"stage-1", "Fit check & café", Vec2d(-503.125, 67.625)},
        {"stage-2", "Final parts", Vec2d(750, -300)}
    };
    const auto restored = deserialize_workspace_stages(serialize_workspace_stages(stages));
    REQUIRE(restored.size() == stages.size());
    for (size_t i = 0; i < stages.size(); ++i) {
        REQUIRE(restored[i].id == stages[i].id);
        REQUIRE(restored[i].name == stages[i].name);
        REQUIRE_THAT((restored[i].position - stages[i].position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    }
    REQUIRE(deserialize_workspace_stages(serialize_workspace_stages({})).empty());
}

TEST_CASE("Malformed and future stage catalogs reject restoration", "[WorkspaceStages]")
{
    auto data = nlohmann::json::parse(serialize_workspace_stages({{"stage-1", "Fit check", Vec2d(0, 0)}}));
    const auto defect = GENERATE("version", "version-type", "position-size", "position-type", "position-range", "position-null",
                                "duplicate", "empty-id", "unsafe-id", "empty-name", "blank-name", "name-type", "list-type");
    const std::string kind(defect);
    if (kind == "version") data["version"] = 3;
    else if (kind == "version-type") data["version"] = "1";
    else if (kind == "position-size") data["stages"][0]["position"] = {1};
    else if (kind == "position-type") data["stages"][0]["position"] = {"0", 1};
    else if (kind == "position-range") data["stages"][0]["position"] = {100001, 0};
    else if (kind == "position-null") data["stages"][0]["position"] = {nullptr, 0};
    else if (kind == "duplicate") data["stages"].push_back(data["stages"][0]);
    else if (kind == "empty-id") data["stages"][0]["id"] = "";
    else if (kind == "unsafe-id") data["stages"][0]["id"] = "stage/1";
    else if (kind == "empty-name") data["stages"][0]["name"] = "";
    else if (kind == "blank-name") data["stages"][0]["name"] = " \t\n";
    else if (kind == "name-type") data["stages"][0]["name"] = 17;
    else data["stages"] = nlohmann::json::object();
    REQUIRE_THROWS(deserialize_workspace_stages(data.dump()));
}

TEST_CASE("Resized and automatic stage areas survive project metadata and undo snapshots", "[WorkspaceStages]")
{
    const auto manual = GENERATE(false, true);
    WorkspaceStage stage{"stage-1", "Fit check", Vec2d(-503.125, 67.625)};
    if (manual) stage.frame = BoundingBoxf(Vec2d(-60, -75), Vec2d(650, 520));
    const auto saved = serialize_workspace_stages({stage});
    REQUIRE(nlohmann::json::parse(saved)["version"] == (manual ? 2 : 1));
    const auto restored = deserialize_workspace_stages(saved).front();
    REQUIRE(restored.frame.defined == manual);
    REQUIRE_THAT((restored.frame.min - stage.frame.min).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE_THAT((restored.frame.max - stage.frame.max).norm(), Catch::Matchers::WithinAbs(0, 1e-9));

    std::stringstream stream;
    { cereal::BinaryOutputArchive archive(stream); archive(stage); }
    WorkspaceStage undone;
    { cereal::BinaryInputArchive archive(stream); archive(undone); }
    REQUIRE(undone.frame.defined == manual);
    REQUIRE_THAT((undone.frame.min - stage.frame.min).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE_THAT((undone.frame.max - stage.frame.max).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}

TEST_CASE("Malformed resized areas reject restoration instead of silently auto sizing", "[WorkspaceStages]")
{
    WorkspaceStage stage{"stage-1", "Fit check", Vec2d(100, -50)};
    stage.frame = BoundingBoxf(Vec2d(-50, -60), Vec2d(650, 500));
    auto data = nlohmann::json::parse(serialize_workspace_stages({stage}));
    const std::string kind = GENERATE("legacy-version", "frame-type", "frame-size", "point-size", "reversed", "too-small",
                                      "null", "text", "range", "translated-range");
    if (kind == "legacy-version") data["version"] = 1;
    else if (kind == "frame-type") data["stages"][0]["frame"] = "700x560";
    else if (kind == "frame-size") data["stages"][0]["frame"] = {{0, 0}};
    else if (kind == "point-size") data["stages"][0]["frame"][0] = {0};
    else if (kind == "reversed") data["stages"][0]["frame"] = {{650, 500}, {-50, -60}};
    else if (kind == "too-small") data["stages"][0]["frame"] = {{0, 0}, {199, 300}};
    else if (kind == "null") data["stages"][0]["frame"][0][0] = nullptr;
    else if (kind == "text") data["stages"][0]["frame"][0][0] = "0";
    else if (kind == "range") data["stages"][0]["frame"][1][0] = 100001;
    else data["stages"][0]["position"] = {99900, 0};
    REQUIRE_THROWS(deserialize_workspace_stages(data.dump()));
}

TEST_CASE("Stage area retains the resized space while growing around moved member beds", "[WorkspaceStages]")
{
    WorkspaceStage stage{"stage", "Fit check", Vec2d(-100, 100)};
    stage.frame = BoundingBoxf(Vec2d(-100, -200), Vec2d(1000, 650));
    std::vector<WorkspacePlateBounds> plates {
        {"stage", Vec2d(0, 0), BoundingBoxf(Vec2d(0, 0), Vec2d(180, 180)), false},
        {"stage", Vec2d(240, 0), BoundingBoxf(Vec2d(240, 0), Vec2d(590, 350)), true},
        {"other", Vec2d(2000, 0), BoundingBoxf(Vec2d(2000, 0), Vec2d(2300, 300)), false}
    };
    const auto frame = workspace_stage_bounds(stage, plates);
    REQUIRE(frame.contains(workspace_stage_minimum_bounds(stage, plates)));
    REQUIRE_THAT(frame.min.x(), Catch::Matchers::WithinAbs(-200, 1e-9));
    REQUIRE_THAT(frame.max.y(), Catch::Matchers::WithinAbs(750, 1e-9));
    plates[0].bounds.translate(Vec2d(-500, 0));
    const auto grown = workspace_stage_bounds(stage, plates);
    REQUIRE(grown.contains(frame));
    REQUIRE_THAT(grown.min.x(), Catch::Matchers::WithinAbs(-560, 1e-9));
    REQUIRE_FALSE(grown.contains(plates[2].bounds));
    REQUIRE_THAT(stage.frame.min.x(), Catch::Matchers::WithinAbs(-100, 1e-9));
    REQUIRE_THAT(plates[1].origin.x(), Catch::Matchers::WithinAbs(240, 1e-9));
}

TEST_CASE("Resizing retains generous space for the beds and their controls without moving them", "[WorkspaceStages]")
{
    const WorkspaceStage stage{"stage", "Fit check", Vec2d(0, 0)};
    const std::vector<WorkspacePlateBounds> plates {
        {"stage", Vec2d(0, 0), BoundingBoxf(Vec2d(0, 0), Vec2d(180, 180)), true},
        {"stage", Vec2d(240, 0), BoundingBoxf(Vec2d(240, 0), Vec2d(590, 350)), false}
    };
    const auto minimum = workspace_stage_minimum_bounds(stage, plates);
    REQUIRE_THAT(minimum.min.x(), Catch::Matchers::WithinAbs(-60, 1e-9));
    REQUIRE_THAT(minimum.min.y(), Catch::Matchers::WithinAbs(-60, 1e-9));
    REQUIRE_THAT(minimum.max.x(), Catch::Matchers::WithinAbs(670, 1e-9));
    REQUIRE_THAT(minimum.max.y(), Catch::Matchers::WithinAbs(450, 1e-9));
    REQUIRE(can_resize_workspace_stage(stage, plates, minimum));
    REQUIRE(can_resize_workspace_stage(stage, plates,
        BoundingBoxf(minimum.min - Vec2d(100, 100), minimum.max + Vec2d(100, 100))));
    REQUIRE_FALSE(can_resize_workspace_stage(stage, plates, BoundingBoxf(minimum.min + Vec2d(1, 0), minimum.max)));
    REQUIRE_THAT(plates[0].origin.norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}

TEST_CASE("Empty stages resize to smaller areas and keep their size when moved", "[WorkspaceStages]")
{
    WorkspaceStage stage{"stage", "Fit check", Vec2d(100, -200)};
    const BoundingBoxf requested(Vec2d(-250, -600), Vec2d(0, -300));
    REQUIRE(can_resize_workspace_stage(stage, {}, requested));
    REQUIRE_FALSE(can_resize_workspace_stage(stage, {}, BoundingBoxf(requested.min, requested.min + Vec2d(199, 300))));
    stage.frame = BoundingBoxf(requested.min - stage.position, requested.max - stage.position);
    REQUIRE_THAT(workspace_stage_bounds(stage, {}).size().x(), Catch::Matchers::WithinAbs(250, 1e-9));
    const Vec2d delta(50, -125);
    stage.position += delta;
    REQUIRE_THAT((workspace_stage_bounds(stage, {}).min - requested.min - delta).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}

TEST_CASE("Resize preflight rejects nonfinite undefined or excessive stage areas", "[WorkspaceStages]")
{
    const WorkspaceStage stage{"stage", "Fit check", Vec2d(0, 0)};
    REQUIRE_FALSE(can_resize_workspace_stage(stage, {}, {}));
    REQUIRE_FALSE(can_resize_workspace_stage(stage, {}, BoundingBoxf(Vec2d(0, 0), Vec2d(100001, 300))));
    REQUIRE_FALSE(can_resize_workspace_stage(stage, {}, BoundingBoxf(Vec2d(0, 0), Vec2d(300, std::numeric_limits<double>::infinity()))));
    REQUIRE_FALSE(can_resize_workspace_stage(stage, {}, BoundingBoxf(Vec2d(std::numeric_limits<double>::quiet_NaN(), 0), Vec2d(300, 300))));
}

TEST_CASE("Stage areas retain a twenty millimeter gap without blocking diagonal separation", "[WorkspaceStages]")
{
    const BoundingBoxf current(Vec2d(0, 0), Vec2d(200, 244));
    const std::vector<BoundingBoxf> others {BoundingBoxf(Vec2d(500, 0), Vec2d(700, 244))};
    const double right = GENERATE(479., 480., 481., 500., 550.);
    const BoundingBoxf proposed(Vec2d(right - 200., 0), Vec2d(right, 244));
    REQUIRE(can_change_workspace_stage_area(current, proposed, others) == (right <= 480.));
    REQUIRE(can_change_workspace_stage_area(current, current, others));
    // Either axis can supply the clearance: corners need not be 20 mm apart radially.
    REQUIRE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(450, 264), Vec2d(650, 508)), others));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(450, 263), Vec2d(650, 507)), others));
}

TEST_CASE("Existing overlapping or contained stages can move towards separation", "[WorkspaceStages]")
{
    const auto contained = GENERATE(false, true);
    const BoundingBoxf current = contained ? BoundingBoxf(Vec2d(100, 100), Vec2d(300, 344)) :
                                             BoundingBoxf(Vec2d(-100, 100), Vec2d(100, 344));
    const std::vector<BoundingBoxf> others {BoundingBoxf(Vec2d(0, 0), Vec2d(600, 600))};
    REQUIRE(can_change_workspace_stage_area(current,
        BoundingBoxf(current.min - Vec2d(10, 0), current.max - Vec2d(10, 0)), others));
    REQUIRE(can_change_workspace_stage_area(current,
        BoundingBoxf(current.min - Vec2d(500, 0), current.max - Vec2d(500, 0)), others));
    REQUIRE_FALSE(can_change_workspace_stage_area(current,
        BoundingBoxf(current.min + Vec2d(10, 0), current.max + Vec2d(10, 0)), others));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, current, others));
    // A smaller stage inside the existing area cannot hide a new containment overlap.
    REQUIRE_FALSE(can_change_workspace_stage_area(BoundingBoxf(Vec2d(-400, 100), Vec2d(-200, 344)), current, others));
}

TEST_CASE("Untangling must improve every existing overlap and create no new overlap", "[WorkspaceStages]")
{
    const BoundingBoxf current(Vec2d(0, 0), Vec2d(400, 244));
    const BoundingBoxf left(Vec2d(-200, 0), Vec2d(100, 300));
    const BoundingBoxf right(Vec2d(300, 0), Vec2d(600, 300));
    REQUIRE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(10, 0), Vec2d(390, 244)), {left, right}));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(10, 0), Vec2d(410, 244)), {left, right}));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(10, 0), Vec2d(400, 244)), {left, right}));
    const BoundingBoxf upper(Vec2d(0, 300), Vec2d(400, 544));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(10, 80), Vec2d(390, 324)), {left, right, upper}));
}

TEST_CASE("A containing stage can shrink away from its enclosed neighbor", "[WorkspaceStages]")
{
    const BoundingBoxf current(Vec2d(0, 0), Vec2d(600, 600));
    const std::vector<BoundingBoxf> others {BoundingBoxf(Vec2d(200, 200), Vec2d(400, 444))};
    REQUIRE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(0, 0), Vec2d(590, 600)), others));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, BoundingBoxf(Vec2d(0, 0), Vec2d(610, 600)), others));
    REQUIRE_FALSE(can_change_workspace_stage_area(BoundingBoxf(Vec2d(-800, 0), Vec2d(-200, 600)), current, others));
}

TEST_CASE("Empty stages and their title bars participate in area separation", "[WorkspaceStages]")
{
    // Areas contain no plate membership: empty stages use the same protection.
    const BoundingBoxf current(Vec2d(-400, 0), Vec2d(-200, 244));
    const BoundingBoxf proposed_body(Vec2d(0, 0), Vec2d(200, 200));
    const BoundingBoxf proposed_display(Vec2d(0, 0), Vec2d(200, 244));
    const std::vector<BoundingBoxf> others {BoundingBoxf(Vec2d(0, 260), Vec2d(200, 504))};
    REQUIRE(can_change_workspace_stage_area(current, proposed_body, others));
    REQUIRE_FALSE(can_change_workspace_stage_area(current, proposed_display, others));
    REQUIRE(can_change_workspace_stage_area(current, proposed_display, {}));
}

TEST_CASE("Area separation rejects undefined degenerate reversed and nonfinite rectangles", "[WorkspaceStages]")
{
    const BoundingBoxf valid(Vec2d(0, 0), Vec2d(200, 244));
    const std::string defect = GENERATE("undefined", "flat", "reversed", "nan", "infinity");
    BoundingBoxf invalid = valid;
    if (defect == "undefined") invalid.reset();
    else if (defect == "flat") invalid.max.x() = invalid.min.x();
    else if (defect == "reversed") invalid.max.y() = invalid.min.y() - 1.;
    else if (defect == "nan") invalid.min.x() = std::numeric_limits<double>::quiet_NaN();
    else invalid.max.y() = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(can_change_workspace_stage_area(invalid, valid, {}));
    REQUIRE_FALSE(can_change_workspace_stage_area(valid, invalid, {}));
    REQUIRE_FALSE(can_change_workspace_stage_area(valid, valid, {invalid}));
}

TEST_CASE("A group move preserves member spacing and permits heterogeneous beds", "[WorkspaceStages]")
{
    const std::vector<WorkspacePlateBounds> plates {
        {"fit-check", Vec2d(0, 0), BoundingBoxf(Vec2d(0, 0), Vec2d(180, 180)), false},
        {"fit-check", Vec2d(190, 0), BoundingBoxf(Vec2d(190, 0), Vec2d(540, 350)), false},
        {"final-parts", Vec2d(1000, 0), BoundingBoxf(Vec2d(1000, 0), Vec2d(1250, 250)), true}
    };
    REQUIRE(can_move_workspace_stage(plates, "fit-check", Vec2d(0, 400)));
    REQUIRE(can_move_workspace_stage(plates, "fit-check", Vec2d(-600, -400)));
    // Every member is evaluated against outside beds before the caller mutates anything.
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d(500, 0)));
}

TEST_CASE("A locked member blocks the whole stage while outside locks do not", "[WorkspaceStages]")
{
    std::vector<WorkspacePlateBounds> plates {
        {"fit-check", Vec2d(0, 0), BoundingBoxf(Vec2d(0, 0), Vec2d(180, 180)), false},
        {"fit-check", Vec2d(200, 0), BoundingBoxf(Vec2d(200, 0), Vec2d(380, 180)), true},
        {"other", Vec2d(1000, 0), BoundingBoxf(Vec2d(1000, 0), Vec2d(1200, 200)), true}
    };
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d(0, 300)));
    plates[1].locked = false;
    REQUIRE(can_move_workspace_stage(plates, "fit-check", Vec2d(0, 300)));
}

TEST_CASE("Group move preflight rejects nonfinite excessive and stationary movement", "[WorkspaceStages]")
{
    const std::vector<WorkspacePlateBounds> plates {
        {"fit-check", Vec2d(99900, 0), BoundingBoxf(Vec2d(99900, 0), Vec2d(100080, 180)), false}
    };
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d(101, 0)));
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d(0, std::numeric_limits<double>::infinity())));
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d(std::numeric_limits<double>::quiet_NaN(), 0)));
    REQUIRE_FALSE(can_move_workspace_stage(plates, "fit-check", Vec2d::Zero()));
    REQUIRE_FALSE(can_move_workspace_stage(plates, "", Vec2d(0, 100)));
    REQUIRE(can_move_workspace_stage({}, "empty-stage", Vec2d(0, 100)));
}

TEST_CASE("Group moves retain a clearance from outside plates", "[WorkspaceStages]")
{
    const std::vector<WorkspacePlateBounds> plates {
        {"stage", Vec2d(0, 0), BoundingBoxf(Vec2d(0, 0), Vec2d(180, 180)), false},
        {"", Vec2d(300, 0), BoundingBoxf(Vec2d(300, 0), Vec2d(480, 180)), false}
    };
    REQUIRE(can_move_workspace_stage(plates, "stage", Vec2d(115, 0)));
    REQUIRE_FALSE(can_move_workspace_stage(plates, "stage", Vec2d(116, 0)));
}

TEST_CASE("Automatic plate spacing leaves clearance for titles and controls", "[WorkspaceStages]")
{
    const double bed = GENERATE(80., 100., 120., 180., 256., 320., 500., 1000.);
    const Vec2d stride = workspace_plate_stride(bed, bed);
    REQUIRE(stride.x() - bed >= 80.);
    REQUIRE(stride.x() >= 180. + 24.);
    REQUIRE(stride.y() - bed >= 100.);
    REQUIRE(stride.y() - bed > 18. + 44. + 24.);
    // The file grid remains the inherited 20-percent convention, including row changes.
    REQUIRE_THAT((orca_plate_origin(4, 5, bed, bed) - Vec2d(bed * 1.2, -bed * 1.2)).norm(),
                 Catch::Matchers::WithinAbs(0., 1e-9));
}
