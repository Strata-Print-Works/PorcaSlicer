#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PlateMaterialTransfer.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "test_utils.hpp"
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace {
DynamicPrintConfig palette(bool reversed = false)
{
    DynamicPrintConfig config;
    std::vector<std::string> profiles{"Black PETG", "White PLA"};
    std::vector<std::string> types{"PETG", "PLA"};
    std::vector<std::string> colors{"#000000", "#FFFFFF"};
    std::vector<int> temperatures{245, 210};
    if (reversed) {
        std::swap(profiles[0], profiles[1]);
        std::swap(types[0], types[1]);
        std::swap(colors[0], colors[1]);
        std::swap(temperatures[0], temperatures[1]);
    }
    config.set_key_value("filament_settings_id", new ConfigOptionStrings(profiles));
    config.set_key_value("filament_type", new ConfigOptionStrings(types));
    config.set_key_value("filament_vendor", new ConfigOptionStrings({"Maker", "Maker"}));
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts(temperatures));
    return config;
}
ModelObject* painted_object(Model& model)
{
    auto* object = model.add_object();
    object->name = "Painted part";
    auto* volume = object->add_volume(make_cube(6, 6, 6));
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    selector.set_facet(1, EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set_data(selector.serialize());
    object->add_instance()->set_offset(Vec3d(20, 30, -1.5));
    object->config.set("wall_loops", 5);
    object->config.set("outer_wall_filament_id", 1);
    object->layer_config_ranges[{1., 3.}].set("sparse_infill_filament_id", 2);
    object->config.set("support_filament", 2);
    volume->config.set("support_interface_filament", 1);
    object->layer_config_ranges[{1., 3.}].set("extruder", 2);
    return object;
}
}

TEST_CASE("Moving a painted part matches material identity rather than filament number", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    const std::map<int, int> expected{{1, 2}, {2, 1}};
    CHECK(match_plate_filaments(*object, palette(), palette(true)) == expected);
    CHECK(object_filament_slots(*object) == std::set<int>{1, 2});
    CHECK_FALSE(object->config.has("extruder"));
}

TEST_CASE("Unknown duplicate incompatible and mixed materials need an explicit choice", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto destination = palette();
    const auto variant = GENERATE(0, 1, 2, 3, 4);
    if (variant == 0) destination.erase("filament_type");
    if (variant == 1) destination.set_key_value("filament_diameter", new ConfigOptionFloats({2.85, 1.75}));
    if (variant == 2) {
        destination = palette();
        for (const auto& key : destination.keys()) {
            auto* values = dynamic_cast<ConfigOptionVectorBase*>(destination.option(key));
            if (values) values->set_at(values, 1, 0);
        }
    }
    if (variant == 3) destination.set_key_value("filament_is_mixed", new ConfigOptionBools({true, false}));
    if (variant == 4) destination.set_key_value("filament_colour", new ConfigOptionStrings({"#123456", "#FFFFFF"}));
    const auto matches = match_plate_filaments(*object, palette(), destination);
    CHECK(matches.count(1) == 0);
}

TEST_CASE("Moving one copy remaps paint and overrides without changing the copy left behind", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = painted_object(model);
    object->instances[0]->production_id = "original-copy";
    object->add_instance()->set_offset(Vec3d(250, 30, -1.5));
    const Model before = model;
    auto transformation = object->instances[0]->get_transformation();
    transformation.set_offset(Vec3d(420, 30, -1.5));
    const auto moved = apply_plate_instance_transfers(model, {{0, 0, transformation, {{1, 2}, {2, 1}}, 2}});
    REQUIRE(moved.size() == 1);
    REQUIRE(model.objects.size() == 2);
    const auto* transferred = model.objects[moved[0].first];
    CHECK(transferred->config.extruder() == 2);
    CHECK(transferred->config.get().opt_int("support_filament") == 1);
    CHECK(transferred->volumes[0]->config.get().opt_int("support_interface_filament") == 2);
    CHECK(transferred->layer_config_ranges.begin()->second.extruder() == 1);
    CHECK(transferred->config.get().opt_int("wall_loops") == 5);
    CHECK(transferred->config.opt_int("outer_wall_filament_id") == 2);
    CHECK(transferred->layer_config_ranges.begin()->second.opt_int("sparse_infill_filament_id") == 1);
    CHECK(transferred->instances[0]->production_id == "original-copy");
    CHECK_THAT((transferred->instances[0]->get_matrix().matrix() - transformation.get_matrix().matrix()).norm(),
               Catch::Matchers::WithinAbs(0., 1e-9));
    TriangleSelector paint(transferred->volumes[0]->mesh());
    paint.deserialize(transferred->volumes[0]->mmu_segmentation_facets.get_data(), false);
    TriangleSelector expected(transferred->volumes[0]->mesh());
    expected.set_facet(0, EnforcerBlockerType::Extruder2);
    expected.set_facet(1, EnforcerBlockerType::Extruder1);
    CHECK(paint.serialize() == expected.serialize());
    REQUIRE(model.objects[0]->instances.size() == 1);
    CHECK(model.objects[0]->config.get() == before.objects[0]->config.get());
    CHECK(model.objects[0]->volumes[0]->mmu_segmentation_facets.get_data() == before.objects[0]->volumes[0]->mmu_segmentation_facets.get_data());
    CHECK_THAT((model.objects[0]->instances[0]->get_matrix().matrix() - before.objects[0]->instances[1]->get_matrix().matrix()).norm(),
               Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("A missing material choice rejects the whole move before changing any instance", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = painted_object(model);
    object->add_instance()->set_offset(Vec3d(250, 30, 0));
    const Model before = model;
    auto transformation = object->instances[0]->get_transformation();
    transformation.set_offset(Vec3d(400, 30, 0));
    REQUIRE_THROWS(apply_plate_instance_transfers(model, {
        {0, 0, transformation, {{1, 2}, {2, 1}}, 2}, {0, 1, transformation, {{1, 1}}, 2}}));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(object->instances.size() == 2);
    CHECK(object->config.get() == before.objects[0]->config.get());
    CHECK(object->instances[0]->get_transformation() == before.objects[0]->instances[0]->get_transformation());
}

TEST_CASE("Moving several copies keeps their individual mappings and returns current indices", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = painted_object(model);
    object->add_instance()->set_offset(Vec3d(200, 30, 0));
    object->add_instance()->set_offset(Vec3d(400, 30, 0));
    const auto transform = object->instances[0]->get_transformation();
    const auto moved = apply_plate_instance_transfers(model, {
        {0, 0, transform, {{1, 2}, {2, 1}}, 2}, {0, 2, transform, {{1, 1}, {2, 2}}, 2}});
    REQUIRE(moved.size() == 2);
    CHECK(model.objects[moved[0].first]->config.extruder() == 2);
    CHECK_FALSE(model.objects[moved[1].first]->config.has("extruder"));
    CHECK(moved[1].second == 1);
}


TEST_CASE("A part with an explicit filament does not require an unused default filament", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    object->add_volume(make_cube(6, 6, 6))->config.set("extruder", 2);
    object->add_instance();
    CHECK(object_filament_slots(*object) == std::set<int>{2});
    const auto transformation = object->instances[0]->get_transformation();
    apply_plate_instance_transfers(model, {{0, 0, transformation, {{2, 1}}, 1}});
    CHECK(object->volumes[0]->config.extruder() == 1);
    CHECK_FALSE(object->config.has("extruder"));
}


TEST_CASE("Copies moved together with the same filaments remain linked", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = painted_object(model);
    object->add_instance()->set_offset(Vec3d(60, 30, 0));
    const auto first = object->instances[0]->get_transformation();
    const auto second = object->instances[1]->get_transformation();
    const auto moved = apply_plate_instance_transfers(model, {
        {0, 0, first, {{1, 2}, {2, 1}}, 2}, {0, 1, second, {{1, 2}, {2, 1}}, 2}});
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects[0]->instances.size() == 2);
    CHECK(model.objects[0]->config.extruder() == 2);
    CHECK(moved[0].first == moved[1].first);
    CHECK(moved[0].second != moved[1].second);
}

TEST_CASE("Material and diameter take priority over a closer destination color", "[PlateMaterialTransfer]")
{
    auto source = palette();
    auto destination = palette();
    // Source black PETG: white PETG is safer to present first than black PLA.
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PETG"}));
    const auto candidates = rank_plate_filaments(source, 1, destination);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].slot == 2);
    CHECK(candidates[0].same_material);
    CHECK_FALSE(candidates[0].exact);
    CHECK(candidates[1].same_color);
    CHECK_FALSE(candidates[1].same_material);
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PETG"}));
    destination.set_key_value("filament_diameter", new ConfigOptionFloats({2.85, 1.75}));
    CHECK(rank_plate_filaments(source, 1, destination).front().slot == 2);
}

TEST_CASE("The same profile precedes similar colors from other profiles", "[PlateMaterialTransfer]")
{
    const auto source = palette();
    auto destination = palette();
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PETG"}));
    destination.set_key_value("filament_settings_id", new ConfigOptionStrings({"Other PETG", "Black PETG"}));
    const auto candidates = rank_plate_filaments(source, 1, destination);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates.front().slot == 2);
    CHECK(candidates.front().same_profile);
    CHECK_FALSE(candidates.front().same_color);
    CHECK_FALSE(candidates.front().exact);
}

TEST_CASE("Colors rank perceptually within the same material and profile", "[PlateMaterialTransfer]")
{
    auto source = palette();
    source.set_key_value("filament_colour", new ConfigOptionStrings({"#0C0D0F", "#FFFFFF"}));
    auto destination = palette();
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PETG"}));
    destination.set_key_value("filament_settings_id", new ConfigOptionStrings({"Black PETG", "Black PETG"}));
    destination.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF", "#141517"}));
    const auto candidates = rank_plate_filaments(source, 1, destination);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].slot == 2);
    CHECK(candidates[0].color_distance < candidates[1].color_distance);
    CHECK_FALSE(candidates[0].exact);
}

TEST_CASE("Equivalent hex spelling matches but invalid colors cannot match automatically", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto source = palette();
    auto destination = palette();
    source.set_key_value("filament_colour", new ConfigOptionStrings({"#aBcDeF", "#FFFFFF"}));
    destination.set_key_value("filament_colour", new ConfigOptionStrings({"#ABCDEF", "#FFFFFF"}));
    CHECK(match_plate_filaments(*object, source, destination).at(1) == 1);
    source.set_key_value("filament_colour", new ConfigOptionStrings({"#XXXXXX", "#FFFFFF"}));
    destination.set_key_value("filament_colour", new ConfigOptionStrings({"#XXXXXX", "#FFFFFF"}));
    CHECK(match_plate_filaments(*object, source, destination).count(1) == 0);
    for (const auto& candidate : rank_plate_filaments(source, 1, destination)) {
        CHECK_FALSE(candidate.same_color);
        CHECK_FALSE(std::isfinite(candidate.color_distance));
    }
}

TEST_CASE("Missing and virtual source filaments never imply a physical replacement", "[PlateMaterialTransfer]")
{
    auto source = palette();
    const auto variant = GENERATE(0, 1, 2);
    if (variant == 0) source.clear();
    if (variant == 1) source.set_key_value("filament_is_mixed", new ConfigOptionBools({true, false}));
    if (variant == 2) source.erase("filament_type");
    for (const auto& candidate : rank_plate_filaments(source, 1, palette())) {
        CHECK_FALSE(candidate.exact);
        CHECK_FALSE(candidate.same_material);
        CHECK_FALSE(candidate.same_profile);
    }
}

TEST_CASE("Matching colors are preselected despite destination printer settings", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto destination = palette(true);
    destination.set_key_value("nozzle_temperature", new ConfigOptionInts({220, 260}));
    destination.set_key_value("filament_flow_ratio", new ConfigOptionFloats({0.97, 0.98}));
    destination.set_key_value("filament_start_gcode", new ConfigOptionStrings({"; destination PLA", "; destination PETG"}));
    const std::map<int, int> expected{{1, 2}, {2, 1}};
    CHECK(match_plate_filaments(*object, palette(), destination) == expected);
    for (int from : object_filament_slots(*object)) {
        const auto candidates = rank_plate_filaments(palette(), from, destination);
        REQUIRE_FALSE(candidates.empty());
        CHECK(candidates.front().same_color);
        CHECK(candidates.front().same_material);
        CHECK_FALSE(candidates.front().exact); // Still reviewed before committing.
    }
}

TEST_CASE("A matching profile disambiguates equal destination colors", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto destination = palette();
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PETG"}));
    destination.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#000000"}));
    destination.set_key_value("filament_settings_id", new ConfigOptionStrings({"Other PETG", "Black PETG"}));
    destination.set_key_value("nozzle_temperature", new ConfigOptionInts({255, 260}));
    CHECK(match_plate_filaments(*object, palette(), destination).at(1) == 2);
    destination.set_key_value("filament_settings_id", new ConfigOptionStrings({"Black PETG", "Black PETG"}));
    CHECK(match_plate_filaments(*object, palette(), destination).count(1) == 0);
}

TEST_CASE("Printer setting variants do not prevent matching physical colors", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto destination = palette();
    destination.set_key_value("filament_self_index", new ConfigOptionInts({1, 1, 1, 2, 2, 2}));
    const std::map<int, int> expected{{1, 1}, {2, 2}};
    CHECK(match_plate_filaments(*object, palette(), destination) == expected);
    for (int from : object_filament_slots(*object))
        CHECK_FALSE(rank_plate_filaments(palette(), from, destination).front().exact);
}

TEST_CASE("A shared physical palette matches across repeated printer variants", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    DynamicPrintConfig source;
    source.set_key_value("filament_colour", new ConfigOptionStrings({"#0C0D0F", "#7135A5", "#70D124", "#0C0D0F"}));
    source.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA", "PLA", "PETG"}));
    source.set_key_value("filament_vendor", new ConfigOptionStrings({"Bambu Lab", "Bambu Lab", "Bambu Lab", "Bambu Lab"}));
    source.set_key_value("filament_settings_id", new ConfigOptionStrings({"Bambu PLA Basic @System", "Bambu PLA Matte @System", "Bambu PLA Basic @System", "Bambu PETG HF @System"}));
    source.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75, 1.75}));
    source.set_key_value("filament_self_index", new ConfigOptionInts({1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,4,4,4,4,4,4}));
    auto destination = source;
    const std::map<int, int> expected{{1, 1}, {2, 2}};
    CHECK(match_plate_filaments(*object, source, destination) == expected);
    // The duplicate black PETG is not an alternative to the black PLA.
    CHECK_FALSE(rank_plate_filaments(source, 1, destination).back().same_material);
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA", "PLA", "PLA"}));
    destination.set_key_value("filament_settings_id", new ConfigOptionStrings({"Bambu PLA Basic @System", "Bambu PLA Matte @System", "Bambu PLA Basic @System", "Bambu PLA Basic @System"}));
    CHECK(match_plate_filaments(*object, source, destination).count(1) == 0);
}

TEST_CASE("Variant-indexed material identities are not guessed as physical colors", "[PlateMaterialTransfer]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    const auto* object = painted_object(model);
    auto destination = palette();
    destination.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PETG", "PLA", "PLA"}));
    CHECK(match_plate_filaments(*object, palette(), destination).empty());
}
