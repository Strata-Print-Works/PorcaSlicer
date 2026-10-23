#include "BambuLanProcess.hpp"
#include "BambuLanProtocol.hpp"
#include "bambu_networking.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process/pipe.hpp>
#include <boost/process/io.hpp>
#include <boost/process/handles.hpp>
#include <boost/system/error_code.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#include <boost/nowide/convert.hpp>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r {
namespace bp = boost::process;
using json = nlohmann::json;
namespace {
std::mutex launch_mutex;

// Runs callbacks in order on one owned thread when no UI queue is installed.
// A pipe reader therefore never executes user code: a handler may reconnect,
// issue a request or release the last reference to its transport.
class CallbackQueue {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::function<void()>> work;
        bool stopping = false;
    };
    std::shared_ptr<State> m_state = std::make_shared<State>();
    std::thread m_thread;

    static void run(const std::shared_ptr<State>& state)
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        for (;;) {
            state->cv.wait(lock, [&] { return state->stopping || !state->work.empty(); });
            if (state->stopping) return;
            auto work = std::move(state->work.front());
            state->work.pop_front();
            lock.unlock();
            try { work(); }
            catch (...) { BOOST_LOG_TRIVIAL(warning) << "Bambu LAN callback failed"; }
            // Release captured state unlocked: it may own this queue.
            work = nullptr;
            lock.lock();
        }
    }

public:
    CallbackQueue() = default;
    CallbackQueue(const CallbackQueue&) = delete;
    CallbackQueue& operator=(const CallbackQueue&) = delete;
    ~CallbackQueue()
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->stopping = true;
            m_state->work.clear();
        }
        m_state->cv.notify_all();
        if (!m_thread.joinable()) return;
        // A callback may release the last owner. Its loop keeps its own state.
        if (m_thread.get_id() == std::this_thread::get_id()) m_thread.detach();
        else m_thread.join();
    }
    void post(std::function<void()> work)
    {
        std::function<void()> dropped;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->stopping) return;
            // A stalled handler sheds the oldest notification, never memory.
            if (m_state->work.size() >= 1024) {
                dropped = std::move(m_state->work.front());
                m_state->work.pop_front();
            }
            m_state->work.push_back(std::move(work));
            if (!m_thread.joinable()) m_thread = std::thread([state = m_state] { run(state); });
        }
        m_state->cv.notify_one();
    }
};

// Uploads report progress for as long as the SDK transfers. Certificate work
// runs on its own worker loop and may contact the printer or a service.
std::chrono::milliseconds request_deadline(const std::string& op, std::chrono::milliseconds control)
{
    if (op.rfind("print_", 0) == 0) return std::chrono::minutes(30);
    if (op == "check_cert" || op == "install_cert") return std::max<std::chrono::milliseconds>(control, std::chrono::minutes(1));
    return control;
}

bool worker_present(const std::string& executable)
{
    boost::system::error_code ec;
#ifdef _WIN32
    const boost::filesystem::path path(boost::nowide::widen(executable));
#else
    const boost::filesystem::path path(executable);
#endif
    return boost::filesystem::is_regular_file(path, ec) && !ec;
}

bp::pipe private_pipe()
{
#ifdef __linux__
    // Created close-on-exec atomically: no other launcher in this process can
    // inherit a descriptor between pipe() and fcntl().
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) == -1) throw std::system_error(errno, std::generic_category());
    return bp::pipe(fds[0], fds[1]);
#else
    return bp::pipe();
#endif
}
} // namespace

struct BambuLanProcess::Callbacks {
    std::mutex mutex;
    OnPrinterConnectedFn connected;
    OnLocalConnectedFn connection;
    OnMessageFn message;
    GetSubscribeFailureFn subscription_failure;
    QueueOnMainFn queue;
    CallbackQueue fallback;
};

struct BambuLanProcess::Channel : std::enable_shared_from_this<Channel> {
    struct Event {
        bool wait = false;
        int status = 0, code = 0;
        std::string message;
        uint64_t callback = 0;
    };
    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        std::string op;
        // `abandoned` keeps a timed-out request's slot until its late reply.
        bool done = false, closed = false, abandoned = false;
        int result = BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        std::deque<Event> events;
    };
    std::string device;
    std::shared_ptr<Callbacks> callbacks;
    std::atomic<bool> valid{true}, ready{false}, failed{false};
    std::atomic<uint64_t> next_id{1};
    bp::ipstream output{private_pipe()};
    bp::opstream input{private_pipe()};
    std::mutex process_mutex, write_mutex, pending_mutex;
    std::unique_ptr<bp::child> process;
    std::map<uint64_t, std::shared_ptr<Pending>> pending;

    Channel()
    {
#if !defined(_WIN32) && !defined(__linux__)
        // No pipe2 here. Construction and spawn share launch_mutex, so none of
        // our worker launches can inherit a pipe before these flags are set.
        for (const int fd : {input.pipe().native_source(), input.pipe().native_sink(),
                             output.pipe().native_source(), output.pipe().native_sink()})
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) throw std::system_error(errno, std::generic_category());
#endif
    }

#ifndef _WIN32
    void spawn(const std::string& executable)
    {
        // Never fork a multithreaded GUI and run C++/directory enumeration in
        // its child. posix_spawn performs only the OS file actions before exec.
        struct Actions {
            posix_spawn_file_actions_t value;
            Actions() { const int e = posix_spawn_file_actions_init(&value); if (e) throw std::system_error(e, std::generic_category()); }
            ~Actions() { posix_spawn_file_actions_destroy(&value); }
        } actions;
        struct Attributes {
            posix_spawnattr_t value;
            Attributes() { const int e = posix_spawnattr_init(&value); if (e) throw std::system_error(e, std::generic_category()); }
            ~Attributes() { posix_spawnattr_destroy(&value); }
        } attributes;
        auto checked = [](int e) { if (e) throw std::system_error(e, std::generic_category()); };
        checked(posix_spawn_file_actions_adddup2(&actions.value, input.pipe().native_source(), STDIN_FILENO));
        checked(posix_spawn_file_actions_adddup2(&actions.value, output.pipe().native_sink(), STDOUT_FILENO));
        checked(posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
#ifdef __APPLE__
        checked(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_CLOEXEC_DEFAULT));
        char** environment = *_NSGetEnviron();
#else
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        // The child closes every other descriptor itself, including any a
        // peer thread opened without close-on-exec after an enumeration.
        checked(posix_spawn_file_actions_addclosefrom_np(&actions.value, STDERR_FILENO + 1));
#else
        // Enumerate in the parent, never after fork. Owned pipes are already
        // close-on-exec; this also covers descriptors other code leaked.
        for (const int fd : bp::get_handles())
            if (fd > STDERR_FILENO) checked(posix_spawn_file_actions_addclose(&actions.value, fd));
#endif
        char** environment = environ;
#endif
        char* argv[] = {const_cast<char*>(executable.c_str()), nullptr};
        pid_t pid;
        checked(posix_spawn(&pid, executable.c_str(), &actions.value, &attributes.value, argv, environment));
        process = std::make_unique<bp::child>(pid);
        close(input.pipe().native_source()); input.pipe().assign_source(-1);
        close(output.pipe().native_sink()); output.pipe().assign_sink(-1);
    }
#endif

    template<class Fn, class... Args> void dispatch(Fn Callbacks::*member, Args... args)
    {
        Fn fn;
        QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(callbacks->mutex);
            fn = callbacks.get()->*member;
            queue = callbacks->queue;
        }
        if (!fn || !valid) return;
        auto work = [weak = weak_from_this(), fn = std::move(fn), args = std::make_tuple(std::move(args)...)] {
            const auto channel = weak.lock();
            if (channel && channel->valid) std::apply(fn, args);
        };
        if (queue) queue(std::move(work)); else callbacks->fallback.post(std::move(work));
    }

    bool send(const json& message)
    {
        const auto frame = message.dump();
        if (frame.size() > BambuLanProtocol::max_frame_bytes) return false;
        std::lock_guard<std::mutex> lock(write_mutex);
        if (!valid) return false;
        try { input << frame << '\n' << std::flush; return bool(input); }
        catch (...) { return false; }
    }

    bool add_pending(uint64_t id, std::shared_ptr<Pending> item)
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        // Unanswered and abandoned requests both count: a stalled worker can
        // never accumulate unbounded queued work.
        if (!valid || failed || pending.size() >= 32) return false;
        if (item->abandoned)
            for (const auto& entry : pending)
                if (entry.second->op == item->op) return false; // already in flight
        pending.emplace(id, std::move(item));
        return true;
    }

    void erase_pending(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending.erase(id);
    }

    int finish_pending(uint64_t id, Pending& item, bool sent)
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        std::lock_guard<std::mutex> item_lock(item.mutex);
        if (item.done || !sent || item.closed) {
            pending.erase(id);
            return item.done ? item.result : BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        }
        item.abandoned = true;
        return BAMBU_NETWORK_ERR_TIMEOUT;
    }

    void close_pending()
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        for (auto it = pending.begin(); it != pending.end();) {
            const auto item = it->second;
            std::lock_guard<std::mutex> item_lock(item->mutex);
            item->closed = true;
            item->cv.notify_all();
            it = item->abandoned ? pending.erase(it) : std::next(it);
        }
    }

    void fail(const std::string& reason)
    {
        ready = false;
        const bool first = !failed.exchange(true);
        close_pending();
        if (first && valid) {
            BOOST_LOG_TRIVIAL(warning) << "Bambu LAN connection stopped: " << reason;
            dispatch(&Callbacks::connection, int(ConnectStatusFailed), device, reason);
        }
    }

    // Invalidates callbacks and wakes requests at once. Closing stdin makes the
    // worker exit; reaping is left to the caller's owned retirement thread.
    void stop()
    {
        if (!valid.exchange(false)) return;
        ready = false;
        close_pending();
        close_input();
    }

    void close_input()
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        input.pipe().close();
    }

    // Boost's timed wait forks a timeout helper on macOS and changes the
    // process-wide SIGCHLD handler. Poll the owned child instead, without
    // affecting another worker or the GUI's signal handling. A hung SDK must
    // not keep Porca alive; the child is exclusively ours, never a printer.
    void reap()
    {
        std::lock_guard<std::mutex> lock(process_mutex);
        if (!process) return;
        std::error_code ec;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (process->running(ec) && !ec && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (process->running(ec)) process->terminate(ec);
        process->wait(ec);
    }

    void deliver(const json& frame, const std::string& event)
    {
        const auto id = frame.at("id").get<uint64_t>();
        const bool progress = event == "update" || event == "wait";
        Event decoded;
        int result = 0;
        if (progress) {
            decoded.wait = event == "wait";
            decoded.status = frame.at("status").get<int>();
            decoded.message = frame.at("message").get<std::string>();
            if (decoded.wait) decoded.callback = frame.at("callback").get<uint64_t>();
            else decoded.code = frame.at("code").get<int>();
        } else result = frame.at("result").get<int>();
        bool decline = decoded.wait;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            const auto it = pending.find(id);
            if (it != pending.end()) {
                const auto item = it->second;
                std::lock_guard<std::mutex> item_lock(item->mutex);
                if (item->abandoned) {
                    if (!progress) pending.erase(it);
                } else if (progress) {
                    // Bound progress per request. An unqueued confirmation is
                    // declined at once instead of stalling its upload.
                    if (item->events.size() < 128) {
                        item->events.push_back(std::move(decoded));
                        decline = false;
                    }
                    item->cv.notify_all();
                } else {
                    item->result = result;
                    item->done = true;
                    item->cv.notify_all();
                }
            }
        }
        if (decline) send({{"op","wait_result"},{"callback",decoded.callback},{"value",false}});
    }

    void run(const BambuLanProcessConfig& config, const PrinterConnectionParams& params)
    {
        bool started = false;
        // An incomplete installation is reported precisely, not as a crash.
        if (!worker_present(config.executable)) {
            BOOST_LOG_TRIVIAL(error) << "Bambu LAN worker is missing: " << config.executable;
            fail("bambu_worker_missing");
            return;
        }
        try {
            {
                std::lock_guard<std::mutex> launch_lock(launch_mutex);
                std::lock_guard<std::mutex> lock(process_mutex);
                if (!valid) return;
#ifdef _WIN32
                process = std::make_unique<bp::child>(boost::nowide::widen(config.executable),
                    bp::windows::create_no_window,
                    bp::std_in < input, bp::std_out > output, bp::std_err > bp::null, bp::limit_handles);
#else
                spawn(config.executable);
#endif
            }
            // Only non-secret argv (the executable) is visible to the OS. All
            // local identity/credentials travel over inherited private pipes.
            if (!send({{"id",0},{"op","init"},{"library",config.library},{"config_dir",config.config_dir},
                       {"resources",config.resources},{"country",config.country},{"device",device},
                       {"host",params.host},{"username",params.username},{"password",params.password},
                       {"ssl",params.use_ssl}})) throw std::runtime_error("worker pipe closed");
            json frame;
            while (valid && BambuLanProtocol::read_frame(output, frame)) {
                const auto event = frame.value("event", std::string());
                if (event == "ready") { started = true; ready = true; continue; }
                if (event == "connection") {
                    dispatch(&Callbacks::connection, frame.at("state").get<int>(), device, frame.at("reason").get<std::string>());
                } else if (event == "connected") {
                    const auto id = frame.value("device", device);
                    if (id == device || id == "tunnel/" + device) dispatch(&Callbacks::connected, id);
                } else if (event == "message") {
                    dispatch(&Callbacks::message, device, frame.at("payload").get<std::string>());
                } else if (event == "subscription_failure") {
                    dispatch(&Callbacks::subscription_failure, device);
                } else if (event == "error") {
                    fail(frame.at("reason").get<std::string>());
                } else if (frame.contains("id")) {
                    deliver(frame, event);
                }
            }
            fail("bambu_worker_exited");
        } catch (const std::exception&) {
            // After startup, a malformed frame is a protocol failure, not an install problem.
            fail(started ? "bambu_worker_protocol_failed" : "bambu_worker_start_failed");
        } catch (...) { fail("bambu_worker_exited"); }
        // Reap failed children even while a long retry backoff is pending.
        close_input();
        reap();
    }
};

BambuLanProcess::BambuLanProcess(BambuLanProcessConfig config)
    : m_callbacks(std::make_shared<Callbacks>()), m_config(std::move(config)) {}

BambuLanProcess::~BambuLanProcess()
{
    std::vector<Retired> retired;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        retire_locked();
        retired.swap(m_retired);
    }
    // Retirement threads run no callbacks, so they never own this transport.
    for (auto& item : retired)
        if (item.thread.get_id() == std::this_thread::get_id()) item.thread.detach();
        else item.thread.join();
}

void BambuLanProcess::retire_locked()
{
    for (auto it = m_retired.begin(); it != m_retired.end();)
        if (it->done->load()) { it->thread.join(); it = m_retired.erase(it); }
        else ++it;
    auto channel = std::exchange(m_channel, {});
    auto reader = std::move(m_reader);
    if (!channel) return;
    channel->stop();
    auto done = std::make_shared<std::atomic<bool>>(false);
    // The worker exits on stdin EOF. Bound that wait and join its reader here,
    // so neither the UI nor a callback thread waits for the child or joins itself.
    m_retired.push_back({std::thread([channel, reader = std::move(reader), done]() mutable {
        channel->reap();
        if (reader.joinable()) reader.join();
        *done = true;
    }), done});
}

std::shared_ptr<BambuLanProcess::Channel> BambuLanProcess::channel() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_channel;
}

bool BambuLanProcess::can_connect_to(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_device_id.empty() || m_device_id == id;
}

int BambuLanProcess::connect_printer(const PrinterConnectionParams& params)
{
    if (params.dev_id.empty() || !can_connect_to(params.dev_id)) return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    std::shared_ptr<Channel> channel;
    { std::lock_guard<std::mutex> lock(launch_mutex); channel = std::make_shared<Channel>(); }
    channel->device = params.dev_id;
    channel->callbacks = m_callbacks;
    auto captured = params;
    captured.use_ssl = BBLPrinterAgent::uses_ssl_for_mqtt(params.dev_id);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_device_id.empty() && m_device_id != params.dev_id) return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    retire_locked();
    m_device_id = params.dev_id;
    m_channel = channel;
    m_reader = std::thread([channel, config = m_config, captured] { channel->run(config, captured); });
    return 0;
}

int BambuLanProcess::disconnect_printer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    retire_locked();
    return 0;
}

int BambuLanProcess::request(const std::string& op, json args, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait)
{
    const auto channel = this->channel();
    if (!channel || !channel->ready || !channel->valid) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    const auto id = channel->next_id.fetch_add(1);
    auto item = std::make_shared<Channel::Pending>();
    item->op = op;
    if (!channel->add_pending(id, item)) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    // A throwing user callback releases the slot; a late reply is then ignored.
    struct Release {
        Channel& channel;
        uint64_t id;
        bool armed = true;
        ~Release() { if (armed) channel.erase_pending(id); }
    } release{*channel, id};
    args["op"] = op; args["id"] = id;
    const auto deadline = std::chrono::steady_clock::now() + request_deadline(op, m_config.control_timeout);
    const bool sent = channel->send(args);
    bool cancel_sent = false;
    std::unique_lock<std::mutex> lock(item->mutex);
    while (sent && !item->closed && (!item->done || !item->events.empty())) {
        while (!item->events.empty()) {
            auto event = std::move(item->events.front());
            item->events.pop_front();
            lock.unlock();
            if (!event.wait) {
                if (update) update(event.status, event.code, event.message);
            } else {
                const bool answer = wait && wait(event.status, event.message);
                channel->send({{"op","wait_result"},{"callback",event.callback},{"value",answer}});
            }
            lock.lock();
        }
        if (item->done || item->closed) continue;
        if (cancel && !cancel_sent) {
            lock.unlock();
            if (cancel()) {
                channel->send({{"op","cancel"},{"id",id}});
                cancel_sent = true;
            }
            lock.lock();
            if (item->done || item->closed || !item->events.empty()) continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        // Only a pending cancellation callback needs periodic polling.
        item->cv.wait_until(lock, cancel && !cancel_sent ? std::min(deadline, now + std::chrono::milliseconds(100)) : deadline);
    }
    lock.unlock();
    release.armed = false;
    const int result = channel->finish_pending(id, *item, sent);
    // The worker keeps the connection. A timed-out upload is asked to stop.
    if (result == BAMBU_NETWORK_ERR_TIMEOUT && !cancel_sent) channel->send({{"op","cancel"},{"id",id}});
    return result;
}

int BambuLanProcess::send_message_to_printer(std::string id, std::string payload, int qos, int flag)
{
    if (!can_connect_to(id)) return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    return request("send", {{"payload",std::move(payload)},{"qos",qos},{"flag",flag}});
}

int BambuLanProcess::check_cert() { return request("check_cert"); }

void BambuLanProcess::install_device_cert(std::string id, bool lan_only)
{
    if (!can_connect_to(id)) return;
    const auto channel = this->channel();
    if (!channel || !channel->ready || !channel->valid) return;
    // The UI calls this periodically and never reads a result. Never block
    // it on the SDK; keep at most one installation in flight per worker.
    auto item = std::make_shared<Channel::Pending>();
    item->op = "install_cert";
    item->abandoned = true;
    const auto request_id = channel->next_id.fetch_add(1);
    if (!channel->add_pending(request_id, std::move(item))) return;
    if (!channel->send({{"op","install_cert"},{"id",request_id},{"lan",lan_only}})) channel->erase_pending(request_id);
}

int BambuLanProcess::print(const std::string& op, const PrintParams& p, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait)
{
    if (!can_connect_to(p.dev_id)) return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
    return request(op, {{"params",BambuLanProtocol::encode_print(p)}}, std::move(update), std::move(cancel), std::move(wait));
}
#define PRINT_WITH_WAIT(name, op) \
int BambuLanProcess::name(PrintParams p, OnUpdateStatusFn u, WasCancelledFn c, OnWaitFn w) { \
    if (p.connection_type != "lan") return BBLPrinterAgent::name(std::move(p),std::move(u),std::move(c),std::move(w)); \
    return print(op,p,std::move(u),std::move(c),std::move(w)); }
PRINT_WITH_WAIT(start_print, "print_start")
PRINT_WITH_WAIT(start_local_print_with_record, "print_record")
PRINT_WITH_WAIT(start_send_gcode_to_sdcard, "print_send")
#undef PRINT_WITH_WAIT
#define PRINT_WITHOUT_WAIT(name, op) \
int BambuLanProcess::name(PrintParams p, OnUpdateStatusFn u, WasCancelledFn c) { \
    if (p.connection_type != "lan") return BBLPrinterAgent::name(std::move(p),std::move(u),std::move(c)); \
    return print(op,p,std::move(u),std::move(c)); }
PRINT_WITHOUT_WAIT(start_local_print, "print_local")
PRINT_WITHOUT_WAIT(start_sdcard_print, "print_sdcard")
#undef PRINT_WITHOUT_WAIT

#define CALLBACK_SETTER(method, type, member) \
int BambuLanProcess::method(type fn) { std::lock_guard<std::mutex> lock(m_callbacks->mutex); m_callbacks->member = std::move(fn); return 0; }
CALLBACK_SETTER(set_on_local_connect_fn, OnLocalConnectedFn, connection)
CALLBACK_SETTER(set_on_local_message_fn, OnMessageFn, message)
CALLBACK_SETTER(set_on_printer_connected_fn, OnPrinterConnectedFn, connected)
CALLBACK_SETTER(set_on_subscribe_failure_fn, GetSubscribeFailureFn, subscription_failure)
CALLBACK_SETTER(set_queue_on_main_fn, QueueOnMainFn, queue)
#undef CALLBACK_SETTER
// These account callbacks have a separate canonical owner. Installing/clearing
// a LAN view's callbacks must never replace that owner's callback storage.
int BambuLanProcess::set_on_ssdp_msg_fn(OnMsgArrivedFn) { return 0; }
int BambuLanProcess::set_on_message_fn(OnMessageFn) { return 0; }
int BambuLanProcess::set_on_user_message_fn(OnMessageFn) { return 0; }
int BambuLanProcess::set_server_callback(OnServerErrFn) { return 0; }
} // namespace Slic3r
