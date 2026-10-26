#pragma once

#include "Library.hpp"
#include <chrono>
#include <map>
#include <string>

namespace Slic3r::ModelLibrary {
// Called on the library worker. No detached thread, GUI or network dependency.
// A bounded poll also recovers missed filesystem notifications and downloads
// completed while Porca was closed.
class Watch {
public:
    Json poll(Library& library, const Extractor& extract, const Cancel& cancel,
              std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
private:
    struct Pending {
        std::string signature;
        std::chrono::steady_clock::time_point changed;
        bool imported = false;
    };
    std::map<std::string, Pending> m_pending;
};
}
