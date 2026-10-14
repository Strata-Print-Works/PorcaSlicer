#pragma once
#include "DesignTokens.hpp"
#include <wx/colour.h>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
class GLCanvas3D;
namespace DesignSystem {
using SpriteColors = std::pair<wxColour, wxColour>;
std::vector<SpriteColors> toolbar_palette(bool dark, bool gizmo);
std::string toolbar_icon_path(const std::string& original);
float toolbar_top_inset(const GLCanvas3D& canvas);
void color_toolbar_sprite(std::vector<unsigned char>& output, const std::vector<unsigned char>& glyph,
                          std::size_t size, const SpriteColors& colors);
}
}
