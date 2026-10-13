#include <catch2/catch_all.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include <set>
#include <cstddef>
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "test_utils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

TEST_CASE("Separating shared copies preserves placement and isolates filament edits", "[Model][PlatePrintSetup]")
{
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* original = model.add_object();
    auto* volume = original->add_volume(make_cube(20, 20, 20));
    original->config.set_key_value("extruder", new ConfigOptionInt(2));
    volume->config.set_key_value("extruder", new ConfigOptionInt(2));
    TriangleSelector paint(volume->mesh());
    paint.set_facet(0, EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set_data(paint.serialize());
    std::vector<Transform3d> transforms;
    for (size_t i = 0; i < 3; ++i) {
        auto* instance = original->add_instance();
        instance->set_offset(Vec3d(30 + 250 * i, 40, -2));
        instance->set_rotation(Vec3d(0, 0, 0.2 * i));
        transforms.push_back(instance->get_matrix());
    }
    const auto retained_id = original->instances[1]->id();
    REQUIRE_FALSE(model.separate_instances(0, {}));
    REQUIRE_FALSE(model.separate_instances(0, {0, 1, 2}));
    REQUIRE_FALSE(model.separate_instances(0, {3}));
    REQUIRE(model.objects.size() == 1);
    auto* detached = model.separate_instances(0, {0, 2});
    REQUIRE(detached);
    REQUIRE(model.objects.size() == 2);
    REQUIRE(original->instances.size() == 1);
    REQUIRE(detached->instances.size() == 2);
    REQUIRE(original->instances[0]->id() == retained_id);
    REQUIRE_THAT((original->instances[0]->get_matrix().matrix() - transforms[1].matrix()).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE_THAT((detached->instances[0]->get_matrix().matrix() - transforms[0].matrix()).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE_THAT((detached->instances[1]->get_matrix().matrix() - transforms[2].matrix()).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE(TriangleSelector::has_facets(detached->volumes[0]->mmu_segmentation_facets.get_data(), EnforcerBlockerType::Extruder2));
    detached->config.set_key_value("extruder", new ConfigOptionInt(1));
    detached->volumes[0]->update_extruder_count_when_delete_filament(1, 2, 1, {});
    REQUIRE(original->config.extruder() == 2);
    REQUIRE(original->volumes[0]->config.extruder() == 2);
    REQUIRE(TriangleSelector::has_facets(original->volumes[0]->mmu_segmentation_facets.get_data(), EnforcerBlockerType::Extruder2));
    REQUIRE_FALSE(TriangleSelector::has_facets(detached->volumes[0]->mmu_segmentation_facets.get_data(), EnforcerBlockerType::Extruder2));
}

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}

TEST_CASE("An object's raw mesh keeps the triangles of each part on its own vertices", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    TriangleMesh second = make_cube(10, 10, 10);
    second.translate(30, 0, 0);
    object->add_volume(std::move(second), ModelVolumeType::MODEL_PART, false);

    // Two separate cubes stay two closed components, one around each cube.
    const std::vector<indexed_triangle_set> parts = its_split(object->raw_indexed_triangle_set());
    REQUIRE(parts.size() == 2);
    std::vector<double> min_x;
    for (const indexed_triangle_set &part : parts) {
        CHECK(part.indices.size() == 12);
        min_x.push_back(bounding_box(part).min.x());
    }
    std::sort(min_x.begin(), min_x.end());
    CHECK_THAT(min_x.front(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(min_x.back(), Catch::Matchers::WithinAbs(30., 1e-4));
}

TEST_CASE("An instance added from another's scale, rotation and mirror matches it", "[Model]")
{
    // Add instance and Fill bed copy an instance this way.
    const int case_idx = GENERATE(0, 1);
    Transform3d trafo = Transform3d::Identity();
    if (case_idx == 0)
        trafo.linear() = Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitZ()).toRotationMatrix() * Vec3d(-1., 1., 1.).asDiagonal();
    else
        trafo.linear() << 4.4408921e-16,   0.819152044,    0.573576436,
                          1.0,            -4.4408921e-16, -1.11022302e-16,
                         -5.55111512e-17, -0.573576436,    0.819152044;
    CAPTURE(case_idx);

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 20, 30));
    ModelInstance *original = object->add_instance();
    original->set_transformation(Geometry::Transformation(trafo));
    const ModelInstance *copy = object->add_instance(original->get_offset(), original->get_scaling_factor(),
                                                     original->get_rotation(), original->get_mirror());
    CHECK(copy->get_matrix().linear().isApprox(original->get_matrix().linear(), 1e-9));
}
