#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "libslic3r/Config.hpp"
#include "libslic3r/Semver.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/PlatePrintSetup.hpp"
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
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <boost/filesystem/operations.hpp>
#include <cstdlib>
#include "test_utils.hpp"
#include <string>
#include <sstream>
#include <vector>
#include <functional>
#include <utility>
#include <miniz.h>
#include <nlohmann/json.hpp>

using namespace Slic3r;

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

void edit_plate_metadata(const std::string& path, const std::function<void(std::string&)>& edit)
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
        if (std::string(stat.m_filename) == "Metadata/model_settings.config") {
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
