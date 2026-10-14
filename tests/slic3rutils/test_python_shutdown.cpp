#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>

#include "slic3r/GUI/MarkdownTip.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/host/PluginHostUi.hpp"
#include "plugin_test_utils.hpp"

namespace {

struct ExitGILProbe
{
    bool armed = false;

    ~ExitGILProbe()
    {
        if (!armed)
            return;
        Slic3r::PythonGILState gil;
        if (gil)
            std::abort();
    }
};

struct ScopedInterpreterShutdown
{
    Slic3r::PythonInterpreter& interpreter;
    ~ScopedInterpreterShutdown() { interpreter.shutdown(); }
};

} // namespace

TEST_CASE("Python GIL acquisition is harmless after interpreter shutdown", "[PythonShutdown]")
{
    // Registered before the interpreter singleton: its destructor runs after
    // the interpreter's mutex is gone. Process exit is part of this regression.
    static ExitGILProbe exit_probe;
    Slic3r::ScopedDataDir data_dir("python-shutdown");
    auto& interpreter = Slic3r::PythonInterpreter::instance();
    ScopedInterpreterShutdown shutdown{interpreter};
    REQUIRE(interpreter.initialize());

    {
        Slic3r::PythonGILState gil;
        REQUIRE(static_cast<bool>(gil));
    }

    interpreter.shutdown();
    REQUIRE_FALSE(Py_IsInitialized());
    {
        Slic3r::PythonGILState gil;
        CHECK_FALSE(static_cast<bool>(gil));
    }
    exit_probe.armed = true;
}

TEST_CASE("Shutdown releases the retained settings callback", "[PythonShutdown]")
{
    auto captured = std::make_shared<int>(0);
    std::weak_ptr<int> retained = captured;
    Slic3r::GUI::MarkdownTip::SetAskHandler([captured](std::string) {});
    captured.reset();
    CHECK_FALSE(retained.expired());

    Slic3r::PluginHostUi::clear_callbacks();
    CHECK(retained.expired());
}
