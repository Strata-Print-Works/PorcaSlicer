#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace Slic3r::ModelLibrary {
std::vector<std::filesystem::path> slicer_download_folders(const std::filesystem::path& config);
std::vector<std::pair<std::string,std::filesystem::path>> installed_slicers(const std::filesystem::path& home);
std::string download_origin(const std::filesystem::path& path);
std::string origin_label(const std::string& url);
}
