#pragma once

#include <filesystem>

namespace Slic3r::ModelLibrary {
// Recoverable platform trash only. Failure never falls back to unlinking.
bool move_to_trash(const std::filesystem::path& path);
}
