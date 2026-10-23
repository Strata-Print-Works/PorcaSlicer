#include "libslic3r/PlateMaterialTransfer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include <sstream>
#include <set>
#include <cctype>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/path.hpp>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;

TEST_CASE("Cascaded layer heights reach toolpaths while object overrides remain local", "[ProcessCascade]")
{
    const int scope = GENERATE(0, 1, 2);
    auto config = Test::multifilament_config(1, {
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"gcode_comments", "1"},
        {"filament_nozzle_map", "0"}, {"enable_arc_fitting", "0"}, {"enable_support", "0"},
        {"enable_prime_tower", "0"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"}
    });
    PlatePrintSetup setup;
    setup.config = config;
    initialize_process_cascade(setup, config, {});
    ProcessSettingsScopes parents;
    parents.project.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    if (scope > 0) parents.stages["stage-a"].set_key_value("layer_height", new ConfigOptionFloat(0.3));
    if (scope > 1) setup.process_overrides.set_key_value("layer_height", new ConfigOptionFloat(0.15));
    config.apply(resolved_process_settings(setup, parents, "stage-a"));
    const double expected = scope == 0 ? 0.1 : scope == 1 ? 0.3 : 0.15;
    Print print;
    Model model;
    Test::init_print({Test::cube(6)}, print, model, config);
    const auto layers = Test::layers_with_role(Test::gcode(print), "perimeter");
    CHECK_THAT(print.objects().front()->config().layer_height.value, Catch::Matchers::WithinAbs(expected, 1e-9));
    CHECK(std::abs(double(layers.size()) - 6. / expected) <= 2.);
    Print local;
    Model local_model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides {{{"layer_height", "0.2"}}};
    Test::init_print(std::vector<TriangleMesh>{Test::cube(6)}, local, local_model, config, &overrides);
    CHECK_FALSE(Test::gcode(local).empty());
    CHECK_THAT(local.objects().front()->config().layer_height.value, Catch::Matchers::WithinAbs(0.2, 1e-9));
}

TEST_CASE("Bulk layer edits reach emitted toolpaths and preserve an object's own layer height", "[PlateProcessEdit]")
{
    struct RestoreResources {
        std::string previous = resources_dir();
        ~RestoreResources() { set_resources_dir(previous); }
    } restore_resources;
    set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string());
    auto config = Test::multifilament_config(1, {
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"gcode_comments", "1"}, {"filament_nozzle_map", "0"},
        {"enable_arc_fitting", "0"}, {"enable_support", "0"},
        {"enable_prime_tower", "0"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"}
    });
    Print first, second;
    Model first_model, second_model;
    Test::init_print({Test::cube(6)}, first, first_model, config);
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides {{{"layer_height", "0.2"}}};
    Test::init_print(std::vector<TriangleMesh>{Test::cube(6)}, second, second_model, config, &overrides);
    const auto original_position = first_model.objects[0]->instances[0]->get_offset();
    const auto first_before = Test::layers_with_role(Test::gcode(first), "perimeter");
    const auto second_before = Test::layers_with_role(Test::gcode(second), "perimeter");
    REQUIRE_FALSE(first_before.empty());
    REQUIRE_FALSE(second_before.empty());
    DynamicPrintConfig changes;
    changes.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    auto edit = prepare_plate_process_edit(config, {}, changes);
    INFO(edit.errors.size());
    REQUIRE(edit.errors.empty());
    first.apply(first_model, edit.config);
    second.apply(second_model, edit.config);
    const auto first_after = Test::layers_with_role(Test::gcode(first), "perimeter");
    const auto second_after = Test::layers_with_role(Test::gcode(second), "perimeter");
    CHECK(first_after.size() > first_before.size());
    CHECK(second_after == second_before);
    CHECK_THAT(first.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(second.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK_THAT((first_model.objects[0]->instances[0]->get_offset() - original_position).norm(),
               Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Editing another plate for the same printer preserves a completed plate slice", "[PlatePrintSetup][WorkspaceStages]")
{
    PresetBundle bundle;
    PlatePrintSetup first;
    first.config = Test::multifilament_config(1, {{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"gcode_comments", "1"}, {"filament_nozzle_map", "0"}});
    first.destination = "scanned-id:79d7522d-2e67-4d82-b281-4cf2bdb43193";
    first.destination_name = "Workshop printer";
    PlatePrintSetup second = first;

    Print first_print, second_print;
    Model first_model, second_model;
    first.activate(bundle, "Plate 1");
    first.capture(bundle);
    auto first_input = bundle.full_config(false);
    first_input.erase("filament_map_2");
    Test::init_print({Test::cube(6)}, first_print, first_model, first_input);
    const std::string first_gcode = Test::gcode(first_print);
    REQUIRE(first_print.is_step_done(psGCodeExport));
    second.config.set_deserialize_strict("layer_height", "0.3");
    second.config.set_deserialize_strict("initial_layer_print_height", "0.3");
    REQUIRE(second.destination == first.destination);
    first.activate(bundle, "Plate 1");
    second.activate(bundle, "Plate 2");
    auto second_input = bundle.full_config(false);
    second_input.erase("filament_map_2");
    Test::init_print({Test::cube(6)}, second_print, second_model, second_input);
    const std::string second_gcode = Test::gcode(second_print);
    REQUIRE(second_print.is_step_done(psGCodeExport));
    REQUIRE(first_print.is_step_done(psGCodeExport));
    REQUIRE(first_gcode != second_gcode);

    first.activate(bundle, "Plate 1");
    auto restored = bundle.full_config(false);
    restored.erase("filament_map_2");
    std::ostringstream differences;
    for (const auto& key : first_print.config().diff(restored))
        differences << key << ": print=" << first_print.config().option(key)->serialize() << " restored=" << restored.option(key)->serialize() << "\n";
    INFO(differences.str());
    REQUIRE(first_print.apply(first_model, restored) == Print::APPLY_STATUS_UNCHANGED);
    REQUIRE(first_print.is_step_done(psGCodeExport));
    REQUIRE_THAT(first_print.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.2, 1e-9));
    REQUIRE_THAT(second_print.objects()[0]->config().layer_height.value, Catch::Matchers::WithinAbs(0.3, 1e-9));
}

namespace {
BoundingBoxf emitted_perimeter_bounds(const std::string& gcode)
{
    BoundingBoxf bounds;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        if (!line.extruding(state) || line.comment().find("perimeter") == std::string_view::npos) return;
        // Closed perimeters include their first point again as the final endpoint.
        // Including prior travel starts would incorrectly enlarge the printed footprint.
        bounds.merge(Vec2d(line.new_X(state), line.new_Y(state)));
    });
    return bounds;
}

void init_workspace_cube(Print& print, Model& model, const DynamicPrintConfig& config, const Vec3d& origin)
{
    Test::init_print(std::vector<TriangleMesh>{Test::cube(6)}, print, model, config, nullptr, false);
    auto* object = model.objects[0];
    object->instances[0]->set_offset(origin + Vec3d(37.25, 56.5, 0.0));
    object->invalidate_bounding_box();
    print.set_plate_origin(origin);
    print.apply(model, config);
}
}

TEST_CASE("Workspace movement keeps emitted extrusion coordinates on the physical bed", "[PlatePrintSetup][WorkspaceStages]")
{
    const Vec3d origin = GENERATE(Vec3d(600.125, -450.375, 0.0),
                                 Vec3d(-500.625, 320.875, 0.0),
                                 Vec3d(17.375, 39.625, 0.0));
    const double bed_size = GENERATE(180.0, 350.0);
    auto config = Test::multifilament_config(1, {
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"gcode_comments", "1"}, {"filament_nozzle_map", "0"},
        {"is_imex", "0"}, {"belt_printer", "0"}, {"enable_arc_fitting", "0"},
        {"enable_support", "0"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"},
        {"enable_prime_tower", "0"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "0"}, {"wall_loops", "2"}
    });
    config.set_key_value("printable_area", new ConfigOptionPoints({Vec2d(0, 0), Vec2d(bed_size, 0),
                                                                  Vec2d(bed_size, bed_size), Vec2d(0, bed_size)}));
    config.set_key_value("printable_height", new ConfigOptionFloat(250.0));
    Print baseline_print, moved_print;
    Model baseline_model, moved_model;
    init_workspace_cube(baseline_print, baseline_model, config, Vec3d::Zero());
    const std::string baseline_gcode = Test::gcode(baseline_print);
    init_workspace_cube(moved_print, moved_model, config, origin);
    const std::string moved_gcode = Test::gcode(moved_print);
    const auto baseline = emitted_perimeter_bounds(baseline_gcode);
    const auto moved = emitted_perimeter_bounds(moved_gcode);
    REQUIRE(baseline.defined);
    REQUIRE(moved.defined);
    // Compare physical toolpath bounds, not byte-identical G-code or path ordering.
    REQUIRE_THAT((moved.min - baseline.min).norm(), Catch::Matchers::WithinAbs(0, 0.002));
    REQUIRE_THAT((moved.max - baseline.max).norm(), Catch::Matchers::WithinAbs(0, 0.002));
    REQUIRE(baseline.min.x() >= 37.25 - 0.002);
    REQUIRE(baseline.min.y() >= 56.5 - 0.002);
    REQUIRE(baseline.max.x() <= 37.25 + 6.0 + 0.002);
    REQUIRE(baseline.max.y() <= 56.5 + 6.0 + 0.002);
    REQUIRE(Test::layers_with_role(moved_gcode, "perimeter").size() == Test::layers_with_role(baseline_gcode, "perimeter").size());
    REQUIRE_THAT((moved_model.objects[0]->instances[0]->get_offset() - baseline_model.objects[0]->instances[0]->get_offset() - origin).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-9));
}


TEST_CASE("A transferred part extrudes using its chosen destination filament", "[PlateMaterialTransfer]")
{
    struct RestoreResources {
        std::string previous = resources_dir();
        ~RestoreResources() { set_resources_dir(previous); }
    } restore_resources;
    set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string());
    auto config = Test::multifilament_config(2, {
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"gcode_comments", "1"}, {"filament_nozzle_map", "0,0"},
        {"enable_arc_fitting", "0"}, {"enable_support", "0"},
        {"enable_prime_tower", "0"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"},
        {"outer_wall_filament_id", "0"}, {"inner_wall_filament_id", "0"},
        {"sparse_infill_filament_id", "0"}, {"internal_solid_filament_id", "0"},
        {"top_surface_filament_id", "0"}, {"bottom_surface_filament_id", "0"}
    });
    Print print;
    Model model;
    Test::init_print({Test::cube(6)}, print, model, config);
    auto* object = model.objects[0];
    const auto transformation = object->instances[0]->get_transformation();
    apply_plate_instance_transfers(model, {{0, 0, transformation, {{1, 2}}, 2}});
    print.apply(model, config);
    const auto gcode = Test::gcode(print);
    std::set<int> extruding_tools;
    int tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string command(line.cmd());
        if (command.size() > 1 && command[0] == 'T' && std::isdigit(static_cast<unsigned char>(command[1])))
            tool = std::stoi(command.substr(1));
        if (line.extruding(self)) extruding_tools.insert(tool);
    });
    CHECK(extruding_tools == std::set<int>{1});
    CHECK(model.objects[0]->config.extruder() == 2);
}
