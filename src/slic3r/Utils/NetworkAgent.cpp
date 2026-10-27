#include "NetworkAgent.hpp"
#include "PrinterSessionRegistry.hpp"

#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <system_error>
#include <thread>
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/Utils.hpp"
#include <stdlib.h>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <utility>
#include "CloudProvider.hpp"
#include "ICloudServiceAgent.hpp"
#include "ICameraSignalingChannel.hpp"
#include "IPrinterAgent.hpp"
#include "BBLNetworkPlugin.hpp"
#include "BBLPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"

namespace Slic3r {

namespace {

// Stamp the trusted origin into a JSON object payload without parsing and
// re-serializing every telemetry or discovery message. The key is appended:
// JSON readers here keep the last duplicate, so a payload cannot name its own
// origin. Returns false for a payload that is not an object; each caller
// decides whether to drop or forward it. Other malformed text is rejected by
// its reader, as before.
bool stamp_printer_agent_origin(std::string& payload, const std::string& origin_json)
{
    const auto first = payload.find_first_not_of(" \t\r\n");
    const auto last = payload.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || payload[first] != '{' || payload[last] != '}') return false;
    const bool empty = payload.find_first_not_of(" \t\r\n", first + 1) == last;
    payload.insert(last, (empty ? "\"__porca_printer_agent_id\":" : ",\"__porca_printer_agent_id\":") + origin_json);
    return true;
}

// Retired transports join their own workers when destroyed (a Moonraker
// websocket read, a Bambu helper reap), which can take seconds. One owned
// thread destroys them so the UI never waits. It runs only while work is
// queued. Callbacks are cleared before hand-off; every NetworkAgent drains this
// on destruction and the network module drains it before unloading, so no
// retired transport outlives its owner or the code it calls.
class RetiredPrinterAgents
{
public:
    static RetiredPrinterAgents& instance()
    {
        static RetiredPrinterAgents reaper;
        return reaper;
    }
    ~RetiredPrinterAgents() { drain(); }

    void retire(std::shared_ptr<IPrinterAgent> agent)
    {
        // Still shared (a session, the factory cache, queued work): not ours to destroy.
        if (!agent || agent.use_count() > 1) return;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(agent));
        if (m_running) return;
        // A finished worker cleared m_running under this lock and only returns now.
        if (m_thread.joinable()) m_thread.join();
        try {
            m_thread = std::thread([this] { run(); });
            m_running = true;
        } catch (const std::system_error&) {
            // Without a thread, destroy here rather than leak.
            auto pending = std::move(m_pending);
            m_pending.clear();
            lock.unlock();
        }
    }

    void drain()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_thread.get_id() == std::this_thread::get_id()) return; // A transport destructor.
        m_idle.wait(lock, [this] { return !m_running; });
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_pending.empty()) {
            auto batch = std::move(m_pending);
            m_pending.clear();
            lock.unlock();
            batch.clear(); // Destructors run without the lock; they may retire more.
            lock.lock();
        }
        m_running = false;
        m_idle.notify_all();
    }

    std::mutex m_mutex;
    std::condition_variable m_idle;
    std::vector<std::shared_ptr<IPrinterAgent>> m_pending;
    std::thread m_thread;
    bool m_running = false;
};

template<typename Fn>
int invoke_on_all_cloud_agents(const std::map<std::string, std::shared_ptr<ICloudServiceAgent>>& cloud_agents, Fn&& fn)
{
    if (cloud_agents.empty()) {
        return -1;
    }

    int result = 0;
    for (const auto& cloud_agent_pair : cloud_agents) {
        const int ret = fn(*cloud_agent_pair.second);
        if (result == 0 && ret != 0) {
            result = ret;
        }
    }

    return result;
}

} // namespace

// ============================================================================
// Static methods - delegate to BBLNetworkPlugin
// ============================================================================

std::string NetworkAgent::get_libpath_in_current_directory(std::string library_name)
{
    return BBLNetworkPlugin::get_libpath_in_current_directory(library_name);
}

std::string NetworkAgent::get_versioned_library_path(const std::string& version)
{
    return BBLNetworkPlugin::get_versioned_library_path(version);
}

bool NetworkAgent::versioned_library_exists(const std::string& version) { return BBLNetworkPlugin::versioned_library_exists(version); }

bool NetworkAgent::legacy_library_exists() { return BBLNetworkPlugin::legacy_library_exists(); }

void NetworkAgent::remove_legacy_library() { BBLNetworkPlugin::remove_legacy_library(); }

std::vector<std::string> NetworkAgent::scan_plugin_versions() { return BBLNetworkPlugin::scan_plugin_versions(); }

int NetworkAgent::initialize_network_module(bool using_backup, const std::string& version)
{
    return BBLNetworkPlugin::instance().initialize(using_backup, version);
}

int NetworkAgent::unload_network_module()
{
    // A retired Bambu SDK handle is destroyed through the module's own code.
    RetiredPrinterAgents::instance().drain();
    return BBLNetworkPlugin::instance().unload();
}

void NetworkAgent::wait_for_retired_printer_agents() { RetiredPrinterAgents::instance().drain(); }

bool NetworkAgent::is_network_module_loaded() { return BBLNetworkPlugin::instance().is_loaded(); }

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#else
void* NetworkAgent::get_bambu_source_entry() { return BBLNetworkPlugin::instance().get_bambu_source_entry(); }
#endif

std::string NetworkAgent::get_version() { return BBLNetworkPlugin::instance().get_version(); }

void* NetworkAgent::get_network_function(const char* name) { return BBLNetworkPlugin::instance().get_network_function(name); }

NetworkLibraryLoadError NetworkAgent::get_load_error() { return BBLNetworkPlugin::instance().get_load_error(); }

void NetworkAgent::clear_load_error() { BBLNetworkPlugin::instance().clear_load_error(); }

void NetworkAgent::set_load_error(const std::string& message, const std::string& technical_details, const std::string& attempted_path)
{
    BBLNetworkPlugin::instance().set_load_error(message, technical_details, attempted_path);
}

// ============================================================================
// Constructors
// ============================================================================

NetworkAgent::NetworkAgent(std::shared_ptr<ICloudServiceAgent> cloud_agent, std::shared_ptr<IPrinterAgent> printer_agent)
    : m_printer_agent(std::move(printer_agent))
{
    if (!cloud_agent) {
        return;
    }
    if (cloud_agent->get_id().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid cloud agent with empty ID provided, skipping agent initialization";
        return;
    }
    m_cloud_agents.emplace(cloud_agent->get_id(), std::move(cloud_agent));
}

NetworkAgent::~NetworkAgent()
{
    m_discovery_handler.reset();
    // Collect IDs first: a session snapshot would keep each transport alive here.
    std::vector<std::string> ids;
    for (const auto& session : m_printer_sessions.sessions()) ids.push_back(session.params.dev_id);
    for (const auto& id : ids) forget_printer_session(id);
    apply_printer_callbacks(m_account_printer, {});
    RetiredPrinterAgents::instance().drain();
}

void NetworkAgent::add_cloud_agent(const std::string& provider, std::shared_ptr<ICloudServiceAgent> agent)
{
    if (agent) {
        m_cloud_agents[provider] = std::move(agent);
    }
}

void NetworkAgent::set_printer_agent(std::shared_ptr<IPrinterAgent> printer_agent)
{
    auto old_printer_agent = get_printer_agent();
    if (printer_agent == old_printer_agent) return;
    m_user_machine_list_generation.fetch_add(1);
    m_discovery_session = std::make_shared<int>(0);

    std::atomic_store(&m_printer_agent, std::move(printer_agent));

    // Disconnect the old agent's connections/threads.
    bool retained = false;
    for (const auto& session : m_printer_sessions.sessions())
        retained = retained || session.agent == old_printer_agent;
    if (old_printer_agent && !retained) {
        old_printer_agent->disconnect_printer();
        apply_printer_callbacks(old_printer_agent, {});
    }

    if (get_printer_agent()) refresh_printer_callbacks();
    // A session or the factory cache may still own it; if not, it is destroyed off this thread.
    RetiredPrinterAgents::instance().retire(std::move(old_printer_agent));
}

void* NetworkAgent::get_network_agent() { return BBLNetworkPlugin::instance().get_agent(); }

void NetworkAgent::apply_printer_callbacks(const std::shared_ptr<IPrinterAgent>& printer_agent, const PrinterCallbacks& callbacks)
{
    if (!printer_agent) {
        return;
    }

    PrinterSessions::Session session;
    for (const auto& candidate : m_printer_sessions.sessions())
        if (candidate.agent == printer_agent) { session = candidate; break; }
    auto connected = callbacks.on_printer_connected_fn;
    auto subscribe_failure = callbacks.on_subscribe_failure_fn;
    auto local_connect = callbacks.on_local_connect_fn;
    auto local_message = callbacks.on_local_message_fn;
    auto cloud_message = callbacks.on_message_fn;
    const auto origin = nlohmann::json(printer_agent->get_agent_info().id).dump();
    // Plain-string SDK notices (wait_info, update_studio, cert_expired) are not
    // objects and carry no device state; forward them unchanged to the GUI.
    if (cloud_message) cloud_message = [fn = cloud_message, origin](std::string id, std::string payload) {
        stamp_printer_agent_origin(payload, origin);
        fn(id, std::move(payload));
    };
    if (session.token) {
        session.agent.reset(); // Callbacks must not own their own transport.
        // The wrapper carries the original attempt token through the backend's
        // queued callbacks. It never interprets a callback using the active view.
        if (connected) connected = [session, fn = connected](std::string id) {
            if (PrinterSessions::accepts(session, id) ||
                (id.rfind("tunnel/", 0) == 0 && PrinterSessions::accepts(session, id.substr(7)))) fn(id);
        };
        // State tracking needs no UI handler, but a retired attempt needs neither.
        if (local_connect || session.token->valid) local_connect = [session, fn = local_connect](int state, std::string id, std::string msg) {
            if (!PrinterSessions::accepts(session, id)) return;
            session.token->set_error(state == ConnectStatus::ConnectStatusOk ? std::string() : msg);
            session.token->state = state == ConnectStatus::ConnectStatusOk ? PrinterControlState::connected :
                msg == "5" ? PrinterControlState::rejected : PrinterControlState::offline;
            if (fn) fn(state, id, msg);
        };
        if (subscribe_failure) subscribe_failure = [session, fn = subscribe_failure](std::string id) {
            if (PrinterSessions::accepts(session, id)) fn(id);
        };
        if (local_message) local_message = [session, fn = local_message](std::string id, std::string message) {
            if (PrinterSessions::accepts(session, id)) fn(id, message);
        };
    }
    apply_discovery_callback(printer_agent, callbacks);
    printer_agent->set_on_printer_connected_fn(connected);
    printer_agent->set_on_subscribe_failure_fn(subscribe_failure);
    printer_agent->set_on_message_fn(cloud_message);
    printer_agent->set_on_user_message_fn(callbacks.on_user_message_fn);
    printer_agent->set_on_local_connect_fn(local_connect);
    printer_agent->set_on_local_message_fn(local_message);
    printer_agent->set_queue_on_main_fn(callbacks.queue_on_main_fn);
    printer_agent->set_server_callback(callbacks.on_server_err_fn);
}

void NetworkAgent::refresh_printer_callbacks()
{
    const auto active_printer = get_printer_agent();
    apply_printer_callbacks(active_printer, m_printer_callbacks);
    for (const auto& session : m_printer_sessions.sessions())
        if (session.agent != active_printer) apply_printer_callbacks(session.agent, m_printer_callbacks);
    // Account messages and discovery keep one callback owner, independent of
    // which LAN detail panel is visible. No LAN callbacks are installed here.
    if (BBLNetworkPlugin::instance().is_loaded()) {
        if (!m_account_printer) m_account_printer = std::make_shared<BBLPrinterAgent>();
        auto account_callbacks = m_printer_callbacks;
        account_callbacks.on_local_connect_fn = {};
        account_callbacks.on_local_message_fn = {};
        apply_printer_callbacks(m_account_printer, account_callbacks);
    }
}

bool NetworkAgent::has_printer_session(const std::string& id) const
{
    return bool(m_printer_sessions.find(id).token);
}

std::shared_ptr<PrinterSessionToken> NetworkAgent::printer_session_token(const std::string& id) const
{
    return m_printer_sessions.find(id).token;
}

std::shared_ptr<IPrinterAgent> NetworkAgent::printer_session_agent(const std::string& id) const
{
    return m_printer_sessions.find(id).agent;
}

void NetworkAgent::forget_printer_session(const std::string& id)
{
    auto session = m_printer_sessions.remove(id);
    if (!session.agent) return;
    apply_printer_callbacks(session.agent, {});
    session.agent->disconnect_printer();
    // A replacement SDK attempt gets a fresh handle and callback storage. Queued
    // SDK work cannot be reinterpreted using callbacks installed on an old handle.
    NetworkAgentFactory::release_printer_agent(session.agent);
    RetiredPrinterAgents::instance().retire(std::move(session.agent));
}

void NetworkAgent::forget_printer_sessions_for_agent(const std::string& agent_id)
{
    std::vector<std::string> ids;
    for (const auto& session : m_printer_sessions.sessions())
        if (session.agent->get_agent_info().id == agent_id) ids.push_back(session.params.dev_id);
    for (const auto& id : ids) forget_printer_session(id);
}

bool NetworkAgent::expire_printer_session(const std::string& id)
{
    const auto session = m_printer_sessions.find(id);
    if (!session.token || !session.token->expire_if_idle()) return false;
    session.token->state = PrinterControlState::offline;
    apply_printer_callbacks(session.agent, {});
    session.agent->disconnect_printer();
    NetworkAgentFactory::release_printer_agent(session.agent);
    // Retain identity until replacement so an active view follows its new
    // transport without selecting a different machine or changing Prepare.
    return true;
}

std::shared_ptr<IPrinterAgent> NetworkAgent::printer_agent_for(const std::string& id) const
{
    const auto session = m_printer_sessions.find(id);
    if (session.agent && session.token->valid) return session.agent;
    // Unknown LAN targets never fall through to the active transport. Cloud
    // APIs explicitly retain their separate, device-addressed dispatch below.
    return nullptr;
}

int NetworkAgent::apply_discovery_callback(const std::shared_ptr<IPrinterAgent>& printer_agent,
                                          const PrinterCallbacks& callbacks)
{
    if (!callbacks.on_ssdp_msg_fn)
        return printer_agent->set_on_ssdp_msg_fn(nullptr);

    const auto origin = nlohmann::json(printer_agent->get_agent_info().id).dump();
    const std::weak_ptr<int> session = m_discovery_session;
    const std::weak_ptr<int> handler = m_discovery_handler;
    std::shared_ptr<PrinterSessionToken> retained;
    for (const auto& candidate : m_printer_sessions.sessions())
        if (candidate.agent == printer_agent) { retained = candidate.token; break; }
    const auto accepts = [session, handler, retained] {
        return !handler.expired() && (!session.expired() || (retained && retained->valid));
    };
    return printer_agent->set_on_ssdp_msg_fn(
        [accepts, origin, fn = callbacks.on_ssdp_msg_fn, queue = callbacks.queue_on_main_fn](std::string payload) {
            if (!accepts() || !queue || !stamp_printer_agent_origin(payload, origin))
                return;
            queue([accepts, fn, payload = std::move(payload)] {
                // A retained device keeps its original backend when the detail view changes.
                if (accepts()) fn(payload);
            });
        });
}

std::shared_ptr<ICloudServiceAgent> NetworkAgent::get_cloud_agent(const std::string& provider) const
{
    const auto& key = (provider.empty() || provider == ORCA_CLOUD_PROVIDER) ? ORCA_CLOUD_PROVIDER : provider;
    auto it = m_cloud_agents.find(key);
    return it != m_cloud_agents.end() ? it->second : nullptr;
}

// ============================================================================
// Shared agent methods
// ============================================================================

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    m_printer_callbacks.queue_on_main_fn = fn;

    int ret = -1;
    if (cloud_agent)
        ret = cloud_agent->set_queue_on_main_fn(fn);
    refresh_printer_callbacks();
    return ret;
}

// ============================================================================
// Cloud agent methods
// ============================================================================

int NetworkAgent::init_log()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.init_log(); });
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [&config_dir](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_config_dir(config_dir); });
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&folder, &filename](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_cert_file(folder, filename);
    });
}

int NetworkAgent::set_country_code(std::string country_code)
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [&country_code](ICloudServiceAgent& cloud_agent) {
        return cloud_agent.set_country_code(country_code);
    });
}

int NetworkAgent::start()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.start(); });
}

int NetworkAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_server_connected_fn(fn); });
}

int NetworkAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_on_http_error_fn(fn); });
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    return invoke_on_all_cloud_agents(m_cloud_agents,
                                      [fn](ICloudServiceAgent& cloud_agent) { return cloud_agent.set_get_country_code_fn(fn); });
}

int NetworkAgent::change_user(std::string user_info, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->change_user(std::move(user_info));
    return -1;
}

bool NetworkAgent::is_user_login(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_user_login();
    return false;
}

int NetworkAgent::user_logout(bool request, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->user_logout(request);
    return -1;
}

std::string NetworkAgent::get_user_id(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_id();
    return "";
}

std::string NetworkAgent::get_user_name(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_name();
    return "";
}

std::string NetworkAgent::get_user_avatar(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_avatar();
    return "";
}

std::string NetworkAgent::get_user_nickname(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_nickname();
    return "";
}

std::string NetworkAgent::build_login_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_cmd();
    return "";
}

std::string NetworkAgent::build_logout_cmd(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_logout_cmd();
    return "";
}

std::string NetworkAgent::build_login_info(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->build_login_info();
    return "";
}

std::string NetworkAgent::get_cloud_service_host(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_service_host();
    return "";
}

std::string NetworkAgent::get_cloud_login_url(const std::string& language, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_cloud_login_url(language);
    return "";
}

int NetworkAgent::connect_server()
{
    return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.connect_server(); });
}

bool NetworkAgent::is_server_connected(const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->is_server_connected();
    return false;
}

int NetworkAgent::refresh_connection(const std::string& provider)
{
    if(provider.empty())
        return invoke_on_all_cloud_agents(m_cloud_agents, [](ICloudServiceAgent& cloud_agent) { return cloud_agent.refresh_connection(); });
    else {
        const auto cloud_agent = get_cloud_agent(provider);
        if (cloud_agent)
            return cloud_agent->refresh_connection();
        return -1;
    }
     
}

void NetworkAgent::enable_multi_machine(bool enable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        cloud_agent->enable_multi_machine(enable);
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_presets(user_presets);
    return -1;
}

std::string NetworkAgent::request_setting_id(std::string                         name,
                                             std::map<std::string, std::string>* values_map,
                                             unsigned int*                       http_code,
                                             const std::string&                  provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->request_setting_id(std::move(name), values_map, http_code);
    return "";
}

int NetworkAgent::put_setting(std::string                         setting_id,
                              std::string                         name,
                              std::map<std::string, std::string>* values_map,
                              unsigned int*                       http_code,
                              const std::string&                  provider,
                              bool force)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_setting(std::move(setting_id), std::move(name), values_map, http_code, force);
    return -1;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list(std::move(bundle_version), pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::get_setting_list2(
    std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_setting_list2(std::move(bundle_version), chk_fn, pro_fn, cancel_fn);
    return -1;
}

int NetworkAgent::delete_setting(std::string setting_id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->delete_setting(std::move(setting_id));
    return -1;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_message(type, after, limit, http_code, http_body);
    return -1;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->check_user_task_report(task_id, printable);
    return -1;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto request_printer = get_printer_agent();
    const std::string request_agent_id = request_printer ? request_printer->get_agent_info().id : "";
    const std::uint64_t request_generation = m_user_machine_list_generation.fetch_add(1) + 1;
    const auto cloud_agent = get_cloud_agent(provider);
    if (!cloud_agent)
        return -1;

    const int result = cloud_agent->get_user_print_info(http_code, http_body);
    if (result == 0 && http_body) {
        try {
            nlohmann::json response = nlohmann::json::parse(*http_body);
            response["provider"] = provider;
            response["agent_id"] = request_agent_id;
            response["generation"] = request_generation;
            *http_body = response.dump();
        }
        catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " metadata injection exception=" << e.what();
        }
    }
    return result;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_tasks(params, http_body);
    return -1;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_printer_firmware(std::move(dev_id), http_code, http_body);
    return -1;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_task_plate_index(std::move(task_id), plate_index);
    return -1;
}

int NetworkAgent::get_user_info(int* identifier, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_user_info(identifier);
    return -1;
}

int NetworkAgent::get_subtask_info(
    std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask_info(std::move(subtask_id), task_json, http_code, http_body);
    return -1;
}

int NetworkAgent::get_slice_info(
    std::string project_id, std::string profile_id, int plate_index, std::string* slice_json, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_slice_info(std::move(project_id), std::move(profile_id), plate_index, slice_json);
    return -1;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list,
                                    unsigned int*            http_code,
                                    std::string*             http_body,
                                    const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->query_bind_status(std::move(query_list), http_code, http_body);
    return -1;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->modify_printer_name(std::move(dev_id), std::move(dev_name));
    return -1;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_camera_url(std::move(dev_id), std::move(callback));
    return -1;
}

std::unique_ptr<ICameraSignalingChannel>
NetworkAgent::create_camera_signaling_channel(const std::string& dev_id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->create_camera_signaling_channel(dev_id);
    return nullptr;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_design_staffpick(offset, limit, std::move(callback));
    return -1;
}

int NetworkAgent::start_publish(
    PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string* out, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->start_publish(params, update_fn, cancel_fn, out);
    return -1;
}

int NetworkAgent::get_model_publish_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_publish_url(url);
    return -1;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_subtask(task, getsub_fn);
    return -1;
}

int NetworkAgent::get_model_mall_home_url(std::string* url, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_home_url(url);
    return -1;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_detail_url(url, std::move(id));
    return -1;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_profile(std::move(token), http_code, http_body);
    return -1;
}

int NetworkAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_my_token(std::move(ticket), http_code, http_body);
    return -1;
}

int NetworkAgent::track_enable(bool enable)
{
    // Orca cloud has no telemetry; the only cloud agent that tracks events is BBL.
    this->enable_track = enable;
    const auto cloud_agent = get_cloud_agent(BBL_CLOUD_PROVIDER);
    if (cloud_agent)
        return cloud_agent->track_enable(enable);
    return 0;
}

int NetworkAgent::track_remove_files()
{
    const auto cloud_agent = get_cloud_agent(BBL_CLOUD_PROVIDER);
    if (cloud_agent)
        return cloud_agent->track_remove_files();
    return 0;
}

int NetworkAgent::track_event(std::string evt_key, std::string content, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_event(std::move(evt_key), std::move(content));
    return -1;
}

int NetworkAgent::track_header(std::string header, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_header(std::move(header));
    return -1;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_update_property(std::move(name), std::move(value), std::move(type));
    return -1;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->track_get_property(std::move(name), value, std::move(type));
    return -1;
}

int NetworkAgent::put_model_mall_rating(int                      design_id,
                                        int                      score,
                                        std::string              content,
                                        std::vector<std::string> images,
                                        unsigned int&            http_code,
                                        std::string&             http_error,
                                        const std::string&       provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_model_mall_rating(design_id, score, std::move(content), std::move(images), http_code, http_error);
    return -1;
}

int NetworkAgent::get_oss_config(
    std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_oss_config(config, std::move(country_code), http_code, http_error);
    return -1;
}

int NetworkAgent::put_rating_picture_oss(std::string&       config,
                                         std::string&       pic_oss_path,
                                         std::string        model_id,
                                         int                profile_id,
                                         unsigned int&      http_code,
                                         std::string&       http_error,
                                         const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->put_rating_picture_oss(config, pic_oss_path, std::move(model_id), profile_id, http_code, http_error);
    return -1;
}

int NetworkAgent::get_model_mall_rating_result(
    int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_model_mall_rating_result(job_id, rating_result, http_code, http_error);
    return -1;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_preference(std::move(callback));
    return -1;
}

int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback, const std::string& provider)
{
    const auto cloud_agent = get_cloud_agent(provider);
    if (cloud_agent)
        return cloud_agent->get_mw_user_4ulist(seed, limit, std::move(callback));
    return -1;
}

// ============================================================================
// Printer agent methods
// ============================================================================

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    const auto active_printer = get_printer_agent();
    m_discovery_session = std::make_shared<int>(0);
    m_discovery_handler = std::make_shared<int>(0);
    m_printer_callbacks.on_ssdp_msg_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_printer_connected_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_subscribe_failure_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_message_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_user_message_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_local_connect_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_local_message_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    const auto active_printer = get_printer_agent();
    m_printer_callbacks.on_server_err_fn = fn;
    refresh_printer_callbacks();
    return active_printer ? 0 : -1;
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    const auto active_printer = get_printer_agent();
    auto printer = printer_agent_for(dev_id);
    if (!printer) printer = active_printer;
    if (printer)
        return printer->send_message(dev_id, json_str, qos, flag);
    return -1;
}

std::shared_ptr<IPrinterAgent> NetworkAgent::command_printer(const std::string& dev_id, bool routed, int& error) const
{
    const auto active_printer = get_printer_agent();
    auto printer = routed ? printer_agent_for(dev_id) : active_printer;
    error = !printer ? -1 : printer != active_printer ? ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE : 0;
    return error == 0 ? printer : nullptr;
}

int NetworkAgent::command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_ams_refresh_rfid(dev_id, ams_id, slot_id, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_ams_calibrate(dev_id, ams_id, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_ams_select_tray(dev_id, tray_id, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_start_camera(std::string dev_id)
{
    int error = 0;
    const auto printer = command_printer(dev_id, has_printer_session(dev_id), error);
    return printer ? printer->command_start_camera(dev_id) : error;
}

int NetworkAgent::command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_xyz_abs(dev_id, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_auto_leveling(dev_id, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_go_home(dev_id, is_printing, supports_mqtt_homing, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_set_bed(dev_id, temp, supports_mqtt_bed_ctrl, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_set_nozzle(dev_id, temp, sequence_id, lan_mode) : error;
}

int NetworkAgent::command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed,
                                        bool is_core_xy, bool supports_mqtt_axis_control, int sequence_id, bool lan_mode)
{
    int error = 0;
    const auto printer = command_printer(dev_id, lan_mode, error);
    return printer ? printer->command_axis_control(dev_id, axis, unit, input_val, speed, is_core_xy, supports_mqtt_axis_control, sequence_id, lan_mode) : error;
}

int NetworkAgent::connect_printer(const PrinterConnectionParams& params)
{
    auto printer = get_printer_agent();
    if (!printer || params.dev_id.empty()) return -1;
    const auto existing = m_printer_sessions.find(params.dev_id);
    if (existing.agent && existing.agent->get_agent_info().id == printer->get_agent_info().id)
        printer = existing.agent;
    // The ordinary LAN picker changes devices within the same backend too.
    // Resolve that device's transport before acquiring a session, so the
    // previous view's handle can never be retargeted or evict another session.
    if (auto* bambu = dynamic_cast<BBLPrinterAgent*>(printer.get()); bambu &&
        (!bambu->owns_device_connection() || !bambu->can_connect_to(params.dev_id) ||
         (existing.token && (!PrinterSessions::reusable(*existing.token) || !(existing.params == params))))) {
        if (existing.agent) NetworkAgentFactory::release_printer_agent(existing.agent);
        printer = NetworkAgentFactory::create_printer_agent_by_id("bbl", get_cloud_agent(BBL_CLOUD_PROVIDER), data_dir(), params.dev_id);
        if (!printer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }
    if (printer != get_printer_agent()) set_printer_agent(printer);
    return connect_printer(params, std::move(printer));
}

int NetworkAgent::connect_printer(const PrinterConnectionParams& params, std::shared_ptr<IPrinterAgent> printer)
{
    if (!printer || params.dev_id.empty()) return -1;
    auto previous = m_printer_sessions.find(params.dev_id);
    auto change = m_printer_sessions.acquire(std::move(printer), params);
    if (change.reused) return 0;
    if (previous.agent && previous.agent == get_printer_agent())
        std::atomic_store(&m_printer_agent, change.session.agent);
    previous = {}; // Only the reaper may hold the last reference to a retired transport.
    for (auto& retired : change.retired) {
        // Retrying a device on its own transport keeps that transport; the new
        // attempt supersedes the old one. A singleton moved to another device
        // still disconnects its previous target.
        if (retired.agent == change.session.agent && retired.params.dev_id == params.dev_id) continue;
        apply_printer_callbacks(retired.agent, {});
        retired.agent->disconnect_printer();
        NetworkAgentFactory::release_printer_agent(retired.agent);
        RetiredPrinterAgents::instance().retire(std::move(retired.agent));
    }
    refresh_printer_callbacks();
    const int result = change.session.agent->connect_printer(params);
    if (result != 0) change.session.token->state = PrinterControlState::offline;
    return result;
}

int NetworkAgent::disconnect_printer()
{
    const auto active_printer = get_printer_agent();
    for (const auto& session : m_printer_sessions.sessions()) {
        if (session.agent == active_printer) {
            forget_printer_session(session.params.dev_id);
            return 0;
        }
    }
    return active_printer ? active_printer->disconnect_printer() : -1;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    const auto active_printer = get_printer_agent();
    if (auto printer = printer_agent_for(dev_id)) {
        if (printer != active_printer) {
            const auto message = nlohmann::json::parse(json_str, nullptr, false);
            const auto is_request = [&](const char* section, const char* command) {
                if (!message.is_object() || message.size() != 1) return false;
                const auto part = message.find(section);
                return part != message.end() && part->is_object() && part->contains("command") &&
                       (*part)["command"] == command;
            };
            // Background sessions may only refresh status. An access-code echo
            // would copy the saved credential into telemetry and app config.
            if (!is_request("info", "get_version") && !is_request("pushing", "pushall"))
                return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
        }
        return printer->send_message_to_printer(dev_id, json_str, qos, flag);
    }
    return -1;
}

std::string NetworkAgent::default_lan_username() const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->default_lan_username();
    return {};
}

int NetworkAgent::check_cert()
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->check_cert();
    return -1;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    const auto active_printer = get_printer_agent();
    if (auto printer = has_printer_session(dev_id) ? printer_agent_for(dev_id) : active_printer)
        printer->install_device_cert(dev_id, lan_only);
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->start_discovery(start, sending);
    return false;
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->ping_bind(ping_code);
    return -1;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->bind_detect(dev_ip, sec_link, detect);
    return -1;
}

int NetworkAgent::bind(
    std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->bind(dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn);
    return -1;
}

int NetworkAgent::unbind(std::string dev_id)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->unbind(dev_id);
    return -1;
}

std::string NetworkAgent::get_user_selected_machine()
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->get_user_selected_machine();
    return "";
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    const auto active_printer = get_printer_agent();
    BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::set_user_selected_machine: dev_id=" << dev_id
                            << " printer_agent=" << (active_printer ? active_printer->get_agent_info().id : "<null>");
    if (active_printer) {
        const int result = active_printer->set_user_selected_machine(dev_id);
        BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::set_user_selected_machine: result=" << result;
        return result;
    }
    BOOST_LOG_TRIVIAL(warning) << "NetworkAgent::set_user_selected_machine: no printer agent";
    return -1;
}

int NetworkAgent::start_subscribe(std::string module)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->start_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->stop_subscribe(std::move(module));
    return -1;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    const auto active_printer = get_printer_agent();
    BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::add_subscribe: count=" << dev_list.size()
                            << " printer_agent=" << (active_printer ? active_printer->get_agent_info().id : "<null>");
    if (active_printer) {
        const int result = active_printer->add_subscribe(std::move(dev_list));
        BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::add_subscribe: result=" << result;
        return result;
    }
    BOOST_LOG_TRIVIAL(warning) << "NetworkAgent::add_subscribe: no printer agent";
    return -1;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    const auto active_printer = get_printer_agent();
    BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::del_subscribe: count=" << dev_list.size()
                            << " printer_agent=" << (active_printer ? active_printer->get_agent_info().id : "<null>");
    if (active_printer) {
        const int result = active_printer->del_subscribe(std::move(dev_list));
        BOOST_LOG_TRIVIAL(trace) << "NetworkAgent::del_subscribe: result=" << result;
        return result;
    }
    BOOST_LOG_TRIVIAL(warning) << "NetworkAgent::del_subscribe: no printer agent";
    return -1;
}

int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    const bool lan = params.connection_type == "lan";
    const auto session = m_printer_sessions.find(params.dev_id);
    PrinterSessionTransfer transfer(lan ? session.token : nullptr);
    if (lan && !transfer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (auto printer = lan ? session.agent : get_printer_agent())
        return printer->start_print(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    const bool lan = params.connection_type == "lan";
    const auto session = m_printer_sessions.find(params.dev_id);
    PrinterSessionTransfer transfer(lan ? session.token : nullptr);
    if (lan && !transfer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (auto printer = lan ? session.agent : get_printer_agent())
        return printer->start_local_print_with_record(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    const bool lan = params.connection_type == "lan";
    const auto session = m_printer_sessions.find(params.dev_id);
    PrinterSessionTransfer transfer(lan ? session.token : nullptr);
    if (lan && !transfer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (auto printer = lan ? session.agent : get_printer_agent())
        return printer->start_send_gcode_to_sdcard(params, update_fn, cancel_fn, wait_fn);
    return -1;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    const bool lan = params.connection_type == "lan";
    const auto session = m_printer_sessions.find(params.dev_id);
    PrinterSessionTransfer transfer(lan ? session.token : nullptr);
    if (lan && !transfer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (auto printer = lan ? session.agent : get_printer_agent())
        return printer->start_local_print(params, update_fn, cancel_fn);
    return -1;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    const bool lan = params.connection_type == "lan";
    const auto session = m_printer_sessions.find(params.dev_id);
    PrinterSessionTransfer transfer(lan ? session.token : nullptr);
    if (lan && !transfer) return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    if (auto printer = lan ? session.agent : get_printer_agent())
        return printer->start_sdcard_print(params, update_fn, cancel_fn);
    return -1;
}

FilamentSyncMode NetworkAgent::get_filament_sync_mode() const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->get_filament_sync_mode();
    return FilamentSyncMode::none;
}

bool NetworkAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode)
{
    const auto active_printer = get_printer_agent();
    if (auto printer = has_printer_session(dev_id) ? printer_agent_for(dev_id) : active_printer) {
        return printer->fetch_filament_info(dev_id, sync_mode);
    }
    return false;
}

std::string NetworkAgent::to_orca_filament_id(const std::string& printer_filament_id) const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->to_orca_filament_id(printer_filament_id);
    return printer_filament_id;
}

std::string NetworkAgent::from_orca_filament_id(const std::string& orca_filament_id) const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->from_orca_filament_id(orca_filament_id);
    return orca_filament_id;
}

CameraStreamMode NetworkAgent::get_camera_stream_mode() const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->get_camera_stream_mode();
    return CameraStreamMode::none;
}

std::string NetworkAgent::get_local_camera_stream_url() const
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->get_camera_url();
    return {};
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    const auto active_printer = get_printer_agent();
    if (active_printer)
        return active_printer->request_bind_ticket(ticket);
    return -1;
}

int NetworkAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    const auto active_printer = get_printer_agent();
    if (auto printer = has_printer_session(dev_id) ? printer_agent_for(dev_id) : active_printer)
        return printer->get_hms_snapshot(dev_id, file_name, callback);
    return -1;
}

} // namespace Slic3r
