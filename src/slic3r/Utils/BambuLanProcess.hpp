#pragma once

#include "BBLPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

struct BambuLanProcessConfig {
    std::string executable, library, config_dir, resources, country;
    // A control request unanswered within this time returns an error. Only EOF
    // or a protocol error ends the worker; certificate and print operations
    // have their own, longer limits.
    std::chrono::milliseconds control_timeout{std::chrono::seconds(5)};
};

// One headless child and one SDK instance per LAN printer. Typed Bambu command
// builders remain inherited; only their transport crosses the private pipes.
// Callbacks never run on a pipe reader, and teardown never waits on the
// calling thread: a retired worker is reaped by an owned thread joined here.
class BambuLanProcess final : public BBLPrinterAgent {
public:
    explicit BambuLanProcess(BambuLanProcessConfig config);
    ~BambuLanProcess() override;
    bool owns_device_connection() const override { return true; }
    bool can_connect_to(const std::string& id) const override;
    int connect_printer(const PrinterConnectionParams& params) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string id, std::string payload, int qos, int flag) override;
    int check_cert() override;
    void install_device_cert(std::string id, bool lan_only) override;
    int start_print(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn) override;
    int start_local_print_with_record(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn) override;
    int start_send_gcode_to_sdcard(PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn) override;
    int start_local_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) override;
    int start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn) override;
    int set_on_message_fn(OnMessageFn) override;
    int set_on_user_message_fn(OnMessageFn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn) override;
    int set_on_local_message_fn(OnMessageFn) override;
    int set_queue_on_main_fn(QueueOnMainFn) override;
    int set_server_callback(OnServerErrFn) override;
private:
    struct Channel;
    struct Callbacks;
    struct Retired {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::shared_ptr<Callbacks> m_callbacks;
    BambuLanProcessConfig m_config;
    mutable std::mutex m_mutex;
    std::shared_ptr<Channel> m_channel;
    std::string m_device_id;
    std::thread m_reader;
    std::vector<Retired> m_retired;
    void retire_locked();
    std::shared_ptr<Channel> channel() const;
    int request(const std::string& op, nlohmann::json args = {},
                OnUpdateStatusFn update = {}, WasCancelledFn cancel = {}, OnWaitFn wait = {});
    int print(const std::string& op, const PrintParams&, OnUpdateStatusFn, WasCancelledFn, OnWaitFn = {});
};
} // namespace Slic3r
