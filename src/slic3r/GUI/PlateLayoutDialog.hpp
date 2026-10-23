#pragma once

#include <string>

namespace Slic3r { namespace GUI {
class Plater;
bool can_align_selection(Plater& plater);
bool can_fill_plate(Plater& plater);
void show_alignment_dialog(Plater& plater);
void show_plate_layout_dialog(Plater& plater, bool fill, bool linked_copies = true);
bool can_optimize_plates(Plater& plater, const std::string& stage_id = {});
void show_optimize_plates_dialog(Plater& plater, const std::string& stage_id = {});
}} // namespace Slic3r::GUI
