#pragma once

#include <wx/colour.h>

namespace Slic3r::GUI::Branding {

inline constexpr const char* coral = "#FF7B7B";
inline constexpr const char* coral_hover = "#F16B70";
// Coral on cream needs a darker ink for text, thin icons and focus indicators.
inline constexpr const char* coral_ink = "#B43F48";
inline constexpr const char* charcoal = "#1E1E1E";
inline constexpr const char* cream = "#FAF7F2";
inline constexpr const char* selected_surface = "#48282A";
// A semantic key: cream on the light theme's ink, charcoal on dark coral.
inline constexpr const char* accent_foreground = "#FAF7EF";

// Native windows store resolved colours; custom widgets retain semantic keys.
// The role distinguishes cream backgrounds from cream header lettering.
wxColour window_color(const wxColour& color, bool background);

} // namespace Slic3r::GUI::Branding
