#include "Library.hpp"
#include "Source.hpp"

#include <sqlite3.h>
#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Slic3r::ModelLibrary {
namespace {
constexpr const char* timestamp = "strftime('%Y-%m-%dT%H:%M:%SZ','now')";
void cancelled(const Cancel& cancel) { if (cancel && cancel()) throw std::runtime_error("Cancelled"); }

bool ignored(const fs::path& path)
{
    const auto name = path.filename().u8string();
    return name.empty() || name[0] == '.' || name == "node_modules" || name == "__MACOSX";
}

std::string get_text(const Json& object, const std::string& key)
{
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : "";
}

struct TemporaryDirectory {
    fs::path path;
    explicit TemporaryDirectory(const fs::path& parent)
    {
        static std::atomic<unsigned long long> sequence{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = parent / (".porca-import-" + std::to_string(stamp) + '-' + std::to_string(sequence++));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("Could not prepare the import");
    }
    ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};

struct Archive {
    mz_zip_archive zip{};
    explicit Archive(const fs::path& path)
    {
        if (!mz_zip_reader_init_file(&zip, path.u8string().c_str(), 0)) throw std::runtime_error("Could not read this ZIP archive");
    }
    ~Archive() { mz_zip_reader_end(&zip); }
};

void unpack(const fs::path& archive, const fs::path& destination, const Cancel& cancel, bool recognized_only = true)
{
    Archive input(archive);
    const auto count = mz_zip_reader_get_num_files(&input.zip);
    if (count > 100000) throw std::runtime_error("This archive has too many files");
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        cancelled(cancel);
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&input.zip, i, &stat)) throw std::runtime_error("Could not read an archive entry");
        if (mz_zip_reader_is_file_a_directory(&input.zip, i)) continue;
        const std::string name = stat.m_filename;
        const auto path = fs::u8path(name);
        if (name.empty() || path.is_absolute() || path.has_root_name() || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
            throw std::runtime_error("This archive contains an unsafe file location");
        bool skip = false;
        for (const auto& part : path) {
            if (part == "..") throw std::runtime_error("This archive contains an unsafe file location");
            if (ignored(part)) skip = true;
        }
        if (recognized_only && (skip || Library::file_kind(path).empty())) continue;
        total += stat.m_uncomp_size;
        if (stat.m_uncomp_size > 4ULL * 1024 * 1024 * 1024 || total > 16ULL * 1024 * 1024 * 1024)
            throw std::runtime_error("This archive exceeds the import size limit");
        const auto target = destination / path;
        if (fs::exists(target)) throw std::runtime_error("This archive contains repeated file names");
        fs::create_directories(target.parent_path());
        if (!mz_zip_reader_extract_to_file(&input.zip, i, target.u8string().c_str(), 0)) throw std::runtime_error("Could not extract " + name);
    }
}

std::vector<fs::path> contents(const fs::path& root, bool recognized_only, const Cancel& cancel)
{
    std::vector<fs::path> result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        cancelled(cancel);
        if (it->is_symlink()) { it.disable_recursion_pending(); if (!recognized_only) throw std::runtime_error("A linked file must stay in its original folder"); continue; }
        if (recognized_only && ignored(it->path())) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file() && (!recognized_only || !Library::file_kind(it->path()).empty())) result.push_back(it->path());
        if (result.size() > 100000) throw std::runtime_error("This folder has too many files");
    }
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace

std::vector<fs::path> Library::scan_items(const fs::path& path, bool folders) const
{
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(path)) {
        if (ignored(entry.path()) || entry.is_symlink() || contains(entry.path())) continue;
        if (entry.is_directory()) {
            if (folders) result.push_back(entry.path());
        } else if (entry.is_regular_file() && !file_kind(entry.path()).empty()) result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

ImportResult Library::import(const fs::path& requested, const Extractor& extract, const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    ImportResult result;
    try {
        cancelled(cancel);
        if (fs::is_symlink(requested)) throw std::runtime_error("Import the original file instead of its link");
        const auto source = fs::canonical(requested);
        if (contains(source)) { result.status = "skipped"; result.message = "Already inside the library"; log(requested, result); return result; }
        const auto origin = download_origin(source);
        const bool directory = fs::is_directory(source);
        const bool archive = !directory && format(source) == "zip" && settings().value("extract_zips",true);
        if (!directory && !archive && file_kind(source).empty()) { result.status = "skipped"; result.message = "This file type is not supported"; log(requested, result); return result; }
        const std::string source_hash = directory ? "" : hash_file(source, cancel);
        if (!source_hash.empty()) {
            const auto duplicate = query("SELECT id FROM models WHERE source_hash=?1 UNION SELECT model_id AS id FROM merged_sources WHERE source_hash=?1 UNION SELECT model_id AS id FROM files WHERE hash=?1 LIMIT 1", {source_hash});
            if (!duplicate.empty()) {
                const Id id = duplicate[0]["id"];
                bool verified = false;
                for (const auto& entry : files(id)) {
                    const auto file = resolve(entry["rel_path"]);
                    if (entry["hash"] == source_hash && fs::is_regular_file(file) && hash_file(file, cancel) == source_hash) verified = true;
                }
                // Archive hashes describe a container; verify every retained file.
                if (archive) {
                    auto entries = files(id);
                    verified = !entries.empty();
                    for (const auto& entry : entries) {
                        const auto file = resolve(entry["rel_path"]);
                        if (!fs::is_regular_file(file) || hash_file(file, cancel) != entry["hash"]) { verified = false; break; }
                    }
                }
                if (verified) { result = {id, "duplicate", "Already in the library"}; log(requested, result); return result; }
            }
        }
        TemporaryDirectory staging(m_root);
        const auto staged_files = staging.path / "files";
        fs::create_directory(staged_files);
        fs::path input_root;
        std::vector<fs::path> input;
        if (archive) {
            const auto expanded = staging.path / "expanded";
            fs::create_directory(expanded);
            unpack(source, expanded, cancel);
            input_root = expanded;
            input = contents(expanded, true, cancel);
        } else if (directory) { input_root = source; input = contents(source, true, cancel); }
        else { input_root = source.parent_path(); input.push_back(source); }
        if (input.empty()) throw std::runtime_error("No supported files were found");
        Json entries = Json::array();
        for (const auto& path : input) {
            cancelled(cancel);
            const auto relative = path.lexically_relative(input_root);
            const auto target = staged_files / relative;
            fs::create_directories(target.parent_path());
            fs::copy_file(path, target);
            const auto hash = hash_file(target, cancel);
            // A download or external editor may have changed the source during copying.
            if (hash_file(path, cancel) != hash) throw std::runtime_error("A file changed during import. Try again when it has finished saving.");
            Json entry{{"relative", relative.generic_u8string()}, {"hash", hash}, {"size", fs::file_size(target)},
                       {"kind", file_kind(path)}, {"format", format(path)}, {"original_name", path.filename().u8string()}, {"meta", Json::array()}};
            if (extract) {
                try { entry.update(extract(target, hash, m_root)); }
                catch (const std::exception& error) { entry["error"] = error.what(); }
            }
            entries.push_back(std::move(entry));
        }
        if (!directory && hash_file(source, cancel) != source_hash)
            throw std::runtime_error("The download changed during import. Try again when it has finished saving.");
        const auto insert_file = [&](Id id, const std::string& dir, const Json& entry) {
                execute("INSERT INTO files(model_id,kind,format,rel_path,original_name,size,hash,thumb,sliced,triangles,vertices,size_x,size_y,size_z,volume,area,meta,error,geometry_hash,geometry_revision) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20)",
                        {id, entry["kind"], entry["format"], dir + '/' + entry["relative"].get<std::string>(), entry["original_name"], entry["size"], entry["hash"], entry.value("thumb", Json()), entry.value("sliced", Json(false)), entry.value("triangles", Json()), entry.value("vertices", Json()), entry.value("size_x", Json()), entry.value("size_y", Json()), entry.value("size_z", Json()), entry.value("volume", Json()), entry.value("area", Json()), entry["meta"].dump(), entry.value("error", Json()), entry.value("geometry_hash", Json()), entry.value("geometry_revision", Json(0))});
            };
        // A pack is an exact match only when one model owns every file at the
        // requested relative path. Matching a subset across models is not enough.
        if (directory || archive) {
            const auto candidates = query("SELECT DISTINCT m.id,m.dir FROM models m JOIN files f ON f.model_id=m.id WHERE f.hash=?1", {entries[0]["hash"]});
            for (const auto& candidate : candidates) {
                const auto existing = files(candidate["id"]);
                bool match = true;
                for (const auto& entry : entries) {
                    const std::string expected = candidate["dir"].get<std::string>() + '/' + entry["relative"].get<std::string>();
                    const auto found = std::find_if(existing.begin(), existing.end(), [&](const Json& f) { return f["rel_path"] == expected && f["hash"] == entry["hash"]; });
                    if (found == existing.end() || !fs::is_regular_file(resolve(expected)) || hash_file(resolve(expected), cancel) != entry["hash"]) { match = false; break; }
                }
                if (match) { result = {candidate["id"], "duplicate", "Already in the library"}; log(requested, result); return result; }
            }
        }
        // Only the same source folder can grow in place. Every previously
        // imported file must still exist unchanged in both copies. An edited or
        // missing file creates a new version, preserving the old pack intact.
        if (directory) {
            for (const auto& candidate : query("SELECT id,dir FROM models WHERE source_path=?1 AND reshaped=0 ORDER BY id DESC", {source.u8string()})) {
                const Id id = candidate["id"];
                const std::string dir = candidate["dir"];
                const auto existing = files(id);
                if (existing.empty()) continue;
                bool unchanged = true;
                std::set<std::string> retained;
                for (const auto& old : existing) {
                    const auto path = get_text(old, "rel_path");
                    if (path.rfind(dir + '/', 0) != 0) { unchanged = false; break; }
                    const auto relative = path.substr(dir.size() + 1);
                    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Json& entry) { return entry["relative"] == relative && entry["hash"] == old["hash"]; });
                    if (found == entries.end() || !fs::is_regular_file(resolve(path)) || hash_file(resolve(path), cancel) != old["hash"]) { unchanged = false; break; }
                    retained.insert(relative);
                }
                if (!unchanged) continue;
                std::vector<std::pair<fs::path, fs::path>> moved_files;
                batch("BEGIN IMMEDIATE;");
                try {
                    for (const auto& entry : entries) {
                        cancelled(cancel);
                        const std::string relative = entry["relative"];
                        if (retained.count(relative)) continue;
                        const auto destination = resolve(dir + '/' + relative);
                        if (fs::exists(destination)) throw std::runtime_error("A file was added to the library folder outside Porca. It has been kept.");
                        fs::create_directories(destination.parent_path());
                        const auto staged = staged_files / fs::u8path(relative);
                        fs::rename(staged, destination);
                        moved_files.emplace_back(destination, staged);
                        insert_file(id, dir, entry);
                    }
                    execute(std::string("UPDATE models SET updated_at=") + timestamp + " WHERE id=?1", {id});
                    result = {id, "imported", "Added new files to the existing model"};
                    log(requested, result);
                    batch("COMMIT;");
                } catch (...) {
                    batch("ROLLBACK;");
                    for (auto it = moved_files.rbegin(); it != moved_files.rend(); ++it) { std::error_code error; fs::rename(it->first, it->second, error); }
                    throw;
                }
                try { organize(); } catch (const std::exception& error) { result.message += std::string(". Organization needs attention: ") + error.what(); }
                return result;
            }
        }
        std::string name = clean_name(directory ? source.filename().u8string() : source.stem().u8string());
        for (const auto& entry : entries) if (!get_text(entry, "title").empty()) { name = entry["title"]; break; }
        batch("BEGIN IMMEDIATE;");
        std::string dir;
        bool moved = false;
        try {
            cancelled(cancel);
            dir = unique_dir(name);
            Json designer, description, license, source_url, source_site, thumb;
            if (!origin.empty()) { source_url = origin; source_site = origin_label(origin); }
            for (const auto& entry : entries) {
                for (auto pair : {std::make_pair("designer", &designer), {"description", &description}, {"license", &license}, {"source_url", &source_url}, {"source_site", &source_site}, {"thumb", &thumb}})
                    if (pair.second->is_null() && entry.contains(pair.first) && !entry[pair.first].is_null()) *pair.second = entry[pair.first];
            }
            execute(std::string("INSERT INTO models(name,dir,designer,description,license,source_url,source_site,source_path,source_hash,thumb,added_at,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,") + timestamp + ',' + timestamp + ")",
                    {name, dir, designer, description, license, source_url, source_site, source.u8string(), source_hash.empty() ? Json() : Json(source_hash), thumb});
            const Id id = sqlite3_last_insert_rowid(m_db);
            for (const auto& entry : entries) insert_file(id, dir, entry);
            fs::rename(staged_files, resolve(dir));
            moved = true;
            result = {id, "imported", "Added to the library"};
            log(requested, result);
            batch("COMMIT;");
        } catch (...) {
            batch("ROLLBACK;");
            if (moved) { std::error_code error; fs::rename(resolve(dir), staged_files, error); }
            throw;
        }
        try { organize(); } catch (const std::exception& error) { result.message += std::string(". Organization needs attention: ") + error.what(); }
        return result;
    } catch (const std::exception& error) {
        result = {0, "failed", error.what()};
        log(requested, result);
        return result;
    }
}

void Library::restore_pack_layout(Id id, const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const auto model = detail(id);
    if (model.value("reshaped",0)) throw std::runtime_error("This model was merged or split. Its folder layout has been kept.");
    const auto source = fs::u8path(get_text(model,"source_path"));
    if (format(source) != "zip" || !fs::is_regular_file(source) || get_text(model,"source_hash").empty() || hash_file(source,cancel) != model["source_hash"])
        throw std::runtime_error("The original, unchanged ZIP is needed to restore this model's folders");
    TemporaryDirectory staging(m_root);
    const auto expanded = staging.path / "expanded"; fs::create_directory(expanded); unpack(source,expanded,cancel);
    const std::string dir = model["dir"]; const auto original = resolve(dir);
    if (original.parent_path() != resolve("models")) throw std::runtime_error("This model folder cannot be restored safely");
    // Match by original name and bytes, not the old flattened path. Ambiguous or
    // missing matches must leave the complete existing pack untouched.
    Json changes = Json::array(); std::set<Id> used;
    for (const auto& path : contents(expanded,true,cancel)) {
        const auto hash = hash_file(path,cancel); Json matches = Json::array();
        for (const auto& file : model["files"]) if (file["hash"] == hash && file["original_name"] == path.filename().u8string() && !used.count(file["id"].get<Id>())) matches.push_back(file);
        if (matches.size() != 1) throw std::runtime_error("The ZIP does not match the saved files closely enough to restore its folders");
        const auto file = matches[0];
        const auto stored = resolve(file["rel_path"]);
        if (!fs::is_regular_file(stored) || hash_file(stored,cancel) != hash) throw std::runtime_error("A saved file changed. Its current layout has been kept.");
        used.insert(file["id"].get<Id>());
        changes.push_back({{"id",file["id"]},{"old",file["rel_path"]},{"next",dir + '/' + path.lexically_relative(expanded).generic_u8string()}});
    }
    const auto replacement = staging.path / "replacement";
    // Also keep files that were added outside the catalog. Reject links rather
    // than accidentally copying content from outside this model.
    (void)contents(original,false,cancel);
    fs::copy(original,replacement,fs::copy_options::recursive);
    for (const auto& change : changes) {
        const auto target = replacement / fs::u8path(change["next"].get<std::string>().substr(dir.size() + 1));
        const auto stored = resolve(change["old"]);
        if (fs::exists(target) && hash_file(target,cancel) != hash_file(stored,cancel)) throw std::runtime_error("Restoring the folder would replace another file. Both files have been kept.");
        fs::create_directories(target.parent_path());
        if (!fs::exists(target)) fs::copy_file(stored,target);
    }
    for (const auto& change : changes) if (change["old"] != change["next"]) {
        bool destination = false;
        for (const auto& other : changes) if (other["next"] == change["old"]) destination = true;
        if (!destination) fs::remove(replacement / fs::u8path(change["old"].get<std::string>().substr(dir.size() + 1)));
    }
    const auto backup = staging.path / "previous";
    bool relocated = false, installed = false;
    batch("BEGIN IMMEDIATE;");
    try {
        cancelled(cancel);
        for (const auto& change : changes) {
            execute("UPDATE files SET rel_path=?1 WHERE id=?2",{change["next"],change["id"]});
            execute("UPDATE models SET thumb=?1 WHERE id=?2 AND thumb=?3",{change["next"],id,change["old"]});
            execute("UPDATE files SET thumb=?1 WHERE model_id=?2 AND thumb=?3",{change["next"],id,change["old"]});
        }
        fs::rename(original,backup); relocated = true;
        fs::rename(replacement,original); installed = true;
        batch("COMMIT;");
    } catch (...) {
        batch("ROLLBACK;"); std::error_code error;
        if (installed) fs::rename(original,replacement,error);
        if (relocated) {
            fs::rename(backup,original,error);
            // Retain recoverable originals if a filesystem failure blocks rollback.
            if (error) { staging.path.clear(); throw std::runtime_error("The previous files are kept in " + backup.u8string() + ". Restore that folder before using this model."); }
        }
        throw;
    }
}

Json Library::plan_organized(const fs::path& path, const Cancel& cancel) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Json categories = Json::array();
    for (const auto& category : fs::directory_iterator(path)) {
        cancelled(cancel);
        if (ignored(category.path()) || category.is_symlink() || contains(category.path())) continue;
        Json group{{"name", category.is_directory() ? category.path().filename().u8string() : ""}, {"items", Json::array()}};
        std::vector<fs::path> items;
        if (category.is_directory()) {
            for (const auto& item : fs::directory_iterator(category.path())) items.push_back(item.path());
        } else items.push_back(category.path());
        std::sort(items.begin(),items.end());
        for (const auto& item : items) {
            cancelled(cancel);
            Json row{{"path", item.u8string()}, {"name", item.filename().u8string()}, {"status", "new"}, {"left_out", Json::array()}};
            try {
                if (fs::is_symlink(item) || ignored(item) || contains(item)) {
                    row["status"] = "unsupported";
                    row["reason"] = "Linked, hidden and library files stay in their original location";
                } else if (fs::is_regular_file(item) && file_kind(item).empty()) {
                    row["status"] = "unsupported";
                    row["reason"] = "This file type stays in its original location";
                } else {
                    const bool archive = format(item) == "zip" && settings().value("extract_zips",true);
                    const bool pack = fs::is_directory(item) || archive;
                    TemporaryDirectory staging(m_root);
                    auto input_root = pack ? item : item.parent_path();
                    if (archive) {
                        input_root = staging.path / "expanded"; fs::create_directory(input_root);
                        unpack(item,input_root,cancel);
                        Archive zip(item);
                        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.zip); ++i) {
                            mz_zip_archive_file_stat stat{};
                            if (!mz_zip_reader_file_stat(&zip.zip,i,&stat)) throw std::runtime_error("Could not read an archive entry");
                            if (mz_zip_reader_is_file_a_directory(&zip.zip,i)) continue;
                            if (!fs::exists(input_root / fs::u8path(stat.m_filename))) row["left_out"].push_back(stat.m_filename);
                        }
                    } else if (pack) {
                        for (auto it = fs::recursive_directory_iterator(item); it != fs::recursive_directory_iterator(); ++it) {
                            cancelled(cancel);
                            if (it->is_symlink() || ignored(it->path())) { row["left_out"].push_back(it->path().lexically_relative(item).generic_u8string()); it.disable_recursion_pending(); }
                            else if (it->is_regular_file() && file_kind(it->path()).empty()) row["left_out"].push_back(it->path().lexically_relative(item).generic_u8string());
                        }
                    }
                    const auto input = pack ? contents(input_root,true,cancel) : std::vector<fs::path>{item};
                    Json entries = Json::array();
                    for (const auto& file : input) entries.push_back({{"relative",file.lexically_relative(input_root).generic_u8string()},{"hash",hash_file(file,cancel)}});
                    if (entries.empty()) { row["status"] = "unsupported"; row["reason"] = "No supported files were found"; }
                    else {
                        for (const auto& candidate : query("SELECT DISTINCT m.id,m.dir FROM models m JOIN files f ON f.model_id=m.id WHERE f.hash=?1",{entries[0]["hash"]})) {
                            const auto existing = files(candidate["id"]); bool match = true;
                            for (const auto& entry : entries) {
                                const auto expected = candidate["dir"].get<std::string>() + '/' + entry["relative"].get<std::string>();
                                const auto found = std::find_if(existing.begin(),existing.end(),[&](const Json& f) { return f["hash"] == entry["hash"] && (!pack || f["rel_path"] == expected); });
                                if (found == existing.end()) { match = false; break; }
                                const auto saved = resolve((*found)["rel_path"]);
                                if (!fs::is_regular_file(saved) || hash_file(saved,cancel) != entry["hash"]) { match = false; break; }
                            }
                            if (match) { row["status"] = "duplicate"; row["model_id"] = candidate["id"]; break; }
                        }
                        if (row["status"] == "new") {
                            const auto name = version_stem(clean_name(pack && !archive ? item.filename().u8string() : item.stem().u8string()));
                            for (const auto& candidate : query("SELECT id,name,source_path FROM models")) {
                                if (candidate["source_path"] == item.u8string() || (name.size() >= 5 && version_stem(get_text(candidate,"name")) == name)) {
                                    row["status"] = "version"; row["model_id"] = candidate["id"]; break;
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& error) { cancelled(cancel); row["status"] = "unreadable"; row["reason"] = error.what(); }
            group["items"].push_back(std::move(row));
        }
        categories.push_back(std::move(group));
    }
    return categories;
}

Json Library::import_organized(const fs::path& path, const std::vector<std::string>& selected, const Extractor& extract, const Cancel& cancel)
{
    auto plan = plan_organized(path, cancel);
    Json results = Json::array();
    for (const auto& group : plan) {
        const std::string name = group["name"];
        if (std::find(selected.begin(), selected.end(), name) == selected.end()) continue;
        Id collection = 0;
        if (!name.empty()) {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            const auto existing = query("SELECT id FROM collections WHERE name=?1 AND query IS NULL LIMIT 1", {name});
            collection = existing.empty() ? create_collection(name) : existing[0]["id"].get<Id>();
        }
        for (const auto& item : group["items"]) {
            cancelled(cancel);
            if (item["status"] == "unsupported" || item["status"] == "unreadable") continue;
            const auto result = import(fs::u8path(item["path"].get<std::string>()), extract, cancel);
            if (collection && result.model_id) collect(collection, {result.model_id}, true);
            results.push_back({{"path", item["path"]}, {"status", result.status}, {"message", result.message}, {"model_id", result.model_id}, {"collection", name}});
        }
    }
    return results;
}

void Library::rescan(Id model, const Extractor& extract, const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!extract) return;
    for (const auto& file : files(model)) {
        cancelled(cancel);
        try {
            const auto path = resolve(file["rel_path"]);
            const auto hash = hash_file(path, cancel);
            const auto data = extract(path, hash, m_root);
            execute("UPDATE files SET hash=?1,size=?2,thumb=?3,sliced=?4,triangles=?5,vertices=?6,size_x=?7,size_y=?8,size_z=?9,volume=?10,area=?11,meta=?12,error=?13,geometry_hash=?14,geometry_revision=?15 WHERE id=?16",
                    {hash, fs::file_size(path), data.value("thumb", Json()), data.value("sliced", Json(false)), data.value("triangles", Json()), data.value("vertices", Json()), data.value("size_x", Json()), data.value("size_y", Json()), data.value("size_z", Json()), data.value("volume", Json()), data.value("area", Json()), data.value("meta", Json::array()).dump(), data.value("error", Json()), data.value("geometry_hash", Json()), data.value("geometry_revision", Json(0)), file["id"]});
        } catch (const std::exception& error) {
            cancelled(cancel);
            execute("UPDATE files SET error=?1 WHERE id=?2", {error.what(), file["id"]});
        }
    }
    execute("UPDATE models SET thumb=coalesce(thumb,(SELECT f.thumb FROM files f WHERE f.model_id=models.id AND f.thumb IS NOT NULL LIMIT 1)) WHERE id=?1", {model});
    organize();
}

void Library::repair_source_links(const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& model : query("SELECT id,source_url,source_path FROM models")) {
        cancelled(cancel);
        const auto url = get_text(model,"source_url");
        // Older downloads sometimes recorded only Printables' home page.
        // Never replace a specific page or a user-entered source link.
        if (!url.empty() && url != "https://www.printables.com/" && url != "https://www.printables.com" &&
            url != "https://printables.com/" && url != "https://printables.com") continue;
        const auto source = fs::u8path(get_text(model,"source_path"));
        if (source.empty()) continue;
        std::error_code error;
        if (!fs::is_regular_file(source,error)) continue;
        const auto origin = download_origin(source);
        if (origin.empty() || origin == url) continue;
        if (!url.empty() && origin.find("printables.com/model/") == std::string::npos) continue;
        execute("UPDATE models SET source_url=?1,source_site=?2,page_read_at=NULL WHERE id=?3",{origin,origin_label(origin),model["id"]});
    }
}

bool Library::backfill_geometry(const Extractor& extract, const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!extract) return false;
    for (const auto& row : query("SELECT DISTINCT m.id FROM models m JOIN files f ON f.model_id=m.id WHERE m.archived_at IS NULL AND f.format IN ('stl','obj','ply','amf','3mf') AND f.geometry_revision<2 AND f.error IS NULL ORDER BY m.id DESC")) {
        cancelled(cancel);
        const Id id = row["id"];
        if (m_geometry_attempted.count(id)) continue;
        // A bounded idle job tries one model once per library session. Large
        // models excluded from fingerprinting must not monopolize the worker.
        m_geometry_attempted.insert(id);
        try { rescan(id,extract,cancel); }
        catch (...) { if (cancel && cancel()) m_geometry_attempted.erase(id); throw; }
        return true;
    }
    return false;
}

bool Library::fully_present(const fs::path& path, const Cancel& cancel) const
{
    // Every file must be present and freshly verified, including unrecognized
    // files and hidden directories. A catalog hash alone never authorizes trash.
    std::vector<fs::path> input;
    if (fs::is_symlink(path)) return false;
    if (fs::is_directory(path)) input = contents(path, false, cancel);
    else if (format(path) == "zip") {
        TemporaryDirectory expanded(m_root);
        // Unlike import, proof of preservation includes unknown and hidden files.
        unpack(path, expanded.path, cancel, false);
        return fully_present(expanded.path, cancel);
    } else input.push_back(path);
    if (input.empty()) return false;
    for (const auto& file : input) {
        cancelled(cancel);
        const auto hash = hash_file(file, cancel);
        const auto matches = query("SELECT rel_path FROM files WHERE hash=?1", {hash});
        bool found = false;
        for (const auto& match : matches) {
            const auto stored = resolve(match["rel_path"]);
            if (fs::is_regular_file(stored) && hash_file(stored, cancel) == hash) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

Json Library::leftovers(const Cancel& cancel) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Json result = Json::array();
    const auto managed = settings().value("slicer_watch_paths", Json::array());
    for (const auto& folder : watch_folders()) {
        if (std::find(managed.begin(), managed.end(), folder["path"]) != managed.end()) continue;
        const auto path = fs::u8path(folder["path"].get<std::string>());
        if (!fs::is_directory(path)) continue;
        for (const auto& item : scan_items(path, true)) {
            cancelled(cancel);
            try { if (fully_present(item, cancel)) result.push_back({{"path", item.u8string()}, {"name", item.filename().u8string()}}); }
            catch (const std::exception&) { cancelled(cancel); }
        }
    }
    return result;
}

void Library::trash_leftovers(const std::vector<fs::path>& paths, const std::function<bool(const fs::path&)>& trash, const Cancel& cancel)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& path : paths) {
        cancelled(cancel);
        const auto managed = settings().value("slicer_watch_paths", Json::array());
        if (std::find(managed.begin(),managed.end(),fs::weakly_canonical(path.parent_path()).u8string()) != managed.end()) throw std::runtime_error("Files managed by a slicer stay in its download folder");
        bool watched = false;
        for (const auto& folder : watch_folders()) if (fs::weakly_canonical(path.parent_path()) == fs::weakly_canonical(fs::u8path(folder["path"].get<std::string>()))) watched = true;
        if (!watched || contains(path) || !fully_present(path, cancel)) throw std::runtime_error("An original changed or is not fully saved in the library. It has been kept.");
        if (!trash(path)) throw std::runtime_error("Could not move " + path.filename().u8string() + " to the Trash");
    }
}

void Library::trash_models(const std::vector<Id>& ids, const std::function<bool(const fs::path&)>& trash)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (Id id : ids) {
        const auto model = detail(id);
        const auto dir = resolve(model["dir"]);
        if (dir.parent_path() != m_root / "models") throw std::runtime_error("This model's folder cannot be moved safely");
        if (fs::exists(dir) && !trash(dir)) throw std::runtime_error("Could not move " + get_text(model, "name") + " to the Trash");
        execute("DELETE FROM models WHERE id=?1", {id});
    }
}
} // namespace Slic3r::ModelLibrary
