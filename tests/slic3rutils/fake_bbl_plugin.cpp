// A socket-free SDK fixture. It has no GUI, filesystem, worker or network code.
#include "slic3r/Utils/bambu_networking.hpp"
#include <chrono>
#include <cstdlib>
#include <thread>
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

// The vendor ABI deliberately exports C-named functions with C++ value types.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

using Connected = std::function<void(int, std::string, std::string)>;
using Message = std::function<void(std::string, std::string)>;
using PrinterConnected = std::function<void(std::string)>;
using Queue = std::function<void(std::function<void()>)>;
struct Agent {
    std::string target, mode;
    bool connected = false;
    bool analytics = true;
    std::atomic<bool> upload_released{false};
    Connected connection;
    Message message;
    PrinterConnected printer_connected;
    Queue queue;
    void dispatch(std::function<void()> fn) { if (queue) queue(std::move(fn)); else fn(); }
};
static std::atomic<int> active_agents{0};
static Agent* singleton = nullptr;

EXPORT std::string bambu_network_get_version() { return "02.08.04.00"; }
EXPORT void* bambu_network_create_agent(std::string mode)
{
    if (mode == "always-null") return nullptr;
    if ((mode == "first-only" || mode == "reuse-singleton") && singleton)
        return mode == "first-only" ? nullptr : singleton;
    ++active_agents;
    auto* agent = new Agent; agent->mode = mode;
    if (mode == "first-only" || mode == "reuse-singleton") singleton = agent;
    return agent;
}
EXPORT int bambu_network_destroy_agent(void* ptr) { if (ptr == singleton) singleton = nullptr; delete static_cast<Agent*>(ptr); --active_agents; return 0; }
EXPORT int porca_fake_active_agents() { return active_agents; }
EXPORT int bambu_network_track_enable(void* ptr, bool enabled) { static_cast<Agent*>(ptr)->analytics = enabled; return 0; }
EXPORT int bambu_network_start(void* ptr) { return static_cast<Agent*>(ptr)->analytics ? -1 : 0; }
EXPORT int bambu_network_connect_printer(void* ptr, std::string id, std::string, std::string, std::string password, bool)
{
    auto& agent = *static_cast<Agent*>(ptr);
    agent.target = id;
    agent.connected = password != "reject";
    if (agent.connection) agent.dispatch([fn = agent.connection, id, ok = agent.connected] { fn(ok ? 0 : 1, id, ok ? "0" : "5"); });
    if (agent.connected && agent.printer_connected)
        agent.dispatch([fn = agent.printer_connected, id = agent.mode == "tunnel" ? "tunnel/" + id : id] { fn(id); });
    // A full status push queues more SDK callbacks than any request backlog.
    if (agent.connected && agent.message && agent.mode == "telemetry-burst")
        for (int i = 0; i < 300; ++i) agent.dispatch([fn = agent.message, id, i] { fn(id, "burst-" + std::to_string(i)); });
    return 0;
}
EXPORT int bambu_network_disconnect_printer(void* ptr) {
    auto& agent = *static_cast<Agent*>(ptr);
    if (agent.mode == "hang-disconnect") for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    agent.connected = false; return 0;
}
EXPORT int bambu_network_send_message_to_printer(void* ptr, std::string id, std::string payload, int, int)
{
    auto& agent = *static_cast<Agent*>(ptr);
    if (payload == "fixture-exit") std::_Exit(77);
    if (payload == "fixture-hang") for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    if (payload == "fixture-slow") std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!agent.connected || agent.target != id) return -4;
    if (payload == "fixture-release-upload") agent.upload_released = true;
    if (agent.message) agent.dispatch([fn = agent.message, id, payload] { fn(id, payload); });
    return 0;
}
EXPORT int bambu_network_set_on_local_connect_fn(void* ptr, Connected fn) { static_cast<Agent*>(ptr)->connection = std::move(fn); return 0; }
EXPORT int bambu_network_set_on_local_message_fn(void* ptr, Message fn) { static_cast<Agent*>(ptr)->message = std::move(fn); return 0; }
EXPORT int bambu_network_set_on_printer_connected_fn(void* ptr, PrinterConnected fn) { static_cast<Agent*>(ptr)->printer_connected = std::move(fn); return 0; }
EXPORT int bambu_network_set_queue_on_main_fn(void* ptr, Queue fn) { static_cast<Agent*>(ptr)->queue = std::move(fn); return 0; }
EXPORT int bambu_network_set_on_message_fn(void*, Message) { return 0; }
EXPORT int bambu_network_set_on_user_message_fn(void*, Message) { return 0; }

EXPORT int bambu_network_set_config_dir(void*, std::string dir) { return dir == "config-error" ? -1 : 0; }
EXPORT int bambu_network_set_cert_file(void*, std::string, std::string) { return 0; }
EXPORT int bambu_network_set_country_code(void*, std::string) { return 0; }
EXPORT int bambu_network_update_cert(void*) { return 19; }
EXPORT void bambu_network_install_device_cert(void* ptr, std::string, bool)
{
    if (static_cast<Agent*>(ptr)->mode == "slow-cert") std::this_thread::sleep_for(std::chrono::seconds(2));
}

static int print_job(void* ptr, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn update,
                     Slic3r::WasCancelledFn cancel, Slic3r::OnWaitFn wait = {})
{
    auto& agent = *static_cast<Agent*>(ptr);
    if (agent.target != p.dev_id || p.password != "fixture") return -4;
    if (agent.mode == "blocked-upload") {
        // A synchronous upload needs the SDK callback loop and ordinary control
        // requests to keep running until release or cancellation, without sockets.
        auto callback_ran = std::make_shared<std::atomic<bool>>(false);
        agent.dispatch([callback_ran] { *callback_ran = true; });
        if (update) update(1, 10, "1.6M/15.4M");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!agent.upload_released && !(cancel && cancel()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancel && cancel()) return BAMBU_NETWORK_ERR_CANCELED;
        return agent.upload_released && *callback_ran ? 0 : -77;
    }
    if (update) update(1, p.plate_index, p.queue_plate_id);
    if (wait && !wait(2, p.task_name)) return BAMBU_NETWORK_ERR_CANCELED;
    if (p.task_name == "cancel-fixture")
        for (int i = 0; i < 1000 && !(cancel && cancel()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return cancel && cancel() ? BAMBU_NETWORK_ERR_CANCELED : 0;
}
EXPORT int bambu_network_start_print(void* a, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn u, Slic3r::WasCancelledFn c, Slic3r::OnWaitFn w) { return print_job(a,p,u,c,w); }
EXPORT int bambu_network_start_local_print_with_record(void* a, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn u, Slic3r::WasCancelledFn c, Slic3r::OnWaitFn w) { return print_job(a,p,u,c,w); }
EXPORT int bambu_network_start_send_gcode_to_sdcard(void* a, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn u, Slic3r::WasCancelledFn c, Slic3r::OnWaitFn w) { return print_job(a,p,u,c,w); }
EXPORT int bambu_network_start_local_print(void* a, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn u, Slic3r::WasCancelledFn c) { return print_job(a,p,u,c); }
EXPORT int bambu_network_start_sdcard_print(void* a, Slic3r::PrintParams p, Slic3r::OnUpdateStatusFn u, Slic3r::WasCancelledFn c) { return print_job(a,p,u,c); }
