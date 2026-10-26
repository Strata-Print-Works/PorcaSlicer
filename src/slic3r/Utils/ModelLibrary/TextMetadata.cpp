#include "TextMetadata.hpp"
#include <miniz.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Slic3r::ModelLibrary {
namespace {
std::string trim(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n;");
    return begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
std::string lower(std::string value)
{
    for (auto& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
void replace(std::string& text, const std::string& find, const std::string& value)
{
    for (size_t at = 0; (at = text.find(find, at)) != std::string::npos; at += value.size()) text.replace(at, find.size(), value);
}
void utf8(std::string& out, unsigned c)
{
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
}
std::string step_string(std::string value)
{
    std::string out;
    for (size_t i = 0; i < value.size();) {
        if (value.compare(i, 4, "\\X2\\") == 0 || value.compare(i, 4, "\\X4\\") == 0) {
            const size_t width = value[i + 2] == '2' ? 4 : 8;
            const auto end = value.find("\\X0\\", i + 4);
            if (end == std::string::npos) { out += value.substr(i); break; }
            unsigned high = 0;
            for (size_t j = i + 4; j + width <= end; j += width) {
                try {
                    unsigned code = unsigned(std::stoul(value.substr(j, width), nullptr, 16));
                    if (code >= 0xd800 && code <= 0xdbff) { high = code; continue; }
                    if (high && code >= 0xdc00 && code <= 0xdfff) { code = 0x10000 + ((high - 0xd800) << 10) + code - 0xdc00; high = 0; }
                    utf8(out, code);
                } catch (const std::exception&) { utf8(out, 0xfffd); }
            }
            i = end + 4;
        } else if (value.compare(i, 3, "\\X\\") == 0 && i + 5 <= value.size()) {
            try { utf8(out, unsigned(std::stoul(value.substr(i + 3, 2), nullptr, 16))); } catch (const std::exception&) { utf8(out, 0xfffd); }
            i += 5;
        } else if (value.compare(i, 3, "\\S\\") == 0 && i + 4 <= value.size()) { utf8(out, static_cast<unsigned char>(value[i + 3]) + 128); i += 4; }
        else if (value.compare(i, 2, "\\\\") == 0) { out += '\\'; i += 2; }
        else out += value[i++];
    }
    return out;
}
void space(const std::string& input, size_t& at)
{
    while (at < input.size()) {
        if (std::isspace(static_cast<unsigned char>(input[at]))) ++at;
        else if (input.compare(at, 2, "/*") == 0) { const auto end = input.find("*/", at + 2); at = end == std::string::npos ? input.size() : end + 2; }
        else break;
    }
}
std::vector<std::string> step_args(const std::string& input, size_t& at, unsigned depth = 0)
{
    if (depth > 16) throw std::runtime_error("STEP header nesting is too deep");
    std::vector<std::string> result;
    while (at < input.size()) {
        space(input, at);
        if (at == input.size()) break;
        if (input[at] == ')') { ++at; break; }
        if (input[at] == ',') { ++at; continue; }
        if (input[at] == '(') {
            ++at; std::string joined;
            for (const auto& value : step_args(input, at, depth + 1)) { if (!joined.empty()) joined += ", "; joined += value; }
            result.push_back(joined);
        } else if (input[at] == '\'') {
            ++at; std::string value;
            while (at < input.size()) {
                if (input[at] != '\'') value += input[at++];
                else if (at + 1 < input.size() && input[at + 1] == '\'') { value += '\''; at += 2; }
                else { ++at; break; }
            }
            result.push_back(step_string(value));
        } else {
            const auto end = input.find_first_of(",);", at);
            result.push_back(trim(input.substr(at, end == std::string::npos ? end : end - at)));
            at = end == std::string::npos ? input.size() : end;
            if (at < input.size() && input[at] == ';') break;
        }
    }
    return result;
}
bool meaningful(const std::string& value)
{
    const auto v = lower(trim(value));
    return !v.empty() && v != "$" && v != "none" && v != "unknown" && v != "author" && v != "anonymous" && v != "not specified" && v != "undefined";
}
void thumbnail(FileMetadata& result, std::vector<std::uint8_t> bytes)
{
    if (bytes.size() < 8) return;
    std::string ext;
    if (bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') ext = "png";
    else if (bytes[0] == 255 && bytes[1] == 216 && bytes[2] == 255) ext = "jpg";
    if (!ext.empty()) { result.thumbnail = std::move(bytes); result.extension = ext; }
}
std::vector<std::uint8_t> base64(const std::string& input)
{
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out; unsigned bits = 0, value = 0;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const auto digit = alphabet.find(char(c));
        if (digit == std::string::npos) return {};
        value = (value << 6) | unsigned(digit); bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(std::uint8_t(value >> bits)); }
    }
    return out;
}
}

std::string plain_text(std::string value)
{
    for (unsigned pass = 0; pass < 2; ++pass) {
        replace(value, "&lt;", "<"); replace(value, "&gt;", ">"); replace(value, "&quot;", "\""); replace(value, "&#39;", "'"); replace(value, "&nbsp;", " "); replace(value, "&amp;", "&");
    }
    std::string out;
    for (size_t at = 0; at < value.size();) {
        if (value[at] == '<') {
            const auto end = value.find('>', at + 1);
            if (end == std::string::npos) { out += value.substr(at); break; }
            const auto tag = lower(value.substr(at + 1, end - at - 1));
            if (tag.rfind("script", 0) == 0 || tag.rfind("style", 0) == 0) {
                const auto close = lower(value).find(tag.rfind("script", 0) == 0 ? "</script>" : "</style>", end);
                at = close == std::string::npos ? value.size() : value.find('>', close) + 1;
            } else { out += (tag == "br" || tag == "br/" || tag == "/p" || tag == "/li" || tag == "/div") ? '\n' : ' '; at = end + 1; }
        } else out += value[at++];
    }
    return trim(out);
}

void append_print_metadata(Json& fields, const std::string& key, const std::string& value)
{
    static const std::map<std::string, std::string> keys{
        {"printer_model","Printer"},{"printer_settings_id","Printer"},{"print_settings_id","Process"},{"filament_type","Filament"},
        {"filament_colour","Colours"},{"filament_color","Colours"},{"layer_height","Layer height"},{"layer height","Layer height"},
        {"nozzle_diameter","Nozzle"},{"sparse_infill_density","Infill"},{"fill_density","Infill"},{"wall_loops","Walls"},{"perimeters","Walls"},
        {"enable_support","Supports"},{"support_material","Supports"},{"estimated printing time (normal mode)","Print time"},
        {"total estimated time","Print time"},{"model printing time","Print time"},{"time","Print time"},
        {"filament used [g]","Weight"},{"total filament used [g]","Weight"},{"total filament weight [g]","Weight"},{"producer","Sliced with"}
    };
    const auto found = keys.find(lower(trim(key)));
    if (found == keys.end() || trim(value).empty()) return;
    for (const auto& row : fields["meta"]) if (row["group"] == "Print" && row["key"] == found->second && row["value"] == trim(value)) return;
    fields["meta"].push_back({{"group","Print"},{"key",found->second},{"value",trim(value)}});
}

Json readme_metadata(const std::string& value)
{
    Json result = Json::object(); std::istringstream stream(value); std::string line;
    for (unsigned count = 0; count < 40 && std::getline(stream, line); ++count) {
        const auto site = line.find(" on Thingiverse: ");
        if (site != std::string::npos) {
            const auto by = line.rfind(" by ", site);
            if (by != std::string::npos) { result["title"] = trim(line.substr(0, by)); result["designer"] = trim(line.substr(by + 4, site - by - 4)); }
            result["source_url"] = trim(line.substr(site + 17)); result["source_site"] = "Thingiverse";
        }
        const auto license = line.find(" is licensed under the ");
        if (license != std::string::npos) {
            auto text = trim(line.substr(license + 23));
            if (!text.empty() && text.back() == '.') text.pop_back();
            if (text.size() >= 8 && text.substr(text.size() - 8) == " license") text.resize(text.size() - 8);
            result["license"] = text;
        }
    }
    return result;
}

Json step_metadata(const std::string& input)
{
    const std::string text = input.substr(0, 65536);
    const auto header = lower(text).find("header;");
    if (header == std::string::npos) throw std::runtime_error("No STEP header was found");
    Json result{{"meta", Json::array()}};
    for (const auto& name : {"FILE_NAME", "FILE_DESCRIPTION", "FILE_SCHEMA"}) {
        auto at = lower(text).find(lower(name), header);
        if (at == std::string::npos) continue;
        at = text.find('(', at); if (at == std::string::npos) continue; ++at;
        const auto args = step_args(text, at);
        const auto add = [&](size_t i, const std::string& key) { if (i < args.size() && meaningful(args[i])) result["meta"].push_back({{"group","File"},{"key",key},{"value",args[i]}}); };
        if (std::string(name) == "FILE_NAME") {
            const std::vector<std::string> labels{"Name","Timestamp","Author","Organization","Preprocessor","Originating system"};
            for (size_t i = 0; i < labels.size(); ++i) add(i, labels[i]);
            if (args.size() > 2 && meaningful(args[2])) result["designer"] = args[2];
            if (!args.empty()) {
                auto base = args[0].substr(args[0].find_last_of("/\\") == std::string::npos ? 0 : args[0].find_last_of("/\\") + 1);
                base = fs::u8path(base).stem().u8string(); const auto folded = lower(base);
                if (meaningful(base) && folded.find("open cascade") == std::string::npos && folded != "untitled" && folded != "model" && folded != "part" && folded != "assembly" && folded != "shape" && folded != "body") result["title"] = base;
            }
        } else add(0, std::string(name) == "FILE_SCHEMA" ? "Schema" : "Description");
    }
    return result;
}

FileMetadata gcode_metadata(const std::string& input)
{
    FileMetadata result; std::istringstream stream(input); std::string line, encoded; bool active = false; std::uint64_t area = 0, best = 0;
    while (std::getline(stream, line)) {
        line = trim(line); const auto folded = lower(line);
        if (folded.rfind("thumbnail", 0) == 0 && folded.find(" begin ") != std::string::npos) {
            const auto at = folded.find(" begin "); std::istringstream dims(folded.substr(at + 7)); unsigned w = 0, h = 0; char x = 0;
            dims >> w >> x >> h; area = x == 'x' ? std::uint64_t(w) * h : 0; active = folded.find("qoi") == std::string::npos; encoded.clear(); continue;
        }
        if (active) {
            if (folded.rfind("thumbnail", 0) == 0 && folded.find(" end") != std::string::npos) {
                if (area > best) { auto bytes = base64(encoded); if (!bytes.empty()) { thumbnail(result, std::move(bytes)); best = area; } }
                active = false;
            } else if (encoded.size() + line.size() <= 16 * 1024 * 1024) encoded += line;
            else active = false;
            continue;
        }
        auto split = line.find('='); if (split == std::string::npos) split = line.find(':');
        if (split != std::string::npos) append_print_metadata(result.fields, line.substr(0, split), line.substr(split + 1));
    }
    return result;
}

FileMetadata bgcode_metadata(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 10 || std::string(bytes.begin(), bytes.begin() + 4) != "GCDE") throw std::runtime_error("Invalid binary G-code header");
    const auto integer = [&](size_t at, size_t size) { if (at + size > bytes.size()) throw std::runtime_error("Truncated binary G-code header"); unsigned value = 0; for (size_t i = 0; i < size; ++i) value |= unsigned(bytes[at + i]) << (i * 8); return value; };
    const auto checksum = integer(8, 2); if (checksum > 1) throw std::runtime_error("Unsupported binary G-code checksum");
    FileMetadata result; size_t at = 10; std::uint64_t best = 0;
    while (at + 8 <= bytes.size()) {
        const unsigned type = integer(at, 2), compression = integer(at + 2, 2), expanded = integer(at + 4, 4); at += 8;
        if (type == 1 || type > 5) break;
        const auto size = compression ? integer(at, 4) : expanded; if (compression) at += 4;
        const auto params = at; at += type == 5 ? 6 : 2;
        if (at > bytes.size() || size > bytes.size() - at || expanded > 64 * 1024 * 1024) break;
        std::vector<std::uint8_t> payload;
        if (compression == 0) payload.assign(bytes.begin() + at, bytes.begin() + at + size);
        else if (compression == 1) {
            payload.resize(expanded);
            auto count = tinfl_decompress_mem_to_mem(payload.data(), payload.size(), bytes.data() + at, size, TINFL_FLAG_PARSE_ZLIB_HEADER);
            if (count == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) count = tinfl_decompress_mem_to_mem(payload.data(), payload.size(), bytes.data() + at, size, 0);
            if (count == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) payload.clear(); else payload.resize(count);
        }
        at += size + (checksum == 1 ? 4 : 0);
        if (type == 5) {
            const auto format = integer(params, 2); const auto area = std::uint64_t(integer(params + 2, 2)) * integer(params + 4, 2);
            if (format <= 1 && area > best) { thumbnail(result, std::move(payload)); best = area; }
        } else if (integer(params, 2) == 0) {
            auto parsed = gcode_metadata(std::string(payload.begin(), payload.end()));
            for (const auto& row : parsed.fields["meta"]) result.fields["meta"].push_back(row);
        }
    }
    return result;
}
}
