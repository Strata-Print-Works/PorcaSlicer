#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PlateCapacity.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectPlateImport.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Zipper.hpp"
#include "test_utils.hpp"

#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
PlatePrintSetup bed_setup(double width, double depth)
{
    PlatePrintSetup setup;
    setup.config = DynamicPrintConfig::full_print_config();
    setup.config.set_key_value("printable_area", new ConfigOptionPoints({Vec2d(0, 0), Vec2d(width, 0),
                                                                         Vec2d(width, depth), Vec2d(0, depth)}));
    return setup;
}

// An incoming project with one cube per listed plate; plate p owns instance p.
struct Incoming {
    Model model;
    std::vector<PlateData> plates;

    explicit Incoming(size_t count, double bed = 200.)
    {
        if (count > 0) {
            auto* object = model.add_object();
            object->add_volume(make_cube(10, 10, 10));
            for (size_t p = 0; p < count; ++p) object->add_instance()->set_offset(Vec3d(15. + 300. * p, 20., 0.));
        }
        for (size_t p = 0; p < count; ++p) {
            PlateData plate;
            plate.plate_index = int(p);
            plate.print_setup = bed_setup(bed, bed);
            plate.objects_and_instances = {{0, int(p)}};
            plate.has_workspace_position = true;
            plate.workspace_position = Vec2d(300. * p, 0.);
            plates.push_back(std::move(plate));
        }
    }

    std::vector<Vec3d> offsets() const
    {
        std::vector<Vec3d> result;
        for (const auto* object : model.objects)
            for (const auto* instance : object->instances) result.push_back(instance->get_offset());
        return result;
    }
};

void write_project_settings(const std::string& path, const std::string& settings)
{
    Zipper zip(path);
    zip.add_entry("Metadata/project_settings.config", settings.data(), settings.size());
    zip.finalize();
}
}

TEST_CASE("Imported stages receive new identities shared by their member plates", "[ProjectPlateImport]")
{
    Incoming incoming(2);
    incoming.plates[0].stage_id = "source-stage";
    incoming.plates[1].stage_id = "source-stage";
    incoming.plates[1].has_workspace_stages = true;
    incoming.plates[1].workspace_stages = {{"source-stage", "Batch", Vec2d(-40., 30.)},
                                           {"empty-stage", "Spare", Vec2d(900., 30.)}};
    const Vec2d stage_from_plate = incoming.plates[1].workspace_stages[0].position - incoming.plates[0].workspace_position;
    prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config, Vec2d(1000., 0.), 2);

    // The catalog moves to the first imported plate and keeps its empty stage.
    REQUIRE(incoming.plates[0].has_workspace_stages);
    CHECK_FALSE(incoming.plates[1].has_workspace_stages);
    const auto& stages = incoming.plates[0].workspace_stages;
    REQUIRE(stages.size() == 2);
    CHECK(stages[0].name == "Batch");
    CHECK(stages[1].name == "Spare");
    CHECK(stages[0].id != "source-stage");
    CHECK(stages[1].id != "empty-stage");
    CHECK(stages[0].id != stages[1].id);
    CHECK(incoming.plates[0].stage_id == stages[0].id);
    CHECK(incoming.plates[1].stage_id == stages[0].id);
    CHECK_NOTHROW(deserialize_workspace_stages(serialize_workspace_stages(stages)));
    // Stages and plates move by the same translation.
    CHECK_THAT((stages[0].position - incoming.plates[0].workspace_position - stage_from_plate).norm(), WithinAbs(0., 1e-9));
    CHECK(incoming.plates[0].plate_index == 2);
    CHECK(incoming.plates[1].plate_index == 3);
}

TEST_CASE("Importing two stage lists or an unknown stage leaves the incoming models unmoved", "[ProjectPlateImport]")
{
    Incoming incoming(2);
    const bool unknown_stage = GENERATE(false, true);
    for (auto& plate : incoming.plates) {
        plate.has_workspace_stages = !unknown_stage || &plate == &incoming.plates.front();
        plate.workspace_stages = {{"stage-a", "Batch", Vec2d(0., 0.)}};
    }
    if (unknown_stage) incoming.plates[1].stage_id = "missing-stage";
    const auto before = incoming.offsets();
    CHECK_THROWS_AS(prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config,
                                                 Vec2d(1000., 0.), 0), RuntimeError);
    CHECK(incoming.offsets() == before);
}

TEST_CASE("Models without a plate are rejected before any plate moves", "[ProjectPlateImport]")
{
    Incoming incoming(2);
    incoming.plates[1].objects_and_instances.clear();
    const auto before = incoming.offsets();
    CHECK_THROWS_AS(prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config,
                                                 Vec2d(1000., 0.), 0), RuntimeError);
    CHECK(incoming.offsets() == before);
    // A copy listed on two plates is equally ambiguous.
    incoming.plates[1].objects_and_instances = {{0, 0}};
    CHECK_THROWS_AS(prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config,
                                                 Vec2d(1000., 0.), 0), RuntimeError);
    CHECK(incoming.offsets() == before);
}

TEST_CASE("Each imported plate keeps its own prime tower position in the appended project", "[ProjectPlateImport]")
{
    Incoming incoming(2);
    for (auto& plate : incoming.plates) {
        plate.print_setup.config.set_key_value("wipe_tower_x", new ConfigOptionFloats({10., 20.}));
        plate.print_setup.config.set_key_value("wipe_tower_y", new ConfigOptionFloats({30., 40.}));
    }
    // A single shared value applies to every plate of the source project.
    incoming.plates[1].print_setup.config.set_key_value("wipe_tower_y", new ConfigOptionFloats({70.}));
    constexpr int first = 3;
    prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config, Vec2d(1000., 0.), first);
    // Each plate reads its tower at its own index in the combined project.
    const auto tower = [&](size_t plate, const char* key) {
        const auto& values = incoming.plates[plate].print_setup.config.option<ConfigOptionFloats>(key)->values;
        REQUIRE(values.size() == first + incoming.plates.size());
        return values[first + plate];
    };
    CHECK_THAT(tower(0, "wipe_tower_x"), WithinAbs(10., 1e-12));
    CHECK_THAT(tower(0, "wipe_tower_y"), WithinAbs(30., 1e-12));
    CHECK_THAT(tower(1, "wipe_tower_x"), WithinAbs(20., 1e-12));
    CHECK_THAT(tower(1, "wipe_tower_y"), WithinAbs(70., 1e-12));
}

TEST_CASE("Imports stay within the project plate limit", "[ProjectPlateImport]")
{
    Incoming incoming(0);
    for (int p = 0; p < 2; ++p) {
        PlateData plate;
        plate.plate_index = p;
        plate.print_setup = bed_setup(200., 200.);
        incoming.plates.push_back(std::move(plate));
    }
    auto plates = incoming.plates;
    CHECK_NOTHROW(prepare_project_plate_import(incoming.model, plates, incoming.plates[0].print_setup.config,
                                               Vec2d::Zero(), max_project_plates - 2));
    CHECK(plates.back().plate_index == max_project_plates - 1);
    for (int first : {max_project_plates - 1, max_project_plates, -1}) {
        DYNAMIC_SECTION("first plate " << first) {
            plates = incoming.plates;
            CHECK_THROWS_AS(prepare_project_plate_import(incoming.model, plates, incoming.plates[0].print_setup.config,
                                                         Vec2d::Zero(), first), RuntimeError);
        }
    }
}

TEST_CASE("Conventional grid origins use whole millimetres like the saved project", "[ProjectPlateImport]")
{
    // PartPlateList and the 3MF exporter size Orca's grid from an integer bed.
    const double bed = 220.6;
    CHECK_THAT((orca_plate_origin(1, 2, bed, bed) - orca_plate_origin(1, 2, 220., 220.)).norm(), WithinAbs(0., 1e-9));
    CHECK_THAT((orca_plate_origin(3, 4, bed, bed) - Vec2d(220. * 1.2, -220. * 1.2)).norm(), WithinAbs(0., 1e-9));

    // A file without Porca layout metadata places plate 1's copy at the
    // exporter's grid origin plus its bed-relative offset.
    Incoming incoming(2, bed);
    const Vec2d relative(15., 10.);
    const Vec2d file_origin(220. * 1.2, 0.);
    for (auto& plate : incoming.plates) plate.has_workspace_position = false;
    incoming.model.objects[0]->instances[1]->set_offset(Vec3d(file_origin.x() + relative.x(), file_origin.y() + relative.y(), 2.));
    incoming.model.objects[0]->instances[0]->set_offset(Vec3d(relative.x(), relative.y(), 2.));
    prepare_project_plate_import(incoming.model, incoming.plates, incoming.plates[0].print_setup.config, Vec2d(500., 0.), 0);
    for (int p = 0; p < 2; ++p) {
        const Vec3d offset = incoming.model.objects[0]->instances[p]->get_offset();
        CHECK_THAT((Vec2d(offset.x(), offset.y()) - incoming.plates[p].workspace_position - relative).norm(), WithinAbs(0., 1e-9));
        CHECK_THAT(offset.z(), WithinAbs(2., 1e-12));
    }
}

TEST_CASE("The source bed is read from string or list settings and bounded in size", "[ProjectPlateImport]")
{
    ScopedTemporaryFile file(".3mf");
    SECTION("Text setting") {
        write_project_settings(file.string(), R"({"printable_area":"0x0,200x0,200x210,0x210"})");
        const auto bed = read_project_bed_shape(file.string());
        REQUIRE(bed.size() == 4);
        CHECK_THAT(BoundingBoxf(bed).size().x(), WithinAbs(200., 1e-9));
        CHECK_THAT(BoundingBoxf(bed).size().y(), WithinAbs(210., 1e-9));
    }
    SECTION("List setting") {
        write_project_settings(file.string(), R"({"printable_area":["0x0","180x0","180x190","0x190"]})");
        const auto bed = read_project_bed_shape(file.string());
        REQUIRE(bed.size() == 4);
        CHECK_THAT(BoundingBoxf(bed).size().y(), WithinAbs(190., 1e-9));
    }
    SECTION("Unusable settings") {
        const std::string settings = GENERATE(std::string(R"({"printable_area":"0x0,200x0"})"),
                                              std::string(R"({"printer_model":"Unknown"})"),
                                              std::string("not json"));
        write_project_settings(file.string(), settings);
        CHECK(read_project_bed_shape(file.string()).empty());
    }
    SECTION("Oversized settings") {
        std::string settings = R"({"printable_area":"0x0,200x0,200x210,0x210"})";
        settings.append(8 * 1024 * 1024, ' ');
        write_project_settings(file.string(), settings);
        CHECK(read_project_bed_shape(file.string()).empty());
    }
    SECTION("Missing archive") {
        CHECK(read_project_bed_shape(file.string()).empty());
    }
}

TEST_CASE("Geometry-only imports drop print overrides but keep filament assignments", "[ProjectPlateImport]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    object->add_instance();
    object->config.set("extruder", 2);
    object->config.set("wall_loops", 6);
    volume->config.set("layer_height", 0.1);
    object->layer_config_ranges[{0., 2.}].set_key_value("sparse_infill_density", new ConfigOptionPercent(80.));
    object->layer_height_profile.set({0., 0.1, 2., 0.2});
    model.plates_custom_gcodes[0];
    clear_project_print_settings(model);
    CHECK(object->config.extruder() == 2);
    CHECK_FALSE(object->config.has("wall_loops"));
    CHECK(volume->config.empty());
    CHECK(object->layer_config_ranges.empty());
    CHECK(object->layer_height_profile.empty());
    CHECK(model.plates_custom_gcodes.empty());
}

TEST_CASE("Plate relocation validates every reference before moving copies", "[ProjectPlateImport]")
{
    Incoming incoming(2);
    const auto before = incoming.offsets();
    PlateData plate;
    for (const auto& references : {std::vector<std::pair<int, int>>{{0, 0}, {0, 0}},
                                   std::vector<std::pair<int, int>>{{0, 0}, {1, 0}},
                                   std::vector<std::pair<int, int>>{{0, 0}, {-1, 0}},
                                   std::vector<std::pair<int, int>>{{0, 0}, {0, 2}},
                                   std::vector<std::pair<int, int>>{{0, -1}}}) {
        plate.objects_and_instances = references;
        CHECK_THROWS_AS(relocate_plate_models(incoming.model, plate, Vec2d::Zero(), Vec2d(50., 0.)), RuntimeError);
        CHECK(incoming.offsets() == before);
        CHECK_FALSE(plate.has_model_origin);
    }
    plate.objects_and_instances = {{0, 1}};
    CHECK_THROWS_AS(relocate_plate_models(incoming.model, plate, Vec2d(std::numeric_limits<double>::quiet_NaN(), 0.),
                                          Vec2d::Zero()), RuntimeError);
    CHECK(incoming.offsets() == before);
    relocate_plate_models(incoming.model, plate, Vec2d(300., 0.), Vec2d(1000., -50.));
    CHECK(incoming.offsets()[0] == before[0]);
    CHECK_THAT((incoming.offsets()[1] - before[1] - Vec3d(700., -50., 0.)).norm(), WithinAbs(0., 1e-9));
    REQUIRE(plate.has_model_origin);
    CHECK_THAT((plate.model_origin - Vec2d(1000., -50.)).norm(), WithinAbs(0., 1e-12));
}
