#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {
Camera camera_for_zoom(Camera::EType type = Camera::EType::Perspective)
{
    Camera camera;
    camera.set_type(type);
    camera.look_at(Vec3d(0., 0., 1000.), Vec3d::Zero(), Vec3d::UnitY());
    camera.set_viewport(0, 0, 1200, 800);
    camera.set_scene_box(BoundingBoxf3(Vec3d(-1000., -1000., 0.), Vec3d(1000., 1000., 100.)));
    return camera;
}

const BoundingBoxf3 scene(Vec3d(-200., -200., 0.), Vec3d(200., 200., 100.));
}

TEST_CASE("Fixed-lens perspective zoom moves the eye and preserves target scale", "[Camera]")
{
    Camera camera = camera_for_zoom();
    camera.set_zoom(1.0);
    camera.apply_projection(scene, -1., -1., true);
    const double distance = camera.get_distance();
    const Vec3d target = camera.get_target();

    const double zoom = GENERATE(0.5, 1.0, 2.0, 10.0);
    camera.set_zoom(zoom);
    camera.apply_projection(scene, -1., -1., true);
    CHECK_THAT(camera.get_fov(), WithinAbs(45.0, 1e-9));
    CHECK_THAT(camera.get_distance(), WithinAbs(distance / zoom, 1e-9));
    CHECK_THAT(camera.get_zoom(), WithinAbs(zoom, 1e-9));
    CHECK_THAT((camera.get_target() - target).norm(), WithinAbs(0.0, 1e-9));
    // A unit at the target plane occupies exactly zoom pixels vertically.
    const double pixels_per_unit = camera.get_projection_matrix()(1, 1) * 400.0 / camera.get_distance();
    CHECK_THAT(pixels_per_unit, WithinAbs(zoom, 1e-9));
    CHECK(camera.get_near_z() > 0.0);
    CHECK(camera.get_far_z() > camera.get_near_z());
}

TEST_CASE("Fixed-lens zoom can inspect objects inside the overall workspace bounds", "[Camera]")
{
    Camera camera = camera_for_zoom();
    camera.set_zoom(20.0);
    camera.apply_projection(scene, -1., -1., true);
    CHECK(camera.get_position().z() < scene.max.z());
    CHECK(camera.get_near_z() < camera.get_distance());
    CHECK_THAT(camera.get_fov(), WithinAbs(45.0, 1e-9));
    CHECK_THAT(camera.get_zoom(), WithinAbs(20.0, 1e-9));
}

TEST_CASE("Fixed-lens zoom survives a zero-height viewport", "[Camera]")
{
    Camera camera = camera_for_zoom();
    camera.set_zoom(2.0);
    camera.apply_projection(scene, -1., -1., true);
    const double distance = camera.get_distance();

    camera.set_viewport(0, 0, 1200, 0);
    camera.apply_projection(scene, -1., -1., true);
    CHECK_THAT(camera.get_zoom(), WithinAbs(2.0, 1e-9));
    CHECK_THAT(camera.get_distance(), WithinAbs(distance, 1e-9));

    camera.set_viewport(0, 0, 1200, 800);
    camera.apply_projection(scene, -1., -1., true);
    CHECK_THAT(camera.get_zoom(), WithinAbs(2.0, 1e-9));
    CHECK_THAT(camera.get_distance(), WithinAbs(distance, 1e-9));
}

TEST_CASE("Only a wide workspace zooms out beyond the ordinary scene limit", "[Camera]")
{
    Camera camera = camera_for_zoom();
    const double ordinary = camera.min_zoom();
    REQUIRE(ordinary > 0.);
    // Ordinary projects keep Orca's limit.
    camera.set_zoom(ordinary * 0.5);
    CHECK_THAT(camera.get_zoom(), WithinAbs(ordinary, 1e-12));
    // The workspace limit is the same fit zoom scaled by its own factor.
    camera.set_min_zoom_factor(Camera::WorkspaceMinZoomFactor);
    const double workspace = ordinary * Camera::WorkspaceMinZoomFactor / Camera::DefaultMinZoomFactor;
    CHECK_THAT(camera.min_zoom(), WithinAbs(workspace, 1e-12));
    camera.set_zoom(ordinary * 0.5);
    CHECK_THAT(camera.get_zoom(), WithinAbs(ordinary * 0.5, 1e-12));
    camera.set_zoom(workspace * 0.5);
    CHECK_THAT(camera.get_zoom(), WithinAbs(workspace, 1e-12));
}

TEST_CASE("Existing perspective zoom changes the lens without moving the eye", "[Camera]")
{
    Camera camera = camera_for_zoom();
    camera.set_zoom(1.0);
    camera.apply_projection(scene);
    const double distance = camera.get_distance();
    const double fov = camera.get_fov();
    camera.set_zoom(0.5);
    camera.apply_projection(scene);
    CHECK_THAT(camera.get_distance(), WithinAbs(distance, 1e-9));
    CHECK(camera.get_fov() > fov);
}

TEST_CASE("Fixed-lens zoom keeps screen picking aligned after zoom and resize", "[Camera]")
{
    Camera camera = camera_for_zoom();
    const double zoom = GENERATE(0.5, 2.0, 10.0);
    const int height = GENERATE(600, 1200);
    camera.set_viewport(0, 0, 1200, height);
    camera.set_zoom(zoom);
    camera.apply_projection(scene, -1., -1., true);
    const Vec3d point(20., 15., 0.);
    const Point screen = CameraUtils::project(camera, point);
    const Vec2d hit = CameraUtils::get_z0_position(camera, screen.cast<double>());
    CHECK_THAT((hit - point.head<2>()).norm(), WithinAbs(0.0, 1.0 / zoom));
    CHECK_THAT(camera.get_fov(), WithinAbs(45.0, 1e-9));
}

TEST_CASE("Orthogonal views ignore the perspective zoom preference", "[Camera]")
{
    Camera camera = camera_for_zoom(Camera::EType::Ortho);
    camera.set_zoom(2.0);
    camera.apply_projection(scene);
    const double distance = camera.get_distance();
    const Transform3d projection = camera.get_projection_matrix();
    camera.apply_projection(scene, -1., -1., true);
    CHECK_THAT(camera.get_distance(), WithinAbs(distance, 1e-9));
    CHECK(camera.get_projection_matrix().isApprox(projection));
}

TEST_CASE("Fixed-lens zoom stays optional and retains an explicit choice", "[Camera]")
{
    AppConfig config;
    CHECK_FALSE(config.get_bool("fixed_perspective_zoom"));
    config.set_bool("fixed_perspective_zoom", true);
    config.set_defaults();
    CHECK(config.get_bool("fixed_perspective_zoom"));
}

TEST_CASE("Preview inspectors preserve model proportions and screen picking", "[Camera][PreviewLayout]")
{
    const auto type = GENERATE(Camera::EType::Perspective, Camera::EType::Ortho);
    Camera camera = camera_for_zoom(type);
    camera.set_zoom(1.0);
    camera.apply_projection(scene);
    const Vec3d target = camera.get_target();
    const Vec3d eye = camera.get_position();
    const auto viewport = camera.get_viewport();
    const auto original = camera.get_projection_matrix();
    const double bottom = GENERATE(36., 260.);
    camera.apply_screen_insets(220., 350., bottom);
    const auto& projection = camera.get_projection_matrix();
    CHECK_THAT(projection(0, 0) / original(0, 0), WithinAbs(projection(1, 1) / original(1, 1), 1e-9));
    // Reserving screen space must retain the projective row and the depth range.
    // An affine Transform multiplication silently replaces the perspective W row,
    // even though projecting and unprojecting with that same broken matrix agree.
    CHECK(projection.matrix().row(2).isApprox(original.matrix().row(2)));
    CHECK(projection.matrix().row(3).isApprox(original.matrix().row(3)));
    const Vec4d clip = projection.matrix() * camera.get_view_matrix().matrix() * Vec4d(target.x(), target.y(), target.z(), 1.);
    REQUIRE(clip.w() > 0.);
    CHECK(std::abs(clip.x()) < clip.w());
    CHECK(std::abs(clip.y()) < clip.w());
    CHECK(std::abs(clip.z()) < clip.w());
    CHECK(camera.get_viewport() == viewport);
    CHECK(camera.get_target().isApprox(target));
    CHECK(camera.get_position().isApprox(eye));
    const Vec3d point(21., -17., 0.);
    const Point screen = CameraUtils::project(camera, point);
    const Vec2d hit = CameraUtils::get_z0_position(camera, screen.cast<double>());
    CHECK_THAT((hit - point.head<2>()).norm(), WithinAbs(0., 4.));
    // Recomputing the base projection removes the screen reservation, with no
    // accumulated scaling after repeated expansion/collapse.
    camera.apply_projection(scene);
    CHECK(camera.get_projection_matrix().isApprox(original));
}

TEST_CASE("Preview detects slices outside the inherited workspace camera", "[Camera][PreviewLayout]")
{
    Camera camera = camera_for_zoom(GENERATE(Camera::EType::Perspective, Camera::EType::Ortho));
    camera.set_zoom(2.0);
    camera.apply_projection(scene);
    camera.apply_screen_insets(232., 495., 96.);
    CHECK(camera.is_box_visible(scene));
    CHECK_FALSE(camera.is_box_visible(BoundingBoxf3()));
    for (const Vec3d& offset : {Vec3d(-5000., 0., 0.), Vec3d(5000., 0., 0.),
                               Vec3d(0., -5000., 0.), Vec3d(0., 5000., 0.), Vec3d(0., 0., 5000.)}) {
        const BoundingBoxf3 distant(scene.min + offset, scene.max + offset);
        CHECK_FALSE(camera.is_box_visible(distant));
    }
    // A box can intersect the view even when none of its corners are onscreen.
    CHECK(camera.is_box_visible(BoundingBoxf3(Vec3d(-10000., -10000., 0.), Vec3d(10000., 10000., 100.))));

    const BoundingBoxf3 slice(Vec3d(4900., -100., 0.), Vec3d(5100., 100., 100.));
    CHECK_FALSE(camera.is_box_visible(slice));
    camera.zoom_to_box(slice);
    camera.apply_projection(slice);
    camera.apply_screen_insets(232., 495., 96.);
    CHECK(camera.is_box_visible(slice));
}
