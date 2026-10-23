#pragma once

#include <functional>
#include <mutex>
#include <memory>
#include <set>
#include <utility>

namespace Slic3r {

// Opaque SDK handles are identities, not evidence of independent C++ wrappers.
// Refuse a second owner when an SDK returns an existing process-wide handle.
// The creating module must outlive this owner and its destroy function.
class PluginAgentOwner {
public:
    PluginAgentOwner() = default;
    PluginAgentOwner(const PluginAgentOwner&) = delete;
    PluginAgentOwner& operator=(const PluginAgentOwner&) = delete;
    ~PluginAgentOwner() { reset(); }

    bool acquire(void* handle, std::function<int(void*)> destroy)
    {
        if (!handle || m_handle || !destroy) return false;
        auto state = registry();
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->handles.insert(handle).second) return false;
        m_registry = std::move(state);
        m_handle = handle;
        m_destroy = std::move(destroy);
        return true;
    }

    void* get() const { return m_handle; }

    int reset()
    {
        if (!m_handle) return 0;
        void* handle = std::exchange(m_handle, nullptr);
        auto destroy = std::move(m_destroy);
        auto state = std::move(m_registry);
        const int result = destroy(handle);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->handles.erase(handle);
        return result;
    }

private:
    struct Registry { std::mutex mutex; std::set<void*> handles; };
    static std::shared_ptr<Registry> registry()
    {
        static const auto state = std::make_shared<Registry>();
        return state;
    }
    // A late singleton destructor may run after the function-static reference
    // has gone away. Each handle keeps the registry alive through its release.
    std::shared_ptr<Registry> m_registry;
    void* m_handle = nullptr;
    std::function<int(void*)> m_destroy;
};

} // namespace Slic3r
