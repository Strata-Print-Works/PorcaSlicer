#pragma once

#include "DesignTokens.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r::GUI::WorkspaceStyle {

// Display geometry only. The printable surface and every object's Z remain unchanged.
inline constexpr double sheet_thickness = 1.0;
inline constexpr double sheet_top_z = -0.04;
inline constexpr float floor_z = float(sheet_top_z - sheet_thickness);
inline constexpr float chrome_z = floor_z + 0.05f;

// A stage's title bar sits on the plane directly above the frame's rear edge.
inline constexpr double stage_title_height = 44.0;
// Height of the stage name drawn inside the title bar.
inline constexpr double stage_title_text_height = 20.0;

// The frame plus the title bar above it: the area other stages must keep clear of.
inline BoundingBoxf stage_display_area(const BoundingBoxf& frame)
{
    return frame.defined ? BoundingBoxf(frame.min, frame.max + Vec2d(0.0, stage_title_height)) : BoundingBoxf{};
}

// Workspace rectangles of one stage's title bar and controls. Rendering and
// pointer hit tests both use this, so a drawn control is always where it is picked.
struct StageLayout
{
    BoundingBoxf bar;     // whole title bar
    Vec2d text_origin;    // lower-left corner of the stage name
    BoundingBoxf title;   // grip and name: drags the stage
    BoundingBoxf rename;
    BoundingBoxf actions;
    // Resize grips; bit 0 of the index selects the right side, bit 1 the rear.
    std::array<BoundingBoxf, 4> corners;
};

// text_width is the drawn stage name's width; the bar grows with it up to the frame width.
inline StageLayout stage_layout(const BoundingBoxf& frame, double text_width)
{
    StageLayout layout;
    const double width = std::min(frame.size().x(), std::max(220., text_width + 140.));
    layout.bar = BoundingBoxf(Vec2d(frame.min.x(), frame.max.y()),
        Vec2d(frame.min.x() + width, frame.max.y() + stage_title_height));
    const BoundingBoxf& bar = layout.bar;
    layout.text_origin = Vec2d(frame.min.x() + 48., frame.max.y() + (stage_title_height - stage_title_text_height) / 2.);
    layout.title = BoundingBoxf(bar.min + Vec2d(6., 4.), Vec2d(bar.max.x() - 82., bar.max.y() - 4.));
    layout.rename = BoundingBoxf(Vec2d(bar.max.x() - 76., bar.min.y() + 6.), Vec2d(bar.max.x() - 44., bar.max.y() - 6.));
    layout.actions = BoundingBoxf(Vec2d(bar.max.x() - 38., bar.min.y() + 6.), bar.max - Vec2d(6., 6.));
    for (int corner = 0; corner < 4; ++corner) {
        const Vec2d center(corner & 1 ? frame.max.x() - 10. : frame.min.x() + 10.,
            corner & 2 ? frame.max.y() - 10. : frame.min.y() + 10.);
        layout.corners[corner] = BoundingBoxf(center - Vec2d(10., 10.), center + Vec2d(10., 10.));
    }
    return layout;
}

inline ColorRGBA color(DesignSystem::ColorRole role, bool dark)
{
    ColorRGBA result;
    decode_color(DesignSystem::color_hex(role, dark), result);
    return result;
}

inline ColorRGBA bed_color(bool dark, bool selected)
{
    using DesignSystem::ColorRole;
    return color(selected ? (dark ? ColorRole::LineControl : ColorRole::Plate) :
        (dark ? ColorRole::Plate : ColorRole::InkMuted), dark);
}

inline Transform3d sheet_transform(const Vec3d& offset, const BoundingBoxf3& bounds)
{
    Transform3d result = Transform3d::Identity();
    result.translation() = offset;
    if (!bounds.defined) return result;
    const double height = bounds.size().z();
    if (std::isfinite(height) && height > 1e-6) {
        result.linear()(2, 2) = sheet_thickness / height;
        result.translation().z() = sheet_top_z - bounds.max.z() * result.linear()(2, 2);
    }
    return result;
}

} // namespace Slic3r::GUI::WorkspaceStyle
