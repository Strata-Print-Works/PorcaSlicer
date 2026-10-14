#pragma once

#include "DesignTokens.hpp"
#include <wx/font.h>

namespace Slic3r::GUI::DesignSystem {
void register_fonts();
wxFont font(TypeStyle style);
// Existing controls retain their measured point sizes while adopting Figtree.
wxFont interface_font(double points, bool strong);
}
