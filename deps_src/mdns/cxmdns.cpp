#include"cxmdns.h"
#include <chrono>
#include <atomic>
#include <functional>
#include"mdns.h"
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include<string.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#define sleep(x) Sleep(x * 1000)
#else
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

// Alias some things to simulate recieving data to fuzz library
#if defined(MDNS_FUZZING)
#define recvfrom(sock, buffer, capacity, flags, src_addr, addrlen) ((mdns_ssize_t)capacity)
#define printf
#endif

#include "mdns.h"

#if defined(MDNS_FUZZING)
#undef recvfrom
#endif

namespace cxnet
{
	template <typename F>
	mdns_record_callback_fn lambda2function(F lambda)
	{
		static auto lambdabak = lambda;
		return [](int sock, const struct sockaddr* from, size_t addrlen,
			mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
			uint16_t rclass, uint32_t ttl, const void* data, size_t size,
			size_t name_offset, size_t name_length, size_t record_offset,
			size_t record_length, void* user_data)->int {lambdabak(sock, from, addrlen, entry, query_id, rtype, rclass, ttl, data, size, name_offset, name_length, record_offset, record_length, user_data); return 0; };
	}

	volatile sig_atomic_t running = 1;
#ifdef _WIN32
	BOOL console_handler(DWORD signal) {
		if (signal == CTRL_C_EVENT) {
			running = 0;
		}
		return TRUE;
	}
#else
	void signal_handler(int signal) {
		running = 0;
	}
#endif

	void recvMachineInfoFromSocket(int sock, void* buffer, size_t capacity, const std::vector<std::string>& prefix, std::vector<machine_info>& retmachineInfos, int recIndex)
	{
		struct sockaddr_in6 addr;
		struct sockaddr* saddr = (struct sockaddr*)&addr;
		socklen_t addrlen = sizeof(addr);
		memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
		saddr->sa_len = sizeof(addr);
#endif
		mdns_ssize_t ret = recvfrom(sock, (char*)buffer, (mdns_size_t)capacity, 0, saddr, &addrlen);
		if (ret <= 0)
			return;

		size_t data_size = (size_t)ret;
		//size_t records = 0;
		const uint16_t* data = (uint16_t*)buffer;

		uint16_t query_id = mdns_ntohs(data++);
		uint16_t flags = mdns_ntohs(data++);
		uint16_t questions = mdns_ntohs(data++);
		uint16_t answer_rrs = mdns_ntohs(data++);
		uint16_t authority_rrs = mdns_ntohs(data++);
		uint16_t additional_rrs = mdns_ntohs(data++);

		// According to RFC 6762 the query ID MUST match the sent query ID (which is 0 in our case)
		if (query_id || (flags != 0x8400))
			return;  // Not a reply to our question

		// It seems some implementations do not fill the correct questions field,
		// so ignore this check for now and only validate answer string
		// if (questions != 1)
		// 	return 0;

		int i;
		for (i = 0; i < questions; ++i) {
			size_t offset = MDNS_POINTER_DIFF(data, buffer);
			size_t verify_offset = 12;
			// Verify it's our question, _services._dns-sd._udp.local.
			if (!mdns_string_equal(buffer, data_size, &offset, mdns_services_query,
				sizeof(mdns_services_query), &verify_offset))
				return;
			data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

			uint16_t rtype = mdns_ntohs(data++);
			uint16_t rclass = mdns_ntohs(data++);

			// Make sure we get a reply based on our PTR question for class IN
			if ((rtype != MDNS_RECORDTYPE_PTR) || ((rclass & 0x7FFF) != MDNS_CLASS_IN))
				return;
		}

		for (i = 0; i < answer_rrs; ++i) {
			size_t offset = MDNS_POINTER_DIFF(data, buffer);
			size_t verify_offset = 12;
			// Verify it's an answer to our question, _services._dns-sd._udp.local.
			size_t name_offset = offset;
			int is_answer = mdns_string_equal(buffer, data_size, &offset, mdns_services_query,
				sizeof(mdns_services_query), &verify_offset);
			if (!is_answer && !mdns_string_skip(buffer, data_size, &offset))
				break;
			size_t name_length = offset - name_offset;
			if ((offset + 10) > data_size)
				return;
			data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

			uint16_t rtype = mdns_ntohs(data++);
			uint16_t rclass = mdns_ntohs(data++);
			uint32_t ttl = mdns_ntohl(data);
			data += 2;
			uint16_t length = mdns_ntohs(data++);
			if (length > (data_size - offset))
				return;

			static char addrbuf[64];
			static char entrybuf[256];
			static char namebuf[256];

			if (is_answer) {
				offset = MDNS_POINTER_DIFF(data, buffer);
				(void)sizeof(sock);
				(void)sizeof(query_id);
				(void)sizeof(name_length);
				//(void)sizeof(0);
				mdns_string_t fromaddrstr = ip_address_to_string(addrbuf, sizeof(addrbuf), saddr, addrlen);
				mdns_string_t entrystr =
					mdns_string_extract(buffer, data_size, &name_offset, entrybuf, sizeof(entrybuf));
				if (rtype == MDNS_RECORDTYPE_PTR) {
					mdns_string_t namestr = mdns_record_parse_ptr(buffer, data_size, offset, length,
						namebuf, sizeof(namebuf));
					if (!namestr.str || namestr.length == 0) {
						return;
					}
					const std::string answer_name(namestr.str, namestr.length);
					bool bFound = false;
					for (const auto& item : prefix)
					{
						if (answer_name.find(item) != std::string::npos)
							bFound = true;
					}
					if (!bFound)
					{
						return;
					}

					char ip[16] = { 0 };
					sscanf(fromaddrstr.str, "%[^:]", ip);
					retmachineInfos.push_back({ ip, answer_name });
				}
			}
		}
	}

    std::vector<machine_info> syncDiscoveryService(const std::vector<std::string>& prefix,
                                                  const std::atomic<bool>* cancelled, std::function<bool()> permit_probe)
    {
        std::vector<machine_info> result;
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(1, 1), &data)) return result;
#endif
        int sockets[4];
        const int count = open_client_sockets(sockets, 4, 0);
        for (int i = 0; i < count; ++i) {
#ifndef _WIN32
            if (sockets[i] >= FD_SETSIZE) { mdns_socket_close(sockets[i]); sockets[i] = -1; continue; }
#endif
            if ((cancelled && cancelled->load()) || (permit_probe && !permit_probe())) break;
            if (mdns_discovery_send(sockets[i])) { mdns_socket_close(sockets[i]); sockets[i] = -1; }
        }
        char buffer[2048];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((!cancelled || !cancelled->load()) && std::chrono::steady_clock::now() < deadline) {
            fd_set readfs;
            FD_ZERO(&readfs);
            int nfds = 0;
            for (int i = 0; i < count; ++i) {
                if (sockets[i] < 0) continue;
                FD_SET(sockets[i], &readfs);
                if (sockets[i] >= nfds) nfds = sockets[i] + 1;
            }
            if (nfds == 0) break;
            timeval timeout{0, 100000};
            const int ready = select(nfds, &readfs, nullptr, nullptr, &timeout);
            if (ready < 0) break;
            if (ready > 0)
                for (int i = 0; i < count; ++i)
                    if (sockets[i] >= 0 && FD_ISSET(sockets[i], &readfs))
                        recvMachineInfoFromSocket(sockets[i], buffer, sizeof(buffer), prefix, result, i);
        }
        for (int i = 0; i < count; ++i) if (sockets[i] >= 0) mdns_socket_close(sockets[i]);
#ifdef _WIN32
        WSACleanup();
#endif
        return result;
    }
}
