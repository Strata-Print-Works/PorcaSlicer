// Private headless worker. No discovery/account startup or GUI application entry.
#include "BambuLanProtocol.hpp"
#include "BBLNetworkPlugin.hpp"
#include "BBLPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/Utils.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace {
using json = nlohmann::json;
using namespace Slic3r;

// Reserve the inherited stdout pipe before loading third-party code. Vendor
// stdout/stderr must never corrupt protocol frames or expose credentials.
FILE* reserve_output()
{
#ifdef _WIN32
    const int fd = _dup(_fileno(stdout));
    if (fd < 0) return nullptr;
    _setmode(fd, _O_BINARY);
    SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), HANDLE_FLAG_INHERIT, 0);
    FILE* pipe = _fdopen(fd, "wb");
    if (!pipe) { _close(fd); return nullptr; }
    std::freopen("NUL", "w", stdout);
#else
    const int fd = dup(STDOUT_FILENO);
    if (fd < 0) return nullptr;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) { close(fd); return nullptr; }
    FILE* pipe = fdopen(fd, "w");
    if (!pipe) { close(fd); return nullptr; }
    std::freopen("/dev/null", "w", stdout);
#endif
    return pipe;
}

struct Worker {
    // Control requests, certificate work, uploads and SDK callbacks each have
    // a loop: a slow vendor call in one never delays or retires the others.
    enum class Queue { control, certificate, print, callback };
    explicit Worker(FILE* pipe) : output(pipe) {}
    FILE* output;
    std::mutex output_mutex, mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks, certificate_tasks, print_tasks, callback_tasks;
    std::map<uint64_t, bool> answers;
    std::set<uint64_t> prints, cancelled, waiting;
    std::atomic<bool> stopping{false}, reader_finished{false};
    bool cleanup_done = false;
    uint64_t callback_id = 0;
    std::string device;
    std::shared_ptr<BBLPrinterAgent> printer;

    void emit(const json& frame)
    {
        const auto text = frame.dump() + '\n';
        if (text.size() > BambuLanProtocol::max_frame_bytes) { stopping = true; cv.notify_all(); return; }
        std::lock_guard<std::mutex> lock(output_mutex);
        if (std::fwrite(text.data(), 1, text.size(), output) != text.size() || std::fflush(output) != 0) { stopping = true; cv.notify_all(); }
    }
    std::deque<std::function<void()>>& queue(Queue which)
    {
        return which == Queue::print ? print_tasks : which == Queue::certificate ? certificate_tasks :
               which == Queue::callback ? callback_tasks : tasks;
    }
    void enqueue(std::function<void()> task, Queue which)
    {
        std::function<void()> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto& pending = queue(which);
            if (which == Queue::callback) {
                // SDK notifications can burst during pushall. Shed the oldest
                // rather than retire a live printer connection.
                if (pending.size() >= 512) { dropped = std::move(pending.front()); pending.pop_front(); }
                pending.push_back(std::move(task));
            } else if (pending.size() >= 64) {
                // The parent keeps at most 32 requests unanswered, so a longer
                // queue is a protocol violation, not backlog.
                stopping = true;
            } else pending.push_back(std::move(task));
        }
        cv.notify_all();
    }
    void run_tasks(std::deque<std::function<void()>>& queue)
    {
        while (!stopping) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) break;
                task = std::move(queue.front()); queue.pop_front();
            }
            try { task(); }
            catch (...) { error("bambu_worker_protocol_failed"); stopping = true; cv.notify_all(); }
        }
    }
    void error(const std::string& reason) { emit({{"event","error"},{"reason",reason}}); }
    bool initialize(const json& init)
    {
        device = init.at("device");
        if (device.empty() || init.at("op") != "init") return false;
        set_resources_dir(init.at("resources"));
        BambuAgentError failure;
        auto plugin = BBLNetworkPlugin::load_device_plugin(init.at("library"), init.at("config_dir"), &failure);
        if (!plugin) {
            const char* reason = failure == BambuAgentError::null_handle ? "bambu_create_null" :
                failure == BambuAgentError::duplicate_handle ? "bambu_create_duplicate" :
                failure == BambuAgentError::missing_symbols ? "bambu_missing_symbols" :
                failure == BambuAgentError::unsupported_version ? "bambu_unsupported_version" : "bambu_library_load_failed";
            error(reason); return false;
        }
        const auto handle = plugin->get_agent();
        if (!plugin->get_set_config_dir() || plugin->get_set_config_dir()(handle, init.at("config_dir")) != 0) {
            error("bambu_config_failed"); return false;
        }
        if (!plugin->get_set_cert_file() || plugin->get_set_cert_file()(handle, init.at("resources").get<std::string>() + "/cert", "slicer_base64.cer") != 0) {
            error("bambu_cert_failed"); return false;
        }
        if (!plugin->get_set_country_code() || plugin->get_set_country_code()(handle, init.at("country")) != 0) {
            error("bambu_country_failed"); return false;
        }
        printer = std::make_shared<BBLPrinterAgent>(std::move(plugin), init.at("ssl").get<bool>());
        printer->set_queue_on_main_fn([this](std::function<void()> fn) { enqueue(std::move(fn), Queue::callback); });
        printer->set_on_local_connect_fn([this](int state, std::string id, std::string reason) {
            if (id != device) return;
            // Only the documented credential code is relevant to the UI. Never
            // forward raw vendor error strings that may include private data.
            emit({{"event","connection"},{"state",state},{"reason",reason == "5" ? "5" : state == 0 ? "0" : "bambu_connection_failed"}});
        });
        printer->set_on_printer_connected_fn([this](std::string id) {
            if (id == device || id == "tunnel/" + device) emit({{"event","connected"},{"device",id}});
        });
        printer->set_on_local_message_fn([this](std::string id, std::string payload) {
            if (id == device) emit({{"event","message"},{"payload",std::move(payload)}});
        });
        printer->set_on_subscribe_failure_fn([this](std::string id) { if (id == device) emit({{"event","subscription_failure"}}); });
        PrinterConnectionParams params;
        params.dev_id = device; params.host = init.at("host"); params.username = init.at("username"); params.password = init.at("password");
        params.use_ssl = init.at("ssl");
        emit({{"event","ready"}});
        if (printer->connect_printer(params) != 0) { error("bambu_start_failed"); return false; }
        return true;
    }
    void execute(const json& frame)
    {
        const uint64_t id = frame.at("id");
        const std::string op = frame.at("op");
        int result = ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
        if (op == "send") result = printer->send_message_to_printer(device, frame.at("payload"), frame.at("qos"), frame.at("flag"));
        else if (op == "check_cert") result = printer->check_cert();
        else if (op == "install_cert") { printer->install_device_cert(device, frame.at("lan")); result = 0; }
        else if (op.rfind("print_", 0) == 0) {
            const auto params = BambuLanProtocol::decode_print(frame.at("params"));
            if (params.dev_id != device || params.connection_type != "lan") {
                { std::lock_guard<std::mutex> lock(mutex); prints.erase(id); cancelled.erase(id); }
                emit({{"id",id},{"result",ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE}}); return;
            }
            auto cancel = [this,id] {
                std::lock_guard<std::mutex> lock(mutex);
                return stopping.load() || cancelled.count(id) != 0;
            };
            if (cancel()) {
                { std::lock_guard<std::mutex> lock(mutex); prints.erase(id); cancelled.erase(id); }
                emit({{"id",id},{"result",BAMBU_NETWORK_ERR_CANCELED}}); return;
            }
            auto update = [this,id](int status, int code, std::string message) {
                emit({{"event","update"},{"id",id},{"status",status},{"code",code},{"message",std::move(message)}});
            };
            auto wait = [this,id](int status, std::string message) {
                uint64_t call;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (waiting.size() >= 32) return false;
                    call = ++callback_id; waiting.insert(call);
                }
                emit({{"event","wait"},{"id",id},{"callback",call},{"status",status},{"message",std::move(message)}});
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::seconds(60), [&] { return stopping || cancelled.count(id) || answers.count(call); });
                waiting.erase(call);
                const auto it = answers.find(call);
                if (it == answers.end()) return false;
                const bool value = it->second; answers.erase(it); return value;
            };
            if (op == "print_start") result = printer->start_print(params, update, cancel, wait);
            else if (op == "print_record") result = printer->start_local_print_with_record(params, update, cancel, wait);
            else if (op == "print_send") result = printer->start_send_gcode_to_sdcard(params, update, cancel, wait);
            else if (op == "print_local") result = printer->start_local_print(params, update, cancel);
            else if (op == "print_sdcard") result = printer->start_sdcard_print(params, update, cancel);
            { std::lock_guard<std::mutex> lock(mutex); prints.erase(id); cancelled.erase(id); }
        }
        emit({{"id",id},{"result",result}});
    }
    void read_input()
    {
        try {
            json frame;
            while (!stopping && BambuLanProtocol::read_frame(std::cin, frame)) {
                const std::string op = frame.at("op");
                if (op == "cancel") {
                    std::lock_guard<std::mutex> lock(mutex);
                    const uint64_t id = frame.at("id");
                    if (prints.count(id)) cancelled.insert(id);
                    cv.notify_all();
                }
                else if (op == "wait_result") {
                    std::lock_guard<std::mutex> lock(mutex);
                    const uint64_t id = frame.at("callback");
                    if (waiting.count(id)) answers[id] = frame.at("value");
                    cv.notify_all();
                } else {
                    const bool print = op.rfind("print_",0) == 0;
                    if (print) {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (prints.size() >= 32) { stopping = true; break; }
                        prints.insert(frame.at("id").get<uint64_t>());
                    }
                    enqueue([this,frame] { execute(frame); }, print ? Queue::print :
                            op == "check_cert" || op == "install_cert" ? Queue::certificate : Queue::control);
                }
            }
        } catch (...) {}
        reader_finished = true; stopping = true; cv.notify_all();
        // EOF also means parent death. Bound a vendor call that ignores cancel
        // or hangs while joining; this process owns all of its SDK resources.
        // A concurrent disconnect would race that stuck call. Exiting closes
        // the SDK's sockets, which ends its printer session.
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return cleanup_done; })) std::_Exit(3);
    }
    void clear_callbacks()
    {
        printer->set_on_local_connect_fn({}); printer->set_on_local_message_fn({});
        printer->set_on_printer_connected_fn({}); printer->set_on_subscribe_failure_fn({});
        printer->set_queue_on_main_fn({});
    }
    // Every loop has stopped, so no SDK call is running. Give the SDK a short
    // chance to end its printer session before this process exits.
    void disconnect_before_exit()
    {
        clear_callbacks();
        std::promise<void> finished;
        auto disconnected = finished.get_future();
        std::thread([this, finished = std::move(finished)]() mutable {
            try { printer->disconnect_printer(); } catch (...) {}
            finished.set_value();
        }).detach();
        disconnected.wait_for(std::chrono::seconds(2));
    }
    int run()
    {
        json init;
        if (!BambuLanProtocol::read_frame(std::cin, init)) return 2;
        std::thread reader([this] { read_input(); });
        // SDK callbacks queued during connection already have their own loop.
        std::thread callbacks([this] { run_tasks(callback_tasks); });
        try { if (!initialize(init)) std::_Exit(2); }
        catch (...) { error("bambu_worker_protocol_failed"); std::_Exit(2); }
        // The SDK's upload calls block, as they do in the GUI's job worker.
        // A status poll must not time out behind an upload or a certificate
        // request. One owned upload thread keeps print jobs serialized per printer.
        std::thread uploader([this] { run_tasks(print_tasks); });
        std::thread certificates([this] { run_tasks(certificate_tasks); });
        run_tasks(tasks);
        cv.notify_all();
        uploader.join();
        certificates.join();
        callbacks.join();
        // Parent EOF, malformed input or overload must also interrupt a reader
        // waiting on stdin. Closing its descriptor is insufficient on all OSes;
        // protocol errors exit this private process, whose OS resources are owned.
        if (!reader_finished) { disconnect_before_exit(); std::_Exit(3); }
        clear_callbacks();
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.clear(); certificate_tasks.clear(); print_tasks.clear(); callback_tasks.clear();
        }
        printer->disconnect_printer(); printer.reset();
        { std::lock_guard<std::mutex> lock(mutex); cleanup_done = true; cv.notify_all(); }
        reader.join();
        return 0;
    }
};
} // namespace

int main()
{
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    FILE* output = reserve_output();
    if (!output) return 2;
    int result = 2;
    try { Worker worker{output}; result = worker.run(); }
    catch (...) {
        const char* error = "{\"event\":\"error\",\"reason\":\"bambu_worker_protocol_failed\"}\n";
        std::fputs(error, output); std::fflush(output);
    }
    std::fclose(output);
    return result;
}
