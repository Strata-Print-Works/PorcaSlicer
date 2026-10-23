#include "Typography.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/fontenum.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <array>
#include <string>
#ifdef __linux__
#include <fontconfig/fontconfig.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#endif

namespace Slic3r::GUI::DesignSystem {
namespace {
wxFont make_font(const char* family, double points, int weight)
{
    wxFont result = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    if (wxFontEnumerator::IsValidFacename(wxString::FromUTF8(family)))
        result.SetFaceName(wxString::FromUTF8(family));
    result.SetFractionalPointSize(points);
    result.SetNumericWeight(weight);
    return result;
}
}

void register_fonts()
{
    // Register together, before Label caches and native drawing contexts exist.
    static bool registered = false;
    if (registered) return;
    registered = true;
    struct Family { const char* name; const char* file; };
    const std::array<Family, 9> files{{
        {"Figtree", "Figtree-400.ttf"}, {"Figtree", "Figtree-500.ttf"},
        {"Figtree", "Figtree-600.ttf"}, {"Figtree", "Figtree-650.ttf"}, {"Figtree", "Figtree-700.ttf"},
        {"Nunito", "Nunito-800.ttf"}, {"Nunito", "Nunito-900.ttf"},
        {"JetBrains Mono", "JetBrainsMono-400.ttf"}, {"JetBrains Mono", "JetBrainsMono-500.ttf"}
    }};
#ifdef __linux__
    // Use fontconfig directly: wx enumeration must happen after registration,
    // and already-installed families must not invalidate Pango's font map.
    const auto installed = [](const char* name) {
        FcPattern* query = FcPatternCreate();
        if (!query) return false;
        FcPatternAddString(query, FC_FAMILY, reinterpret_cast<const FcChar8*>(name));
        FcResult status;
        FcPattern* match = FcFontMatch(nullptr, query, &status);
        FcChar8* family = nullptr;
        const bool found = match && FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch &&
            std::string(reinterpret_cast<const char*>(family)) == name;
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(query);
        return found;
    };
    const bool figtree = installed("Figtree");
    const bool nunito = installed("Nunito");
    const bool mono = installed("JetBrains Mono");
#endif
    for (const auto& file : files) {
        const std::string path = resources_dir() + "/design-system/fonts/" + file.file;
#ifdef __APPLE__
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
            reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false);
        if (url) {
            CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, nullptr);
            CFRelease(url);
        }
#else
#ifdef __linux__
        const std::string family(file.name);
        if ((family == "Figtree" && figtree) || (family == "Nunito" && nunito) || (family == "JetBrains Mono" && mono))
            continue;
#endif
        wxFont::AddPrivateFont(wxString::FromUTF8(path));
#endif
    }
    wxFontEnumerator::InvalidateCache();
}

double font_points(double logical_pixels)
{
#ifdef __APPLE__
    return logical_pixels;
#else
    return logical_pixels * 72.0 / 96.0;
#endif
}

wxFont font(TypeStyle style)
{
    const auto& token = type_token(style);
    return make_font(token.family, font_points(token.size), token.weight);
}

wxFont interface_font(double points, bool strong)
{
    return make_font("Figtree", points, strong ? type_token(TypeStyle::BodyStrong).weight : type_token(TypeStyle::Body).weight);
}
} // namespace Slic3r::GUI::DesignSystem
