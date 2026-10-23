#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/PrinterSessionRegistry.hpp"

#include <memory>
#include <chrono>
#include <string>

namespace {
struct FakeAgent {};
struct Connection {
    std::string dev_id, credentials;
    bool operator==(const Connection& other) const
    { return dev_id == other.dev_id && credentials == other.credentials; }
};
using Sessions = Slic3r::PrinterSessionRegistry<FakeAgent, Connection>;
}

TEST_CASE("A silent connected printer becomes eligible for a fresh connection", "[PrinterSessions]")
{
    Slic3r::PrinterSessionToken token;
    token.state = Slic3r::PrinterControlState::connected;
    const auto later = token.started_at + std::chrono::seconds(31);
    CHECK_FALSE(token.needs_reconnect(true, later));
    CHECK(token.needs_reconnect(false, later));
    CHECK_FALSE(token.needs_reconnect(false, token.started_at + std::chrono::seconds(29)));
}

TEST_CASE("Connection recovery respects pending attempts and rejected access", "[PrinterSessions]")
{
    Slic3r::PrinterSessionToken token;
    const auto deadline = token.started_at + std::chrono::seconds(30);
    CHECK_FALSE(token.needs_reconnect(false, deadline - std::chrono::milliseconds(1)));
    CHECK(token.needs_reconnect(false, deadline));
    token.state = Slic3r::PrinterControlState::rejected;
    CHECK_FALSE(token.needs_reconnect(false, deadline));
    token.state = Slic3r::PrinterControlState::offline;
    CHECK_FALSE(token.needs_reconnect(false, deadline));
    token.state = Slic3r::PrinterControlState::connected;
    token.valid = false;
    CHECK_FALSE(token.needs_reconnect(false, deadline));
}

TEST_CASE("Automatic reconnect waits for all transfers and fresh status after completion", "[PrinterSessions]")
{
    Slic3r::PrinterSessionToken token;
    token.state = Slic3r::PrinterControlState::connected;
    const auto during = token.started_at + std::chrono::seconds(60);
    REQUIRE(token.begin_transfer());
    REQUIRE(token.begin_transfer());
    CHECK(token.recovery_deferred(during));
    CHECK_FALSE(token.needs_reconnect(false, during));
    CHECK_FALSE(token.expire_if_idle(during));
    token.end_transfer(during);
    CHECK(token.recovery_deferred(during + std::chrono::seconds(29)));
    CHECK_FALSE(token.needs_reconnect(false, during + std::chrono::seconds(29)));
    token.end_transfer(during + std::chrono::seconds(30));
    CHECK_FALSE(token.needs_reconnect(false, during + std::chrono::seconds(59)));
    CHECK_FALSE(token.expire_if_idle(during + std::chrono::seconds(59)));
    CHECK(token.needs_reconnect(false, during + std::chrono::seconds(60)));
    CHECK_FALSE(token.recovery_deferred(during + std::chrono::seconds(60)));
    CHECK(token.expire_if_idle(during + std::chrono::seconds(60)));
    CHECK_FALSE(token.begin_transfer());
}

TEST_CASE("A transfer starting after the reconnect check prevents automatic expiration", "[PrinterSessions]")
{
    auto token = std::make_shared<Slic3r::PrinterSessionToken>();
    token->state = Slic3r::PrinterControlState::connected;
    const auto later = token->started_at + std::chrono::seconds(31);
    REQUIRE(token->needs_reconnect(false, later));
    {
        Slic3r::PrinterSessionTransfer transfer(token);
        REQUIRE(bool(transfer));
        CHECK_FALSE(token->expire_if_idle(later));
        CHECK(token->valid);
    }
    CHECK_FALSE(token->needs_reconnect(false, std::chrono::steady_clock::now()));
    CHECK(token->needs_reconnect(false, std::chrono::steady_clock::now() + std::chrono::seconds(31)));
}

TEST_CASE("Repeated selections reuse the same connection attempt and cached state", "[PrinterSessions]")
{
    Sessions sessions;
    auto agent = std::make_shared<FakeAgent>();
    const auto first = sessions.acquire(agent, {"one", "saved-code"});
    first.session.token->state = Slic3r::PrinterControlState::connected;
    const auto again = sessions.acquire(agent, {"one", "saved-code"});
    CHECK(again.reused);
    CHECK(again.retired.empty());
    CHECK(again.session.token == first.session.token);
    CHECK(again.session.token->state == Slic3r::PrinterControlState::connected);
}

TEST_CASE("An explicit connection retries a lost attempt with unchanged settings", "[PrinterSessions]")
{
    Sessions sessions;
    auto agent = std::make_shared<FakeAgent>();
    const auto lost = sessions.acquire(agent, {"one", "saved-code"});
    lost.session.token->state = Slic3r::PrinterControlState::offline;
    const auto retry = sessions.acquire(agent, {"one", "saved-code"});
    CHECK_FALSE(retry.reused);
    CHECK(retry.session.token != lost.session.token);
    CHECK_FALSE(lost.session.token->valid);
    CHECK(retry.session.token->state == Slic3r::PrinterControlState::connecting);
    REQUIRE(retry.retired.size() == 1);
    CHECK(retry.retired.front().token == lost.session.token);
    // The retry is pending, so a further request reuses it.
    CHECK(sessions.acquire(agent, {"one", "saved-code"}).reused);
}

TEST_CASE("Independent transports keep their device ownership across view changes", "[PrinterSessions]")
{
    Sessions sessions;
    auto a = std::make_shared<FakeAgent>(), b = std::make_shared<FakeAgent>();
    const auto one = sessions.acquire(a, {"one", {}});
    const auto two = sessions.acquire(b, {"two", {}});
    CHECK(one.session.token->valid);
    CHECK(two.retired.empty());
    CHECK(sessions.find("one").agent == a);
    CHECK(sessions.find("two").agent == b);
    CHECK_FALSE(sessions.find("unknown").agent);
    CHECK_FALSE(Sessions::accepts(one.session, "two"));
    CHECK(Sessions::accepts(one.session, "one"));
}

TEST_CASE("Singleton transports retire their previous target before replacement", "[PrinterSessions]")
{
    Sessions sessions;
    auto singleton = std::make_shared<FakeAgent>();
    const auto one = sessions.acquire(singleton, {"one", {}});
    const auto two = sessions.acquire(singleton, {"two", {}});
    REQUIRE(two.retired.size() == 1);
    CHECK(two.retired.front().params.dev_id == "one");
    CHECK_FALSE(one.session.token->valid);
    CHECK_FALSE(sessions.find("one").agent);
    CHECK(sessions.find("two").agent == singleton);
}

TEST_CASE("Edited credentials invalidate queued responses without losing other sessions", "[PrinterSessions]")
{
    Sessions sessions;
    auto a = std::make_shared<FakeAgent>(), b = std::make_shared<FakeAgent>();
    const auto old = sessions.acquire(a, {"one", "old-code"});
    const auto other = sessions.acquire(b, {"two", "other-code"});
    const auto changed = sessions.acquire(a, {"one", "new-code"});
    CHECK_FALSE(changed.reused);
    CHECK_FALSE(Sessions::accepts(old.session, "one"));
    CHECK(Sessions::accepts(changed.session, "one"));
    CHECK(other.session.token->valid);
    CHECK(changed.session.token->state == Slic3r::PrinterControlState::connecting);
}

TEST_CASE("Explicit reconnect and removal invalidate callbacks for that attempt only", "[PrinterSessions]")
{
    Sessions sessions;
    auto a = std::make_shared<FakeAgent>();
    const auto old = sessions.acquire(a, {"one", {}});
    sessions.remove("one");
    const auto next = sessions.acquire(a, {"one", {}});
    CHECK_FALSE(old.session.token->valid);
    CHECK(next.session.token != old.session.token);
    CHECK_FALSE(next.reused);
    sessions.remove("one");
    CHECK_FALSE(next.session.token->valid);
    CHECK(sessions.sessions().empty());
}

TEST_CASE("Refresh does not turn a rejected control attempt into a connection", "[PrinterSessions]")
{
    Sessions sessions;
    auto a = std::make_shared<FakeAgent>();
    const auto first = sessions.acquire(a, {"one", {}});
    first.session.token->state = Slic3r::PrinterControlState::rejected;
    const auto refresh = sessions.acquire(a, {"one", {}});
    CHECK(refresh.reused);
    CHECK(refresh.session.token->state == Slic3r::PrinterControlState::rejected);
}

TEST_CASE("An expired attempt cannot be reused even when its connection parameters match", "[PrinterSessions]")
{
    Sessions sessions;
    auto agent = std::make_shared<FakeAgent>();
    const auto first = sessions.acquire(agent, {"one", {}});
    first.session.token->valid = false;
    const auto retry = sessions.acquire(agent, {"one", {}});
    CHECK_FALSE(retry.reused);
    CHECK(retry.session.token != first.session.token);
    CHECK(retry.session.token->valid);
    REQUIRE(retry.retired.size() == 1);
    CHECK_FALSE(Sessions::accepts(retry.retired.front(), "one"));
}
