#include "Library.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>

namespace Slic3r::ModelLibrary {
namespace {
std::string text(const Json& row, const std::string& key)
{
    const auto value = row.find(key);
    return value != row.end() && value->is_string() ? value->get<std::string>() : "";
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

bool generic(const std::string& name)
{
    static const std::set<std::string> names{"", "open cascade shape model", "untitled", "model", "part", "part1", "body", "body1", "mesh", "object", "none", "unknown", "noname", "3d model", "default", "scene", "unsaved", "(unsaved)"};
    return names.count(lower(name)) != 0;
}

void relocate(const fs::path& from, const fs::path& to, std::vector<std::pair<fs::path, fs::path>>& moved)
{
    fs::create_directories(to.parent_path());
    fs::rename(from, to);
    moved.emplace_back(from, to);
}

void restore_moves(const std::vector<std::pair<fs::path, fs::path>>& moved)
{
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
        std::error_code error;
        fs::create_directories(it->first.parent_path(), error);
        fs::rename(it->second, it->first, error);
        // Never remove a destination after a failed rollback: it may contain
        // the only surviving copy. The next rescan reports the missing source.
    }
}
} // namespace

Json Library::default_tag_rules()
{
    Json result = Json::array();
    const std::pair<const char*, const char*> rules[] = {
        {"openGrid", R"(\bopen ?grid)"}, {"Gridfinity", "gridfinity"}, {"Multiboard", "multiboard"},
        {"HSW", R"(\bhsw\b|honeycomb storage)"}, {"Skadis", "skadis|skådis"}, {"VESA", R"(\bvesa\b)"},
        {"Rack", R"(\b(\d+x)*\d{1,2}u\b|rack ?(mount|cage|ears?)|server rack|mini ?rack|19 ?(inch|in|\x22) rack)"},
        {"Camera", R"(camera|webcam|\bcam\b|\d+cam\b|gopro|wyze|tripod|lens ?(cap|hood|adapter)|body ?cap|\be ?mount\b|\bdslr\b|shutter)"},
        {"Filament", R"(spool|filament|swatch|dry ?box|\bams( \w+){0,2} (riser|stand|mount|holder|clip|flipper|buffer|feeder|hub|dryer|lid|cover|base|shelf))"},
        {"Printer parts", R"(\b(a1|a1 mini|p1s|p1p|p2s|x1c|x1e|h2d|x2d|ender ?\d|mk[34]s?|voron|prusa|rat ?rig)\b|extruder|hotend|build plate|toolhead|nozzle|\b[xyz] axis\b|ptfe|anti ?vibration|\b3d printers?\b)"},
        {"Test prints", R"(benchy|\b(calibrat\w*|tolerance|temp ?tower|retraction|test)\b|test ?prints?|\bcoupons?|fit (test|sample|check)|speed ?boat)"},
        {"Signs & logos", R"(\blogos?\b|\bsigns?\b|sign ?generator|\bletters?\b|\bicon\b|name ?plate|\blettering\b|\bplaques?\b|\bqr\b|symbol)"},
        {"Terrain & maps", R"(terrain|topo(graph\w*)?\b|\brelief\b|\bmaps?\b|national park)"},
        {"Storage", R"(storage|organi[sz]er|\bbox(es)?\b|\bbins?\b|drawers?|\btote\b|shel(f|ves)|\bcase\b)"},
        {"Mounts", R"(\bmount(s|ing)?\b|bracket|\bholders?\b|\bhooks?\b|\bclips?\b)"},
        {"Cables", R"(cable|\bcords?\b|\bwires?\b|drag ?chain|grommet)"},
        {"Figures", R"(figur(e|ine)|\bbust\b|statue|miniature|dragon|character|meshy)"}
    };
    for (const auto& [tag, pattern] : rules) result.push_back({{"tag", tag}, {"pattern", pattern}, {"enabled", true}});
    return result;
}

void Library::organize()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto config = settings();
    const auto rules = config.value("tag_rules", default_tag_rules());
    std::vector<std::pair<std::string, std::regex>> compiled;
    if (rules.is_array()) for (const auto& rule : rules) {
        if (!rule.value("enabled", true)) continue;
        try { compiled.emplace_back(rule.at("tag").get<std::string>(), std::regex(rule.at("pattern").get<std::string>(), std::regex::icase)); }
        catch (const std::regex_error&) { /* Invalid user rules remain editable. */ }
    }
    const auto all = query("SELECT id,name,designer,source_url,source_site,unstacked,stack_id FROM models ORDER BY added_at,id");
    std::map<std::string, std::vector<Id>> versions;
    for (const auto& model : all) {
        const Id id = model["id"];
        auto url = text(model, "source_url");
        url = url.substr(0, url.find_first_of("?#"));
        auto subject = text(model, "name") + ' ' + url;
        subject = std::regex_replace(subject, std::regex("[-_./+]"), " ");
        const auto entries = files(id);
        std::set<std::string> wanted;
        for (const auto& [tag_name, pattern] : compiled) {
            size_t matched = 0, printable = 0;
            for (const auto& entry : entries) {
                if (entry["kind"] != "model") continue;
                ++printable;
                const auto file_name = std::regex_replace(text(entry, "original_name"), std::regex("[-_./+]"), " ");
                if (std::regex_search(file_name, pattern)) ++matched;
            }
            if (std::regex_search(subject, pattern) || (printable && matched * 2 > printable)) {
                wanted.insert(lower(tag_name));
                tag({id}, tag_name, true, "auto");
            }
        }
        for (const auto& old : query("SELECT t.id,t.name FROM model_tags mt JOIN tags t ON t.id=mt.tag_id WHERE mt.model_id=?1 AND mt.source='auto'", {id}))
            if (!wanted.count(lower(text(old,"name")))) {
                execute("DELETE FROM model_tags WHERE model_id=?1 AND tag_id=?2 AND source='auto'", {id,old["id"]});
                execute("UPDATE models SET ai_tagged_at=NULL WHERE id=?1", {id});
            }
    }
    execute("DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM model_tags)");
    if (!config.value("auto_stack",true)) return;
    const auto candidates = query("SELECT m.*,"
        "(SELECT count(*) FROM files f WHERE f.model_id=m.id AND f.kind='model') AS parts,"
        "(SELECT json_array(f.size_x,f.size_y,f.size_z) FROM files f WHERE f.model_id=m.id AND f.kind='model' AND f.size_x IS NOT NULL ORDER BY f.id LIMIT 1) AS dims "
        "FROM models m WHERE archived_at IS NULL AND unstacked=0 ORDER BY added_at,id");
    const auto same_source = [](const Json& a, const Json& b) {
        return a["source_site"] == b["source_site"] &&
            (a["designer"].is_null() || b["designer"].is_null() || lower(text(a,"designer")) == lower(text(b,"designer")));
    };
    const auto same_part = [&](const Json& a, const Json& b) {
        if (a["parts"].get<int>() > 3 || b["parts"].get<int>() > 3 || !same_source(a,b)) return false;
        if (a["dims"].is_string() && b["dims"].is_string()) {
            const auto x = Json::parse(a["dims"].get<std::string>()), y = Json::parse(b["dims"].get<std::string>());
            if (std::all_of(x.begin(),x.end(),[](const Json& v) { return v.is_number(); }) &&
                std::all_of(y.begin(),y.end(),[](const Json& v) { return v.is_number(); })) {
                auto dx = x.get<std::array<double,3>>(), dy = y.get<std::array<double,3>>();
                std::sort(dx.begin(),dx.end()); std::sort(dy.begin(),dy.end());
                for (size_t i=0;i<3;++i) if (std::min(dx[i],dy[i]) > 0.5 && std::max(dx[i],dy[i])/std::min(dx[i],dy[i]) > 1.5) return false;
            }
        }
        return true;
    };
    std::map<std::string,std::vector<const Json*>> stems, names;
    for (const auto& model : candidates) {
        const auto name = clean_name(text(model,"name")), stem = version_stem(name);
        if (!generic(stem) && (stem.find(' ') != std::string::npos || stem.size() >= 5) && stem.find_first_not_of("0123456789 ") != std::string::npos) stems[stem].push_back(&model);
        if (!generic(name) && !stem.empty() && (stem.find(' ') != std::string::npos || stem.size() > 4)) names[lower(name)].push_back(&model);
    }
    const auto cluster = [&](const auto& buckets, const auto& fits, const std::string& prefix) {
        for (const auto& [key,members] : buckets) {
            std::vector<std::vector<const Json*>> groups;
            for (const auto* member : members) {
                auto found = std::find_if(groups.begin(),groups.end(),[&](const auto& group) { return fits(*group.back(),*member); });
                if (found == groups.end()) groups.push_back({member}); else found->push_back(member);
            }
            for (size_t i=0;i<groups.size();++i) for (const auto* member : groups[i]) versions[prefix+key+':'+std::to_string(i)].push_back((*member)["id"]);
        }
    };
    cluster(stems,same_part,"stem:"); cluster(names,same_source,"name:");
    // Only complete file sets qualify; partial geometry parses leave no hash.
    for (const auto& model : candidates) {
        const auto entries = files(model["id"]);
        std::vector<std::string> shapes;
        bool complete = true;
        for (const auto& entry : entries) if (entry["kind"] == "model") {
            const auto hash = text(entry, "geometry_hash");
            if (hash.empty()) { complete = false; break; }
            shapes.push_back(hash);
        }
        if (complete && !shapes.empty()) {
            std::sort(shapes.begin(), shapes.end());
            std::string signature = "geometry:";
            for (const auto& shape : shapes) signature += shape + ';';
            versions[signature].push_back(model["id"]);
        }
    }
    if (config.value("auto_stack", true)) {
        const auto overlaps = query("SELECT a.id AS a,b.id AS b,count(DISTINCT f.hash) AS shared,"
            "(SELECT count(*) FROM files WHERE model_id=a.id AND kind='model') AS parts_a,"
            "(SELECT count(*) FROM files WHERE model_id=b.id AND kind='model') AS parts_b "
            "FROM files f JOIN files g ON g.hash=f.hash AND g.model_id>f.model_id AND g.kind='model' "
            "JOIN models a ON a.id=f.model_id JOIN models b ON b.id=g.model_id "
            "WHERE f.kind='model' AND a.archived_at IS NULL AND b.archived_at IS NULL AND a.unstacked=0 AND b.unstacked=0 GROUP BY a.id,b.id");
        for (const auto& overlap : overlaps) {
            const auto a = overlap["parts_a"].get<int>(), b = overlap["parts_b"].get<int>(), shared = overlap["shared"].get<int>();
            if (a > 1 && b > 1 && shared >= 2 && shared * 5 >= std::min(a,b) * 3)
                versions["overlap:" + std::to_string(overlap["a"].get<Id>()) + ':' + std::to_string(overlap["b"].get<Id>())] = {overlap["a"],overlap["b"]};
        }
    }
    // Combine overlapping evidence before assigning stacks. Existing manual
    // stacks remain intact; only models without a stack join a component.
    std::map<Id,Id> parents;
    const auto root = [&](Id id) { while (parents.count(id) && parents.at(id) != id) id = parents.at(id); return id; };
    for (const auto& [key,ids] : versions) for (size_t i=1;i<ids.size();++i) {
        const auto a = root(ids.front()), b = root(ids[i]); if (a != b) parents[b] = a;
    }
    std::map<Id,std::vector<Id>> components;
    for (const auto& model : candidates) components[root(model["id"])].push_back(model["id"]);
    for (const auto& [key, ids] : components) {
        if (ids.size() < 2) continue;
        Id group = 0;
        for (Id id : ids) {
            const auto row = query("SELECT stack_id FROM models WHERE id=?1", {id});
            if (!row[0]["stack_id"].is_null()) { group = row[0]["stack_id"]; break; }
        }
        if (!group) group = query("SELECT coalesce(max(stack_id),0)+1 AS id FROM models")[0]["id"];
        for (Id id : ids) execute("UPDATE models SET stack_id=?1 WHERE id=?2 AND unstacked=0 AND stack_id IS NULL", {group, id});
    }
}

Json Library::attention() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Json result = Json::array();
    std::map<std::string, Id> names;
    for (const auto& model : query("SELECT id,name,dismissed FROM models WHERE archived_at IS NULL ORDER BY id")) {
        const Id id = model["id"];
        const auto name = text(model, "name");
        const auto dismissed = text(model, "dismissed");
        const auto add = [&](const std::string& kind, const std::string& suggestion) {
            if (dismissed.find(kind) == std::string::npos) result.push_back({{"model_id", id}, {"name", name}, {"kind", kind}, {"suggestion", suggestion}});
        };
        const auto cleaned = clean_name(name);
        if (cleaned != name) add("name", cleaned);
        if (version_stem(name).size() <= 4 || name == "Untitled") add("vague", "Give this model a more useful name");
        if (!names.emplace(version_stem(name), id).second) add("same-name", "Review models with the same name");
        if (!query("SELECT id FROM files WHERE model_id=?1 AND error IS NOT NULL LIMIT 1", {id}).empty()) add("unreadable", "Review files that could not be read");
        if (!query("SELECT f.id FROM files f JOIN files other ON other.hash=f.hash AND other.model_id<>f.model_id WHERE f.model_id=?1 LIMIT 1", {id}).empty()) add("duplicate", "Review files shared by another model");
    }
    return result;
}

void Library::dismiss(Id model, const std::string& kind)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    execute("UPDATE models SET dismissed=dismissed||' '||?1 WHERE id=?2", {kind, model});
}

void Library::merge(Id target, const std::vector<Id>& others)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto destination = detail(target);
    const auto target_dir = text(destination, "dir");
    std::vector<std::pair<fs::path, fs::path>> moved;
    batch("BEGIN IMMEDIATE;");
    try {
        for (Id id : others) {
            if (id == target) continue;
            const auto model = detail(id);
            const auto old_dir = text(model, "dir");
            const auto child = target_dir + '/' + fs::u8path(old_dir).filename().u8string();
            std::string new_dir = child;
            for (unsigned n = 2; fs::exists(resolve(new_dir)); ++n) new_dir = child + " (" + std::to_string(n) + ')';
            relocate(resolve(old_dir), resolve(new_dir), moved);
            for (const auto& file : model["files"]) {
                const auto old = text(file, "rel_path");
                if (old.rfind(old_dir + '/', 0) != 0) throw std::runtime_error("This pack has an invalid file location");
                const auto replacement = new_dir + old.substr(old_dir.size());
                execute("UPDATE files SET model_id=?1,rel_path=?2 WHERE id=?3", {target, replacement, file["id"]});
                execute("UPDATE models SET thumb=?1 WHERE thumb=?2", {replacement, old});
            }
            execute("INSERT INTO merged_sources(model_id,source_path,source_hash) SELECT ?1,source_path,source_hash FROM models WHERE id=?2", {target, id});
            execute("UPDATE merged_sources SET model_id=?1 WHERE model_id=?2", {target, id});
            execute("INSERT OR IGNORE INTO model_tags(model_id,tag_id,source) SELECT ?1,tag_id,source FROM model_tags WHERE model_id=?2", {target, id});
            execute("INSERT OR IGNORE INTO collection_models(collection_id,model_id,added_at) SELECT collection_id,?1,added_at FROM collection_models WHERE model_id=?2", {target, id});
            execute("UPDATE prints SET model_id=?1 WHERE model_id=?2", {target, id});
            // Merging must not discard annotations or provenance that differ.
            const auto current = detail(target);
            std::string notes = text(current, "notes");
            for (const auto* field : {"notes", "description", "designer", "license", "source_url"}) {
                const auto value = text(model, field);
                if (value.empty() || value == text(current, field)) continue;
                if (std::string(field) != "notes" && text(current, field).empty())
                    execute(std::string("UPDATE models SET ") + field + "=?1 WHERE id=?2", {value, target});
                else notes += "\n\n" + text(model, "name") + " (" + field + ")\n" + value;
            }
            execute("UPDATE models SET notes=?1,favorite=max(favorite,?2) WHERE id=?3", {notes, model.value("favorite", 0), target});
            execute("DELETE FROM models WHERE id=?1", {id});
        }
        execute("UPDATE models SET reshaped=1 WHERE id=?1", {target});
        batch("COMMIT;");
    } catch (...) { batch("ROLLBACK;"); restore_moves(moved); throw; }
}

Id Library::split(Id model, const std::string& folder)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto original = detail(model);
    const auto old_root = text(original, "dir");
    if (folder.empty() || fs::u8path(folder).is_absolute()) throw std::invalid_argument("Choose a folder inside the pack");
    const auto source = resolve(old_root + '/' + folder);
    const auto parent = resolve(old_root);
    auto relative = source.lexically_relative(parent);
    if (relative.empty() || relative == "." || *relative.begin() == ".." || !fs::is_directory(source)) throw std::invalid_argument("Choose a folder inside the pack");
    const auto old_dir = old_root + '/' + relative.generic_u8string();
    const auto name = clean_name(source.filename().u8string());
    std::vector<std::pair<fs::path, fs::path>> moved;
    batch("BEGIN IMMEDIATE;");
    try {
        const auto new_dir = unique_dir(name);
        execute("INSERT INTO models(name,dir,designer,description,license,notes,source_url,source_site,added_at,updated_at,reshaped) SELECT ?1,?2,designer,description,license,notes,source_url,source_site,strftime('%Y-%m-%dT%H:%M:%SZ','now'),strftime('%Y-%m-%dT%H:%M:%SZ','now'),1 FROM models WHERE id=?3", {name, new_dir, model});
        const Id id = sqlite3_last_insert_rowid(m_db);
        size_t count = 0;
        for (const auto& file : original["files"]) {
            const auto path = text(file, "rel_path");
            if (path.rfind(old_dir + '/', 0) != 0) continue;
            const auto replacement = new_dir + path.substr(old_dir.size());
            execute("UPDATE files SET model_id=?1,rel_path=?2 WHERE id=?3", {id, replacement, file["id"]});
            execute("UPDATE models SET thumb=NULL WHERE id=?1 AND thumb=?2", {model, path});
            ++count;
        }
        if (!count) throw std::runtime_error("This folder contains no saved files");
        execute("INSERT INTO model_tags(model_id,tag_id,source) SELECT ?1,tag_id,source FROM model_tags WHERE model_id=?2", {id, model});
        execute("INSERT INTO collection_models(collection_id,model_id,added_at) SELECT collection_id,?1,added_at FROM collection_models WHERE model_id=?2", {id, model});
        execute("UPDATE models SET reshaped=1 WHERE id=?1", {model});
        relocate(source, resolve(new_dir), moved);
        batch("COMMIT;");
        return id;
    } catch (...) { batch("ROLLBACK;"); restore_moves(moved); throw; }
}
} // namespace Slic3r::ModelLibrary
