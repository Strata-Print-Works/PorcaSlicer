#pragma once

#include "Bonjour.hpp"
#include <atomic>
#include <string>

namespace Slic3r {
#ifdef __APPLE__
bool system_bonjour_lookup(const std::string& service, const std::string& protocol,
                           const Bonjour::TxtKeys& keys, const std::atomic<bool>& cancelled,
                           unsigned timeout, const Bonjour::ReplyFn& reply, bool enumerate_types = false);
void system_bonjour_resolve(const std::string& hostname, const std::atomic<bool>& cancelled,
                            unsigned timeout, const Bonjour::ResolveFn& reply);
#endif
} // namespace Slic3r
