// These checks use CPU meshes only: no wx application, GL context or sockets.
#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PlateTransfer.hpp"
#include "slic3r/GUI/WorkspaceStyle.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/PlateCapacity.hpp"
#include "libslic3r/ProjectPlateImport.hpp"
#include "libslic3r/Semver.hpp"
#include "test_utils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <set>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Preview depth bounds keep shared plate titles intact when orbiting", "[WorkspaceScene][PreviewTitleClipping][Regression]")
{
    const double depth = GENERATE(200., 400.);
    const auto type = GENERATE(Camera::EType::Perspective, Camera::EType::Ortho);
    const double azimuth = Geometry::deg2rad(GENERATE(0., 45., 90., 135., 180., 225., 270., 315.));
    const double elevation = GENERATE(0.05, 0.3, 1.);
    Model model;
    Slic3r::GUI::PartPlateList plates(200, int(depth), 200., nullptr, &model, ptFFF);
    plates.set_shapes({{0., 0.}, {200., 0.}, {200., depth}, {0., depth}}, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE_FALSE(plates.independent_setups());

    // Titles retain their full size on small and large beds.
    const double scale = 1.;
    const double bottom = depth + 18.;
    const double top = bottom + 44. * scale;
    const auto& bounds = plates.get_bounding_box();
    CHECK(bounds.max.y() >= top);

    Camera camera;
    camera.set_type(type);
    camera.set_viewport(0, 0, 1200, 800);
    const Vec3d target(100., depth * 0.5, 0.);
    camera.look_at(target + 600. * Vec3d(std::cos(azimuth), std::sin(azimuth), elevation), target, Vec3d::UnitZ());
    camera.apply_projection(bounds);
    const Matrix4d transform = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    for (double x : {0., 200. * scale})
        for (double y : {bottom, top}) {
            const Vec4d clip = transform * Vec4d(x, y, WorkspaceStyle::chrome_z, 1.);
            REQUIRE(clip.w() > 0.);
            CHECK(clip.z() >= -clip.w());
            CHECK(clip.z() <= clip.w());
        }
}

TEST_CASE("Moving a plate into a new stage preserves a large scene with modifiers", "[WorkspaceScene][Regression]")
{
    constexpr int plate_count = 25;
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("printable_height", new ConfigOptionFloat(200.));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());

    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs plate_data;
    for (int i = 0; i < plate_count; ++i) {
        const Vec2d origin(400. * (i % 5), -400. * (i / 5));
        auto* object = model.add_object();
        for (int part = 0; part < 4; ++part) {
            auto* volume = object->add_volume(make_cube(10., 10., 10.));
            // Also exercise the clipped bounds used by parts that cross the bed plane.
            volume->set_offset(Vec3d(15. * part, 0., part == 0 ? -1. : 0.));
        }
        auto* modifier = object->add_volume(make_cube(4., 4., 4.));
        modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        modifier->set_offset(Vec3d(2., 2., 2.));
        object->add_instance()->set_offset(Vec3d(origin.x() + 20., origin.y() + 20., 0.));

        auto data = std::make_unique<PlateData>();
        data->plate_index = i;
        data->plate_name = "Plate " + std::to_string(i + 1);
        data->print_setup.config = config;
        data->has_workspace_position = true;
        data->workspace_position = origin;
        plate_data.push_back(data.get());
        stored.push_back(std::move(data));
    }

    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(plate_data) == 0);
    REQUIRE(plates.independent_setups());
    REQUIRE(plates.get_plate_count() == plate_count);
    // Keep the moved plate inactive so validation takes the independent-bed path.
    REQUIRE(plates.select_plate(1) == 0);

    GLVolumeCollection scene;
    for (int i = 0; i < plate_count; ++i)
        scene.load_object(model.objects[i], i, {0}, "volume", false, false);
    REQUIRE(scene.volumes.size() == plate_count * 5);
    REQUIRE(scene.volumes[4]->is_modifier);
    REQUIRE(scene.volumes[4]->convex_hull() == nullptr);

    const auto validate = [&]() {
        for (auto* volume : scene.volumes)
            volume->set_instance_transformation(model.objects[volume->object_idx()]->instances[0]->get_transformation());
        const auto* active = plates.get_curr_plate();
        const BuildVolume build_volume(active->get_shape(), 200., {}, {});
        REQUIRE(scene.check_outside_state(build_volume, model, plates, config, ptFFF, nullptr, nullptr));
        for (const auto* volume : scene.volumes)
            CHECK_FALSE(volume->is_outside);
    };
    validate();

    const auto stationary = model.objects[1]->instances[0]->get_offset();
    const Vec2d new_stage_position(-500., 500.);
    const std::string stage = plates.create_stage("New stage", new_stage_position);
    REQUIRE_FALSE(stage.empty());
    REQUIRE(plates.move_plate(0, new_stage_position));
    REQUIRE(plates.set_plate_stage(0, stage));
    validate();

    for (const Vec2d& destination : {Vec2d(-850., 850.), Vec2d(-700., 700.), new_stage_position}) {
        REQUIRE(plates.move_stage(stage, destination));
        CHECK_THAT((model.objects[0]->instances[0]->get_offset().head<2>() - destination - Vec2d(20., 20.)).norm(), WithinAbs(0., 1e-8));
        CHECK_THAT((model.objects[1]->instances[0]->get_offset() - stationary).norm(), WithinAbs(0., 1e-8));
        validate();
    }
}

TEST_CASE("Opening conventional plate coordinates restores independent bed placements", "[WorkspaceScene][Regression]")
{
    const Pointfs bed{{0., 0.}, {180., 0.}, {180., 180.}, {0., 180.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("printable_height", new ConfigOptionFloat(180.));
    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int p = 0; p < 3; ++p) {
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = p;
        plate->print_setup.config = config;
        plate->has_model_origin = true;
        plate->model_origin = orca_plate_origin(p, 3, 180., 180.);
        plate->has_workspace_position = true;
        plate->workspace_position = Vec2d(-400.25 + 550. * p, 60.125 - 440. * p);
        if (p != 2) {
            auto* object = model.add_object();
            object->add_volume(make_cube(10., 10., 10.));
            for (int copy = 0; copy <= p; ++copy) {
                object->add_instance()->set_offset(Vec3d(plate->model_origin.x() + 25. + 20. * copy, plate->model_origin.y() + 35., -1.));
                plate->objects_and_instances.emplace_back(p, copy);
            }
        }
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }
    // A previously selected larger printer must not change the saved positions.
    Slic3r::GUI::PartPlateList plates(500, 500, 500., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    REQUIRE(plates.get_plate_count() == 3);
    for (int p = 0; p < 3; ++p) {
        const Vec3d origin = plates.get_plate(p)->get_origin();
        CHECK_THAT((origin.head<2>() - data[p]->workspace_position).norm(), WithinAbs(0., 1e-9));
        for (const auto& [object, copy] : data[p]->objects_and_instances)
            CHECK_THAT((model.objects[object]->instances[copy]->get_offset() - origin - Vec3d(25. + 20. * copy, 35., -1.)).norm(), WithinAbs(0., 1e-9));
    }
}

TEST_CASE("Arranging scattered beds preserves locked jobs and bed-relative models", "[WorkspaceScene][PlateArrangement][Regression]")
{
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());

    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int i = 0; i < 4; ++i) {
        const Vec2d position(i == 0 ? 0. : 1000. * i, i == 0 ? 0. : -800. * i);
        auto* object = model.add_object();
        object->add_volume(make_cube(10., 10., 10.));
        object->add_instance()->set_offset(Vec3d(position.x() + 35., position.y() + 45., 7.));
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = position;
        plate->objects_and_instances.emplace_back(i, 0);
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }

    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    plates.get_plate(0)->lock(true);
    const auto preview = plates.workspace_arrangement();
    REQUIRE(preview.size() == 3);
    const auto after_removal = plates.workspace_arrangement({3});
    REQUIRE(after_removal.size() == 2);
    CHECK(after_removal[0].first == 1);
    CHECK(after_removal[1].first == 2);
    REQUIRE(plates.arrange_workspace_plates());
    CHECK_THAT(plates.get_plate(0)->get_origin().head<2>().norm(), WithinAbs(0., 1e-8));
    for (const auto& [index, position] : preview)
        CHECK_THAT((plates.get_plate(index)->get_origin().head<2>() - position).norm(), WithinAbs(0., 1e-8));
    for (int i = 1; i < 4; ++i) {
        const auto* plate = plates.get_plate(i);
        CHECK(plate->has_workspace_position());
        CHECK(plate->get_origin().y() > 200.);
        CHECK_THAT((model.objects[i]->instances[0]->get_offset() - plate->get_origin() - Vec3d(35., 45., 7.)).norm(), WithinAbs(0., 1e-8));
        CHECK(plate->print_setup().config == config);
        if (i > 1) {
            const auto& previous = plates.get_plate(i - 1)->get_bounding_box();
            const auto& current = plate->get_bounding_box();
            const bool separated = previous.max.x() < current.min.x() ||
                                   current.max.x() < previous.min.x() ||
                                   previous.max.y() < current.min.y() ||
                                   current.max.y() < previous.min.y();
            CHECK(separated);
        }
    }
    CHECK_FALSE(plates.arrange_workspace_plates());
}

TEST_CASE("Numbered print waves keep beds separate and model positions relative to each bed", "[WorkspaceScene][WorkspaceStages][Regression]")
{
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());
    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int i = 0; i < 4; ++i) {
        auto* object = model.add_object();
        object->add_volume(make_cube(10., 10., 10.));
        object->add_instance()->set_offset(Vec3d(35. + 400. * i, 45., 7.));
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = Vec2d(400. * i, 0.);
        plate->objects_and_instances.emplace_back(i, 0);
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }
    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    const std::vector<Vec2d> positions {Vec2d(2000., 0.), Vec2d(2400., 0.),
                                        Vec2d(2000., -500.), Vec2d(2400., -500.)};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(plates.move_plate(i, positions[i]));
        CHECK_THAT((model.objects[i]->instances[0]->get_offset() - plates.get_plate(i)->get_origin() -
                    Vec3d(35., 45., 7.)).norm(), WithinAbs(0., 1e-8));
    }
    const auto first = plates.create_stage("Stage 1", Vec2d(1940., -60.));
    REQUIRE_FALSE(first.empty());
    REQUIRE(plates.set_plates_stage({0, 1}, first));
    const auto second = plates.create_stage("Stage 2", Vec2d(1940., -560.));
    REQUIRE_FALSE(second.empty());
    REQUIRE(plates.set_plates_stage({2, 3}, second));
    CHECK(plates.stage_bounds(first).min.y() > plates.stage_bounds(second).max.y() + 20.);
    for (int i = 0; i < 4; ++i)
        CHECK(plates.get_plate(i)->stage_id() == (i < 2 ? first : second));
}

TEST_CASE("Empty plate cleanup keeps jobs, locks, and a usable plate", "[WorkspaceScene][PlateCleanup][Regression]")
{
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());

    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance()->set_offset(Vec3d(335., 35., 0.));
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int i = 0; i < 4; ++i) {
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = Vec2d(300. * i, 0.);
        plate->locked = i == 2;
        if (i == 1) plate->objects_and_instances.emplace_back(0, 0);
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }

    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    const auto stage = plates.create_stage("Stage", Vec2d(1200., 0.), 3);
    REQUIRE(plates.select_plate(3) == 0);
    const auto empty = plates.empty_plate_indices_to_remove();
    REQUIRE((empty == std::vector<int>{0, 3}));
    REQUIRE(plates.select_plate(1) == 0);
    for (auto it = empty.rbegin(); it != empty.rend(); ++it)
        REQUIRE(plates.delete_plate(*it) == 0);
    REQUIRE(plates.get_plate_count() == 2);
    CHECK(plates.get_curr_plate() == plates.get_plate(0));
    CHECK_FALSE(plates.get_plate(0)->empty());
    CHECK_THAT((plates.get_plate(0)->get_origin().head<2>() - Vec2d(300., 0.)).norm(), WithinAbs(0., 1e-9));
    CHECK(plates.get_plate(1)->is_locked());
    CHECK(plates.stages().size() == 1);
    CHECK(plates.stages().front().id == stage);
    CHECK(plates.empty_plate_indices_to_remove().empty());

    Model empty_model;
    std::vector<std::unique_ptr<PlateData>> empty_stored;
    PlateDataPtrs empty_data;
    for (int i = 0; i < 4; ++i) {
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = Vec2d(300. * i, 0.);
        empty_data.push_back(plate.get());
        empty_stored.push_back(std::move(plate));
    }
    Slic3r::GUI::PartPlateList empty_workspace(200, 200, 200., nullptr, &empty_model, ptFFF);
    empty_workspace.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(empty_workspace.load_from_3mf_structure(empty_data) == 0);
    // With every plate empty and unlocked, cleanup retains the selected one.
    REQUIRE(empty_workspace.select_plate(2) == 0);
    const auto candidates = empty_workspace.empty_plate_indices_to_remove();
    REQUIRE((candidates == std::vector<int>{0, 1, 3}));
    const auto* kept = empty_workspace.get_curr_plate();
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
        REQUIRE(empty_workspace.delete_plate(*it) == 0);
    CHECK(empty_workspace.get_plate_count() == 1);
    CHECK(empty_workspace.get_curr_plate() == kept);
}


TEST_CASE("Deleting the selected plate keeps every remaining plate's setup and a valid selection", "[WorkspaceScene][PlateCleanup][Regression]")
{
    // First, middle and last plate: the successor or the end of the list must
    // never stand in for the deleted plate.
    const int deleted = GENERATE(0, 1, 3);
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    const auto layer_height = [](int plate) { return 0.1 + 0.05 * plate; };
    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int i = 0; i < 4; ++i) {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_key_value("printable_area", new ConfigOptionPoints(bed));
        config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
        config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
        config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height(i)));
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = Vec2d(300. * i, 0.);
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }
    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    REQUIRE(plates.independent_setups());
    REQUIRE(plates.select_plate(deleted) == 0);
    std::vector<const PartPlate*> survivors;
    for (int i = 0; i < 4; ++i)
        if (i != deleted) survivors.push_back(plates.get_plate(i));

    REQUIRE(plates.delete_plate(deleted) == 0);
    REQUIRE(plates.get_plate_count() == 3);
    const int expected = deleted == 0 ? 0 : deleted - 1;
    REQUIRE(plates.get_curr_plate_index() == expected);
    CHECK(plates.get_curr_plate() == survivors[size_t(expected)]);
    for (int i = 0; i < 3; ++i) {
        const int original = i < deleted ? i : i + 1;
        CHECK(plates.get_plate(i) == survivors[size_t(i)]);
        CHECK(plates.get_plate(i)->get_index() == i);
        CHECK_THAT(plates.get_plate(i)->print_setup().config.opt_float("layer_height"), WithinAbs(layer_height(original), 1e-12));
    }
}

TEST_CASE("Selected plates move together without changing their physical jobs", "[WorkspaceScene][PlateGroupSelection][Regression]")
{
    auto config = DynamicPrintConfig::full_print_config();
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());
    Model model;
    std::vector<std::unique_ptr<PlateData>> stored;
    PlateDataPtrs data;
    for (int i = 0; i < 3; ++i) {
        const Vec2d origin(300. * i, 0.);
        auto* object = model.add_object();
        object->add_volume(make_cube(10., 20., 30.));
        auto* instance = object->add_instance();
        instance->set_offset(Vec3d(origin.x() + 35., 45., 7.));
        instance->set_rotation(Vec3d(0.2, 0.1, 0.6));
        instance->set_scaling_factor(Vec3d(1.1, 0.9, 1.2));
        auto plate = std::make_unique<PlateData>();
        plate->plate_index = i;
        plate->print_setup.config = config;
        plate->has_workspace_position = true;
        plate->workspace_position = origin;
        plate->objects_and_instances.emplace_back(i, 0);
        data.push_back(plate.get());
        stored.push_back(std::move(plate));
    }
    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.load_from_3mf_structure(data) == 0);
    REQUIRE(plates.select_plate(0) == 0);
    plates.select_workspace_plate(0);
    plates.select_workspace_plate(1, true);
    CHECK(plates.workspace_selection() == std::vector<int>{0, 1});
    CHECK(plates.get_curr_plate_index() == 0);
    plates.select_workspace_plate(1, true);
    CHECK(plates.workspace_selection() == std::vector<int>{0});
    plates.select_workspace_plate(1, true);
    const auto selected = plates.workspace_selection();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        plates.get_plate(i)->update_slice_result_valid_state(true);
    const auto transform = model.objects[0]->instances[0]->get_matrix().linear().eval();
    const auto stationary = model.objects[2]->instances[0]->get_offset();
    const auto before = model.objects[0]->instances[0]->get_offset();

    // Moving into a selected member's preceding position is legal. Sequential
    // individual moves would reject this or leave a partially translated group.
    REQUIRE(plates.move_plates(selected, Vec2d(0., 400.)));
    REQUIRE(plates.move_plates(selected, Vec2d(300., 0.)));
    CHECK_THAT((model.objects[0]->instances[0]->get_offset() - before - Vec3d(300., 400., 0.)).norm(), WithinAbs(0., 1e-8));
    for (int i : selected) {
        const auto local = model.objects[i]->instances[0]->get_offset() - plates.get_plate(i)->get_origin();
        CHECK_THAT((local - Vec3d(35., 45., 7.)).norm(), WithinAbs(0., 1e-8));
        CHECK(model.objects[i]->instances[0]->get_matrix().linear().isApprox(transform));
        CHECK(plates.get_plate(i)->print_setup().config == config);
    }
    CHECK_THAT((model.objects[2]->instances[0]->get_offset() - stationary).norm(), WithinAbs(0., 1e-8));
    CHECK_FALSE(plates.get_plate(0)->is_slice_result_valid());
    CHECK_FALSE(plates.get_plate(1)->is_slice_result_valid());
    CHECK(plates.get_plate(2)->is_slice_result_valid());
    const auto moved = model.objects[0]->instances[0]->get_offset();
    // Only member 1 would collide with stationary bed 2: reject the whole move.
    CHECK_FALSE(plates.move_plates(selected, Vec2d(0., -400.)));
    CHECK_THAT((model.objects[0]->instances[0]->get_offset() - moved).norm(), WithinAbs(0., 1e-8));
    plates.get_plate(1)->lock(true);
    CHECK_FALSE(plates.move_plates(selected, Vec2d(0., 300.)));
    CHECK_THAT((model.objects[0]->instances[0]->get_offset() - moved).norm(), WithinAbs(0., 1e-8));
    plates.get_plate(1)->lock(false);
    CHECK_FALSE(plates.move_plates({0, 0}, Vec2d(0., 300.)));
    CHECK_FALSE(plates.move_plates({0, 50}, Vec2d(0., 300.)));
    CHECK_FALSE(plates.move_plates(selected, Vec2d(100001., 0.)));

    const auto source = plates.create_stage("Source", Vec2d(300., 400.), 0);
    REQUIRE_FALSE(source.empty());
    REQUIRE(plates.set_plates_stage(selected, source));
    const auto source_bounds = plates.stage_bounds(source);
    const auto destination = plates.create_stage("Destination", Vec2d(0., 1600.));
    REQUIRE_FALSE(destination.empty());
    plates.begin_plate_drag(0);
    REQUIRE(plates.move_plates(selected, Vec2d(-300., 1200.)));
    CHECK(plates.stage_bounds(source).min.isApprox(source_bounds.min));
    CHECK(plates.stage_bounds(source).max.isApprox(source_bounds.max));
    REQUIRE(plates.can_set_plates_stage(selected, destination));
    REQUIRE(plates.set_plates_stage(selected, destination));
    plates.end_plate_drag();
    CHECK(plates.get_plate(0)->stage_id() == destination);
    CHECK(plates.get_plate(1)->stage_id() == destination);
    CHECK(plates.get_plate(2)->stage_id().empty());
    CHECK_FALSE(plates.set_plates_stage({0, 50}, source));
    CHECK(plates.get_plate(0)->stage_id() == destination);
    REQUIRE(plates.set_plates_stage(selected, {}));
    // Escape/capture-loss uses the same atomic translation to restore a group.
    REQUIRE(plates.move_plates(selected, Vec2d(300., -1200.)));
    CHECK_THAT((model.objects[0]->instances[0]->get_offset() - moved).norm(), WithinAbs(0., 1e-8));
    plates.clear_workspace_selection();
    CHECK(plates.workspace_selection().empty());
}


TEST_CASE("Project import fills the expanded plate capacity without overrunning it", "[WorkspaceScene][ProjectPlateImport][Regression]")
{
    constexpr int existing_count = 40;
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());

    Model live;
    Slic3r::GUI::PartPlateList workspace(200, 200, 200., nullptr, &live, ptFFF);
    workspace.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    std::vector<PlateData> existing(existing_count);
    PlateDataPtrs initial;
    for (int i = 0; i < existing_count; ++i) {
        existing[i].plate_index = i;
        existing[i].print_setup.config = config;
        existing[i].has_workspace_position = true;
        existing[i].workspace_position = Vec2d(400. * (i % 8), -400. * (i / 8));
        initial.push_back(&existing[i]);
    }
    REQUIRE(workspace.load_from_3mf_structure(initial) == 0);
    REQUIRE(workspace.get_plate_count() == existing_count);

    Model incoming;
    std::vector<PlateData> added(max_project_plates - existing_count);
    for (auto& plate : added) plate.print_setup.config = config;
    prepare_project_plate_import(incoming, added, config, Vec2d(4000., 0.), existing_count);
    workspace.append_project_plates(incoming, added);
    REQUIRE(workspace.get_plate_count() == max_project_plates);
    CHECK(workspace.get_plate(max_project_plates - 1)->get_index() == max_project_plates - 1);
    CHECK(workspace.create_plate(false) == -1);

    std::vector<PlateData> overflow{added.front()};
    REQUIRE_THROWS(prepare_project_plate_import(incoming, overflow, config, Vec2d(8000., 0.), max_project_plates));
    PlateDataPtrs too_many = initial;
    for (auto& plate : added) too_many.push_back(&plate);
    too_many.push_back(&overflow.front());
    REQUIRE_THROWS(workspace.load_from_3mf_structure(too_many));
    CHECK(workspace.get_plate_count() == max_project_plates);
}

TEST_CASE("Three projects append independent jobs without replacing existing work", "[WorkspaceScene][ProjectPlateImport][Regression]")
{
    auto config = DynamicPrintConfig::full_print_config();
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    config.set_key_value("printable_area", new ConfigOptionPoints(bed));
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups());
    config.set_key_value("extruder_printable_height", new ConfigOptionFloatsNullable());
    Model live;
    ScopedTemporaryDir backup;
    live.set_backup_path(backup.string());
    auto* existing = live.add_object();
    existing->add_volume(make_cube(10., 10., 10.));
    live.add_material("shared")->attributes["name"] = "Existing material";
    existing->volumes[0]->set_material_id("shared");
    existing->add_instance()->set_offset(Vec3d(25., 30., 0.));
    const auto original = existing->instances[0]->get_matrix();
    PlateData first;
    first.plate_index = 0;
    first.plate_name = "Existing work";
    first.print_setup.config = config;
    first.has_model_origin = first.has_workspace_position = true;
    first.objects_and_instances = {{0, 0}};
    PlateDataPtrs initial{&first};
    Slic3r::GUI::PartPlateList workspace(200, 200, 200., nullptr, &live, ptFFF);
    workspace.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(workspace.load_from_3mf_structure(initial) == 0);
    workspace.get_plate(0)->update_slice_result_valid_state(true);
    std::set<std::string> stage_ids;
    for (int file = 0; file < 3; ++file) {
        Model incoming;
        auto* object = incoming.add_object();
        object->add_volume(make_cube(10., 20., 30.));
        incoming.add_material("shared")->attributes["name"] = "Imported material";
        object->volumes[0]->set_material_id("shared");
        incoming.plates_custom_gcodes[0].gcodes = {{0.4, CustomGCode::PausePrint, 1, "", "Pause for insert"}};
        for (int copy = 0; copy < 2; ++copy) {
            auto* instance = object->add_instance();
            instance->set_offset(Vec3d(1035. + 30. * copy, -555., -2.));
            instance->set_rotation(Vec3d(0.2, 0.3, 0.4));
            instance->set_scaling_factor(Vec3d(1.1, 0.9, 1.2));
        }
        const auto linear = object->instances[0]->get_matrix().linear().eval();
        PlateData job;
        job.plate_index = 0;
        job.plate_name = "Job " + std::to_string(file + 1);
        job.print_setup.config = config;
        job.print_setup.config.set_key_value("layer_height", new ConfigOptionFloat(0.12 + file * 0.04));
        job.print_setup.destination = "saved:printer-" + std::to_string(file);
        job.print_setup.destination_name = "Printer " + std::to_string(file);
        job.has_model_origin = job.has_workspace_position = true;
        job.model_origin = Vec2d(1000., -600.);
        job.workspace_position = Vec2d(2000., -900.);
        job.objects_and_instances = {{0, 0}, {0, 1}};
        job.stage_id = "same-source-stage";
        job.has_workspace_stages = true;
        job.workspace_stages = {{job.stage_id, "Batch", Vec2d(2000., -900.)}};
        PlateData empty = job;
        empty.plate_index = 1;
        empty.plate_name = "Spare";
        empty.objects_and_instances.clear();
        empty.workspace_position += Vec2d(320., 0.);
        empty.locked = true;
        empty.has_workspace_stages = false;
        empty.workspace_stages.clear();
        std::vector<PlateData> jobs{job, empty};
        const int start = workspace.get_plate_count();
        const Vec2d minimum(workspace.get_bounding_box().max.x() + 120., 0.);
        prepare_project_plate_import(incoming, jobs, config, minimum, start);
        const auto incoming_id = incoming.objects[0]->id();
        workspace.append_project_plates(incoming, jobs);
        REQUIRE(workspace.get_plate_count() == start + 2);
        CHECK(workspace.get_curr_plate_index() == 0);
        CHECK(workspace.get_plate(0)->is_slice_result_valid());
        CHECK(workspace.get_plate(0)->print_setup().config == config);
        CHECK(live.objects[0]->instances[0]->get_matrix().isApprox(original));
        auto* added = workspace.get_plate(start);
        CHECK(added->contain_instance(file + 1, 0));
        CHECK(added->contain_instance(file + 1, 1));
        CHECK_FALSE(added->is_slice_result_valid());
        CHECK_THAT(added->print_setup().config.opt_float("layer_height"), WithinAbs(0.12 + file * 0.04, 1e-9));
        CHECK(added->print_setup().destination == job.print_setup.destination);
        CHECK(workspace.get_plate(start + 1)->is_locked());
        CHECK(workspace.get_plate(start + 1)->empty());
        CHECK(stage_ids.insert(added->stage_id()).second);
        CHECK(added->stage_id() != "same-source-stage");
        CHECK(live.objects[file + 1]->id() != incoming_id);
        CHECK(live.get_material("shared")->attributes.at("name") == "Existing material");
        CHECK(live.objects[file + 1]->volumes[0]->material()->attributes.at("name") == "Imported material");
        CHECK(live.objects[file + 1]->volumes[0]->material_id() != "shared");
        CHECK(live.plates_custom_gcodes.at(start) == incoming.plates_custom_gcodes.at(0));
        for (int copy = 0; copy < 2; ++copy) {
            const auto* instance = live.objects[file + 1]->instances[copy];
            const Vec3d local = instance->get_offset() - added->get_origin();
            CHECK_THAT((local - Vec3d(35. + 30. * copy, 45., -2.)).norm(), WithinAbs(0., 1e-7));
            CHECK(instance->get_matrix().linear().isApprox(linear));
        }
    }
    CHECK(workspace.stages().size() == 3);
    CHECK(live.objects.size() == 4);
}

namespace {
// Two 100 mm beds side by side with a 20 mm gap, printable 0..250 mm high.
std::vector<BoundingBoxf3> two_beds()
{
    return {BoundingBoxf3(Vec3d(0., 0., 0.), Vec3d(100., 100., 250.)),
            BoundingBoxf3(Vec3d(120., 0., 0.), Vec3d(220., 100., 250.))};
}

BoundingBoxf3 part_at(double min_x, double max_x)
{
    return BoundingBoxf3(Vec3d(min_x, 40., 0.), Vec3d(max_x, 60., 20.));
}
} // namespace

TEST_CASE("A moved part stays on its plate while it still touches that bed", "[WorkspaceScene][PlateTransfer]")
{
    // The part reaches across the gap onto the second bed but keeps a foothold on its own.
    const auto destinations = plate_move_destinations({{0, part_at(90., 130.)}}, two_beds());
    REQUIRE(destinations.size() == 1);
    CHECK(destinations[0] == 0);
}

TEST_CASE("A part dragged fully onto another bed joins that plate", "[WorkspaceScene][PlateTransfer]")
{
    const auto destinations = plate_move_destinations({{0, part_at(150., 170.)}, {1, part_at(10., 30.)}}, two_beds());
    REQUIRE(destinations.size() == 2);
    CHECK(destinations[0] == 1);
    CHECK(destinations[1] == 0);
}

TEST_CASE("A part dragged off every bed lands outside the plates", "[WorkspaceScene][PlateTransfer]")
{
    // The gap between the beds and the space beyond them belong to no plate.
    const auto destinations = plate_move_destinations({{0, part_at(102., 118.)}, {1, part_at(300., 320.)}}, two_beds());
    REQUIRE(destinations.size() == 2);
    CHECK(destinations[0] == -1);
    CHECK(destinations[1] == -1);
}

TEST_CASE("A part from outside the plates joins the first bed it touches", "[WorkspaceScene][PlateTransfer]")
{
    // Touching both beds from outside picks the first plate in order.
    const auto destinations = plate_move_destinations({{-1, part_at(150., 170.)}, {-1, part_at(90., 130.)}}, two_beds());
    REQUIRE(destinations.size() == 2);
    CHECK(destinations[0] == 1);
    CHECK(destinations[1] == 0);
}

TEST_CASE("A part whose recorded plate no longer exists is placed by the beds it touches", "[WorkspaceScene][PlateTransfer]")
{
    const auto destinations = plate_move_destinations({{5, part_at(150., 170.)}}, two_beds());
    REQUIRE(destinations.size() == 1);
    CHECK(destinations[0] == 1);
}

TEST_CASE("Clearing the plate selection leaves the active plate with its cue", "[WorkspaceScene][PlateGroupSelection][Regression]")
{
    Model model;
    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes({{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}}, {}, {}, {}, {}, "", 0.f, 0.f);
    REQUIRE(plates.get_plate_count() == 1);
    // Before any explicit choice the active plate is the selection.
    CHECK(plates.has_workspace_selection());
    CHECK(plates.is_workspace_selected(0));
    // An empty selection leaves no plate selected; the renderer then cues the active plate.
    plates.clear_workspace_selection();
    CHECK(plates.workspace_selection().empty());
    CHECK_FALSE(plates.has_workspace_selection());
    CHECK(plates.get_curr_plate_index() == 0);
    plates.select_workspace_plate(0);
    CHECK(plates.has_workspace_selection());
    // Toggling the only member off empties the selection again.
    plates.select_workspace_plate(0, true);
    CHECK_FALSE(plates.has_workspace_selection());
}

TEST_CASE("Stage title controls are drawn and picked inside one title bar", "[WorkspaceScene][StageLayout]")
{
    const double width = GENERATE(200., 400., 1200.);
    const double text_width = GENERATE(0., 120., 900.);
    const BoundingBoxf frame(Vec2d(-150., 40.), Vec2d(-150. + width, 340.));
    const auto layout = WorkspaceStyle::stage_layout(frame, text_width);

    // The bar sits on the frame's rear edge, at the shared title height.
    CHECK_THAT(layout.bar.min.x(), WithinAbs(frame.min.x(), 1e-9));
    CHECK_THAT(layout.bar.min.y(), WithinAbs(frame.max.y(), 1e-9));
    CHECK_THAT(layout.bar.size().y(), WithinAbs(WorkspaceStyle::stage_title_height, 1e-9));
    // It grows with the name from a minimum width but never past the frame.
    CHECK(layout.bar.size().x() <= frame.size().x() + 1e-9);
    CHECK(layout.bar.size().x() >= std::min(220., frame.size().x()) - 1e-9);
    if (std::max(220., text_width + 140.) <= frame.size().x())
        CHECK_THAT(layout.bar.size().x(), WithinAbs(std::max(220., text_width + 140.), 1e-9));

    // The name is centred vertically in the bar.
    CHECK_THAT(layout.text_origin.y() - layout.bar.min.y(),
        WithinAbs(layout.bar.max.y() - (layout.text_origin.y() + WorkspaceStyle::stage_title_text_height), 1e-9));

    // Title, rename and actions stay inside the bar, left to right, without overlapping.
    for (const BoundingBoxf* control : {&layout.title, &layout.rename, &layout.actions}) {
        CHECK(control->min.x() >= layout.bar.min.x());
        CHECK(control->max.x() <= layout.bar.max.x());
        CHECK(control->min.y() >= layout.bar.min.y());
        CHECK(control->max.y() <= layout.bar.max.y());
    }
    CHECK(layout.title.max.x() <= layout.rename.min.x());
    CHECK(layout.rename.max.x() <= layout.actions.min.x());

    // Resize grips sit inside the frame, one at each corner.
    for (int corner = 0; corner < 4; ++corner) {
        const auto& grip = layout.corners[corner];
        CHECK(grip.min.x() >= frame.min.x());
        CHECK(grip.max.x() <= frame.max.x());
        CHECK(grip.min.y() >= frame.min.y());
        CHECK(grip.max.y() <= frame.max.y());
        CHECK_THAT(corner & 1 ? grip.max.x() : grip.min.x(), WithinAbs(corner & 1 ? frame.max.x() : frame.min.x(), 1e-9));
        CHECK_THAT(corner & 2 ? grip.max.y() : grip.min.y(), WithinAbs(corner & 2 ? frame.max.y() : frame.min.y(), 1e-9));
    }

    // The area other stages keep clear of covers the frame and its title bar.
    const auto area = WorkspaceStyle::stage_display_area(frame);
    CHECK(area.contains(layout.bar.min));
    CHECK(area.contains(layout.bar.max));
    CHECK(area.contains(frame.min));
    CHECK_FALSE(WorkspaceStyle::stage_display_area(BoundingBoxf()).defined);
}

TEST_CASE("An inactive plate checks collision volumes from its own printer setup", "[WorkspaceScene][Regression]")
{
    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance()->set_offset(Vec3d(20., 20., 0.));
    const Pointfs bed{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}};
    Slic3r::GUI::PartPlateList plates(200, 200, 200., nullptr, &model, ptFFF);
    plates.set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    auto* plate = plates.get_plate(0);
    plate->set_unselected();
    auto& config = plate->print_setup().config;
    config = DynamicPrintConfig::full_print_config();
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints());
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(BedExcludeVolumeMode::Shared));
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString("0..20;15x15,40x15,40x40,15x40"));
    CHECK(plate->check_outside(0, 0));
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString("30..40;15x15,40x15,40x40,15x40"));
    CHECK_FALSE(plate->check_outside(0, 0));
}
