#pragma once

#include <string>

namespace Slic3r {

struct PrintDeliveryIdentity {
    std::string model;
    std::string nozzle;
    std::string preset;
};

bool confirmed_flashforge_profile(const std::string& physical_model, const std::string& profile_model);

enum class PrintDeliveryIssue {
    none,
    unknown_destination_model,
    destination_profile_mismatch,
    plate_profile_mismatch,
    editing_profile_mismatch,
    slice_not_ready,
    slice_identity_unavailable,
    slice_profile_mismatch
};

// Reads the settings footer of the actual G-code selected for delivery.
PrintDeliveryIdentity read_gcode_delivery_identity(const std::string& path);

// A single-nozzle Flashforge send must agree across the saved physical model,
// saved profile, plate, editing context and already-generated G-code.
PrintDeliveryIssue check_flashforge_delivery(const std::string& physical_model,
    const PrintDeliveryIdentity& saved_profile,
    const PrintDeliveryIdentity& plate_setup,
    const PrintDeliveryIdentity& editing_setup,
    const PrintDeliveryIdentity& sliced_gcode,
    bool slice_ready);

} // namespace Slic3r
