#pragma once

#include "ElegooCanvas.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <string>

namespace Slic3r {

inline bool is_elegoo_canvas_generic(const ElegooCanvasSlot& slot, const Preset& preset)
{
    // The scope suffix identifies a printer tune, not a different material.
    return preset.is_system && !preset.is_default &&
        boost::iequals(boost::trim_copy(preset.name.substr(0, preset.name.find('@'))), "Generic " + slot.material);
}

inline const Preset* match_elegoo_canvas_preset(const ElegooCanvasSlot& slot, const PresetCollection& filaments)
{
    const Preset* match = nullptr;
    bool targeted_match = false;
    bool ambiguous = false;
    for (const auto& preset : filaments) {
        if (!preset.is_compatible || !is_elegoo_canvas_generic(slot, preset) ||
            !elegoo_canvas_material_matches(slot, preset.config.opt_string("filament_type", 0u))) continue;
        const auto* printers = preset.config.option<ConfigOptionStrings>("compatible_printers");
        const bool targeted = printers && !printers->values.empty();
        if (!match || (targeted && !targeted_match)) {
            match = &preset;
            targeted_match = targeted;
            ambiguous = false;
        } else if (targeted == targeted_match) {
            ambiguous = true;
        }
    }
    // Visibility only controls the normal picker. A hidden installed generic is
    // still usable, but an ambiguous match must be reviewed instead of guessed.
    return ambiguous ? nullptr : match;
}

} // namespace Slic3r
