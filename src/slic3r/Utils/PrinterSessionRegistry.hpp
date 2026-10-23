#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

enum class PrinterControlState { connecting, connected, offline, rejected };

// A token belongs to one connection attempt, not a view or backend type. Keep
// it with queued work and check it again when that work reaches the UI thread.
struct PrinterSessionToken {
    const std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
    std::atomic<bool> valid{true};
    std::atomic<PrinterControlState> state{PrinterControlState::connecting};
    bool recovery_deferred(std::chrono::steady_clock::time_point now) const
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        return active_transfers != 0 || now < reconnect_after;
    }
    bool needs_reconnect(bool receiving_status, std::chrono::steady_clock::time_point now) const
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        const auto current = state.load();
        return valid && active_transfers == 0 && now >= reconnect_after &&
            now - started_at >= std::chrono::seconds(30) &&
            (current == PrinterControlState::connecting ||
             (current == PrinterControlState::connected && !receiving_status));
    }
    bool begin_transfer()
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        if (!valid) return false;
        ++active_transfers;
        return true;
    }
    void end_transfer(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        if (active_transfers > 0 && --active_transfers == 0)
            reconnect_after = now + std::chrono::seconds(30);
    }
    bool expire_if_idle(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        // Recheck under the same lock as begin_transfer: the UI may have
        // decided to reconnect just before a job started on its worker thread.
        std::lock_guard<std::mutex> lock(transfer_mutex);
        return active_transfers == 0 && now >= reconnect_after && valid.exchange(false);
    }
    void set_error(std::string reason) { std::lock_guard<std::mutex> lock(error_mutex); last_error = std::move(reason); }
    std::string error() const { std::lock_guard<std::mutex> lock(error_mutex); return last_error; }
private:
    mutable std::mutex transfer_mutex;
    unsigned active_transfers = 0;
    std::chrono::steady_clock::time_point reconnect_after{};
    mutable std::mutex error_mutex;
    std::string last_error;
};

// Automatic status recovery must not close a transport carrying an upload.
// Explicit disconnect/removal still invalidates its token and cancels the job.
class PrinterSessionTransfer {
public:
    explicit PrinterSessionTransfer(std::shared_ptr<PrinterSessionToken> token)
        : m_token(token && token->begin_transfer() ? std::move(token) : nullptr) {}
    ~PrinterSessionTransfer() { if (m_token) m_token->end_transfer(); }
    explicit operator bool() const { return bool(m_token); }
    PrinterSessionTransfer(const PrinterSessionTransfer&) = delete;
    PrinterSessionTransfer& operator=(const PrinterSessionTransfer&) = delete;
private:
    std::shared_ptr<PrinterSessionToken> m_token;
};

// Transport-independent ownership, also used by socket-free fake tests. A
// singleton adapter can own only one target; genuinely independent instances
// retain their sessions when the selected view changes. Lifecycle mutations are
// UI-thread operations; lookups may also come from command/camera workers.
template<class Agent, class Params> class PrinterSessionRegistry {
public:
    struct Session {
        std::shared_ptr<Agent> agent;
        Params params;
        std::shared_ptr<PrinterSessionToken> token;
    };
    struct Change {
        Session session;
        bool reused = false;
        std::vector<Session> retired;
    };

    // An explicit connection request reuses an attempt that is pending or
    // connected. A lost one is retried, even with unchanged settings.
    static bool reusable(const PrinterSessionToken& token)
    {
        return token.valid && token.state != PrinterControlState::offline;
    }

    Change acquire(std::shared_ptr<Agent> agent, const Params& params)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(params.dev_id);
        if (it != m_sessions.end() && reusable(*it->second.token) && it->second.agent == agent && it->second.params == params)
            return {it->second, true, {}};
        Change change;
        for (auto old = m_sessions.begin(); old != m_sessions.end();) {
            if (old->first == params.dev_id || old->second.agent == agent) {
                old->second.token->valid = false;
                change.retired.push_back(old->second);
                old = m_sessions.erase(old);
            } else ++old;
        }
        change.session = {std::move(agent), params, std::make_shared<PrinterSessionToken>()};
        m_sessions.emplace(params.dev_id, change.session);
        return change;
    }

    Session find(const std::string& id) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(id);
        return it == m_sessions.end() ? Session{} : it->second;
    }

    std::vector<Session> sessions() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Session> result;
        for (const auto& entry : m_sessions) result.push_back(entry.second);
        return result;
    }

    Session remove(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(id);
        if (it == m_sessions.end()) return {};
        Session session = it->second;
        session.token->valid = false;
        m_sessions.erase(it);
        return session;
    }

    static bool accepts(const Session& session, const std::string& id)
    {
        return session.token && session.token->valid && session.params.dev_id == id;
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, Session> m_sessions;
};

} // namespace Slic3r
