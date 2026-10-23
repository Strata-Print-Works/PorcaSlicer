#include "PrintDeliveryValidation.hpp"
#include "LocalesUtils.hpp"

#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ios>
#include <string>
#include <string_view>
#include <utility>

namespace Slic3r {
namespace {

std::string value_after(const std::string& line, const std::string& key)
{
    const std::string prefix = "; " + key + " = ";
    return line.compare(0, prefix.size(), prefix) == 0 ? line.substr(prefix.size()) : std::string();
}

bool same_identity(const PrintDeliveryIdentity& a, const PrintDeliveryIdentity& b)
{
    return !a.model.empty() && !a.nozzle.empty() && a.model == b.model && a.nozzle == b.nozzle;
}

bool parse_nozzle(std::string_view text, double& value)
{
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    size_t parsed = 0;
    value = string_to_double_decimal_point(text, &parsed);
    return !text.empty() && parsed == text.size() && std::isfinite(value) && value > 0.;
}

// A profile names one nozzle variant ("0.4"); a G-code footer lists every
// extruder's nozzle ("0.4,0.4"). Each listed nozzle must match the variant.
bool same_sliced_nozzle(const std::string& variant, const std::string& sliced)
{
    if (variant.empty() || sliced.empty()) return false;
    if (variant == sliced) return true;
    double expected = 0.;
    if (!parse_nozzle(variant, expected)) return false;
    std::string_view rest(sliced);
    for (;;) {
        const size_t comma = rest.find(',');
        double value = 0.;
        if (!parse_nozzle(rest.substr(0, comma), value) || std::abs(value - expected) > 1e-6) return false;
        if (comma == std::string_view::npos) return true;
        rest.remove_prefix(comma + 1);
    }
}

} // namespace

bool confirmed_flashforge_profile(const std::string& physical_model, const std::string& profile_model)
{
    return !physical_model.empty() && physical_model != "Flashforge" && physical_model == profile_model;
}

PrintDeliveryIdentity read_gcode_delivery_identity(const std::string& path)
{
    // UTF-8 paths, including non-ASCII Windows paths, need the nowide stream.
    boost::nowide::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size <= 0) return {};
    constexpr std::streamoff footer_limit = 1024 * 1024;
    const std::streamoff start = std::max<std::streamoff>(0, std::streamoff(size) - footer_limit);
    input.seekg(start);
    std::string line;
    if (start > 0) std::getline(input, line); // Discard a partial line.
    PrintDeliveryIdentity identity;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto value = value_after(line, "printer_model"); !value.empty()) identity.model = std::move(value);
        if (auto value = value_after(line, "nozzle_diameter"); !value.empty()) identity.nozzle = std::move(value);
        if (auto value = value_after(line, "printer_settings_id"); !value.empty()) identity.preset = std::move(value);
    }
    return identity;
}

PrintDeliveryIssue check_flashforge_delivery(const std::string& physical_model,
    const PrintDeliveryIdentity& saved_profile,
    const PrintDeliveryIdentity& plate_setup,
    const PrintDeliveryIdentity& editing_setup,
    const PrintDeliveryIdentity& sliced_gcode,
    bool slice_ready)
{
    if (physical_model.empty() || physical_model == "Flashforge")
        return PrintDeliveryIssue::unknown_destination_model;
    if (!confirmed_flashforge_profile(physical_model, saved_profile.model) || saved_profile.nozzle.empty())
        return PrintDeliveryIssue::destination_profile_mismatch;
    if (!same_identity(saved_profile, plate_setup))
        return PrintDeliveryIssue::plate_profile_mismatch;
    if (!same_identity(plate_setup, editing_setup))
        return PrintDeliveryIssue::editing_profile_mismatch;
    if (!slice_ready) return PrintDeliveryIssue::slice_not_ready;
    if (sliced_gcode.model.empty() || sliced_gcode.nozzle.empty())
        return PrintDeliveryIssue::slice_identity_unavailable;
    if (plate_setup.model != sliced_gcode.model || !same_sliced_nozzle(plate_setup.nozzle, sliced_gcode.nozzle) ||
        (!plate_setup.preset.empty() && !sliced_gcode.preset.empty() && plate_setup.preset != sliced_gcode.preset))
        return PrintDeliveryIssue::slice_profile_mismatch;
    return PrintDeliveryIssue::none;
}

} // namespace Slic3r
