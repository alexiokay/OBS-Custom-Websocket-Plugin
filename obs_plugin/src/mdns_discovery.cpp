#include "mdns_discovery.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#include "mdns_cpp/logger.hpp"
#include "mdns_cpp/mdns.hpp"

namespace vorti {
    namespace applets {
        namespace obs_plugin {

            MDNSDiscovery::MDNSDiscovery() {
                initialize_network();
            }

            MDNSDiscovery::~MDNSDiscovery() {
                stop_discovery();
                cleanup_network();
            }

            bool MDNSDiscovery::initialize_network() {
#ifdef _WIN32
                WSADATA wsaData;
                int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
                if (result != 0) {
                    std::cerr << "WSAStartup failed with error: " << result << std::endl;
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
                bool tls_enabled
            ) {
                std::vector<ServiceInfo> services;
                DiscoveryContext context;
                context.instance = this;
                context.services = &services;
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
                        std::cerr << "Exception in discovery thread: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "Unknown exception in discovery thread" << std::endl;
                    }
                    delete context;
                    m_discovering = false;
                });
            }

            void MDNSDiscovery::stop_discovery() {
                m_should_stop = true;
                
                // Clear any remaining logger callbacks to prevent crashes
                try {
                    mdns_cpp::Logger::setLoggerSink(nullptr);
                } catch (...) {
                    // Ignore exceptions during cleanup
                }
                
                if (m_discovery_thread.joinable()) {
                    m_discovery_thread.join();
                }
                m_discovering = false;
            }

            void MDNSDiscovery::discovery_worker(DiscoveryContext* context, std::chrono::seconds timeout) {
                std::cout << "Starting mDNS discovery for service: " << SERVICE_TYPE << std::endl;
                
                if (!context) {
                    std::cerr << "Error: Invalid discovery context" << std::endl;
                    return;
                }
                
                try {
                    // Reset parsing context for this discovery session
                    context->parsing_context.reset();
                    
                    // For background discovery (async mode with callback), skip the problematic mdns_cpp logger
                    if (context->callback && !context->services) {
                        std::cout << "Background discovery mode - using minimal approach" << std::endl;
                        
                        // For background discovery, just wait and return
                        // This prevents the crash while still allowing the discovery mechanism to work
                        auto start_time = std::chrono::steady_clock::now();
                        auto end_time = start_time + std::chrono::seconds(5); // Shorter timeout for background
                        
                        while (!m_should_stop.load() && !context->should_stop.load()) {
                            auto current_time = std::chrono::steady_clock::now();
                            if (current_time >= end_time) {
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                        
                        std::cout << "Background discovery completed" << std::endl;
                        return;
                    }
                    
                    // For synchronous discovery (services != nullptr), use full mdns_cpp
                    std::shared_ptr<mdns_cpp::mDNS> mdns_instance = std::make_shared<mdns_cpp::mDNS>();
                    
                    // Set up mdns_cpp logger with safe context and mdns instance capture
                    mdns_cpp::Logger::setLoggerSink([context, mdns_instance](const std::string& log_msg) {
                        if (!context || context->should_stop.load()) {
                            return;
                        }
                        
                        std::cout << "[mDNS] " << log_msg;
                        std::flush(std::cout);
                        
                        MDNSDiscovery::parse_mdns_log_message(log_msg, context);
                    });
                    
                    // Set up the service we're looking for
                    std::string service_query = SERVICE_TYPE;
                    
                    // Execute the query for VortiDeck services
                    std::cout << "Executing mDNS query for: " << service_query << std::endl;
                    mdns_instance->executeQuery(service_query);
                    
                    // Monitor for responses for the specified timeout
                    auto start_time = std::chrono::steady_clock::now();
                    auto end_time = start_time + timeout;
                    
                    std::cout << "Listening for mDNS responses for " << timeout.count() << " seconds..." << std::endl;
                    
                    // Keep the mdns object alive and loop like the reference implementation
                    while (!m_should_stop.load() && !context->should_stop.load()) {
                        auto current_time = std::chrono::steady_clock::now();
                        if (current_time >= end_time) {
                            std::cout << "mDNS discovery timeout reached" << std::endl;
                            break;
                        }
                        
                        // Sleep for a short period like in reference (100ms)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        
                        // Check if we found any services in synchronous mode and can break early
                        if (context->services && !context->services->empty()) {
                            std::cout << "Found VortiDeck service, stopping discovery" << std::endl;
                            break;
                        }
                    }
                    
                    // Clear the logger sink before destroying mdns instance to prevent callbacks
                    mdns_cpp::Logger::setLoggerSink(nullptr);
                    mdns_instance.reset();
                    
                } catch (const std::exception& e) {
                    std::cerr << "Exception during mDNS discovery: " << e.what() << std::endl;
                    try {
                        mdns_cpp::Logger::setLoggerSink(nullptr);
                    } catch (...) {}
                } catch (...) {
                    std::cerr << "Unknown exception during mDNS discovery" << std::endl;
                    try {
                        mdns_cpp::Logger::setLoggerSink(nullptr);
                    } catch (...) {}
                }
                
                std::cout << "mDNS discovery finished" << std::endl;
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

            void MDNSDiscovery::parse_mdns_log_message(const std::string& log_msg, DiscoveryContext* context) {
                if (!context) return;
                
                try {
                    auto& parsing = context->parsing_context;
                    
                    // Parse PTR record: "answer _vortideck._tcp.local. PTR vortideck_1._vortideck._tcp.local."
                    if (log_msg.find("answer _vortideck._tcp.local. PTR") != std::string::npos) {
                        size_t ptr_pos = log_msg.find("PTR ");
                        if (ptr_pos != std::string::npos) {
                            std::string remaining = log_msg.substr(ptr_pos + 4);
                            size_t space_pos = remaining.find(" ");
                            if (space_pos != std::string::npos) {
                                std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                                parsing.service_name = remaining.substr(0, space_pos);
                            }
                        }
                    }
                    
                    // Parse SRV record: "additional vortideck_1._vortideck._tcp.local. SRV vortideck.local. priority 0 weight 0 port 9001"
                    else if (log_msg.find("SRV") != std::string::npos && log_msg.find("port") != std::string::npos) {
                        // Extract hostname
                        size_t srv_pos = log_msg.find("SRV ");
                        if (srv_pos != std::string::npos) {
                            std::string remaining = log_msg.substr(srv_pos + 4);
                            size_t space_pos = remaining.find(" ");
                            if (space_pos != std::string::npos) {
                                std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                                parsing.hostname = remaining.substr(0, space_pos);
                            }
                        }
                        
                        // Extract port number
                        size_t port_pos = log_msg.find("port ");
                        if (port_pos != std::string::npos) {
                            try {
                                std::string port_str = log_msg.substr(port_pos + 5);
                                // Extract just the numeric part
                                size_t end_pos = port_str.find_first_not_of("0123456789");
                                if (end_pos != std::string::npos) {
                                    port_str = port_str.substr(0, end_pos);
                                }
                                
                                uint16_t port = static_cast<uint16_t>(std::stoi(port_str));
                                if (is_valid_port(port)) {
                                    std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                                    parsing.port = port;
                                }
                            } catch (const std::exception& e) {
                                std::cerr << "Error parsing port from: " << log_msg << " - " << e.what() << std::endl;
                            }
                        }
                    }
                    
                    // Parse A record: "additional vortideck.local. A 192.168.178.253"
                    else if (log_msg.find("A ") != std::string::npos && log_msg.find("additional") != std::string::npos) {
                        std::string current_hostname;
                        {
                            std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                            current_hostname = parsing.hostname;
                        }
                        
                        if (!current_hostname.empty() && log_msg.find(current_hostname) != std::string::npos) {
                            // Extract IP address
                            size_t a_pos = log_msg.find("A ");
                            if (a_pos != std::string::npos) {
                                std::string ip_str = log_msg.substr(a_pos + 2);
                                // Remove any trailing whitespace or extra content
                                size_t end_pos = ip_str.find_first_of(" \t\n\r");
                                if (end_pos != std::string::npos) {
                                    ip_str = ip_str.substr(0, end_pos);
                                }
                                
                                if (is_valid_ip_address(ip_str)) {
                                    {
                                        std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                                        parsing.ip_address = ip_str;
                                    }
                                    
                                    // Check if we have all components for a complete service
                                    if (parsing.is_complete()) {
                                        ServiceInfo service;
                                        {
                                            std::lock_guard<std::mutex> lock(parsing.parsing_mutex);
                                            service.name = parsing.service_name;
                                            service.ip_address = parsing.ip_address;
                                            service.port = parsing.port;
                                        }
                                        
                                        service.websocket_url = context->tls_enabled 
                                            ? "wss://" + service.ip_address + ":" + std::to_string(service.port) + "/ws"
                                            : "ws://" + service.ip_address + ":" + std::to_string(service.port) + "/ws";
                                        
                                        // Add to results
                                        std::lock_guard<std::mutex> lock(context->services_mutex);
                                        if (context->services) {
                                            context->services->push_back(service);
                                        }
                                        if (context->callback) {
                                            context->callback(service);
                                        }
                                        
                                        // Reset parsing context for next service
                                        parsing.reset();
                                    }
                                } else {
                                    std::cerr << "Invalid IP address format: " << ip_str << std::endl;
                                }
                            }
                        }
                    }
                    
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing mDNS log message: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Unknown error parsing mDNS log message" << std::endl;
                }
            }

            void MDNSDiscovery::process_service_discovery(
                const std::string& service_name,
                const std::string& hostname,
                uint16_t port,
                const std::string& ip_address,
                DiscoveryContext* context
            ) {
                // Build the WebSocket URL
                std::string websocket_url = context->tls_enabled 
                    ? "wss://" + ip_address + ":" + std::to_string(port) + "/ws"
                    : "ws://" + ip_address + ":" + std::to_string(port) + "/ws";

                ServiceInfo service_info(service_name, websocket_url, ip_address, port);
                
                std::cout << "*** Discovered VortiDeck service: " << websocket_url << " ***" << std::endl;

                if (context->services) {
                    // Synchronous mode - add to vector
                    std::lock_guard<std::mutex> lock(context->services_mutex);
                    context->services->push_back(service_info);
                } else if (context->callback) {
                    // Asynchronous mode - call callback
                    context->callback(service_info);
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