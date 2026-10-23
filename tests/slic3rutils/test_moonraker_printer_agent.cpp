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

TEST_CASE("Moonraker keeps only the latest pending connection", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker disconnect cancels a connection waiting for initial status", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker joins each status worker before replacing it", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker discards queued connection callbacks after disconnect or destruction", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker contains worker exceptions and accepts the next connection", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker reports rejected initial connections instead of staying connecting", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker reports unsupported vendor operations without pretending success", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Saved Moonraker connections keep their own profile identity without an active GUI preset", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker requires a valid status snapshot before reporting connected", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker rejects malformed or empty initial telemetry", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker publishes the initial snapshot and retires queued telemetry", "[MoonrakerPrinterAgent]")
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

TEST_CASE("HTTPS Moonraker monitoring keeps requesting fresh status", "[MoonrakerPrinterAgent]")
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

TEST_CASE("HTTPS Moonraker monitoring reports lost status without replaying its cache", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker verifies its server and status response envelopes", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Unimplemented Moonraker print entry points return unsupported", "[MoonrakerPrinterAgent]")
{
    // These paths must not start a worker, inspect files or call a transport.
    FakeReplyAgent agent;
    CHECK(agent.start_print({}, {}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_local_print_with_record({}, {}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
    CHECK(agent.start_sdcard_print({}, {}, {}) == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED);
}

TEST_CASE("HTTPS Moonraker discards a status request completed after disconnect", "[MoonrakerPrinterAgent]")
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

TEST_CASE("Moonraker G-code completions reply while connected and never reach a destroyed agent", "[MoonrakerPrinterAgent]")
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
