#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
struct GCodeProcessorResult;
class DynamicPrintConfig;
class Model;

struct ComparedPath {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float speed = 0, width = 0, height = 0;
    uint16_t role = 0, filament = 0;
    uint64_t material_id = 0;
    float flow = 0;
};
struct ComparedLayer {
    double z = 0;
    double filament_mm = 0;
    std::vector<ComparedPath> paths;
    std::optional<double> seconds;
};
struct ComparedSetting { std::string label, value; };
struct ComparedFeature {
    double seconds = 0;
    std::optional<double> grams;
    bool time_available = true;
};
struct SliceSnapshot {
    unsigned int revision = 0;
    std::string label;
    std::optional<double> seconds, grams;
    bool settings_available = false;
    bool paths_available = true;
    std::map<std::string, ComparedSetting> settings;
    std::map<uint16_t, ComparedFeature> features;
    std::map<int64_t, ComparedLayer> layers; // physical Z, rounded to 0.001 mm
    size_t memory_bytes() const;
};
using SliceSnapshotPtr = std::shared_ptr<const SliceSnapshot>;

// Capture a finished parser result in bed coordinates. A null config means the
// saved file's slice settings are unknown; never use today's edited presets.
SliceSnapshot make_slice_snapshot(const GCodeProcessorResult& result, const DynamicPrintConfig* config,
                                 const Model* sliced_model, double origin_x, double origin_y,
                                 const std::string& label, size_t path_limit = 500000);

struct ComparedSettingChange { std::string label, before, after; };
std::vector<ComparedSettingChange> compare_slice_settings(const SliceSnapshot& before, const SliceSnapshot& after);
struct ComparedPaths {
    std::vector<ComparedPath> shared, before_only, after_only, adjusted;
};
// Shared geometry with different speed, material, role or bead dimensions is
// adjusted, not unchanged. Match reversed lines and preserve duplicate moves.
ComparedPaths compare_slice_paths(const ComparedLayer* before, const ComparedLayer* after);
std::vector<int64_t> compared_layer_heights(const SliceSnapshot& before, const SliceSnapshot& after);

// Session-only, bounded history keyed by stable plate identity, never plate index.
class SliceComparisonHistory {
public:
    explicit SliceComparisonHistory(size_t budget = 128 * 1024 * 1024) : m_budget(budget) {}
    void record(size_t plate, SliceSnapshot snapshot);
    SliceSnapshotPtr current(size_t plate) const;
    SliceSnapshotPtr previous(size_t plate) const;
    void clear() { m_entries.clear(); }
private:
    struct Entry { SliceSnapshotPtr current, previous; size_t touched = 0; };
    std::map<size_t, Entry> m_entries;
    size_t m_budget, m_clock = 0;
};
} // namespace Slic3r
