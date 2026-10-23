#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/OptionsGroup.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <string>
#include <vector>
#include <wx/string.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
class ScopeFields : public OptionsGroup {
public:
    ScopeFields() : OptionsGroup(nullptr, wxEmptyString, wxEmptyString) {}
    const std::vector<ConfigOptionMode>& modes() const { return m_options_mode; }
    void add(const std::string& key, ConfigOptionMode mode) {
        ConfigOptionDef def;
        def.type = coFloat;
        def.opt_key = key;
        def.mode = mode;
        Slic3r::GUI::Line line(wxEmptyString, wxEmptyString);
        line.append_option(Option(def, key));
        append_line(line);
    }
};
}

TEST_CASE("Filtering a settings scope keeps modes aligned across separators", "[SettingsScope]")
{
    ScopeFields fields;
    fields.add("first", comSimple);
    fields.append_separator();
    fields.add("second", comAdvanced);
    fields.add("third", comExpert);
    fields.remove_option_if([](const std::string& key) { return key == "third"; });
    REQUIRE(fields.get_lines().size() == 2);
    REQUIRE(fields.modes().size() == fields.get_lines().size());
    CHECK(fields.modes()[0] == comSimple);
    CHECK(fields.modes()[1] == comAdvanced);
    fields.remove_option_if([](const std::string& key) { return key == "first"; });
    REQUIRE(fields.modes().size() == 1);
    CHECK(fields.modes().front() == comAdvanced);
    fields.remove_option_if([](const std::string&) { return true; });
    CHECK(fields.get_lines().empty());
    CHECK(fields.modes().empty());
}
