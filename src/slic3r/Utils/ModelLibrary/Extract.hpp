#pragma once

#include "Library.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstdint>
#include <vector>

namespace Slic3r::ModelLibrary {
struct PreviewPixels { unsigned width = 0, height = 0; std::vector<std::uint8_t> rgb; };
PreviewPixels read_preview(const fs::path& path, unsigned edge);
Json extract_file(const fs::path& path, const std::string& hash, const fs::path& root);
TriangleMesh read_mesh(const fs::path& path);
std::string geometry_fingerprint(const indexed_triangle_set& mesh);
// Shared by background thumbnails and the interactive native inspector.
std::vector<std::uint8_t> render_mesh(const indexed_triangle_set& mesh, unsigned edge,
                                    double yaw = 0.785398, double pitch = 0.55, const Cancel& cancel = {});
}
