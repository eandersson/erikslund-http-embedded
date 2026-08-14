#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace erikslund::http {

inline constexpr size_t kIpv6ByteCount = 16;

inline constexpr size_t kIpv4MappedPrefixBits = 96;

struct PeerAddress {
    // IPv4 addresses use IPv4-mapped IPv6 form.
    std::array<uint8_t, kIpv6ByteCount> bytes{};

    // Controls presentation; bytes is mapped either way.
    bool is_v4 = false;

    // Host byte order; zero for Unix-domain peers.
    uint16_t port = 0;

    // Unix-domain peers are not matched against CIDRs.
    bool is_unix = false;

    // Returns "10.0.1.5:51234", "[fd00::1]:51234", or "unix".
    [[nodiscard]] std::string to_string() const;

    // Includes Unix-domain peers.
    [[nodiscard]] bool is_loopback() const noexcept;
};

} // namespace erikslund::http
