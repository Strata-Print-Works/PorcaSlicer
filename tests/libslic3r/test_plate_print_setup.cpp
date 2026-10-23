#include "libslic3r/PlateMaterialTransfer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Semver.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/ProjectPlateImport.hpp"
#include "libslic3r/PlateCapacity.hpp"
#include "libslic3r/DiscoveredPrinter.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <boost/filesystem/operations.hpp>
#include <cstdlib>
#include <cmath>
#include "test_utils.hpp"
#include <string>
#include <sstream>
#include <vector>
#include <functional>
#include <exception>
#include <array>
#include <algorithm>
#include <utility>
#include <limits>
#include <miniz.h>
#include <nlohmann/json.hpp>

using namespace Slic3r;

TEST_CASE("Project stage and plate process settings cascade without losing local overrides", "[PlatePrintSetup][ProcessCascade]")
{
    auto preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("wall_loops", new ConfigOptionInt(2));
    PlatePrintSetup setup;
    setup.config = preset;
    setup.config.set_key_value("wall_loops", new ConfigOptionInt(4));
    initialize_process_cascade(setup, preset, {});
    CHECK(setup.process_base.opt_int("wall_loops") == 2);
    CHECK(setup.process_overrides.opt_int("wall_loops") == 4);
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("wall_loops", new ConfigOptionInt(3));
    scopes.stages["stage-a"].set_key_value("wall_loops", new ConfigOptionInt(5));
    CHECK(inherited_process_settings(setup, scopes, "").opt_int("wall_loops") == 3);
    CHECK(inherited_process_settings(setup, scopes, "stage-a").opt_int("wall_loops") == 5);
    CHECK(resolved_process_settings(setup, scopes, "stage-a").opt_int("wall_loops") == 4);
    setup.process_overrides.erase("wall_loops");
    CHECK(resolved_process_settings(setup, scopes, "stage-a").opt_int("wall_loops") == 5);
    CHECK(resolved_process_settings(setup, scopes, "stage-b").opt_int("wall_loops") == 3);
    scopes.stages["stage-a"].erase("wall_loops");
    CHECK(resolved_process_settings(setup, scopes, "stage-a").opt_int("wall_loops") == 3);
    scopes.project.erase("wall_loops");
    CHECK(resolved_process_settings(setup, scopes, "stage-a").opt_int("wall_loops") == 2);
    CHECK(setup.config.opt_int("wall_loops") == 4);
}

TEST_CASE("Cascade migration preserves existing edits and explicit plate values", "[PlatePrintSetup][ProcessCascade]")
{
    const auto preset = DynamicPrintConfig::full_print_config();
    PlatePrintSetup setup;
    setup.config = preset;
    setup.config.set_key_value("wall_loops", new ConfigOptionInt(7));
    DynamicPrintConfig local;
    local.set_key_value("enable_support", new ConfigOptionBool(true));
    initialize_process_cascade(setup, preset, local);
    auto resolved = setup.config;
    resolved.apply(resolved_process_settings(setup, {}, ""));
    auto expected = setup.config;
    expected.apply(local);
    CHECK(resolved == expected);
    CHECK(setup.process_overrides.opt_bool("enable_support"));
    CHECK_FALSE(setup.process_base.has("nozzle_diameter"));
    CHECK_FALSE(setup.process_base.has("filament_colour"));
    CHECK_FALSE(setup.process_base.has("machine_start_gcode"));
    const auto previous = setup.process_base;
    initialize_process_cascade(setup, {}, {});
    CHECK(setup.process_base == previous);
}

TEST_CASE("Cascading settings preserve exact values and reject unsupported metadata", "[PlatePrintSetup][ProcessCascade]")
{
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("layer_height", new ConfigOptionFloat(0.123456789123));
    scopes.stages["stage-a"].set_key_value("wall_loops", new ConfigOptionInt(5));
    const auto restored = ProcessSettingsScopes::deserialize(scopes.serialize());
    CHECK(restored.project == scopes.project);
    CHECK(restored.stages.at("stage-a") == scopes.stages.at("stage-a"));
    PlatePrintSetup setup;
    setup.config = DynamicPrintConfig::full_print_config();
    initialize_process_cascade(setup, setup.config, {});
    setup.process_scope_catalog = scopes.serialize();
    const auto roundtrip = PlatePrintSetup::deserialize(setup.serialize());
    CHECK(roundtrip.process_base == setup.process_base);
    CHECK(roundtrip.process_scope_catalog == setup.process_scope_catalog);
    CHECK(roundtrip.config == setup.config);
    CHECK_THROWS(ProcessSettingsScopes::deserialize(R"({"version":2,"project":{},"stages":{}})"));
    scopes.project.set_key_value("print_host", new ConfigOptionString("private"));
    CHECK_THROWS(scopes.serialize());
}

TEST_CASE("Bulk process edits preserve unedited settings and synchronize explicit plate values", "[PlatePrintSetup][PlateProcessEdit]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("wall_loops", new ConfigOptionInt(2));
    DynamicPrintConfig overrides;
    overrides.set_key_value("wall_loops", new ConfigOptionInt(4));
    overrides.set_key_value("brim_width", new ConfigOptionFloat(7.));
    DynamicPrintConfig changes;
    changes.set_key_value("wall_loops", new ConfigOptionInt(5));
    const auto original = config;
    const auto result = prepare_plate_process_edit(config, overrides, changes);
    REQUIRE(result.errors.empty());
    REQUIRE(result.changed);
    CHECK(result.config.opt_int("wall_loops") == 5);
    CHECK(result.overrides.opt_int("wall_loops") == 5);
    CHECK(result.overrides.opt_float("brim_width") == 7.);
    CHECK(result.config.option("nozzle_diameter")->serialize() == original.option("nozzle_diameter")->serialize());
    CHECK(result.config.option("filament_colour")->serialize() == original.option("filament_colour")->serialize());
    CHECK(result.config.diff(original) == std::vector<std::string>{"wall_loops"});
    CHECK(config == original);
    CHECK(overrides.opt_int("wall_loops") == 4);
    CHECK_FALSE(prepare_plate_process_edit(result.config, result.overrides, changes).changed);
}

TEST_CASE("Bulk process edits reject printer material routing and invalid values", "[PlatePrintSetup][PlateProcessEdit]")
{
    const auto config = DynamicPrintConfig::full_print_config();
    for (const auto& key : {"nozzle_diameter", "filament_colour", "support_filament", "machine_start_gcode", "print_extruder_id"}) {
        DYNAMIC_SECTION(key) {
            DynamicPrintConfig changes;
            if (const auto* value = config.option(key)) changes.set_key_value(key, value->clone());
            else changes.set_key_value(key, new ConfigOptionInt(1));
            CHECK_FALSE(prepare_plate_process_edit(config, {}, changes).errors.empty());
        }
    }
    for (double value : {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        DYNAMIC_SECTION(value) {
            DynamicPrintConfig changes;
            changes.set_key_value("layer_height", new ConfigOptionFloat(value));
            CHECK_FALSE(prepare_plate_process_edit(config, {}, changes).errors.empty());
        }
    }
}

TEST_CASE("Bulk layer height checks use each plate's own nozzle limits", "[PlatePrintSetup][PlateProcessEdit]")
{
    auto fine = DynamicPrintConfig::full_print_config();
    fine.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.2}));
    fine.set_key_value("min_layer_height", new ConfigOptionFloats({0.05}));
    fine.set_key_value("max_layer_height", new ConfigOptionFloats({0.15}));
    // Keep widths valid for the smaller nozzle, independently of this test's height.
    for (const auto* key : {"outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width",
                           "internal_solid_infill_line_width", "bridge_line_width", "top_surface_line_width",
                           "support_line_width", "initial_layer_line_width", "skin_infill_line_width", "skeleton_infill_line_width"})
        fine.set_key_value(key, new ConfigOptionFloatOrPercent(0., false));
    auto coarse = fine;
    coarse.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
    coarse.set_key_value("max_layer_height", new ConfigOptionFloats({0.45}));
    DynamicPrintConfig changes;
    changes.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    CHECK(prepare_plate_process_edit(coarse, {}, changes).errors.empty());
    CHECK(prepare_plate_process_edit(fine, {}, changes).errors.count("layer_height") == 1);
    CHECK(fine.opt_float("layer_height") == 0.2);
}

TEST_CASE("Bulk process edits report dependent settings without correcting them", "[PlatePrintSetup][PlateProcessEdit]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("spiral_mode", new ConfigOptionBool(true));
    config.set_key_value("wall_loops", new ConfigOptionInt(1));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    DynamicPrintConfig changes;
    changes.set_key_value("wall_loops", new ConfigOptionInt(3));
    const auto result = prepare_plate_process_edit(config, {}, changes);
    CHECK(result.errors.count("wall_loops") == 1);
    CHECK(config.opt_int("wall_loops") == 1);
}

TEST_CASE("Bulk process edits report a partial plate configuration instead of reading it", "[PlatePrintSetup][PlateProcessEdit]")
{
    auto config = DynamicPrintConfig::full_print_config();
    const std::string missing = GENERATE("brim_type", "seam_slope_start_height", "raft_layers", "layer_height");
    const bool wrong_type = GENERATE(false, true);
    if (wrong_type) config.set_key_value(missing, new ConfigOptionString("1"));
    else config.erase(missing);
    DynamicPrintConfig changes;
    changes.set_key_value("wall_loops", new ConfigOptionInt(3));
    const auto result = prepare_plate_process_edit(config, {}, changes);
    CHECK(result.errors.count(missing) == 1);
    CHECK_FALSE(result.changed);
}

TEST_CASE("Bulk process edits reject values that native dependency checks would replace", "[PlatePrintSetup][PlateProcessEdit]")
{
    auto config = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig changes;
    SECTION("Support styles must match the chosen support type") {
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("detect_overhang_wall", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        changes.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsTreeSlim));
        CHECK(prepare_plate_process_edit(config, {}, changes).errors.count("support_style") == 1);
    }
    SECTION("Belt-only brims stay unavailable on ordinary beds") {
        config.set_key_value("belt_printer", new ConfigOptionBool(false));
        changes.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btLeadingEdgeOnly));
        CHECK(prepare_plate_process_edit(config, {}, changes).errors.count("brim_type") == 1);
    }
    SECTION("Enumeration values must belong to the setting") {
        changes.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(static_cast<BrimType>(999)));
        CHECK(prepare_plate_process_edit(config, {}, changes).errors.count("brim_type") == 1);
    }
}

namespace {
PlatePrintSetup cascade_setup(const DynamicPrintConfig& preset)
{
    PlatePrintSetup setup;
    setup.config = preset;
    initialize_process_cascade(setup, preset, {});
    return setup;
}
}

TEST_CASE("Switching a plate's process preset keeps its defaults and explicit values", "[PlatePrintSetup][ProcessCascade]")
{
    auto preset_a = DynamicPrintConfig::full_print_config();
    preset_a.set_key_value("wall_loops", new ConfigOptionInt(2));
    preset_a.set_key_value("top_shell_layers", new ConfigOptionInt(4));
    auto preset_b = preset_a;
    preset_b.set_key_value("wall_loops", new ConfigOptionInt(6));
    preset_b.set_key_value("top_shell_layers", new ConfigOptionInt(5));
    preset_b.set_key_value("bottom_shell_layers", new ConfigOptionInt(7));
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("wall_loops", new ConfigOptionInt(3));
    auto setup = cascade_setup(preset_a);
    // This explicit plate value matches the new preset and must stay explicit.
    setup.process_overrides.set_key_value("top_shell_layers", new ConfigOptionInt(5));
    setup.config.apply(resolved_process_settings(setup, scopes, ""));
    REQUIRE(setup.config.opt_int("wall_loops") == 3);
    const auto overrides = setup.process_overrides;

    select_plate_process_preset(setup, preset_b);
    CHECK(setup.process_overrides == overrides);
    CHECK(setup.process_base.opt_int("wall_loops") == 6);
    // The editor loads the raw preset; the cascade restores the plate's defaults.
    PlateProcessCascade plate{setup, {}, ""};
    plate.setup.config = preset_b;
    const auto resolved = resolve_plate_process(plate, scopes);
    REQUIRE(resolved.errors.empty());
    CHECK(resolved.config.opt_int("wall_loops") == 3);
    CHECK(resolved.config.opt_int("top_shell_layers") == 5);
    CHECK(resolved.config.opt_int("bottom_shell_layers") == 7);
    CHECK(resolved.overrides.empty());
    CHECK(resolved_process_settings(setup, scopes, "").opt_int("wall_loops") == 3);
}

TEST_CASE("Plate edits become explicit values unless they return to the inherited value", "[PlatePrintSetup][ProcessCascade]")
{
    auto preset = DynamicPrintConfig::full_print_config();
    preset.set_key_value("wall_loops", new ConfigOptionInt(2));
    ProcessSettingsScopes scopes;
    scopes.stages["stage-a"].set_key_value("wall_loops", new ConfigOptionInt(3));
    auto setup = cascade_setup(preset);
    setup.config.apply(resolved_process_settings(setup, scopes, "stage-a"));

    auto edited = setup.config;
    edited.set_key_value("wall_loops", new ConfigOptionInt(5));
    record_plate_process_edits(setup, edited, scopes, "stage-a");
    CHECK(setup.process_overrides.opt_int("wall_loops") == 5);

    setup.config = edited;
    edited.set_key_value("wall_loops", new ConfigOptionInt(3));
    record_plate_process_edits(setup, edited, scopes, "stage-a");
    CHECK_FALSE(setup.process_overrides.has("wall_loops"));

    // An unchanged value keeps an explicit plate value equal to the inherited one.
    setup.config = edited;
    setup.process_overrides.set_key_value("wall_loops", new ConfigOptionInt(3));
    record_plate_process_edits(setup, edited, scopes, "stage-a");
    CHECK(setup.process_overrides.opt_int("wall_loops") == 3);
}

TEST_CASE("Project and stage editors report mixed values and mixed inherited settings", "[PlatePrintSetup][ProcessCascade]")
{
    auto two = DynamicPrintConfig::full_print_config();
    two.set_key_value("wall_loops", new ConfigOptionInt(2));
    auto four = two;
    four.set_key_value("wall_loops", new ConfigOptionInt(4));
    auto seven = two;
    seven.set_key_value("wall_loops", new ConfigOptionInt(7));
    const std::vector<PlateProcessCascade> plates{
        {cascade_setup(two), {}, "stage-a"}, {cascade_setup(four), {}, "stage-a"}, {cascade_setup(seven), {}, ""}};
    auto contains = [](const std::vector<std::string>& keys, const std::string& key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    ProcessSettingsScopes scopes;

    auto view = process_scope_view(plates, scopes, "");
    CHECK(contains(view.mixed, "wall_loops"));
    CHECK(contains(view.mixed_inherited, "wall_loops"));
    CHECK_FALSE(contains(view.mixed, "top_shell_layers"));

    scopes.project.set_key_value("wall_loops", new ConfigOptionInt(3));
    view = process_scope_view(plates, scopes, "");
    CHECK(view.values.opt_int("wall_loops") == 3);
    CHECK(view.inherited.opt_int("wall_loops") == 2);
    CHECK_FALSE(contains(view.mixed, "wall_loops"));
    CHECK(contains(view.mixed_inherited, "wall_loops"));

    // A stage inherits the project value, and plates outside it are not shown.
    scopes.stages["stage-a"].set_key_value("wall_loops", new ConfigOptionInt(5));
    view = process_scope_view(plates, scopes, "stage-a");
    CHECK(view.values.opt_int("wall_loops") == 5);
    CHECK(view.inherited.opt_int("wall_loops") == 3);
    CHECK_FALSE(contains(view.mixed_inherited, "wall_loops"));
    // The Project editor shows project defaults, not a stage's.
    CHECK(process_scope_view(plates, scopes, "").values.opt_int("wall_loops") == 3);
    CHECK(process_scope_view(plates, scopes, "missing").values.empty());
}

TEST_CASE("Cascade resolution names the first plate that cannot take its values", "[PlatePrintSetup][ProcessCascade]")
{
    auto coarse = DynamicPrintConfig::full_print_config();
    coarse.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
    coarse.set_key_value("min_layer_height", new ConfigOptionFloats({0.05}));
    coarse.set_key_value("max_layer_height", new ConfigOptionFloats({0.45}));
    for (const auto* key : {"outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width",
                           "internal_solid_infill_line_width", "bridge_line_width", "top_surface_line_width",
                           "support_line_width", "initial_layer_line_width", "skin_infill_line_width", "skeleton_infill_line_width"})
        coarse.set_key_value(key, new ConfigOptionFloatOrPercent(0., false));
    auto fine = coarse;
    fine.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.2}));
    fine.set_key_value("max_layer_height", new ConfigOptionFloats({0.15}));
    REQUIRE_THAT(fine.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
    std::vector<PlateProcessCascade> plates{{cascade_setup(coarse), {}, ""}, {cascade_setup(fine), {}, ""}};
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("layer_height", new ConfigOptionFloat(0.3));

    auto resolved = resolve_process_cascade(plates, scopes);
    CHECK(resolved.failed == 1);
    CHECK(resolved.plates.at(1).errors.count("layer_height") == 1);
    CHECK(resolved.plates.at(0).errors.empty());

    // The fine plate keeps its own height and is no longer changed.
    plates[1].setup.process_overrides.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    plates[1].setup.config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    resolved = resolve_process_cascade(plates, scopes);
    REQUIRE(resolved.failed == -1);
    CHECK(resolved.plates.at(0).changed);
    CHECK_FALSE(resolved.plates.at(1).changed);
    CHECK_THAT(resolved.plates.at(0).config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
}

TEST_CASE("Project and stage edits count inheriting plates and lower overrides", "[PlatePrintSetup][ProcessCascade]")
{
    const auto preset = DynamicPrintConfig::full_print_config();
    std::vector<PlateProcessCascade> plates{
        {cascade_setup(preset), {}, "stage-a"}, {cascade_setup(preset), {}, "stage-a"}, {cascade_setup(preset), {}, ""}};
    plates[0].setup.process_overrides.set_key_value("wall_loops", new ConfigOptionInt(4));
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("wall_loops", new ConfigOptionInt(3));

    auto reach = process_scope_reach(plates, scopes, "", "wall_loops");
    CHECK(reach.inheriting == 2);
    CHECK(reach.overridden == 1);

    scopes.stages["stage-a"].set_key_value("wall_loops", new ConfigOptionInt(5));
    reach = process_scope_reach(plates, scopes, "", "wall_loops");
    CHECK(reach.inheriting == 1);
    CHECK(reach.overridden == 2);
    reach = process_scope_reach(plates, scopes, "stage-a", "wall_loops");
    CHECK(reach.inheriting == 1);
    CHECK(reach.overridden == 1);
}

TEST_CASE("New installations default to independent plates and preserve an explicit preference", "[PlatePrintSetup]")
{
    AppConfig config;
    REQUIRE(config.get_bool("independent_plate_setups"));

    const bool independent = GENERATE(false, true);
    config.set("version", "2.3.0");
    config.set_bool("independent_plate_setups", independent);
    config.set_defaults();
    REQUIRE(config.get_bool("independent_plate_setups") == independent);
}

TEST_CASE("Upgraded configurations without the preference keep shared plate setups", "[PlatePrintSetup]")
{
    AppConfig config;
    config.set("version", "2.3.0");
    config.erase("app", "independent_plate_setups");
    config.set_defaults();
    CHECK(config.has("app", "independent_plate_setups"));
    CHECK_FALSE(config.get_bool("independent_plate_setups"));
}

namespace {
PlatePrintSetup setup(double bed, double nozzle, double layer, const std::string& color)
{
    PlatePrintSetup result;
    result.config = DynamicPrintConfig::full_print_config();
    result.config.erase("enable_filament_dynamic_map");
    for (const char* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile", "printhost_user", "printhost_password", "printhost_port"})
        result.config.erase(key);
    result.config.set_key_value("printable_area", new ConfigOptionPoints({Vec2d(0, 0), Vec2d(bed, 0), Vec2d(bed, bed), Vec2d(0, bed)}));
    result.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({nozzle}));
    // Fresh dynamic enum options retain their name tables for the ordinary
    // 3MF settings as well as the exact per-plate extension.
    for (const char* key : {"extruder_type", "nozzle_type", "nozzle_volume_type"})
        result.config.erase(key);
    result.config.set_deserialize_strict("extruder_type", "Direct Drive");
    result.config.set_deserialize_strict("nozzle_type", "undefine");
    result.config.set_deserialize_strict("nozzle_volume_type", "Standard");
    for (const char* key : {"extruder_variant_list", "printer_extruder_variant", "print_extruder_variant"})
        result.config.set_key_value(key, new ConfigOptionStrings({"Direct Drive Standard"}));
    for (const char* key : {"printer_extruder_id", "print_extruder_id"})
        result.config.set_key_value(key, new ConfigOptionInts({1}));
    result.config.set_key_value("filament_nozzle_map", new ConfigOptionInts({0}));
    result.config.set_key_value("layer_height", new ConfigOptionFloat(layer));
    result.config.set_key_value("before_layer_change_gcode", new ConfigOptionString("G92 E0"));
    result.config.set_key_value("filament_colour", new ConfigOptionStrings({color}));
    result.config.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA"}));
    result.config.set_key_value("filament_ids", new ConfigOptionStrings({"GFL99"}));
    result.config.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    result.config.set_key_value("filament_self_index", new ConfigOptionInts({1}));
    result.config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard"}));
    result.destination = color;
    return result;
}

struct Imported {
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    ~Imported() { release_PlateData_list(plates); for (Preset* preset : presets) delete preset; }
};

struct ProjectFixture {
    ScopedTemporaryDir backup;
    Model model;
    PlateData plate;

    ProjectFixture()
    {
        model.set_backup_path(backup.string());
        auto* object = model.add_object();
        object->add_volume(make_cube(20, 20, 10));
        object->add_instance()->set_offset(Vec3d(42, 56, 0));
        plate.plate_index = 0;
        plate.objects_and_instances = {{0, 0}};
        plate.print_setup = setup(180, 0.4, 0.12, "#FF0000");
        plate.has_workspace_position = true;
        plate.workspace_position = Vec2d(20, 30);
    }

    void save(const std::string& path, PlateData* extra = nullptr)
    {
        StoreParams params;
        params.path = path;
        params.model = &model;
        params.config = &plate.print_setup.config;
        params.plate_data_list = {&plate};
        if (extra) params.plate_data_list.push_back(extra);
        REQUIRE(store_bbs_3mf(params));
    }
};

void edit_archive_entry(const std::string& path, const std::string& name, const std::function<void(std::string&)>& edit)
{
    std::vector<std::pair<std::string, std::string>> entries;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path));
    bool edited = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) continue;
        std::string data(stat.m_uncomp_size, '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
        if (std::string(stat.m_filename) == name) {
            edit(data);
            edited = true;
        }
        entries.emplace_back(stat.m_filename, std::move(data));
    }
    close_zip_reader(&zip);
    REQUIRE(edited);
    Zipper out(path);
    for (const auto& entry : entries)
        out.add_entry(entry.first, entry.second.data(), entry.second.size());
    out.finalize();
}

void edit_plate_metadata(const std::string& path, const std::function<void(std::string&)>& edit)
{
    edit_archive_entry(path, "Metadata/model_settings.config", edit);
}
}

TEST_CASE("A saved 3MF retains project defaults and plate inheritance", "[PlatePrintSetup][ProcessCascade][3MF]")
{
    ProjectFixture project;
    auto& saved = project.plate.print_setup;
    initialize_process_cascade(saved, saved.config, {});
    ProcessSettingsScopes scopes;
    scopes.project.set_key_value("wall_loops", new ConfigOptionInt(3));
    saved.process_overrides.set_key_value("wall_loops", new ConfigOptionInt(6));
    saved.process_scope_catalog = scopes.serialize();
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &model, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 1);
    const auto& loaded = imported.plates.front()->print_setup;
    const auto parents = ProcessSettingsScopes::deserialize(loaded.process_scope_catalog);
    CHECK(loaded.process_base == saved.process_base);
    CHECK(loaded.process_overrides == saved.process_overrides);
    CHECK(resolved_process_settings(loaded, parents, "").opt_int("wall_loops") == 6);
    auto inherit = loaded;
    inherit.process_overrides.erase("wall_loops");
    CHECK(resolved_process_settings(inherit, parents, "").opt_int("wall_loops") == 3);
}

TEST_CASE("Switching plate setups restores independent printer and print settings", "[PlatePrintSetup]")
{
    PresetBundle bundle;
    auto first = setup(180, 0.4, 0.12, "#FF0000");
    auto second = setup(350, 0.6, 0.3, "#0000FF");
    for (int repetition = 0; repetition < 3; ++repetition) {
        first.activate(bundle, "Plate 1");
        const auto actual_first = bundle.full_config(false);
        REQUIRE_THAT(actual_first.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
        REQUIRE_THAT(actual_first.opt_float("nozzle_diameter", 0), Catch::Matchers::WithinAbs(0.4, 1e-9));
        REQUIRE(actual_first.option<ConfigOptionPoints>("printable_area")->values == first.config.option<ConfigOptionPoints>("printable_area")->values);
        REQUIRE(actual_first.opt_string("filament_colour", 0) == "#FF0000");

        second.activate(bundle, "Plate 2");
        const auto actual_second = bundle.full_config(false);
        REQUIRE_THAT(actual_second.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
        REQUIRE_THAT(actual_second.opt_float("nozzle_diameter", 0), Catch::Matchers::WithinAbs(0.6, 1e-9));
        REQUIRE(actual_second.option<ConfigOptionPoints>("printable_area")->values == second.config.option<ConfigOptionPoints>("printable_area")->values);
        REQUIRE(actual_second.opt_string("filament_colour", 0) == "#0000FF");
    }
}

TEST_CASE("Plate setups preserve unsaved edits when captured and reactivated", "[PlatePrintSetup]")
{
    PresetBundle bundle;
    auto first = setup(180, 0.4, 0.12, "#FF0000");
    auto second = setup(350, 0.6, 0.3, "#0000FF");
    first.activate(bundle, "Plate 1");
    bundle.prints.get_edited_preset().config.set_key_value("sparse_infill_density", new ConfigOptionPercent(37));
    bundle.project_config.set_key_value("filament_colour", new ConfigOptionStrings({"#00FF00"}));
    first.capture(bundle);
    second.activate(bundle, "Plate 2");
    first.activate(bundle, "Plate 1");
    const auto restored = bundle.full_config(false);
    REQUIRE_THAT(restored.opt_float("sparse_infill_density"), Catch::Matchers::WithinAbs(37, 1e-9));
    REQUIRE(restored.opt_string("filament_colour", 0) == "#00FF00");
    REQUIRE(second.config.option<ConfigOptionStrings>("filament_colour")->values[0] == "#0000FF");
}

TEST_CASE("Saved plate setups round trip exact settings and destination text", "[PlatePrintSetup]")
{
    const auto original = setup(350, 0.6, 0.3, "#0000FF");
    auto modified = original;
    modified.destination = "printer & \"name\"\nsecond line";
    modified.destination_name = "Workshop & \"large\"";
    modified.config.set_key_value("notes", new ConfigOptionString("quotes \" & < >\nline\\path"));
    const auto restored = PlatePrintSetup::deserialize(modified.serialize());
    std::ostringstream differences;
    for (const auto& key : restored.config.diff(modified.config))
        differences << key << ": saved=" << modified.config.option(key)->serialize() << " restored=" << restored.config.option(key)->serialize() << "\n";
    INFO(differences.str());
    REQUIRE(restored.config.diff(modified.config).empty());
    REQUIRE(restored.destination == modified.destination);
    REQUIRE(restored.destination_name == modified.destination_name);
}

TEST_CASE("Previously saved plate setups open without a printer display name", "[PlatePrintSetup]")
{
    const auto original = setup(180, 0.4, 0.12, "#FF0000");
    auto data = nlohmann::json::parse(original.serialize());
    data.erase("destination_name");
    const auto restored = PlatePrintSetup::deserialize(data.dump());
    REQUIRE(restored.destination == original.destination);
    REQUIRE(restored.destination_name.empty());
    REQUIRE(restored.config.diff(original.config).empty());
}

TEST_CASE("Saving directly supplied plate settings excludes local connections", "[PlatePrintSetup]")
{
    auto direct = setup(180, 0.4, 0.12, "#FF0000");
    for (const char* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
                            "printhost_user", "printhost_password", "printhost_port"})
        direct.config.set_key_value(key, new ConfigOptionString("private-test-value"));
    direct.config.set_key_value("enable_filament_dynamic_map", new ConfigOptionBool(true));
    const auto serialized = direct.serialize();
    REQUIRE(serialized.find("private-test-value") == std::string::npos);
    const auto data = nlohmann::json::parse(serialized);
    for (const char* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
                            "printhost_user", "printhost_password", "printhost_port", "enable_filament_dynamic_map"}) {
        REQUIRE_FALSE(data.at("config").contains(key));
        REQUIRE_FALSE(data.at("exact").contains(key));
    }
    REQUIRE(direct.config.opt_string("printhost_apikey") == "private-test-value");
}

TEST_CASE("Opening plate settings never restores embedded connection details", "[PlatePrintSetup]")
{
    const auto original = setup(180, 0.4, 0.12, "#FF0000");
    auto data = nlohmann::json::parse(original.serialize());
    for (const char* key : {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
                            "printhost_user", "printhost_password", "printhost_port"})
        data["config"][key] = "private-test-value";
    data["config"]["enable_filament_dynamic_map"] = "1";
    const auto restored = PlatePrintSetup::deserialize(data.dump());
    REQUIRE(restored.config.diff(original.config).empty());
    REQUIRE(restored.serialize().find("private-test-value") == std::string::npos);
}

TEST_CASE("Embedded printer profiles do not leak connection credentials into a 3MF", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    ScopedTemporaryFile file(".3mf");
    Preset printer(Preset::TYPE_PRINTER, "Offline test printer");
    printer.config = project.plate.print_setup.config;
    printer.config.set_key_value("printhost_apikey", new ConfigOptionString("private-profile-key"));
    printer.config.set_key_value("printhost_password", new ConfigOptionString("private-profile-password"));
    StoreParams params;
    params.path = file.string();
    params.model = &project.model;
    params.config = &project.plate.print_setup.config;
    params.plate_data_list = {&project.plate};
    params.project_presets = {&printer};
    REQUIRE(store_bbs_3mf(params));

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, file.string()));
    bool embedded = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) continue;
        std::string data(stat.m_uncomp_size, '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
        REQUIRE(data.find("private-profile-key") == std::string::npos);
        REQUIRE(data.find("private-profile-password") == std::string::npos);
        if (std::string(stat.m_filename) == "Metadata/machine_settings_1.config") embedded = true;
    }
    close_zip_reader(&zip);
    REQUIRE(embedded);
    REQUIRE(printer.config.opt_string("printhost_apikey") == "private-profile-key");
    REQUIRE(printer.config.opt_string("printhost_password") == "private-profile-password");
}

TEST_CASE("Unsupported saved plate setups fail without substituting settings", "[PlatePrintSetup]")
{
    REQUIRE_THROWS(PlatePrintSetup::deserialize(R"({"version":2,"config":{}})"));
    REQUIRE_THROWS(PlatePrintSetup::deserialize(R"({"version":1,"config":{"unknown_setting":"1"}})"));
    REQUIRE_THROWS(PlatePrintSetup::deserialize(R"({"version":1,"config":{}})"));
    REQUIRE_THROWS(PlatePrintSetup::deserialize(R"({"version":1,"config":{"layer_height":"0.2"}})"));
}

TEST_CASE("Plate setups from a newer format version are rejected", "[PlatePrintSetup]")
{
    auto original = setup(180, 0.4, 0.12, "#FF0000");
    initialize_process_cascade(original, original.config, {});
    auto data = nlohmann::json::parse(original.serialize());
    REQUIRE(data.at("version") == 2);
    REQUIRE_NOTHROW(PlatePrintSetup::deserialize(data.dump()));
    data["version"] = 3;
    CHECK_THROWS_AS(PlatePrintSetup::deserialize(data.dump()), RuntimeError);
}

TEST_CASE("Plate overrides without a process fallback survive a save", "[PlatePrintSetup][ProcessCascade]")
{
    auto original = setup(180, 0.4, 0.12, "#FF0000");
    original.process_overrides.set_key_value("wall_loops", new ConfigOptionInt(5));
    REQUIRE(original.process_base.empty());
    REQUIRE(original.process_scope_catalog.empty());
    const auto serialized = original.serialize();
    // Version 1 readers ignore process fields, so the overrides need version 2.
    CHECK(nlohmann::json::parse(serialized).at("version") == 2);
    const auto restored = PlatePrintSetup::deserialize(serialized);
    CHECK(restored.process_overrides == original.process_overrides);
    CHECK(restored.process_base.empty());
}

TEST_CASE("Malformed saved process settings report a restoration error", "[PlatePrintSetup][ProcessCascade]")
{
    auto original = setup(180, 0.4, 0.12, "#FF0000");
    initialize_process_cascade(original, original.config, {});
    original.process_overrides.set_key_value("wall_loops", new ConfigOptionInt(4));
    const auto valid = nlohmann::json::parse(original.serialize());
    REQUIRE_NOTHROW(PlatePrintSetup::deserialize(valid.dump()));
    std::vector<nlohmann::json> malformed;
    malformed.push_back(4);
    malformed.push_back("4");
    malformed.push_back(nlohmann::json::object());
    malformed.push_back(nlohmann::json::object({{"value", 4}}));
    malformed.push_back(nlohmann::json::object({{"value", "4"}, {"number", "4"}}));
    for (size_t i = 0; i < malformed.size(); ++i) {
        DYNAMIC_SECTION("entry " << i << ": " << malformed[i].dump()) {
            auto data = valid;
            data["process_overrides"]["wall_loops"] = malformed[i];
            CHECK_THROWS_AS(PlatePrintSetup::deserialize(data.dump()), RuntimeError);
        }
    }
}

TEST_CASE("Saved numeric arrays must match their setting's length", "[PlatePrintSetup]")
{
    const auto original = setup(180, 0.4, 0.12, "#FF0000");
    const auto valid = nlohmann::json::parse(original.serialize());
    REQUIRE_NOTHROW(PlatePrintSetup::deserialize(valid.dump()));
    // One value per extruder, per filament, and a per-filament percentage.
    const std::string key = GENERATE("nozzle_diameter", "filament_diameter", "filament_shrink");
    REQUIRE(valid.at("exact").at(key).is_array());
    REQUIRE(valid.at("exact").at(key).size() == 1);
    auto longer = valid;
    longer["exact"][key].push_back(0.4);
    CHECK_THROWS_AS(PlatePrintSetup::deserialize(longer.dump()), RuntimeError);
    auto scalar = valid;
    scalar["exact"][key] = 0.4;
    CHECK_THROWS_AS(PlatePrintSetup::deserialize(scalar.dump()), RuntimeError);
}

TEST_CASE("Incomplete or invalid bed settings are rejected before project restoration", "[PlatePrintSetup]")
{
    const auto original = setup(180, 0.4, 0.12, "#FF0000");
    for (const char* key : {"printable_area", "printable_height", "nozzle_diameter"}) {
        auto data = nlohmann::json::parse(original.serialize());
        data["config"].erase(key);
        REQUIRE_THROWS(PlatePrintSetup::deserialize(data.dump()));
    }
    for (const auto& area : {nlohmann::json::array(), nlohmann::json{{0, 0}, {0, 10}},
                            nlohmann::json{{0, 0}, {0, 10}, {0, 20}}}) {
        auto data = nlohmann::json::parse(original.serialize());
        data["exact"]["printable_area"] = area;
        REQUIRE_THROWS(PlatePrintSetup::deserialize(data.dump()));
    }
    auto data = nlohmann::json::parse(original.serialize());
    data["exact"]["nozzle_diameter"] = nlohmann::json::array();
    REQUIRE_THROWS(PlatePrintSetup::deserialize(data.dump()));
    data = nlohmann::json::parse(original.serialize());
    data["exact"]["printable_height"] = -1;
    REQUIRE_THROWS(PlatePrintSetup::deserialize(data.dump()));
    data = nlohmann::json::parse(original.serialize());
    data["config"]["printer_technology"] = "SLA";
    REQUIRE_THROWS(PlatePrintSetup::deserialize(data.dump()));
}

TEST_CASE("Portable project destinations never publish legacy connection addresses", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    AppConfig local;
    project.plate.print_setup.destination = "scanned:moonraker:192.168.1.60:7125";
    project.plate.print_setup.destination_name = "Workshop";
    REQUIRE_THROWS(project.plate.print_setup.serialize());
    project.plate.print_setup.destination = "192.168.1.60:7125";
    REQUIRE_THROWS(project.plate.print_setup.serialize());
    project.plate.print_setup.destination = "printer.local:7125";
    REQUIRE_THROWS(project.plate.print_setup.serialize());
    project.plate.print_setup.destination = "192.168.1.60:7125";
    project.plate.print_setup.destination = portable_printer_destination(project.plate.print_setup.destination, local);
    const auto reopened = PlatePrintSetup::deserialize(project.plate.print_setup.serialize());
    REQUIRE(reopened.destination == project.plate.print_setup.destination);
    REQUIRE(reopened.destination_name == "Workshop");
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, file.string()));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) continue;
        std::string data(stat.m_uncomp_size, '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
        REQUIRE(data.find("192.168.1.60") == std::string::npos);
    }
    close_zip_reader(&zip);
}

TEST_CASE("Plate setup files omit printer connection credentials", "[PlatePrintSetup]")
{
    PresetBundle bundle;
    auto initial = setup(180, 0.4, 0.12, "#FF0000");
    initial.activate(bundle, "Plate 1");
    bundle.printers.get_edited_preset().config.set_key_value("printhost_apikey", new ConfigOptionString("test-only-key"));
    PlatePrintSetup saved;
    saved.capture(bundle);
    REQUIRE_FALSE(saved.config.has("printhost_apikey"));
    REQUIRE(saved.serialize().find("test-only-key") == std::string::npos);
}

TEST_CASE("A two-plate 3MF retains each printing setup and original geometry", "[PlatePrintSetup][3MF]")
{
    ScopedTemporaryDir backup;
    ScopedTemporaryFile file(".3mf");
    Model original;
    original.set_backup_path(backup.string());
    auto* object = original.add_object();
    auto* volume = object->add_volume(make_cube(20, 20, 10));
    TriangleSelector paint(volume->mesh());
    paint.set_facet(0, EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set_data(paint.serialize());
    object->add_instance()->set_offset(Vec3d(42, 56, 0));
    auto* large_object = original.add_object();
    large_object->add_volume(make_cube(30, 30, 15));
    large_object->add_instance()->set_offset(Vec3d(475, 56, 0));

    PlateData first;
    first.plate_index = 0;
    first.plate_name = "Small printer";
    first.locked = true;
    first.has_workspace_position = true;
    first.workspace_position = Vec2d(-37.25, 18.5);
    first.objects_and_instances = {{0, 0}};
    first.print_setup = setup(180, 0.4, 0.12, "#FF0000");
    first.print_setup.config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    first.print_setup.config.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA", "PLA"}));
    first.print_setup.config.set_key_value("filament_ids", new ConfigOptionStrings({"GFL99", "GFL99"}));
    first.print_setup.config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA"}));
    first.print_setup.config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2}));
    first.print_setup.config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    first.print_setup.config.set_key_value("filament_map", new ConfigOptionInts({1, 1}));
    first.print_setup.config.set_key_value("filament_volume_map", new ConfigOptionInts({0, 0}));
    first.print_setup.config.set_key_value("filament_nozzle_map", new ConfigOptionInts({0, 0}));
    PlateData second;
    second.plate_index = 1;
    second.has_workspace_position = true;
    second.workspace_position = Vec2d(420.125, -37.875);
    second.objects_and_instances = {{1, 0}};
    second.plate_name = "Large & fine\n\"second\" &lt;";
    second.print_setup = setup(350, 0.6, 0.3, "#0000FF");
    second.print_setup.destination = "printer & \"second\"";
    second.print_setup.destination_name = "Workshop & \"second\"";
    StoreParams params;
    params.path = file.string();
    params.model = &original;
    params.config = &first.print_setup.config;
    params.plate_data_list = {&first, &second};
    REQUIRE(store_bbs_3mf(params));
    if (const char* output = std::getenv("ORCA_PLATE_TEST_ARTIFACT_DIR")) {
        boost::filesystem::create_directories(output);
        boost::filesystem::copy_file(file.path(), boost::filesystem::path(output) / "independent-plates.3mf", boost::filesystem::copy_options::overwrite_existing);
    }

    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 2);
    std::ostringstream differences;
    for (const auto& key : imported.plates[0]->print_setup.config.diff(first.print_setup.config))
        differences << key << ": saved=" << first.print_setup.config.option(key)->serialize() << " restored=" << imported.plates[0]->print_setup.config.option(key)->serialize() << "\n";
    INFO(differences.str());
    REQUIRE(imported.plates[0]->print_setup.config.diff(first.print_setup.config).empty());
    REQUIRE(imported.plates[1]->print_setup.config.diff(second.print_setup.config).empty());
    REQUIRE(imported.plates[1]->print_setup.destination == second.print_setup.destination);
    REQUIRE(imported.plates[1]->print_setup.destination_name == second.print_setup.destination_name);
    REQUIRE(imported.plates[0]->plate_name == first.plate_name);
    REQUIRE(imported.plates[1]->plate_name == second.plate_name);
    REQUIRE(imported.plates[0]->locked);
    REQUIRE_FALSE(imported.plates[1]->locked);
    REQUIRE(imported.plates[1]->has_workspace_position);
    REQUIRE_THAT((imported.plates[1]->workspace_position - second.workspace_position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE(imported.plates[0]->has_workspace_position);
    REQUIRE_THAT((imported.plates[0]->workspace_position - first.workspace_position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE(loaded.objects.size() == 2);
    REQUIRE(TriangleSelector::has_facets(loaded.objects[0]->volumes[0]->mmu_segmentation_facets.get_data(), EnforcerBlockerType::Extruder2));
    // The reader may recenter mesh vertices and compensate in the instance;
    // compare the physical placement rather than a particular representation.
    for (size_t i = 0; i < original.objects.size(); ++i) {
        const auto before = original.objects[i]->instance_bounding_box(0);
        const auto after = loaded.objects[i]->instance_bounding_box(0);
        REQUIRE_THAT((after.min - before.min).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
        REQUIRE_THAT((after.max - before.max).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
    }

    // The core reader does not know our plate metadata, but still opens the meshes.
    Model generic;
    DynamicPrintConfig generic_config;
    ConfigSubstitutionContext generic_substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(load_3mf(file.string().c_str(), generic_config, generic_substitutions, &generic, false));
    REQUIRE(generic.objects.size() == original.objects.size());
    for (size_t i = 0; i < original.objects.size(); ++i) {
        const auto before = original.objects[i]->instance_bounding_box(0);
        const auto after = generic.objects[i]->instance_bounding_box(0);
        REQUIRE_THAT((after.min - before.min).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
        REQUIRE_THAT((after.max - before.max).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
    }

    // Saving an opened project retains its printer intent without consulting saved devices.
    ScopedTemporaryFile saved_again(".3mf");
    params.path = saved_again.string();
    params.model = &loaded;
    params.config = &config;
    params.plate_data_list = imported.plates;
    REQUIRE(store_bbs_3mf(params));
    ScopedTemporaryDir reopened_backup;
    Model reopened;
    reopened.set_backup_path(reopened_backup.string());
    Imported reopened_plates;
    REQUIRE(load_bbs_3mf(saved_again.string().c_str(), &config, &substitutions, &reopened, &reopened_plates.plates,
        &reopened_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(reopened_plates.plates.size() == imported.plates.size());
    for (size_t i = 0; i < imported.plates.size(); ++i) {
        REQUIRE(reopened_plates.plates[i]->print_setup.serialize() == imported.plates[i]->print_setup.serialize());
        REQUIRE(reopened_plates.plates[i]->plate_name == imported.plates[i]->plate_name);
        REQUIRE(reopened_plates.plates[i]->locked == imported.plates[i]->locked);
        REQUIRE(reopened_plates.plates[i]->has_workspace_position == imported.plates[i]->has_workspace_position);
        REQUIRE_THAT((reopened_plates.plates[i]->workspace_position - imported.plates[i]->workspace_position).norm(),
                     Catch::Matchers::WithinAbs(0, 1e-9));
    }
}

TEST_CASE("Geometry can be imported when a project's plate settings are unsupported", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_plate_metadata(file.string(), [](std::string& xml) {
        for (const char* key : {"orca_plate_print_setup", "orca_plate_workspace_position"}) {
            const auto key_start = xml.find(std::string("key=\"") + key + "\"");
            REQUIRE(key_start != std::string::npos);
            const auto value_start = xml.find("value=\"", key_start) + 7;
            const auto value_end = xml.find('"', value_start);
            xml.replace(value_start, value_end - value_start, "unsupported-future-value");
        }
    });
    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(loaded.objects.size() == 1);
    const auto before = project.model.objects[0]->instance_bounding_box(0);
    const auto after = loaded.objects[0]->instance_bounding_box(0);
    REQUIRE_THAT((after.min - before.min).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
    REQUIRE_THAT((after.max - before.max).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
    for (const auto* loaded_plate : imported.plates) {
        REQUIRE(loaded_plate->print_setup.empty());
        REQUIRE_FALSE(loaded_plate->has_workspace_position);
    }
}

TEST_CASE("Incomplete plate setups reject project restoration and allow geometry recovery", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_plate_metadata(file.string(), [](std::string& xml) {
        const auto key_start = xml.find("key=\"orca_plate_print_setup\"");
        REQUIRE(key_start != std::string::npos);
        const auto value_start = xml.find("value=\"", key_start) + 7;
        const auto value_end = xml.find('"', value_start);
        xml.replace(value_start, value_end - value_start,
                    "{&quot;version&quot;:1,&quot;config&quot;:{&quot;layer_height&quot;:&quot;0.2&quot;}}");
    });
    ScopedTemporaryDir backup;
    Model rejected;
    rejected.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported rejected_plates;
    bool bambu = false, orca = false;
    Semver version;
    bool restored = false;
    try {
        restored = load_bbs_3mf(file.string().c_str(), &config, &substitutions, &rejected, &rejected_plates.plates,
            &rejected_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    } catch (const RuntimeError&) {}
    REQUIRE_FALSE(restored);
    Model recovered;
    recovered.set_backup_path(backup.string());
    Imported recovered_plates;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &recovered, &recovered_plates.plates,
        &recovered_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(recovered.objects.size() == 1);
    REQUIRE_THAT((recovered.objects[0]->instance_bounding_box(0).min - project.model.objects[0]->instance_bounding_box(0).min).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-6));
    REQUIRE_THAT((recovered.objects[0]->instance_bounding_box(0).max - project.model.objects[0]->instance_bounding_box(0).max).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-6));
}

TEST_CASE("Saved plate setups are required even without the project's application tag", "[PlatePrintSetup][3MF]")
{
    // intact: the control; setup: an unreadable setup; document: a readable setup
    // followed by a damaged plate document.
    const std::string damage = GENERATE("intact", "setup", "document");
    ProjectFixture project;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_archive_entry(file.string(), "3D/3dmodel.model", [](std::string& xml) {
        for (const std::string name : {"Application", "OrcaSlicer"}) {
            const auto start = xml.find("<metadata name=\"" + name + "\">");
            REQUIRE(start != std::string::npos);
            const std::string close = "</metadata>";
            const auto end = xml.find(close, start);
            REQUIRE(end != std::string::npos);
            xml.erase(start, end + close.size() - start);
        }
    });
    edit_plate_metadata(file.string(), [&](std::string& xml) {
        if (damage == "setup") {
            const auto key_start = xml.find("key=\"orca_plate_print_setup\"");
            REQUIRE(key_start != std::string::npos);
            const auto value_start = xml.find("value=\"", key_start) + 7;
            const auto value_end = xml.find('"', value_start);
            xml.replace(value_start, value_end - value_start,
                        "{&quot;version&quot;:1,&quot;config&quot;:{&quot;layer_height&quot;:&quot;0.2&quot;}}");
        } else if (damage == "document") {
            const std::string close = "</config>";
            const auto end = xml.rfind(close);
            REQUIRE(end != std::string::npos);
            xml.replace(end, close.size(), "</damaged>");
        }
    });
    ScopedTemporaryDir backup;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    bool bambu = false, orca = false;
    Semver version;
    Model full;
    full.set_backup_path(backup.string());
    Imported full_plates;
    bool restored = false;
    try {
        restored = load_bbs_3mf(file.string().c_str(), &config, &substitutions, &full, &full_plates.plates,
            &full_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    } catch (const std::exception&) {}
    CHECK_FALSE(bambu);
    if (damage == "intact") {
        REQUIRE(restored);
        REQUIRE(full_plates.plates.size() == 1);
        CHECK_FALSE(full_plates.plates.front()->print_setup.empty());
    } else
        CHECK_FALSE(restored);

    Model recovered;
    recovered.set_backup_path(backup.string());
    Imported recovered_plates;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &recovered, &recovered_plates.plates,
        &recovered_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(recovered.objects.size() == 1);
    REQUIRE_THAT((recovered.objects[0]->instance_bounding_box(0).min - project.model.objects[0]->instance_bounding_box(0).min).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-6));
}

TEST_CASE("Projects without recognized plate metadata retain ordinary Orca settings", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_plate_metadata(file.string(), [](std::string& xml) {
        for (const char* key : {"orca_plate_print_setup", "orca_plate_workspace_position"}) {
            const auto offset = xml.find(key);
            REQUIRE(offset != std::string::npos);
            // Simulate a reader that does not recognize either optional metadata key.
            xml.replace(offset, std::string(key).size(), std::string("unknown_") + key);
        }
    });
    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 1);
    REQUIRE(imported.plates[0]->print_setup.empty());
    REQUIRE_FALSE(imported.plates[0]->has_workspace_position);
    REQUIRE_THAT(config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    REQUIRE(loaded.objects.size() == 1);
}

TEST_CASE("Roomy plate layouts save conventional Orca coordinates without moving the live model", "[PlatePrintSetup][3MF]")
{
    const bool independent = GENERATE(false, true);
    const auto strategy = GENERATE(SaveStrategy::Zip64, SaveStrategy::SplitModel);
    ScopedTemporaryDir backup;
    ScopedTemporaryFile file(".3mf");
    Model model;
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    object->add_volume(make_cube(12, 18, 9));
    auto config = setup(320, .4, .2, "#FF0000").config;
    constexpr int count = 5;
    std::array<PlateData, count> plates;
    const Vec2d stride = workspace_plate_stride(320, 320);
    std::vector<BoundingBoxf3> original;
    std::vector<Vec2d> owners;
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &config;
    params.strategy = strategy;
    for (int i = 0; i < count; ++i) {
        auto& plate = plates[i];
        plate.plate_index = i;
        plate.plate_name = "Plate " + std::to_string(i + 1);
        plate.locked = i == 2;
        plate.has_model_origin = true;
        plate.model_origin = independent ? Vec2d(-420.25 + i * 510., 97.125 - i * 430.) :
            Vec2d((i % 3) * stride.x(), -(i / 3) * stride.y());
        plate.has_workspace_position = independent;
        plate.workspace_position = plate.model_origin;
        if (independent) {
            plate.print_setup.config = config;
            plate.stage_id = "stage";
            if (i == 0) {
                plate.has_workspace_stages = true;
                plate.workspace_stages = {{"stage", "Test stage", Vec2d(-500., 200.), {}}};
            }
        }
        params.plate_data_list.push_back(&plate);
        if (i == 3) continue; // Retain empty plates in the grid and metadata.
        const int copies = i == 2 ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            const int instance = int(object->instances.size());
            auto* copy = object->add_instance();
            copy->set_offset(Vec3d(plate.model_origin.x() + 45.25 + c * 40., plate.model_origin.y() + 61.75, -1.5));
            copy->set_rotation(Vec3d(0., 0., .27 * i));
            plate.objects_and_instances.emplace_back(0, instance);
            original.push_back(object->instance_bounding_box(instance));
            owners.push_back(plate.model_origin);
        }
    }
    REQUIRE(store_bbs_3mf(params));
    if (const char* output = std::getenv("ORCA_PLATE_TEST_ARTIFACT_DIR")) {
        boost::filesystem::create_directories(output);
        const std::string name = independent ? "roomy-independent.3mf" : "roomy-shared.3mf";
        boost::filesystem::copy_file(file.path(), boost::filesystem::path(output) / name, boost::filesystem::copy_options::overwrite_existing);
    }
    for (size_t i = 0; i < original.size(); ++i)
        REQUIRE_THAT((object->instance_bounding_box(i).min - original[i].min).norm(), Catch::Matchers::WithinAbs(0., 1e-9));

    ScopedTemporaryDir restored_backup;
    Model restored;
    restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    const auto load = [&](const std::string& path, Model& destination, Imported& data) {
        return load_bbs_3mf(path.c_str(), &restored_config, &substitutions, &destination, &data.plates,
            &data.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    };
    REQUIRE(load(file.string(), restored, imported));
    REQUIRE(imported.plates.size() == count);
    REQUIRE(restored.objects.size() == 1);
    REQUIRE(imported.plates[2]->objects_and_instances.size() == 2);
    REQUIRE(imported.plates[3]->objects_and_instances.empty());
    const auto check_placement = [&](const Model& current, const PlateDataPtrs& data, bool conventional) {
        for (int p = 0; p < count; ++p) {
            REQUIRE(data[p]->plate_name == plates[p].plate_name);
            REQUIRE(data[p]->locked == plates[p].locked);
            for (const auto& [obj, inst] : data[p]->objects_and_instances) {
                const Vec2d origin = conventional ? orca_plate_origin(p, count, 320, 320) : plates[p].model_origin;
                const Vec3d expected_delta(origin.x() - owners[inst].x(), origin.y() - owners[inst].y(), 0.);
                const auto box = current.objects[obj]->instance_bounding_box(inst);
                REQUIRE_THAT((box.min - original[inst].min - expected_delta).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
                REQUIRE_THAT((box.max - original[inst].max - expected_delta).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
            }
        }
    };
    check_placement(restored, imported.plates, true);
    for (int p = 0; p < count; ++p) {
        REQUIRE(imported.plates[p]->has_model_origin);
        REQUIRE(imported.plates[p]->has_workspace_position == independent);
        if (independent)
            REQUIRE_THAT((imported.plates[p]->workspace_position - plates[p].workspace_position).norm(), Catch::Matchers::WithinAbs(0., 1e-9));
        relocate_plate_models(restored, *imported.plates[p], imported.plates[p]->model_origin, plates[p].model_origin);
    }
    check_placement(restored, imported.plates, false);

    // Re-export the reopened workspace: normalization must not accumulate drift.
    ScopedTemporaryFile second_save(".3mf");
    params.path = second_save.string();
    params.model = &restored;
    params.config = &restored_config;
    params.plate_data_list = imported.plates;
    REQUIRE(store_bbs_3mf(params));
    check_placement(restored, imported.plates, false);
    ScopedTemporaryDir twice_backup;
    Model twice;
    twice.set_backup_path(twice_backup.string());
    Imported twice_imported;
    REQUIRE(load(second_save.string(), twice, twice_imported));
    check_placement(twice, twice_imported.plates, true);

    // Exercise the inherited reader with every Porca layout/setup key unknown,
    // then save that ordinary Orca project and reopen it again.
    edit_plate_metadata(second_save.string(), [](std::string& xml) {
        for (const std::string key : {"porca_plate_layout", "orca_plate_print_setup", "porca_workspace_stages", "porca_plate_stage"}) {
            size_t position = 0;
            while ((position = xml.find(key, position)) != std::string::npos) {
                xml.insert(position, "ignored_");
                position += 8 + key.size();
            }
        }
    });
    ScopedTemporaryDir orca_backup;
    Model conventional;
    conventional.set_backup_path(orca_backup.string());
    Imported conventional_plates;
    REQUIRE(load(second_save.string(), conventional, conventional_plates));
    for (const auto* plate : conventional_plates.plates) {
        REQUIRE_FALSE(plate->has_model_origin);
        REQUIRE_FALSE(plate->has_workspace_position);
        REQUIRE(plate->print_setup.empty());
    }
    check_placement(conventional, conventional_plates.plates, true);
    params.model = &conventional;
    params.plate_data_list = conventional_plates.plates;
    params.path = file.string();
    REQUIRE(store_bbs_3mf(params));
    ScopedTemporaryDir return_backup;
    Model returned;
    returned.set_backup_path(return_backup.string());
    Imported returned_plates;
    REQUIRE(load(file.string(), returned, returned_plates));
    check_placement(returned, returned_plates.plates, true);
    for (int p = 0; p < count; ++p)
        relocate_plate_models(returned, *returned_plates.plates[p], orca_plate_origin(p, count, 320, 320), plates[p].model_origin);
    check_placement(returned, returned_plates.plates, false);
}

TEST_CASE("Models outside every plate keep their place through a conventional-grid save", "[PlatePrintSetup][3MF]")
{
    // A shared-setup Orca project laid out on Porca's roomy desktop grid, with a
    // model left on the unprintable area after the last bed.
    constexpr int count = 5;
    constexpr double bed = 320.;
    const Vec2d stride = workspace_plate_stride(bed, bed);
    const auto roomy = [&](int index) { return Vec2d((index % 3) * stride.x(), -(index / 3) * stride.y()); };
    ScopedTemporaryDir backup;
    ScopedTemporaryFile file(".3mf");
    Model model;
    model.set_backup_path(backup.string());
    auto config = setup(bed, .4, .2, "#FF0000").config;
    std::array<PlateData, count> plates;
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &config;
    for (int i = 0; i < count; ++i) {
        auto* object = model.add_object();
        object->add_volume(make_cube(12, 18, 9));
        object->add_instance()->set_offset(Vec3d(roomy(i).x() + 40., roomy(i).y() + 60., 0.));
        auto& plate = plates[i];
        plate.plate_index = i;
        plate.has_model_origin = true;
        plate.model_origin = roomy(i);
        plate.objects_and_instances = {{i, 0}};
        params.plate_data_list.push_back(&plate);
    }
    auto* unassigned = model.add_object();
    unassigned->add_volume(make_cube(10, 10, 10));
    unassigned->add_instance()->set_offset(Vec3d(roomy(count).x() + 75., roomy(count).y() + 90., 0.));
    std::vector<BoundingBoxf3> original;
    for (const auto* object : model.objects) original.push_back(object->instance_bounding_box(0));
    REQUIRE(store_bbs_3mf(params));

    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &loaded_config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == count);
    REQUIRE(loaded.objects.size() == original.size());
    const auto box = [&](size_t object) { return loaded.objects[object]->instance_bounding_box(0); };

    // Plate members are written on Orca's conventional grid...
    for (int p = 0; p < count; ++p) {
        REQUIRE(imported.plates[p]->has_model_origin);
        REQUIRE((imported.plates[p]->objects_and_instances == std::vector<std::pair<int, int>>{{p, 0}}));
        const Vec2d delta = orca_plate_origin(p, count, bed, bed) - roomy(p);
        CHECK_THAT((box(p).min - original[p].min - Vec3d(delta.x(), delta.y(), 0.)).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    }
    // ...while the unassigned model keeps its workspace coordinates, clear of every
    // conventional bed, so another slicer does not adopt it into a plate.
    const auto saved = box(count);
    CHECK_THAT((saved.min - original[count].min).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    CHECK_THAT((saved.max - original[count].max).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    for (int p = 0; p < count; ++p) {
        const Vec2d origin = orca_plate_origin(p, count, bed, bed);
        const bool overlaps = saved.min.x() < origin.x() + bed && origin.x() < saved.max.x() &&
                              saved.min.y() < origin.y() + bed && origin.y() < saved.max.y();
        CHECK_FALSE(overlaps);
    }
    // Reopening restores members to the roomy grid; the unassigned model keeps its
    // position relative to the unprintable area after the last bed.
    for (int p = 0; p < count; ++p)
        relocate_plate_models(loaded, *imported.plates[p], imported.plates[p]->model_origin, roomy(p));
    for (size_t i = 0; i < original.size(); ++i) {
        CHECK_THAT((box(i).min - original[i].min).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
        CHECK_THAT((box(i).max - original[i].max).norm(), Catch::Matchers::WithinAbs(0., 1e-5));
    }
}

TEST_CASE("Unsupported plate coordinate frames reject full import and allow geometry recovery", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    project.plate.has_model_origin = true;
    project.plate.model_origin = project.plate.workspace_position;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_plate_metadata(file.string(), [](std::string& xml) {
        const auto key = xml.find("porca_plate_layout");
        REQUIRE(key != std::string::npos);
        const auto version = xml.find("&quot;version&quot;:1", key);
        REQUIRE(version != std::string::npos);
        xml.replace(version, std::string("&quot;version&quot;:1").size(), "&quot;version&quot;:999");
    });
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    bool loaded = false;
    try {
        loaded = load_bbs_3mf(file.string().c_str(), &config, &substitutions, &model, &imported.plates,
            &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    } catch (const std::exception&) {}
    REQUIRE_FALSE(loaded);
    Model geometry;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &geometry, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(geometry.objects.size() == 1);
}

TEST_CASE("Empty plates retain their independent settings when a project opens", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    PlateData empty;
    empty.plate_index = 1;
    empty.plate_name = "Spare printer";
    empty.print_setup = setup(350, 0.6, 0.3, "#0000FF");
    empty.print_setup.destination = "scanned:bambu:offline-test-printer";
    empty.print_setup.destination_name = "Workshop spare";
    empty.has_workspace_position = true;
    empty.workspace_position = Vec2d(-500.125, 33.625);
    empty.locked = true;
    ScopedTemporaryFile file(".3mf");
    project.save(file.string(), &empty);
    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 2);
    const auto* restored = imported.plates[1];
    REQUIRE(restored->objects_and_instances.empty());
    REQUIRE(restored->print_setup.config.diff(empty.print_setup.config).empty());
    REQUIRE(restored->print_setup.destination == empty.print_setup.destination);
    REQUIRE(restored->print_setup.destination_name == empty.print_setup.destination_name);
    REQUIRE(restored->plate_name == empty.plate_name);
    REQUIRE(restored->locked);
    REQUIRE(restored->has_workspace_position);
    REQUIRE_THAT((restored->workspace_position - empty.workspace_position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}

TEST_CASE("Several plates for one printer retain independent jobs and stage membership", "[PlatePrintSetup][WorkspaceStages][3MF]")
{
    ProjectFixture project;
    project.plate.print_setup.destination = "scanned-id:79d7522d-2e67-4d82-b281-4cf2bdb43193";
    project.plate.print_setup.destination_name = "Workshop printer";
    project.plate.stage_id = "fit-check";
    project.plate.has_workspace_stages = true;
    project.plate.workspace_stages = {
        {"fit-check", "Fit check & café", Vec2d(-40.125, 12.625)},
        {"final-parts", "Final parts", Vec2d(750.25, -300.75)}
    };
    const bool resized = GENERATE(false, true);
    if (resized) project.plate.workspace_stages[1].frame = BoundingBoxf(Vec2d(-80.125, -75.375), Vec2d(625.625, 420.25));
    PlateData extra;
    extra.plate_index = 1;
    extra.plate_name = "Connector test";
    extra.print_setup = setup(180, 0.4, 0.28, "#0000FF");
    extra.print_setup.destination = project.plate.print_setup.destination;
    extra.print_setup.destination_name = project.plate.print_setup.destination_name;
    extra.stage_id = project.plate.stage_id;
    extra.has_workspace_position = true;
    extra.workspace_position = Vec2d(300, 30);
    ScopedTemporaryFile file(".3mf");
    project.save(file.string(), &extra);
    ScopedTemporaryDir backup;
    Model loaded;
    loaded.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 2);
    REQUIRE(imported.plates[0]->stage_id == project.plate.stage_id);
    REQUIRE(imported.plates[1]->stage_id == extra.stage_id);
    REQUIRE(imported.plates[0]->has_workspace_stages);
    REQUIRE(imported.plates[0]->workspace_stages.size() == 2);
    // The second stage contains no plates and still survives save/reopen.
    const auto& restored_stage = imported.plates[0]->workspace_stages[1];
    REQUIRE(restored_stage.id == project.plate.workspace_stages[1].id);
    REQUIRE(restored_stage.name == project.plate.workspace_stages[1].name);
    REQUIRE(restored_stage.frame.defined == resized);
    REQUIRE_THAT((restored_stage.frame.min - project.plate.workspace_stages[1].frame.min).norm(), Catch::Matchers::WithinAbs(0., 1e-9));
    REQUIRE_THAT((restored_stage.frame.max - project.plate.workspace_stages[1].frame.max).norm(), Catch::Matchers::WithinAbs(0., 1e-9));
    REQUIRE_THAT((restored_stage.position - project.plate.workspace_stages[1].position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
    REQUIRE(imported.plates[0]->workspace_stages[0].name == project.plate.workspace_stages[0].name);
    REQUIRE(imported.plates[0]->print_setup.destination == imported.plates[1]->print_setup.destination);
    REQUIRE(imported.plates[0]->print_setup.config.diff(project.plate.print_setup.config).empty());
    REQUIRE(imported.plates[1]->print_setup.config.diff(extra.print_setup.config).empty());
    REQUIRE_THAT(imported.plates[0]->print_setup.config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    REQUIRE_THAT(imported.plates[1]->print_setup.config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 1e-9));
    REQUIRE(imported.plates[1]->objects_and_instances.empty());
    REQUIRE_THAT((imported.plates[1]->workspace_position - extra.workspace_position).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}

TEST_CASE("Invalid stage metadata rejects full restoration and permits geometry recovery", "[WorkspaceStages][3MF]")
{
    ProjectFixture project;
    project.plate.stage_id = "stage-1";
    project.plate.has_workspace_stages = true;
    project.plate.workspace_stages = {{"stage-1", "Fit check", Vec2d(0, 0)}};
    const std::string defect = GENERATE("future-catalog", "future-membership", "missing-catalog", "unknown-stage", "duplicate-catalog", "invalid-frame");
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    edit_plate_metadata(file.string(), [&](std::string& xml) {
        const std::string key = defect == "future-membership" || defect == "unknown-stage" ? "porca_plate_stage" : "porca_workspace_stages";
        const auto key_start = xml.find("key=\"" + key + "\"");
        REQUIRE(key_start != std::string::npos);
        const auto metadata_start = xml.rfind("<metadata", key_start);
        const auto metadata_end = xml.find("/>", key_start) + 2;
        if (defect == "missing-catalog")
            xml.erase(metadata_start, metadata_end - metadata_start);
        else if (defect == "duplicate-catalog")
            xml.insert(metadata_end, xml.substr(metadata_start, metadata_end - metadata_start));
        else {
            const auto value_start = xml.find("value=\"", key_start) + 7;
            const auto value_end = xml.find('"', value_start);
            const std::string value = defect == "future-catalog" ? "{&quot;version&quot;:3,&quot;stages&quot;:[]}" :
                defect == "future-membership" ? "{&quot;version&quot;:2,&quot;id&quot;:&quot;stage-1&quot;}" :
                defect == "invalid-frame" ? "{&quot;version&quot;:2,&quot;stages&quot;:[{&quot;id&quot;:&quot;stage-1&quot;,&quot;name&quot;:&quot;Fit check&quot;,&quot;position&quot;:[0,0],&quot;frame&quot;:[[0,0],[10,20]]}]}" :
                                               "{&quot;version&quot;:1,&quot;id&quot;:&quot;missing-stage&quot;}";
            xml.replace(value_start, value_end - value_start, value);
        }
    });
    ScopedTemporaryDir backup;
    Model rejected;
    rejected.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported rejected_plates;
    bool bambu = false, orca = false;
    Semver version;
    bool restored = false;
    try {
        restored = load_bbs_3mf(file.string().c_str(), &config, &substitutions, &rejected, &rejected_plates.plates,
            &rejected_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    } catch (const RuntimeError&) {}
    REQUIRE_FALSE(restored);
    Model recovered;
    recovered.set_backup_path(backup.string());
    Imported recovered_plates;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &recovered, &recovered_plates.plates,
        &recovered_plates.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(recovered.objects.size() == 1);
    REQUIRE_THAT((recovered.objects[0]->instance_bounding_box(0).min - project.model.objects[0]->instance_bounding_box(0).min).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-6));
    REQUIRE_THAT((recovered.objects[0]->instance_bounding_box(0).max - project.model.objects[0]->instance_bounding_box(0).max).norm(),
                 Catch::Matchers::WithinAbs(0, 1e-6));
    for (const auto* plate : recovered_plates.plates) {
        REQUIRE(plate->stage_id.empty());
        REQUIRE_FALSE(plate->has_workspace_stages);
        REQUIRE(plate->workspace_stages.empty());
    }
}

TEST_CASE("Stages reopen when a member plate was saved without its own setup", "[PlatePrintSetup][WorkspaceStages][3MF]")
{
    ProjectFixture project;
    project.plate.stage_id = "stage-1";
    project.plate.has_workspace_stages = true;
    project.plate.workspace_stages = {{"stage-1", "Fit check", Vec2d(0, 0)}, {"stage-2", "Spare", Vec2d(600, 0)}};
    // Porca writes the stage list whenever stages exist, but a plate's setup
    // only when it has one. Both plates' stages must survive reopening.
    PlateData shared;
    shared.plate_index = 1;
    shared.plate_name = "Shared settings";
    shared.stage_id = "stage-2";
    const bool first_without_setup = GENERATE(false, true);
    DynamicPrintConfig project_config = project.plate.print_setup.config;
    if (first_without_setup) std::swap(project.plate.print_setup, shared.print_setup);
    ScopedTemporaryFile file(".3mf");
    StoreParams params;
    params.path = file.string();
    params.model = &project.model;
    params.config = &project_config;
    params.plate_data_list = {&project.plate, &shared};
    REQUIRE(store_bbs_3mf(params));
    ScopedTemporaryDir backup;
    Model loaded;
    loaded.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 2);
    CHECK(imported.plates[0]->print_setup.empty() == first_without_setup);
    CHECK(imported.plates[1]->print_setup.empty() != first_without_setup);
    CHECK(imported.plates[0]->stage_id == "stage-1");
    CHECK(imported.plates[1]->stage_id == "stage-2");
    REQUIRE(imported.plates[0]->has_workspace_stages);
    REQUIRE(imported.plates[0]->workspace_stages.size() == 2);
    CHECK(imported.plates[0]->workspace_stages[1].name == "Spare");
    REQUIRE(loaded.objects.size() == 1);
}

TEST_CASE("Invalid plate placement lists leave all model copies untouched", "[PlatePrintSetup]")
{
    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance()->set_offset(Vec3d(25., 35., -1.));
    PlateData plate;
    const bool duplicate = GENERATE(false, true);
    plate.objects_and_instances = {{0, 0}, {0, duplicate ? 0 : 4}};
    const Vec3d before = object->instances[0]->get_offset();
    REQUIRE_THROWS(relocate_plate_models(model, plate, Vec2d::Zero(), Vec2d(500., -500.)));
    CHECK_THAT((object->instances[0]->get_offset() - before).norm(), Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Single plate imports retain valid ownership after discarding other objects", "[PlatePrintSetup][3MF]")
{
    ProjectFixture project;
    project.plate.has_model_origin = true;
    project.plate.model_origin = project.plate.workspace_position;
    PlateData second;
    second.plate_index = 1;
    second.print_setup = project.plate.print_setup;
    second.has_model_origin = true;
    second.model_origin = Vec2d(800., -100.);
    auto* object = project.model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance()->set_offset(Vec3d(825., -65., -1.));
    second.objects_and_instances = {{1, 0}};
    const auto original_bounds = object->instance_bounding_box(0);
    ScopedTemporaryFile file(".3mf");
    project.save(file.string(), &second);
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &model, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig, nullptr, 2));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(imported.plates.size() == 2);
    REQUIRE(imported.plates[0]->objects_and_instances.empty());
    REQUIRE(imported.plates[1]->objects_and_instances.size() == 1);
    REQUIRE(imported.plates[1]->objects_and_instances[0].first == 0);
    relocate_plate_models(model, *imported.plates[1], imported.plates[1]->model_origin, second.model_origin);
    CHECK_THAT((model.objects[0]->instance_bounding_box(0).min - original_bounds.min).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT((model.objects[0]->instance_bounding_box(0).max - original_bounds.max).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
}

namespace {
void write_prusa_layout(const std::string& path, const std::string& config, const std::string& beds)
{
    Zipper zip(path);
    zip.add_entry("Metadata/Slic3r_PE.config", config.data(), config.size());
    zip.add_entry("Metadata/Prusa_Slicer_wipe_tower_information.xml", beds.data(), beds.size());
    zip.finalize();
}
}

TEST_CASE("Prusa multi-bed imports preserve empty beds, copies and bed-relative geometry", "[PlatePrintSetup]")
{
    ScopedTemporaryFile file(".3mf");
    std::string beds = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    for (int i = 0; i < 9; ++i)
        beds += "<wipe_tower_information bed_idx=\"" + std::to_string(i) + "\" position_x=\"180\" position_y=\"140\"/>\n";
    write_prusa_layout(file.string(), "; generated by PrusaSlicer 2.9.4 on 2026-10-16\n; bed_shape = 0x0,250x0,250x220,0x220\n", beds);
    const std::array<Vec2d, 9> grid = {Vec2d(0,0), Vec2d(1,0), Vec2d(0,1), Vec2d(1,1), Vec2d(2,0),
                                     Vec2d(2,1), Vec2d(0,2), Vec2d(1,2), Vec2d(2,2)};
    const Vec2d stride(250 + std::hypot(250.,220.) * .3, 220 + std::hypot(250.,220.) * .3);
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(10, 20, 5));
    volume->set_rotation(Vec3d(.15, -.2, .1));
    std::vector<BoundingBoxf3> original;
    std::vector<Transform3d> original_transforms;
    // Bed 9 is intentionally empty; two copies share bed 3.
    for (int p : {0, 1, 2, 3, 4, 5, 6, 7, 2}) {
        auto* instance = object->add_instance();
        instance->set_offset(Vec3d(grid[p].x() * stride.x() + 42, grid[p].y() * stride.y() + 56, -1));
        instance->set_rotation(Vec3d(.07 * p, -.03 * p, .3));
        instance->set_scaling_factor(Vec3d(1.1, .9, 1.2));
        instance->set_mirror(Vec3d(p % 2 ? -1 : 1, 1, 1));
        original_transforms.push_back(instance->get_matrix() * volume->get_matrix());
        original.push_back(object->instance_bounding_box(*instance));
    }
    auto plates = load_prusa_plate_layout(file.string().c_str(), model);
    REQUIRE(plates.size() == 9);
    REQUIRE(plates[2].objects_and_instances.size() == 2);
    REQUIRE(plates[8].objects_and_instances.empty());
    for (size_t p = 0; p < plates.size(); ++p) {
        REQUIRE_THAT((plates[p].model_origin - grid[p].cwiseProduct(stride)).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
        for (const auto& [o, i] : plates[p].objects_and_instances)
            REQUIRE_THAT((model.objects[o]->instance_bounding_box(i).min - original[i].min).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
        const Vec2d source = plates[p].model_origin;
        // Destination printer differs from the source; never scale or recenter the job.
        plates[p].print_setup = setup(180, .4, .2, "#FF0000");
        plates[p].has_workspace_position = true;
        plates[p].workspace_position = Vec2d((p % 3) * 260., -double(p / 3) * 280.);
        relocate_plate_models(model, plates[p], source, plates[p].workspace_position);
        for (const auto& [o, i] : plates[p].objects_and_instances) {
            const auto actual = model.objects[o]->instance_bounding_box(i);
            const Vec3d delta(plates[p].workspace_position.x() - source.x(), plates[p].workspace_position.y() - source.y(), 0);
            REQUIRE_THAT((actual.min - original[i].min - delta).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
            REQUIRE_THAT((actual.size() - original[i].size()).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
            const Transform3d transform = model.objects[o]->instances[i]->get_matrix() * volume->get_matrix();
            // XY relocation must retain the entire basis: XYZ rotation, scale and handedness.
            REQUIRE_THAT((transform.linear() - original_transforms[i].linear()).norm(), Catch::Matchers::WithinAbs(0, 1e-12));
        }
    }
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &plates[0].print_setup.config;
    for (auto& plate : plates) params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model loaded;
    ScopedTemporaryDir loaded_backup;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 9);
    REQUIRE(imported.plates[8]->objects_and_instances.empty());
    for (size_t p = 0; p < plates.size(); ++p) {
        auto& plate = *imported.plates[p];
        REQUIRE(plate.objects_and_instances == plates[p].objects_and_instances);
        relocate_plate_models(loaded, plate, plate.model_origin, plate.workspace_position);
    }
    for (size_t i = 0; i < object->instances.size(); ++i) {
        REQUIRE_THAT((loaded.objects[0]->instance_bounding_box(i).min - object->instance_bounding_box(i).min).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
        const Transform3d transform = loaded.objects[0]->instances[i]->get_matrix() * loaded.objects[0]->volumes[0]->get_matrix();
        REQUIRE_THAT((transform.linear() - original_transforms[i].linear()).norm(), Catch::Matchers::WithinAbs(0, 1e-6));
    }
}

TEST_CASE("Unrecognized or incomplete Prusa bed layouts leave geometry untouched", "[PlatePrintSetup]")
{
    ScopedTemporaryFile file(".3mf");
    ScopedTemporaryDir backup;
    Model model;
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    object->add_volume(make_cube(10,10,10));
    object->add_instance()->set_offset(Vec3d(40,50,0));
    const std::string config = "; generated by PrusaSlicer 2.9.4 on today\n; bed_shape = 0x0,250x0,250x220,0x220\n";
    const std::string valid = "<wipe_tower_information bed_idx=\"0\"/><wipe_tower_information bed_idx=\"1\"/>";
    SECTION("Unknown version") {
        write_prusa_layout(file.string(), "; generated by PrusaSlicer 2.10.0 on today\n; bed_shape = 0x0,250x0,250x220,0x220\n", valid);
    }
    SECTION("Missing bed shape") { write_prusa_layout(file.string(), "; generated by PrusaSlicer 2.9.4 on today\n", valid); }
    SECTION("Missing bed index") { write_prusa_layout(file.string(), config, "<wipe_tower_information bed_idx=\"0\"/><wipe_tower_information bed_idx=\"2\"/>"); }
    SECTION("Duplicate bed index") { write_prusa_layout(file.string(), config, valid + valid); }
    SECTION("Too many beds") { write_prusa_layout(file.string(), config, valid + "<wipe_tower_information bed_idx=\"" + std::to_string(max_project_plates) + "\"/>"); }
    SECTION("Malformed XML") { write_prusa_layout(file.string(), config, "<wipe_tower_information"); }
    SECTION("Unassigned model") { object->instances[0]->set_offset(Vec3d(-500,-500,0)); write_prusa_layout(file.string(), config, valid); }
    const Vec3d offset = object->instances[0]->get_offset();
    REQUIRE(load_prusa_plate_layout(file.string().c_str(), model).empty());
    REQUIRE_THAT((object->instances[0]->get_offset() - offset).norm(), Catch::Matchers::WithinAbs(0, 1e-9));
}


TEST_CASE("Geometry imports keep absent and explicit filament assignments", "[PlatePrintSetup][ProjectPlateImport]")
{
    ProjectFixture project;
    auto* object = project.model.objects[0];
    auto* volume = object->volumes[0];
    object->config.reset();
    volume->config.reset();
    object->config.set("layer_height", 0.12);
    volume->config.set("wall_loops", 5);
    SECTION("No assignment inherits the plate") {
        clear_project_print_settings(project.model);
        CHECK(object->config.empty());
        CHECK(volume->config.empty());
    }
    SECTION("Explicit assignments survive") {
        object->config.set("extruder", 2);
        volume->config.set("extruder", 3);
        clear_project_print_settings(project.model);
        CHECK(object->config.extruder() == 2);
        CHECK(volume->config.extruder() == 3);
        CHECK_FALSE(object->config.has("layer_height"));
        CHECK_FALSE(volume->config.has("wall_loops"));
    }
}

TEST_CASE("Geometry-only project imports retain their source bed origins", "[PlatePrintSetup][ProjectPlateImport][3MF]")
{
    ProjectFixture project;
    project.plate.has_model_origin = true;
    project.plate.model_origin = Vec2d(1000., -600.);
    project.plate.workspace_position = Vec2d(2400., -900.);
    auto* instance = project.model.objects[0]->instances[0];
    instance->set_offset(Vec3d(1042., -544., -2.));
    instance->set_rotation(Vec3d(0.2, 0.3, 0.4));
    const auto linear = instance->get_matrix().linear().eval();
    const auto expected_bounds = project.model.objects[0]->instance_bounding_box(0);
    ScopedTemporaryFile file(".3mf");
    project.save(file.string());
    const auto bed = read_project_bed_shape(file.string());
    REQUIRE(bed.size() == 4);
    CHECK_THAT(BoundingBoxf(bed).size().x(), Catch::Matchers::WithinAbs(180., 1e-8));
    ScopedTemporaryDir backup;
    Model geometry;
    geometry.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &geometry, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel));
    REQUIRE(imported.plates.size() == 1);
    CHECK(imported.plates[0]->print_setup.empty());
    REQUIRE(imported.plates[0]->has_model_origin);
    REQUIRE(imported.plates[0]->has_workspace_position);
    std::vector<PlateData> plates{*imported.plates[0]};
    plates[0].print_setup = setup(250., 0.4, 0.2, "#00AA00");
    prepare_project_plate_import(geometry, plates, config, Vec2d(3000., 0.), 2);
    const auto* after = geometry.objects[0]->instances[0];
    const auto after_bounds = geometry.objects[0]->instance_bounding_box(0);
    const Vec3d origin(plates[0].workspace_position.x(), plates[0].workspace_position.y(), 0.);
    const Vec3d source_origin(1000., -600., 0.);
    CHECK_THAT((after_bounds.min - origin - expected_bounds.min + source_origin).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT((after_bounds.max - origin - expected_bounds.max + source_origin).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK(after->get_matrix().linear().isApprox(linear, 1e-6));
}

TEST_CASE("Project import rejects ambiguous assignments before moving incoming models", "[PlatePrintSetup][ProjectPlateImport]")
{
    ProjectFixture project;
    project.plate.has_model_origin = true;
    project.plate.model_origin = Vec2d::Zero();
    std::vector<PlateData> plates{project.plate, project.plate};
    const auto before = project.model.objects[0]->instances[0]->get_matrix();
    REQUIRE_THROWS(prepare_project_plate_import(project.model, plates, project.plate.print_setup.config, Vec2d(500., 0.), 1));
    CHECK(project.model.objects[0]->instances[0]->get_matrix().isApprox(before));
    plates = {project.plate};
    REQUIRE_THROWS(prepare_project_plate_import(project.model, plates, project.plate.print_setup.config, Vec2d(500., 0.), max_project_plates));
    CHECK(project.model.objects[0]->instances[0]->get_matrix().isApprox(before));
}

TEST_CASE("Project import can fill the final available plate", "[PlatePrintSetup][ProjectPlateImport]")
{
    ProjectFixture project;
    std::vector<PlateData> plates{project.plate};
    prepare_project_plate_import(project.model, plates, project.plate.print_setup.config,
                                 Vec2d(500., 0.), max_project_plates - 1);
    CHECK(plates[0].plate_index == max_project_plates - 1);
    CHECK(plates[0].has_workspace_position);
}

TEST_CASE("Projects retain plates beyond the former limit after saving", "[PlatePrintSetup][ProjectPlateImport][3MF]")
{
    ProjectFixture project;
    std::vector<PlateData> plates(max_project_plates, project.plate);
    for (int i = 0; i < max_project_plates; ++i) {
        plates[i].plate_index = i;
        plates[i].plate_name = "Plate " + std::to_string(i + 1);
        plates[i].workspace_position = Vec2d(20. + 300. * (i % 9), 30. - 300. * (i / 9));
        if (i > 0) plates[i].objects_and_instances.clear();
    }
    ScopedTemporaryFile file(".3mf");
    StoreParams params;
    params.path = file.string();
    params.model = &project.model;
    params.config = &plates.front().print_setup.config;
    for (auto& plate : plates) params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));

    ScopedTemporaryDir backup;
    Model reopened;
    reopened.set_backup_path(backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &reopened, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == max_project_plates);
    CHECK(imported.plates.front()->objects_and_instances.size() == 1);
    CHECK(imported.plates.back()->objects_and_instances.empty());
    CHECK(imported.plates.back()->plate_name == "Plate " + std::to_string(max_project_plates));
    CHECK_THAT((imported.plates.back()->workspace_position - plates.back().workspace_position).norm(),
               Catch::Matchers::WithinAbs(0., 1e-7));
}

TEST_CASE("Combined projects retain independent settings and empty plates after saving", "[PlatePrintSetup][ProjectPlateImport][3MF]")
{
    ScopedTemporaryDir backup;
    Model combined;
    combined.set_backup_path(backup.string());
    std::vector<PlateData> all;
    std::vector<WorkspaceStage> stages;
    for (int file = 0; file < 3; ++file) {
        ProjectFixture incoming;
        incoming.plate.print_setup = setup(180. + file * 50., 0.4 + file * 0.2, 0.12 + file * 0.04, file == 0 ? "#FF0000" : file == 1 ? "#00FF00" : "#0000FF");
        incoming.plate.has_model_origin = true;
        incoming.plate.model_origin = Vec2d::Zero();
        incoming.plate.plate_name = "Job " + std::to_string(file);
        incoming.plate.stage_id = "shared-source-id";
        incoming.plate.has_workspace_stages = true;
        incoming.plate.workspace_stages = {{"shared-source-id", "Batch", Vec2d(20., 30.)}};
        PlateData empty = incoming.plate;
        empty.objects_and_instances.clear();
        empty.plate_name = "Spare " + std::to_string(file);
        empty.locked = true;
        empty.has_workspace_stages = false;
        empty.workspace_stages.clear();
        empty.workspace_position += Vec2d(400., 0.);
        std::vector<PlateData> jobs{incoming.plate, empty};
        prepare_project_plate_import(incoming.model, jobs, incoming.plate.print_setup.config, Vec2d(file * 1400., 0.), int(all.size()));
        const int object_index = int(combined.objects.size());
        combined.add_object(*incoming.model.objects[0]);
        for (auto& job : jobs) {
            for (auto& ref : job.objects_and_instances) ref.first += object_index;
            if (job.has_workspace_stages) {
                stages.insert(stages.end(), job.workspace_stages.begin(), job.workspace_stages.end());
                job.has_workspace_stages = false;
                job.workspace_stages.clear();
            }
            all.push_back(std::move(job));
        }
    }
    all.front().has_workspace_stages = true;
    all.front().workspace_stages = stages;
    ScopedTemporaryFile file(".3mf");
    StoreParams params;
    params.path = file.string();
    params.model = &combined;
    params.config = &all.front().print_setup.config;
    for (auto& plate : all) params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    ScopedTemporaryDir reopened_backup;
    Model reopened;
    reopened.set_backup_path(reopened_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &reopened, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(imported.plates.size() == 6);
    REQUIRE(imported.plates.front()->workspace_stages.size() == 3);
    for (int p = 0; p < 6; ++p) {
        const auto& loaded = *imported.plates[p];
        CHECK(loaded.plate_name == all[p].plate_name);
        CHECK(loaded.locked == all[p].locked);
        CHECK(loaded.stage_id == all[p].stage_id);
        CHECK(loaded.print_setup.config.diff(all[p].print_setup.config).empty());
        CHECK(loaded.print_setup.destination == all[p].print_setup.destination);
        CHECK(loaded.objects_and_instances.size() == all[p].objects_and_instances.size());
        CHECK_THAT((loaded.workspace_position - all[p].workspace_position).norm(), Catch::Matchers::WithinAbs(0., 1e-7));
        if (loaded.objects_and_instances.empty()) continue;
        const auto ref = loaded.objects_and_instances.front();
        const auto bounds = reopened.objects[ref.first]->instance_bounding_box(ref.second);
        const Vec3d origin(loaded.model_origin.x(), loaded.model_origin.y(), 0.);
        const auto before = all[p].objects_and_instances.front();
        const auto expected = combined.objects[before.first]->instance_bounding_box(before.second);
        const Vec3d previous_origin(all[p].workspace_position.x(), all[p].workspace_position.y(), 0.);
        CHECK_THAT((bounds.min - origin - expected.min + previous_origin).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT((bounds.max - origin - expected.max + previous_origin).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
    }
}


TEST_CASE("Transferred paint and independent printer palettes survive saving and reopening", "[PlateMaterialTransfer][PlatePrintSetup]")
{
    ScopedTemporaryDir backup;
    ScopedTemporaryFile file(".3mf");
    Model model;
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    object->name = "Moved painted part";
    auto* volume = object->add_volume(make_cube(12, 12, 6));
    TriangleSelector paint(volume->mesh());
    paint.set_facet(0, EnforcerBlockerType::Extruder1);
    paint.set_facet(1, EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set_data(paint.serialize());
    object->config.set("extruder", 1);
    object->config.set("wall_loops", 5);
    object->layer_config_ranges[{1., 3.}].set("extruder", 2);
    object->add_instance()->set_offset(Vec3d(30, 50, -1.5));
    object->add_instance()->set_offset(Vec3d(70, 50, -1.5));
    PlateData first, second;
    first.plate_index = 0;
    first.plate_name = "Small printer";
    first.print_setup = setup(180, 0.4, 0.2, "#000000");
    first.print_setup.config.set_num_filaments(2);
    first.print_setup.config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#FFFFFF"}));
    first.print_setup.config.set_key_value("filament_settings_id", new ConfigOptionStrings({"Black PETG", "White PLA"}));
    first.print_setup.config.set_key_value("filament_type", new ConfigOptionStrings({"PETG", "PLA"}));
    first.print_setup.config.set_key_value("filament_vendor", new ConfigOptionStrings({"Maker", "Maker"}));
    first.print_setup.config.set_key_value("filament_ids", new ConfigOptionStrings({"GFL99", "GFL98"}));
    first.print_setup.config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2}));
    first.print_setup.config.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    first.print_setup.config.set_key_value("filament_nozzle_map", new ConfigOptionInts({0, 0}));
    first.print_setup.config.set_key_value("filament_map", new ConfigOptionInts({1, 1}));
    first.print_setup.config.set_key_value("filament_volume_map", new ConfigOptionInts({0, 0}));
    first.objects_and_instances = {{0, 0}};
    second.plate_index = 1;
    second.plate_name = "Large printer";
    second.print_setup = first.print_setup;
    second.print_setup.config.set_key_value("printable_area", new ConfigOptionPoints({Vec2d(0, 0), Vec2d(350, 0), Vec2d(350, 350), Vec2d(0, 350)}));
    second.print_setup.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
    second.print_setup.config.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF", "#000000"}));
    second.print_setup.config.set_key_value("filament_settings_id", new ConfigOptionStrings({"White PLA", "Black PETG"}));
    second.print_setup.config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PETG"}));
    second.print_setup.config.set_key_value("filament_ids", new ConfigOptionStrings({"GFL98", "GFL99"}));
    second.objects_and_instances = {{1, 0}};
    first.has_workspace_position = second.has_workspace_position = true;
    first.workspace_position = Vec2d(0, 0);
    second.workspace_position = Vec2d(420, 0);
    if (const char* output = std::getenv("PORCA_MATERIAL_TRANSFER_ARTIFACT_DIR")) {
        boost::filesystem::create_directories(output);
        StoreParams fixture;
        fixture.path = (boost::filesystem::path(output) / "before-transfer.3mf").string();
        fixture.model = &model;
        fixture.config = &first.print_setup.config;
        first.objects_and_instances = {{0, 0}, {0, 1}};
        second.objects_and_instances.clear();
        fixture.plate_data_list = {&first, &second};
        REQUIRE(store_bbs_3mf(fixture));
        first.objects_and_instances = {{0, 0}};
        second.objects_and_instances = {{1, 0}};
    }
    auto transformation = object->instances[0]->get_transformation();
    transformation.set_offset(Vec3d(450, 50, -1.5));
    const auto transferred = apply_plate_instance_transfers(model, {{0, 0, transformation, {{1, 2}, {2, 1}}, 2}});
    REQUIRE(transferred[0].first == 1);
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &first.print_setup.config;
    params.plate_data_list = {&first, &second};
    REQUIRE(store_bbs_3mf(params));

    ScopedTemporaryDir loaded_backup;
    Model loaded;
    loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    Imported imported;
    bool bambu = false, orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(file.string().c_str(), &config, &substitutions, &loaded, &imported.plates,
        &imported.presets, &bambu, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(loaded.objects.size() == 2);
    REQUIRE(imported.plates.size() == 2);
    CHECK(loaded.objects[0]->config.extruder() == 1);
    CHECK(loaded.objects[1]->config.extruder() == 2);
    CHECK(loaded.objects[1]->config.opt_int("wall_loops") == 5);
    CHECK(loaded.objects[1]->layer_config_ranges.begin()->second.extruder() == 1);
    CHECK(loaded.objects[1]->volumes[0]->mmu_segmentation_facets.get_data() == model.objects[1]->volumes[0]->mmu_segmentation_facets.get_data());
    CHECK_THAT(loaded.objects[1]->instance_bounding_box(0).min.z(),
               Catch::Matchers::WithinAbs(model.objects[1]->instance_bounding_box(0).min.z(), 1e-9));
    CHECK(imported.plates[0]->print_setup.config.option("filament_settings_id")->serialize() == first.print_setup.config.option("filament_settings_id")->serialize());
    CHECK(imported.plates[1]->print_setup.config.option("filament_settings_id")->serialize() == second.print_setup.config.option("filament_settings_id")->serialize());
    CHECK_THAT(imported.plates[0]->print_setup.config.option<ConfigOptionFloats>("nozzle_diameter")->values[0], Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK_THAT(imported.plates[1]->print_setup.config.option<ConfigOptionFloats>("nozzle_diameter")->values[0], Catch::Matchers::WithinAbs(0.6, 1e-9));
}
