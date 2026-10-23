#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/PrinterSessionRegistry.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/path.hpp>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string fixture_path()
{
    return (boost::dll::program_location().parent_path() / PORCA_FAKE_BBL_PLUGIN).string();
}
std::shared_ptr<Slic3r::BBLPrinterAgent> fake_bambu()
{
    // Explicit fixture path only. Never resolves the installed vendor plugin.
    auto plugin = Slic3r::BBLNetworkPlugin::load_device_plugin(fixture_path(), "");
    REQUIRE(plugin);
    return std::make_shared<Slic3r::BBLPrinterAgent>(std::move(plugin));
}
Slic3r::PrinterConnectionParams params(const std::string& id)
{
    Slic3r::PrinterConnectionParams result;
    result.dev_id = id; result.host = "offline-fixture"; result.password = "test";
    return result;
}
void drain(std::vector<std::function<void()>>& queue)
{
    auto pending = std::move(queue);
    queue.clear();
    for (auto& fn : pending) fn();
}
}

TEST_CASE("Four Bambu SDK handles retain independent connections while only the view changes", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    std::vector<std::string> telemetry;
    network.set_on_local_connect_fn([](int, std::string, std::string) {});
    network.set_on_local_message_fn([&](std::string id, std::string) { telemetry.push_back(id); });
    std::array<std::shared_ptr<Slic3r::BBLPrinterAgent>, 4> printers;
    for (size_t i = 0; i < printers.size(); ++i) {
        printers[i] = fake_bambu();
        const auto id = std::to_string(i);
        REQUIRE(network.connect_printer(params(id), printers[i]) == 0);
        CHECK(network.printer_session_token(id)->state == Slic3r::PrinterControlState::connected);
        CHECK_FALSE(network.get_printer_agent()); // Eager connection never selects a view.
    }
    for (size_t i = 0; i < printers.size(); ++i) {
        const auto id = std::to_string(i);
        network.set_printer_agent(printers[i]);
        auto token = network.printer_session_token(id);
        REQUIRE(network.connect_printer(params(id)) == 0);
        CHECK(network.printer_session_token(id) == token);
        REQUIRE(network.send_message_to_printer(id, R"({"print":{"command":"pause"}})", 0, 0) == 0);
        for (size_t j = 0; j < printers.size(); ++j)
            CHECK(network.printer_session_token(std::to_string(j))->state == Slic3r::PrinterControlState::connected);
    }
    const std::vector<std::string> expected{"0", "1", "2", "3"};
    CHECK(telemetry == expected);
    CHECK(printers[0]->send_message_to_printer("3", "{}", 0, 0) != 0);
    CHECK(printers[0]->connect_printer(params("3")) != 0);
    network.forget_printer_session("1");
    network.set_printer_agent(printers[3]);
    CHECK(network.send_message_to_printer("3", "{}", 0, 0) == 0);
    CHECK(network.send_message_to_printer("1", "{}", 0, 0) != 0);
    CHECK(network.printer_session_token("0")->state == Slic3r::PrinterControlState::connected);
    CHECK(network.printer_session_token("2")->state == Slic3r::PrinterControlState::connected);
}

TEST_CASE("A replaced Bambu SDK attempt rejects queued callbacks and retains the visible target", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    std::vector<std::function<void()>> queue;
    std::vector<std::string> telemetry;
    network.set_queue_on_main_fn([&](std::function<void()> fn) { queue.push_back(std::move(fn)); });
    network.set_on_local_connect_fn([](int, std::string, std::string) {});
    network.set_on_local_message_fn([&](std::string id, std::string) { telemetry.push_back(id); });
    auto first = fake_bambu(), second = fake_bambu();
    network.set_printer_agent(first);
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), second);
    auto old = network.printer_session_token("one");
    auto replacement = fake_bambu();
    network.connect_printer(params("one"), replacement);
    CHECK_FALSE(old->valid);
    CHECK(network.get_printer_agent() == replacement);
    drain(queue);
    CHECK(old->state == Slic3r::PrinterControlState::connecting);
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected);
    CHECK(network.printer_session_token("two")->state == Slic3r::PrinterControlState::connected);
    first->send_message_to_printer("one", "{}", 0, 0);
    CHECK(telemetry.empty());
    CHECK(network.send_message_to_printer("one", "{}", 0, 0) == 0);
    drain(queue);
    REQUIRE(telemetry.size() == 1);
    CHECK(telemetry.front() == "one");
}

TEST_CASE("Rejected Bambu credentials do not replace another device's connected state", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_local_connect_fn([](int, std::string, std::string) {});
    auto good = fake_bambu(), denied = fake_bambu();
    auto rejected = params("denied"); rejected.password = "reject";
    network.connect_printer(params("good"), good);
    network.connect_printer(rejected, denied);
    CHECK(network.printer_session_token("good")->state == Slic3r::PrinterControlState::connected);
    CHECK(network.printer_session_token("denied")->state == Slic3r::PrinterControlState::rejected);
}

TEST_CASE("Queued SDK work keeps its agent and library alive through removal", "[BambuDeviceSessions]")
{
    auto probe = Slic3r::BBLNetworkPlugin::load_device_plugin(fixture_path(), "");
    REQUIRE(probe);
    const auto count = reinterpret_cast<int(*)()>(probe->get_network_function("porca_fake_active_agents"));
    REQUIRE(count);
    const int baseline = count();
    Slic3r::NetworkAgent network(nullptr, nullptr);
    std::vector<std::function<void()>> queue;
    network.set_queue_on_main_fn([&](std::function<void()> work) { queue.push_back(std::move(work)); });
    network.set_on_local_connect_fn([](int, std::string, std::string) {});
    {
        auto printer = fake_bambu();
        network.connect_printer(params("one"), printer);
        network.forget_printer_session("one");
    }
    CHECK(count() == baseline + 1);
    drain(queue);
    // The removed transport is destroyed by the owned reaper or the last queued work.
    Slic3r::NetworkAgent::wait_for_retired_printer_agents();
    CHECK(count() == baseline);
}

TEST_CASE("Bambu session state does not depend on an installed view callback", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.connect_printer(params("one"), fake_bambu());
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected);
    auto rejected = params("two"); rejected.password = "reject";
    network.connect_printer(rejected, fake_bambu());
    CHECK(network.printer_session_token("two")->state == Slic3r::PrinterControlState::rejected);
}

TEST_CASE("Expiring a Bambu attempt cancels only that device and its view follows replacement", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    auto first = fake_bambu(), second = fake_bambu();
    network.set_printer_agent(first);
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), second);
    auto old = network.printer_session_token("one");
    network.expire_printer_session("one");
    CHECK_FALSE(old->valid);
    CHECK(old->state == Slic3r::PrinterControlState::offline);
    CHECK(network.send_message_to_printer("one", "{}", 0, 0) != 0);
    CHECK(network.printer_session_token("two")->state == Slic3r::PrinterControlState::connected);
    auto replacement = fake_bambu();
    network.connect_printer(params("one"), replacement);
    CHECK(network.get_printer_agent() == replacement);
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected);
    CHECK(network.printer_session_token("two")->state == Slic3r::PrinterControlState::connected);
}

TEST_CASE("The ordinary LAN picker resolves a retained Bambu device before switching transport", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    auto first = fake_bambu(), second = fake_bambu();
    network.set_printer_agent(first);
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), second);
    const auto one = network.printer_session_token("one"), two = network.printer_session_token("two");
    REQUIRE(network.connect_printer(params("two")) == 0);
    CHECK(network.get_printer_agent() == second);
    CHECK(network.printer_session_token("two") == two);
    CHECK(one->valid);
    CHECK(one->state == Slic3r::PrinterControlState::connected);
    CHECK(network.connect_printer(params("one")) == 0);
    CHECK(network.get_printer_agent() == first);
    CHECK(network.printer_session_token("one") == one);
    CHECK(two->valid);
}

TEST_CASE("Clearing the Bambu view retains connections until their owner is explicitly retired", "[BambuDeviceSessions]")
{
    Slic3r::NetworkAgent network(nullptr, nullptr);
    auto first = fake_bambu(), second = fake_bambu();
    network.set_printer_agent(first);
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), second);
    auto one = network.printer_session_token("one"), two = network.printer_session_token("two");
    network.set_printer_agent(nullptr);
    CHECK_FALSE(network.get_printer_agent());
    CHECK(one->valid);
    CHECK(two->valid);
    CHECK(one->state == Slic3r::PrinterControlState::connected);
    CHECK(two->state == Slic3r::PrinterControlState::connected);
    network.forget_printer_sessions_for_agent("bbl");
    CHECK_FALSE(one->valid);
    CHECK_FALSE(two->valid);
    CHECK_FALSE(network.has_printer_session("one"));
    CHECK_FALSE(network.has_printer_session("two"));
}
