#pragma once

#include "libslic3r/SliceComparison.hpp"
#include <functional>
#include <string>
#include <vector>

class wxWindow;
namespace Slic3r::GUI {
struct SliceComparisonSource {
    std::string label;
    std::function<SliceSnapshotPtr()> load;
};
void show_slice_comparison(wxWindow* parent, SliceSnapshotPtr current, std::vector<SliceComparisonSource> sources);
}
