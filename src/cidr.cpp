#include "erikslund/http/cidr.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/peer_address.hpp"

namespace erikslund::http {

namespace {

constexpr unsigned int kBitsPerByte = 8;
constexpr unsigned int kAllBitsSet = 0xFFu;

constexpr unsigned int kIpv6PrefixBitCount =
    static_cast<unsigned int>(kIpv6ByteCount) * kBitsPerByte;
constexpr unsigned int kMappedPrefixOffsetBits = static_cast<unsigned int>(kIpv4MappedPrefixBits);
constexpr unsigned int kIpv4PrefixBitCount = kIpv6PrefixBitCount - kMappedPrefixOffsetBits;

constexpr size_t kIpv4OffsetInMapped = kMappedPrefixOffsetBits / kBitsPerByte;
constexpr size_t kMappedMarkerByteCount = 2;
constexpr size_t kMappedMarkerFirstIndex = kIpv4OffsetInMapped - kMappedMarkerByteCount;
constexpr size_t kMappedMarkerLastIndex = kIpv4OffsetInMapped - 1;
constexpr uint8_t kMappedMarkerByte = 0xFF;

constexpr uint8_t kIpv4LoopbackFirstOctet = 127;
constexpr uint8_t kIpv6LoopbackLastByte = 1;

constexpr char kPrefixSeparator = '/';

constexpr size_t kAddressTextCapacity = INET6_ADDRSTRLEN;

constexpr std::string_view kUnixPeerText = "unix";
constexpr std::string_view kUnpresentablePeerText = "unknown";

struct ParsedAddress {
    std::array<uint8_t, kIpv6ByteCount> bytes{};
    bool from_dotted_quad = false;
};

[[nodiscard]] bool is_ipv4_mapped(const std::array<uint8_t, kIpv6ByteCount>& bytes) noexcept {
    for (size_t index = 0; index < kMappedMarkerFirstIndex; ++index)
        if (bytes[index] != 0)
            return false;
    return bytes[kMappedMarkerFirstIndex] == kMappedMarkerByte &&
           bytes[kMappedMarkerLastIndex] == kMappedMarkerByte;
}

[[nodiscard]] bool is_ipv6_loopback(const std::array<uint8_t, kIpv6ByteCount>& bytes) noexcept {
    for (size_t index = 0; index + 1 < bytes.size(); ++index)
        if (bytes[index] != 0)
            return false;
    return bytes[bytes.size() - 1] == kIpv6LoopbackLastByte;
}

[[nodiscard]] std::optional<ParsedAddress> parse_address(std::string_view text) noexcept {
    if (text.empty() || text.size() >= kAddressTextCapacity ||
        text.find('\0') != std::string_view::npos)
        return std::nullopt;

    std::array<char, kAddressTextCapacity> terminated{};
    std::memcpy(terminated.data(), text.data(), text.size());

    ParsedAddress parsed{};

    in_addr address_v4{};
    if (inet_pton(AF_INET, terminated.data(), &address_v4) == 1) {
        parsed.bytes[kMappedMarkerFirstIndex] = kMappedMarkerByte;
        parsed.bytes[kMappedMarkerLastIndex] = kMappedMarkerByte;
        std::memcpy(parsed.bytes.data() + kIpv4OffsetInMapped, &address_v4.s_addr,
                    sizeof(address_v4.s_addr));
        parsed.from_dotted_quad = true;
        return parsed;
    }

    in6_addr address_v6{};
    if (inet_pton(AF_INET6, terminated.data(), &address_v6) == 1) {
        std::memcpy(parsed.bytes.data(), address_v6.s6_addr, parsed.bytes.size());
        return parsed;
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<unsigned int, CidrError> parse_prefix(std::string_view text,
                                                                  bool from_dotted_quad) noexcept {
    unsigned int written_bits = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const std::from_chars_result conversion = std::from_chars(first, last, written_bits);

    if (conversion.ec != std::errc{} || conversion.ptr != last)
        return std::unexpected(CidrError::BadPrefixLength);

    const unsigned int maximum_bits = from_dotted_quad ? kIpv4PrefixBitCount : kIpv6PrefixBitCount;
    if (written_bits > maximum_bits)
        return std::unexpected(CidrError::BadPrefixLength);

    return from_dotted_quad ? written_bits + kMappedPrefixOffsetBits : written_bits;
}

void clear_host_bits(std::array<uint8_t, kIpv6ByteCount>& bytes,
                     unsigned int prefix_bits) noexcept {
    const size_t whole_bytes = prefix_bits / kBitsPerByte;
    const unsigned int remaining_bits = prefix_bits % kBitsPerByte;

    if (remaining_bits != 0 && whole_bytes < bytes.size()) {
        const unsigned int mask = kAllBitsSet << (kBitsPerByte - remaining_bits);
        bytes[whole_bytes] = static_cast<uint8_t>(bytes[whole_bytes] & mask);
    }
    for (size_t index = whole_bytes + (remaining_bits != 0 ? 1 : 0); index < bytes.size(); ++index)
        bytes[index] = 0;
}

[[nodiscard]] std::expected<CidrAllowList::Rule, CidrError> parse_entry(std::string_view entry) {
    if (entry.empty())
        return std::unexpected(CidrError::MalformedEntry);

    const size_t separator = entry.find(kPrefixSeparator);
    const bool has_prefix = separator != std::string_view::npos;
    const std::string_view address_text = has_prefix ? entry.substr(0, separator) : entry;
    const std::string_view prefix_text =
        has_prefix ? entry.substr(separator + 1) : std::string_view{};

    if (address_text.empty() || prefix_text.find(kPrefixSeparator) != std::string_view::npos)
        return std::unexpected(CidrError::MalformedEntry);

    const std::optional<ParsedAddress> address = parse_address(address_text);
    if (!address)
        return std::unexpected(CidrError::BadAddress);

    unsigned int prefix_bits = kIpv6PrefixBitCount;
    if (has_prefix) {
        const std::expected<unsigned int, CidrError> parsed_prefix =
            parse_prefix(prefix_text, address->from_dotted_quad);
        if (!parsed_prefix)
            return std::unexpected(parsed_prefix.error());
        prefix_bits = *parsed_prefix;
    }
    ERIKSLUND_HTTP_ASSERT(prefix_bits <= kIpv6PrefixBitCount);

    CidrAllowList::Rule rule{};
    rule.network = address->bytes;
    rule.prefix_bits = static_cast<uint8_t>(prefix_bits);
    clear_host_bits(rule.network, prefix_bits);
    rule.is_v4 = address->from_dotted_quad ||
                 (is_ipv4_mapped(rule.network) && prefix_bits >= kMappedPrefixOffsetBits);
    return rule;
}

[[nodiscard]] bool matches(const CidrAllowList::Rule& rule,
                           const std::array<uint8_t, kIpv6ByteCount>& address) noexcept {
    const unsigned int prefix_bits = rule.prefix_bits;

    ERIKSLUND_HTTP_ASSERT(prefix_bits <= kIpv6PrefixBitCount);
    if (prefix_bits > kIpv6PrefixBitCount)
        return false;

    const size_t whole_bytes = prefix_bits / kBitsPerByte;
    for (size_t index = 0; index < whole_bytes; ++index)
        if (address[index] != rule.network[index])
            return false;

    const unsigned int remaining_bits = prefix_bits % kBitsPerByte;
    if (remaining_bits == 0)
        return true;

    const unsigned int mask = kAllBitsSet << (kBitsPerByte - remaining_bits);
    return (address[whole_bytes] & mask) == (rule.network[whole_bytes] & mask);
}

[[nodiscard]] bool write_address_text(const std::array<uint8_t, kIpv6ByteCount>& bytes,
                                      bool as_ipv4,
                                      std::array<char, kAddressTextCapacity>& out) noexcept {
    if (as_ipv4) {
        in_addr address{};
        std::memcpy(&address.s_addr, bytes.data() + kIpv4OffsetInMapped, sizeof(address.s_addr));
        return inet_ntop(AF_INET, &address, out.data(), static_cast<socklen_t>(out.size())) !=
               nullptr;
    }
    in6_addr address{};
    std::memcpy(address.s6_addr, bytes.data(), bytes.size());
    return inet_ntop(AF_INET6, &address, out.data(), static_cast<socklen_t>(out.size())) != nullptr;
}

} // namespace

std::expected<CidrAllowList, CidrError> CidrAllowList::parse(std::span<const std::string> entries) {
    CidrAllowList list;
    list.rules_.reserve(entries.size());
    for (const std::string& entry : entries) {
        const std::expected<Rule, CidrError> rule = parse_entry(entry);
        if (!rule)
            return std::unexpected(rule.error());
        list.rules_.push_back(*rule);
    }
    return list;
}

bool CidrAllowList::allows(const PeerAddress& peer) const noexcept {
    if (rules_.empty())
        return true;

    if (peer.is_unix)
        return true;

    for (const Rule& rule : rules_)
        if (matches(rule, peer.bytes))
            return true;
    return false;
}

std::string PeerAddress::to_string() const {
    if (is_unix)
        return std::string(kUnixPeerText);

    const bool present_as_ipv4 = is_v4 || is_ipv4_mapped(bytes);

    std::array<char, kAddressTextCapacity> text{};
    if (!write_address_text(bytes, present_as_ipv4, text))
        return std::string(kUnpresentablePeerText);

    if (present_as_ipv4)
        return std::format("{}:{}", text.data(), port);
    return std::format("[{}]:{}", text.data(), port);
}

bool PeerAddress::is_loopback() const noexcept {
    if (is_unix)
        return true;
    if (is_v4 || is_ipv4_mapped(bytes))
        return bytes[kIpv4OffsetInMapped] == kIpv4LoopbackFirstOctet;
    return is_ipv6_loopback(bytes);
}

} // namespace erikslund::http
