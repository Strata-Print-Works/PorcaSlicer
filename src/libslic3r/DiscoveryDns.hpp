#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace Slic3r {
// DNS-SD supplies expanded (uncompressed) PTR/SRV RDATA. Return an escaped
// absolute name suitable for DNSServiceQueryRecord, retaining literal dots.
inline std::optional<std::string> discovery_dns_name(const unsigned char* data, size_t size)
{
    std::string name;
    size_t at = 0;
    while (data && at < size && at < 255) {
        const unsigned length = data[at++];
        if (length == 0) return name.empty() ? std::nullopt : std::optional<std::string>(name);
        if (length > 63 || at + length > size || at + length >= 255) return std::nullopt;
        for (unsigned i = 0; i < length; ++i) {
            const unsigned char c = data[at++];
            if (c <= 32 || c >= 127) {
                name += '\\';
                name += char('0' + c / 100);
                name += char('0' + c / 10 % 10);
                name += char('0' + c % 10);
            } else {
                if (c == '.' || c == '\\') name += '\\';
                name += char(c);
            }
        }
        name += '.';
    }
    return std::nullopt;
}
} // namespace Slic3r
