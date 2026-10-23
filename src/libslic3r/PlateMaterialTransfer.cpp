#include "PlateMaterialTransfer.hpp"
#include "Config.hpp"
#include "Color.hpp"
#include "Model.hpp"
#include "ObjectID.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "TriangleSelector.hpp"
#include "TextureToColor/ColorUtils.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

int configured_extruder(const ModelConfig& config)
{
    const auto* value = config.get().option<ConfigOptionInt>("extruder");
    return value ? value->value : 0;
}

void collect_config_slots(const ModelConfig& config, std::set<int>& slots)
{
    for (const auto* key : object_filament_keys)
        if (const auto* value = config.get().option<ConfigOptionInt>(key); value && value->value > 0)
            slots.insert(value->value);
}

// Every filament setting an automatic match must keep unchanged.
const std::vector<std::string>& compared_filament_keys()
{
    static const std::vector<std::string> keys = [] {
        auto result = Preset::filament_options();
        for (const char* key : {"filament_multi_colour", "filament_colour_type", "filament_finish"}) result.emplace_back(key);
        return result;
    }();
    return keys;
}

// Serialized once per config: comparing slots would otherwise serialize every
// slot of every compared vector for each pair of candidate filaments.
class FilamentSettingValues
{
public:
    explicit FilamentSettingValues(const DynamicPrintConfig& config)
    {
        const auto& keys = compared_filament_keys();
        m_entries.reserve(keys.size());
        for (const auto& key : keys) {
            Entry entry;
            if (const auto* option = config.option(key)) {
                if (const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(option)) {
                    entry.values = values->vserialize();
                } else {
                    entry.values = {option->serialize()};
                    entry.scalar = true;
                }
            }
            m_entries.push_back(std::move(entry));
        }
    }

    bool same_slot(size_t slot, const FilamentSettingValues& other, size_t other_slot) const
    {
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].at(slot) != other.m_entries[i].at(other_slot)) return false;
        return true;
    }

private:
    struct Entry
    {
        std::vector<std::string> values;
        bool scalar = false;
        const std::string& at(size_t slot) const
        {
            static const std::string missing;
            return scalar ? values.front() : slot < values.size() ? values[slot] : missing;
        }
    };
    std::vector<Entry> m_entries;
};

// Built on first use, so candidates rejected by cheaper checks serialize nothing.
class LazyFilamentSettings
{
public:
    explicit LazyFilamentSettings(const DynamicPrintConfig& config) : m_config(config) {}
    const FilamentSettingValues& get() const
    {
        if (!m_values) m_values.emplace(m_config);
        return *m_values;
    }

private:
    const DynamicPrintConfig& m_config;
    mutable std::optional<FilamentSettingValues> m_values;
};

bool known_value(const std::string& value)
{
    return !value.empty() && value != "(Undefined)";
}

bool filament_rgb(const DynamicPrintConfig& config, size_t slot, ColorRGBA& color)
{
    const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || slot >= colors->values.size()) return false;
    const auto& value = colors->values[slot];
    return can_decode_color(value) && std::all_of(value.begin() + 1, value.end(),
        [](unsigned char c) { return std::isxdigit(c); }) && decode_color(value, color);
}

double filament_diameter(const DynamicPrintConfig& config, size_t slot)
{
    const auto* values = config.option<ConfigOptionFloats>("filament_diameter");
    return values && values->values.size() == plate_filament_count(config) && slot < values->values.size() && std::isfinite(values->values[slot])
        ? values->values[slot] : 0.;
}

std::string filament_identity_value(const DynamicPrintConfig& config, const char* key, size_t slot)
{
    const auto* values = config.option<ConfigOptionStrings>(key);
    return values && values->values.size() == plate_filament_count(config) && slot < values->values.size()
        ? values->values[slot] : std::string{};
}

bool flat_filament_palette(const DynamicPrintConfig& config)
{
    const auto* indices = config.option<ConfigOptionInts>("filament_self_index");
    if (!indices) return true;
    if (indices->values.size() != plate_filament_count(config)) return false;
    for (size_t i = 0; i < indices->values.size(); ++i)
        if (indices->values[i] != int(i + 1)) return false;
    return true;
}

bool same_filament(const DynamicPrintConfig& source, size_t from, const LazyFilamentSettings& source_settings,
                   const DynamicPrintConfig& destination, size_t to, const LazyFilamentSettings& destination_settings)
{
    const double source_diameter = filament_diameter(source, from);
    const double destination_diameter = filament_diameter(destination, to);
    if (source_diameter <= 0 || destination_diameter <= 0 || source_diameter != destination_diameter) return false;
    // Variant rows are indexed by filament_self_index, not by color. Leave
    // non-flat variant palettes to explicit choices rather than comparing the
    // wrong row or ignoring an edited secondary variant.
    if (!flat_filament_palette(source) || !flat_filament_palette(destination)) return false;
    ColorRGBA source_color, destination_color;
    if (!filament_rgb(source, from, source_color) || !filament_rgb(destination, to, destination_color) ||
        source_color != destination_color) return false;
    for (const char* key : {"filament_type", "filament_vendor", "filament_settings_id"}) {
        const auto value = filament_identity_value(source, key, from);
        if (!known_value(value) || value != filament_identity_value(destination, key, to)) return false;
    }
    // Identity alone does not establish equal settings: edited temperature,
    // flow, scripts or plugin overrides must also prevent an automatic match.
    return source_settings.get().same_slot(from, destination_settings.get(), to);
}
}

size_t plate_filament_count(const DynamicPrintConfig& config)
{
    const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    return colors ? colors->values.size() : 0;
}

bool plate_filament_is_physical(const DynamicPrintConfig& config, int slot)
{
    if (slot <= 0 || size_t(slot) > plate_filament_count(config)) return false;
    const auto* mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    // Match PresetBundle::is_mixed_filament: older ordinary palettes may
    // have no flag or a shorter flag vector. Only an explicit true is virtual.
    return !mixed || size_t(slot) > mixed->values.size() || !mixed->values[size_t(slot - 1)];
}

std::set<int> object_filament_slots(const ModelObject& object)
{
    std::set<int> slots;
    collect_config_slots(object.config, slots);
    for (const auto* volume : object.volumes) {
        collect_config_slots(volume->config, slots);
        if (volume->is_model_part() && configured_extruder(volume->config) <= 0)
            slots.insert(std::max(1, configured_extruder(object.config)));
        for (size_t slot : volume->get_extruders_from_multi_material_painting()) slots.insert(int(slot + 1));
    }
    for (const auto& range : object.layer_config_ranges) collect_config_slots(range.second, slots);
    if (slots.empty()) slots.insert(1);
    return slots;
}

std::map<int, int> match_plate_filaments(const ModelObject& object,
                                       const DynamicPrintConfig& source,
                                       const DynamicPrintConfig& destination)
{
    std::map<int, int> matches;
    for (int from : object_filament_slots(object)) {
        if (!plate_filament_is_physical(source, from)) continue;
        const auto candidates = rank_plate_filaments(source, from, destination);
        const auto unique_match = [&candidates](auto accepts) {
            int match = 0;
            for (const auto& candidate : candidates) {
                if (!accepts(candidate)) continue;
                if (match) return 0;
                match = candidate.slot;
            }
            return match;
        };
        int match = unique_match([](const auto& candidate) { return candidate.exact; });
        // A printer's temperature, flow or scripts do not change which color
        // the painted part should use. Preselect that assignment independently;
        // the GUI still reviews destination settings that differ.
        if (!match) match = unique_match([](const auto& candidate) {
            return candidate.same_material && candidate.same_color && candidate.same_profile;
        });
        if (!match) match = unique_match([](const auto& candidate) {
            return candidate.same_material && candidate.same_color;
        });
        if (match) matches.emplace(from, match);
    }
    return matches;
}

std::vector<PlateFilamentCandidate> rank_plate_filaments(const DynamicPrintConfig& source, int from,
                                                       const DynamicPrintConfig& destination)
{
    std::vector<PlateFilamentCandidate> candidates;
    const bool physical = plate_filament_is_physical(source, from);
    const size_t index = from > 0 ? size_t(from - 1) : 0;
    const auto type = physical ? filament_identity_value(source, "filament_type", index) : std::string{};
    const double diameter = physical ? filament_diameter(source, index) : 0.;
    ColorRGBA source_color;
    const bool source_color_known = physical && filament_rgb(source, index, source_color);
    const auto profile = physical ? filament_identity_value(source, "filament_settings_id", index) : std::string{};
    const auto vendor = physical ? filament_identity_value(source, "filament_vendor", index) : std::string{};
    const LazyFilamentSettings source_settings(source), destination_settings(destination);
    for (size_t to = 0; to < plate_filament_count(destination); ++to) {
        if (!plate_filament_is_physical(destination, int(to + 1))) continue;
        PlateFilamentCandidate candidate{int(to + 1)};
        const auto destination_type = filament_identity_value(destination, "filament_type", to);
        const double destination_diameter = filament_diameter(destination, to);
        candidate.material_known = known_value(type) && known_value(destination_type) && diameter > 0 && destination_diameter > 0;
        candidate.same_material = candidate.material_known && type == destination_type && diameter == destination_diameter;
        candidate.same_profile = candidate.same_material && known_value(profile) && known_value(vendor) &&
            profile == filament_identity_value(destination, "filament_settings_id", to) && vendor == filament_identity_value(destination, "filament_vendor", to);
        ColorRGBA destination_color;
        if (source_color_known && filament_rgb(destination, to, destination_color)) {
            candidate.same_color = source_color == destination_color;
            // An alpha swatch has no single perceived color without a background.
            if (source_color.a() == 1.f && destination_color.a() == 1.f)
                candidate.color_distance = tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
                    {source_color.r(), source_color.g(), source_color.b()},
                    {destination_color.r(), destination_color.g(), destination_color.b()});
        }
        candidate.exact = physical && same_filament(source, index, source_settings, destination, to, destination_settings);
        candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::make_tuple(!a.exact, !a.same_material, !a.same_profile, a.color_distance, a.slot) <
               std::make_tuple(!b.exact, !b.same_material, !b.same_profile, b.color_distance, b.slot);
    });
    return candidates;
}

void validate_plate_instance_transfers(const Model& model, const std::vector<PlateInstanceTransfer>& transfers)
{
    std::set<std::pair<size_t, size_t>> seen;
    for (const auto& transfer : transfers) {
        if (transfer.object_idx >= model.objects.size() ||
            transfer.instance_idx >= model.objects[transfer.object_idx]->instances.size() ||
            !seen.emplace(transfer.object_idx, transfer.instance_idx).second ||
            !transfer.transformation.get_matrix().matrix().allFinite())
            throw std::invalid_argument("Invalid plate transfer instance");
        for (const auto& [from, to] : transfer.filaments)
            if (from <= 0 || size_t(from) >= EnforcerBlockerStateMap{}.size() ||
                to <= 0 || size_t(to) > transfer.destination_filament_count ||
                size_t(to) >= EnforcerBlockerStateMap{}.size())
                throw std::invalid_argument("Invalid plate transfer filament choice");
        for (int from : object_filament_slots(*model.objects[transfer.object_idx])) {
            const auto match = transfer.filaments.find(from);
            if (match == transfer.filaments.end() || match->second <= 0 ||
                size_t(match->second) > transfer.destination_filament_count ||
                size_t(match->second) >= EnforcerBlockerStateMap{}.size())
                throw std::invalid_argument("Incomplete plate transfer filament choices");
        }
    }
}

std::vector<std::pair<size_t, size_t>> apply_plate_instance_transfers(
    Model& model, const std::vector<PlateInstanceTransfer>& transfers)
{
    validate_plate_instance_transfers(model, transfers);
    // Copies moving with the same assignment stay linked. A different mapping
    // gets its own object before any shared configuration or paint is changed.
    using Mapping = std::map<int, int>;
    std::map<size_t, std::map<Mapping, std::vector<size_t>>> groups;
    std::vector<ObjectID> original_ids;
    for (size_t i = 0; i < transfers.size(); ++i) {
        const auto& transfer = transfers[i];
        groups[transfer.object_idx][transfer.filaments].push_back(i);
        original_ids.push_back(model.objects[transfer.object_idx]->instances[transfer.instance_idx]->id());
    }
    std::vector<std::pair<size_t, size_t>> result(transfers.size());
    std::vector<ObjectID> result_ids(transfers.size());
    for (const auto& [object_idx, mappings] : groups) {
        for (const auto& [filaments, indices] : mappings) {
            auto* object = model.objects[object_idx];
            std::map<size_t, size_t> instance_to_transfer;
            std::set<size_t> instances;
            for (size_t i : indices)
                for (size_t n = 0; n < object->instances.size(); ++n)
                    if (object->instances[n]->id() == original_ids[i]) {
                        instances.insert(n);
                        instance_to_transfer.emplace(n, i);
                        break;
                    }
            const bool remap = std::any_of(filaments.begin(), filaments.end(),
                                           [](const auto& entry) { return entry.first != entry.second; });
            const bool detach = remap && instances.size() < object->instances.size();
            std::vector<std::string> production_ids;
            for (size_t n : instances) production_ids.push_back(object->instances[n]->production_id);
            if (detach) {
                object = model.separate_instances(object_idx, instances);
                if (!object) throw std::logic_error("Validated plate transfer copies could not be separated");
            }
            if (remap) {
                Mapping zero_based;
                for (const auto& entry : filaments) zero_based.emplace(entry.first - 1, entry.second - 1);
                // Resolve an implicit default only when it was actually used.
                if (configured_extruder(object->config) <= 0 && filaments.count(1)) object->config.set("extruder", 1);
                remap_object_filament_slots(*object, zero_based);
            }
            size_t local = 0;
            for (const auto& [old_instance, i] : instance_to_transfer) {
                auto* instance = object->instances[detach ? local : old_instance];
                if (detach) instance->production_id = production_ids[local];
                instance->set_transformation(transfers[i].transformation);
                result_ids[i] = instance->id();
                ++local;
            }
            object->invalidate_bounding_box();
        }
    }
    for (size_t i = 0; i < result_ids.size(); ++i)
        for (size_t o = 0; o < model.objects.size(); ++o)
            for (size_t n = 0; n < model.objects[o]->instances.size(); ++n)
                if (model.objects[o]->instances[n]->id() == result_ids[i]) result[i] = {o, n};
    return result;
}
} // namespace Slic3r
