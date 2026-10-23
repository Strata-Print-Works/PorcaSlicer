#pragma once

#include "DesignTokens.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::GUI::DesignSystem {

using ToolbarColor = std::array<unsigned char, 4>;

struct ToolbarThemeColors {
    ToolbarColor surface, surface_hover, ink, ink_muted, ink_disabled, accent_ink, accent_soft;
    bool operator==(const ToolbarThemeColors& other) const;
};

struct ToolbarStyleSnapshot {
    float control_size = control_md;
    float icon_size = icon_lg;
    float gap = space_4;
    float border = space_4;
    float top_inset = space_12;
    float corner_radius = radius_lg;
    float control_radius = radius_md;
    ToolbarThemeColors dark, light;

    ToolbarStyleSnapshot();
    bool operator==(const ToolbarStyleSnapshot& other) const;
    bool operator!=(const ToolbarStyleSnapshot& other) const { return !(*this == other); }
};

// Parses a complete override against compiled defaults. Failure leaves output unchanged.
bool parse_toolbar_style(std::string_view content, ToolbarStyleSnapshot& output, std::string& error);

// 192px RGBA nine-slice artwork: 32px core, 80px transparent gutter.
std::vector<unsigned char> toolbar_surface_pixels(const ToolbarStyleSnapshot& style, bool dark);

// Main-thread only. An explicitly supplied local file enables development overrides;
// normal launches never read a style file. Invalid edits preserve the last good snapshot.
class LiveToolbarStyle {
public:
    explicit LiveToolbarStyle(std::string path = {});
    const ToolbarStyleSnapshot& snapshot() const { return m_snapshot; }
    std::uint64_t revision() const { return m_revision; }
    bool enabled() const { return !m_path.empty(); }
    const std::string& error() const { return m_error; }
    bool poll();
    bool reload();

private:
    std::string m_path;
    ToolbarStyleSnapshot m_snapshot;
    std::uint64_t m_revision = 0;
    std::string m_error, m_last_content;
    bool m_has_content = false;
    std::chrono::steady_clock::time_point m_next_poll{};
};

LiveToolbarStyle& live_toolbar_style();

} // namespace Slic3r::GUI::DesignSystem
