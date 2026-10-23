#pragma once

#include "PrintConfig.hpp"
#include "BoundingBox.hpp"
#include "Point.hpp"
#include <cstddef>
#include <string>
#include <map>
#include <vector>

namespace Slic3r {

class PresetBundle;
enum class ProcessSettingsScope { Project, Stage, Plate, Object };

// Workspace display clearances, in millimetres. A bed's side controls sit to
// its right and its title panel above it; stages also use the title allowance.
// Stage frames keep a margin around their beds and a gap from other stages.
inline constexpr double workspace_controls_clearance = 80.;
inline constexpr double workspace_title_clearance = 100.;
inline constexpr double workspace_stage_margin = 60.;
inline constexpr double workspace_stage_gap = 20.;

// Display clearance is independent of the grid encoded in Orca/Bambu projects.
Vec2d workspace_plate_stride(double width, double depth);
// Orca's grid uses its integer plate size, so fractional bed sizes are truncated.
Vec2d orca_plate_origin(int index, int count, double width, double depth);

// The editable setup belongs to a plate. The application's preset bundle is only
// the editing context for the selected plate, never the owner of all plate jobs.
struct PlatePrintSetup
{
    DynamicPrintConfig config;
    std::string destination;
    std::string destination_name;
    // The printer's process preset is the fallback below project/stage defaults.
    DynamicPrintConfig process_base;
    DynamicPrintConfig process_overrides;
    std::string process_scope_catalog; // Exported on the first plate only.

    bool empty() const { return config.empty(); }
    void capture(const PresetBundle& bundle);
    void activate(PresetBundle& bundle, const std::string& plate_name) const;
    std::string serialize() const;
    static PlatePrintSetup deserialize(const std::string& value);

    template<class Archive> void serialize(Archive& ar) { ar(config, destination, destination_name, process_base, process_overrides, process_scope_catalog); }
};

struct ProcessSettingsScopes
{
    DynamicPrintConfig project;
    std::map<std::string, DynamicPrintConfig> stages;
    template<class Archive> void serialize(Archive& ar) { ar(project, stages); }
    std::string serialize() const;
    static ProcessSettingsScopes deserialize(const std::string& value);
};

void initialize_process_cascade(PlatePrintSetup& setup, const DynamicPrintConfig& preset,
                                const DynamicPrintConfig& plate_options);
DynamicPrintConfig inherited_process_settings(const PlatePrintSetup& setup,
                                             const ProcessSettingsScopes& scopes, const std::string& stage);
DynamicPrintConfig resolved_process_settings(const PlatePrintSetup& setup,
                                            const ProcessSettingsScopes& scopes, const std::string& stage);

// Bulk edits are patches to process settings, never copies of a printer or palette.
const std::vector<std::string>& plate_process_edit_keys();
struct PlateProcessEdit
{
    DynamicPrintConfig config;
    DynamicPrintConfig overrides;
    std::map<std::string, std::string> errors;
    bool changed {false};
};
PlateProcessEdit prepare_plate_process_edit(const DynamicPrintConfig& config,
                                          const DynamicPrintConfig& overrides,
                                          const DynamicPrintConfig& changes);

// A plate's place in the process cascade. `options` are its native plate
// overrides, which apply over its resolved config and stay in step with it.
struct PlateProcessCascade
{
    PlatePrintSetup setup;
    DynamicPrintConfig options;
    std::string stage;
};

// Brings a plate's config to its cascade values. Only values that change are
// written and checked, so a plate that already has its values reports no errors.
PlateProcessEdit resolve_plate_process(const PlateProcessCascade& plate, const ProcessSettingsScopes& scopes);

// Resolves every plate before any of them changes. `failed` is the first plate
// whose new values cannot be applied, or -1 when all of them can.
struct ProcessCascadeEdit
{
    std::vector<PlateProcessEdit> plates;
    int failed {-1};
};
ProcessCascadeEdit resolve_process_cascade(const std::vector<PlateProcessCascade>& plates,
                                           const ProcessSettingsScopes& scopes);

// After a Project (empty `stage`) or Stage edit of `key`: how many of the
// affected plates inherit the new value and how many keep a lower value.
struct ProcessScopeReach
{
    size_t inheriting {0};
    size_t overridden {0};
};
ProcessScopeReach process_scope_reach(const std::vector<PlateProcessCascade>& plates,
                                      const ProcessSettingsScopes& scopes, const std::string& stage,
                                      const std::string& key);

// What a Project (empty `stage`) or Stage editor shows: its first plate's values
// and the baseline below that scope, plus the keys whose value or baseline differs
// between its plates. Values are empty when the scope has no plates.
struct ProcessScopeView
{
    DynamicPrintConfig values;
    DynamicPrintConfig inherited;
    std::vector<std::string> mixed;
    std::vector<std::string> mixed_inherited;
};
ProcessScopeView process_scope_view(const std::vector<PlateProcessCascade>& plates,
                                    const ProcessSettingsScopes& scopes, const std::string& stage);

// The Plate editor changes the plate's live config. Each changed process value
// becomes an explicit plate value, unless it returns to the inherited value.
void record_plate_process_edits(PlatePrintSetup& setup, const DynamicPrintConfig& edited,
                                const ProcessSettingsScopes& scopes, const std::string& stage);
// Another process preset replaces only the plate's fallback. Project, stage and
// explicit plate values keep applying over it.
void select_plate_process_preset(PlatePrintSetup& setup, const DynamicPrintConfig& preset);

// Stages organize the workspace only; print settings and destinations remain on plates.
struct WorkspaceStage
{
    std::string id;
    std::string name;
    Vec2d position {Vec2d::Zero()};
    // Relative to position, so moving a stage carries its manually sized area.
    // An undefined frame keeps the original automatic sizing behavior.
    BoundingBoxf frame;

    template<class Archive> void serialize(Archive& ar) { ar(id, name, position, frame); }
};

std::string serialize_workspace_stages(const std::vector<WorkspaceStage>& stages);
std::vector<WorkspaceStage> deserialize_workspace_stages(const std::string& value);

// A pure preflight shared by native group movement and offline regressions.
struct WorkspacePlateBounds
{
    std::string stage_id;
    Vec2d origin;
    BoundingBoxf bounds;
    bool locked {false};
};
bool can_move_workspace_stage(const std::vector<WorkspacePlateBounds>& plates,
                              const std::string& stage_id, const Vec2d& delta);

BoundingBoxf workspace_stage_minimum_bounds(const WorkspaceStage& stage,
                                            const std::vector<WorkspacePlateBounds>& plates);
BoundingBoxf workspace_stage_bounds(const WorkspaceStage& stage,
                                    const std::vector<WorkspacePlateBounds>& plates);
bool can_resize_workspace_stage(const WorkspaceStage& stage,
                                const std::vector<WorkspacePlateBounds>& plates,
                                const BoundingBoxf& bounds);

// Display areas include the title bar. Keep 20 mm between stages; existing
// overlaps may only change when each required separating translation decreases.
bool can_change_workspace_stage_area(const BoundingBoxf& current, const BoundingBoxf& proposed,
                                     const std::vector<BoundingBoxf>& others);

} // namespace Slic3r
