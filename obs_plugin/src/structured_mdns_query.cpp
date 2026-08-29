#include "structured_mdns_query.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "mdns.h"

namespace vorti::applets::obs_plugin::structured_mdns {
namespace {

constexpr std::size_t max_sockets = 32;
constexpr std::size_t packet_capacity = 2048;

struct SocketSet {
    std::array<int, max_sockets> values{};
    std::size_t count{0};

    SocketSet() = default;
    SocketSet(const SocketSet&) = delete;
    SocketSet& operator=(const SocketSet&) = delete;
    SocketSet(SocketSet&& other) noexcept : values(other.values), count(other.count) { other.count = 0; }
    ~SocketSet()
    {
        for (std::size_t index = 0; index < count; ++index) mdns_socket_close(values[index]);
    }

    void open(const sockaddr* address)
    {
        if (count >= values.size()) return;
        int socket = -1;
        if (address->sa_family == AF_INET) {
            auto value = *reinterpret_cast<const sockaddr_in*>(address);
            value.sin_port = htons(MDNS_PORT);
            socket = mdns_socket_open_ipv4(&value);
        } else if (address->sa_family == AF_INET6) {
            auto value = *reinterpret_cast<const sockaddr_in6*>(address);
            value.sin6_port = htons(MDNS_PORT);
            socket = mdns_socket_open_ipv6(&value);
        }
        if (socket >= 0) values[count++] = socket;
    }
};

SocketSet open_query_sockets()
{
    SocketSet sockets;
#ifdef _WIN32
    ULONG size = 15 * 1024;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &size);
    }
    if (result != NO_ERROR) {
        throw std::runtime_error("GetAdaptersAddresses failed with error " + std::to_string(result));
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->TunnelType == TUNNEL_TYPE_TEREDO) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = unicast->Address.lpSockaddr;
            if (!address) continue;
            if (address->sa_family == AF_INET) {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
                if ((ntohl(ipv4->sin_addr.s_addr) >> 24U) == 127U) continue;
                sockets.open(address);
            } else if (address->sa_family == AF_INET6) {
                const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
                if (IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) continue;
                sockets.open(address);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        throw std::runtime_error(std::string("getifaddrs failed: ") + std::strerror(errno));
    }
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard(interfaces, freeifaddrs);
    for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
        const auto* address = interface->ifa_addr;
        if (!address) continue;
        if (address->sa_family == AF_INET) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
            if (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
            sockets.open(address);
        } else if (address->sa_family == AF_INET6) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
            if (IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) continue;
            sockets.open(address);
        }
    }
#endif
    return sockets;
}

int receive_record(
    int, const sockaddr*, std::size_t, mdns_entry_type_t, std::uint16_t, std::uint16_t rtype,
    std::uint16_t, std::uint32_t, const void* data, std::size_t size, std::size_t name_offset,
    std::size_t, std::size_t record_offset, std::size_t record_length, void* user_data)
{
    auto* callback = static_cast<RecordCallback*>(user_data);
    if (!callback || !*callback) return 0;
    std::array<char, 256> buffer{};
    const auto name = mdns_string_extract(data, size, &name_offset, buffer.data(), buffer.size());
    Record record;
    record.name.assign(name.str, name.length);
    if (rtype == MDNS_RECORDTYPE_PTR) {
        const auto value = mdns_record_parse_ptr(data, size, record_offset, record_length, buffer.data(), buffer.size());
        record.type = RecordType::Ptr;
        record.target.assign(value.str, value.length);
    } else if (rtype == MDNS_RECORDTYPE_SRV) {
        const auto value = mdns_record_parse_srv(data, size, record_offset, record_length, buffer.data(), buffer.size());
        record.type = RecordType::Srv;
        record.target.assign(value.name.str, value.name.length);
        record.port = value.port;
    } else if (rtype == MDNS_RECORDTYPE_A) {
        sockaddr_in value{};
        mdns_record_parse_a(data, size, record_offset, record_length, &value);
        std::array<char, INET_ADDRSTRLEN> text{};
        if (!inet_ntop(AF_INET, &value.sin_addr, text.data(), text.size())) return 0;
        record.type = RecordType::A;
        record.address = text.data();
    } else if (rtype == MDNS_RECORDTYPE_AAAA) {
        sockaddr_in6 value{};
        mdns_record_parse_aaaa(data, size, record_offset, record_length, &value);
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (!inet_ntop(AF_INET6, &value.sin6_addr, text.data(), text.size())) return 0;
        record.type = RecordType::Aaaa;
        record.address = text.data();
    } else if (rtype == MDNS_RECORDTYPE_TXT) {
        std::array<mdns_record_txt_t, 128> values{};
        const auto count = mdns_record_parse_txt(data, size, record_offset, record_length, values.data(), values.size());
        record.type = RecordType::Txt;
        for (std::size_t index = 0; index < count; ++index) {
            record.txt.emplace_back(
                std::string(values[index].key.str, values[index].key.length),
                std::string(values[index].value.str, values[index].value.length));
        }
    } else {
        record.type = RecordType::Other;
    }
    (*callback)(record);
    return 0;
}

} // namespace

void query(
    const std::string& service,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>& should_stop,
    RecordCallback callback)
{
    auto sockets = open_query_sockets();
    if (sockets.count == 0) throw std::runtime_error("failed to open any multicast DNS query sockets");
    std::array<int, max_sockets> query_ids{};
    std::array<unsigned char, packet_capacity> buffer{};
    bool sent = false;
    for (std::size_t index = 0; index < sockets.count; ++index) {
        query_ids[index] = mdns_query_send(
            sockets.values[index], MDNS_RECORDTYPE_PTR, service.data(), service.size(),
            buffer.data(), buffer.size(), 0);
        sent = sent || query_ids[index] >= 0;
    }
    if (!sent) throw std::runtime_error("failed to send multicast DNS query");

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!should_stop.load()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::microseconds::zero()) break;
        const auto slice = (std::min)(
            remaining,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(200)));
        timeval wait{static_cast<long>(slice.count() / 1000000), static_cast<long>(slice.count() % 1000000)};
        fd_set readable;
        FD_ZERO(&readable);
        int nfds = 0;
        for (std::size_t index = 0; index < sockets.count; ++index) {
            FD_SET(sockets.values[index], &readable);
            nfds = (std::max)(nfds, sockets.values[index] + 1);
        }
        const int selected = select(nfds, &readable, nullptr, nullptr, &wait);
        if (selected < 0) throw std::runtime_error("multicast DNS select failed");
        if (selected == 0) continue;
        for (std::size_t index = 0; index < sockets.count; ++index) {
            if (FD_ISSET(sockets.values[index], &readable)) {
                mdns_query_recv(
                    sockets.values[index], buffer.data(), buffer.size(), receive_record,
                    &callback, query_ids[index]);
            }
        }
    }
}

} // namespace vorti::applets::obs_plugin::structured_mdns
