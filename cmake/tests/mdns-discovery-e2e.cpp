#include "mdns_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "mdns_cpp/mdns.hpp"
#include "mdns_cpp/logger.hpp"

int main(int argc, char** argv)
{
    const bool probe_existing = argc == 3 && std::string(argv[1]) == "--probe-existing";
    const std::uint16_t test_port = probe_existing
        ? static_cast<std::uint16_t>(std::stoi(argv[2]))
        : static_cast<std::uint16_t>(39001);

    std::vector<std::string> logs;
    vorti::applets::obs_plugin::MDNSDiscovery discovery(
        [&logs](const std::string& message) { logs.push_back(message); });

    std::unique_ptr<mdns_cpp::mDNS> advertised_service;
    if (!probe_existing) {
        mdns_cpp::Logger::setLoggerSink([](const std::string&) {});
        advertised_service = std::make_unique<mdns_cpp::mDNS>();
        advertised_service->setServiceName(vorti::applets::obs_plugin::MDNSDiscovery::SERVICE_TYPE);
        advertised_service->setServiceHostname("vortideck-e2e");
        advertised_service->setServicePort(test_port);
        advertised_service->startService();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    bool callback_received = false;
    const auto services = discovery.discover_services(
        std::chrono::seconds(3),
        false,
        [&callback_received, test_port](const auto& service) {
            if (service.port == test_port) callback_received = true;
        });

    const auto match = std::find_if(services.begin(), services.end(), [test_port](const auto& service) {
        return service.port == test_port && !service.ip_address.empty() &&
               service.websocket_url.ends_with(":" + std::to_string(test_port) + "/ws");
    });

    if (advertised_service) {
        std::thread stop_thread([&advertised_service]() { advertised_service->stopService(); });
        vorti::applets::obs_plugin::MDNSDiscovery wake_discovery;
        wake_discovery.discover_services(std::chrono::seconds(1));
        stop_thread.join();
        mdns_cpp::Logger::useDefaultSink();
    }

    if (match == services.end() || !callback_received) {
        std::cerr << "Structured mDNS discovery did not resolve the advertised test service\n";
        for (const auto& message : logs) {
            std::cerr << message << '\n';
        }
        return 1;
    }

    std::cout << "Resolved " << match->name << " at " << match->websocket_url << '\n';
    return 0;
}
