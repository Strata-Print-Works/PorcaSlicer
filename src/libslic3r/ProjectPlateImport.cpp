#include "ProjectPlateImport.hpp"
#include "BoundingBox.hpp"
#include "Config.hpp"
#include "Exception.hpp"
#include "Format/bbs_3mf.hpp"
#include "I18N.hpp"
#include "Model.hpp"
#include "PlatePrintSetup.hpp"
#include "PlateCapacity.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "miniz_extension.hpp"
#include "Utils.hpp"
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
ProjectSettingsReview review_project_settings(const ConfigSubstitutionContext& substitutions)
{
    const auto readable = [](std::string key) {
        std::replace(key.begin(), key.end(), '_', ' ');
        if (!key.empty()) key.front() = char(std::toupper(static_cast<unsigned char>(key.front())));
        return key;
    };
    std::set<std::string> unavailable;
    for (const auto& key : substitutions.unrecogized_keys)
        if (!key.empty()) unavailable.insert(readable(key));
    std::set<ProjectSettingsReview::Change> changed;
    for (const auto& substitution : substitutions.substitutions) {
        const ConfigOptionDef* def = substitution.opt_def;
        if (!def || !substitution.new_value) continue;
        std::string used = def->type == coBool ? (substitution.new_value->getBool() ? "true" : "false") :
                                                 substitution.new_value->serialize();
        const auto value = std::find(def->enum_values.begin(), def->enum_values.end(), used);
        if (value != def->enum_values.end() && size_t(value - def->enum_values.begin()) < def->enum_labels.size())
            used = I18N::translate(def->enum_labels[size_t(value - def->enum_values.begin())]);
        const std::string& label = def->full_label.empty() ? def->label : def->full_label;
        changed.insert({label.empty() ? readable(def->opt_key) : I18N::translate(label), substitution.old_value, used});
    }
    return {{unavailable.begin(), unavailable.end()}, {changed.begin(), changed.end()}};
}

void relocate_plate_models(Model& model, PlateData& plate, const Vec2d& source, const Vec2d& target)
{
    if (!source.allFinite() || !target.allFinite())
        throw RuntimeError("Cannot restore this plate's layout.");
    // Validate the complete list before changing any instance.
    std::set<std::pair<int, int>> instances;
    for (const auto& [object, instance] : plate.objects_and_instances) {
        if (object < 0 || object >= int(model.objects.size()) || instance < 0 ||
            instance >= int(model.objects[object]->instances.size()) || !instances.emplace(object, instance).second)
            throw RuntimeError("Cannot restore this plate's model placement.");
    }
    const Vec3d delta(target.x() - source.x(), target.y() - source.y(), 0.);
    for (const auto& [object, instance] : instances) {
        auto* owner = model.objects[object];
        auto* copy = owner->instances[instance];
        copy->set_offset(copy->get_offset() + delta);
        owner->invalidate_bounding_box();
    }
    plate.has_model_origin = true;
    plate.model_origin = target;
}

void clear_project_print_settings(Model& model)
{
    const auto clear_config = [](ModelConfig& config) {
        const bool assigned = config.has("extruder");
        const int slot = assigned ? config.extruder() : 0;
        config.reset();
        if (assigned) config.set("extruder", slot);
    };
    for (auto* object : model.objects) {
        clear_config(object->config);
        object->layer_config_ranges.clear();
        object->layer_height_profile.clear();
        for (auto* volume : object->volumes) clear_config(volume->config);
    }
    model.plates_custom_gcodes.clear();
}

Pointfs read_project_bed_shape(const std::string& path)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path)) return {};
    ScopeGuard close([&] { close_zip_reader(&zip); });
    const int index = mz_zip_reader_locate_file(&zip, "Metadata/project_settings.config", nullptr, 0);
    mz_zip_archive_file_stat stat;
    if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_uncomp_size > 8 * 1024 * 1024) return {};
    std::string text(size_t(stat.m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(&zip, index, text.data(), text.size(), 0)) return {};
    try {
        const auto json = nlohmann::json::parse(text);
        const auto& area = json.at("printable_area");
        ConfigOptionPoints points;
        std::string value;
        if (area.is_string()) value = area.get<std::string>();
        else if (area.is_array()) {
            for (const auto& point : area) {
                if (!value.empty()) value += ',';
                value += point.get<std::string>();
            }
        }
        if (!points.deserialize(value) || points.values.size() < 3 ||
            std::any_of(points.values.begin(), points.values.end(), [](const Vec2d& p) { return !p.allFinite(); })) return {};
        return points.values;
    } catch (const std::exception&) { return {}; }
}

void prepare_project_plate_import(Model& model, std::vector<PlateData>& plates,
                                  const DynamicPrintConfig& source_config,
                                  const Vec2d& minimum, int first_plate_index)
{
    if (plates.empty() || !minimum.allFinite())
        throw RuntimeError("Cannot import these plates.");
    if (first_plate_index < 0 || first_plate_index > max_project_plates ||
        plates.size() > size_t(max_project_plates - first_plate_index))
        throw RuntimeError("Importing these plates would exceed the " + std::to_string(max_project_plates) + "-plate project limit.");
    std::set<std::pair<int, int>> assigned;
    std::vector<WorkspaceStage> stages;
    bool has_catalog = false;
    double source_width = 0., source_depth = 0., width = 0., depth = 0.;
    if (const auto* area = source_config.option<ConfigOptionPoints>("printable_area"); area && area->values.size() >= 3) {
        const auto size = BoundingBoxf(area->values).size();
        source_width = size.x();
        source_depth = size.y();
    }
    for (auto& plate : plates) {
        // Validate and remove local connection settings before any geometry moves.
        plate.print_setup = PlatePrintSetup::deserialize(plate.print_setup.serialize());
        const auto size = BoundingBoxf(plate.print_setup.config.option<ConfigOptionPoints>("printable_area")->values).size();
        width = std::max(width, size.x());
        depth = std::max(depth, size.y());
        if (plate.has_workspace_stages) {
            if (has_catalog) {
                BOOST_LOG_TRIVIAL(error) << "Project import found more than one porca_workspace_stages catalog";
                throw RuntimeError(_u8L("This project's plate arrangement is damaged and can't be imported."));
            }
            stages = deserialize_workspace_stages(serialize_workspace_stages(plate.workspace_stages));
            has_catalog = true;
        }
        for (const auto& ref : plate.objects_and_instances) {
            if (ref.first < 0 || ref.first >= int(model.objects.size()) || ref.second < 0 ||
                ref.second >= int(model.objects[ref.first]->instances.size()) || !assigned.insert(ref).second)
                throw RuntimeError("Cannot import this project's plate assignments.");
        }
    }
    for (int o = 0; o < int(model.objects.size()); ++o)
        for (int i = 0; i < int(model.objects[o]->instances.size()); ++i)
            if (!assigned.count({o, i}))
                throw RuntimeError("Some models are not assigned to a plate. Import geometry only to recover them.");
    if (source_width <= 0. || source_depth <= 0.) { source_width = width; source_depth = depth; }
    std::map<std::string, std::string> stage_ids;
    for (auto& stage : stages) {
        const std::string id = boost::uuids::to_string(boost::uuids::random_generator()());
        stage_ids.emplace(stage.id, id);
        stage.id = id;
    }
    BoundingBoxf enclosure;
    const int columns = std::max(1, int(std::ceil(std::sqrt(double(plates.size())))));
    const Vec2d stride = workspace_plate_stride(width, depth);
    std::vector<Vec2d> origins;
    for (int p = 0; p < int(plates.size()); ++p) {
        auto& plate = plates[p];
        const Vec2d origin = plate.has_workspace_position ? plate.workspace_position :
            (!stages.empty() && !plate.has_model_origin ? orca_plate_origin(p, int(plates.size()), source_width, source_depth) :
                Vec2d((p % columns) * stride.x(), -(p / columns) * stride.y()));
        if (!origin.allFinite()) throw RuntimeError("Cannot import this project's plate positions.");
        origins.push_back(origin);
        const BoundingBoxf bed(plate.print_setup.config.option<ConfigOptionPoints>("printable_area")->values);
        enclosure.merge(BoundingBoxf(bed.min + origin - Vec2d::Constant(workspace_stage_gap),
                                     bed.max + origin + Vec2d(workspace_controls_clearance, workspace_title_clearance)));
        if (!plate.stage_id.empty()) {
            const auto found = stage_ids.find(plate.stage_id);
            if (found == stage_ids.end()) throw RuntimeError("Cannot import this plate's stage.");
            plate.stage_id = found->second;
        }
    }
    std::vector<WorkspacePlateBounds> bounds;
    for (int p = 0; p < int(plates.size()); ++p) {
        const BoundingBoxf bed(plates[p].print_setup.config.option<ConfigOptionPoints>("printable_area")->values);
        bounds.push_back({plates[p].stage_id, origins[p], BoundingBoxf(bed.min + origins[p], bed.max + origins[p]), plates[p].locked});
    }
    for (const auto& stage : stages) {
        auto box = workspace_stage_bounds(stage, bounds);
        if (box.defined) { box.max.y() += workspace_title_clearance; enclosure.merge(box); }
    }
    const Vec2d shift = minimum - enclosure.min;
    for (int p = 0; p < int(plates.size()); ++p) {
        auto& plate = plates[p];
        const Vec2d source = plate.has_model_origin ? plate.model_origin : plate.has_workspace_position ? plate.workspace_position :
            orca_plate_origin(p, int(plates.size()), source_width, source_depth);
        const Vec2d destination = origins[p] + shift;
        if (!destination.allFinite() || destination.cwiseAbs().maxCoeff() > 100000.)
            throw RuntimeError("These plates would be too far from the workspace origin.");
        relocate_plate_models(model, plate, source, destination);
        plate.has_workspace_position = true;
        plate.workspace_position = destination;
        plate.plate_index = first_plate_index + p;
        for (const char* key : {"wipe_tower_x", "wipe_tower_y"}) {
            auto* values = plate.print_setup.config.option<ConfigOptionFloats>(key);
            if (!values || values->values.empty()) continue;
            const double value = values->get_at(p);
            // resize() may reallocate; never pass it a reference into the vector.
            const double fill = values->values.front();
            values->values.resize(first_plate_index + plates.size(), fill);
            values->values[first_plate_index + p] = value;
        }
        plate.has_workspace_stages = false;
        plate.workspace_stages.clear();
    }
    for (auto& stage : stages) stage.position += shift;
    if (!stages.empty()) {
        plates.front().has_workspace_stages = true;
        plates.front().workspace_stages = std::move(stages);
    }
}
}
