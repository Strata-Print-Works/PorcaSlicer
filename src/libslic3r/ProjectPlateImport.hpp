#pragma once

#include "Format/bbs_3mf.hpp"
#include "Point.hpp"
#include <string>
#include <vector>

namespace Slic3r {
class Model;
class DynamicPrintConfig;

// Read only the physical source bed, even when full settings cannot be restored.
Pointfs read_project_bed_shape(const std::string& path);

// Keep filament assignments/painting when choosing the current print settings.
void clear_project_print_settings(Model& model);

// Move only this plate's instances, preserving bed-relative transforms and Z.
void relocate_plate_models(Model& model, PlateData& plate, const Vec2d& source, const Vec2d& target);

// Operates on an isolated incoming model. Resolved setups must already be on
// every plate. Remaps stage identities and translates XY only, preserving the
// source layout beside the existing workspace. No live project is touched.
void prepare_project_plate_import(Model& model, std::vector<PlateData>& plates,
                                  const DynamicPrintConfig& source_config,
                                  const Vec2d& minimum, int first_plate_index);
}
