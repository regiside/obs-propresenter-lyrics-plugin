#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <netdb.h>
#include <cstring>
#endif

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <dns_sd.h>
#elif defined(_WIN32)
#include <windows.h>
// Bonjour's public DNS-SD ABI. Load at runtime so manual connections still
// work on Windows machines without Bonjour installed.
#define DNSSD_API __stdcall
using DNSServiceRef = struct _DNSServiceRef_t *;
using DNSServiceFlags = uint32_t;
using DNSServiceErrorType = int32_t;
using DNSServiceGetAddrInfoReply = void (DNSSD_API *)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const sockaddr *, uint32_t, void *);
using DNSServiceBrowseReply = void (DNSSD_API *)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const char *, const char *, void *);
using DNSServiceResolveReply = void (DNSSD_API *)(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType, const char *, const char *, uint16_t, uint16_t, const unsigned char *, void *);
#endif

namespace stage_discovery {
struct Service {
	std::string name, type, domain, host;
	int port = 0;
	std::string id() const { return name + "\t" + type + "\t" + domain; }
};

inline bool parse_id(const std::string &id, Service &service)
{
	auto first = id.find('\t');
	auto second = first == std::string::npos ? first : id.find('\t', first + 1);
	if (first == std::string::npos || second == std::string::npos || first == 0 || second + 1 == id.size())
		return false;
	service.name = id.substr(0, first);
	service.type = id.substr(first + 1, second - first - 1);
	service.domain = id.substr(second + 1);
	return service.type == "_pro7stagedsply._tcp." && service.domain == "local.";
}

#if defined(__APPLE__) || defined(_WIN32)
struct API {
#ifdef __APPLE__
	decltype(&DNSServiceGetAddrInfo) get_address = DNSServiceGetAddrInfo;
	decltype(&DNSServiceBrowse) browse = DNSServiceBrowse;
	decltype(&DNSServiceResolve) resolve = DNSServiceResolve;
	decltype(&DNSServiceRefSockFD) fd = DNSServiceRefSockFD;
	decltype(&DNSServiceProcessResult) process = DNSServiceProcessResult;
	decltype(&DNSServiceRefDeallocate) release = DNSServiceRefDeallocate;
#else
	DNSServiceErrorType (DNSSD_API *get_address)(DNSServiceRef *, DNSServiceFlags, uint32_t, uint32_t, const char *, DNSServiceGetAddrInfoReply, void *) = nullptr;
	DNSServiceErrorType (DNSSD_API *browse)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, DNSServiceBrowseReply, void *) = nullptr;
	DNSServiceErrorType (DNSSD_API *resolve)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, const char *, DNSServiceResolveReply, void *) = nullptr;
	int (DNSSD_API *fd)(DNSServiceRef) = nullptr;
	DNSServiceErrorType (DNSSD_API *process)(DNSServiceRef) = nullptr;
	void (DNSSD_API *release)(DNSServiceRef) = nullptr;
	API() {
		// Bonjour installs its client DLL in the system directory.
		HMODULE module = LoadLibraryExW(L"dnssd.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) return;
		get_address = reinterpret_cast<decltype(get_address)>(GetProcAddress(module, "DNSServiceGetAddrInfo"));
		browse = reinterpret_cast<decltype(browse)>(GetProcAddress(module, "DNSServiceBrowse"));
		resolve = reinterpret_cast<decltype(resolve)>(GetProcAddress(module, "DNSServiceResolve"));
		fd = reinterpret_cast<decltype(fd)>(GetProcAddress(module, "DNSServiceRefSockFD"));
		process = reinterpret_cast<decltype(process)>(GetProcAddress(module, "DNSServiceProcessResult"));
		release = reinterpret_cast<decltype(release)>(GetProcAddress(module, "DNSServiceRefDeallocate"));
	}
#endif
	bool available() const { return browse && resolve && fd && process && release; }
};
inline API &api() { static API instance; return instance; }
inline bool available() { return api().available(); }

inline void pump(DNSServiceRef ref, int milliseconds, const std::atomic<bool> &cancel, const bool *done = nullptr)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	int descriptor = api().fd(ref);
	if (descriptor < 0) return;
#ifndef _WIN32
	if (descriptor >= FD_SETSIZE) return;
#endif
	while (!cancel && (!done || !*done) && std::chrono::steady_clock::now() < end) {
		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(descriptor, &read_set);
		timeval timeout{0, 100000};
		int ready = select(descriptor + 1, &read_set, nullptr, nullptr, &timeout);
		if (ready < 0 || (ready > 0 && api().process(ref) != 0)) break;
	}
}
struct AddressResult {
	std::vector<sockaddr_storage> values;
	bool done = false;
};
inline void DNSSD_API address_received(DNSServiceRef, DNSServiceFlags flags, uint32_t interface_index,
	DNSServiceErrorType error, const char *, const sockaddr *address, uint32_t, void *context)
{
	auto &result = *static_cast<AddressResult *>(context);
	if (!error && address && (flags & 2)) {
		sockaddr_storage copy{};
		if (address->sa_family == AF_INET) {
			memcpy(&copy, address, sizeof(sockaddr_in));
		} else if (address->sa_family == AF_INET6) {
			memcpy(&copy, address, sizeof(sockaddr_in6));
			auto *v6 = reinterpret_cast<sockaddr_in6 *>(&copy);
			if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr) && !v6->sin6_scope_id)
				v6->sin6_scope_id = interface_index;
		} else return;
		result.values.push_back(copy);
	}
	result.done = !result.values.empty() && !(flags & 1);
}
struct Resolution { Service *service; bool done = false; };
inline void DNSSD_API resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error,
	const char *, const char *host, uint16_t port, uint16_t, const unsigned char *, void *context)
{
	auto &result = *static_cast<Resolution *>(context);
	result.done = true;
	if (!error) {
		result.service->host = host;
		result.service->port = ntohs(port);
	}
}
inline bool resolve(Service &service, const std::atomic<bool> &cancel)
{
	service.host.clear();
	service.port = 0;
	if (!available()) return false;
	DNSServiceRef ref = nullptr;
	Resolution result{&service};
	if (api().resolve(&ref, 0, 0, service.name.c_str(), service.type.c_str(), service.domain.c_str(), resolved, &result)) return false;
	pump(ref, 2000, cancel, &result.done);
	api().release(ref);
	return !cancel && !service.host.empty() && service.port > 0;
}
inline void DNSSD_API browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType error,
	const char *name, const char *type, const char *domain, void *context)
{
	if (error || !(flags & 2)) return;
	auto &services = *static_cast<std::map<std::string, Service> *>(context);
	Service service{name, type, domain, "", 0};
	services[service.id()] = service;
}
inline std::vector<Service> scan(const std::atomic<bool> &cancel)
{
	std::vector<Service> results;
	if (!available()) return results;
	std::map<std::string, Service> found;
	DNSServiceRef ref = nullptr;
	if (api().browse(&ref, 0, 0, "_pro7stagedsply._tcp", "local.", browsed, &found)) return results;
	pump(ref, 2000, cancel);
	api().release(ref);
	for (auto &entry : found) {
		if (cancel) break;
		if (resolve(entry.second, cancel)) results.push_back(entry.second);
	}
	return results;
}
#else
inline bool available() { return false; }
inline bool resolve(Service &, const std::atomic<bool> &) { return false; }
inline std::vector<Service> scan(const std::atomic<bool> &) { return {}; }
#endif
// Numeric addresses need no network lookup. Bonjour host lookup is polled
// with the same cancellation token as discovery and socket I/O.
inline std::vector<sockaddr_storage> addresses(const std::string &host, int port, const std::atomic<bool> &cancel)
{
	std::vector<sockaddr_storage> values;
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	addrinfo *result = nullptr;
	std::string port_text = std::to_string(port);
	int error = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
	if (error) {
#if defined(__APPLE__) || defined(_WIN32)
		if (available() && api().get_address) {
			DNSServiceRef ref = nullptr;
			AddressResult found;
			if (!api().get_address(&ref, 0, 0, 0, host.c_str(), address_received, &found)) {
				pump(ref, 2000, cancel, &found.done);
				api().release(ref);
			}
			if (cancel) return {};
			for (auto &address : found.values) {
				if (address.ss_family == AF_INET)
					reinterpret_cast<sockaddr_in *>(&address)->sin_port = htons(static_cast<uint16_t>(port));
				else reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port = htons(static_cast<uint16_t>(port));
			}
			return found.values;
		}
#endif
#ifdef _WIN32
		// Use Windows asynchronous DNS when Bonjour is absent, so stopping a
		// source does not wait for the system's synchronous resolver timeout.
		if (cancel) return {};
		int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.c_str(), -1, nullptr, 0);
		if (!count) return {};
		std::wstring wide_host(static_cast<size_t>(count), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.c_str(), -1, wide_host.data(), count);
		std::wstring wide_port = std::to_wstring(port);
		ADDRINFOEXW async_hints{};
		async_hints.ai_family = AF_UNSPEC;
		async_hints.ai_socktype = SOCK_STREAM;
		async_hints.ai_flags = AI_NUMERICSERV;
		PADDRINFOEXW async_result = nullptr;
		OVERLAPPED operation{};
		operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!operation.hEvent) return {};
		HANDLE query = nullptr;
		int status = GetAddrInfoExW(wide_host.c_str(), wide_port.c_str(), NS_DNS, nullptr,
			&async_hints, &async_result, nullptr, &operation, nullptr, &query);
		if (status == WSA_IO_PENDING) {
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (WaitForSingleObject(operation.hEvent, 50) == WAIT_TIMEOUT) {
				if (cancel || std::chrono::steady_clock::now() >= deadline) {
					GetAddrInfoExCancel(&query);
					// Cancellation signals completion; keep the overlapped storage
					// alive until Windows has finished accessing it.
					WaitForSingleObject(operation.hEvent, INFINITE);
					break;
				}
			}
			status = GetAddrInfoExOverlappedResult(&operation);
		}
		if (!status && !cancel) {
			for (auto *entry = async_result; entry; entry = entry->ai_next) {
				if (entry->ai_addrlen > sizeof(sockaddr_storage)) continue;
				sockaddr_storage address{};
				memcpy(&address, entry->ai_addr, entry->ai_addrlen);
				values.push_back(address);
			}
		}
		if (async_result) FreeAddrInfoExW(async_result);
		CloseHandle(operation.hEvent);
		return values;
#else
		// Manual hostname support without a DNS-SD runtime. This lookup runs
		// only on the connection worker, never in OBS's settings callback.
		hints.ai_flags = AI_NUMERICSERV;
		if (cancel || getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result)) return {};
#endif
	}
	for (auto *entry = result; entry && !cancel; entry = entry->ai_next) {
		if (entry->ai_addrlen > sizeof(sockaddr_storage)) continue;
		sockaddr_storage address{};
		memcpy(&address, entry->ai_addr, entry->ai_addrlen);
		values.push_back(address);
	}
	freeaddrinfo(result);
	return values;
}
} // namespace stage_discovery
