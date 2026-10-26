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
// The filament slots a plate configuration loads, one entry each. `used` holds
// the one-based slots the plate's objects, parts and feature settings print with.
nlohmann::json plugin_plate_filaments(const DynamicPrintConfig& config, const std::vector<int>& used);
// One value per filament slot for each requested filament setting, choosing each
// filament's extruder variant as slicing does. Other settings are left out.
nlohmann::json plugin_filament_values(const DynamicPrintConfig& config, const std::vector<std::string>& keys);

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

// Pure descriptor builder: incomplete/stale slices never expose a file path.
nlohmann::json plugin_sliced_gcode_descriptor(const std::string& state, int plate,
    const std::string& name, const std::string& filename, unsigned int result_id);

// UI-thread only. An opaque comparison token; never contains connection credentials.
std::string plugin_workspace_token();
}
