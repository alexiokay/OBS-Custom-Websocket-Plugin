#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "structured_mdns_query.hpp"

namespace vorti {
    namespace applets {
        namespace obs_plugin {

            /// Represents a discovered WebSocket service.
            struct ServiceInfo {
                std::string name;
                std::string websocket_url;
                std::string ip_address;
                uint16_t port;
                
                ServiceInfo() = default;
                ServiceInfo(const std::string& n, const std::string& url, const std::string& ip, uint16_t p)
                    : name(n), websocket_url(url), ip_address(ip), port(p) {}
            };

            /// mDNS discovery class for finding VortiDeck services using mdns_cpp library
            class MDNSDiscovery {
            public:
                using LogCallback = std::function<void(const std::string&)>;
                static constexpr const char* SERVICE_TYPE = "_vortideck._tcp.local.";
                static constexpr std::chrono::seconds DEFAULT_TIMEOUT{30};
                
                explicit MDNSDiscovery(LogCallback log_callback = {});
                ~MDNSDiscovery();

                /// Discover WebSocket services using mDNS synchronously
                /// @param timeout Maximum time to search for services
                /// @param tls_enabled Whether to use WSS or WS protocol
                /// @return Vector of discovered services
                std::vector<ServiceInfo> discover_services(
                    std::chrono::seconds timeout = DEFAULT_TIMEOUT,
                    bool tls_enabled = false,
                    std::function<void(const ServiceInfo&)> on_service = {}
                );

                /// Discover services asynchronously
                /// @param callback Function called for each discovered service
                /// @param timeout Maximum time to search for services
                /// @param tls_enabled Whether to use WSS or WS protocol
                void discover_services_async(
                    std::function<void(const ServiceInfo&)> callback,
                    std::chrono::seconds timeout = DEFAULT_TIMEOUT,
                    bool tls_enabled = false
                );

                /// Stop any ongoing discovery
                void stop_discovery();

                /// Check if discovery is currently running
                bool is_discovering() const { return m_discovering.load(); }

            private:
                struct DiscoveryContext {
                    MDNSDiscovery* instance;
                    std::vector<ServiceInfo>* services;
                    std::function<void(const ServiceInfo&)> callback;
                    bool tls_enabled;
                    std::mutex services_mutex;
                    std::unordered_set<std::string> service_instances;
                    std::unordered_map<std::string, std::pair<std::string, uint16_t>> srv_records;
                    std::unordered_map<std::string, std::string> ipv4_records;
                    std::unordered_map<std::string, std::string> ipv6_records;
                    std::unordered_set<std::string> emitted_services;
                };

                std::atomic<bool> m_discovering{false};
                std::atomic<bool> m_should_stop{false};
                std::thread m_discovery_thread;
                LogCallback m_log_callback;

                /// Initialize network for mDNS
                bool initialize_network();
                
                /// Clean up network resources
                void cleanup_network();

                /// Perform the actual discovery work using mdns_cpp
                void discovery_worker(DiscoveryContext* context, std::chrono::seconds timeout);

                /// Validate IP address format
                static bool is_valid_ip_address(const std::string& ip);
                static bool is_valid_ipv6_address(const std::string& ip);
                
                /// Validate port number
                static bool is_valid_port(uint16_t port);
                
                void report(const std::string& message) const;
                static void process_query_record(
                    const structured_mdns::Record& record,
                    DiscoveryContext* context
                );
            };

            /// Utility functions
            namespace mdns_utils {
                /// Get the first discovered VortiDeck service
                /// @param timeout Maximum time to search
                /// @param tls_enabled Whether to use WSS or WS protocol
                /// @return Service info if found, empty ServiceInfo if not found
                ServiceInfo get_first_vortideck_service(
                    std::chrono::seconds timeout = MDNSDiscovery::DEFAULT_TIMEOUT,
                    bool tls_enabled = false
                );

                /// Check if a VortiDeck service is available
                /// @param timeout Maximum time to search
                /// @return true if service found, false otherwise
                bool is_vortideck_service_available(
                    std::chrono::seconds timeout = MDNSDiscovery::DEFAULT_TIMEOUT
                );
            }
        }
    }
}
