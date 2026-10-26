#include "Source.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <sys/xattr.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace Slic3r::ModelLibrary {
namespace {
std::string host(const std::string& url)
{
    const auto start = url.find("://");
    if (start == std::string::npos) return {};
    auto value = url.substr(start + 3, url.find_first_of("/?#", start + 3) - start - 3);
    const auto auth = value.rfind('@'); if (auth != std::string::npos) value.erase(0, auth + 1);
    if (!value.empty() && value.front() != '[') value.resize(value.find(':') == std::string::npos ? value.size() : value.find(':'));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (value.rfind("www.", 0) == 0) value.erase(0, 4);
    return value;
}
bool suffix(const std::string& value, const std::string& ending)
{
    return value.size() >= ending.size() && value.compare(value.size() - ending.size(), ending.size(), ending) == 0;
}
}

std::string origin_label(const std::string& url)
{
    const auto domain = host(url);
    if (domain.empty()) return {};
    const std::map<std::string, std::string> sites{{"printables.com", "Printables"}, {"thingiverse.com", "Thingiverse"}, {"makerworld.com", "MakerWorld"}, {"thangs.com", "Thangs"}, {"cults3d.com", "Cults3D"}, {"myminifactory.com", "MyMiniFactory"}, {"crealitycloud.com", "Creality Cloud"}, {"claude.ai", "Claude"}, {"claudeusercontent.com", "Claude"}, {"chatgpt.com", "ChatGPT"}, {"chatgpt.site", "ChatGPT"}};
    for (const auto& [site, label] : sites) if (domain == site || suffix(domain, '.' + site)) return label;
    if (domain == "localhost" || domain == "[::1]" || suffix(domain, ".local") || suffix(domain, ".localhost") || suffix(domain, ".ts.net") || suffix(domain, ".home.arpa") ||
        std::regex_match(domain, std::regex(R"((10\..*|127\..*|192\.168\..*|172\.(1[6-9]|2[0-9]|3[01])\..*))"))) return "Local tools";
    return domain;
}

std::string download_origin(const std::filesystem::path& path)
{
    std::vector<std::string> urls;
#ifdef __APPLE__
    const char* key = "com.apple.metadata:kMDItemWhereFroms";
    const auto size = getxattr(path.c_str(), key, nullptr, 0, 0, 0);
    if (size > 0 && size < 1024 * 1024) {
        std::vector<unsigned char> bytes(size_t(size), 0);
        if (getxattr(path.c_str(), key, bytes.data(), bytes.size(), 0, 0) == size) {
            CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(), CFIndex(bytes.size()));
            CFPropertyListRef plist = data ? CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr) : nullptr;
            if (plist && CFGetTypeID(plist) == CFArrayGetTypeID()) {
                auto array = static_cast<CFArrayRef>(plist);
                for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
                    const auto value = CFArrayGetValueAtIndex(array, i);
                    if (CFGetTypeID(value) != CFStringGetTypeID()) continue;
                    auto string = static_cast<CFStringRef>(value);
                    const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
                    std::vector<char> buffer(size_t(capacity), 0);
                    if (CFStringGetCString(string, buffer.data(), capacity, kCFStringEncodingUTF8)) urls.emplace_back(buffer.data());
                }
            }
            if (plist) CFRelease(plist);
            if (data) CFRelease(data);
        }
    }
#elif defined(__linux__)
    for (const char* key : {"user.xdg.origin.url", "user.xdg.referrer.url"}) {
        const auto size = getxattr(path.c_str(), key, nullptr, 0);
        if (size <= 0 || size > 1024 * 1024) continue;
        std::string value(size_t(size), '\0');
        if (getxattr(path.c_str(), key, value.data(), value.size()) == size) urls.push_back(std::move(value));
    }
#elif defined(_WIN32)
    auto stream_path = path; stream_path += L":Zone.Identifier";
    std::ifstream stream(stream_path);
    std::string line, download, referrer;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("HostUrl=", 0) == 0) download = line.substr(8);
        if (line.rfind("ReferrerUrl=", 0) == 0) referrer = line.substr(12);
    }
    urls = {download, referrer};
#endif
    for (const auto& url : urls) {
        std::smatch match;
        if (std::regex_search(url, match, std::regex(R"(^https?://files\.printables\.com/media/prints/([0-9]+)/)"))) return "https://www.printables.com/model/" + match[1].str();
    }
    for (auto it = urls.rbegin(); it != urls.rend(); ++it) if (it->rfind("http://", 0) == 0 || it->rfind("https://", 0) == 0) return *it;
    // Creality's downloaded models carry their origin in a neighboring index.
    const auto index = path.parent_path().parent_path() / "cloud_download_data.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(index, error) && std::filesystem::file_size(index, error) <= 8 * 1024 * 1024) {
        std::ifstream stream(index);
        auto data = nlohmann::json::parse(stream, nullptr, false);
        if (data.is_object()) for (const auto& list : data) if (list.is_array()) for (const auto& entry : list) {
            if (!entry.is_object() || !entry.value("path", nlohmann::json()).is_string() || !entry.value("modelGroupId", nlohmann::json()).is_string()) continue;
            if (std::filesystem::u8path(entry["path"].get<std::string>()).filename() != path.filename()) continue;
            const std::string id = entry["modelGroupId"];
            if (!id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c); })) return "https://www.crealitycloud.com/model-detail/" + id;
        }
    }
    return {};
}
}

namespace Slic3r::ModelLibrary {
std::vector<std::filesystem::path> slicer_download_folders(const std::filesystem::path& config)
{
    namespace fs = std::filesystem;
    const auto download = [](const fs::path& file) -> fs::path {
        std::error_code error;
        if (!fs::is_regular_file(file,error) || fs::file_size(file,error) > 4 * 1024 * 1024) return {};
        std::ifstream stream(file); const std::string content((std::istreambuf_iterator<char>(stream)),{});
        std::smatch match;
        if (!std::regex_search(content,match,std::regex(R"rx("download_path"\s*:\s*("(?:[^"\\]|\\.)*"))rx"))) return {};
        const auto value = nlohmann::json::parse(match[1].str(),nullptr,false);
        return value.is_string() ? fs::u8path(value.get<std::string>()) : fs::path();
    };
    std::vector<fs::path> paths;
    const auto add = [&](const fs::path& path) {
        std::error_code error;
        if (path.empty() || !fs::is_directory(path,error)) return;
        const auto canonical = fs::canonical(path,error);
        if (!error && std::find(paths.begin(),paths.end(),canonical) == paths.end()) paths.push_back(canonical);
    };
    for (const auto& [folder,file] : std::vector<std::pair<std::string,std::string>>{{"BambuStudio","BambuStudio.conf"},{"BambuStudioBeta","BambuStudio.conf"},{"OrcaSlicer","OrcaSlicer.conf"},{"Snapmaker_Orca","Snapmaker_Orca.conf"},{"ElegooSlicer","ElegooSlicer.conf"},{"QIDIStudio","QIDIStudio.conf"}}) add(download(config / folder / file));
    std::error_code error;
    const auto creality = config / "Creality" / "Creality Print";
    if (fs::is_directory(creality,error)) for (const auto& version : fs::directory_iterator(creality,fs::directory_options::skip_permission_denied)) {
        const auto base = download(version.path() / "Creality.conf");
        if (!base.empty() && fs::is_directory(base,error)) for (const auto& account : fs::directory_iterator(base,fs::directory_options::skip_permission_denied)) {
            add(account.path() / "3mfs"); add(account.path() / "models");
        }
    }
    return paths;
}
}

namespace Slic3r::ModelLibrary {
std::vector<std::pair<std::string,std::filesystem::path>> installed_slicers(const std::filesystem::path& home)
{
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string,fs::path>> result;
    const auto add = [&](const std::string& name,const fs::path& path) { std::error_code error; if (fs::exists(path,error) && std::none_of(result.begin(),result.end(),[&](const auto& row) { return row.first == name; })) result.emplace_back(name,path); };
#ifdef __APPLE__
    for (const auto& root : {fs::path("/Applications"),home / "Applications"})
        for (const auto& [name,file] : std::vector<std::pair<std::string,std::string>>{{"Bambu Studio","BambuStudio.app"},{"Bambu Studio","Bambu Studio.app"},{"OrcaSlicer","OrcaSlicer.app"},{"PrusaSlicer","PrusaSlicer.app"},{"UltiMaker Cura","UltiMaker Cura.app"},{"Snapmaker Orca","Snapmaker Orca.app"},{"ElegooSlicer","ElegooSlicer.app"},{"Creality Print","Creality Print.app"},{"Creality Print","CrealityPrint.app"},{"QIDI Studio","QIDIStudio.app"},{"Anycubic Slicer Next","AnycubicSlicerNext.app"},{"SuperSlicer","SuperSlicer.app"},{"FlashPrint","FlashPrint 5.app"},{"Lychee Slicer","LycheeSlicer.app"},{"CHITUBOX","CHITUBOX.app"}}) add(name,root / file);
#elif defined(_WIN32)
    const auto variable = _wgetenv(L"ProgramFiles"); const fs::path root = variable ? fs::path(variable) : fs::path(L"C:\\Program Files");
    for (const auto& [name,file] : std::vector<std::pair<std::string,std::string>>{{"Bambu Studio","Bambu Studio/bambu-studio.exe"},{"OrcaSlicer","OrcaSlicer/orca-slicer.exe"},{"PrusaSlicer","Prusa3D/PrusaSlicer/prusa-slicer.exe"},{"ElegooSlicer","ElegooSlicer/elegoo-slicer.exe"},{"Creality Print","Creality/Creality Print/CrealityPrint.exe"},{"QIDI Studio","QIDIStudio/qidi-studio.exe"}}) add(name,root / file);
    std::error_code error;
    if (fs::is_directory(root,error)) for (const auto& folder : fs::directory_iterator(root,fs::directory_options::skip_permission_denied)) for (const auto* executable : {"UltiMaker-Cura.exe","Ultimaker-Cura.exe","Cura.exe"}) add("UltiMaker Cura",folder.path() / executable);
#else
    std::vector<fs::path> roots{fs::path("/usr/bin"),fs::path("/usr/local/bin"),home / ".local" / "bin"};
    for (const auto& root : roots) for (const auto& [name,file] : std::vector<std::pair<std::string,std::string>>{{"Bambu Studio","bambu-studio"},{"OrcaSlicer","orca-slicer"},{"PrusaSlicer","prusa-slicer"},{"UltiMaker Cura","cura"},{"SuperSlicer","superslicer"},{"ElegooSlicer","elegoo-slicer"},{"Creality Print","CrealityPrint"},{"QIDI Studio","qidi-studio"}}) add(name,root / file);
#endif
    return result;
}
}
