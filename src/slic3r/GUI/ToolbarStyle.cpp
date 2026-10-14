#include "ToolbarStyle.hpp"
#include "GLCanvas3D.hpp"
#include "libslic3r/Utils.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r::GUI::DesignSystem {
std::vector<SpriteColors> toolbar_palette(bool dark, bool gizmo)
{
    const auto color = [dark](ColorRole role) { return wxColour(color_hex(role, dark)); };
    const SpriteColors normal{color(ColorRole::InkMuted), wxColour(0,0,0,0)};
    const SpriteColors hover{color(ColorRole::Ink), color(ColorRole::SurfaceHover)};
    const SpriteColors selected{color(ColorRole::AccentInk), color(ColorRole::AccentSoft)};
    const SpriteColors disabled{color(ColorRole::InkDisabled), wxColour(0,0,0,0)};
    if (gizmo) return {normal, hover, selected, disabled, selected, normal};
    return {normal, selected, disabled, hover, selected, disabled, selected, normal};
}

std::string toolbar_icon_path(const std::string& original)
{
    static const std::map<std::string, std::string> icons{
        {"toolbar_open", "square-plus"}, {"toolbar_add_plate", "grid-3x3"},
        {"toolbar_orient", "axis-3d"}, {"toolbar_arrange", "layout-grid"},
        {"toolbar_more", "plus"}, {"toolbar_fewer", "minus"},
        {"toolbar_move", "move"}, {"toolbar_rotate", "rotate-3d"},
        {"toolbar_scale", "scaling"}, {"toolbar_flatten", "axis-3d"},
        {"toolbar_cut", "scissors"}, {"toolbar_text", "type"},
        {"toolbar_measure", "ruler"}, {"toolbar_assembly", "box"}
    };
    auto name = original.substr(original.find_last_of("/\\") + 1);
    const auto extension = name.rfind(".svg");
    if (extension == std::string::npos) return original;
    name.resize(extension);
    if (name.size() > 5 && name.compare(name.size() - 5, 5, "_dark") == 0) name.resize(name.size() - 5);
    const auto found = icons.find(name);
    return found == icons.end() ? original : resources_dir() + "/design-system/icons/" + found->second + ".svg";
}

float toolbar_top_inset(const GLCanvas3D& canvas)
{
    return canvas.get_canvas_type() == GLCanvas3D::CanvasView3D && !canvas.is_design_canvas() ? space_12 * canvas.get_scale() : 0.0f;
}

void color_toolbar_sprite(std::vector<unsigned char>& output, const std::vector<unsigned char>& glyph,
                          std::size_t size, const SpriteColors& colors)
{
    const auto bytes = (size + 1) * (size + 1) * 4;
    if (glyph.size() != bytes || output.size() != bytes || size == 0) return;
    std::fill(output.begin(), output.end(), 0);
    const float half = float(size) * 0.5f;
    const float radius = radius_md * float(size) / control_md;
    for (std::size_t y = 1; y < size; ++y) {
        for (std::size_t x = 1; x < size; ++x) {
            const auto offset = (y * (size + 1) + x) * 4;
            const float dx = std::max(std::abs(float(x) - 0.5f - half) - (half - radius), 0.0f);
            const float dy = std::max(std::abs(float(y) - 0.5f - half) - (half - radius), 0.0f);
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            const float ink = glyph[offset + 3] / 255.0f;
            const float ground = colors.second.Alpha() / 255.0f * coverage * (1.0f - ink);
            const float alpha = ink + ground;
            if (alpha == 0.0f) continue;
            const unsigned char foreground[]{colors.first.Red(), colors.first.Green(), colors.first.Blue()};
            const unsigned char background[]{colors.second.Red(), colors.second.Green(), colors.second.Blue()};
            for (std::size_t c = 0; c < 3; ++c)
                output[offset + c] = static_cast<unsigned char>(std::lround((foreground[c] * ink + background[c] * ground) / alpha));
            output[offset + 3] = static_cast<unsigned char>(std::lround(alpha * 255.0f));
        }
    }
}
} // namespace Slic3r::GUI::DesignSystem
