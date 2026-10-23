#include "LiveToolbarStyle.hpp"
#include "DesignTokens.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::GUI::DesignSystem {
namespace {
constexpr std::size_t max_file_size = 32 * 1024;
using Json = nlohmann::json;

int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
}

bool parse_color(std::string_view text, ToolbarColor& color)
{
    if (text.size() != 7 || text[0] != '#') return false;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int high = hex_digit(text[1 + channel * 2]);
        const int low = hex_digit(text[2 + channel * 2]);
        if (high < 0 || low < 0) return false;
        color[channel] = static_cast<unsigned char>(high * 16 + low);
    }
    color[3] = 255;
    return true;
}

ToolbarThemeColors default_colors(bool dark)
{
    const auto color = [dark](ColorRole role) {
        ToolbarColor result{};
        parse_color(color_hex(role, dark), result);
        return result;
    };
    return {color(ColorRole::Surface), color(ColorRole::SurfaceHover),
            color(ColorRole::Ink), color(ColorRole::InkMuted), color(ColorRole::InkDisabled),
            color(ColorRole::AccentInk), color(ColorRole::AccentSoft)};
}

bool allowed_keys(const Json& object, std::initializer_list<std::string_view> keys, std::string& error)
{
    if (!object.is_object()) { error = "Expected an object"; return false; }
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        bool found = false;
        for (const auto key : keys) if (entry.key() == key) { found = true; break; }
        if (!found) { error = "Unknown style field: " + entry.key(); return false; }
    }
    return true;
}

bool metric(const Json& object, const char* name, float minimum, float maximum, float& value, std::string& error)
{
    const auto found = object.find(name);
    if (found == object.end()) return true;
    if (!found->is_number()) { error = std::string(name) + " must be a number"; return false; }
    const double number = found->get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        error = std::string(name) + " is outside its supported range";
        return false;
    }
    value = static_cast<float>(number);
    return true;
}

bool theme_colors(const Json& object, ToolbarThemeColors& colors, std::string& error)
{
    if (!allowed_keys(object, {"surface", "surface_hover", "ink", "ink_muted", "ink_disabled", "accent_ink", "accent_soft"}, error)) return false;
    const std::pair<const char*, ToolbarColor*> fields[]{
        {"surface", &colors.surface}, {"surface_hover", &colors.surface_hover},
        {"ink", &colors.ink}, {"ink_muted", &colors.ink_muted}, {"ink_disabled", &colors.ink_disabled},
        {"accent_ink", &colors.accent_ink}, {"accent_soft", &colors.accent_soft}
    };
    for (const auto& field : fields) {
        const auto found = object.find(field.first);
        if (found == object.end()) continue;
        if (!found->is_string() || !parse_color(found->get_ref<const std::string&>(), *field.second)) {
            error = std::string(field.first) + " must be a #RRGGBB color";
            return false;
        }
    }
    return true;
}
} // namespace

bool ToolbarThemeColors::operator==(const ToolbarThemeColors& other) const
{
    return std::tie(surface, surface_hover, ink, ink_muted, ink_disabled, accent_ink, accent_soft) ==
           std::tie(other.surface, other.surface_hover, other.ink, other.ink_muted, other.ink_disabled, other.accent_ink, other.accent_soft);
}

ToolbarStyleSnapshot::ToolbarStyleSnapshot() : dark(default_colors(true)), light(default_colors(false)) {}

bool ToolbarStyleSnapshot::operator==(const ToolbarStyleSnapshot& other) const
{
    return std::tie(control_size, icon_size, gap, border, top_inset, corner_radius, control_radius, dark, light) ==
           std::tie(other.control_size, other.icon_size, other.gap, other.border, other.top_inset, other.corner_radius, other.control_radius, other.dark, other.light);
}

bool parse_toolbar_style(std::string_view content, ToolbarStyleSnapshot& output, std::string& error)
{
    error.clear();
    if (content.size() > max_file_size) { error = "Style file exceeds 32 KiB"; return false; }
    try {
        bool too_deep = false;
        const auto parsed = Json::parse(content.begin(), content.end(), [&too_deep](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) too_deep = true;
            return depth <= 8;
        });
        if (too_deep) { error = "Style file is nested too deeply"; return false; }
        if (!allowed_keys(parsed, {"version", "metrics", "themes"}, error)) return false;
        const auto version = parsed.find("version");
        if (version == parsed.end() || !version->is_number_integer() || *version != 1) {
            error = "Style file requires integer version 1";
            return false;
        }
        ToolbarStyleSnapshot next;
        if (const auto metrics = parsed.find("metrics"); metrics != parsed.end()) {
            if (!allowed_keys(*metrics, {"control_size", "icon_size", "gap", "border", "top_inset", "corner_radius", "control_radius"}, error)) return false;
            if (!metric(*metrics, "control_size", 24, 64, next.control_size, error) ||
                !metric(*metrics, "icon_size", 12, 48, next.icon_size, error) ||
                !metric(*metrics, "gap", 0, 24, next.gap, error) ||
                !metric(*metrics, "border", 0, 24, next.border, error) ||
                !metric(*metrics, "top_inset", 0, 64, next.top_inset, error) ||
                !metric(*metrics, "corner_radius", 0, 16, next.corner_radius, error) ||
                !metric(*metrics, "control_radius", 0, 24, next.control_radius, error)) return false;
            if (next.icon_size > next.control_size || next.control_radius > next.control_size * 0.5f) {
                error = "Icon size and control radius must fit within the control";
                return false;
            }
        }
        if (const auto themes = parsed.find("themes"); themes != parsed.end()) {
            if (!allowed_keys(*themes, {"dark", "light"}, error)) return false;
            if (const auto dark = themes->find("dark"); dark != themes->end() && !theme_colors(*dark, next.dark, error)) return false;
            if (const auto light = themes->find("light"); light != themes->end() && !theme_colors(*light, next.light, error)) return false;
        }
        output = next;
        return true;
    } catch (const Json::exception&) {
        error = "Style file contains invalid JSON";
        return false;
    }
}

std::vector<unsigned char> toolbar_surface_pixels(const ToolbarStyleSnapshot& style, bool dark)
{
    constexpr int width = 192;
    constexpr int blur_radius = 48;
    constexpr float sigma = 16.0f;
    const float radius = std::clamp(style.corner_radius, 0.0f, 16.0f);
    const auto coverage = [radius](float x, float y) {
        const float dx = std::abs(x - 96.0f) - (16.0f - radius);
        const float dy = std::abs(y - 96.0f) - (16.0f - radius);
        const float distance = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) + std::min(std::max(dx, dy), 0.0f) - radius;
        return std::clamp(0.5f - distance, 0.0f, 1.0f);
    };
    std::array<float, blur_radius * 2 + 1> kernel{};
    float total = 0;
    for (int offset = -blur_radius; offset <= blur_radius; ++offset) {
        const float weight = std::exp(-float(offset * offset) / (2.0f * sigma * sigma));
        kernel[static_cast<std::size_t>(offset + blur_radius)] = weight;
        total += weight;
    }
    for (auto& weight : kernel) weight /= total;
    std::vector<float> source(width * width), horizontal(width * width), blurred(width * width);
    for (int y = 0; y < width; ++y)
        for (int x = 0; x < width; ++x)
            source[y * width + x] = coverage(float(x) + 0.5f, float(y) + 0.5f - 12.0f);
    for (int y = 0; y < width; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int offset = -blur_radius; offset <= blur_radius; ++offset) {
                if (x + offset >= 0 && x + offset < width)
                    horizontal[y * width + x] += source[y * width + x + offset] * kernel[static_cast<std::size_t>(offset + blur_radius)];
            }
        }
    }
    for (int y = 0; y < width; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int offset = -blur_radius; offset <= blur_radius; ++offset) {
                if (y + offset >= 0 && y + offset < width)
                    blurred[y * width + x] += horizontal[(y + offset) * width + x] * kernel[static_cast<std::size_t>(offset + blur_radius)];
            }
        }
    }
    const auto& surface = dark ? style.dark.surface : style.light.surface;
    const ToolbarColor shadow = dark ? ToolbarColor{0, 0, 0, 255} : ToolbarColor{40, 30, 20, 255};
    std::vector<unsigned char> pixels(width * width * 4, 0);
    for (int y = 0; y < width; ++y) {
        for (int x = 0; x < width; ++x) {
            const float core = coverage(float(x) + 0.5f, float(y) + 0.5f) * float(surface[3]) / 255.0f;
            const float shade = blurred[y * width + x] * (dark ? 0.55f : 0.16f) * (1.0f - core);
            const float alpha = core + shade;
            if (alpha == 0) continue;
            const int index = (y * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
                pixels[index + channel] = static_cast<unsigned char>(std::lround((surface[channel] * core + shadow[channel] * shade) / alpha));
            pixels[index + 3] = static_cast<unsigned char>(std::lround(alpha * 255.0f));
        }
    }
    return pixels;
}

LiveToolbarStyle::LiveToolbarStyle(std::string path) : m_path(std::move(path)) {}

bool LiveToolbarStyle::poll()
{
    if (!enabled()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now < m_next_poll) return false;
    m_next_poll = now + std::chrono::milliseconds(500);
    return reload();
}

bool LiveToolbarStyle::reload()
{
    if (!enabled()) return false;
    const auto failed = [this](const char* reason) {
        m_error = reason;
        m_has_content = false;
        return false;
    };
    try {
        const auto path = std::filesystem::u8path(m_path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return failed("Style file is unavailable");
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > max_file_size) return failed("Style file is unreadable or exceeds 32 KiB");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return failed("Style file cannot be opened");
        std::string content(max_file_size + 1, '\0');
        stream.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (stream.bad()) return failed("Style file cannot be read");
        content.resize(static_cast<std::size_t>(stream.gcount()));
        if (content.size() > max_file_size) return failed("Style file exceeds 32 KiB");
        if (m_has_content && content == m_last_content) return false;
        m_last_content = content;
        m_has_content = true;
        ToolbarStyleSnapshot next;
        if (!parse_toolbar_style(content, next, m_error) || next == m_snapshot) return false;
        m_snapshot = next;
        ++m_revision;
        return true;
    } catch (const std::filesystem::filesystem_error&) {
        return failed("Style file path is invalid");
    }
}

LiveToolbarStyle& live_toolbar_style()
{
    static LiveToolbarStyle style([] {
        const char* path = boost::nowide::getenv("PORCA_TOOLBAR_STYLE_FILE");
        return path == nullptr ? std::string{} : std::string(path);
    }());
    return style;
}
} // namespace Slic3r::GUI::DesignSystem
