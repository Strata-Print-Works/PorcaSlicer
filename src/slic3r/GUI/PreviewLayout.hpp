#pragma once

#include <algorithm>

namespace Slic3r::GUI {

// One allocation for the camera, navigation, inspector, sliders and code drawer.
// Sizes are physical pixels, like the canvas and ImGui mouse coordinates.
struct PreviewLayout
{
    float navigation_width;
    float inspector_width;
    float center_left;
    float center_right;
    float scene_bottom;
    float drawer_height;

    float center_width() const { return center_right - center_left; }
};

inline PreviewLayout preview_layout(float width, float height, float scale, bool navigation, float drawer_height)
{
    width = std::max(0.f, width);
    height = std::max(0.f, height);
    const float left = navigation ? std::min(236.f * scale, width * .24f) : 0.f;
    const float right = std::min(344.f * scale, width * .35f);
    const float drawer = std::clamp(drawer_height, 0.f, height * .45f);
    return {left, right, left, width - right, height - drawer, drawer};
}

} // namespace Slic3r::GUI
