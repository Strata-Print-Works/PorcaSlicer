#include "Extract.hpp"
#include "TextMetadata.hpp"
#include "Source.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Point.hpp"

#include <blake3.h>
#include <miniz.h>
#include <cstdio>
#include <csetjmp>
#include <jpeglib.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::ModelLibrary {
namespace {
void little_endian(unsigned char* bytes, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
}
std::array<unsigned char, 32> digest(const void* data, size_t size)
{
    blake3_hasher hash;
    blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, data, size);
    std::array<unsigned char, 32> result{};
    blake3_hasher_finalize(&hash, result.data(), result.size());
    return result;
}
std::string trim(std::string value)
{
    const auto begin = value.find_first_not_of(" \t\r\n;");
    return begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
std::string flat(const Json& value)
{
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) {
        std::string result;
        for (const auto& item : value) { if (!result.empty()) result += ", "; result += flat(item); }
        return result;
    }
    return value.dump();
}
void print_metadata(Json& result, const std::string& key, const std::string& value)
{
    append_print_metadata(result, key, value);
}
std::string prefix(const fs::path& path, size_t limit)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not read file metadata");
    std::string content(std::min<std::uintmax_t>(fs::file_size(path), limit), '\0');
    stream.read(content.data(), std::streamsize(content.size())); content.resize(size_t(stream.gcount()));
    return content;
}

struct JpegError { jpeg_error_mgr base; std::jmp_buf jump; };
void jpeg_failure(j_common_ptr decoder) { std::longjmp(reinterpret_cast<JpegError*>(decoder->err)->jump, 1); }
PreviewPixels decode_preview(const std::string& bytes, unsigned edge)
{
    PreviewPixels result;
    if (bytes.size() < 24 || edge == 0 || edge > 2048) return result;
    std::vector<std::uint8_t> rgb;
    unsigned width = 0, height = 0;
    if (bytes.compare(0,8,"\x89PNG\r\n\x1a\n",8) == 0) {
        const auto number = [&](size_t at) { unsigned value = 0; for (unsigned i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[at + i]); return value; };
        width = number(16); height = number(20);
        if (!width || !height || width > 8192 || height > 8192 || std::uint64_t(width) * height > 16000000) return result;
        png::ImageColorscale decoded;
        if (!png::decode_colored_png(png::ReadBuf{bytes.data(),bytes.size()},decoded) || decoded.bytes_per_pixel < 1 || decoded.bytes_per_pixel > 4 || decoded.cols != width || decoded.rows != height) return result;
        rgb.resize(size_t(width) * height * 3);
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            const auto* pixel = &decoded.buf[i * decoded.bytes_per_pixel];
            const unsigned alpha = decoded.bytes_per_pixel == 2 || decoded.bytes_per_pixel == 4 ? pixel[decoded.bytes_per_pixel - 1] : 255;
            for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = std::uint8_t((unsigned(pixel[decoded.bytes_per_pixel < 3 ? 0 : c]) * alpha + 255 * (255 - alpha)) / 255);
        }
    } else if (static_cast<unsigned char>(bytes[0]) == 255 && static_cast<unsigned char>(bytes[1]) == 216) {
        struct DecodeState { jpeg_decompress_struct decoder{}; JpegError error{}; std::vector<std::uint8_t> pixels; };
        auto state = std::make_unique<DecodeState>();
        auto& decoder = state->decoder; auto& error = state->error;
        decoder.err = jpeg_std_error(&error.base); error.base.error_exit = jpeg_failure;
        if (setjmp(error.jump)) { jpeg_destroy_decompress(&decoder); return result; }
        jpeg_create_decompress(&decoder);
        jpeg_mem_src(&decoder,reinterpret_cast<const unsigned char*>(bytes.data()),static_cast<unsigned long>(bytes.size()));
        jpeg_read_header(&decoder,TRUE);
        if (!decoder.image_width || !decoder.image_height || decoder.image_width > 8192 || decoder.image_height > 8192 || std::uint64_t(decoder.image_width) * decoder.image_height > 16000000) { jpeg_destroy_decompress(&decoder); return result; }
        decoder.out_color_space = JCS_RGB; jpeg_start_decompress(&decoder);
        state->pixels.resize(size_t(decoder.output_width) * decoder.output_height * 3);
        while (decoder.output_scanline < decoder.output_height) { JSAMPROW row = &state->pixels[size_t(decoder.output_scanline) * decoder.output_width * 3]; jpeg_read_scanlines(&decoder,&row,1); }
        jpeg_finish_decompress(&decoder);
        width = decoder.output_width; height = decoder.output_height;
        jpeg_destroy_decompress(&decoder); rgb = std::move(state->pixels);
    } else return result;
    if (rgb.empty()) return result;
    bool varied = false;
    for (size_t i = 3; i < rgb.size(); i += 3) for (unsigned c = 0; c < 3; ++c) if (std::abs(int(rgb[i + c]) - int(rgb[c])) > 8) varied = true;
    if (!varied) return result;
    const double scale = std::min(1.0, double(edge) / std::max(width,height));
    result.width = std::max(1u,unsigned(width * scale)); result.height = std::max(1u,unsigned(height * scale));
    result.rgb.resize(size_t(result.width) * result.height * 3);
    for (unsigned y = 0; y < result.height; ++y) for (unsigned x = 0; x < result.width; ++x) {
        const size_t source = (size_t(y) * height / result.height * width + size_t(x) * width / result.width) * 3;
        std::copy_n(rgb.data() + source,3,result.rgb.data() + (size_t(y) * result.width + x) * 3);
    }
    return result;
}

void archive_metadata(const fs::path& path, const std::string& hash, const fs::path& root, Json& result)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.u8string().c_str(), 0)) throw std::runtime_error("Could not read the 3MF archive");
    struct Close { mz_zip_archive* zip; ~Close() { mz_zip_reader_end(zip); } } close{&zip};
    PreviewPixels cover;
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        const std::string name = stat.m_filename;
        const auto ext = Library::format(fs::u8path(name));
        if (ext == "gcode") result["sliced"] = true;
        if ((ext == "png" || ext == "jpg" || ext == "jpeg") && stat.m_uncomp_size <= 16 * 1024 * 1024) {
            std::string bytes(size_t(stat.m_uncomp_size), '\0');
            if (mz_zip_reader_extract_to_mem(&zip,i,bytes.data(),bytes.size(),0)) {
                auto image = decode_preview(bytes,1024);
                if (std::uint64_t(image.width) * image.height > std::uint64_t(cover.width) * cover.height) cover = std::move(image);
            }
        }
        if (name == "Metadata/Slic3r_PE.config" && stat.m_uncomp_size <= 4 * 1024 * 1024) {
            std::string bytes(size_t(stat.m_uncomp_size), '\0');
            if (mz_zip_reader_extract_to_mem(&zip,i,bytes.data(),bytes.size(),0)) { const auto parsed = gcode_metadata(bytes); for (const auto& row : parsed.fields["meta"]) result["meta"].push_back(row); }
        }
        if (name == "Metadata/project_settings.config" && stat.m_uncomp_size <= 4 * 1024 * 1024) {
            std::string content(size_t(stat.m_uncomp_size), '\0');
            if (mz_zip_reader_extract_to_mem(&zip, i, content.data(), content.size(), 0)) {
                auto settings = Json::parse(content, nullptr, false);
                if (settings.is_object()) for (auto it = settings.begin(); it != settings.end(); ++it) print_metadata(result, it.key(), flat(it.value()));
            }
        }
        if ((name == "3D/3dmodel.model" || name == "3D/3DModel.model") && stat.m_uncomp_size <= 32 * 1024 * 1024) {
            std::string content(size_t(stat.m_uncomp_size), '\0');
            if (mz_zip_reader_extract_to_mem(&zip, i, content.data(), content.size(), 0)) {
                try {
                    boost::property_tree::ptree xml;
                    std::istringstream stream(content);
                    boost::property_tree::read_xml(stream, xml);
                    if (auto model = xml.get_child_optional("model")) for (const auto& [type, node] : *model) if (type == "metadata") {
                        const auto key = node.get<std::string>("<xmlattr>.name", "");
                        const auto value = node.data();
                        if (value.empty()) continue;
                        const std::map<std::string, std::string> keys{{"Title", "title"}, {"Designer", "designer"}, {"DesignerUserName", "designer"}, {"Description", "description"}, {"License", "license"}, {"LicenseName", "license"}, {"LicenseTerms", "license"}, {"Author", "designer"}, {"Creator", "designer"}, {"ModelOrigin", "source_url"}};
                        const auto found = keys.find(key);
                        if (found != keys.end()) result[found->second] = found->second == "description" ? plain_text(value) : value;
                        if (key == "ModelOrigin" && !origin_label(value).empty()) result["source_site"] = origin_label(value);
                        if (key == "DesignModelId" && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
                            result["source_site"] = "MakerWorld";
                            if (!result.contains("source_url")) result["source_url"] = "https://makerworld.com/en/models/" + value;
                        }
                        result["meta"].push_back({{"group", "Model"}, {"key", key}, {"value", value}});
                    }
                } catch (const boost::property_tree::xml_parser_error&) {}
            }
        }
    }
    if (!cover.rgb.empty()) {
        const std::string relative = "thumbs/" + hash + ".png";
        if (png::write_rgb_to_file((root / fs::u8path(relative)).u8string(),cover.width,cover.height,cover.rgb)) result["thumb"] = relative;
    }
}
} // namespace

PreviewPixels read_preview(const fs::path& path, unsigned edge)
{
    if (!fs::is_regular_file(path) || fs::file_size(path) > 16 * 1024 * 1024) return {};
    return decode_preview(prefix(path,16 * 1024 * 1024),edge);
}

std::string geometry_fingerprint(const indexed_triangle_set& mesh)
{
    if (mesh.indices.empty()) return {};
    Vec3d minimum = Vec3d::Constant(std::numeric_limits<double>::infinity());
    Vec3d maximum = -minimum;
    bool surface = false;
    for (const auto& triangle : mesh.indices) {
        for (int index : triangle) {
            if (index < 0 || size_t(index) >= mesh.vertices.size()) return {};
            const Vec3d p = mesh.vertices[size_t(index)].cast<double>();
            if (!p.allFinite()) return {};
            minimum = minimum.cwiseMin(p); maximum = maximum.cwiseMax(p);
        }
        surface |= (mesh.vertices[size_t(triangle[1])] - mesh.vertices[size_t(triangle[0])]).cross(mesh.vertices[size_t(triangle[2])] - mesh.vertices[size_t(triangle[0])]).squaredNorm() > 0;
    }
    if (!surface || (maximum - minimum).maxCoeff() > 1e12) return {};
    std::array<std::int64_t, 3> size{};
    for (unsigned a = 0; a < 3; ++a) size[a] = std::llround((maximum[a] - minimum[a]) * 10000);
    auto sorted_size = size; std::sort(sorted_size.begin(), sorted_size.end());
    std::array<unsigned char, 32> canonical{}; canonical.fill(255);
    const std::array<std::array<int, 3>, 6> permutations{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}};
    const int parities[]{1,-1,-1,1,1,-1};
    for (size_t permutation = 0; permutation < permutations.size(); ++permutation) {
        const auto& axes = permutations[permutation];
        if (std::array<std::int64_t, 3>{size[axes[0]], size[axes[1]], size[axes[2]]} != sorted_size) continue;
        for (unsigned signs = 0; signs < 8; ++signs) {
            const int sign[]{signs & 1 ? -1 : 1, signs & 2 ? -1 : 1, signs & 4 ? -1 : 1};
            if (parities[permutation] * sign[0] * sign[1] * sign[2] != 1) continue;
            std::array<std::uint64_t, 4> sum{};
            for (const auto& triangle : mesh.indices) {
                std::array<std::array<std::int64_t, 3>, 3> normalized{};
                for (unsigned v = 0; v < 3; ++v) for (unsigned a = 0; a < 3; ++a) {
                    const int from = axes[a];
                    const double value = mesh.vertices[size_t(triangle[v])][from];
                    normalized[v][a] = std::llround((sign[a] > 0 ? value - minimum[from] : maximum[from] - value) * 10000);
                }
                std::sort(normalized.begin(), normalized.end());
                std::array<unsigned char, 80> bytes{};
                std::copy_n("strataT1", 8, bytes.begin());
                for (unsigned v = 0; v < 3; ++v) for (unsigned a = 0; a < 3; ++a) little_endian(bytes.data() + 8 + (v * 3 + a) * 8, std::uint64_t(normalized[v][a]));
                const auto hash = digest(bytes.data(), bytes.size());
                bool carry = false;
                for (unsigned limb = 0; limb < 4; ++limb) {
                    std::uint64_t value = 0;
                    for (unsigned b = 0; b < 8; ++b) value |= std::uint64_t(hash[limb * 8 + b]) << (b * 8);
                    const auto added = sum[limb] + value;
                    const bool overflow = added < sum[limb];
                    sum[limb] = added + carry;
                    carry = overflow || sum[limb] < added;
                }
            }
            const char domain[] = "strata:triangle-multiset:v1:0.0001mm";
            std::vector<unsigned char> bytes(domain, domain + sizeof(domain));
            const auto offset = bytes.size(); bytes.resize(offset + 40);
            little_endian(bytes.data() + offset, mesh.indices.size());
            for (unsigned limb = 0; limb < 4; ++limb) little_endian(bytes.data() + offset + 8 + limb * 8, sum[limb]);
            canonical = std::min(canonical, digest(bytes.data(), bytes.size()));
        }
    }
    std::ostringstream result; result << "mesh-v1:" << std::hex << std::setfill('0');
    for (unsigned char c : canonical) result << std::setw(2) << unsigned(c);
    return result.str();
}

std::vector<std::uint8_t> render_mesh(const indexed_triangle_set& mesh, unsigned edge, double yaw, double pitch, const Cancel& cancel)
{
    edge = std::clamp(edge, 32u, 1024u);
    std::vector<std::uint8_t> image(size_t(edge) * edge * 3, 242);
    if (mesh.vertices.empty() || mesh.indices.empty()) return image;
    std::vector<Vec3d> points;
    points.reserve(mesh.vertices.size());
    Vec3d lo = Vec3d::Constant(std::numeric_limits<double>::infinity()), hi = -lo;
    for (const auto& v : mesh.vertices) {
        const double x = std::cos(yaw) * v.x() - std::sin(yaw) * v.y();
        const double y = std::sin(yaw) * v.x() + std::cos(yaw) * v.y();
        Vec3d p(x, std::sin(pitch) * y - std::cos(pitch) * v.z(), std::cos(pitch) * y + std::sin(pitch) * v.z());
        if (!p.allFinite()) return image;
        lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); points.push_back(p);
    }
    const double scale = edge * 0.85 / std::max({hi.x() - lo.x(), hi.y() - lo.y(), 1e-6});
    const Vec3d center = (lo + hi) * 0.5;
    for (auto& p : points) { p -= center; p *= scale; p.x() += edge * 0.5; p.y() += edge * 0.5; }
    std::vector<double> depth(size_t(edge) * edge, -std::numeric_limits<double>::infinity());
    const size_t stride = std::max<size_t>(1,(mesh.indices.size() + 99999) / 100000);
    std::uint64_t work = 0;
    for (size_t face = 0; face < mesh.indices.size(); face += stride) {
        if (cancel && cancel()) return image;
        const auto& triangle = mesh.indices[face];
        if (triangle.minCoeff() < 0 || size_t(triangle.maxCoeff()) >= points.size()) continue;
        const Vec3d& a = points[size_t(triangle[0])]; const Vec3d& b = points[size_t(triangle[1])]; const Vec3d& c = points[size_t(triangle[2])];
        const double area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
        if (std::abs(area) < 1e-9) continue;
        const Vec3d normal = (b - a).cross(c - a).normalized();
        const double light = 0.35 + 0.65 * std::abs(normal.dot(Vec3d(-0.3, -0.5, 0.8).normalized()));
        const int min_x = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
        const int max_x = std::min(int(edge) - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
        const int min_y = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
        const int max_y = std::min(int(edge) - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
        work += std::uint64_t(std::max(0,max_x - min_x + 1)) * std::max(0,max_y - min_y + 1);
        if (work > 32000000) break; // Bound software preview latency, never geometry identity.
        for (int y = min_y; y <= max_y; ++y) for (int x = min_x; x <= max_x; ++x) {
            const double u = ((b.x() - x - 0.5) * (c.y() - y - 0.5) - (b.y() - y - 0.5) * (c.x() - x - 0.5)) / area;
            const double v = ((c.x() - x - 0.5) * (a.y() - y - 0.5) - (c.y() - y - 0.5) * (a.x() - x - 0.5)) / area;
            const double w = 1 - u - v;
            if (u < 0 || v < 0 || w < 0) continue;
            const double z = u * a.z() + v * b.z() + w * c.z();
            const auto pixel = size_t(y) * edge + size_t(x);
            if (z <= depth[pixel]) continue;
            depth[pixel] = z;
            image[pixel * 3] = std::uint8_t(225 * light); image[pixel * 3 + 1] = std::uint8_t(116 * light); image[pixel * 3 + 2] = std::uint8_t(92 * light);
        }
    }
    return image;
}

Json extract_file(const fs::path& path, const std::string& hash, const fs::path& root)
{
    Json result{{"meta", Json::array()}};
    const auto format = Library::format(path);
    const auto kind = Library::file_kind(path);
    if (kind == "image") {
        if (fs::file_size(path) <= 16 * 1024 * 1024) {
            const auto relative = "thumbs/" + hash + '.' + format;
            const auto target = root / fs::u8path(relative);
            if (!fs::exists(target)) fs::copy_file(path, target);
            result["thumb"] = relative;
        }
        return result;
    }
    if (kind == "document" || kind == "doc") {
        result.update(readme_metadata(prefix(path, 65536)));
        return result;
    }
    if (kind != "model") return result;
    if (format == "3mf") archive_metadata(path, hash, root, result);
    if (format == "gcode" || format == "bgcode") {
        FileMetadata metadata;
        if (format == "bgcode") {
            const auto data = prefix(path, 32 * 1024 * 1024);
            metadata = bgcode_metadata(std::vector<std::uint8_t>(data.begin(), data.end()));
        } else {
            auto content = prefix(path, 1024 * 1024);
            const auto size = fs::file_size(path);
            if (size > 1024 * 1024) {
                std::ifstream stream(path, std::ios::binary); stream.seekg(std::streamoff(size - std::min<std::uintmax_t>(size, 1024 * 1024)));
                std::string tail(1024 * 1024, '\0'); stream.read(tail.data(), std::streamsize(tail.size())); tail.resize(size_t(stream.gcount()));
                const auto newline = tail.find('\n'); if (newline != std::string::npos) content += '\n' + tail.substr(newline + 1);
            }
            metadata = gcode_metadata(content);
        }
        result.update(metadata.fields);
        if (!metadata.thumbnail.empty()) {
            const auto relative = "thumbs/" + hash + '.' + metadata.extension;
            std::ofstream stream(root / fs::u8path(relative), std::ios::binary);
            stream.write(reinterpret_cast<const char*>(metadata.thumbnail.data()), std::streamsize(metadata.thumbnail.size()));
            if (stream) result["thumb"] = relative;
        }
        return result;
    }
    if (format == "step") { result.update(step_metadata(prefix(path, 65536))); return result; }
    try {
        auto mesh = read_mesh(path);
        if (mesh.its.indices.empty()) return result;
        const auto size = mesh.bounding_box().size();
        result["triangles"] = mesh.its.indices.size(); result["vertices"] = mesh.its.vertices.size();
        result["size_x"] = size.x(); result["size_y"] = size.y(); result["size_z"] = size.z();
        result["volume"] = std::abs(mesh.volume());
        double area = 0;
        for (const auto& t : mesh.its.indices) area += 0.5 * double((mesh.its.vertices[size_t(t[1])] - mesh.its.vertices[size_t(t[0])]).cross(mesh.its.vertices[size_t(t[2])] - mesh.its.vertices[size_t(t[0])]).norm());
        result["area"] = area;
        if (mesh.its.indices.size() <= 2000000) {
            const auto fingerprint = geometry_fingerprint(mesh.its);
            if (!fingerprint.empty()) { result["geometry_hash"] = fingerprint; result["geometry_revision"] = 2; }
        }
        if (!result.contains("thumb")) {
            const auto relative = "thumbs/" + hash + ".png";
            const auto pixels = render_mesh(mesh.its, 256);
            if (png::write_rgb_to_file((root / fs::u8path(relative)).u8string(), 256, 256, pixels)) result["thumb"] = relative;
        }
    } catch (const std::exception& error) { result["error"] = error.what(); }
    return result;
}
} // namespace Slic3r::ModelLibrary
