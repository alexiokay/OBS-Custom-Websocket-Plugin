#include <iostream>
#include <chrono>
#include <thread>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "mdns_discovery.hpp"

volatile bool g_running = true;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", stopping..." << std::endl;
    g_running = false;
}

void print_service_info(const vorti::applets::obs_plugin::ServiceInfo& service) {
    std::cout << "=== Found VortiDeck Service ===" << std::endl;
    std::cout << "Name: " << service.name << std::endl;
    std::cout << "WebSocket URL: " << service.websocket_url << std::endl;
    std::cout << "IP Address: " << service.ip_address << std::endl;
    std::cout << "Port: " << service.port << std::endl;
    std::cout << "=============================" << std::endl;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        return 1;
    }
#endif

    std::cout << "mDNS Discovery Test Tool" << std::endl;
    std::cout << "========================" << std::endl;
    std::cout << "Searching for VortiDeck services..." << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl << std::endl;

    try {
        vorti::applets::obs_plugin::MDNSDiscovery discovery;
        
        // Test synchronous discovery
        std::cout << "Starting synchronous discovery (10 seconds)..." << std::endl;
        auto services = discovery.discover_services(std::chrono::seconds(10), false);
        
        if (services.empty()) {
            std::cout << "No VortiDeck services found in synchronous mode." << std::endl;
        } else {
            std::cout << "Found " << services.size() << " service(s):" << std::endl;
            for (const auto& service : services) {
                print_service_info(service);
            }
        }
        
        // Test asynchronous discovery if no services found
        if (services.empty() && g_running) {
            std::cout << "\nStarting asynchronous discovery (30 seconds)..." << std::endl;
            
            std::atomic<bool> found_service{false};
            discovery.discover_services_async(
                [&found_service](const vorti::applets::obs_plugin::ServiceInfo& service) {
                    print_service_info(service);
                    found_service = true;
                },
                std::chrono::seconds(30),
                false
            );
            
            // Wait for discovery to complete or user interruption
            auto start_time = std::chrono::steady_clock::now();
            while (g_running && discovery.is_discovering() && 
                   std::chrono::steady_clock::now() - start_time < std::chrono::seconds(30)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            discovery.stop_discovery();
            
            if (!found_service) {
                std::cout << "No VortiDeck services found in asynchronous mode." << std::endl;
            }
        }
        
        // Test utility functions
        std::cout << "\nTesting utility functions..." << std::endl;
        bool available = vorti::applets::obs_plugin::mdns_utils::is_vortideck_service_available(
            std::chrono::seconds(5)
        );
        std::cout << "VortiDeck service available: " << (available ? "Yes" : "No") << std::endl;
        
        if (available) {
            auto service = vorti::applets::obs_plugin::mdns_utils::get_first_vortideck_service(
                std::chrono::seconds(5), false
            );
            std::cout << "First service URL: " << service.websocket_url << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "\nTest completed." << std::endl;
    return 0;
}