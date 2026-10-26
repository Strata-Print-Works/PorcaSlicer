#include "Library.hpp"
#include "Schema.hpp"

#include <sqlite3.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Slic3r::ModelLibrary {
namespace {
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::string text(const Json& row, const std::string& key)
{
    const auto found = row.find(key);
    return found != row.end() && found->is_string() ? found->get<std::string>() : std::string();
}

struct Statement {
    sqlite3_stmt* stmt = nullptr;
    Statement(sqlite3* db, const std::string& sql, const Json& args)
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        for (size_t i = 0; i < args.size(); ++i) {
            const auto& v = args[i];
            int rc;
            if (v.is_null()) rc = sqlite3_bind_null(stmt, int(i + 1));
            else if (v.is_boolean()) rc = sqlite3_bind_int(stmt, int(i + 1), v.get<bool>());
            else if (v.is_number_integer()) rc = sqlite3_bind_int64(stmt, int(i + 1), v.get<Id>());
            else if (v.is_number()) rc = sqlite3_bind_double(stmt, int(i + 1), v.get<double>());
            else {
                const auto s = v.is_string() ? v.get<std::string>() : v.dump();
                rc = sqlite3_bind_text(stmt, int(i + 1), s.data(), int(s.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) { sqlite3_finalize(stmt); stmt = nullptr; throw std::runtime_error(sqlite3_errmsg(db)); }
        }
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

constexpr const char* timestamp = "strftime('%Y-%m-%dT%H:%M:%SZ','now')";

void check_outcome(const std::string& outcome)
{
    if (outcome != "good" && outcome != "meh" && outcome != "failed") throw std::invalid_argument("Unknown print outcome");
}

std::string safe_name(std::string name)
{
    for (char& c : name) if (static_cast<unsigned char>(c) < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty() || name == "." || name == "..") name = "Untitled";
    // Windows device names are reserved even when they have an extension.
    const auto stem = lower(fs::u8path(name).stem().u8string());
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        std::regex_match(stem, std::regex("(com|lpt)[0-9]"))) name = '_' + name;
    if (name.size() > 160) {
        name.resize(160);
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xc0) == 0x80) name.pop_back();
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xc0) name.pop_back();
    }
    return name;
}
} // namespace

Library::Library(fs::path root)
{
    fs::create_directories(root);
    m_root = fs::canonical(root);
    // A portable library must not redirect writes into a different directory.
    for (const auto* child : {"library.db", "models", "thumbs", "backups"})
        if (fs::is_symlink(m_root / child)) throw std::runtime_error("Choose a library whose files and folders are stored together, without links");
    const bool existed = fs::exists(m_root / "library.db");
    const int rc = sqlite3_open_v2((m_root / "library.db").u8string().c_str(), &m_db,
                                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string error = m_db ? sqlite3_errmsg(m_db) : "Could not open the library";
        if (m_db) sqlite3_close(m_db);
        m_db = nullptr;
        throw std::runtime_error(error);
    }
    try {
        sqlite3_busy_timeout(m_db, 3000);
        const auto version = query("PRAGMA user_version")[0]["user_version"].get<size_t>();
        if (version > migrations.size()) throw std::runtime_error("This library needs a newer version of Porca. Its files have not been changed.");
        batch("PRAGMA foreign_keys=ON;");
        if (existed && version < migrations.size()) backup(true);
        for (size_t i = version; i < migrations.size(); ++i) {
            batch("BEGIN IMMEDIATE;");
            try { batch(std::string(migrations[i]) + "PRAGMA user_version=" + std::to_string(i + 1) + ";COMMIT;"); }
            catch (...) { batch("ROLLBACK;"); throw; }
        }
        batch("PRAGMA journal_mode=WAL;");
        fs::create_directories(m_root / "models");
        fs::create_directories(m_root / "thumbs");
        backup();
    } catch (...) { sqlite3_close(m_db); m_db = nullptr; throw; }
}

Library::~Library() { if (m_db) sqlite3_close(m_db); }

Json Library::query(const std::string& sql, const Json& args) const
{
    // A brace-wrapped Json scalar can select Json's copy constructor instead
    // of its initializer-list constructor. Normalize that single argument.
    const Json parameters = args.is_array() ? args : Json::array({args});
    Statement statement(m_db, sql, parameters);
    Json result = Json::array();
    int rc;
    while ((rc = sqlite3_step(statement.stmt)) == SQLITE_ROW) {
        Json row = Json::object();
        for (int col = 0; col < sqlite3_column_count(statement.stmt); ++col) {
            const char* key = sqlite3_column_name(statement.stmt, col);
            switch (sqlite3_column_type(statement.stmt, col)) {
            case SQLITE_INTEGER: row[key] = sqlite3_column_int64(statement.stmt, col); break;
            case SQLITE_FLOAT: row[key] = sqlite3_column_double(statement.stmt, col); break;
            case SQLITE_TEXT: row[key] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement.stmt, col)), size_t(sqlite3_column_bytes(statement.stmt, col))); break;
            default: row[key] = nullptr; break;
            }
        }
        result.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(m_db));
    return result;
}

void Library::execute(const std::string& sql, const Json& args) const { (void)query(sql, args); }
void Library::batch(const std::string& sql) const
{
    char* message = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string error = message ? message : sqlite3_errmsg(m_db);
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}

fs::path Library::resolve(const std::string& relative) const
{
    const fs::path path = fs::u8path(relative);
    if (path.empty() || path.is_absolute() || path.has_root_name() || relative.find('\\') != std::string::npos)
        throw std::runtime_error("The library contains an invalid file location");
    for (const auto& part : path) if (part == "..") throw std::runtime_error("The library contains an invalid file location");
    auto component = m_root;
    for (const auto& part : path) {
        component /= part;
        if (fs::is_symlink(component)) throw std::runtime_error("A library file location was replaced with a link");
    }
    const auto result = fs::weakly_canonical(m_root / path);
    if (!contains(result)) throw std::runtime_error("This file is outside the library");
    return result;
}

bool Library::contains(const fs::path& path) const
{
    const auto canonical = fs::weakly_canonical(path);
    auto a = m_root.begin(), b = canonical.begin();
    for (; a != m_root.end(); ++a, ++b) if (b == canonical.end() || *a != *b) return false;
    return true;
}

void Library::backup(bool before_upgrade)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    fs::create_directories(m_root / "backups");
    const std::string date = query("SELECT strftime('%Y-%m-%d','now') AS date")[0]["date"];
    const std::string prefix = before_upgrade ? "pre-v6-" : "daily-";
    auto destination = m_root / "backups" / (prefix + date + ".db");
    if (fs::exists(destination)) return;
    const auto pending = fs::path(destination.u8string() + ".partial");
    sqlite3* copy = nullptr;
    if (sqlite3_open(pending.u8string().c_str(), &copy) != SQLITE_OK) {
        if (copy) sqlite3_close(copy);
        throw std::runtime_error("Could not back up the library");
    }
    auto* handle = sqlite3_backup_init(copy, "main", m_db, "main");
    const int rc = handle ? sqlite3_backup_step(handle, -1) : SQLITE_ERROR;
    const int finish = handle ? sqlite3_backup_finish(handle) : SQLITE_ERROR;
    sqlite3_close(copy);
    if (rc != SQLITE_DONE || finish != SQLITE_OK) { fs::remove(pending); throw std::runtime_error("Could not back up the library"); }
    fs::rename(pending, destination);
    if (!before_upgrade) {
        std::vector<fs::path> daily;
        for (const auto& entry : fs::directory_iterator(m_root / "backups"))
            if (entry.is_regular_file() && entry.path().filename().u8string().rfind("daily-", 0) == 0 && entry.path().extension() == ".db") daily.push_back(entry.path());
        std::sort(daily.begin(), daily.end());
        while (daily.size() > 14) { fs::remove(daily.front()); daily.erase(daily.begin()); }
    }
}

Json Library::models(const Json& filter) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Json args = Json::array();
    std::string sql = R"sql(SELECT m.*,
      (SELECT count(*) FROM files f WHERE f.model_id=m.id AND f.kind IN ('model','source')) AS file_count,
      (SELECT coalesce(sum(f.size),0) FROM files f WHERE f.model_id=m.id) AS total_size,
      (SELECT coalesce(sum(f.triangles),0) FROM files f WHERE f.model_id=m.id) AS triangles,
      (SELECT group_concat(DISTINCT f.format) FROM files f WHERE f.model_id=m.id AND f.kind IN ('model','source')) AS formats,
      (SELECT count(*) FROM prints p WHERE p.model_id=m.id) AS print_count,
      (SELECT max(p.at) FROM prints p WHERE p.model_id=m.id) AS last_printed,
      (SELECT count(*) FROM models s WHERE s.stack_id=m.stack_id AND s.archived_at IS NULL) AS stack_size,
      (SELECT s.id FROM models s WHERE s.stack_id=m.stack_id AND s.archived_at IS NULL ORDER BY s.stack_top DESC,s.added_at DESC,s.id DESC LIMIT 1) AS stack_top_id,
      (SELECT group_concat(t.name,char(31)) FROM model_tags mt JOIN tags t ON t.id=mt.tag_id WHERE mt.model_id=m.id AND mt.source<>'hidden') AS tags
      FROM models m WHERE 1=1)sql";
    std::string search = filter.value("search", "");
    if (filter.contains("collection")) {
        const auto collection = query("SELECT query FROM collections WHERE id=?1", {filter["collection"]});
        if (collection.empty()) sql += " AND 0";
        else if (collection[0]["query"].is_string()) search = collection[0]["query"].get<std::string>() + ' ' + search;
        else { args.push_back(filter["collection"]); sql += " AND EXISTS (SELECT 1 FROM collection_models c WHERE c.model_id=m.id AND c.collection_id=?" + std::to_string(args.size()) + ")"; }
    }
    bool archived = filter.value("archived", false);
    sql += search_clause(search, args, archived);
    sql += archived ? " AND m.archived_at IS NOT NULL" : " AND m.archived_at IS NULL";
    for (const auto& key : {"id", "stack_id", "designer"}) if (filter.contains(key)) {
        args.push_back(filter[key]); sql += " AND m." + std::string(key) + "=?" + std::to_string(args.size());
    }
    if (filter.value("favorites", false)) sql += " AND m.favorite=1";
    if (filter.value("printed", false)) sql += " AND EXISTS (SELECT 1 FROM prints p WHERE p.model_id=m.id)";
    if (filter.contains("tag")) { args.push_back(filter["tag"]); sql += " AND EXISTS (SELECT 1 FROM model_tags mt JOIN tags t ON t.id=mt.tag_id WHERE mt.model_id=m.id AND mt.source<>'hidden' AND t.name=?" + std::to_string(args.size()) + ")"; }
    const std::map<std::string, std::string> sort{{"name", "m.name COLLATE NOCASE"}, {"size", "total_size"}, {"triangles", "triangles"}, {"printed", "last_printed"}, {"added", "m.added_at"}};
    const auto order = sort.find(filter.value("sort", "added"));
    const std::string direction = filter.value("descending", true) ? " DESC" : " ASC";
    sql += " ORDER BY " + (order == sort.end() ? "m.added_at" : order->second) + direction + ",m.id" + direction;
    auto rows = query(sql, args);
    if (filter.value("families", false)) {
        // Families are based on the whole active catalog, so filtering a family
        // down to one result does not change its label. A version stack counts
        // as one design when deciding whether a shared name is a family.
        struct Member { Id id; Id unit; std::vector<std::string> words; };
        std::map<std::string,std::vector<Member>> groups;
        const std::set<std::string> skip{"the","and","for","with","my","new","simple","custom","mini","small","big","large","easy","printable","printed","bambu","bambulab","model","part","test"};
        for (const auto& model : query("SELECT id,name,stack_id FROM models WHERE archived_at IS NULL")) {
            std::istringstream input(version_stem(text(model,"name")));
            std::vector<std::string> words;
            for (std::string word; input >> word;) words.push_back(std::move(word));
            const auto start = std::find_if(words.begin(),words.end(),[&](const std::string& word) {
                return word.size() >= 3 && !skip.count(word) && word.find_first_not_of("0123456789") != std::string::npos;
            });
            if (start == words.end()) continue;
            words.erase(words.begin(),start);
            const Id id = model["id"];
            const Id unit = model["stack_id"].is_null() ? -id : model["stack_id"].get<Id>();
            const auto key = words.front();
            groups[key].push_back({id,unit,std::move(words)});
        }
        std::map<Id,std::string> labels;
        for (const auto& [key,members] : groups) {
            std::set<Id> units; for (const auto& member : members) units.insert(member.unit);
            if (units.size() < 3) continue;
            size_t shared = std::min(size_t(3),members.front().words.size());
            for (const auto& member : members) {
                shared = std::min(shared,member.words.size());
                size_t same = 0;
                while (same < shared && member.words[same] == members.front().words[same]) ++same;
                shared = same;
            }
            std::string label;
            for (size_t i=0;i<shared;++i) { if (i) label += ' '; label += members.front().words[i]; }
            if (!label.empty()) label.front() = char(std::toupper(static_cast<unsigned char>(label.front())));
            for (const auto& member : members) labels[member.id] = label;
        }
        for (auto& row : rows) row["family"] = labels[row["id"].get<Id>()];
    }
    if (filter.value("collapse_stacks", false) && !filter.contains("stack_id")) {
        // A filtered-out top must not hide all matching versions of a stack.
        std::map<Id, size_t> seen;
        Json collapsed = Json::array();
        for (auto& row : rows) {
            if (row["stack_id"].is_number_integer()) {
                const Id stack = row["stack_id"];
                const auto found = seen.find(stack);
                if (found != seen.end()) {
                    if (row["id"] == row["stack_top_id"]) collapsed[found->second] = std::move(row);
                    continue;
                }
                seen[stack] = collapsed.size();
            }
            collapsed.push_back(std::move(row));
        }
        return collapsed;
    }
    return rows;
}

Json Library::detail(Id id) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto rows = query("SELECT * FROM models WHERE id=?1", {id});
    if (rows.empty()) throw std::runtime_error("This model is no longer in the library");
    auto result = rows[0];
    result["files"] = files(id);
    result["tags"] = query("SELECT t.name,mt.source FROM model_tags mt JOIN tags t ON t.id=mt.tag_id WHERE mt.model_id=?1 AND mt.source<>'hidden' ORDER BY t.name COLLATE NOCASE", {id});
    result["prints"] = prints(id);
    result["collections"] = query("SELECT c.* FROM collections c JOIN collection_models cm ON cm.collection_id=c.id WHERE cm.model_id=?1", {id});
    if (!result["stack_id"].is_null()) result["versions"] = query("SELECT id,name,stack_top FROM models WHERE stack_id=?1 ORDER BY stack_top DESC,added_at DESC,id DESC", {result["stack_id"]});
    else result["versions"] = Json::array();
    return result;
}

Json Library::files(Id id) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto rows = query("SELECT * FROM files WHERE model_id=?1 ORDER BY rel_path COLLATE NOCASE", {id});
    for (auto& row : rows) {
        auto meta = Json::parse(text(row, "meta"), nullptr, false);
        row["meta"] = meta.is_array() ? std::move(meta) : Json::array();
    }
    return rows;
}

Json Library::facets() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return {
        {"tags", query("SELECT t.name,count(*) AS count FROM tags t JOIN model_tags mt ON mt.tag_id=t.id JOIN models m ON m.id=mt.model_id WHERE mt.source<>'hidden' AND m.archived_at IS NULL GROUP BY t.id ORDER BY t.name COLLATE NOCASE")},
        {"designers", query("SELECT designer AS name,count(*) AS count FROM models WHERE archived_at IS NULL AND designer IS NOT NULL AND designer<>'' GROUP BY designer ORDER BY designer COLLATE NOCASE")},
        {"sources", query("SELECT coalesce(source_site,'Local') AS name,count(*) AS count FROM models WHERE archived_at IS NULL GROUP BY source_site ORDER BY name COLLATE NOCASE")},
        {"formats", query("SELECT format AS name,count(DISTINCT model_id) AS count FROM files WHERE kind IN ('model','source') GROUP BY format ORDER BY format")}
    };
}

Json Library::activity() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return query("SELECT * FROM activity ORDER BY id DESC LIMIT 500"); }
void Library::clear_activity() { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("DELETE FROM activity"); }
void Library::log(const fs::path& path, const ImportResult& result)
{
    execute(std::string("INSERT INTO activity(at,path,status,message,model_id) VALUES(") + timestamp + ",?1,?2,?3,?4)", {path.u8string(), result.status, result.message, result.model_id ? Json(result.model_id) : Json()});
}

Json Library::settings() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Json values = Json::object();
    for (const auto& row : query("SELECT key,value FROM settings")) {
        const std::string raw = row["value"];
        auto parsed = Json::parse(raw, nullptr, false);
        values[row["key"].get<std::string>()] = parsed.is_discarded() ? Json(raw) : parsed;
    }
    return values;
}

void Library::set_setting(const std::string& key, const Json& value)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    execute("INSERT INTO settings(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value", {key, value.is_string() ? value.get<std::string>() : value.dump()});
}

void Library::update(Id id, const Json& fields)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const std::set<std::string> allowed{"name", "designer", "notes", "description", "source_url", "source_site", "license", "favorite"};
    Json args = {id};
    std::string sql = std::string("UPDATE models SET updated_at=") + timestamp;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (!allowed.count(it.key())) throw std::invalid_argument("Unsupported model field");
        if (it.key() == "name" && (!it.value().is_string() || it.value().get<std::string>().empty())) throw std::invalid_argument("Give the model a name");
        args.push_back(it.value()); sql += ',' + it.key() + "=?" + std::to_string(args.size());
    }
    execute(sql + " WHERE id=?1", args);
}

void Library::rename(const std::vector<Id>& ids, const std::string& find, const std::string& replacement)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (find.empty()) return;
    batch("BEGIN IMMEDIATE;");
    try {
        for (Id id : ids) {
            auto name = text(detail(id), "name");
            for (size_t at = 0; (at = name.find(find, at)) != std::string::npos; at += replacement.size()) name.replace(at, find.size(), replacement);
            update(id, {{"name", name}});
        }
        batch("COMMIT;");
    } catch (...) { batch("ROLLBACK;"); throw; }
}

void Library::tag(const std::vector<Id>& ids, const std::string& name, bool on, const std::string& source)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (name.empty()) return;
    if (source != "user" && source != "auto" && source != "ai") throw std::invalid_argument("Unknown tag source");
    execute("INSERT OR IGNORE INTO tags(name) VALUES(?1)", {name});
    const Id tag_id = query("SELECT id FROM tags WHERE name=?1 COLLATE NOCASE", {name})[0]["id"];
    for (Id id : ids) {
        if (on) {
            if (source == "user") execute("INSERT INTO model_tags(model_id,tag_id,source) VALUES(?1,?2,'user') ON CONFLICT(model_id,tag_id) DO UPDATE SET source='user'", {id, tag_id});
            else execute("INSERT OR IGNORE INTO model_tags(model_id,tag_id,source) VALUES(?1,?2,?3)", {id, tag_id, source});
        } else execute("INSERT INTO model_tags(model_id,tag_id,source) VALUES(?1,?2,'hidden') ON CONFLICT(model_id,tag_id) DO UPDATE SET source='hidden'", {id, tag_id});
    }
}

void Library::archive(const std::vector<Id>& ids, bool archived)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (Id id : ids) execute("UPDATE models SET archived_at=" + std::string(archived ? timestamp : "NULL") + " WHERE id=?1", {id});
}

void Library::stack(const std::vector<Id>& ids)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (ids.size() < 2) return;
    const Id group = query("SELECT coalesce(max(stack_id),0)+1 AS id FROM models")[0]["id"];
    batch("BEGIN IMMEDIATE;");
    try {
        for (Id id : ids) execute("UPDATE models SET stack_id=?1,stack_top=?2,unstacked=1 WHERE id=?3", {group, id == ids.front(), id});
        batch("COMMIT;");
    } catch (...) { batch("ROLLBACK;"); throw; }
}
void Library::unstack(const std::vector<Id>& ids)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (Id id : ids) execute("UPDATE models SET stack_id=NULL,stack_top=0,unstacked=1 WHERE id=?1", {id});
}
void Library::set_stack_top(Id id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    execute("UPDATE models SET stack_top=(id=?1) WHERE stack_id=(SELECT stack_id FROM models WHERE id=?1)", {id});
}
void Library::set_cover(Id model, Id file)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto rows = query("SELECT rel_path,thumb,kind FROM files WHERE id=?1 AND model_id=?2", {file, model});
    if (rows.empty()) throw std::runtime_error("This file is no longer available");
    const auto cover = rows[0]["kind"] == "image" ? rows[0]["rel_path"] : rows[0]["thumb"];
    if (!cover.is_null()) execute("UPDATE models SET thumb=?1 WHERE id=?2", {cover, model});
}

Json Library::collections() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return query("SELECT c.*,(SELECT count(*) FROM collection_models cm WHERE cm.collection_id=c.id) AS count FROM collections c ORDER BY name COLLATE NOCASE"); }
Id Library::create_collection(const std::string& name, const std::string* search)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (name.empty()) throw std::invalid_argument("Give the collection a name");
    execute(std::string("INSERT INTO collections(name,query,created_at) VALUES(?1,?2,") + timestamp + ")", {name, search ? Json(*search) : Json()});
    return sqlite3_last_insert_rowid(m_db);
}
void Library::update_collection(Id id, const std::string& name) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("UPDATE collections SET name=?1 WHERE id=?2", {name, id}); }
void Library::delete_collection(Id id) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("DELETE FROM collections WHERE id=?1", {id}); }
void Library::collect(Id collection, const std::vector<Id>& models, bool on)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (Id id : models) {
        if (on) execute(std::string("INSERT OR IGNORE INTO collection_models(collection_id,model_id,added_at) VALUES(?1,?2,") + timestamp + ")", {collection, id});
        else execute("DELETE FROM collection_models WHERE collection_id=?1 AND model_id=?2", {collection, id});
    }
}
Json Library::prints(Id model) const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return query("SELECT * FROM prints WHERE model_id=?1 ORDER BY at DESC,id DESC", {model}); }
Id Library::record_print(Id model, const std::string& outcome, const std::string& notes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    check_outcome(outcome);
    Json printer, filament, file;
    auto entries = files(model);
    std::stable_sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) { return a.value("sliced", 0) > b.value("sliced", 0); });
    for (const auto& entry : entries) for (const auto& meta : entry["meta"]) {
        if (meta.value("group", "") != "Print") continue;
        if (printer.is_null() && meta.value("key", "") == "Printer") { printer = meta["value"]; file = entry["original_name"]; }
        if (filament.is_null() && meta.value("key", "") == "Filament") { filament = meta["value"]; if (file.is_null()) file = entry["original_name"]; }
    }
    execute(std::string("INSERT INTO prints(model_id,file_name,at,outcome,printer,filament,notes) VALUES(?1,?2,") + timestamp + ",?3,?4,?5,?6)", {model, file, outcome, printer, filament, notes});
    return sqlite3_last_insert_rowid(m_db);
}
void Library::update_print(Id id, const std::string& outcome, const std::string& notes) { std::lock_guard<std::recursive_mutex> lock(m_mutex); check_outcome(outcome); execute("UPDATE prints SET outcome=?1,notes=?2 WHERE id=?3", {outcome, notes, id}); }
void Library::delete_print(Id id) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("DELETE FROM prints WHERE id=?1", {id}); }

Json Library::watch_folders() const { std::lock_guard<std::recursive_mutex> lock(m_mutex); return query("SELECT * FROM watch_folders ORDER BY path"); }
Id Library::add_watch(const fs::path& path, bool recursive)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto canonical = fs::canonical(path);
    if (!fs::is_directory(canonical) || contains(canonical)) throw std::invalid_argument("Choose a folder outside the library");
    execute(std::string("INSERT INTO watch_folders(path,recursive,enabled,added_at) VALUES(?1,?2,1,") + timestamp + ") ON CONFLICT(path) DO UPDATE SET recursive=excluded.recursive,enabled=1", {canonical.u8string(), recursive});
    return query("SELECT id FROM watch_folders WHERE path=?1", {canonical.u8string()})[0]["id"];
}
void Library::update_watch(Id id, bool enabled, bool recursive) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("UPDATE watch_folders SET enabled=?1,recursive=?2 WHERE id=?3", {enabled, recursive, id}); }
void Library::remove_watch(Id id) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute("DELETE FROM watch_folders WHERE id=?1", {id}); }
void Library::mark_scanned(Id id) { std::lock_guard<std::recursive_mutex> lock(m_mutex); execute(std::string("UPDATE watch_folders SET last_scan_at=") + timestamp + " WHERE id=?1", {id}); }

std::string Library::format(const fs::path& path)
{
    auto ext = lower(path.extension().u8string());
    if (!ext.empty()) ext.erase(0, 1);
    if (ext == "stp") ext = "step";
    return ext;
}
std::string Library::file_kind(const fs::path& path)
{
    const auto ext = format(path);
    static const std::set<std::string> models{"3mf", "stl", "obj", "ply", "amf", "step", "gcode", "bgcode"};
    static const std::set<std::string> sources{"scad", "f3d", "sldprt", "svg", "dxf", "fcstd", "blend", "iges", "igs", "3dm", "ipt"};
    static const std::set<std::string> images{"png", "jpg", "jpeg", "webp", "gif", "bmp"};
    static const std::set<std::string> docs{"pdf", "txt", "md", "html", "htm", "rtf", "doc", "docx"};
    static const std::set<std::string> extras{"py", "json", "zip", "7z", "rar", "tar", "gz", "mtl", "ini", "csv", "yaml", "yml"};
    if (models.count(ext)) return "model";
    if (sources.count(ext)) return "source";
    if (images.count(ext)) return "image";
    const auto name = lower(path.filename().u8string());
    if (docs.count(ext) || name == "readme" || name == "license" || name == "licence" || name == "copying") return "doc";
    if (extras.count(ext)) return "source";
    return {};
}

std::string Library::hash_file(const fs::path& path, const Cancel& cancel)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read " + path.filename().u8string());
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<char, 65536> buffer{};
    while (input) {
        if (cancel && cancel()) throw std::runtime_error("Cancelled");
        input.read(buffer.data(), buffer.size());
        blake3_hasher_update(&hasher, buffer.data(), size_t(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Could not finish reading " + path.filename().u8string());
    std::array<unsigned char, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned char c : digest) result << std::setw(2) << unsigned(c);
    return result.str();
}

std::string Library::clean_name(const std::string& name)
{
    std::string clean = std::regex_replace(name, std::regex(R"(\.(stl|3mf|obj|step|stp|ply|amf|gcode)\b(\s\d{1,3}\b)?)", std::regex::icase), "");
    clean = std::regex_replace(clean, std::regex(R"([\s_-]+(stls?|3mf|step|obj|(print|model)[\s_-]?files|files)$)", std::regex::icase), "");
    clean = std::regex_replace(clean, std::regex(R"(\s*\((dup)?\d{1,3}\)$)", std::regex::icase), "");
    clean = std::regex_replace(clean, std::regex(R"(\s+)"), " ");
    const auto first = clean.find_first_not_of(" -_\t\r\n");
    return first == std::string::npos ? name : clean.substr(first, clean.find_last_not_of(" -_\t\r\n") - first + 1);
}
std::string Library::version_stem(const std::string& name)
{
    auto stem = lower(clean_name(name));
    stem = std::regex_replace(stem, std::regex("[^a-z0-9]+"), " ");
    stem = std::regex_replace(stem, std::regex(R"(\b([a-z]{3,})([0-9]))"), "$1 $2");
    stem = std::regex_replace(stem, std::regex(R"(\b(copy|new|old|final|fixed|fix|wip|alt|updated|update|edit|edited|version|print|printready|painted|colored|coloured|color|colour|export|exported|arranged|stacked|stack|bambu|stl|3mf|step|stp|obj|gcode|files|v[0-9]+[a-z]?|ver[0-9]+|r[0-9]+|x[0-9]+|rev([a-z]|[0-9]+)?)\b)"), "");
    stem = std::regex_replace(stem, std::regex(R"(\s+[0-9]{1,3}\s*$)"), "");
    return clean_name(stem);
}

std::string Library::unique_dir(const std::string& name) const
{
    const std::string stem = "models/" + safe_name(name);
    std::string candidate = stem;
    for (unsigned n = 2; fs::exists(resolve(candidate)) || !query("SELECT id FROM models WHERE dir=?1", {candidate}).empty(); ++n) candidate = stem + " (" + std::to_string(n) + ')';
    return candidate;
}
} // namespace Slic3r::ModelLibrary
