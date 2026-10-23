#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/LiveToolbarStyle.hpp"
#include "test_utils.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace DS = Slic3r::GUI::DesignSystem;
using Catch::Matchers::WithinAbs;

namespace {
void write_style(const std::string& path, const std::string& contents)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << contents;
    out.close();
    REQUIRE(out.good());
}
}

TEST_CASE("Omitted toolbar overrides restore the compiled style", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    std::string error;
    REQUIRE(DS::parse_toolbar_style(R"({"version":1,"metrics":{"control_size":40,"icon_size":24,"gap":8}})", style, error));
    CHECK_THAT(style.control_size, WithinAbs(40.0f, 0.001f));
    CHECK_THAT(style.gap, WithinAbs(8.0f, 0.001f));
    REQUIRE(DS::parse_toolbar_style(R"({"version":1})", style, error));
    CHECK(style == DS::ToolbarStyleSnapshot{});
    CHECK(error.empty());
}

TEST_CASE("Toolbar themes and layout metrics update in one validated snapshot", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    const auto defaults = style;
    std::string error;
    REQUIRE(DS::parse_toolbar_style(R"({"version":1,
        "metrics":{"control_size":40,"icon_size":26,"border":6,"gap":8,"top_inset":20,"corner_radius":12,"control_radius":10},
        "themes":{"dark":{"surface":"#123456","ink":"#abcdef"},"light":{"accent_soft":"#FEDCBA"}}})", style, error));
    CHECK_THAT(style.control_size, WithinAbs(40.0f, 0.001f));
    CHECK_THAT(style.icon_size, WithinAbs(26.0f, 0.001f));
    CHECK_THAT(style.border, WithinAbs(6.0f, 0.001f));
    CHECK_THAT(style.gap, WithinAbs(8.0f, 0.001f));
    CHECK_THAT(style.top_inset, WithinAbs(20.0f, 0.001f));
    CHECK_THAT(style.corner_radius, WithinAbs(12.0f, 0.001f));
    CHECK_THAT(style.control_radius, WithinAbs(10.0f, 0.001f));
    const std::array<unsigned char, 4> surface{0x12, 0x34, 0x56, 255};
    const std::array<unsigned char, 4> ink{0xAB, 0xCD, 0xEF, 255};
    const std::array<unsigned char, 4> accent{0xFE, 0xDC, 0xBA, 255};
    CHECK(style.dark.surface == surface);
    CHECK(style.dark.ink == ink);
    CHECK(style.light.accent_soft == accent);
    CHECK(style.light.surface == defaults.light.surface);
}

TEST_CASE("Malformed toolbar overrides preserve every last good value", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    std::string error;
    REQUIRE(DS::parse_toolbar_style(R"({"version":1,"metrics":{"gap":10},"themes":{"dark":{"ink":"#112233"}}})", style, error));
    const auto good = style;
    for (const std::string invalid : {
             "", "{", "null", "[]", "{}",
             R"({"version":true})", R"({"version":1.0})", R"({"version":2})",
             R"({"version":1,"unexpected":3})", R"({"version":1,"metrics":[]})",
             R"({"version":1,"metrics":{"gap":false}})", R"({"version":1,"metrics":{"gap":"8"}})",
             R"({"version":1,"metrics":{"gap":8,"icon_size":-1}})",
             R"({"version":1,"metrics":{"icon_size":40,"control_size":28}})",
             R"({"version":1,"metrics":{"control_size":24,"control_radius":13}})",
             R"({"version":1,"metrics":{"gap":25}})", R"({"version":1,"metrics":{"top_inset":65}})",
             R"({"version":1,"metrics":{"control_size":1e100}})",
             R"({"version":1,"metrics":{"spacing":4}})",
             R"({"version":1,"themes":{"dark":{"ink":"#12345G"}}})",
             R"({"version":1,"themes":{"dark":{"ink":"#12345678"}}})",
             R"({"version":1,"themes":{"dark":{"ink":[1,2,3]}}})",
             R"({"version":1,"themes":{"dark":{"ink":"#123456","other":"#112233"}}})",
             R"({"version":1,"themes":{"unknown":{}}})"}) {
        DYNAMIC_SECTION(invalid) {
            CHECK_FALSE(DS::parse_toolbar_style(invalid, style, error));
            CHECK(style == good);
            CHECK_FALSE(error.empty());
        }
    }
}

TEST_CASE("Toolbar input limits reject oversized and excessively nested edits", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    std::string error;
    const auto good = style;
    const std::string oversized = R"({"version":1})" + std::string(32769, ' ');
    CHECK_FALSE(DS::parse_toolbar_style(oversized, style, error));
    CHECK(style == good);
    CHECK_FALSE(error.empty());
    const std::string nested = std::string(64, '[') + "0" + std::string(64, ']');
    CHECK_FALSE(DS::parse_toolbar_style(nested, style, error));
    CHECK(style == good);
    CHECK_FALSE(error.empty());
}

TEST_CASE("Disabled toolbar reload never reads a development file", "[LiveToolbarStyle]")
{
    DS::LiveToolbarStyle reload;
    CHECK_FALSE(reload.enabled());
    CHECK_FALSE(reload.reload());
    CHECK_FALSE(reload.poll());
    CHECK(reload.revision() == 0);
    CHECK(reload.snapshot() == DS::ToolbarStyleSnapshot{});
    CHECK(reload.error().empty());
}

TEST_CASE("Toolbar dimensions stay valid at the supported limits", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    std::string error;
    REQUIRE(DS::parse_toolbar_style(R"({"version":1,"metrics":{"control_size":24,"icon_size":24,"gap":0,"border":0,"top_inset":0,"corner_radius":0,"control_radius":12}})", style, error));
    CHECK_THAT(style.icon_size, WithinAbs(style.control_size, 0.001f));
    CHECK_THAT(style.control_radius, WithinAbs(style.control_size / 2, 0.001f));
    REQUIRE(DS::parse_toolbar_style(R"({"version":1,"metrics":{"control_size":64,"icon_size":48,"gap":24,"border":24,"top_inset":64,"corner_radius":16,"control_radius":24}})", style, error));
    CHECK_THAT(style.control_size, WithinAbs(64.0f, 0.001f));
    CHECK_THAT(style.top_inset, WithinAbs(64.0f, 0.001f));
    const auto good = style;
    CHECK_FALSE(DS::parse_toolbar_style(R"({"version":1,"metrics":{"corner_radius":17}})", style, error));
    CHECK(style == good);
}

TEST_CASE("Toolbar revisions change only when the effective style changes", "[LiveToolbarStyle]")
{
    ScopedTemporaryFile file(".json");
    write_style(file.string(), R"({"version":1,"metrics":{"gap":8}})");
    DS::LiveToolbarStyle reload(file.string());
    REQUIRE(reload.enabled());
    CHECK(reload.revision() == 0);
    REQUIRE(reload.reload());
    const auto changed_revision = reload.revision();
    CHECK(changed_revision > 0);
    CHECK_THAT(reload.snapshot().gap, WithinAbs(8.0f, 0.001f));
    CHECK_FALSE(reload.reload());
    CHECK(reload.revision() == changed_revision);
    write_style(file.string(), "{\n  \"metrics\": { \"gap\": 8.0 }, \"version\": 1\n}");
    CHECK_FALSE(reload.reload());
    CHECK(reload.revision() == changed_revision);
    write_style(file.string(), R"({"version":1})");
    REQUIRE(reload.reload());
    CHECK(reload.revision() > changed_revision);
    CHECK(reload.snapshot() == DS::ToolbarStyleSnapshot{});
}

TEST_CASE("Partial and missing toolbar files keep the last good style", "[LiveToolbarStyle]")
{
    ScopedTemporaryFile file(".json");
    DS::LiveToolbarStyle reload(file.string());
    CHECK_FALSE(reload.reload());
    CHECK_FALSE(reload.error().empty());
    write_style(file.string(), R"({"version":1,"metrics":{"gap":9}})");
    REQUIRE(reload.reload());
    const auto good = reload.snapshot();
    const auto revision = reload.revision();
    write_style(file.string(), R"({"version":1,"metrics":{"gap":)");
    CHECK_FALSE(reload.reload());
    CHECK(reload.snapshot() == good);
    CHECK(reload.revision() == revision);
    CHECK_FALSE(reload.error().empty());
    write_style(file.string(), R"({"version":1})" + std::string(32769, ' '));
    CHECK_FALSE(reload.reload());
    CHECK(reload.snapshot() == good);
    CHECK(reload.revision() == revision);
    REQUIRE(std::filesystem::remove(file.string()));
    CHECK_FALSE(reload.reload());
    CHECK(reload.snapshot() == good);
    CHECK(reload.revision() == revision);
    write_style(file.string(), R"({"version":1,"metrics":{"gap":9}})");
    CHECK_FALSE(reload.reload());
    CHECK(reload.error().empty());
    CHECK(reload.revision() == revision);
}

TEST_CASE("Replacing a toolbar file does not require restarting its watcher", "[LiveToolbarStyle]")
{
    ScopedTemporaryDir dir("toolbar-reload");
    const auto live = (dir.path() / "style.json").string();
    const auto replacement = (dir.path() / "replacement.json").string();
    write_style(live, R"({"version":1})");
    DS::LiveToolbarStyle reload(live);
    CHECK_FALSE(reload.reload());
    CHECK(reload.error().empty());
    write_style(replacement, R"({"version":1,"metrics":{"top_inset":30}})");
    REQUIRE(std::filesystem::remove(live));
    std::filesystem::rename(replacement, live);
    REQUIRE(reload.reload());
    CHECK_THAT(reload.snapshot().top_inset, WithinAbs(30.0f, 0.001f));
    CHECK(reload.error().empty());
}

TEST_CASE("Reloaded toolbar surfaces preserve solid colors and transparent gutters", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    style.dark.surface = {17, 34, 51, 255};
    style.light.surface = {230, 220, 210, 255};
    constexpr std::size_t width = 192;
    for (bool dark : {false, true}) {
        DYNAMIC_SECTION(dark) {
            const auto pixels = DS::toolbar_surface_pixels(style, dark);
            REQUIRE(pixels.size() == width * width * 4);
            const auto& surface = dark ? style.dark.surface : style.light.surface;
            const std::size_t centre = (96 * width + 96) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel)
                CHECK(pixels[centre + channel] == surface[channel]);
            for (std::size_t position = 0; position < width; ++position) {
                CHECK(pixels[(position * width) * 4 + 3] == 0);
                CHECK(pixels[(position * width + width - 1) * 4 + 3] == 0);
                CHECK(pixels[position * 4 + 3] == 0);
                CHECK(pixels[((width - 1) * width + position) * 4 + 3] == 0);
            }
        }
    }
}

TEST_CASE("Toolbar surface corners and elevation respond to the selected style", "[LiveToolbarStyle]")
{
    DS::ToolbarStyleSnapshot style;
    style.corner_radius = 0;
    const auto square = DS::toolbar_surface_pixels(style, true);
    style.corner_radius = 16;
    const auto round = DS::toolbar_surface_pixels(style, true);
    constexpr std::size_t width = 192;
    REQUIRE(square.size() == width * width * 4);
    REQUIRE(round.size() == square.size());
    const std::size_t corner = (80 * width + 80) * 4 + 3;
    CHECK(square[corner] == 255);
    CHECK(round[corner] < square[corner]);
    // The supplied elevation shifts the shadow down, beneath the solid surface.
    const std::size_t above = (56 * width + 96) * 4 + 3;
    const std::size_t below = (136 * width + 96) * 4 + 3;
    CHECK(round[below] > round[above]);
    CHECK(round[below] > 0);
}
