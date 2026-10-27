#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

// Every transport entry point is fake: these tests never create a socket or wx app.
struct ConnectionState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> fetched, queried, streamed;
    std::vector<std::function<void()>> queued;
    std::vector<std::string> connected;
    std::string block_fetch, block_query;
    unsigned block_query_at = 0;
    bool released = false;
    bool throw_fetch = false;
    bool reject_fetch = false;
    unsigned reject_query_at = 0;
    nlohmann::json status = {{"print_stats", {{"state", "standby"}}}, {"extruder", {{"temperature", 23.0}}}};
    unsigned poll_waits = 0;
    unsigned polls = 0;
    std::string fetch_error = "HTTP error: 401";
    std::string query_error = "HTTP error: 403";
    std::vector<int> connection_states;
    std::vector<std::string> reasons;
    std::vector<std::pair<std::string, nlohmann::json>> messages;
    std::vector<std::string> commands;
    std::vector<std::function<void(bool)>> completions;

    template<class Predicate> bool wait(Predicate predicate)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), predicate);
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }

    void drain()
    {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callbacks.swap(queued);
        }
        for (auto& callback : callbacks) callback();
    }
};

class FakeMoonrakerAgent : public Slic3r::MoonrakerPrinterAgent {
public:
    explicit FakeMoonrakerAgent(std::shared_ptr<ConnectionState> state)
        : MoonrakerPrinterAgent(""), state(std::move(state))
    {
        set_queue_on_main_fn([s = this->state](std::function<void()> callback) {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->queued.push_back(std::move(callback));
            s->cv.notify_all();
        });
        set_on_local_connect_fn([s = this->state](int status, std::string id, std::string reason) {
            s->connected.push_back(id);
            s->connection_states.push_back(status);
            s->reasons.push_back(std::move(reason));
        });
        set_on_printer_connected_fn([s = this->state](std::string id) { s->connected.push_back(id); });
    }

    ~FakeMoonrakerAgent() override
    {
        state->release();
        shutdown_connection();
    }

    void finish() { shutdown_connection(); }
    void collect_messages()
    {
        set_on_local_message_fn([s = state](std::string id, std::string payload) {
            s->messages.emplace_back(std::move(id), nlohmann::json::parse(payload));
        });
    }
    std::pair<std::string, std::string> model_snapshot() const
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex);
        return {device_info.model_name, device_info.model_id};
    }

protected:
    bool init_device_info(const std::string& id, const std::string& host, const std::string&,
                          const std::string&, bool, const std::string&) override
    {
        device_info = {};
        device_info.dev_id = id;
        device_info.base_url = host;
        return true;
    }

    bool fetch_device_info(const std::string& host, const std::string&, MoonrakerDeviceInfo& info, std::string& error) const override
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->fetched.push_back(host);
        state->cv.notify_all();
        state->cv.wait(lock, [&]() { return host != state->block_fetch || state->released; });
        if (state->throw_fetch && host == state->block_fetch) throw 42;
        if (state->reject_fetch) { error = state->fetch_error; return false; }
        info.dev_name = host;
        return true;
    }

    bool query_printer_status(const std::string& host, const std::string&, nlohmann::json& status, std::string& error) const override
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->queried.push_back(host);
        state->cv.notify_all();
        state->cv.wait(lock, [&]() {
            return host != state->block_query || state->released ||
                   (state->block_query_at > 0 && state->queried.size() != state->block_query_at);
        });
        if (state->reject_query_at > 0 && state->queried.size() >= state->reject_query_at) {
            error = state->query_error;
            return false;
        }
        status = state->status;
        return true;
    }

    void run_status_stream(std::string, std::string host, std::string, uint64_t) override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->streamed.push_back(std::move(host));
        state->cv.notify_all();
        // Return without sockets, leaving a completed but joinable status thread.
    }

protected:
    std::shared_ptr<ConnectionState> state;
};

class FakePollingAgent final : public FakeMoonrakerAgent {
public:
    using FakeMoonrakerAgent::FakeMoonrakerAgent;
protected:
    void run_status_stream(std::string id, std::string host, std::string key, uint64_t generation) override
    {
        // Only HTTPS is used here. It enters production polling, whose only
        // I/O boundary is query_printer_status(), overridden without sockets.
        MoonrakerPrinterAgent::run_status_stream(std::move(id), std::move(host), std::move(key), generation);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->streamed.push_back("finished");
        state->cv.notify_all();
    }
    bool wait_status_poll(uint64_t) override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->poll_waits++ < state->polls;
    }
};

// G-code requests are recorded instead of posted; tests complete them later.
class FakeCommandAgent final : public FakeMoonrakerAgent {
public:
    using FakeMoonrakerAgent::FakeMoonrakerAgent;
protected:
    std::shared_ptr<Slic3r::Http> post_gcode_async(const std::string&, const std::string&, const std::string& gcode,
                                                   std::function<void(bool)> done) const override
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->commands.push_back(gcode);
        state->completions.push_back(std::move(done));
        return nullptr;
    }
};

class FakeReplyAgent final : public Slic3r::MoonrakerPrinterAgent {
public:
    FakeReplyAgent() : MoonrakerPrinterAgent("") {}
    nlohmann::json reply;
    bool server_info_valid()
    {
        MoonrakerDeviceInfo info;
        std::string error;
        return fetch_device_info("unused", "unused", info, error);
    }
    bool status_valid()
    {
        nlohmann::json status;
        std::string error;
        return query_printer_status("unused", "unused", status, error);
    }
protected:
    bool fetch_json(const std::string&, const std::string&, const std::string&,
                    nlohmann::json& response, std::string&) const override
    {
        response = reply;
        return true;
    }
};

Slic3r::PrinterConnectionParams connection(const std::string& id)
{
    Slic3r::PrinterConnectionParams params;
    params.dev_id = id;
    params.host = id;
    return params;
}

} // namespace

TEST_CASE("Moonraker keeps only the latest pending connection", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->block_fetch = "first";
    FakeMoonrakerAgent agent(state);
    REQUIRE(agent.connect_printer(connection("first")) == BAMBU_NETWORK_SUCCESS);
    REQUIRE(state->wait([&]() { return state->fetched.size() == 1; }));
    for (int i = 0; i < 20; ++i) agent.connect_printer(connection("superseded"));
    agent.connect_printer(connection("last"));
    state->release();
    REQUIRE(state->wait([&]() { return state->queued.size() == 2 && state->streamed.size() == 1; }));
    state->drain();
    agent.finish();
    CHECK(state->fetched == std::vector<std::string>{"first", "last"});
    CHECK(state->queried == std::vector<std::string>{"last"});
    CHECK(state->streamed == std::vector<std::string>{"last"});
    CHECK(state->connected == std::vector<std::string>{"last", "last"});
}

TEST_CASE("Moonraker disconnect cancels a connection waiting for initial status", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->block_query = "first";
    FakeMoonrakerAgent agent(state);
    agent.connect_printer(connection("first"));
    REQUIRE(state->wait([&]() { return state->queried.size() == 1; }));
    REQUIRE(agent.disconnect_printer() == BAMBU_NETWORK_SUCCESS);
    state->release();
    agent.finish();
    CHECK(state->streamed.empty());
    CHECK(state->queued.empty());
}

TEST_CASE("Moonraker joins each status worker before replacing it", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    FakeMoonrakerAgent agent(state);
    for (std::size_t i = 0; i < 30; ++i) {
        agent.connect_printer(connection(std::to_string(i)));
        REQUIRE(state->wait([&]() { return state->queued.size() == 2 && state->streamed.size() == i + 1; }));
        state->drain();
    }
    agent.finish();
    CHECK(state->connected.size() == 60);
    CHECK(state->streamed.size() == 30);
}

TEST_CASE("Moonraker discards queued connection callbacks after disconnect or destruction", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    {
        FakeMoonrakerAgent agent(state);
        agent.connect_printer(connection("first"));
        REQUIRE(state->wait([&]() { return state->queued.size() == 2; }));
        agent.disconnect_printer();
        state->drain();
        CHECK(state->connected.empty());
        agent.connect_printer(connection("second"));
        REQUIRE(state->wait([&]() { return state->queued.size() == 2; }));
    }
    state->drain();
    CHECK(state->connected.empty());
}

TEST_CASE("Moonraker contains worker exceptions and accepts the next connection", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->block_fetch = "throws";
    state->throw_fetch = true;
    FakeMoonrakerAgent agent(state);
    agent.connect_printer(connection("throws"));
    REQUIRE(state->wait([&]() { return state->fetched.size() == 1; }));
    agent.connect_printer(connection("next"));
    state->release();
    REQUIRE(state->wait([&]() { return state->queued.size() == 2; }));
    state->drain();
    agent.finish();
    CHECK(state->connected == std::vector<std::string>{"next", "next"});
}

TEST_CASE("Moonraker reports rejected initial connections instead of staying connecting", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->reject_fetch = true;
    state->fetch_error = GENERATE("HTTP error: 401", "HTTP error: 403", "Connection failed at port 1401");
    FakeMoonrakerAgent agent(state);
    agent.connect_printer(connection("rejected"));
    REQUIRE(state->wait([&]() { return state->queued.size() == 1; }));
    state->drain();
    agent.finish();
    REQUIRE(state->connection_states.size() == 1);
    CHECK(state->connection_states.front() == Slic3r::ConnectStatusFailed);
    REQUIRE(state->reasons.size() == 1);
    CHECK(state->reasons.front() == (state->fetch_error.rfind("HTTP error:", 0) == 0 ? "5" : "connection_failed"));
    CHECK(state->queried.empty());
    CHECK(state->streamed.empty());
}

TEST_CASE("Moonraker reports unsupported vendor operations without pretending success", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    FakeMoonrakerAgent agent(state);
    agent.connect_printer(connection("one"));
    REQUIRE(state->wait([&]() { return state->queued.size() == 2; }));
    // This reserved command has no transport case. The fake overrides every
    // connection/status transport path and no G-code branch is exercised.
    CHECK(agent.send_message_to_printer("one", R"({"print":{"command":"__offline_unsupported_test__"}})", 0, 0)
          == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    agent.finish();
}

TEST_CASE("Saved Moonraker connections keep their own profile identity without an active GUI preset", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto first = std::make_shared<ConnectionState>(), second = std::make_shared<ConnectionState>();
    FakeMoonrakerAgent a(first), b(second);
    auto p = connection("one");
    p.port = "7125"; p.model_name = "Saved model A"; p.model_id = "A";
    REQUIRE(a.connect_printer(p) == 0);
    p.dev_id = "two"; p.host = "two"; p.model_name = "Saved model B"; p.model_id = "B";
    REQUIRE(b.connect_printer(p) == 0);
    REQUIRE(first->wait([&] { return !first->streamed.empty(); }));
    REQUIRE(second->wait([&] { return !second->streamed.empty(); }));
    CHECK(a.model_snapshot() == std::make_pair(std::string("Saved model A"), std::string("A")));
    CHECK(b.model_snapshot() == std::make_pair(std::string("Saved model B"), std::string("B")));
    CHECK(first->streamed.front() == "http://one:7125");
    CHECK(second->streamed.front() == "http://two:7125");
}

TEST_CASE("Moonraker requires a valid status snapshot before reporting connected", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->reject_query_at = 1;
    state->query_error = GENERATE("HTTP error: 401", "HTTP error: 403", "Connection failed");
    FakeMoonrakerAgent agent(state);
    agent.collect_messages();
    agent.connect_printer(connection("unavailable"));
    REQUIRE(state->wait([&] { return state->queued.size() == 1; }));
    state->drain();
    agent.finish();
    CHECK(state->connection_states == std::vector<int>{Slic3r::ConnectStatusFailed});
    CHECK(state->reasons == std::vector<std::string>{state->query_error == "Connection failed" ? "status_unavailable" : "5"});
    CHECK(state->messages.empty());
    CHECK(state->streamed.empty());
}

TEST_CASE("Moonraker rejects malformed or empty initial telemetry", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->status = GENERATE(nlohmann::json(), nlohmann::json::array(), nlohmann::json::object());
    FakeMoonrakerAgent agent(state);
    agent.connect_printer(connection("malformed"));
    REQUIRE(state->wait([&] { return state->queued.size() == 1; }));
    state->drain();
    agent.finish();
    CHECK(state->connection_states == std::vector<int>{Slic3r::ConnectStatusFailed});
    CHECK(state->streamed.empty());
}

TEST_CASE("Moonraker publishes the initial snapshot and retires queued telemetry", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    FakeMoonrakerAgent agent(state);
    agent.collect_messages();
    agent.connect_printer(connection("initial"));
    REQUIRE(state->wait([&] { return state->queued.size() == 3; }));
    state->drain();
    REQUIRE(state->messages.size() == 1);
    CHECK(state->messages.front().first == "initial");
    CHECK(state->messages.front().second["print"]["nozzle_temper"] == 23.0);
    CHECK(state->messages.front().second["print"]["gcode_state"] == "IDLE");
    agent.connect_printer(connection("replacement"));
    REQUIRE(state->wait([&] { return state->queued.size() == 3; }));
    agent.disconnect_printer();
    state->drain();
    agent.finish();
    CHECK(state->messages.size() == 1);
}

TEST_CASE("HTTPS Moonraker monitoring keeps requesting fresh status", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->polls = 2;
    FakePollingAgent agent(state);
    agent.collect_messages();
    auto p = connection("secure");
    p.use_ssl = true;
    p.port = "7125";
    p.model_name = "saved printer";
    REQUIRE(agent.connect_printer(p) == BAMBU_NETWORK_SUCCESS);
    REQUIRE(state->wait([&] { return !state->streamed.empty(); }));
    state->drain();
    agent.finish();
    CHECK(state->queried == std::vector<std::string>(3, "https://secure:7125"));
    REQUIRE(state->messages.size() == 3);
    for (const auto& message : state->messages) CHECK(message.first == "secure");
    CHECK(state->connection_states == std::vector<int>{Slic3r::ConnectStatusOk});
}

TEST_CASE("HTTPS Moonraker monitoring reports lost status without replaying its cache", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->polls = 10;
    state->reject_query_at = 3;
    state->query_error = GENERATE("HTTP error: 403", "Connection failed");
    FakePollingAgent agent(state);
    agent.collect_messages();
    auto p = connection("secure");
    p.use_ssl = true;
    p.port = "7125";
    p.model_name = "saved printer";
    agent.connect_printer(p);
    REQUIRE(state->wait([&] { return !state->streamed.empty(); }));
    state->drain();
    agent.finish();
    CHECK(state->queried.size() == 3);
    CHECK(state->messages.size() == 2);
    CHECK(state->connection_states == std::vector<int>{Slic3r::ConnectStatusOk,
          state->query_error == "Connection failed" ? Slic3r::ConnectStatusLost : Slic3r::ConnectStatusFailed});
    CHECK(state->reasons.back() == (state->query_error == "Connection failed" ? "status_unavailable" : "5"));
}

TEST_CASE("Moonraker verifies its server and status response envelopes", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    FakeReplyAgent agent;
    agent.reply = {{"result", {{"moonraker_version", "v0.9.3"}, {"klippy_state", "ready"}}}};
    CHECK(agent.server_info_valid());
    agent.reply = {{"result", {{"status", {{"extruder", {{"temperature", 23.0}}}}}}}};
    CHECK(agent.status_valid());
    for (const auto& reply : {nlohmann::json(), nlohmann::json::array(), nlohmann::json::object(),
                             nlohmann::json{{"error", {{"code", 401}}}},
                             nlohmann::json{{"result", nullptr}},
                             nlohmann::json{{"result", {{"moonraker_version", 1}, {"status", nullptr}}}},
                             nlohmann::json{{"result", {{"moonraker_version", ""}, {"status", nlohmann::json::object()}}}}}) {
        agent.reply = reply;
        CHECK_FALSE(agent.server_info_valid());
        CHECK_FALSE(agent.status_valid());
    }
}

TEST_CASE("Moonraker rejects missing local files and unsupported SD-card printing", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    // These paths must not start a worker, inspect files or call a transport.
    FakeReplyAgent agent;
    CHECK(agent.start_print({}, {}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_local_print_with_record({}, {}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_sdcard_print({}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("HTTPS Moonraker discards a status request completed after disconnect", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    state->polls = 1;
    state->block_query = "https://secure:7125";
    state->block_query_at = 2;
    FakePollingAgent agent(state);
    agent.collect_messages();
    auto p = connection("secure");
    p.use_ssl = true;
    p.port = "7125";
    p.model_name = "saved printer";
    agent.connect_printer(p);
    REQUIRE(state->wait([&] { return state->queried.size() == 2; }));
    state->drain();
    REQUIRE(state->messages.size() == 1);
    agent.disconnect_printer();
    state->release();
    agent.finish();
    state->drain();
    CHECK(state->messages.size() == 1);
}

TEST_CASE("Moonraker G-code completions reply while connected and never reach a destroyed agent", "[MoonrakerPrinterAgent][MoonrakerOffline]")
{
    auto state = std::make_shared<ConnectionState>();
    std::function<void(bool)> late;
    {
        FakeCommandAgent agent(state);
        agent.collect_messages();
        agent.connect_printer(connection("one"));
        REQUIRE(state->wait([&] { return state->queued.size() == 3; }));
        state->drain();
        REQUIRE(state->messages.size() == 1);
        CHECK(agent.send_message_to_printer("one", R"({"print":{"command":"gcode_line","param":"M115","sequence_id":"7"}})", 0, 0) == 0);
        CHECK(agent.send_message_to_printer("one", R"({"print":{"command":"gcode_line","param":"M114"}})", 0, 0) == 0);
        REQUIRE(state->completions.size() == 2);
        CHECK(state->commands == std::vector<std::string>{"M115", "M114"});
        state->completions[0](true);
        state->drain();
        REQUIRE(state->messages.size() == 2);
        CHECK(state->messages.back().second["print"]["result"] == "success");
        CHECK(state->messages.back().second["print"]["sequence_id"] == "7");
        late = std::move(state->completions[1]);
    }
    // The request thread may finish after its agent is destroyed.
    late(false);
    state->drain();
    CHECK(state->messages.size() == 2);
}

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

using namespace Slic3r;

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;
using namespace std::chrono_literals;

// A self-signed certificate for 127.0.0.1, valid until 2126; the agent does not verify it.
const char* const TEST_CERTIFICATE = R"(-----BEGIN CERTIFICATE-----
MIIBfjCCASWgAwIBAgIUQ6GXkNGttqdwgzP8jnIbjZmbmsUwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MTAyNTIyMzQzNloYDzIxMjYxMDAx
MjIzNDM2WjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAQzyTk4fzS67d2QTOgff5VZdiFu6zfRKL1j1XrmZRdSNSQ+A/ecb+RA
wYLxO+ImIhC9nfFDz83gN6HMdIDhKZqvo1MwUTAdBgNVHQ4EFgQUqLCEdBHexenh
R0myp07v6RmToacwHwYDVR0jBBgwFoAUqLCEdBHexenhR0myp07v6RmToacwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiBtuziYmXx6OICr8OZ/ljgW
206TTprzJEsote+V0YwmawIgCTzg5n5sn/JH8DSUh7udfljYTtgIwlfqiqLZ9Y79
Xz8=
-----END CERTIFICATE-----
)";
const char* const TEST_PRIVATE_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgvrE3Z+AMBnG/JEQl
PP9feoILjFkSRm5XAYES7mSg6JWhRANCAAQzyTk4fzS67d2QTOgff5VZdiFu6zfR
KL1j1XrmZRdSNSQ+A/ecb+RAwYLxO+ImIhC9nfFDz83gN6HMdIDhKZqv
-----END PRIVATE KEY-----
)";

class DiscoveryAgent : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::MoonrakerPrinterAgent;
    using MoonrakerPrinterAgent::device_info;
    mutable unsigned fetch_count = 0;

protected:
    bool fetch_device_info(
        const std::string&, const std::string&, MoonrakerDeviceInfo& info, std::string&, const std::function<bool()>&) const override
    {
        ++fetch_count;
        info.dev_name = "Test printer";
        return true;
    }
};

// Connects without the app's printer preset, and counts the device info queries that have returned.
class ConnectAgent : public MoonrakerPrinterAgent
{
public:
    using MoonrakerPrinterAgent::MoonrakerPrinterAgent;
    std::shared_ptr<std::atomic<int>> fetches_returned;

protected:
    bool init_device_info(
        const std::string& dev_id, const std::string& dev_ip, const std::string&, const std::string&, bool, const std::string& port) override
    {
        device_info          = MoonrakerDeviceInfo{};
        device_info.dev_id   = dev_id;
        device_info.base_url = "http://" + dev_ip + ":" + port;
        return true;
    }

    bool fetch_device_info(const std::string&           base_url,
                           const std::string&           api_key,
                           MoonrakerDeviceInfo&         info,
                           std::string&                 error,
                           const std::function<bool()>& cancelled) const override
    {
        // Copied first, since the base destructor that waits for this query runs after this class's members are gone
        const std::shared_ptr<std::atomic<int>> counter = fetches_returned;
        const bool fetched = MoonrakerPrinterAgent::fetch_device_info(base_url, api_key, info, error, cancelled);
        if (counter)
            ++*counter;
        return fetched;
    }
};

class StreamAgent : public ConnectAgent
{
public:
    using ConnectAgent::ConnectAgent;
    using MoonrakerPrinterAgent::start_status_stream;
    using MoonrakerPrinterAgent::stop_status_stream;
    using MoonrakerPrinterAgent::ws_timings;

protected:
    bool fetch_device_info(
        const std::string&, const std::string&, MoonrakerDeviceInfo& info, std::string&, const std::function<bool()>&) const override
    {
        info.dev_name = "Test printer";
        return true;
    }
};

// Fails its first `refusals` device info queries, as when macOS refuses the app's first local network connections.
class RefusedAgent : public StreamAgent
{
public:
    using StreamAgent::StreamAgent;
    int refusals = 0;
    mutable std::atomic<int> fetches{0};

protected:
    bool fetch_device_info(const std::string&           base_url,
                           const std::string&           api_key,
                           MoonrakerDeviceInfo&         info,
                           std::string&                 error,
                           const std::function<bool()>& cancelled) const override
    {
        if (++fetches <= refusals) {
            error = "Couldn't connect to server";
            return false;
        }
        return StreamAgent::fetch_device_info(base_url, api_key, info, error, cancelled);
    }
};

enum class Behavior {
    SilentBeforeHandshake,     // Accepts the connection and never answers
    Silent,                    // Completes the upgrade, then never reads or sends
    AnswersPings,              // Reports a status, then answers pings and sends nothing else
    AnswersPingsWithoutStatus, // Answers pings but never reports a status
    Chatters,                  // Reports a status, then answers pings and sends notifications that carry no status
    HangsUp,                   // Reports a status, then closes the websocket
    HangsUpWithoutAnswer,      // Closes the websocket without sending anything
    IgnoresClose,              // Reports Klippy disconnected, then never answers the close
    StallsObjectList,          // Never answers the stream's object list request
    StallsHttp,                // Never answers HTTP requests until released, then hangs up on them
};

class FakePrinter
{
public:
    explicit FakePrinter(Behavior behavior, bool tls = false) : m_behavior(behavior), m_tls(tls)
    {
        m_tls_context.use_certificate_chain(net::buffer(std::string_view(TEST_CERTIFICATE)));
        m_tls_context.use_private_key(net::buffer(std::string_view(TEST_PRIVATE_KEY)), ssl::context::pem);
        m_accept_thread = std::thread([this] { accept_loop(); });
    }

    ~FakePrinter()
    {
        release();
        m_stopping = true;
        // Wakes the blocking accept.
        tcp::socket wake{m_accept_ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        m_accept_thread.join();
        for (std::thread& connection : m_connections)
            connection.join();
    }

    std::string host() const { return "127.0.0.1"; }
    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }
    std::string base_url() const { return (m_tls ? "https://" : "http://") + host() + ":" + port(); }

    // A connection is ready once it reaches the state its behavior waits in.
    bool wait_for_ready_connections(int count, std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] { return m_ready >= count; });
    }

    bool wait_for_http_requests(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_http_requests >= count; });
    }

    // Waits until the printer, once released, has hung up on `count` stalled HTTP requests.
    bool wait_for_http_hang_ups(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_http_hung_up >= count; });
    }

    int close_frames()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_close_frames;
    }

    // The printer answers a close frame before its own read completes, so the agent's stop can return first.
    bool wait_for_close_frames(int count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_close_frames >= count; });
    }

    // The answer to the gcodes directory listing, which reports the free space in disk_usage. Null leaves disk_usage out.
    void set_gcodes_listing(nlohmann::json disk_usage)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_disk_usage = std::move(disk_usage);
    }

    // Hangs up every connection that is waiting.
    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    void accept_loop()
    {
        for (;;) {
            // Each connection gets its own io_context, so a connection that runs one never runs another's handlers.
            auto ioc = std::make_unique<net::io_context>();
            tcp::socket socket{*ioc};
            beast::error_code ec;
            m_acceptor.accept(socket, ec);
            if (ec || m_stopping)
                return;
            m_connections.emplace_back(
                [this, ioc = std::move(ioc), socket = std::move(socket)]() mutable { serve(*ioc, std::move(socket)); });
        }
    }

    void serve(net::io_context& ioc, tcp::socket socket)
    {
        if (m_behavior == Behavior::SilentBeforeHandshake) {
            mark_ready();
            wait_for_release();
            return;
        }
        if (!m_tls) {
            serve_stream(ioc, std::move(socket));
            return;
        }
        ssl::stream<tcp::socket> stream{std::move(socket), m_tls_context};
        beast::error_code ec;
        stream.handshake(ssl::stream_base::server, ec);
        if (!ec)
            serve_stream(ioc, std::move(stream));
    }

    template<class Stream> void serve_stream(net::io_context& ioc, Stream stream)
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code ec;
        http::read(stream, buffer, request, ec);
        if (ec)
            return;
        // The agent also queries the printer's status and object list over HTTP.
        if (!websocket::is_upgrade(request)) {
            serve_http(stream, request);
            return;
        }

        websocket::stream<Stream> ws{std::move(stream)};
        ws.accept(request, ec);
        // Without its object list the stream never sends the subscribe request.
        const int requests = m_behavior == Behavior::StallsObjectList ? 1 : 2;
        for (int i = 0; i < requests && !ec; ++i) {
            beast::flat_buffer message;
            ws.read(message, ec);
        }
        if (!ec && (m_behavior == Behavior::AnswersPings || m_behavior == Behavior::Chatters || m_behavior == Behavior::HangsUp)) {
            const std::string status =
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"status", {{"print_stats", {{"state", "standby"}}}}}}}}.dump();
            ws.write(net::buffer(status), ec);
        }
        if (ec)
            return;

        switch (m_behavior) {
        case Behavior::AnswersPings:
        case Behavior::AnswersPingsWithoutStatus:
        case Behavior::Chatters:
            mark_ready();
            answer_pings(ioc, ws, m_behavior == Behavior::Chatters);
            break;
        case Behavior::HangsUp:
        case Behavior::HangsUpWithoutAnswer:
            ws.close(websocket::close_code::normal, ec);
            mark_ready();
            break;
        case Behavior::IgnoresClose: {
            const std::string notification = nlohmann::json{{"jsonrpc", "2.0"}, {"method", "notify_klippy_disconnected"}}.dump();
            ws.write(net::buffer(notification), ec);
            mark_ready();
            wait_for_release();
            break;
        }
        case Behavior::StallsObjectList: answer_pings(ioc, ws, false); break;
        default: mark_ready(); wait_for_release();
        }
    }

    template<class Stream> void serve_http(Stream& stream, const http::request<http::string_body>& request)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_http_requests;
            m_changed.notify_all();
        }
        if (m_behavior == Behavior::StallsObjectList) {
            mark_ready();
            wait_for_release();
            return;
        }
        if (m_behavior == Behavior::StallsHttp) {
            wait_for_release();
            beast::error_code ec;
            beast::get_lowest_layer(stream).close(ec);
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_http_hung_up;
            m_changed.notify_all();
            return;
        }
        http::response<http::string_body> response{http::status::not_found, request.version()};
        if (request.target() == "/server/files/directory?path=gcodes") {
            nlohmann::json result = {{"dirs", nlohmann::json::array()}, {"files", nlohmann::json::array()}};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_disk_usage.is_null())
                    result["disk_usage"] = m_disk_usage;
            }
            response.result(http::status::ok);
            response.body() = nlohmann::json{{"result", result}}.dump();
        }
        response.prepare_payload();
        beast::error_code ec;
        http::write(stream, response, ec);
    }

    // Keeps a read pending, which answers pings, until released or closed, and with `chatter` also sends a notification every 50 ms.
    template<class WebSocket> void answer_pings(net::io_context& ioc, WebSocket& ws, bool chatter)
    {
        const std::string notification = nlohmann::json{{"jsonrpc", "2.0"},
                                                        {"method", "notify_proc_stat_update"},
                                                        {"params", nlohmann::json::array({nlohmann::json::object()})}}
                                             .dump();
        beast::flat_buffer message;
        bool reading                    = true;
        bool writing                    = false;
        std::function<void()> read_next = [&] {
            ws.async_read(message, [&](beast::error_code ec, std::size_t) {
                message.clear();
                if (ec == websocket::error::closed) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_close_frames;
                    m_changed.notify_all();
                }
                if (ec)
                    reading = false;
                else
                    read_next();
            });
        };
        read_next();

        auto next_notification = std::chrono::steady_clock::now();
        while (reading && !wait_for_release(10ms)) {
            if (chatter && !writing && std::chrono::steady_clock::now() >= next_notification) {
                writing = true;
                next_notification += 50ms;
                ws.async_write(net::buffer(notification), [&](beast::error_code, std::size_t) { writing = false; });
            }
            ioc.restart();
            ioc.run_for(20ms);
        }
        beast::error_code ignored;
        beast::get_lowest_layer(ws).close(ignored);
        while (reading || writing) {
            ioc.restart();
            ioc.run_for(20ms);
        }
    }

    void mark_ready()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_ready;
        m_changed.notify_all();
    }

    void wait_for_release()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    bool wait_for_release(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [this] { return m_released; });
    }

    const Behavior m_behavior;
    const bool m_tls;
    ssl::context m_tls_context{ssl::context::tls_server};
    net::io_context m_accept_ioc;
    tcp::acceptor m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool> m_stopping{false};
    std::thread m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    int m_ready         = 0;
    int m_http_requests = 0;
    int m_http_hung_up  = 0;
    int m_close_frames  = 0;
    bool m_released     = false;
    nlohmann::json m_disk_usage = {{"total", 8e9}, {"used", 2e9}, {"free", 6e9}};
};

// Records when the agent dispatches a status report.
class Reports
{
public:
    void attach(StreamAgent& agent)
    {
        agent.set_on_local_message_fn([this](std::string, std::string) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_times.push_back(std::chrono::steady_clock::now());
            m_changed.notify_all();
        });
    }

    bool wait_for(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, 10s, [&] { return m_times.size() >= count; });
    }

    std::vector<std::chrono::steady_clock::time_point> times()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_times;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::chrono::steady_clock::time_point> m_times;
};

// A printer and a stream agent connected to it.
struct StreamFixture
{
    explicit StreamFixture(Behavior behavior, bool tls = false) : printer(behavior, tls) {}

    // Runs before the members are destroyed, so the agent's destructor never joins a stream the printer still holds open.
    ~StreamFixture() { printer.release(); }

    void start() { agent.start_status_stream("printer", printer.base_url(), ""); }

    // Stops the stream, giving it `limit` to return before the printer hangs up on it.
    bool stops_within(std::chrono::milliseconds limit)
    {
        auto stopped       = std::async(std::launch::async, [this] { agent.stop_status_stream(); });
        const bool in_time = stopped.wait_for(limit) == std::future_status::ready;
        printer.release();
        stopped.wait();
        return in_time;
    }

    FakePrinter printer;
    StreamAgent agent{""};
};

PrinterConnectionParams connection_to(const FakePrinter& printer)
{
    PrinterConnectionParams params;
    params.dev_id = "printer";
    params.host   = printer.host();
    params.port   = printer.port();
    return params;
}

// Destroys the agent, giving it `limit` to return.
template<class Agent> bool destroys_within(std::unique_ptr<Agent>& agent, std::chrono::milliseconds limit)
{
    auto destroyed = std::async(std::launch::async, [&] { agent.reset(); });
    return destroyed.wait_for(limit) == std::future_status::ready;
}

} // namespace

TEST_CASE("Moonraker discovery waits for a complete device identity", "[MoonrakerPrinterAgent][Discovery]")
{
    DiscoveryAgent agent("");
    unsigned announcements = 0;
    nlohmann::json announcement;
    agent.set_on_ssdp_msg_fn([&](std::string payload) {
        ++announcements;
        announcement = nlohmann::json::parse(payload);
    });

    agent.start_discovery(true, false);
    REQUIRE(announcements == 0);
    REQUIRE(agent.fetch_count == 0);

    agent.device_info.dev_id = "127.0.0.1:7125";
    agent.device_info.dev_ip = agent.device_info.dev_id;
    agent.device_info.base_url = "http://127.0.0.1:7125";
    agent.device_info.model_name = "Test model";
    agent.start_discovery(true, false);
    REQUIRE(announcements == 1);
    REQUIRE(agent.fetch_count == 0);
    REQUIRE(announcement.at("dev_id") == agent.device_info.dev_id);
    REQUIRE(announcement.at("dev_ip") == agent.device_info.dev_ip);
    REQUIRE(announcement.at("dev_name") == "Test model");
}

TEST_CASE("Stopping the status stream gives a silent printer no more than its timeouts", "[MoonrakerPrinterAgent][StatusStream]")
{
    // An open websocket gets the 200 ms close timeout, and a stalled object list request up to 1 s of curl progress to see the stop.
    const auto [behavior, limit] = GENERATE(table<Behavior, std::chrono::milliseconds>({
        {Behavior::SilentBeforeHandshake, 500ms},
        {Behavior::Silent, 1000ms},
        {Behavior::StallsObjectList, 2000ms},
    }));
    CAPTURE(static_cast<int>(behavior));

    StreamFixture f(behavior);
    f.agent.ws_timings.close = 200ms;
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    CHECK(f.stops_within(limit));
}

TEST_CASE("Stopping the status stream sends the printer a close frame", "[MoonrakerPrinterAgent][StatusStream]")
{
    // A stop while the stream reads, and one while it waits on the object list request.
    const Behavior behavior = GENERATE(Behavior::AnswersPings, Behavior::StallsObjectList);
    CAPTURE(static_cast<int>(behavior));

    StreamFixture f(behavior);
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    CHECK(f.stops_within(3s));
    CHECK(f.printer.wait_for_close_frames(1));
    CHECK(f.printer.close_frames() == 1);
}

TEST_CASE("Stopping the status stream does not wait out the reconnect delay", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUp);
    f.agent.ws_timings.reconnect_delay = 10s;
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));
    // Lets the stream reach its reconnect wait.
    std::this_thread::sleep_for(200ms);

    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream reconnects when the printer stops answering", "[MoonrakerPrinterAgent][StatusStream]")
{
    // An unanswered close ends at whichever of the handshake and close timeouts is shorter.
    const auto [behavior, handshake, close] = GENERATE(table<Behavior, std::chrono::milliseconds, std::chrono::milliseconds>({
        {Behavior::Silent, 300ms, 1000ms},
        {Behavior::IgnoresClose, 300ms, 1000ms},
        {Behavior::IgnoresClose, 1000ms, 200ms},
    }));
    CAPTURE(static_cast<int>(behavior), handshake.count(), close.count());

    StreamFixture f(behavior);
    f.agent.ws_timings.handshake       = handshake;
    f.agent.ws_timings.close           = close;
    f.agent.ws_timings.idle            = 400ms;
    f.agent.ws_timings.reconnect_delay = 20ms;
    f.start();

    CHECK(f.printer.wait_for_ready_connections(2));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream reconnects at the shortest delay while the printer answers", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUp);
    f.agent.ws_timings.reconnect_delay = 50ms;
    f.start();

    // Twelve 50 ms waits; a backoff that kept doubling would need over 10 s.
    CHECK(f.printer.wait_for_ready_connections(12, 3s));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream bounds retries when a printer never answers", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::HangsUpWithoutAnswer);
    f.agent.ws_timings.reconnect_delay = 1ms;
    f.start();

    CHECK(f.printer.wait_for_ready_connections(5));
    CHECK_FALSE(f.printer.wait_for_ready_connections(6, 250ms));
    CHECK(f.stops_within(3s));
}

TEST_CASE("A callback on the stream thread can stop the status stream", "[MoonrakerPrinterAgent][StatusStream]")
{
    std::promise<void> stopped;
    std::once_flag stopped_once;
    StreamFixture f(Behavior::AnswersPings);
    f.agent.set_on_local_message_fn([&](std::string, std::string) {
        std::call_once(stopped_once, [&] {
            f.agent.stop_status_stream();
            stopped.set_value();
        });
    });
    f.start();

    REQUIRE(stopped.get_future().wait_for(5s) == std::future_status::ready);
    CHECK(f.stops_within(3s));
}

TEST_CASE("Status reports tell the send dialog whether the gcodes storage has free space", "[MoonrakerPrinterAgent][StatusStream]")
{
    auto [disk_usage, has_storage] = GENERATE(table<nlohmann::json, bool>({
        {{{"total", 8e9}, {"used", 2e9}, {"free", 6e9}}, true},
        {{{"total", 8e9}, {"used", 8e9}, {"free", 0}}, false},
        {nullptr, false},
    }));
    CAPTURE(disk_usage.dump());

    std::promise<std::string> first_report;
    std::once_flag reported;
    StreamFixture f(Behavior::AnswersPings);
    f.printer.set_gcodes_listing(disk_usage);
    f.agent.set_on_local_message_fn([&](std::string, std::string payload) {
        std::call_once(reported, [&] { first_report.set_value(payload); });
    });
    f.start();

    auto report = first_report.get_future();
    REQUIRE(report.wait_for(5s) == std::future_status::ready);
    const nlohmann::json print = nlohmann::json::parse(report.get()).at("print");
    // MachineObject reads the storage state from "sdcard" and then again from home_flag bits 8-9.
    CHECK(print.at("sdcard") == has_storage);
    CHECK(((print.at("home_flag").get<int>() >> 8) & 0x3) == (has_storage ? DevStorage::HAS_SDCARD_NORMAL : DevStorage::NO_SDCARD));
    CHECK(f.stops_within(3s));
}

TEST_CASE("The status stream keeps re-reading the printer's free space", "[MoonrakerPrinterAgent][StatusStream]")
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<bool> storage_reports;
    // Chatters sends a message every 50 ms, which the stream re-reads the free space after
    StreamFixture f(Behavior::Chatters);
    f.agent.ws_timings.storage_check = 100ms;
    f.printer.set_gcodes_listing({{"total", 8e9}, {"used", 8e9}, {"free", 0}});
    f.agent.set_on_local_message_fn([&](std::string, std::string payload) {
        std::lock_guard<std::mutex> lock(mutex);
        storage_reports.push_back(nlohmann::json::parse(payload).at("print").at("sdcard").get<bool>());
        changed.notify_all();
    });
    const auto wait_for_storage = [&](bool has_storage) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return !storage_reports.empty() && storage_reports.back() == has_storage; });
    };
    f.start();
    REQUIRE(wait_for_storage(false));

    f.printer.set_gcodes_listing({{"total", 8e9}, {"used", 2e9}, {"free", 6e9}});
    REQUIRE(wait_for_storage(true));

    // A listing without disk usage cannot be checked, which counts as no storage
    f.printer.set_gcodes_listing(nullptr);
    CHECK(wait_for_storage(false));
    CHECK(f.stops_within(3s));
}

TEST_CASE("A quiet printer does not replay cached telemetry as a fresh report", "[MoonrakerPrinterAgent][StatusStream]")
{
    Reports reports;
    StreamFixture f(Behavior::AnswersPings);
    f.agent.ws_timings.idle      = 400ms;
    reports.attach(f.agent);
    f.start();

    REQUIRE(reports.wait_for(1));
    std::this_thread::sleep_for(1000ms);
    CHECK(reports.times().size() == 1);
    CHECK(f.stops_within(3s));
}

// HTTPS monitoring uses the existing HTTP transport; the fake polling
// cases above cover its lifecycle without a self-signed websocket endpoint.

TEST_CASE("The status stream sends no heartbeat while the printer has nothing new to report", "[MoonrakerPrinterAgent][StatusStream]")
{
    const Behavior behavior = GENERATE(Behavior::Chatters, Behavior::AnswersPingsWithoutStatus);
    CAPTURE(static_cast<int>(behavior));

    Reports reports;
    StreamFixture f(behavior);
    f.agent.ws_timings.idle      = 400ms;
    reports.attach(f.agent);
    f.start();
    REQUIRE(f.printer.wait_for_ready_connections(1));

    // The chattering printer's status, and nothing from the one that never sent a status.
    const std::size_t expected = behavior == Behavior::Chatters ? 1 : 0;
    std::this_thread::sleep_for(1500ms);
    CHECK(reports.times().size() == expected);
    CHECK(f.stops_within(3s));
}

TEST_CASE("A disconnect during the connection queries keeps the status stream from starting", "[MoonrakerPrinterAgent][StatusStream]")
{
    StreamFixture f(Behavior::StallsHttp);
    f.agent.connect_printer(connection_to(f.printer));
    REQUIRE(f.printer.wait_for_http_requests(1));

    f.agent.disconnect_printer();
    f.printer.release();
    // The query has returned once the printer hangs up on it, and a stream would connect right after.
    REQUIRE(f.printer.wait_for_http_hang_ups(1));
    CHECK_FALSE(f.printer.wait_for_ready_connections(1, 500ms));
}

TEST_CASE("Destroying the agent waits for the connection threads it superseded", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::StallsHttp);
    const auto fetches_returned = std::make_shared<std::atomic<int>>(0);
    auto agent                  = std::make_unique<ConnectAgent>("");
    agent->fetches_returned     = fetches_returned;

    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(1));
    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(2));
    agent->disconnect_printer();

    // The stalled queries see the change within 1 s of curl progress.
    CHECK(destroys_within(agent, 2s));
    CHECK(*fetches_returned == 2);
}

TEST_CASE("Destroying the agent stops a stalled status query", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::StallsHttp);
    auto agent = std::make_unique<StreamAgent>("");
    agent->connect_printer(connection_to(printer));
    REQUIRE(printer.wait_for_http_requests(1));

    // The stalled query sees the change within 1 s of curl progress.
    CHECK(destroys_within(agent, 2s));
}

TEST_CASE("A refused connection returns control to the saved-device retry policy", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::AnswersPings);
    RefusedAgent agent("");
    agent.refusals                   = 2;
    agent.ws_timings.reconnect_delay = 10ms;
    agent.connect_printer(connection_to(printer));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (agent.fetches == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    CHECK(agent.fetches == 1);
    CHECK_FALSE(printer.wait_for_ready_connections(1, 100ms));
    printer.release();
}

TEST_CASE("Destroying the agent does not wait out a connection retry's delay", "[MoonrakerPrinterAgent][Connect]")
{
    FakePrinter printer(Behavior::AnswersPings);
    auto agent                        = std::make_unique<RefusedAgent>("");
    agent->refusals                   = 1;
    agent->ws_timings.reconnect_delay = 10s;
    agent->connect_printer(connection_to(printer));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (agent->fetches == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(agent->fetches == 1);

    CHECK(destroys_within(agent, 1s));
}
