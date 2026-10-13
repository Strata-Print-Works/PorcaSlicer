#include "PlatePrintSetup.hpp"
#include "PresetBundle.hpp"
#include "Config.hpp"
#include "Exception.hpp"
#include "Preset.hpp"
#include "DiscoveredPrinter.hpp"
#include "BoundingBox.hpp"
#include "Point.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstddef>
#include <limits>
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r {

namespace {
bool local_connection_setting(const std::string& key)
{
    static constexpr std::array<const char*, 8> keys {
        "print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
        "printhost_user", "printhost_password", "printhost_port", "enable_filament_dynamic_map"
    };
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
}

void PlatePrintSetup::capture(const PresetBundle& bundle)
{
    config = bundle.full_config_secure();
    // Live routing must be refreshed from the destination, not replayed from a file.
    config.erase("enable_filament_dynamic_map");
}

void PlatePrintSetup::activate(PresetBundle& bundle, const std::string& plate_name) const
{
    if (empty())
        throw RuntimeError("The plate has no printing setup.");
    bundle.load_config_model(plate_name, config);
}

std::string PlatePrintSetup::serialize() const
{
    // GUI import/capture migrates legacy destinations through the local ID map.
    // Reject unmigrated callers too, rather than publishing a connection address.
    if (address_based_printer_destination(destination))
        throw RuntimeError("This printer's project destination must be updated before saving.");
    nlohmann::json values = nlohmann::json::object();
    nlohmann::json exact = nlohmann::json::object();
    for (const auto& key : config.keys()) {
        // Saving must be safe even when a caller supplied a config directly.
        if (local_connection_setting(key)) continue;
        const ConfigOption* option = config.option(key);
        values[key] = option->serialize();
        // Orca's display serialization rounds floats, and some generic enums have
        // no name table. Retain numeric values as well as the readable config.
        switch (option->type()) {
        case coEnum: exact[key] = option->getInt(); break;
        case coEnums: exact[key] = static_cast<const ConfigOptionEnumsGeneric*>(option)->values; break;
        case coFloat:
        case coPercent:
        case coFloatOrPercent: exact[key] = option->getFloat(); break;
        case coFloats:
        case coPercents: exact[key] = static_cast<const ConfigOptionVector<double>*>(option)->values; break;
        case coFloatsOrPercents: {
            exact[key] = nlohmann::json::array();
            for (const auto& value : static_cast<const ConfigOptionVector<FloatOrPercent>*>(option)->values)
                exact[key].push_back(value.value);
            break;
        }
        case coPoints: {
            exact[key] = nlohmann::json::array();
            for (const auto& point : static_cast<const ConfigOptionPoints*>(option)->values)
                exact[key].push_back({point.x(), point.y()});
            break;
        }
        default: break;
        }
    }
    return nlohmann::json{{"version", 1}, {"config", std::move(values)}, {"exact", std::move(exact)},
                          {"destination", destination}, {"destination_name", destination_name}}.dump();
}

PlatePrintSetup PlatePrintSetup::deserialize(const std::string& value)
{
    const auto data = nlohmann::json::parse(value);
    if (data.at("version") != 1 || !data.at("config").is_object())
        throw RuntimeError("This plate's printing setup requires a newer version of OrcaSlicer.");
    PlatePrintSetup setup;
    const auto exact = data.value("exact", nlohmann::json::object());
    if (!exact.is_object()) throw RuntimeError("Cannot restore this plate's printing setup.");
    for (const auto& item : data.at("config").items()) {
        // Project files describe print intent; connection details come from this computer.
        if (local_connection_setting(item.key())) continue;
        ConfigOption* option = setup.config.option(item.key(), true);
        if (!option)
            throw RuntimeError("Cannot restore this plate's setting: " + item.key());
        const bool numeric_enum = exact.contains(item.key()) && (option->type() == coEnum || option->type() == coEnums);
        if (!numeric_enum && !option->deserialize(item.value().get<std::string>()))
            throw RuntimeError("Cannot restore this plate's setting: " + item.key());
        if (!exact.contains(item.key())) continue;
        const auto& value = exact.at(item.key());
        switch (option->type()) {
        case coEnum: option->setInt(value.get<int>()); break;
        case coEnums: static_cast<ConfigOptionEnumsGeneric*>(option)->values = value.get<std::vector<int>>(); break;
        case coFloat:
        case coPercent:
        case coFloatOrPercent: static_cast<ConfigOptionFloat*>(option)->value = value.get<double>(); break;
        case coFloats:
        case coPercents: {
            auto& values = static_cast<ConfigOptionVector<double>*>(option)->values;
            values.clear();
            for (const auto& entry : value)
                values.push_back(entry.is_null() ? std::numeric_limits<double>::quiet_NaN() : entry.get<double>());
            break;
        }
        case coFloatsOrPercents: {
            auto& values = static_cast<ConfigOptionVector<FloatOrPercent>*>(option)->values;
            if (values.size() != value.size()) throw RuntimeError("Cannot restore this plate's setting: " + item.key());
            for (size_t i = 0; i < values.size(); ++i)
                values[i].value = value[i].is_null() ? std::numeric_limits<double>::quiet_NaN() : value[i].get<double>();
            break;
        }
        case coPoints: {
            auto& points = static_cast<ConfigOptionPoints*>(option)->values;
            points.clear();
            for (const auto& point : value) points.emplace_back(point.at(0).get<double>(), point.at(1).get<double>());
            break;
        }
        default: break;
        }
    }
    setup.destination = data.value("destination", std::string());
    setup.destination_name = data.value("destination_name", std::string());
    // Version 1 inherits Orca's implicit FFF technology when the field is absent.
    for (const char* key : {"printable_area", "printable_height", "nozzle_diameter",
                            "layer_height", "filament_diameter", "filament_type", "filament_colour"})
        if (!setup.config.has(key))
            throw RuntimeError("Cannot restore this plate's setting: " + std::string(key));
    const auto& area = setup.config.option<ConfigOptionPoints>("printable_area")->values;
    const auto& nozzles = setup.config.option<ConfigOptionFloats>("nozzle_diameter")->values;
    const double height = setup.config.opt_float("printable_height");
    if (Preset::printer_technology(setup.config) != ptFFF || area.size() < 3 ||
        std::any_of(area.begin(), area.end(), [](const Vec2d& point) { return !point.allFinite(); }) ||
        !std::isfinite(height) || height <= 0 || nozzles.empty() ||
        std::any_of(nozzles.begin(), nozzles.end(), [](double nozzle) { return !std::isfinite(nozzle) || nozzle <= 0; }))
        throw RuntimeError("This plate's printing setup is not supported.");
    const auto bed_size = BoundingBoxf(area).size();
    if (bed_size.x() <= 0 || bed_size.y() <= 0 ||
        bed_size.x() > std::numeric_limits<int>::max() || bed_size.y() > std::numeric_limits<int>::max())
        throw RuntimeError("Cannot restore this plate's printable area.");
    return setup;
}

} // namespace Slic3r
