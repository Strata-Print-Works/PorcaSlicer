#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace Slic3r::ModelLibrary {

using Json = nlohmann::json;
using Id = std::int64_t;
namespace fs = std::filesystem;

struct ImportResult {
    Id model_id = 0;
    std::string status;
    std::string message;
};

// Metadata/thumbnail extraction is supplied by Porca's format readers. The
// catalog and all filesystem operations remain testable without wx or OpenGL.
using Extractor = std::function<Json(const fs::path&, const std::string&, const fs::path&)>;
using Cancel = std::function<bool()>;

class Library final {
public:
    explicit Library(fs::path root);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    const fs::path& root() const { return m_root; }
    fs::path resolve(const std::string& relative) const;
    bool contains(const fs::path& path) const;
    void backup(bool before_upgrade = false);

    Json models(const Json& filter = Json::object()) const;
    Json detail(Id id) const;
    Json files(Id id) const;
    Json facets() const;
    Json activity() const;
    void clear_activity();
    Json settings() const;
    void set_setting(const std::string& key, const Json& value);
    void update(Id id, const Json& fields);
    void rename(const std::vector<Id>& ids, const std::string& find, const std::string& replacement);
    void tag(const std::vector<Id>& ids, const std::string& name, bool on, const std::string& source = "user");
    void archive(const std::vector<Id>& ids, bool archived);
    void stack(const std::vector<Id>& ids);
    void unstack(const std::vector<Id>& ids);
    void set_stack_top(Id id);
    void set_cover(Id model, Id file);

    Json collections() const;
    Id create_collection(const std::string& name, const std::string* query = nullptr);
    void update_collection(Id id, const std::string& name);
    void delete_collection(Id id);
    void collect(Id collection, const std::vector<Id>& models, bool on);
    Json prints(Id model) const;
    Id record_print(Id model, const std::string& outcome, const std::string& notes);
    void update_print(Id id, const std::string& outcome, const std::string& notes);
    void delete_print(Id id);

    Json ai_vocabulary() const;
    void save_ai_tags(const std::vector<Id>& asked, const Json& answers);
    void save_page_details(Id id, const Json& details);
    void save_page_photo(Id id, unsigned index, const std::string& bytes);

    Json watch_folders() const;
    Id add_watch(const fs::path& path, bool recursive);
    void update_watch(Id id, bool enabled, bool recursive);
    void remove_watch(Id id);
    std::vector<fs::path> scan_items(const fs::path& path, bool folders) const;
    void mark_scanned(Id id);

    ImportResult import(const fs::path& path, const Extractor& extract = {}, const Cancel& cancel = {});
    Json plan_organized(const fs::path& path, const Cancel& cancel = {}) const;
    Json import_organized(const fs::path& path, const std::vector<std::string>& categories,
                          const Extractor& extract = {}, const Cancel& cancel = {});
    void restore_pack_layout(Id model, const Cancel& cancel = {});
    void rescan(Id model, const Extractor& extract, const Cancel& cancel = {});
    void repair_source_links(const Cancel& cancel = {});
    bool backfill_geometry(const Extractor& extract, const Cancel& cancel = {});
    void organize();
    Json attention() const;
    void dismiss(Id model, const std::string& kind);
    void merge(Id target, const std::vector<Id>& others);
    Id split(Id model, const std::string& folder);

    // Trash is provided by the platform. Rows are removed only after a confirmed
    // move; callers never implement a destructive unlink fallback.
    void trash_models(const std::vector<Id>& ids, const std::function<bool(const fs::path&)>& trash);
    Json leftovers(const Cancel& cancel = {}) const;
    void trash_leftovers(const std::vector<fs::path>& paths,
                         const std::function<bool(const fs::path&)>& trash, const Cancel& cancel = {});

    static std::string hash_file(const fs::path& path, const Cancel& cancel = {});
    static std::string file_kind(const fs::path& path);
    static std::string format(const fs::path& path);
    static std::string clean_name(const std::string& name);
    static std::string version_stem(const std::string& name);
    static Json default_tag_rules();

private:
    fs::path m_root;
    sqlite3* m_db = nullptr;
    mutable std::recursive_mutex m_mutex;
    std::set<Id> m_geometry_attempted;

    Json query(const std::string& sql, const Json& args = Json::array()) const;
    void execute(const std::string& sql, const Json& args = Json::array()) const;
    void batch(const std::string& sql) const;
    void log(const fs::path& path, const ImportResult& result);
    bool fully_present(const fs::path& path, const Cancel& cancel) const;
    std::string unique_dir(const std::string& name) const;
};

// SQL generated here contains only fixed clauses; all user values are bound.
std::string search_clause(const std::string& query, Json& args, bool& archived);

} // namespace Slic3r::ModelLibrary
