#pragma once

#include <cstdint>
#include <memory>

class MdnsTestAdvertiser final {
public:
    explicit MdnsTestAdvertiser(std::uint16_t port);
    ~MdnsTestAdvertiser();

    MdnsTestAdvertiser(const MdnsTestAdvertiser&) = delete;
    MdnsTestAdvertiser& operator=(const MdnsTestAdvertiser&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
