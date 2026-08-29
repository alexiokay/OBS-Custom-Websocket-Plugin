#include "mdns-test-advertiser.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#endif

#include "mdns.h"

namespace {
constexpr std::string_view service_type = "_vortideck._tcp.local.";
constexpr std::string_view instance_name = "vortideck-e2e";
constexpr std::string_view txt_record = "test=1";
constexpr std::size_t packet_capacity = 2048;

struct Addresses {
    std::uint32_t ipv4{0};
    std::array<std::uint8_t, 16> ipv6{};
    bool has_ipv6{false};
};

Addresses find_test_addresses()
{
    Addresses result;
#ifdef _WIN32
    ULONG size = 15 * 1024;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG status = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr,
        adapters,
        &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            adapters,
            &size);
    }
    if (status != NO_ERROR) {
        throw std::runtime_error("GetAdaptersAddresses failed: " + std::to_string(status));
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->TunnelType == TUNNEL_TYPE_TEREDO) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = unicast->Address.lpSockaddr;
            if (!address) continue;
            if (address->sa_family == AF_INET && result.ipv4 == 0) {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
                if ((ntohl(ipv4->sin_addr.s_addr) >> 24U) != 127U) result.ipv4 = ipv4->sin_addr.s_addr;
            } else if (address->sa_family == AF_INET6 && !result.has_ipv6) {
                const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
                if (!IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) {
                    std::memcpy(result.ipv6.data(), &ipv6->sin6_addr, result.ipv6.size());
                    result.has_ipv6 = true;
                }
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) throw std::runtime_error("getifaddrs failed");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> guard(interfaces, freeifaddrs);
    for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
        const auto* address = interface->ifa_addr;
        if (!address || !(interface->ifa_flags & IFF_UP)) continue;
        if (address->sa_family == AF_INET && result.ipv4 == 0) {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
            if (ipv4->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) result.ipv4 = ipv4->sin_addr.s_addr;
        } else if (address->sa_family == AF_INET6 && !result.has_ipv6) {
            const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
            if (!IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr)) {
                std::memcpy(result.ipv6.data(), &ipv6->sin6_addr, result.ipv6.size());
                result.has_ipv6 = true;
            }
        }
    }
#endif
    if (result.ipv4 == 0 && !result.has_ipv6) {
        throw std::runtime_error("No non-loopback address is available for the mDNS test advertiser");
    }
    return result;
}
} // namespace

struct MdnsTestAdvertiser::Impl {
    explicit Impl(std::uint16_t service_port) : port(service_port) {}

    std::uint16_t port;
    Addresses addresses;
    std::vector<int> sockets;
    std::atomic_bool running{false};
    std::jthread worker;
#ifdef _WIN32
    bool winsock_started{false};
#endif

    static int answer_query(
        int socket,
        const sockaddr* from,
        std::size_t address_size,
        mdns_entry_type_t entry,
        std::uint16_t query_id,
        std::uint16_t record_type,
        std::uint16_t record_class,
        std::uint32_t,
        const void* data,
        std::size_t size,
        std::size_t name_offset,
        std::size_t,
        std::size_t,
        std::size_t,
        void* user_data)
    {
        if (entry != MDNS_ENTRYTYPE_QUESTION || record_type != MDNS_RECORDTYPE_PTR) return 0;
        auto* advertiser = static_cast<Impl*>(user_data);
        std::array<char, 256> name_buffer{};
        auto offset = name_offset;
        const auto question = mdns_string_extract(
            data, size, &offset, name_buffer.data(), name_buffer.size());
        if (question.length != service_type.size() ||
            std::memcmp(question.str, service_type.data(), service_type.size()) != 0) {
            return 0;
        }

        std::array<char, packet_capacity> response{};
        const bool unicast = (record_class & MDNS_UNICAST_RESPONSE) != 0;
        return mdns_query_answer(
            socket,
            from,
            unicast ? address_size : 0,
            response.data(),
            response.size(),
            query_id,
            service_type.data(),
            service_type.size(),
            instance_name.data(),
            instance_name.size(),
            advertiser->addresses.ipv4,
            advertiser->addresses.has_ipv6 ? advertiser->addresses.ipv6.data() : nullptr,
            advertiser->port,
            txt_record.data(),
            txt_record.size());
    }

    void open_sockets()
    {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        winsock_started = true;
#endif
        addresses = find_test_addresses();

        sockaddr_in ipv4{};
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(MDNS_PORT);
        ipv4.sin_addr.s_addr = INADDR_ANY;
        if (const int socket = mdns_socket_open_ipv4(&ipv4); socket >= 0) sockets.push_back(socket);

        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(MDNS_PORT);
        ipv6.sin6_addr = in6addr_any;
        if (const int socket = mdns_socket_open_ipv6(&ipv6); socket >= 0) sockets.push_back(socket);

        if (sockets.empty()) throw std::runtime_error("Failed to open an mDNS advertiser socket");
    }

    void close_sockets() noexcept
    {
        for (const int socket : sockets) mdns_socket_close(socket);
        sockets.clear();
#ifdef _WIN32
        if (winsock_started) {
            WSACleanup();
            winsock_started = false;
        }
#endif
    }

    void run(std::promise<void> ready)
    {
        try {
            open_sockets();
            ready.set_value();
        } catch (...) {
            ready.set_exception(std::current_exception());
            close_sockets();
            return;
        }

        std::array<char, packet_capacity> packet{};
        while (running.load()) {
            fd_set readable{};
            FD_ZERO(&readable);
            int highest = 0;
            for (const int socket : sockets) {
                FD_SET(socket, &readable);
                if (socket > highest) highest = socket;
            }
            timeval timeout{0, 100'000};
            const int selected = select(highest + 1, &readable, nullptr, nullptr, &timeout);
            if (selected < 0) break;
            if (selected == 0) continue;
            for (const int socket : sockets) {
                if (FD_ISSET(socket, &readable)) {
                    mdns_socket_listen(
                        socket,
                        packet.data(),
                        packet.size(),
                        &Impl::answer_query,
                        this);
                }
            }
        }
        close_sockets();
    }
};

MdnsTestAdvertiser::MdnsTestAdvertiser(std::uint16_t port)
    : m_impl(std::make_unique<Impl>(port))
{
}

MdnsTestAdvertiser::~MdnsTestAdvertiser()
{
    stop();
}

void MdnsTestAdvertiser::start()
{
    if (m_impl->running.exchange(true)) return;
    std::promise<void> ready;
    auto initialized = ready.get_future();
    m_impl->worker = std::jthread(
        [impl = m_impl.get(), ready = std::move(ready)]() mutable { impl->run(std::move(ready)); });
    try {
        initialized.get();
    } catch (...) {
        m_impl->running = false;
        if (m_impl->worker.joinable()) m_impl->worker.join();
        throw;
    }
}

void MdnsTestAdvertiser::stop()
{
    m_impl->running = false;
    if (m_impl->worker.joinable()) m_impl->worker.join();
}
