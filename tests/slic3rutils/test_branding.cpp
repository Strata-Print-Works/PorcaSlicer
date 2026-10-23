#include <catch2/catch_test_macros.hpp>

#include <wx/colour.h>

#include "slic3r/GUI/Branding.hpp"
#include "slic3r/GUI/DesignTokens.hpp"
#include "slic3r/GUI/ToolbarStyle.hpp"
#include <vector>
#include <string>
#include <utility>
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace Brand = Slic3r::GUI::Branding;

namespace {
struct RestoreLightTheme
{
    ~RestoreLightTheme() { StateColor::SetDarkMode(false); }
};
}

TEST_CASE("Native surfaces keep their palette across repeated theme changes", "[Branding]")
{
    RestoreLightTheme restore;
    for (const char* semantic : {"#FFFFFF", "#FEFFFF", "#F8F8F8", "#F1F1F1", "#DFDFDF", "#D4D4D4", "#BFE1DE"}) {
        DYNAMIC_SECTION(semantic) {
            wxColour surface(semantic);
            for (bool dark : {false, false, true, true, false, true, false}) {
                StateColor::SetDarkMode(dark);
                surface = Brand::window_color(surface, true);
                CHECK(surface == StateColor::darkModeColorFor(wxColour(semantic)));
            }
        }
    }
}

TEST_CASE("Primary actions keep coral backgrounds in both themes", "[Branding]")
{
    RestoreLightTheme restore;
    for (bool dark : {false, true, false}) {
        StateColor::SetDarkMode(dark);
        CHECK(Brand::window_color(wxColour(Brand::coral), true) == wxColour(Brand::coral));
        CHECK(Brand::window_color(wxColour(Brand::charcoal), true) == wxColour(Brand::charcoal));
        CHECK(StateColor::darkModeColorFor(wxColour(Brand::accent_foreground)) == wxColour(dark ? Brand::charcoal : Brand::cream));
    }
}

TEST_CASE("Native text stays readable through repeated theme changes", "[Branding]")
{
    RestoreLightTheme restore;
    wxColour text("#000000");
    for (bool dark : {false, false, true, true, false, false, true, false}) {
        StateColor::SetDarkMode(dark);
        text = Brand::window_color(text, false);
        CHECK(text == StateColor::darkModeColorFor(wxColour("#000000")));
    }

    StateColor::SetDarkMode(false);
    CHECK(Brand::window_color(wxColour(Brand::charcoal), false) == wxColour(Brand::charcoal));
    CHECK(StateColor::GetColorDifference(wxColour(Brand::coral), wxColour(Brand::charcoal)) > 30);
}

namespace DS = Slic3r::GUI::DesignSystem;

TEST_CASE("Resolved sidebar palettes preserve exact colors without changing legacy mapping", "[DesignSystem]")
{
    RestoreLightTheme restore;
    StateColor legacy(std::make_pair(wxColour("#FF7B7B"), static_cast<int>(StateColor::Focused)),
                      std::make_pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal)));
    auto resolved = StateColor::resolved(legacy);
    CHECK(resolved != legacy);
    for (bool dark : {false, true, false}) {
        StateColor::SetDarkMode(dark);
        CHECK(resolved.colorForStates(StateColor::Normal) == wxColour("#FFFFFF"));
        CHECK(resolved.colorForStates(StateColor::Focused) == wxColour("#FF7B7B"));
        CHECK(legacy.colorForStates(StateColor::Normal) == StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    }
}

TEST_CASE("Porca tokens retain distinct light and dark roles", "[DesignSystem]")
{
    CHECK(std::string(DS::color_hex(DS::ColorRole::Primary, false)) == "#FF7B7B");
    CHECK(std::string(DS::color_hex(DS::ColorRole::OnPrimary, true)) == "#1E1E1E");
    CHECK(std::string(DS::color_hex(DS::ColorRole::Field, false)) == "#FFFFFF");
    CHECK(std::string(DS::color_hex(DS::ColorRole::Field, true)) == "#131312");
    CHECK(DS::color_alpha(DS::ColorRole::Scrim, true) == 158);
    CHECK(DS::color_alpha(DS::ColorRole::Scrim, false) == 92);
    CHECK(DS::control_md == 28);
    CHECK(DS::icon_lg == 20);
    CHECK(std::string(DS::type_token(DS::TypeStyle::Body).family) == "Figtree");
    CHECK(std::string(DS::type_token(DS::TypeStyle::Display).family) == "Nunito");
}

TEST_CASE("Toolbar state sprites preserve glyph coverage and transparent gutters", "[DesignSystem]")
{
    const std::size_t size = DS::control_md;
    const std::size_t bytes = (size + 1) * (size + 1) * 4;
    std::vector<unsigned char> glyph(bytes, 0), output(bytes, 0);
    const std::size_t centre = (15 * (size + 1) + 15) * 4;
    glyph[centre + 3] = 255;
    for (bool dark : {false, true}) {
        const auto palette = DS::toolbar_palette(dark, true);
        DS::color_toolbar_sprite(output, glyph, size, palette[2]);
        CHECK(output[centre] == palette[2].first.Red());
        CHECK(output[centre + 3] == 255);
        CHECK(output[3] == 0);
        CHECK(output[(15 * (size + 1) + size) * 4 + 3] == 0);
        CHECK(output[(size * (size + 1) + 15) * 4 + 3] == 0);
        const std::size_t ground = (15 * (size + 1) + 5) * 4;
        CHECK(output[ground] == palette[2].second.Red());
        CHECK(output[ground + 3] == 255);
        DS::color_toolbar_sprite(output, glyph, size, palette[0]);
        CHECK(output[ground + 3] == 0);
        CHECK(output[centre + 3] == 255);
    }
}
