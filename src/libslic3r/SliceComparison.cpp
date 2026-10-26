#include "SliceComparison.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>
#include <tuple>

namespace Slic3r {
size_t SliceSnapshot::memory_bytes() const
{
    size_t bytes = sizeof(*this) + label.capacity() + features.size() * (sizeof(ComparedFeature) + 64);
    for (const auto& [key, value] : settings)
        bytes += sizeof(value) + key.capacity() + value.label.capacity() + value.value.capacity() + 64;
    for (const auto& [z, layer] : layers)
        bytes += sizeof(layer) + layer.paths.capacity() * sizeof(ComparedPath) + 64;
    return bytes;
}
std::vector<ComparedSettingChange> compare_slice_settings(const SliceSnapshot& before, const SliceSnapshot& after)
{
    if (!before.settings_available || !after.settings_available) return {};
    std::set<std::string> keys;
    for (const auto& [key, value] : before.settings) keys.insert(key);
    for (const auto& [key, value] : after.settings) keys.insert(key);
    std::vector<ComparedSettingChange> changes;
    for (const auto& key : keys) {
        const auto a = before.settings.find(key), b = after.settings.find(key);
        if (a != before.settings.end() && b != after.settings.end() && a->second.value == b->second.value) continue;
        changes.push_back({b == after.settings.end() ? a->second.label : b->second.label,
                           a == before.settings.end() ? std::string() : a->second.value,
                           b == after.settings.end() ? std::string() : b->second.value});
    }
    return changes;
}
namespace {
using PathKey = std::array<int64_t, 4>;
PathKey key(const ComparedPath& p)
{
    std::array<int64_t, 2> a{std::llround(p.x0 * 100.), std::llround(p.y0 * 100.)};
    std::array<int64_t, 2> b{std::llround(p.x1 * 100.), std::llround(p.y1 * 100.)};
    if (b < a) std::swap(a, b);
    return {a[0], a[1], b[0], b[1]};
}
using PrintingKey = std::tuple<uint16_t, uint64_t, int64_t, int64_t, int64_t, int64_t>;
PrintingKey printing_key(const ComparedPath& p)
{
    return {p.role, p.material_id, std::llround(p.speed * 100.), std::llround(p.width * 1000.),
            std::llround(p.height * 1000.), std::llround(p.flow * 100000.)};
}
}
ComparedPaths compare_slice_paths(const ComparedLayer* before, const ComparedLayer* after)
{
    ComparedPaths result;
    std::map<PathKey, std::multimap<PrintingKey, ComparedPath>> remaining;
    if (before) for (const auto& path : before->paths) remaining[key(path)].emplace(printing_key(path), path);
    std::vector<ComparedPath> unmatched;
    // Match every unchanged occurrence first so an adjusted duplicate cannot
    // consume a line that has an exact counterpart later in the other result.
    if (after) for (const auto& path : after->paths) {
        auto found = remaining.find(key(path));
        if (found != remaining.end()) {
            auto match = found->second.find(printing_key(path));
            if (match != found->second.end()) {
                result.shared.push_back(path); found->second.erase(match); continue;
            }
        }
        unmatched.push_back(path);
    }
    for (const auto& path : unmatched) {
        auto found = remaining.find(key(path));
        if (found == remaining.end() || found->second.empty()) result.after_only.push_back(path);
        else {
            result.adjusted.push_back(path);
            found->second.erase(found->second.begin());
        }
    }
    for (const auto& [k, paths] : remaining)
        for (const auto& [printing, path] : paths) result.before_only.push_back(path);
    return result;
}

std::vector<int64_t> compared_layer_heights(const SliceSnapshot& before, const SliceSnapshot& after)
{
    std::set<int64_t> heights;
    for (const auto& [z, layer] : before.layers) heights.insert(z);
    for (const auto& [z, layer] : after.layers) heights.insert(z);
    return {heights.begin(), heights.end()};
}
void SliceComparisonHistory::record(size_t plate, SliceSnapshot snapshot)
{
    auto& entry = m_entries[plate];
    entry.touched = ++m_clock;
    if (entry.current && entry.current->revision == snapshot.revision) {
        // An authoritative completion can enrich a previously loaded file without
        // inventing an earlier slice of the same result.
        if (!entry.current->settings_available && snapshot.settings_available)
            entry.current = std::make_shared<SliceSnapshot>(std::move(snapshot));
        return;
    }
    if (snapshot.memory_bytes() > m_budget) {
        snapshot.paths_available = false;
        for (auto& [z, layer] : snapshot.layers) std::vector<ComparedPath>().swap(layer.paths);
    }
    entry.previous = std::move(entry.current);
    entry.current = std::make_shared<SliceSnapshot>(std::move(snapshot));
    const auto bytes = [&] {
        size_t size = 0;
        for (const auto& [id, item] : m_entries) {
            if (item.current) size += item.current->memory_bytes();
            if (item.previous) size += item.previous->memory_bytes();
        }
        return size;
    };
    while (m_entries.size() > 1 && bytes() > m_budget) {
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->first != plate && (oldest == m_entries.end() || it->second.touched < oldest->second.touched)) oldest = it;
        m_entries.erase(oldest);
    }
    if (bytes() > m_budget) entry.previous.reset();
}
SliceSnapshotPtr SliceComparisonHistory::current(size_t plate) const
{
    const auto it = m_entries.find(plate);
    return it == m_entries.end() ? nullptr : it->second.current;
}
SliceSnapshotPtr SliceComparisonHistory::previous(size_t plate) const
{
    const auto it = m_entries.find(plate);
    return it == m_entries.end() ? nullptr : it->second.previous;
}
} // namespace Slic3r
