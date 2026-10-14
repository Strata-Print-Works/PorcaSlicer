#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace Slic3r {
class GLShaderProgram;
class DynamicPrintConfig;
namespace GUI {

// Stable shader IDs. These describe display response, never slicing settings.
enum class SurfaceFinish { Satin = 0, Matte = 1, Silk = 2, CarbonFiber = 3, Galaxy = 4, Wood = 5 };
SurfaceFinish filament_surface_finish(const DynamicPrintConfig* config, size_t filament);
size_t filament_surface_signature(const DynamicPrintConfig* config);

// Display policy only; unsupported modes keep the inherited renderer.
bool prepare_plastic_enabled(bool enhanced, bool realistic, bool prepare,
                             bool design, bool supported);

struct WorkspaceShadowFit
{
    Transform3d view{Transform3d::Identity()};
    Transform3d projection{Transform3d::Identity()};
    std::array<float, 4> receiver_bounds{};
    float depth_bias{0.0f};
};

// Z-up, millimetres. Include the casters' projection onto the workspace floor,
// without clamping it to whichever printer happens to be selected.
std::optional<WorkspaceShadowFit> fit_workspace_shadow(const BoundingBoxf3& casters,
    const Vec3d& direction_to_light, unsigned int map_size, double floor_z = -3.0);

struct ShadowRenderContext
{
    unsigned int texture{0};
    unsigned int size{0};
    Transform3d light_vp{Transform3d::Identity()};
    Vec3f light_direction_eye{Vec3f::UnitZ()};
    float depth_bias{0.0f};
    std::array<float, 4> receiver_bounds{};

    bool valid() const { return texture != 0 && size != 0; }
    void set_uniforms(GLShaderProgram& shader) const;
};

// Receiver draws must not leak a binding into another canvas sharing the context.
class ShadowTextureBinding
{
public:
    explicit ShadowTextureBinding(const ShadowRenderContext* shadow);
    ~ShadowTextureBinding();
    ShadowTextureBinding(const ShadowTextureBinding&) = delete;
    ShadowTextureBinding& operator=(const ShadowTextureBinding&) = delete;
private:
    bool m_bound{false};
    int m_active{0};
    int m_texture{0};
};

} // namespace GUI
} // namespace Slic3r
