#include "ViewportLighting.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <string>
#include <vector>
#include <boost/container_hash/hash.hpp>

namespace Slic3r::GUI {

namespace {
bool finish_word(const std::string& text, std::string_view word)
{
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
        if ((at == 0 || !letter(text[at - 1])) &&
            (at + word.size() == text.size() || !letter(text[at + word.size()]))) return true;
    return false;
}
}

SurfaceFinish filament_surface_finish(const DynamicPrintConfig* config, size_t filament)
{
    if (config == nullptr) return SurfaceFinish::Satin;
    std::string description;
    for (const char* key : {"filament_type", "filament_settings_id"}) {
        const auto* values = config->option<ConfigOptionStrings>(key);
        if (values && filament < values->values.size()) {
            // The printer suffix can contain terms such as X1 Carbon; it is not
            // evidence about the filament assigned to this region.
            const auto& name = values->values[filament];
            description += name.substr(0, name.find('@')) + " ";
        }
    }
    for (char& c : description) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '-' || c == '_') c = ' ';
    }
    if (finish_word(description, "galaxy") || finish_word(description, "glitter") || finish_word(description, "sparkle"))
        return SurfaceFinish::Galaxy;
    if (finish_word(description, "cf") || finish_word(description, "carbon fiber") || finish_word(description, "carbon fibre"))
        return SurfaceFinish::CarbonFiber;
    if (finish_word(description, "wood") || finish_word(description, "woodfill")) return SurfaceFinish::Wood;
    if (finish_word(description, "silk")) return SurfaceFinish::Silk;
    if (finish_word(description, "matte") || finish_word(description, "matt")) return SurfaceFinish::Matte;
    return SurfaceFinish::Satin;
}

size_t filament_surface_signature(const DynamicPrintConfig* config)
{
    size_t signature = 0;
    if (config)
        for (const char* key : {"filament_type", "filament_settings_id"}) {
            const auto* values = config->option<ConfigOptionStrings>(key);
            boost::hash_combine(signature, values ? values->values.size() : 0);
            if (values)
                for (const auto& value : values->values) boost::hash_combine(signature, value);
        }
    return signature;
}

bool prepare_plastic_enabled(bool enhanced, bool realistic, bool prepare,
                             bool design, bool supported)
{
    return enhanced && realistic && prepare && !design && supported;
}

std::optional<WorkspaceShadowFit> fit_workspace_shadow(const BoundingBoxf3& casters,
    const Vec3d& direction_to_light, unsigned int map_size, double floor_z)
{
    if (!casters.defined || !casters.min.allFinite() || !casters.max.allFinite() ||
        !direction_to_light.allFinite() || direction_to_light.norm() < 1e-9 ||
        map_size == 0 || !std::isfinite(floor_z))
        return std::nullopt;
    const Vec3d z = direction_to_light.normalized();
    // Near-horizontal light has unbounded floor reach and cannot give a useful fit.
    if (z.z() < 0.05)
        return std::nullopt;
    const Vec3d up = std::abs(z.z()) > 0.99 ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d x = up.cross(z).normalized();
    const Vec3d y = z.cross(x).normalized();
    Vec3d low = Vec3d::Constant(std::numeric_limits<double>::max());
    Vec3d high = -low;
    Vec2d reach_low = low.head<2>();
    Vec2d reach_high = high.head<2>();
    auto enclose = [&](const Vec3d& p) {
        const Vec3d lp(x.dot(p), y.dot(p), z.dot(p));
        low = low.cwiseMin(lp);
        high = high.cwiseMax(lp);
        reach_low = reach_low.cwiseMin(p.head<2>());
        reach_high = reach_high.cwiseMax(p.head<2>());
    };
    for (unsigned int i = 0; i < 8; ++i) {
        const Vec3d p((i & 1) ? casters.max.x() : casters.min.x(),
                      (i & 2) ? casters.max.y() : casters.min.y(),
                      (i & 4) ? casters.max.z() : casters.min.z());
        enclose(p);
        if (p.z() >= floor_z)
            enclose(p - z * ((p.z() - floor_z) / z.z()));
    }
    const Vec2d half = ((high - low).head<2>() * 0.51).cwiseMax(Vec2d::Constant(1.0));
    const Vec2d texel = half * 2.0 / map_size;
    // Leave padding for PCF and round only the fit's centre, never the geometry.
    const Vec2d padded_half = half + texel * 3.0;
    Vec2d centre = (low + high).head<2>() * 0.5;
    for (int i = 0; i < 2; ++i)
        centre[i] = std::round(centre[i] / texel[i]) * texel[i];
    const double range = high.z() - low.z();
    const double margin = std::max(1.0, range * 0.05);
    const Vec3d eye = x * centre.x() + y * centre.y() + z * (high.z() + margin);
    WorkspaceShadowFit fit;
    fit.view.matrix().block<1, 3>(0, 0) = x.transpose();
    fit.view.matrix().block<1, 3>(1, 0) = y.transpose();
    fit.view.matrix().block<1, 3>(2, 0) = z.transpose();
    fit.view.matrix()(0, 3) = -x.dot(eye);
    fit.view.matrix()(1, 3) = -y.dot(eye);
    fit.view.matrix()(2, 3) = -z.dot(eye);
    const double near_z = margin * 0.5;
    const double far_z = range + margin * 1.5;
    fit.projection.matrix()(0, 0) = 1.0 / padded_half.x();
    fit.projection.matrix()(1, 1) = 1.0 / padded_half.y();
    fit.projection.matrix()(2, 2) = -2.0 / (far_z - near_z);
    fit.projection.matrix()(2, 3) = -(far_z + near_z) / (far_z - near_z);
    const double pad = texel.maxCoeff() * 3.0 / z.z();
    fit.receiver_bounds = {float(reach_low.x() - pad), float(reach_low.y() - pad),
                           float(reach_high.x() + pad), float(reach_high.y() + pad)};
    fit.depth_bias = float(0.06 / (far_z - near_z));
    if (!fit.view.matrix().allFinite() || !fit.projection.matrix().allFinite())
        return std::nullopt;
    return fit;
}

} // namespace Slic3r::GUI
