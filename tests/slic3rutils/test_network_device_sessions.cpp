#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
// No transport entry point invokes the base implementation. Construction only
// initializes fields; these fakes never start wx, a worker, HTTP, DNS or sockets.
class FakeTransport final : public Slic3r::MoonrakerPrinterAgent {
public:
    explicit FakeTransport(std::string id = "fake") : MoonrakerPrinterAgent(""), id(std::move(id)) {}
    Slic3r::AgentInfo get_agent_info() override { return {id, "Fake transport", "1", "Offline test"}; }
    std::string id;
    std::function<void()> during_transfer;
    Slic3r::OnMsgArrivedFn discovery;
    int connects = 0, disconnects = 0;
    std::vector<std::string> sent;
    Slic3r::OnLocalConnectedFn connection;
    Slic3r::OnMessageFn message;
    Slic3r::OnMessageFn cloud_message;
    int connect_printer(const Slic3r::PrinterConnectionParams&) override { ++connects; return 0; }
    int disconnect_printer() override { ++disconnects; return 0; }
    int send_message_to_printer(std::string id, std::string, int, int) override { sent.push_back(id); return 0; }
    int transfer() { if (during_transfer) during_transfer(); return 0; }
    int start_print(Slic3r::PrintParams, Slic3r::OnUpdateStatusFn, Slic3r::WasCancelledFn, Slic3r::OnWaitFn) override { return transfer(); }
    int start_local_print_with_record(Slic3r::PrintParams, Slic3r::OnUpdateStatusFn, Slic3r::WasCancelledFn, Slic3r::OnWaitFn) override { return transfer(); }
    int start_send_gcode_to_sdcard(Slic3r::PrintParams, Slic3r::OnUpdateStatusFn, Slic3r::WasCancelledFn, Slic3r::OnWaitFn) override { return transfer(); }
    int start_local_print(Slic3r::PrintParams, Slic3r::OnUpdateStatusFn, Slic3r::WasCancelledFn) override { return transfer(); }
    int start_sdcard_print(Slic3r::PrintParams, Slic3r::OnUpdateStatusFn, Slic3r::WasCancelledFn) override { return transfer(); }
    int set_on_ssdp_msg_fn(Slic3r::OnMsgArrivedFn fn) override { discovery = std::move(fn); return 0; }
    int set_on_local_connect_fn(Slic3r::OnLocalConnectedFn fn) override { connection = std::move(fn); return 0; }
    int set_on_local_message_fn(Slic3r::OnMessageFn fn) override { message = std::move(fn); return 0; }
    int set_on_message_fn(Slic3r::OnMessageFn fn) override { cloud_message = std::move(fn); return 0; }
};
Slic3r::PrinterConnectionParams params(const std::string& id)
{ Slic3r::PrinterConnectionParams p; p.dev_id = id; p.host = id; return p; }
}

TEST_CASE("The network facade reuses attempts and routes retained devices to their own transports", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>(), b = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, a);
    network.connect_printer(params("one"));
    network.connect_printer(params("one"));
    CHECK(a->connects == 1);
    network.set_printer_agent(b);
    network.connect_printer(params("two"));
    CHECK(a->disconnects == 0);
    CHECK(network.send_message_to_printer("one", R"({"info":{"command":"get_version"}})", 0, 0) == 0);
    CHECK(network.send_message_to_printer("one", R"({"print":{"command":"pause"}})", 0, 0) != 0);
    // A background session never echoes its saved credential into telemetry.
    CHECK(network.send_message_to_printer("one", R"({"system":{"command":"get_access_code"}})", 0, 0) != 0);
    CHECK(network.command_set_bed("one", 0, false, 1, true) != 0);
    CHECK(network.send_message_to_printer("one", R"({"info":{"command":"get_version"},"print":{"command":"pause"}})", 0, 0) != 0);
    CHECK(network.send_message_to_printer("two", "{}", 0, 0) == 0);
    CHECK(network.send_message_to_printer("unknown", "{}", 0, 0) != 0);
    REQUIRE(a->sent.size() == 1);
    REQUIRE(b->sent.size() == 1);
    CHECK(a->sent.front() == "one");
    CHECK(b->sent.front() == "two");
    network.set_printer_agent(a);
    network.connect_printer(params("one"));
    CHECK(a->connects == 1);
    CHECK(b->disconnects == 0);
}

TEST_CASE("An explicit reconnect retries an offline printer with unchanged settings", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, a);
    std::vector<std::string> messages;
    network.set_on_local_message_fn([&](std::string id, std::string) { messages.push_back(id); });
    network.connect_printer(params("one"));
    const auto lost = network.printer_session_token("one");
    auto late_message = a->message;
    a->connection(Slic3r::ConnectStatus::ConnectStatusFailed, "one", "connection_failed");
    REQUIRE(lost->state == Slic3r::PrinterControlState::offline);
    CHECK(network.connect_printer(params("one")) == 0);
    CHECK(a->connects == 2);
    CHECK(a->disconnects == 0);
    const auto retry = network.printer_session_token("one");
    CHECK(retry != lost);
    CHECK_FALSE(lost->valid);
    CHECK(retry->state == Slic3r::PrinterControlState::connecting);
    CHECK(network.get_printer_agent() == a);
    late_message("one", "{}");
    CHECK(messages.empty());
    a->message("one", "{}");
    CHECK(messages == std::vector<std::string>{"one"});
    // The new attempt is pending, so selecting the printer again reuses it.
    network.connect_printer(params("one"));
    CHECK(a->connects == 2);
}

TEST_CASE("Expired attempts keep no connection handler without a UI callback", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, a);
    network.connect_printer(params("one"));
    REQUIRE(a->connection);
    a->connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected);
    REQUIRE(network.expire_printer_session("one"));
    CHECK_FALSE(a->connection);
}

TEST_CASE("Retired and cross-device callbacks cannot update connection state or telemetry", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, a);
    std::vector<std::string> messages;
    network.set_on_local_message_fn([&](std::string id, std::string) { messages.push_back(id); });
    network.set_on_local_connect_fn([](int, std::string, std::string) {});
    network.connect_printer(params("one"));
    auto old_message = a->message;
    auto old_connection = a->connection;
    auto token = network.printer_session_token("one");
    a->message("other", "{}");
    CHECK(messages.empty());
    a->connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    CHECK(token->state == Slic3r::PrinterControlState::connected);
    a->connection(Slic3r::ConnectStatus::ConnectStatusFailed, "one", "5");
    CHECK(token->state == Slic3r::PrinterControlState::rejected);
    network.forget_printer_session("one");
    network.connect_printer(params("one"));
    old_message("one", "{}");
    old_connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    CHECK(messages.empty());
    CHECK_FALSE(token->valid);
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connecting);
    a->message("one", "{}");
    REQUIRE(messages.size() == 1);
}

TEST_CASE("Shutdown removes callbacks and disconnects every retained transport", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>(), b = std::make_shared<FakeTransport>();
    std::shared_ptr<Slic3r::PrinterSessionToken> token;
    {
        Slic3r::NetworkAgent network(nullptr, a);
        network.set_on_local_message_fn([](std::string, std::string) {});
        network.connect_printer(params("one"));
        token = network.printer_session_token("one");
        network.set_printer_agent(b);
        network.connect_printer(params("two"));
        network.set_on_local_message_fn(nullptr);
        CHECK_FALSE(a->message);
        CHECK_FALSE(b->message);
    }
    CHECK_FALSE(token->valid);
    CHECK(a->disconnects == 1);
    CHECK(b->disconnects == 1);
}

TEST_CASE("Recovering a silent printer retires its callbacks without interrupting another printer", "[NetworkDeviceSessions]")
{
    auto silent = std::make_shared<FakeTransport>(), healthy = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, silent);
    std::vector<std::string> messages;
    network.set_on_local_message_fn([&](std::string id, std::string) { messages.push_back(id); });
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), healthy);
    silent->connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    healthy->connection(Slic3r::ConnectStatus::ConnectStatusOk, "two", "");
    auto late_message = silent->message;
    auto late_connection = silent->connection;
    const auto retired = network.printer_session_token("one");
    const auto other = network.printer_session_token("two");

    network.expire_printer_session("one");
    CHECK_FALSE(retired->valid);
    CHECK(silent->disconnects == 1);
    CHECK(healthy->disconnects == 0);
    CHECK(other->state == Slic3r::PrinterControlState::connected);
    CHECK(network.send_message_to_printer("one", "{}", 0, 0) != 0);

    auto replacement = std::make_shared<FakeTransport>();
    network.connect_printer(params("one"), replacement);
    const auto retry = network.printer_session_token("one");
    CHECK(retry != retired);
    late_connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    late_message("one", "{}");
    CHECK(retry->state == Slic3r::PrinterControlState::connecting);
    CHECK(messages.empty());
    replacement->connection(Slic3r::ConnectStatus::ConnectStatusOk, "one", "");
    replacement->message("one", "{}");
    healthy->message("two", "{}");
    CHECK(messages == std::vector<std::string>{"one", "two"});
    CHECK(other->valid);
}

TEST_CASE("Automatic recovery preserves the transport used by every LAN print entry point", "[NetworkDeviceSessions]")
{
    const int operation = GENERATE(0, 1, 2, 3, 4);
    auto uploading = std::make_shared<FakeTransport>(), idle = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, uploading);
    network.connect_printer(params("one"));
    network.connect_printer(params("two"), idle);
    auto token = network.printer_session_token("one");
    token->state = Slic3r::PrinterControlState::connected;
    uploading->during_transfer = [&] {
        CHECK_FALSE(token->needs_reconnect(false, token->started_at + std::chrono::seconds(60)));
        CHECK_FALSE(network.expire_printer_session("one"));
        CHECK(uploading->disconnects == 0);
        CHECK(token->valid);
        CHECK(network.expire_printer_session("two"));
    };
    Slic3r::PrintParams print{};
    print.dev_id = "one";
    print.connection_type = "lan";
    int result = -1;
    switch (operation) {
    case 0: result = network.start_print(print, {}, {}, {}); break;
    case 1: result = network.start_local_print_with_record(print, {}, {}, {}); break;
    case 2: result = network.start_send_gcode_to_sdcard(print, {}, {}, {}); break;
    case 3: result = network.start_local_print(print, {}, {}); break;
    case 4: result = network.start_sdcard_print(print, {}, {}); break;
    }
    CHECK(result == 0);
    CHECK_FALSE(network.expire_printer_session("one"));
    CHECK(uploading->disconnects == 0);
    CHECK(idle->disconnects == 1);
    CHECK(token->needs_reconnect(false, std::chrono::steady_clock::now() + std::chrono::seconds(31)));
}

TEST_CASE("Failed uploads release recovery protection and explicit removal still disconnects", "[NetworkDeviceSessions]")
{
    auto uploading = std::make_shared<FakeTransport>();
    Slic3r::NetworkAgent network(nullptr, uploading);
    network.connect_printer(params("one"));
    auto token = network.printer_session_token("one");
    token->state = Slic3r::PrinterControlState::connected;
    Slic3r::PrintParams print{};
    print.dev_id = "one";
    print.connection_type = "lan";
    uploading->during_transfer = [] { throw std::runtime_error("fixture upload failure"); };
    CHECK_THROWS_AS(network.start_local_print(print, {}, {}), std::runtime_error);
    CHECK(token->needs_reconnect(false, std::chrono::steady_clock::now() + std::chrono::seconds(31)));
    uploading->during_transfer = [&] { network.forget_printer_session("one"); };
    CHECK(network.start_local_print(print, {}, {}) == 0);
    CHECK_FALSE(token->valid);
    CHECK(uploading->disconnects == 1);
}

TEST_CASE("Delayed discovery keeps its originating backend after another view is selected", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>("first"), b = std::make_shared<FakeTransport>("second");
    Slic3r::NetworkAgent network(nullptr, a);
    std::string notice;
    network.set_queue_on_main_fn([](std::function<void()> fn) { fn(); });
    network.set_on_ssdp_msg_fn([&](std::string payload) { notice = std::move(payload); });
    network.connect_printer(params("one"));
    auto pending_discovery = a->discovery;
    network.set_printer_agent(b);
    network.connect_printer(params("two"));
    pending_discovery(R"({"dev_id":"one","__porca_printer_agent_id":"untrusted"})");
    const auto parsed = nlohmann::json::parse(notice);
    CHECK(parsed["__porca_printer_agent_id"] == "first");
    CHECK(parsed["dev_id"] == "one");
}

TEST_CASE("Origin stamping keeps each payload intact and drops non-object payloads", "[NetworkDeviceSessions]")
{
    auto a = std::make_shared<FakeTransport>("first");
    Slic3r::NetworkAgent network(nullptr, a);
    std::vector<std::string> notices;
    network.set_queue_on_main_fn([](std::function<void()> fn) { fn(); });
    network.set_on_ssdp_msg_fn([&](std::string payload) { notices.push_back(std::move(payload)); });
    network.connect_printer(params("one"));
    a->discovery(" { } ");
    a->discovery(R"({"dev_id":"one","nested":{"text":"}"}})");
    a->discovery(R"({"\u005f_porca_printer_agent_id":"forged"})");
    a->discovery("[]");
    a->discovery("");
    REQUIRE(notices.size() == 3);
    for (const auto& notice : notices) {
        const auto parsed = nlohmann::json::parse(notice);
        CHECK(parsed["__porca_printer_agent_id"] == "first");
    }
    CHECK(nlohmann::json::parse(notices[0]).size() == 1);
    const auto fields = nlohmann::json::parse(notices[1]);
    CHECK(fields["dev_id"] == "one");
    CHECK(fields["nested"]["text"] == "}");
}

TEST_CASE("Retained cloud callbacks preserve their backend while another view is selected", "[NetworkDeviceSessions]")
{
    auto bambu = std::make_shared<FakeTransport>("bbl"), other = std::make_shared<FakeTransport>("other");
    Slic3r::NetworkAgent network(nullptr, bambu);
    std::string received;
    network.set_on_message_fn([&](std::string, std::string payload) { received = std::move(payload); });
    network.connect_printer(params("one"));
    network.set_printer_agent(other);
    network.connect_printer(params("two"));
    bambu->cloud_message("one", R"({"print":{"command":"push_status"}})");
    CHECK(nlohmann::json::parse(received)["__porca_printer_agent_id"] == "bbl");
}

TEST_CASE("Plain-string cloud notices reach the application unchanged", "[NetworkDeviceSessions]")
{
    auto bambu = std::make_shared<FakeTransport>("bbl");
    Slic3r::NetworkAgent network(nullptr, bambu);
    std::vector<std::pair<std::string, std::string>> received;
    network.set_on_message_fn([&](std::string id, std::string payload) { received.emplace_back(std::move(id), std::move(payload)); });
    REQUIRE(bambu->cloud_message);
    bambu->cloud_message("", "wait_info");
    bambu->cloud_message("", "cert_expired");
    bambu->cloud_message("one", R"({"print":{"command":"push_status"}})");
    REQUIRE(received.size() == 3);
    CHECK(received[0].first.empty());
    CHECK(received[0].second == "wait_info");
    CHECK(received[1].second == "cert_expired");
    CHECK(nlohmann::json::parse(received[2].second)["__porca_printer_agent_id"] == "bbl");
}

namespace {
struct Teardown {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false, waited = false, destroyed = false;
    std::thread::id thread;
};
// Its destructor blocks until released, like a transport joining its workers.
class BlockingTeardownTransport final : public Slic3r::MoonrakerPrinterAgent {
public:
    explicit BlockingTeardownTransport(std::shared_ptr<Teardown> teardown) : MoonrakerPrinterAgent(""), teardown(std::move(teardown)) {}
    ~BlockingTeardownTransport() override
    {
        std::unique_lock<std::mutex> lock(teardown->mutex);
        teardown->waited = teardown->cv.wait_for(lock, std::chrono::seconds(5), [this] { return teardown->released; });
        teardown->thread = std::this_thread::get_id();
        teardown->destroyed = true;
    }
    Slic3r::AgentInfo get_agent_info() override { return {"blocking", "Blocking teardown", "1", "Offline test"}; }
    int connect_printer(const Slic3r::PrinterConnectionParams&) override { return 0; }
    int disconnect_printer() override { return 0; }
private:
    std::shared_ptr<Teardown> teardown;
};
}

TEST_CASE("Forgotten transports are destroyed off the caller's thread and joined by their owner", "[NetworkDeviceSessions]")
{
    auto teardown = std::make_shared<Teardown>();
    {
        Slic3r::NetworkAgent network(nullptr, nullptr);
        REQUIRE(network.connect_printer(params("one"), std::make_shared<BlockingTeardownTransport>(teardown)) == 0);
        network.forget_printer_session("one"); // Returns while the transport is still tearing down.
        CHECK_FALSE(network.has_printer_session("one"));
        {
            std::lock_guard<std::mutex> lock(teardown->mutex);
            teardown->released = true;
        }
        teardown->cv.notify_all();
    } // The owner waits for the retired transport before it is gone.
    std::lock_guard<std::mutex> lock(teardown->mutex);
    CHECK(teardown->destroyed);
    CHECK(teardown->waited);
    CHECK(teardown->thread != std::this_thread::get_id());
}
