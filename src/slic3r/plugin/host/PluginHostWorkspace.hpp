#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
bool plugin_workspace_public_setting(const std::string& key);
// Plain value export used for live settings and object overrides.
nlohmann::json plugin_workspace_config_values(const DynamicPrintConfig& config);

// Tells whether the editable context changed between host calls without
// serializing it. Settings are compared with the previous call's, so a token
// differs whenever the plate, the scene state or a setting has changed since.
class PluginWorkspaceRevision
{
public:
    std::string token(std::size_t plate, std::size_t state, const std::vector<const DynamicPrintConfig*>& configs);

private:
    std::vector<DynamicPrintConfig> m_configs;
    std::size_t m_config_revision {0};
};

// UI-thread only. An opaque comparison token; never contains connection credentials.
std::string plugin_workspace_token();
}
