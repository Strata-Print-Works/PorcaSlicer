#pragma once

#include <stdint.h>
#include <string.h>

/* Shared by C DNS-SD and C++ discovery. Addresses are host-order IPv4 values. */
static inline int discovery_interface_name_allowed(const char* name)
{
    static const char* const excluded[] = {
        "lo", "utun", "tun", "tap", "bridge", "vmenet", "vmnet", "veth", "docker",
        "awdl", "llw", "anpi", "p2p", "wg", "tailscale", "virbr", "vbox", "zt"
    };
    if (!name || !*name) return 0;
    for (unsigned i = 0; i < sizeof(excluded) / sizeof(excluded[0]); ++i)
        if (strncmp(name, excluded[i], strlen(excluded[i])) == 0) return 0;
    if (name[0] == 'a' && name[1] == 'p' && name[2] >= '0' && name[2] <= '9') return 0;
    return 1;
}

static inline int discovery_ipv4_allowed(uint32_t address)
{
    return (address >> 24) != 0 && (address >> 24) != 127 && (address >> 24) < 224 &&
           (address >> 16) != 0xa9fe;
}

static inline int discovery_interface_allowed(const char* name, uint32_t address,
                                             int up, int running, int multicast, int loopback, int point_to_point)
{
    return up && running && multicast && !loopback && !point_to_point &&
           discovery_interface_name_allowed(name) && discovery_ipv4_allowed(address);
}

static inline int discovery_netmask_valid(uint32_t mask)
{
    const uint32_t hosts = ~mask;
    return mask != 0 && hosts > 1 && (hosts & (hosts + 1)) == 0;
}
