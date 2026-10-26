#include "Extract.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <igl/readPLY.h>
#include <miniz.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Slic3r::ModelLibrary {
namespace {
using Tree = boost::property_tree::ptree;
constexpr size_t max_triangles = 4000000;
std::string local(const std::string& name) { return name.substr(name.find(':') == std::string::npos ? 0 : name.find(':') + 1); }
std::string attr(const Tree& tree, const std::string& key, const std::string& fallback = "")
{
    if (const auto attributes = tree.get_child_optional("<xmlattr>")) for (const auto& pair : *attributes) if (local(pair.first) == key) return pair.second.data();
    return fallback;
}
const Tree* child(const Tree& tree, const std::string& name)
{
    for (const auto& pair : tree) if (local(pair.first) == name) return &pair.second;
    return nullptr;
}
Tree xml(const std::string& content)
{
    if (content.find("<!DOCTYPE") != std::string::npos || content.find("<!ENTITY") != std::string::npos) throw std::runtime_error("XML declarations are not supported in model previews");
    Tree result; std::istringstream stream(content); boost::property_tree::read_xml(stream,result); return result;
}
double unit(const std::string& name)
{
    if (name == "micron") return 0.001;
    if (name == "centimeter") return 10;
    if (name == "meter") return 1000;
    if (name == "inch") return 25.4;
    if (name == "foot" || name == "feet") return 304.8;
    if (name.empty() || name == "millimeter") return 1;
    throw std::runtime_error("Unknown model unit");
}
Transform3d transform(const std::string& value)
{
    Transform3d matrix = Transform3d::Identity(); if (value.empty()) return matrix;
    std::istringstream stream(value);
    for (unsigned column = 0; column < 4; ++column) for (unsigned row = 0; row < 3; ++row) if (!(stream >> matrix(row,column)) || !std::isfinite(matrix(row,column))) throw std::runtime_error("Invalid model placement");
    return matrix;
}
TriangleMesh checked_mesh(indexed_triangle_set geometry)
{
    if (geometry.indices.size() > max_triangles) throw std::runtime_error("This model exceeds the library preview limit");
    for (const auto& vertex : geometry.vertices) if (!vertex.allFinite()) throw std::runtime_error("The model has invalid coordinates");
    for (const auto& triangle : geometry.indices) for (int index : triangle) if (index < 0 || size_t(index) >= geometry.vertices.size()) throw std::runtime_error("The model has invalid face references");
    return TriangleMesh(std::move(geometry));
}
void merge(TriangleMesh& output, const TriangleMesh& part, const Transform3d& matrix)
{
    if (part.its.indices.size() + output.its.indices.size() > max_triangles) throw std::runtime_error("This model exceeds the library preview limit");
    auto transformed = part; transformed.transform(matrix); output.merge(transformed);
}
struct Archive {
    mz_zip_archive value{};
    explicit Archive(const fs::path& path) { if (!mz_zip_reader_init_file(&value,path.u8string().c_str(),0)) throw std::runtime_error("Could not read model archive"); }
    ~Archive() { mz_zip_reader_end(&value); }
    std::string read(const std::string& name, size_t limit = 64 * 1024 * 1024) {
        const int index = mz_zip_reader_locate_file(&value,name.c_str(),nullptr,0); if (index < 0) throw std::runtime_error("A model part is missing: " + name);
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&value,unsigned(index),&stat) || stat.m_uncomp_size > limit) throw std::runtime_error("This model part exceeds the library preview limit");
        std::string text(size_t(stat.m_uncomp_size),'\0');
        if (!mz_zip_reader_extract_to_mem(&value,unsigned(index),text.data(),text.size(),0)) throw std::runtime_error("Could not read model part");
        return text;
    }
};
std::string package_path(const std::string& path, const std::string& from = "")
{
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) throw std::runtime_error("Invalid model part path");
    auto value = fs::u8path(path);
    if (value.is_absolute()) value = value.relative_path(); else if (!from.empty()) value = fs::u8path(from).parent_path() / value;
    value = value.lexically_normal();
    for (const auto& component : value) if (component == "..") throw std::runtime_error("Invalid model part path");
    return value.generic_u8string();
}
TriangleMesh read_stl(const fs::path& path)
{
    std::ifstream stream(path,std::ios::binary);
    std::array<unsigned char,84> header{}; stream.read(reinterpret_cast<char*>(header.data()),header.size());
    const auto number = [](const unsigned char* bytes) { return unsigned(bytes[0]) | (unsigned(bytes[1]) << 8) | (unsigned(bytes[2]) << 16) | (unsigned(bytes[3]) << 24); };
    const auto count = number(header.data() + 80);
    indexed_triangle_set geometry;
    if (stream.gcount() == 84 && count > 0 && std::uint64_t(count) * 50 + 84 == fs::file_size(path)) {
        if (count > max_triangles) throw std::runtime_error("This STL exceeds the library preview limit");
        geometry.vertices.reserve(size_t(count) * 3); geometry.indices.reserve(count);
        for (unsigned face = 0; face < count; ++face) {
            std::array<unsigned char,50> bytes{}; stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
            if (!stream) throw std::runtime_error("The STL is incomplete");
            for (unsigned vertex = 0; vertex < 3; ++vertex) {
                Vec3f position;
                for (unsigned axis = 0; axis < 3; ++axis) { const auto bits = number(bytes.data() + 12 + vertex * 12 + axis * 4); std::memcpy(&position[axis],&bits,4); }
                geometry.vertices.push_back(position);
            }
            const int begin = int(geometry.vertices.size()) - 3; geometry.indices.emplace_back(begin,begin + 1,begin + 2);
        }
    } else {
        stream.clear(); stream.seekg(0); std::string word; unsigned vertices = 0;
        while (stream >> word) {
            if (word == "vertex") {
                Vec3f position; if (!(stream >> position.x() >> position.y() >> position.z()) || ++vertices > 3) throw std::runtime_error("Invalid STL facet");
                geometry.vertices.push_back(position);
            } else if (word == "endfacet") {
                if (vertices != 3) throw std::runtime_error("Incomplete STL facet");
                const int begin = int(geometry.vertices.size()) - 3; geometry.indices.emplace_back(begin,begin + 1,begin + 2); vertices = 0;
                if (geometry.indices.size() > max_triangles) throw std::runtime_error("This STL exceeds the library preview limit");
            }
        }
        if (vertices) throw std::runtime_error("The STL is incomplete");
    }
    return checked_mesh(std::move(geometry));
}
TriangleMesh read_3mf(const fs::path& path)
{
    Archive archive(path); std::string root = "3D/3dmodel.model";
    try {
        const auto relationships = xml(archive.read("_rels/.rels",1024 * 1024));
        if (const auto rels = child(relationships,"Relationships")) for (const auto& pair : *rels)
            if (local(pair.first) == "Relationship" && attr(pair.second,"Type").find("/3dmodel") != std::string::npos) root = package_path(attr(pair.second,"Target"));
    } catch (const std::exception&) { /* Standard root remains the fallback. */ }
    std::set<std::pair<std::string,std::string>> excluded;
    if (mz_zip_reader_locate_file(&archive.value,"Metadata/model_settings.config",nullptr,0) >= 0) {
        const auto settings = xml(archive.read("Metadata/model_settings.config",8 * 1024 * 1024));
        if (const auto config = child(settings,"config")) for (const auto& object : *config) if (local(object.first) == "object") for (const auto& part : object.second) if (local(part.first) == "part") {
            const auto subtype = attr(part.second,"subtype");
            if (!subtype.empty() && subtype != "normal_part") excluded.emplace(attr(object.second,"id"),attr(part.second,"id"));
        }
    }
    std::map<std::string,Tree> documents;
    std::map<std::pair<std::string,std::string>,TriangleMesh> cache;
    std::set<std::pair<std::string,std::string>> active;
    const auto document = [&](const std::string& name) -> const Tree& {
        auto found = documents.find(name);
        if (found == documents.end()) {
            if (documents.size() >= 256) throw std::runtime_error("This model has too many parts");
            found = documents.emplace(name,xml(archive.read(name))).first;
        }
        const auto model = child(found->second,"model"); if (!model) throw std::runtime_error("A 3MF model part has no model element"); return *model;
    };
    std::function<const TriangleMesh&(const std::string&,const std::string&,unsigned)> object;
    object = [&](const std::string& name,const std::string& id,unsigned depth) -> const TriangleMesh& {
        const auto key = std::make_pair(name,id);
        if (const auto found = cache.find(key); found != cache.end()) return found->second;
        if (depth > 64 || !active.insert(key).second) throw std::runtime_error("The model contains a circular component reference");
        const auto resources = child(document(name),"resources"); if (!resources) throw std::runtime_error("A model part has no resources");
        const Tree* node = nullptr;
        for (const auto& entry : *resources) if (local(entry.first) == "object" && attr(entry.second,"id") == id) { node = &entry.second; break; }
        if (!node) throw std::runtime_error("A referenced object is missing");
        TriangleMesh mesh;
        if (const auto geometry = child(*node,"mesh")) {
            indexed_triangle_set triangles;
            const auto vertices = child(*geometry,"vertices"); const auto faces = child(*geometry,"triangles");
            if (!vertices || !faces) throw std::runtime_error("An object has incomplete geometry");
            for (const auto& entry : *vertices) if (local(entry.first) == "vertex") {
                triangles.vertices.emplace_back(std::stof(attr(entry.second,"x")),std::stof(attr(entry.second,"y")),std::stof(attr(entry.second,"z")));
                if (triangles.vertices.size() > max_triangles * 3) throw std::runtime_error("This model exceeds the library preview limit");
            }
            for (const auto& entry : *faces) if (local(entry.first) == "triangle") {
                triangles.indices.emplace_back(std::stoi(attr(entry.second,"v1")),std::stoi(attr(entry.second,"v2")),std::stoi(attr(entry.second,"v3")));
                if (triangles.indices.size() > max_triangles) throw std::runtime_error("This model exceeds the library preview limit");
            }
            mesh = checked_mesh(std::move(triangles));
        }
        if (const auto components = child(*node,"components")) for (const auto& component : *components) if (local(component.first) == "component") {
            const auto next_id = attr(component.second,"objectid"); if (excluded.count({id,next_id})) continue;
            const auto reference = attr(component.second,"path"); const auto next = reference.empty() ? name : package_path(reference,name);
            merge(mesh,object(next,next_id,depth + 1),transform(attr(component.second,"transform")));
        }
        active.erase(key); return cache.emplace(key,std::move(mesh)).first->second;
    };
    const auto& model = document(root); const auto build = child(model,"build");
    if (!build) throw std::runtime_error("The model has no build items");
    TriangleMesh result;
    for (const auto& item : *build) if (local(item.first) == "item") {
        const auto reference = attr(item.second,"path"); const auto name = reference.empty() ? root : package_path(reference,root);
        merge(result,object(name,attr(item.second,"objectid"),0),transform(attr(item.second,"transform")));
    }
    result.scale(float(unit(attr(model,"unit")))); return result;
}
TriangleMesh read_amf(const fs::path& path)
{
    std::ifstream stream(path,std::ios::binary); std::string content;
    char magic[2]{}; stream.read(magic,2); stream.seekg(0);
    if (magic[0] == 'P' && magic[1] == 'K') {
        Archive archive(path);
        for (unsigned i = 0; i < mz_zip_reader_get_num_files(&archive.value); ++i) { mz_zip_archive_file_stat stat{}; if (mz_zip_reader_file_stat(&archive.value,i,&stat) && Library::format(fs::u8path(stat.m_filename)) == "amf") { content = archive.read(stat.m_filename); break; } }
    } else { if (fs::file_size(path) > 64 * 1024 * 1024) throw std::runtime_error("This AMF exceeds the library preview limit"); content.assign(std::istreambuf_iterator<char>(stream),{}); }
    const auto tree = xml(content); const auto amf = child(tree,"amf"); if (!amf) throw std::runtime_error("No AMF model was found");
    std::map<std::string,TriangleMesh> objects;
    for (const auto& entry : *amf) if (local(entry.first) == "object") {
        indexed_triangle_set geometry; const auto mesh = child(entry.second,"mesh"); if (!mesh) throw std::runtime_error("An AMF object has no mesh");
        if (const auto vertices = child(*mesh,"vertices")) for (const auto& vertex : *vertices) if (local(vertex.first) == "vertex") {
            const auto coordinates = child(vertex.second,"coordinates"); if (!coordinates) throw std::runtime_error("An AMF vertex has no coordinates");
            geometry.vertices.emplace_back(coordinates->get<float>("x"),coordinates->get<float>("y"),coordinates->get<float>("z"));
        }
        for (const auto& volume : *mesh) if (local(volume.first) == "volume") for (const auto& triangle : volume.second) if (local(triangle.first) == "triangle") geometry.indices.emplace_back(triangle.second.get<int>("v1"),triangle.second.get<int>("v2"),triangle.second.get<int>("v3"));
        objects.emplace(attr(entry.second,"id"),checked_mesh(std::move(geometry)));
    }
    TriangleMesh result; bool instanced = false;
    for (const auto& constellation : *amf) if (local(constellation.first) == "constellation") for (const auto& instance : constellation.second) if (local(instance.first) == "instance") {
        const auto found = objects.find(attr(instance.second,"objectid")); if (found == objects.end()) throw std::runtime_error("An AMF instance refers to a missing object");
        Transform3d matrix = Transform3d::Identity();
        matrix.translate(Vec3d(instance.second.get<double>("deltax",0),instance.second.get<double>("deltay",0),instance.second.get<double>("deltaz",0)));
        constexpr double radians = 3.141592653589793 / 180;
        matrix.rotate(Eigen::AngleAxisd(instance.second.get<double>("rz",0) * radians,Vec3d::UnitZ()));
        matrix.rotate(Eigen::AngleAxisd(instance.second.get<double>("ry",0) * radians,Vec3d::UnitY()));
        matrix.rotate(Eigen::AngleAxisd(instance.second.get<double>("rx",0) * radians,Vec3d::UnitX()));
        merge(result,found->second,matrix); instanced = true;
    }
    if (!instanced) for (const auto& pair : objects) merge(result,pair.second,Transform3d::Identity());
    result.scale(float(unit(attr(*amf,"unit")))); return result;
}
}

TriangleMesh read_mesh(const fs::path& path)
{
    // Preview readers own only meshes. They must never construct Model objects:
    // editor object IDs and config timestamps have main-thread-only counters.
    TriangleMesh mesh; const auto format = Library::format(path);
    if (fs::file_size(path) > 512ULL * 1024 * 1024) throw std::runtime_error("This file exceeds the library preview limit");
    if (format == "stl") mesh = read_stl(path);
    else if (format == "obj") { ObjInfo info; std::string message; if (!load_obj(path.u8string().c_str(),&mesh,info,message)) throw std::runtime_error("Could not read OBJ geometry"); }
    else if (format == "ply") {
        Eigen::MatrixXf vertices; Eigen::MatrixXi faces;
        if (!igl::readPLY(path.u8string(),vertices,faces) || vertices.cols() != 3 || faces.cols() != 3) throw std::runtime_error("Could not read PLY geometry");
        indexed_triangle_set geometry;
        for (Eigen::Index i = 0; i < vertices.rows(); ++i) geometry.vertices.emplace_back(vertices(i,0),vertices(i,1),vertices(i,2));
        for (Eigen::Index i = 0; i < faces.rows(); ++i) geometry.indices.emplace_back(faces(i,0),faces(i,1),faces(i,2));
        mesh = checked_mesh(std::move(geometry));
    } else if (format == "3mf") mesh = read_3mf(path);
    else if (format == "amf") mesh = read_amf(path);
    else throw std::runtime_error("This file has no supported mesh preview");
    if (mesh.its.indices.empty() || mesh.its.indices.size() > max_triangles) throw std::runtime_error("The model is empty or exceeds the library preview limit");
    for (const auto& vertex : mesh.its.vertices) if (!vertex.allFinite()) throw std::runtime_error("The model has invalid coordinates");
    for (const auto& triangle : mesh.its.indices) for (int index : triangle) if (index < 0 || size_t(index) >= mesh.its.vertices.size()) throw std::runtime_error("The model has invalid face references");
    return mesh;
}
}
