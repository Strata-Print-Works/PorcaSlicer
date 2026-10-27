#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <slic3r/GUI/DeviceCore/DevManager.h>
#include <slic3r/GUI/DeviceManager.hpp>
#include <libslic3r/AppConfig.hpp>
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include <slic3r/Utils/NetworkAgent.hpp>
#include <slic3r/Utils/NetworkAgentFactory.hpp>
#include <slic3r/Utils/OrcaCloudServiceAgent.hpp>
#include <slic3r/Utils/OrcaPrinterAgent.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using json = nlohmann::json;

namespace {

class StubCloudAgent final : public OrcaCloudServiceAgent
{
public:
    StubCloudAgent() : OrcaCloudServiceAgent("") {}

    int get_user_print_info(unsigned int* http_code, std::string* http_body) override
    {
        if (http_code)
            *http_code = 200;
        if (http_body)
            *http_body = R"({"devices":[]})";
        return 0;
    }

    std::string get_user_name() override { return "integration-test-user"; }
};

class TestPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit TestPrinterAgent(std::string id)
        : OrcaPrinterAgent(""), m_info{std::move(id), "Integration Test Agent", "1.0", "test agent"}
    {
    }

    AgentInfo get_agent_info() override { return m_info; }

    int send_message(std::string dev_id, std::string json_str, int, int) override
    {
        published_device = std::move(dev_id);
        published_message = json::parse(json_str);
        return 0;
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        return send_message(std::move(dev_id), std::move(json_str), qos, flag);
    }

    std::string published_device;
    json published_message;

private:
    AgentInfo m_info;
};

// OrcaPrinterAgent's connection/callback methods are inert stubs. These overrides
// reject requests without opening a transport, starting workers or launching wx.
class UnsupportedPrinterAgent final : public OrcaPrinterAgent
{
public:
    explicit UnsupportedPrinterAgent(int error) : OrcaPrinterAgent(""), m_error(error) {}
    AgentInfo get_agent_info() override { return {"unsupported", "Offline test", "1", ""}; }
    int send_message(std::string, std::string, int, int) override { return m_error; }
    int send_message_to_printer(std::string, std::string, int, int) override { return m_error; }

private:
    int m_error;
};

struct ScopedAppConfig
{
    AppConfig config;
};

std::string machine_list_response(const std::string& provider, const std::string& agent_id,
                                  std::uint64_t generation, const std::string& name)
{
    json machine;
    machine["dev_id"]      = "integration-device";
    machine["dev_name"]    = name;
    machine["dev_online"]  = true;
    machine["task_status"] = "idle";

    json response;
    response["provider"]   = provider;
    response["agent_id"]   = agent_id;
    response["generation"] = generation;
    response["devices"]    = json::array({machine});
    return response.dump();
}

} // namespace

TEST_CASE("Cloud accounts are optional and legacy Orca entries are ignored", "[LocalAccounts]")
{
    AppConfig config;
    CHECK(config.get_cloud_providers().empty());
    config.set_cloud_providers({"orca", "bbl"});
    CHECK(config.get_cloud_providers() == std::vector<std::string>{"bbl"});
    CHECK_FALSE(config.has_cloud_provider("orca"));
    config.remove_cloud_provider("bbl");
    CHECK(config.get_cloud_providers().empty());
    config.add_cloud_provider("orca");
    CHECK(config.get_cloud_providers().empty());
    config.add_cloud_provider("bbl");
    CHECK(config.has_cloud_provider("bbl"));
}

TEST_CASE("Old cloud settings do not start Orca services or change the local preset folder", "[LocalAccounts]")
{
    AppConfig config;
    config.set("preset_folder", "saved-local-presets");
    config.set_cloud_providers({"orca"});
    config.set_bool("sync_user_preset", true);
    CHECK(NetworkAgentFactory::create_cloud_agent(ORCA_CLOUD_PROVIDER, "") == nullptr);
    auto network = create_agent_from_config("", &config);
    REQUIRE(network != nullptr);
    CHECK(network->get_cloud_agent() == nullptr);
    CHECK_FALSE(network->is_user_login());
    CHECK(config.get("preset_folder") == "saved-local-presets");
}

TEST_CASE("Automatic printer requests preserve errors without opening control warnings", "[DeviceManager][DeviceRequestFeedback]")
{
    const int error = GENERATE(ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED, ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE);
    const std::string connection = GENERATE(std::string("lan"), std::string("cloud"));
    const bool background = GENERATE(false, true);
    auto printer = std::make_shared<UnsupportedPrinterAgent>(error);
    NetworkAgent network(nullptr, printer);
    PrinterConnectionParams params;
    params.dev_id = "offline-device";
    REQUIRE(network.connect_printer(params, printer) == 0);
    if (background)
        network.set_printer_agent(std::make_shared<TestPrinterAgent>("other-printer"));
    MachineObject machine(nullptr, &network, "Offline printer", params.dev_id, "");
    machine.dev_connection_type = connection;

    // No wx application runs here: reaching show_unsupported_dlg would attempt to
    // construct a window. A retained LAN session rejects credential requests in
    // NetworkAgent before the fake transport is called.
    CHECK(machine.command_get_version() == error);
    CHECK(machine.command_get_version(false) == error);
    const int access_error = background && connection == "lan" ? ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE : error;
    CHECK(machine.command_get_access_code() == access_error);
    CHECK(machine.command_request_push_all(true) == error);
    CHECK_FALSE(machine.m_unsupported_dlg_shown);
}

TEST_CASE("Network agent stamps user-machine responses with request context", "[DeviceManager][integration]")
{
    auto cloud = std::make_shared<StubCloudAgent>();
    NetworkAgent network(cloud, nullptr);
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent-a"));

    const std::uint64_t generation_before = network.get_user_machine_list_generation();
    unsigned int        http_code         = 0;
    std::string         body;
    REQUIRE(network.get_user_print_info(&http_code, &body, ORCA_CLOUD_PROVIDER) == 0);

    const json response = json::parse(body);
    CHECK(http_code == 200);
    CHECK(response["provider"] == ORCA_CLOUD_PROVIDER);
    CHECK(response["agent_id"] == "integration-agent-a");
    CHECK(response["generation"] == generation_before + 1);
}

TEST_CASE("Device manager ignores stale cloud machine responses", "[DeviceManager][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("integration-agent"));
    network.set_printer_agent(std::make_shared<TestPrinterAgent>("integration-agent"));
    DeviceManager manager(&network, false, &app_config.config);

    const std::uint64_t current_generation = network.get_user_machine_list_generation();
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation, "Fresh name"));

    auto machines = manager.get_user_machinelist();
    REQUIRE(machines.size() == 1);
    REQUIRE(machines.at("integration-device") != nullptr);
    CHECK(machines.at("integration-device")->get_dev_name() == "Fresh name");

    manager.parse_user_print_info(machine_list_response(BBL_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation, "Stale provider"));
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "other-agent",
                                                        current_generation, "Stale agent"));
    manager.parse_user_print_info(machine_list_response(ORCA_CLOUD_PROVIDER, "integration-agent",
                                                        current_generation + 1, "Stale generation"));

    machines = manager.get_user_machinelist();
    REQUIRE(machines.size() == 1);
    CHECK(machines.at("integration-device")->get_dev_name() == "Fresh name");
}

TEST_CASE("Device manager filters and rehomes devices by printer-agent ownership", "[DeviceManager][DeviceOwnership][integration]")
{
    ScopedAppConfig app_config;
    auto agent_a = std::make_shared<TestPrinterAgent>("integration-agent-a");
    auto agent_b = std::make_shared<TestPrinterAgent>("integration-agent-b");
    NetworkAgent network(nullptr, agent_a);
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id       = "integration-lan-device";
    machine.dev_name     = "Integration LAN device";
    machine.dev_ip       = "192.0.2.10";
    machine.printer_type = "C11";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "access-code");
    REQUIRE(object != nullptr);
    CHECK(object->printer_agent_id == "integration-agent-a");
    CHECK(manager.get_my_machine_list("integration-agent-a").count(machine.dev_id) == 1);
    CHECK(manager.get_my_machine_list("integration-agent-b").empty());

    network.set_printer_agent(agent_b);
    CHECK(manager.get_my_machine_list("integration-agent-b").empty());

    manager.on_machine_alive(R"({
        "dev_name":"Rediscovered device",
        "dev_id":"integration-lan-device",
        "dev_ip":"192.0.2.10",
        "dev_type":"C11",
        "dev_signal":"strong",
        "connect_type":"lan",
        "bind_state":"free"
    })");

    CHECK(object->printer_agent_id == "integration-agent-b");
    CHECK(manager.get_my_machine_list("integration-agent-a").empty());
    CHECK(manager.get_my_machine_list("integration-agent-b").count(machine.dev_id) == 1);
}

TEST_CASE("Delayed discovery does not reassign a retained printer to the visible backend", "[DeviceManager][DeviceOwnership][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("visible"));
    BBLocalMachine saved;
    saved.dev_id = "background";
    saved.dev_ip = "192.0.2.10";
    saved.dev_name = "Background printer";
    saved.printer_type = "C11";
    saved.printer_agent_id = "retained";
    saved.access_code = "offline-test-code";
    app_config.config.update_local_machine(saved);
    DeviceManager manager(&network, false, &app_config.config);
    manager.on_machine_alive(R"({
        "dev_name":"Background printer", "dev_id":"background", "dev_ip":"192.0.2.10",
        "dev_type":"C11", "dev_signal":"strong", "connect_type":"lan", "bind_state":"free",
        "__porca_printer_agent_id":"retained"
    })");
    REQUIRE(manager.get_local_machine("background") != nullptr);
    CHECK(manager.get_local_machine("background")->printer_agent_id == "retained");
    CHECK(manager.get_my_machine_list("visible").empty());
    CHECK(manager.get_my_machine_list("retained").count("background") == 1);
}

TEST_CASE("Saved sessions keep their own printer agent while another backend is visible", "[DeviceManager][DeviceOwnership][integration]")
{
    ScopedAppConfig app_config;
    NetworkAgent network(nullptr, std::make_shared<TestPrinterAgent>("visible"));
    DeviceManager manager(&network, false, &app_config.config);

    BBLocalMachine machine;
    machine.dev_id           = "background-session";
    machine.dev_name         = "Background session";
    machine.dev_ip           = "192.0.2.20";
    machine.printer_type     = "C11";
    machine.printer_agent_id = "retained";

    MachineObject* object = manager.insert_local_device(machine, "lan", "free", "", "offline-test-code");
    REQUIRE(object != nullptr);
    CHECK(object->printer_agent_id == "retained");
    CHECK(manager.get_my_machine_list("retained").count(machine.dev_id) == 1);
    CHECK(manager.get_my_machine_list("visible").empty());

    // Reusing the entry for another session start keeps the saved owner.
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "offline-test-code") == object);
    CHECK(object->printer_agent_id == "retained");

    // Callers without a saved owner still follow the visible backend.
    machine.printer_agent_id.clear();
    REQUIRE(manager.insert_local_device(machine, "lan", "free", "", "offline-test-code") == object);
    CHECK(object->printer_agent_id == "visible");
}

TEST_CASE("AMS filament settings keep legacy single-color commands compatible", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", "FF000080", "PLA", 190, 220) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(agent->published_device == object.get_dev_id());
    CHECK(payload.at("command") == "ams_filament_setting");
    CHECK(payload.at("ams_id") == 1);
    CHECK(payload.at("slot_id") == 2);
    CHECK(payload.at("tray_color") == "FF000080");
    CHECK_FALSE(payload.contains("cols"));
    CHECK_FALSE(payload.contains("ctype"));
}

TEST_CASE("AMS filament settings preserve ordered gradient and multicolor RGBA components", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");
    const int color_type = GENERATE(0, 1);
    const std::vector<std::string> colors{"FF000000", "0000FF40", "00FF00FF", "FF000000", "12345601", "FEDCBAFE", "ABCDEF80"};

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", colors.front(), "PLA", 190, 220,
                                                 colors, color_type) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(payload.at("tray_color") == colors.front());
    CHECK(payload.at("cols").get<std::vector<std::string>>() == colors);
    CHECK(payload.at("ctype") == color_type);
}

TEST_CASE("AMS filament settings default a supplied single-color list to type two", "[DeviceManager][AMSMaterialsSetting][integration]")
{
    ScopedAppConfig app_config;
    auto agent = std::make_shared<TestPrinterAgent>("integration-agent");
    NetworkAgent network(nullptr, agent);
    DeviceManager manager(&network, false, &app_config.config);
    MachineObject object(&manager, &network, "Test printer", "integration-device", "192.0.2.10");
    object.dev_connection_type = GENERATE("lan", "cloud");
    const std::vector<std::string> colors{"FF000080"};

    REQUIRE(object.command_ams_filament_settings(1, 2, "filament-id", "setting-id", colors.front(), "PLA", 190, 220,
                                                 colors) == 0);

    const json& payload = agent->published_message.at("print");
    CHECK(payload.at("cols").get<std::vector<std::string>>() == colors);
    CHECK(payload.at("ctype") == 2);
}
