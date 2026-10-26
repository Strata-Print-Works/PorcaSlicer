#include "Watch.hpp"
#include <library_storage/blake3.h>

#include <algorithm>
#include <cstdint>
#include <array>
#include <vector>
#include <utility>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r::ModelLibrary {
namespace {
std::string modified(const fs::file_time_type& time)
{
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto epoch = std::chrono::system_clock::to_time_t(system);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &epoch);
#else
    gmtime_r(&epoch, &utc);
#endif
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return value.str();
}

struct Signature { std::string value; fs::file_time_type newest; bool ready = true; };
Signature signature(const fs::path& path, const Cancel& cancel)
{
    Signature result;
    result.newest = fs::last_write_time(path);
    std::uintmax_t bytes = 0, files = 0;
    std::vector<std::string> entries;
    const auto add = [&](const fs::path& file) {
        const auto stamp = fs::last_write_time(file);
        result.newest = std::max(result.newest, stamp);
        const auto size = fs::file_size(file);
        bytes += size;
        auto entry = file.lexically_relative(path).generic_u8string();
        entry.push_back('\0');
        entry += std::to_string(size) + ':' + std::to_string(std::uint64_t(stamp.time_since_epoch().count()));
        entries.push_back(std::move(entry));
        ++files;
        const auto extension = file.extension().u8string();
        if (extension == ".part" || extension == ".crdownload" || extension == ".download" || extension == ".tmp") result.ready = false;
    };
    if (fs::is_directory(path)) {
        for (auto it = fs::recursive_directory_iterator(path); it != fs::recursive_directory_iterator(); ++it) {
            if (cancel && cancel()) throw std::runtime_error("Cancelled");
            if (it->is_symlink()) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file()) add(it->path());
            if (files > 100000) throw std::runtime_error("This folder has too many files to watch");
        }
    } else add(path);
    // Include each relative filename and its metadata. A sum of sizes/times
    // misses renames and can pair one file's size with another file's timestamp.
    std::sort(entries.begin(), entries.end());
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    for (const auto& entry : entries) {
        blake3_hasher_update(&hasher, entry.data(), entry.size());
        blake3_hasher_update(&hasher, "", 1);
    }
    std::array<unsigned char, BLAKE3_OUT_LEN> digest;
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    result.value.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    result.ready = result.ready && files > 0 && bytes > 0;
    return result;
}
}

Json Watch::poll(Library& library, const Extractor& extract, const Cancel& cancel, std::chrono::steady_clock::time_point now)
{
    Json results = Json::array();
    std::set<std::string> seen;
    for (const auto& folder : library.watch_folders()) {
        if (cancel && cancel()) break;
        if (!folder.value("enabled", 0)) continue;
        const auto root = fs::u8path(folder["path"].get<std::string>());
        if (!fs::is_directory(root)) continue;
        const std::string since = folder["last_scan_at"].is_string() ? folder["last_scan_at"].get<std::string>() : folder["added_at"].get<std::string>();
        bool pending = false;
        try {
            auto paths = library.scan_items(root, folder.value("recursive", 0) != 0);
            if (paths.size() > 10000) paths.resize(10000);
            for (const auto& path : paths) {
                if (cancel && cancel()) break;
                const auto key = path.u8string(); seen.insert(key);
                const auto current = signature(path, cancel);
                auto found = m_pending.find(key);
                if (found == m_pending.end()) {
                    const bool old = modified(current.newest) < since;
                    found = m_pending.emplace(key, Pending{current.value, now, old}).first;
                } else if (found->second.signature != current.value) found->second = {current.value, now, false};
                if (found->second.imported) continue;
                pending = true;
                const auto settle = fs::is_directory(path) ? std::chrono::seconds(4) : std::chrono::seconds(2);
                if (!current.ready || now - found->second.changed < settle) continue;
                auto result = library.import(path, extract, cancel);
                found->second.imported = result.status != "failed";
                results.push_back({{"path",path.u8string()},{"model_id", result.model_id}, {"status", result.status}, {"message", result.message}});
                if (result.status == "failed") found->second.changed = now + std::chrono::minutes(1);
                if (results.size() >= 8) break;
            }
            // Do not advance past a download still settling or a bounded batch.
            if (!pending && !(cancel && cancel())) library.mark_scanned(folder["id"]);
        } catch (const std::exception&) { /* Missing/inaccessible folders are retried, never removed. */ }
    }
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (!seen.count(it->first)) it = m_pending.erase(it);
        else ++it;
    }
    return results;
}
}
