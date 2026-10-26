#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include "slic3r/Utils/PrinterConnectionStatus.hpp"

using namespace Slic3r;

TEST_CASE("Startup reconnect waits for initialization and runs only once", "[PrinterConnectionStatus]")
{
    PrinterConnectionRestore restore("saved-device");
    CHECK_FALSE(restore.take_target(false, false, false));
    CHECK(restore.take_target(true, false, false) == "saved-device");
    CHECK_FALSE(restore.take_target(true, false, false));
    PrinterConnectionRestore empty("");
    CHECK_FALSE(empty.take_target(true, false, false));
}

TEST_CASE("A user selection or shutdown cancels deferred startup reconnect", "[PrinterConnectionStatus]")
{
    PrinterConnectionRestore selection("saved-device");
    CHECK_FALSE(selection.take_target(false, true, false));
    CHECK_FALSE(selection.take_target(true, false, false));
    PrinterConnectionRestore closing("saved-device");
    CHECK_FALSE(closing.take_target(true, false, true));
    CHECK_FALSE(closing.take_target(true, false, false));
}

TEST_CASE("Bambu replies distinguish authentication rejection from protocol failure", "[PrinterConnectionStatus]")
{
    CHECK(printer_mqtt_response({0x20, 2, 0, 0}).status == PrinterLinkStatus::connected);
    CHECK(printer_mqtt_response({0x20, 2, 0, 5}).failure == PrinterLinkFailure::credentials_rejected);
    CHECK(printer_mqtt_response({0x20, 2, 0, 3}).failure == PrinterLinkFailure::service_rejected);
    CHECK(printer_mqtt_response({0x20, 2, 0, 99}).failure == PrinterLinkFailure::invalid_response);
    CHECK(printer_mqtt_response({0x20, 2, 2, 0}).status == PrinterLinkStatus::unreachable);
    CHECK(printer_mqtt_response({0, 0, 0, 0}).status == PrinterLinkStatus::unreachable);
}

TEST_CASE("Printer checks preserve useful failures without exposing server bodies", "[PrinterConnectionStatus]")
{
    CHECK(printer_http_response("snapmaker", R"({"result":{"klippy_connected":true}})", 200).status == PrinterLinkStatus::connected);
    const auto starting = printer_http_response("moonraker", R"({"result":{"klippy_connected":false}})", 200);
    CHECK(starting.status == PrinterLinkStatus::not_ready);
    CHECK(starting.failure == PrinterLinkFailure::printer_not_ready);
    CHECK(printer_http_response("moonraker", R"({"result":{"klippy_connected":"true"}})", 200).failure == PrinterLinkFailure::invalid_response);
    CHECK(printer_http_response("octoprint", R"({"server":"1.0","api":"0.1"})", 200).status == PrinterLinkStatus::connected);
    CHECK(printer_http_response("octoprint", "secret echoed by server", 401).failure == PrinterLinkFailure::credentials_rejected);
    CHECK(printer_http_response("flashforge", R"({"code":1})", 200).failure == PrinterLinkFailure::service_rejected);
    CHECK(printer_http_response("flashforge", R"({"code":0,"detail":{}})", 200).status == PrinterLinkStatus::connected);
    CHECK(printer_http_response("creality", R"({"model":"K1"})", 200).status == PrinterLinkStatus::connected);
    CHECK(printer_http_response("creality", "<html>sign in</html>", 200).failure == PrinterLinkFailure::invalid_response);
    const auto failure = printer_http_response("moonraker", "private server details", 503);
    CHECK(failure.failure == PrinterLinkFailure::http_error);
    CHECK(failure.http_status == 503);
    CHECK(printer_http_response("moonraker", "", 0).failure == PrinterLinkFailure::request_failed);
}

namespace {
std::string flashforge_detail(const std::string& state) { return R"({"code":0,"detail":{"status":")" + state + R"("}})"; }
} // namespace

TEST_CASE("A Flashforge printer is available only while it reports being ready", "[PrinterConnectionStatus]")
{
    const std::string state = GENERATE(as<std::string>{}, "ready", "READY", "Ready");
    const auto result = printer_http_response("flashforge", flashforge_detail(state), 200);
    CHECK(result.status == PrinterLinkStatus::connected);
    CHECK(result.failure == PrinterLinkFailure::none);
    CHECK(flashforge_job_readiness(flashforge_detail(state)) == PrinterLinkFailure::none);
}

TEST_CASE("A Flashforge printer in any other state answers but is busy", "[PrinterConnectionStatus]")
{
    // Firmware may report states beyond the documented ones; none of them takes a new job.
    const std::string state = GENERATE(as<std::string>{}, "busy", "printing", "PAUSED", "pausing", "heating",
                                       "calibrate_doing", "cancel", "error", "an_unlisted_state");
    const auto result = printer_http_response("flashforge", flashforge_detail(state), 200);
    CHECK(result.status == PrinterLinkStatus::not_ready);
    CHECK(result.failure == PrinterLinkFailure::printer_busy);
    CHECK(flashforge_job_readiness(flashforge_detail(state)) == PrinterLinkFailure::printer_busy);
}

TEST_CASE("A finished Flashforge print waits to be cleared before the next job", "[PrinterConnectionStatus]")
{
    const std::string state = GENERATE(as<std::string>{}, "completed", "Completed");
    const auto result = printer_http_response("flashforge", flashforge_detail(state), 200);
    CHECK(result.status == PrinterLinkStatus::not_ready);
    CHECK(result.failure == PrinterLinkFailure::print_finished);
    CHECK(flashforge_job_readiness(flashforge_detail(state)) == PrinterLinkFailure::print_finished);
}

TEST_CASE("A Flashforge reply without a machine state keeps the printer available", "[PrinterConnectionStatus]")
{
    const std::string reply = GENERATE(as<std::string>{}, R"({"code":0,"detail":{}})", R"({"code":0,"detail":{"status":""}})",
                                       R"({"code":0,"detail":{"status":null}})", R"({"code":0,"detail":{"status":3}})");
    CHECK(printer_http_response("flashforge", reply, 200).status == PrinterLinkStatus::connected);
    CHECK(flashforge_job_readiness(reply) == PrinterLinkFailure::none);
}

TEST_CASE("A Flashforge refusal outranks the machine state it reports", "[PrinterConnectionStatus]")
{
    const auto refused = printer_http_response("flashforge", R"({"code":2,"message":"Printer is busy","detail":{"status":"ready"}})", 200);
    CHECK(refused.status == PrinterLinkStatus::refused);
    CHECK(refused.failure == PrinterLinkFailure::service_rejected);
    const auto unavailable = printer_http_response("flashforge", R"({"code":0,"detail":{"status":"printing"}})", 503);
    CHECK(unavailable.status == PrinterLinkStatus::unreachable);
    CHECK(unavailable.failure == PrinterLinkFailure::http_error);
}

TEST_CASE("Unreadable Flashforge replies never claim a busy printer", "[PrinterConnectionStatus]")
{
    const std::string reply = GENERATE(as<std::string>{}, "", "not json", "[]", R"({"code":0})", R"({"detail":"printing"})");
    CHECK(flashforge_job_readiness(reply) == PrinterLinkFailure::none);
}

TEST_CASE("Project loading preserves independent and inspected printer setups", "[PrinterConnectionStatus]")
{
    CHECK(may_sync_connected_printer_after_project_load(true, false, false));
    CHECK_FALSE(may_sync_connected_printer_after_project_load(true, true, false));
    CHECK_FALSE(may_sync_connected_printer_after_project_load(true, false, true));
    CHECK_FALSE(may_sync_connected_printer_after_project_load(true, true, true));
    CHECK_FALSE(may_sync_connected_printer_after_project_load(false, false, false));
}
