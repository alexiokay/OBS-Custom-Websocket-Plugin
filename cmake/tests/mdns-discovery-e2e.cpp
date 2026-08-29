#include "mdns_discovery.hpp"
#include "mdns-test-advertiser.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const bool probe_existing = argc == 3 && std::string(argv[1]) == "--probe-existing";
    const std::uint16_t test_port = probe_existing
        ? static_cast<std::uint16_t>(std::stoi(argv[2]))
        : static_cast<std::uint16_t>(39001);

    std::vector<std::string> logs;
    vorti::applets::obs_plugin::MDNSDiscovery discovery(
        [&logs](const std::string& message) { logs.push_back(message); });

    std::unique_ptr<MdnsTestAdvertiser> advertised_service;
    if (!probe_existing) {
        advertised_service = std::make_unique<MdnsTestAdvertiser>(test_port);
        advertised_service->start();
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
        advertised_service->stop();
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
