#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/Utils/BambuLanProcess.hpp"
#include "slic3r/Utils/BambuLanProtocol.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/PrinterSessionRegistry.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/path.hpp>
#include <array>
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
// The only executable launched is the built headless worker, given only the
// explicitly built fake SDK path. Its overridden SDK exports have no network,
// discovery or physical commands. OS pipes/processes and local asset reads only.
struct Signals {
#ifndef _WIN32
    using Handler = void(*)(int);
    Handler previous = std::signal(SIGPIPE, SIG_IGN);
    ~Signals() { std::signal(SIGPIPE, previous); }
#endif
};
std::string sdk()
{ return (boost::dll::program_location().parent_path() / PORCA_FAKE_BBL_PLUGIN).string(); }
Slic3r::BambuLanProcessConfig config(const std::string& mode)
{
    const auto dir = boost::dll::program_location().parent_path();
    return {(dir / PORCA_BAMBU_WORKER).string(), sdk(), mode, std::string(TEST_DATA_DIR) + "/../../resources", "US"};
}
Slic3r::PrinterConnectionParams params(const std::string& id)
{
    Slic3r::PrinterConnectionParams p;
    p.dev_id = id; p.host = "offline-fixture"; p.password = "fixture"; p.username = "bblp";
    return p;
}
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> messages, reasons;
    std::vector<std::function<void()>> queue;
    template<class Fn> bool wait(Fn ready)
    { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock, std::chrono::seconds(10), ready); }
};
}

TEST_CASE("A vendor first-only or reused handle is distinguished without destroying its current owner", "[BambuProcessSessions]")
{
    const std::string mode = GENERATE("first-only", "reuse-singleton");
    Slic3r::BambuAgentError failure;
    auto first = Slic3r::BBLNetworkPlugin::load_device_plugin(sdk(), mode, &failure);
    REQUIRE(first);
    const auto count = reinterpret_cast<int(*)()>(first->get_network_function("porca_fake_active_agents"));
    REQUIRE(count);
    const int before = count();
    auto second = Slic3r::BBLNetworkPlugin::load_device_plugin(sdk(), mode, &failure);
    CHECK_FALSE(second);
    CHECK(failure == (mode == "first-only" ? Slic3r::BambuAgentError::null_handle : Slic3r::BambuAgentError::duplicate_handle));
    CHECK(count() == before);
    Slic3r::BBLPrinterAgent owner(first);
    CHECK(owner.connect_printer(params("one")) == 0);
    CHECK(owner.send_message_to_printer("one", "{}", 0, 0) == 0);
}

TEST_CASE("Four process-isolated Bambu sessions survive a process-wide single-agent SDK", "[BambuProcessSessions]")
{
    Signals signals;
    const std::string mode = GENERATE("first-only", "reuse-singleton");
    // Occupy the fake SDK in the parent too. Reopening this module would fail or
    // alias, so independent success requires fresh child address spaces.
    auto account = Slic3r::BBLNetworkPlugin::load_device_plugin(sdk(), mode);
    REQUIRE(account);
    State state;
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_local_connect_fn([&](int, std::string, std::string) { std::lock_guard<std::mutex> lock(state.mutex); state.cv.notify_all(); });
    network.set_on_local_message_fn([&](std::string id, std::string) {
        std::lock_guard<std::mutex> lock(state.mutex); state.messages.push_back(id); state.cv.notify_all();
    });
    std::array<std::shared_ptr<Slic3r::BambuLanProcess>,4> agents;
    for (size_t i=0; i<agents.size(); ++i) {
        agents[i] = std::make_shared<Slic3r::BambuLanProcess>(config(mode));
        REQUIRE(network.connect_printer(params(std::to_string(i)), agents[i]) == 0);
    }
    REQUIRE(state.wait([&] {
        for (size_t i=0; i<agents.size(); ++i)
            if (network.printer_session_token(std::to_string(i))->state != Slic3r::PrinterControlState::connected) return false;
        return true;
    }));
    CHECK_FALSE(network.get_printer_agent());
    for (size_t i=0; i<agents.size(); ++i) {
        const auto id = std::to_string(i);
        network.set_printer_agent(agents[i]);
        const auto token = network.printer_session_token(id);
        REQUIRE(network.connect_printer(params(id)) == 0);
        CHECK(network.printer_session_token(id) == token);
        CHECK(network.send_message_to_printer(id, R"({"print":{"command":"pause"}})", 0, 0) == 0);
    }
    REQUIRE(state.wait([&] { return state.messages.size() == 4; }));
    std::sort(state.messages.begin(), state.messages.end());
    CHECK((state.messages == std::vector<std::string>{"0","1","2","3"}));
    CHECK(agents[0]->send_message_to_printer("3", "{}", 0, 0) == ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE);
    network.forget_printer_session("1");
    agents[1].reset();
    CHECK(network.send_message_to_printer("3", "{}", 0, 0) == 0);
    CHECK(network.printer_session_token("0")->state == Slic3r::PrinterControlState::connected);
    CHECK(network.printer_session_token("2")->state == Slic3r::PrinterControlState::connected);
    network.set_printer_agent(nullptr);
    CHECK(network.printer_session_token("3")->state == Slic3r::PrinterControlState::connected);
}

TEST_CASE("Bambu worker initialization failures keep their precise reason and a replacement can recover", "[BambuProcessSessions]")
{
    Signals signals;
    const std::string mode = GENERATE("always-null", "config-error", "missing-library", "missing-worker", "missing-symbols", "unsupported-abi", "rejected");
    auto options = config(mode);
    const std::string reason = mode == "always-null" ? "bambu_create_null" : mode == "config-error" ? "bambu_config_failed" :
        mode == "missing-library" ? "bambu_library_load_failed" : mode == "missing-symbols" ? "bambu_missing_symbols" :
        mode == "unsupported-abi" ? "bambu_unsupported_version" : mode == "rejected" ? "5" : "bambu_worker_missing";
    if (mode == "missing-library") options.library += ".not-present";
    if (mode == "missing-worker") options.executable += ".not-present";
    if (mode == "missing-symbols" || mode == "unsupported-abi") {
        auto path = boost::filesystem::path(sdk());
        auto name = path.filename().string();
        name.replace(name.find("bbl_plugin"), std::string("bbl_plugin").size(), mode == "missing-symbols" ? "bbl_missing" : "bbl_badabi");
        options.library = (path.parent_path() / name).string();
    }
    State state;
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_local_connect_fn([&](int, std::string, std::string why) {
        std::lock_guard<std::mutex> lock(state.mutex); state.reasons.push_back(std::move(why)); state.cv.notify_all();
    });
    auto failed = std::make_shared<Slic3r::BambuLanProcess>(options);
    network.set_printer_agent(failed);
    auto connection = params("one");
    if (mode == "rejected") connection.password = "reject";
    network.connect_printer(connection);
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    CHECK(state.reasons.front() == reason);
    CHECK(network.printer_session_token("one")->error() == reason);
    CHECK(network.printer_session_token("one")->state == (mode == "rejected" ? Slic3r::PrinterControlState::rejected : Slic3r::PrinterControlState::offline));
    auto old = network.printer_session_token("one");
    auto recovered = std::make_shared<Slic3r::BambuLanProcess>(config("first-only"));
    network.connect_printer(params("one"), recovered);
    REQUIRE(state.wait([&] { return network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected; }));
    CHECK_FALSE(old->valid);
    CHECK(network.get_printer_agent() == recovered);
    CHECK(recovered->check_cert() == 19);
}

TEST_CASE("Retired Bambu process callbacks cannot update a replacement view", "[BambuProcessSessions]")
{
    Signals signals;
    State state;
    auto agent = std::make_shared<Slic3r::BambuLanProcess>(config("first-only"));
    agent->set_on_local_connect_fn([&](int status, std::string, std::string) {
        if (status == 0) { std::lock_guard<std::mutex> lock(state.mutex); state.reasons.push_back("connected"); state.cv.notify_all(); }
    });
    agent->connect_printer(params("one"));
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    agent->set_on_local_message_fn([&](std::string, std::string) { state.messages.push_back("old"); });
    agent->set_queue_on_main_fn([&](std::function<void()> work) {
        std::lock_guard<std::mutex> lock(state.mutex); state.queue.push_back(std::move(work)); state.cv.notify_all();
    });
    REQUIRE(agent->send_message_to_printer("one", "{}", 0, 0) == 0);
    REQUIRE(state.wait([&] { return !state.queue.empty(); }));
    agent->disconnect_printer();
    for (auto& work : state.queue) work();
    CHECK(state.messages.empty());
    CHECK_FALSE(agent->can_connect_to("two"));
}

TEST_CASE("Bambu local print dispatch preserves parameters, wait callbacks and cancellation through its process", "[BambuProcessSessions]")
{
    Signals signals;
    State state;
    Slic3r::BambuLanProcess agent(config("first-only"));
    agent.set_on_local_connect_fn([&](int status, std::string, std::string) {
        if (status == 0) { std::lock_guard<std::mutex> lock(state.mutex); state.reasons.push_back("connected"); state.cv.notify_all(); }
    });
    agent.connect_printer(params("one"));
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    Slic3r::PrintParams p{};
    p.dev_id = "one"; p.password = "fixture"; p.connection_type = "lan";
    p.plate_index = 7; p.queue_plate_id = "plate-identity"; p.task_name = "task-identity";
    int update_count=0, wait_count=0;
    auto update = [&](int status, int code, std::string value) { CHECK(status == 1); CHECK(code == 7); CHECK(value == "plate-identity"); ++update_count; };
    auto wait = [&](int status, std::string value) { CHECK(status == 2); CHECK(value == "task-identity"); ++wait_count; return true; };
    REQUIRE(agent.start_local_print_with_record(p, update, [] { return false; }, wait) == 0);
    REQUIRE(agent.start_print(p, update, [] { return false; }, wait) == 0);
    REQUIRE(agent.start_send_gcode_to_sdcard(p, update, [] { return false; }, wait) == 0);
    REQUIRE(agent.start_local_print(p, update, [] { return false; }) == 0);
    REQUIRE(agent.start_sdcard_print(p, update, [] { return false; }) == 0);
    CHECK(update_count == 5); CHECK(wait_count == 3);
    p.task_name = "cancel-fixture";
    REQUIRE(agent.start_local_print(p, {}, [] { return true; }) == BAMBU_NETWORK_ERR_CANCELED);
    p.dev_id = "two";
    CHECK(agent.start_local_print(p, {}, {}) == ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE);
}

TEST_CASE("Bambu private IPC rejects incomplete and oversized frames", "[BambuProcessSessions]")
{
    nlohmann::json frame;
    std::istringstream incomplete("{\"op\":\"send\"}");
    CHECK_FALSE(Slic3r::BambuLanProtocol::read_frame(incomplete,frame));
    std::istringstream oversized(std::string(Slic3r::BambuLanProtocol::max_frame_bytes + 1, 'x') + '\n');
    CHECK_FALSE(Slic3r::BambuLanProtocol::read_frame(oversized,frame));
    std::istringstream malformed("[]\n");
    CHECK_FALSE(Slic3r::BambuLanProtocol::read_frame(malformed,frame));
}

TEST_CASE("Bambu uploads keep status requests and SDK callbacks responsive", "[BambuProcessSessions][BambuUploadConcurrency]")
{
    Signals signals;
    enum class Completion { release, cancel, disconnect };
    const auto completion = GENERATE(Completion::release, Completion::cancel, Completion::disconnect);
    State state;
    Slic3r::BambuLanProcess agent(config("blocked-upload"));
    agent.set_on_local_connect_fn([&](int status, std::string, std::string) {
        if (status == 0) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.reasons.push_back("connected");
            state.cv.notify_all();
        }
    });
    REQUIRE(agent.connect_printer(params("one")) == 0);
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    Slic3r::PrintParams print{};
    print.dev_id = "one";
    print.password = "fixture";
    print.connection_type = "lan";
    std::atomic<bool> cancel{false};
    auto upload = std::async(std::launch::async, [&] {
        return agent.start_local_print(print, [&](int, int, std::string message) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.messages.push_back(std::move(message));
            state.cv.notify_all();
        }, [&] { return cancel.load(); });
    });
    REQUIRE(state.wait([&] { return !state.messages.empty(); }));
    CHECK(state.messages.front() == "1.6M/15.4M");
    CHECK(upload.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    // These requests share the upload's connection but must not queue behind it.
    CHECK(agent.send_message_to_printer("one", "{}", 0, 0) == 0);
    if (completion == Completion::disconnect) {
        agent.disconnect_printer();
        REQUIRE(upload.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        CHECK(upload.get() != 0);
        return;
    }
    if (completion == Completion::cancel)
        cancel = true;
    else
        CHECK(agent.send_message_to_printer("one", "fixture-release-upload", 0, 0) == 0);
    REQUIRE(upload.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(upload.get() == (completion == Completion::cancel ? BAMBU_NETWORK_ERR_CANCELED : 0));
    CHECK(agent.send_message_to_printer("one", "{}", 0, 0) == 0);
}

TEST_CASE("A crashing or uncooperative Bambu worker cannot retain another printer's pipes or connection", "[BambuProcessSessions]")
{
    Signals signals;
    const bool hang = GENERATE(false, true);
    State state;
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_local_connect_fn([&](int, std::string, std::string) { std::lock_guard<std::mutex> lock(state.mutex); state.cv.notify_all(); });
    auto a = std::make_shared<Slic3r::BambuLanProcess>(config(hang ? "hang-disconnect" : "first-only"));
    auto b = std::make_shared<Slic3r::BambuLanProcess>(config("first-only"));
    network.connect_printer(params("one"),a); network.connect_printer(params("two"),b);
    REQUIRE(state.wait([&] { return network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected &&
                                  network.printer_session_token("two")->state == Slic3r::PrinterControlState::connected; }));
    network.set_printer_agent(a);
    if (!hang) {
        CHECK(network.send_message_to_printer("one", "fixture-exit", 0, 0) != 0);
        REQUIRE(state.wait([&] { return network.printer_session_token("one")->state == Slic3r::PrinterControlState::offline; }));
        CHECK(network.printer_session_token("one")->error() == "bambu_worker_exited");
    }
    const auto start = std::chrono::steady_clock::now();
    network.forget_printer_session("one");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(6));
    network.set_printer_agent(b);
    CHECK(network.send_message_to_printer("two", "{}", 0, 0) == 0);
    CHECK(network.printer_session_token("two")->state == Slic3r::PrinterControlState::connected);
}

TEST_CASE("A Bambu control request timeout reports an error without retiring its connection", "[BambuProcessSessions]")
{
    Signals signals;
    const std::string payload = GENERATE("fixture-slow", "fixture-hang");
    State state;
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_local_connect_fn([&](int, std::string, std::string) { std::lock_guard<std::mutex> lock(state.mutex); state.cv.notify_all(); });
    auto options = config("first-only");
    options.control_timeout = std::chrono::milliseconds(200);
    auto agent = std::make_shared<Slic3r::BambuLanProcess>(options);
    network.set_printer_agent(agent);
    network.connect_printer(params("one"));
    REQUIRE(state.wait([&] { return network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected; }));
    const auto token = network.printer_session_token("one");
    const auto start = std::chrono::steady_clock::now();
    CHECK(network.send_message_to_printer("one", payload, 0, 0) == BAMBU_NETWORK_ERR_TIMEOUT);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    CHECK(token->valid);
    CHECK(token->state == Slic3r::PrinterControlState::connected);
    CHECK(token->error().empty());
    if (payload == "fixture-slow") {
        // The worker finishes the slow vendor call; its connection keeps serving requests.
        int result = BAMBU_NETWORK_ERR_TIMEOUT;
        for (int attempt = 0; attempt < 25 && result != 0; ++attempt)
            result = network.send_message_to_printer("one", "{}", 0, 0);
        CHECK(result == 0);
        CHECK(token->state == Slic3r::PrinterControlState::connected);
    }
    // Unanswered requests stay bounded, and retiring a wedged worker stays bounded too.
    const auto retire = std::chrono::steady_clock::now();
    network.forget_printer_session("one");
    agent.reset();
    CHECK(std::chrono::steady_clock::now() - retire < std::chrono::seconds(6));
}

TEST_CASE("Bambu certificate work never blocks the caller or control requests", "[BambuProcessSessions]")
{
    Signals signals;
    State state;
    auto options = config("slow-cert");
    options.control_timeout = std::chrono::milliseconds(500);
    Slic3r::BambuLanProcess agent(options);
    agent.set_on_local_connect_fn([&](int status, std::string, std::string) {
        if (status == 0) { std::lock_guard<std::mutex> lock(state.mutex); state.reasons.push_back("connected"); state.cv.notify_all(); }
    });
    REQUIRE(agent.connect_printer(params("one")) == 0);
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    const auto start = std::chrono::steady_clock::now();
    agent.install_device_cert("one", true);
    agent.install_device_cert("one", true);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    // The vendor installation is still running on its own loop.
    CHECK(agent.send_message_to_printer("one", "{}", 0, 0) == 0);
    CHECK(agent.check_cert() == 19);
    CHECK(agent.send_message_to_printer("one", "{}", 0, 0) == 0);
}

TEST_CASE("A burst of Bambu SDK notifications keeps its connection", "[BambuProcessSessions]")
{
    Signals signals;
    State state;
    Slic3r::BambuLanProcess agent(config("telemetry-burst"));
    agent.set_on_local_connect_fn([&](int status, std::string, std::string) {
        std::lock_guard<std::mutex> lock(state.mutex); state.reasons.push_back(status == 0 ? "connected" : "failed"); state.cv.notify_all();
    });
    agent.set_on_local_message_fn([&](std::string, std::string payload) {
        std::lock_guard<std::mutex> lock(state.mutex); state.messages.push_back(std::move(payload)); state.cv.notify_all();
    });
    REQUIRE(agent.connect_printer(params("one")) == 0);
    REQUIRE(state.wait([&] { return state.messages.size() >= 300; }));
    CHECK(agent.send_message_to_printer("one", "{}", 0, 0) == 0);
    std::lock_guard<std::mutex> lock(state.mutex);
    CHECK(state.reasons == std::vector<std::string>{"connected"});
    CHECK(state.messages.front() == "burst-0");
}

TEST_CASE("Bambu callbacks without a UI queue may reconnect, disconnect or release their own transport", "[BambuProcessSessions]")
{
    Signals signals;
    const bool release = GENERATE(false, true);
    State state;
    auto agent = std::make_shared<Slic3r::BambuLanProcess>(config("first-only"));
    std::shared_ptr<Slic3r::BambuLanProcess> owner = agent;
    Slic3r::BambuLanProcess* transport = agent.get();
    agent.reset();
    int send_result = -1;
    bool retried = false;
    // No set_queue_on_main_fn: these handlers run without a UI loop.
    transport->set_on_local_connect_fn([&](int status, std::string, std::string) {
        if (status != 0) {
            if (retried) return;
            retried = true;
            transport->connect_printer(params("one"));
            return;
        }
        send_result = transport->send_message_to_printer("one", "{}", 0, 0);
        if (release) {
            std::shared_ptr<Slic3r::BambuLanProcess> last;
            { std::lock_guard<std::mutex> lock(state.mutex); last.swap(owner); }
            last.reset();
        } else transport->disconnect_printer();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.reasons.push_back("finished");
        state.cv.notify_all();
    });
    auto rejected = params("one");
    rejected.password = "reject";
    REQUIRE(transport->connect_printer(rejected) == 0);
    REQUIRE(state.wait([&] { return !state.reasons.empty(); }));
    CHECK(retried);
    CHECK(send_result == 0);
    std::lock_guard<std::mutex> lock(state.mutex);
    CHECK(bool(owner) == !release);
}

TEST_CASE("Bambu tunnel readiness retains the bound printer identity across its worker", "[BambuProcessSessions]")
{
    Signals signals;
    State state;
    Slic3r::NetworkAgent network(nullptr, nullptr);
    network.set_on_printer_connected_fn([&](std::string id) {
        std::lock_guard<std::mutex> lock(state.mutex); state.messages.push_back(std::move(id)); state.cv.notify_all();
    });
    auto agent = std::make_shared<Slic3r::BambuLanProcess>(config("tunnel"));
    network.connect_printer(params("one"),agent);
    REQUIRE(state.wait([&] { return !state.messages.empty(); }));
    CHECK(state.messages.front() == "tunnel/one");
    CHECK(network.printer_session_token("one")->state == Slic3r::PrinterControlState::connected);
    CHECK_FALSE(network.printer_session_token("tunnel/one"));
}
