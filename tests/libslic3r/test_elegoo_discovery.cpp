#include <catch2/catch_test_macros.hpp>
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/DiscoveryPolicy.hpp"
#include "libslic3r/ElegooDiscovery.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace {
std::string notice(const std::string& serial = "CC2-FIXTURE", nlohmann::json lan = 1, nlohmann::json protection = 1)
{
    // Synthetic identities only. Shape follows the first-party CC2 wire format;
    // no Strata private/hardware fixture is copied or executed.
    return nlohmann::json{{"id",0},{"result",{{"machine_model","Centauri Carbon 2"},{"host_name","Workshop"},
        {"sn",serial},{"lan_status",lan},{"token_status",protection},{"ip","192.168.50.199"}}}}.dump();
}
class FakeTransport : public ElegooDiscoveryTransport {
public:
    Clock::time_point clock{};
    bool stop = false, flood = false, cancel_on_receive = false;
    size_t interfaces = 1, receives = 0, sends = 0, idles = 0;
    mutable size_t interface_queries = 0;
    std::vector<Clock::time_point> send_times;
    std::deque<ElegooDiscoveryDatagram> replies;
    Clock::time_point now() const override { return clock; }
    bool cancelled() const override { return stop; }
    size_t interface_count() const override { ++interface_queries; return interfaces; }
    void send_probe(size_t index, Clock::time_point deadline) override {
        CHECK(index < 4); CHECK(clock < deadline); ++sends;send_times.push_back(clock);
    }
    std::optional<ElegooDiscoveryDatagram> receive(size_t) override {
        ++receives;
        if (cancel_on_receive) stop = true;
        if (flood) return ElegooDiscoveryDatagram{"malformed", "192.168.50.10", 52700, true};
        if (replies.empty()) return std::nullopt;
        auto reply = std::move(replies.front());replies.pop_front();return reply;
    }
    void idle() override { ++idles;clock += std::chrono::milliseconds(20); }
};
}

TEST_CASE("CC2 advertisements retain setup requirements and use the reply source address", "[ElegooDiscovery]")
{
    const auto printer = parse_elegoo_printer_notice(notice(), "192.168.50.10", 52700);
    REQUIRE(printer);
    CHECK(printer->vendor == "elegoo");
    CHECK(printer->model == "Elegoo Centauri Carbon 2");
    CHECK(printer->address == "192.168.50.10");
    CHECK(printer->port == 0); // UDP discovery port is not an HTTP upload port
    CHECK(printer->preset.empty()); // discovery never knows the installed nozzle
    CHECK(printer->lan_mode == true);
    CHECK(printer->access_code_required == true);
    CHECK(discovered_printer_requires_code(*printer));
    const auto cloud = parse_elegoo_printer_notice(notice("CC2-FIXTURE",0,1), "192.168.50.10",52700);
    REQUIRE(cloud);
    CHECK_FALSE(cloud->local_control);
    CHECK(cloud->lan_mode == false);
    auto restored = DiscoveredPrinter::deserialize(cloud->serialize());
    REQUIRE(restored);
    CHECK(restored->lan_mode == false);
    CHECK(restored->access_code_required == true);
}

TEST_CASE("Unknown discovery mode and protection flags never imply ready or unprotected", "[ElegooDiscovery]")
{
    for (const auto& flag : {nlohmann::json(nullptr), nlohmann::json(2), nlohmann::json("1"), nlohmann::json::object()}) {
        const auto printer = parse_elegoo_printer_notice(notice("CC2-FIXTURE",flag,flag),"192.168.50.10",52700);
        REQUIRE(printer);
        CHECK_FALSE(printer->local_control);
        CHECK_FALSE(printer->lan_mode.has_value());
        CHECK_FALSE(printer->access_code_required.has_value());
        const auto protected_unknown = parse_elegoo_printer_notice(notice("CC2-FIXTURE",true,flag),"192.168.50.10",52700);
        REQUIRE(protected_unknown);
        CHECK(discovered_printer_requires_code(*protected_unknown));
    }
    const auto unprotected = parse_elegoo_printer_notice(notice("CC2-FIXTURE",true,false),"192.168.50.10",52700);
    REQUIRE(unprotected);
    CHECK_FALSE(discovered_printer_requires_code(*unprotected));
}

TEST_CASE("CC2 discovery rejects malformed oversized unrelated and invalid-source replies", "[ElegooDiscovery]")
{
    for (const auto* source : {"127.0.0.1","0.0.0.0","169.254.1.3","8.8.8.8","224.0.0.1","::1","invalid"})
        CHECK_FALSE(parse_elegoo_printer_notice(notice(),source,52700));
    CHECK_FALSE(parse_elegoo_printer_notice(notice(),"192.168.50.10",7125));
    CHECK_FALSE(parse_elegoo_printer_notice(std::string(8193,' '),"192.168.50.10",52700));
    auto bounded = notice(); bounded.resize(8192, ' ');
    CHECK(bool(parse_elegoo_printer_notice(bounded,"192.168.50.10",52700)));
    for (const auto* packet : {"", "{", "[]", "{}", R"({"id":0,"result":[]})"})
        CHECK_FALSE(parse_elegoo_printer_notice(packet,"192.168.50.10",52700));
    auto packet = nlohmann::json::parse(notice());
    for (const auto& id : {nlohmann::json(1),nlohmann::json(-1),nlohmann::json("0"),nlohmann::json(false)}) {
        packet["id"]=id;CHECK_FALSE(parse_elegoo_printer_notice(packet.dump(),"192.168.50.10",52700));
    }
    packet["id"]=0;
    for (const auto* model : {"Centauri Carbon","Centauri Carbon 20","Moonraker",""}) {
        packet["result"]["machine_model"]=model;CHECK_FALSE(parse_elegoo_printer_notice(packet.dump(),"192.168.50.10",52700));
    }
    packet["result"]["machine_model"]="Centauri Carbon 2 Combo";
    for (const auto& serial : {std::string(),std::string(129,'A'),std::string("bad/#"),std::string("bad\nserial")}) {
        packet["result"]["sn"]=serial;CHECK_FALSE(parse_elegoo_printer_notice(packet.dump(),"192.168.50.10",52700));
    }
    CHECK(elegoo_reply_on_interface(0xc0a8320a,0xc0a83205,0xffffff00));
    CHECK_FALSE(elegoo_reply_on_interface(0xc0a8330a,0xc0a83205,0xffffff00));
    CHECK_FALSE(elegoo_reply_on_interface(0xc0a832ff,0xc0a83205,0xffffff00));
    CHECK_FALSE(elegoo_reply_on_interface(0xc0a83200,0xc0a83205,0xffffff00));
    CHECK_FALSE(elegoo_reply_on_interface(0xc0a8320a,0xc0a83205,0));
}

TEST_CASE("Repeated CC2 sightings and DHCP changes preserve saved identity profile and endpoint", "[ElegooDiscovery]")
{
    auto printer = *parse_elegoo_printer_notice(notice(),"192.168.50.10",52700);
    printer.preset="Elegoo Centauri Carbon 2 0.6 nozzle";printer.name="My printer";printer.port=8088;printer.identity_key="previous-local-key";
    std::vector<DiscoveredPrinter> saved{printer};
    auto update = *parse_elegoo_printer_notice(notice(),"192.168.50.20",52700);
    CHECK_FALSE(merge_discovered_printer(saved,update));
    CHECK_FALSE(merge_discovered_printer(saved,update));
    REQUIRE(saved.size()==1);
    CHECK(saved[0].key()==printer.key());
    CHECK(saved[0].preset==printer.preset);
    CHECK(saved[0].name==printer.name);
    CHECK(saved[0].port==8088);
    CHECK(saved[0].address=="192.168.50.20");
    update.serial="OTHER-CC2";
    CHECK(merge_discovered_printer(saved,update));
    CHECK(saved.size()==2);
}

TEST_CASE("The real CC2 scan loop suppresses duplicate sightings and rejects off-interface replies", "[ElegooDiscovery]")
{
    FakeTransport transport;
    transport.replies={{notice(),"192.168.50.10",52700,false},{notice(),"192.168.50.10",52700,true},
        {notice(),"192.168.50.10",52700,true},{notice(),"192.168.50.20",52700,true}};
    std::vector<DiscoveredPrinter> found;
    CHECK(run_elegoo_discovery(transport,[&](DiscoveredPrinter p){found.push_back(std::move(p));}));
    REQUIRE(found.size()==2);
    CHECK(found[0].serial==found[1].serial);
    CHECK(found[1].address=="192.168.50.20");
    CHECK(transport.sends==3);
    CHECK(transport.send_times[1]-transport.send_times[0]>=std::chrono::seconds(2));
    CHECK(transport.clock<=ElegooDiscoveryTransport::Clock::time_point{}+std::chrono::seconds(6));
}

TEST_CASE("CC2 cancellation and receive budgets bound the actual scan loop without sockets", "[ElegooDiscovery]")
{
    FakeTransport cancelled;cancelled.stop=true;
    CHECK_FALSE(run_elegoo_discovery(cancelled,[](DiscoveredPrinter){}));
    CHECK(cancelled.interface_queries==0);
    CHECK(cancelled.receives==0);
    CHECK(cancelled.sends==0);
    FakeTransport mid;mid.cancel_on_receive=true;mid.replies={{notice(),"192.168.50.10",52700,true}};
    unsigned published=0;
    CHECK(run_elegoo_discovery(mid,[&](DiscoveredPrinter){++published;}));
    CHECK(published==0);
    CHECK(mid.receives==1);
    FakeTransport flood;flood.flood=true;flood.interfaces=100;
    CHECK(run_elegoo_discovery(flood,[](DiscoveredPrinter){}));
    CHECK(flood.receives==1024);
    CHECK(flood.sends==4);
}

TEST_CASE("A full CC2 scan result still publishes a known printer's new address", "[ElegooDiscovery]")
{
    FakeTransport transport;
    constexpr size_t limit = 256; // run_elegoo_discovery's distinct-printer bound
    for (size_t i=0;i<limit;++i) transport.replies.push_back({notice("CC2-"+std::to_string(i)),"192.168.50.10",52700,true});
    transport.replies.push_back({notice("CC2-NEW"),"192.168.50.30",52700,true});
    transport.replies.push_back({notice("CC2-0"),"192.168.50.20",52700,true});
    std::vector<DiscoveredPrinter> found;
    CHECK(run_elegoo_discovery(transport,[&](DiscoveredPrinter p){found.push_back(std::move(p));}));
    REQUIRE(found.size()==limit+1);
    CHECK(found.back().serial=="CC2-0");
    CHECK(found.back().address=="192.168.50.20");
    for (const auto& printer : found) CHECK(printer.serial!="CC2-NEW");
}

TEST_CASE("CC2 scan result and concurrent producer completion accounting remain bounded", "[ElegooDiscovery]")
{
    FakeTransport transport;
    for (size_t i=0;i<300;++i) transport.replies.push_back({notice("CC2-"+std::to_string(i)),"192.168.50.10",52700,true});
    unsigned count=0;
    CHECK(run_elegoo_discovery(transport,[&](DiscoveredPrinter){++count;}));
    CHECK(count==256);
    DiscoveryScanProgress progress;progress.reset(7);
    for (unsigned i=0;i<6;++i) progress.complete(i,false);
    progress.complete(0,true);progress.complete(99,true);
    CHECK_FALSE(progress.finished());
    CHECK_FALSE(progress.available());
    progress.complete(6,true);
    CHECK(progress.finished());
    CHECK(progress.available());
    progress.reset(7);
    CHECK_FALSE(progress.finished());
    DiscoveryScanGate gate;auto first=gate.try_acquire();
    REQUIRE(bool(first));CHECK_FALSE(bool(gate.try_acquire()));first.reset();CHECK(bool(gate.try_acquire()));
}
