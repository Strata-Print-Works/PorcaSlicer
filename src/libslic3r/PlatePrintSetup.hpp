#pragma once

#include "PrintConfig.hpp"
#include <string>

namespace Slic3r {

class PresetBundle;

// The editable setup belongs to a plate. The application's preset bundle is only
// the editing context for the selected plate, never the owner of all plate jobs.
struct PlatePrintSetup
{
    DynamicPrintConfig config;
    std::string destination;
    std::string destination_name;

    bool empty() const { return config.empty(); }
    void capture(const PresetBundle& bundle);
    void activate(PresetBundle& bundle, const std::string& plate_name) const;
    std::string serialize() const;
    static PlatePrintSetup deserialize(const std::string& value);

    template<class Archive> void serialize(Archive& ar) { ar(config, destination, destination_name); }
};

} // namespace Slic3r
