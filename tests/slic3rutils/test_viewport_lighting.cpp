#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "slic3r/GUI/ViewportLighting.hpp"
#include "slic3r/GUI/SceneCache.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/3DBed.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include <limits>
#include <cstddef>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <array>
#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

TEST_CASE("Enhanced materials are confined to supported Prepare views", "[ViewportLighting]")
{
    CHECK(prepare_plastic_enabled(true, true, true, false, true));
    CHECK_FALSE(prepare_plastic_enabled(false, true, true, false, true));
    CHECK_FALSE(prepare_plastic_enabled(true, false, true, false, true));
    CHECK_FALSE(prepare_plastic_enabled(true, true, false, false, true));
    CHECK_FALSE(prepare_plastic_enabled(true, true, true, true, true));
    CHECK_FALSE(prepare_plastic_enabled(true, true, true, false, false));
}

TEST_CASE("Filament names and types select recognizable surface finishes", "[ViewportLighting]")
{
    struct Example { const char* type; const char* name; SurfaceFinish finish; };
    const std::array<Example, 11> examples{{
        {"PLA", "Bambu PLA Galaxy @BBL A1", SurfaceFinish::Galaxy},
        {"PLA", "Glitter PLA", SurfaceFinish::Galaxy},
        {"PLA", "Bambu PLA Silk+ @BBL H2D", SurfaceFinish::Silk},
        {"PA-CF", "Generic nylon", SurfaceFinish::CarbonFiber},
        {"PLA", "Carbon Fibre PLA", SurfaceFinish::CarbonFiber},
        {"PLA", "Carbon-fiber filled PLA", SurfaceFinish::CarbonFiber},
        {"PLA", "PLA Matte", SurfaceFinish::Matte},
        {"PLA", "PLA Woodfill", SurfaceFinish::Wood},
        {"PLA", "Wood-filled PLA", SurfaceFinish::Wood},
        {"PLA", "Generic PLA @BBL X1 Carbon", SurfaceFinish::Satin},
        {"PLA", "Calf figurine filament @Galaxy CF Silk printer", SurfaceFinish::Satin}
    }};
    for (const auto& example : examples) {
        DynamicPrintConfig config;
        config.set_key_value("filament_type", new ConfigOptionStrings({example.type}));
        config.set_key_value("filament_settings_id", new ConfigOptionStrings({example.name}));
        CHECK(filament_surface_finish(&config, 0) == example.finish);
    }
}

TEST_CASE("Each palette slot keeps its own finish across independent setups", "[ViewportLighting]")
{
    DynamicPrintConfig first;
    first.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA"}));
    first.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA Silk", "PLA Galaxy"}));
    DynamicPrintConfig second;
    second.set_key_value("filament_type", new ConfigOptionStrings({"PETG-CF", "PLA"}));
    second.set_key_value("filament_settings_id", new ConfigOptionStrings({"Generic PETG-CF", "PLA Wood"}));
    CHECK(filament_surface_finish(&first, 0) == SurfaceFinish::Silk);
    CHECK(filament_surface_finish(&first, 1) == SurfaceFinish::Galaxy);
    CHECK(filament_surface_finish(&second, 0) == SurfaceFinish::CarbonFiber);
    CHECK(filament_surface_finish(&second, 1) == SurfaceFinish::Wood);
    CHECK(filament_surface_finish(&first, 2) == SurfaceFinish::Satin);
    CHECK(filament_surface_finish(nullptr, 0) == SurfaceFinish::Satin);
    const auto before = filament_surface_signature(&first);
    first.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA Matte", "PLA Galaxy"}));
    CHECK(filament_surface_finish(&first, 0) == SurfaceFinish::Matte);
    SceneCache::Key capture;
    capture.render_settings_signature = before;
    auto changed = capture;
    changed.render_settings_signature = filament_surface_signature(&first);
    CHECK_FALSE(capture == changed);
    CHECK(filament_surface_finish(&second, 0) == SurfaceFinish::CarbonFiber);
}

TEST_CASE("Filament matching preserves the print setup", "[ViewportLighting]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_type", new ConfigOptionStrings({"PLA-CF"}));
    const auto original = config;
    CHECK(filament_surface_finish(&config, 0) == SurfaceFinish::CarbonFiber);
    CHECK(config.diff(original).empty());
}

TEST_CASE("Enhanced surfaces default to filament matching while realistic lighting stays opt-in", "[ViewportLighting]")
{
    AppConfig config;
    CHECK(config.get_bool(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE));
    CHECK_FALSE(config.get_bool(SETTING_OPENGL_REALISTIC_MODE));
    for (bool enabled : {false, true, false}) {
        config.set_bool(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE, enabled);
        config.set_defaults();
        CHECK(config.get_bool(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE) == enabled);
    }
}

TEST_CASE("Legacy surface choices migrate to automatic filament matching", "[ViewportLighting]")
{
    for (const char* surface : {"classic", "filament", "satin", "matte", "silk", "carbon_fiber", "galaxy", "wood", "future"}) {
        DYNAMIC_SECTION(surface) {
            AppConfig config;
            config.set(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE, surface);
            config.set_defaults();
            const bool enhanced = std::string(surface) != "classic" && std::string(surface) != "future";
            CHECK(config.get_bool(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE) == enhanced);
            config.set_defaults();
            CHECK(config.get_bool(SETTING_OPENGL_ENHANCED_MATERIAL_SURFACE) == enhanced);
            if (enhanced) {
                DynamicPrintConfig filament;
                filament.set_key_value("filament_type", new ConfigOptionStrings({"PLA-CF"}));
                CHECK(filament_surface_finish(&filament, 0) == SurfaceFinish::CarbonFiber);
            }
        }
    }
}

TEST_CASE("The shadow fit encloses moved models and their shadows on the floor", "[ViewportLighting]")
{
    const Vec3d light = Vec3d(-0.4, -0.6, 1.0).normalized();
    // Two beds far from the origin, with a tall part extending above both.
    const BoundingBoxf3 bounds(Vec3d(1400, -850, 0), Vec3d(2150, -300, 320));
    const auto fit = fit_workspace_shadow(bounds, light, 2048);
    REQUIRE(fit.has_value());
    const Transform3d vp = fit->projection * fit->view;
    for (unsigned int corner = 0; corner < 8; ++corner) {
        const Vec3d p((corner & 1) ? bounds.max.x() : bounds.min.x(),
                      (corner & 2) ? bounds.max.y() : bounds.min.y(),
                      (corner & 4) ? bounds.max.z() : bounds.min.z());
        const Vec3d floor = p - light * ((p.z() + 3.0) / light.z());
        for (const Vec3d& receiver : {p, floor}) {
            const Vec3d ndc = vp * receiver;
            CHECK(ndc.cwiseAbs().maxCoeff() <= 1.0);
            CHECK(receiver.x() >= fit->receiver_bounds[0]);
            CHECK(receiver.y() >= fit->receiver_bounds[1]);
            CHECK(receiver.x() <= fit->receiver_bounds[2]);
            CHECK(receiver.y() <= fit->receiver_bounds[3]);
        }
        CHECK_THAT(floor.z(), WithinAbs(-3.0, 1e-9));
    }
}

TEST_CASE("A stationary world light gives the same fit without a camera input", "[ViewportLighting]")
{
    const BoundingBoxf3 bounds(Vec3d(-20, -35, 0), Vec3d(30, 45, 80));
    const auto first = fit_workspace_shadow(bounds, Vec3d(-0.4, -0.6, 1), 2048);
    const auto next = fit_workspace_shadow(bounds, Vec3d(-0.4, -0.6, 1), 2048);
    REQUIRE(first.has_value());
    REQUIRE(next.has_value());
    CHECK(first->view.matrix().isApprox(next->view.matrix()));
    CHECK(first->projection.matrix().isApprox(next->projection.matrix()));
    CHECK(first->depth_bias > 0.0f);
}

TEST_CASE("Invalid shadow inputs keep the scene unshadowed", "[ViewportLighting]")
{
    const BoundingBoxf3 bounds(Vec3d(0, 0, 0), Vec3d(30, 40, 80));
    CHECK_FALSE(fit_workspace_shadow(BoundingBoxf3{}, Vec3d::UnitZ(), 2048));
    CHECK_FALSE(fit_workspace_shadow(bounds, Vec3d::Zero(), 2048));
    CHECK_FALSE(fit_workspace_shadow(bounds, Vec3d::UnitX(), 2048));
    CHECK_FALSE(fit_workspace_shadow(bounds, -Vec3d::UnitZ(), 2048));
    CHECK_FALSE(fit_workspace_shadow(bounds, Vec3d::UnitZ(), 0));
    CHECK_FALSE(fit_workspace_shadow(bounds, Vec3d::UnitZ(), 2048,
        std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("Scene captures cannot survive an appearance change", "[ViewportLighting]")
{
    SceneCache::Key before;
    SceneCache::Key after = before;
    REQUIRE(before == after);
    after.render_settings_signature = 1;
    CHECK_FALSE(before == after);
}

TEST_CASE("Replacing display geometry changes its identity even with the same bounds", "[ViewportLighting]")
{
    // These buffers are never uploaded; reset and destruction do not issue GL calls.
    GLModel model;
    auto geometry = [](float corner_y) {
        GLModel::Geometry data;
        data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
        data.add_vertex(Vec3f(0, 0, 0));
        data.add_vertex(Vec3f(20, corner_y, 0));
        data.add_vertex(Vec3f(0, 20, 0));
        data.add_triangle(0, 1, 2);
        return data;
    };
    model.init_from(geometry(0));
    const auto old_revision = model.geometry_revision();
    const BoundingBoxf3 old_bounds = model.get_bounding_box();
    model.reset();
    model.init_from(geometry(20));
    CHECK(model.geometry_revision() > old_revision);
    CHECK(model.get_bounding_box().min.isApprox(old_bounds.min));
    CHECK(model.get_bounding_box().max.isApprox(old_bounds.max));
    CHECK(model.vertices_count() == 3);
}

TEST_CASE("Moved concave bed receivers retain their outline and physical position", "[ViewportLighting]")
{
    ExPolygon outline;
    for (const Vec2d& p : {Vec2d(1000, 500), Vec2d(1020, 500), Vec2d(1020, 510),
                          Vec2d(1010, 510), Vec2d(1010, 520), Vec2d(1000, 520)})
        outline.contour.append({scale_(p.x()), scale_(p.y())});
    GLModel receiver;
    REQUIRE(init_model_from_poly(receiver, outline, -0.04f));
    const auto& geometry = receiver.get_geometry();
    double area = 0.0;
    for (size_t i = 0; i < geometry.indices.size(); i += 3) {
        const Vec3f a = geometry.extract_position_3(geometry.indices[i]);
        const Vec3f b = geometry.extract_position_3(geometry.indices[i + 1]);
        const Vec3f c = geometry.extract_position_3(geometry.indices[i + 2]);
        area += (b - a).cross(c - a).norm() * 0.5;
        const Vec3f centre = (a + b + c) / 3.0f;
        const bool in_missing_corner = centre.x() > 1010.0f && centre.y() > 510.0f;
        CHECK_FALSE(in_missing_corner);
        CHECK_THAT(a.z(), WithinAbs(-0.04, 1e-6));
    }
    CHECK_THAT(area, WithinAbs(300.0, 1e-6));
    CHECK_THAT(receiver.get_bounding_box().min.x(), WithinAbs(1000.0, 1e-6));
    CHECK_THAT(receiver.get_bounding_box().min.y(), WithinAbs(500.0, 1e-6));
}
