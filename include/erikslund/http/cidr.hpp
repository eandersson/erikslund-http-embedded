#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "erikslund/http/peer_address.hpp"

namespace erikslund::http {

enum class CidrError : uint8_t {
    MalformedEntry,
    BadPrefixLength,
    BadAddress,
};

// IPv4 rules also match IPv4-mapped IPv6 peers.
class CidrAllowList {
public:
    struct Rule {
        // IPv4 networks use IPv4-mapped form.
        std::array<uint8_t, kIpv6ByteCount> network{};
        // IPv4 prefixes include the 96 mapped-prefix bits.
        uint8_t prefix_bits = 0;
        bool is_v4 = false;
    };

    CidrAllowList() = default;

    // Fails instead of applying a partial allow list.
    [[nodiscard]] static std::expected<CidrAllowList, CidrError> parse(
        std::span<const std::string> entries);

    // An empty list allows every peer.
    [[nodiscard]] bool empty() const noexcept { return rules_.empty(); }

    [[nodiscard]] bool allows(const PeerAddress& peer) const noexcept;

    [[nodiscard]] std::span<const Rule> rules() const noexcept { return rules_; }

private:
    std::vector<Rule> rules_;
};

} // namespace erikslund::http
