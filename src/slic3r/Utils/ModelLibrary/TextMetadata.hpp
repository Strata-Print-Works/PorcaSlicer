#pragma once
#include "Library.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::ModelLibrary {
std::string plain_text(std::string text);
Json readme_metadata(const std::string& text);
Json step_metadata(const std::string& text);
struct FileMetadata {
    Json fields{{"meta", Json::array()}, {"sliced", true}};
    std::vector<std::uint8_t> thumbnail;
    std::string extension;
};
FileMetadata gcode_metadata(const std::string& text);
FileMetadata bgcode_metadata(const std::vector<std::uint8_t>& bytes);
void append_print_metadata(Json& fields, const std::string& key, const std::string& value);
}
