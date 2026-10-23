#pragma once

#include "libslic3r/ObjectID.hpp"
#include <vector>
#include <wx/string.h>

class wxWindow;

namespace Slic3r { namespace GUI {

class PartPlateList;

// Capture the active plate's edited setup before calling. Does not activate plates
// or change the Prepare destination; reads only currently valid slice results.
void show_filament_usage_report(wxWindow* parent, PartPlateList& plates,
                                const std::vector<ObjectID>& plate_ids, const wxString& scope_label);

}} // namespace Slic3r::GUI
