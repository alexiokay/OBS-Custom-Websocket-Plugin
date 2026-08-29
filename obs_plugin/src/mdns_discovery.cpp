#include "mdns_discovery.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

namespace vorti {
    namespace applets {
        namespace obs_plugin {

            MDNSDiscovery::MDNSDiscovery(LogCallback log_callback)
                : m_log_callback(std::move(log_callback)) {
                initialize_network();
            }

            MDNSDiscovery::~MDNSDiscovery() {
                stop_discovery();
                cleanup_network();
            }

            void MDNSDiscovery::report(const std::string& message) const {
                if (m_log_callback) {
                    m_log_callback(message);
                }
            }

            bool MDNSDiscovery::initialize_network() {
#ifdef _WIN32
                WSADATA wsaData;
                int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
                if (result != 0) {
                    report("mDNS network initialization failed with Winsock error " + std::to_string(result));
                    return false;
                }
#endif
                return true;
            }

            void MDNSDiscovery::cleanup_network() {
#ifdef _WIN32
                WSACleanup();
#endif
            }

            std::vector<ServiceInfo> MDNSDiscovery::discover_services(
                std::chrono::seconds timeout,
                bool tls_enabled,
                std::function<void(const ServiceInfo&)> on_service
            ) {
                std::vector<ServiceInfo> services;
                DiscoveryContext context;
                context.instance = this;
                context.services = &services;
                context.callback = std::move(on_service);
                context.tls_enabled = tls_enabled;
                
                m_discovering = true;
                m_should_stop = false;

                discovery_worker(&context, timeout);

                m_discovering = false;
                return services;
            }

            void MDNSDiscovery::discover_services_async(
                std::function<void(const ServiceInfo&)> callback,
                std::chrono::seconds timeout,
                bool tls_enabled
            ) {
                if (m_discovering.load()) {
                    stop_discovery();
                }

                auto context = new DiscoveryContext();
                context->instance = this;
                context->services = nullptr;
                context->callback = std::move(callback);
                context->tls_enabled = tls_enabled;

                m_discovering = true;
                m_should_stop = false;

                m_discovery_thread = std::thread([this, context, timeout]() {
                    try {
                        discovery_worker(context, timeout);
                    } catch (const std::exception& e) {
                        report(std::string("mDNS discovery thread failed: ") + e.what());
                    } catch (...) {
                        report("mDNS discovery thread failed with an unknown error");
                    }
                    delete context;
                    m_discovering = false;
                });
            }

            void MDNSDiscovery::stop_discovery() {
                m_should_stop = true;

                if (m_discovery_thread.joinable()) {
                    m_discovery_thread.join();
                }
                m_discovering = false;
            }

            void MDNSDiscovery::discovery_worker(DiscoveryContext* context, std::chrono::seconds timeout) {
                if (!context) {
                    report("mDNS discovery failed: invalid discovery context");
                    return;
                }

                report("Starting structured mDNS discovery for _vortideck._tcp.local.");
                try {
                    structured_mdns::query(
                        SERVICE_TYPE,
                        std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
                        m_should_stop,
                        [this, context](const structured_mdns::Record& record) {
                            if (!m_should_stop.load()) {
                                MDNSDiscovery::process_query_record(record, context);
                            }
                        });
                } catch (const std::exception& e) {
                    report(std::string("mDNS discovery failed: ") + e.what());
                } catch (...) {
                    report("mDNS discovery failed with an unknown error");
                }

                size_t service_count = 0;
                if (context->services) {
                    std::lock_guard<std::mutex> lock(context->services_mutex);
                    service_count = context->services->size();
                } else {
                    service_count = context->emitted_services.size();
                }
                report("Structured mDNS discovery completed with " + std::to_string(service_count) + " service(s)");
            }

            bool MDNSDiscovery::is_valid_ip_address(const std::string& ip) {
                if (ip.empty() || ip.length() > 15) return false;
                
                // Simple IPv4 validation
                int parts = 0;
                size_t start = 0;
                for (size_t i = 0; i <= ip.length(); ++i) {
                    if (i == ip.length() || ip[i] == '.') {
                        if (i == start) return false; // Empty part
                        
                        std::string part = ip.substr(start, i - start);
                        if (part.length() > 3) return false;
                        
                        try {
                            int num = std::stoi(part);
                            if (num < 0 || num > 255) return false;
                        } catch (...) {
                            return false;
                        }
                        
                        parts++;
                        start = i + 1;
                    } else if (!std::isdigit(ip[i])) {
                        return false;
                    }
                }
                
                return parts == 4;
            }

            bool MDNSDiscovery::is_valid_port(uint16_t port) {
                // Valid port range: 1-65535 (0 is reserved)
                return port > 0 && port <= 65535;
            }

            bool MDNSDiscovery::is_valid_ipv6_address(const std::string& ip) {
                if (ip.empty()) return false;
                in6_addr address{};
                return inet_pton(AF_INET6, ip.c_str(), &address) == 1;
            }

            void MDNSDiscovery::process_query_record(
                const structured_mdns::Record& record,
                DiscoveryContext* context
            ) {
                if (!context) return;

                std::vector<ServiceInfo> completed;
                {
                    std::lock_guard<std::mutex> lock(context->services_mutex);
                    switch (record.type) {
                    case structured_mdns::RecordType::Ptr:
                        if (record.name == SERVICE_TYPE && !record.target.empty()) {
                            context->service_instances.insert(record.target);
                        }
                        break;
                    case structured_mdns::RecordType::Srv:
                        if (!record.name.empty() && !record.target.empty() && is_valid_port(record.port)) {
                            context->srv_records[record.name] = {record.target, record.port};
                        }
                        break;
                    case structured_mdns::RecordType::A:
                        if (!record.name.empty() && is_valid_ip_address(record.address)) {
                            context->ipv4_records[record.name] = record.address;
                        }
                        break;
                    case structured_mdns::RecordType::Aaaa:
                        if (!record.name.empty() && is_valid_ipv6_address(record.address)) {
                            context->ipv6_records[record.name] = record.address;
                        }
                        break;
                    default:
                        break;
                    }

                    for (const auto& instance : context->service_instances) {
                        const auto srv = context->srv_records.find(instance);
                        if (srv == context->srv_records.end()) continue;
                        std::string address;
                        bool ipv6 = false;
                        if (const auto ipv4_record = context->ipv4_records.find(srv->second.first);
                            ipv4_record != context->ipv4_records.end()) {
                            address = ipv4_record->second;
                        } else if (const auto ipv6_record = context->ipv6_records.find(srv->second.first);
                                   ipv6_record != context->ipv6_records.end()) {
                            address = ipv6_record->second;
                            ipv6 = true;
                        } else {
                            continue;
                        }

                        const std::string key = instance + "|" + address + "|" + std::to_string(srv->second.second);
                        if (!context->emitted_services.insert(key).second) continue;

                        const std::string url_host = ipv6 ? "[" + address + "]" : address;
                        const std::string websocket_url = context->tls_enabled
                            ? "wss://" + url_host + ":" + std::to_string(srv->second.second) + "/ws"
                            : "ws://" + url_host + ":" + std::to_string(srv->second.second) + "/ws";
                        completed.emplace_back(instance, websocket_url, address, srv->second.second);
                    }

                    if (context->services) {
                        context->services->insert(context->services->end(), completed.begin(), completed.end());
                    }
                }

                if (context->callback) {
                    for (const auto& service : completed) {
                        context->callback(service);
                    }
                }
            }

            namespace mdns_utils {
                ServiceInfo get_first_vortideck_service(std::chrono::seconds timeout, bool tls_enabled) {
                    MDNSDiscovery discovery;
                    auto services = discovery.discover_services(timeout, tls_enabled);
                    
                    if (!services.empty()) {
                        return services[0];
                    }
                    
                    return ServiceInfo(); // Return empty service info if none found
                }

                bool is_vortideck_service_available(std::chrono::seconds timeout) {
                    auto service = get_first_vortideck_service(timeout, false);
                    return !service.websocket_url.empty();
                }
            }
        }
    }
}
