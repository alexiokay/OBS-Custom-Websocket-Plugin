#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vorti::applets::obs_plugin::structured_mdns {

enum class RecordType { Ptr, Srv, A, Aaaa, Txt, Other };

struct Record {
    RecordType type{RecordType::Other};
    std::string name;
    std::string target;
    std::string address;
    std::uint16_t port{0};
    std::vector<std::pair<std::string, std::string>> txt;
};

using RecordCallback = std::function<void(const Record&)>;

void query(
    const std::string& service,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>& should_stop,
    RecordCallback callback);

} // namespace vorti::applets::obs_plugin::structured_mdns
