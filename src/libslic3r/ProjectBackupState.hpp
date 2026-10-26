#pragma once

#include <cstddef>
#include <unordered_map>

namespace Slic3r {
// Edits outside model undo snapshots need independent save and recovery baselines.
// Accessed only on the UI thread, like the owning project's dirty state.
class ProjectBackupState
{
public:
    void changed(std::size_t project) { m_flags[project] = {true, true}; }
    bool dirty(std::size_t project, bool backup) const
    {
        const auto found = m_flags.find(project);
        return found != m_flags.end() && (backup ? found->second.backup : found->second.saved);
    }
    void clear(std::size_t project, bool backup)
    {
        const auto found = m_flags.find(project);
        if (found == m_flags.end()) return;
        (backup ? found->second.backup : found->second.saved) = false;
        if (!found->second.saved && !found->second.backup) m_flags.erase(found);
    }
    void forget(std::size_t project) { m_flags.erase(project); }
private:
    struct Flags { bool saved; bool backup; };
    std::unordered_map<std::size_t, Flags> m_flags;
};
}
