
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <doctest/doctest.h>

#include "erikslund/http/cidr.hpp"
#include "erikslund/http/peer_address.hpp"

namespace erikslund::http {

namespace {

constexpr size_t kBitsPerByte = 8;

constexpr size_t kIpv4OffsetInMapped = kIpv4MappedPrefixBits / kBitsPerByte;
constexpr size_t kMappedMarkerByteCount = 2;
constexpr size_t kMappedMarkerFirstIndex = kIpv4OffsetInMapped - kMappedMarkerByteCount;
constexpr size_t kMappedMarkerLastIndex = kIpv4OffsetInMapped - 1;
constexpr uint8_t kMappedMarkerByte = 0xFF;

constexpr uint8_t kIpv4RuleOffsetBits = static_cast<uint8_t>(kIpv4MappedPrefixBits);
constexpr uint8_t kSingleHostPrefixBits = static_cast<uint8_t>(kIpv6ByteCount * kBitsPerByte);
constexpr uint8_t kEightBitPrefix = 8;
constexpr uint8_t kTwentyFourBitPrefix = 24;

constexpr std::string_view kMappedTenNetworkEntry = "::ffff:10.0.0.0/104";

constexpr std::string_view kUniqueLocalLastAddress = "fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";
constexpr std::string_view kJustBelowUniqueLocal = "fcff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";
constexpr std::string_view kJustAboveUniqueLocal = "fe00::";

constexpr uint16_t kEphemeralPort = 51'234;
constexpr std::string_view kIpv4PeerText = "10.0.1.5:51234";
constexpr std::string_view kIpv4LoopbackPeerText = "127.0.0.1:51234";
constexpr std::string_view kIpv6PeerText = "[fd00::1]:51234";
constexpr std::string_view kIpv6LoopbackPeerText = "[::1]:51234";
constexpr std::string_view kUnixPeerText = "unix";

enum class ArrivedAs : uint8_t { Ipv4, Ipv6 };

[[nodiscard]] PeerAddress peer_from_text(std::string_view text, ArrivedAs family) {
    const std::string terminated(text);

    PeerAddress peer{};
    peer.is_v4 = family == ArrivedAs::Ipv4;
    peer.port = kEphemeralPort;

    in_addr address_v4{};
    if (inet_pton(AF_INET, terminated.c_str(), &address_v4) == 1) {
        peer.bytes[kMappedMarkerFirstIndex] = kMappedMarkerByte;
        peer.bytes[kMappedMarkerLastIndex] = kMappedMarkerByte;
        std::memcpy(peer.bytes.data() + kIpv4OffsetInMapped, &address_v4.s_addr,
                    sizeof(address_v4.s_addr));
        return peer;
    }

    in6_addr address_v6{};
    REQUIRE_MESSAGE(inet_pton(AF_INET6, terminated.c_str(), &address_v6) == 1,
                    "the test's own peer address must parse");
    std::memcpy(peer.bytes.data(), address_v6.s6_addr, peer.bytes.size());
    return peer;
}

[[nodiscard]] PeerAddress unix_peer() {
    PeerAddress peer{};
    peer.is_unix = true;
    peer.port = kEphemeralPort;
    return peer;
}

[[nodiscard]] CidrAllowList parsed_allow_list(std::initializer_list<std::string> entries) {
    const std::vector<std::string> owned(entries);
    const std::expected<CidrAllowList, CidrError> list = CidrAllowList::parse(owned);
    REQUIRE_MESSAGE(list.has_value(), "the test's own allow-list entries must parse");
    return *list;
}

[[nodiscard]] CidrError rejection_of(std::string_view entry) {
    const std::vector<std::string> owned{std::string(entry)};
    const std::expected<CidrAllowList, CidrError> list = CidrAllowList::parse(owned);
    REQUIRE_FALSE(list.has_value());
    return list.error();
}

} // namespace

TEST_SUITE("cidr") {

TEST_CASE("an_ipv4_slash_8_rule_matches_every_address_in_the_block_and_stops_at_its_edge") {
    const CidrAllowList list = parsed_allow_list({"10.0.0.0/8"});
    CHECK_FALSE(list.empty());
    REQUIRE(list.rules().size() == 1);

    CHECK(list.allows(peer_from_text("10.0.0.0", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("10.0.1.5", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("10.255.255.255", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("11.0.0.0", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("9.255.255.255", ArrivedAs::Ipv4)));
}

TEST_CASE("an_ipv4_rule_is_stored_ipv4_mapped_with_its_prefix_counted_over_all_128_bits") {
    const CidrAllowList list = parsed_allow_list({"10.0.0.0/8"});
    REQUIRE(list.rules().size() == 1);
    const CidrAllowList::Rule& rule = list.rules().front();

    CHECK(rule.is_v4);
    CHECK(rule.prefix_bits == kEightBitPrefix + kIpv4RuleOffsetBits);
    CHECK(rule.network == peer_from_text("10.0.0.0", ArrivedAs::Ipv4).bytes);
    CHECK(rule.network[kMappedMarkerFirstIndex] == kMappedMarkerByte);
    CHECK(rule.network[kMappedMarkerLastIndex] == kMappedMarkerByte);
}

TEST_CASE("an_ipv4_rule_written_with_host_bits_set_names_the_network_and_not_the_host") {
    const CidrAllowList list = parsed_allow_list({"10.1.2.3/8"});
    REQUIRE(list.rules().size() == 1);

    CHECK(list.rules().front().network == peer_from_text("10.0.0.0", ArrivedAs::Ipv4).bytes);
    CHECK(list.allows(peer_from_text("10.9.9.9", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("10.1.2.3", ArrivedAs::Ipv4)));
}

TEST_CASE("an_ipv4_slash_24_rule_matches_only_the_addresses_sharing_its_first_three_octets") {
    const CidrAllowList list = parsed_allow_list({"192.168.1.0/24"});
    REQUIRE(list.rules().size() == 1);
    CHECK(list.rules().front().prefix_bits == kTwentyFourBitPrefix + kIpv4RuleOffsetBits);

    CHECK(list.allows(peer_from_text("192.168.1.0", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("192.168.1.255", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("192.168.0.255", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("192.168.2.0", ArrivedAs::Ipv4)));
}

TEST_CASE("an_ipv4_slash_32_rule_matches_exactly_one_address") {
    const CidrAllowList list = parsed_allow_list({"203.0.113.7/32"});
    REQUIRE(list.rules().size() == 1);
    CHECK(list.rules().front().prefix_bits == kSingleHostPrefixBits);

    CHECK(list.allows(peer_from_text("203.0.113.7", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("203.0.113.6", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("203.0.113.8", ArrivedAs::Ipv4)));
}

TEST_CASE("an_ipv6_slash_128_rule_matches_exactly_one_address") {
    const CidrAllowList list = parsed_allow_list({"fd00::1/128"});
    REQUIRE(list.rules().size() == 1);
    CHECK_FALSE(list.rules().front().is_v4);
    CHECK(list.rules().front().prefix_bits == kSingleHostPrefixBits);

    CHECK(list.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("fd00::2", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("fd00::", ArrivedAs::Ipv6)));
}

TEST_CASE("an_ipv6_slash_8_rule_matches_on_its_first_byte_and_ignores_the_other_fifteen") {
    const CidrAllowList list = parsed_allow_list({"fd00::/8"});
    REQUIRE(list.rules().size() == 1);
    CHECK_FALSE(list.rules().front().is_v4);
    CHECK(list.rules().front().prefix_bits == kEightBitPrefix);

    CHECK(list.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK(list.allows(peer_from_text(kUniqueLocalLastAddress, ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text(kJustBelowUniqueLocal, ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text(kJustAboveUniqueLocal, ArrivedAs::Ipv6)));
}

TEST_CASE("a_prefix_ending_inside_a_byte_compares_only_the_bits_ahead_of_where_it_ends") {
    const CidrAllowList twelve = parsed_allow_list({"10.0.0.0/12"});
    CHECK(twelve.allows(peer_from_text("10.15.255.255", ArrivedAs::Ipv4)));
    CHECK_FALSE(twelve.allows(peer_from_text("10.16.0.0", ArrivedAs::Ipv4)));

    const CidrAllowList thirty = parsed_allow_list({"203.0.113.0/30"});
    CHECK(thirty.allows(peer_from_text("203.0.113.3", ArrivedAs::Ipv4)));
    CHECK_FALSE(thirty.allows(peer_from_text("203.0.113.4", ArrivedAs::Ipv4)));

    const CidrAllowList four = parsed_allow_list({"fd00::/4"});
    CHECK(four.allows(peer_from_text("fc00::1", ArrivedAs::Ipv6)));
    CHECK_FALSE(four.allows(peer_from_text("ef00::1", ArrivedAs::Ipv6)));

    const CidrAllowList near_whole = parsed_allow_list({"fd00::/127"});
    CHECK(near_whole.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK_FALSE(near_whole.allows(peer_from_text("fd00::2", ArrivedAs::Ipv6)));
}

TEST_CASE("an_entry_with_no_prefix_is_a_single_host_rule_in_either_family") {
    const CidrAllowList list = parsed_allow_list({"192.168.1.7", "fd00::1"});
    REQUIRE(list.rules().size() == 2);
    CHECK(list.rules()[0].prefix_bits == kSingleHostPrefixBits);
    CHECK(list.rules()[1].prefix_bits == kSingleHostPrefixBits);

    CHECK(list.allows(peer_from_text("192.168.1.7", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("192.168.1.6", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("192.168.1.8", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("fd00::2", ArrivedAs::Ipv6)));
}

TEST_CASE("an_ipv4_rule_matches_a_dual_stack_peer_that_arrives_ipv4_mapped") {
    const CidrAllowList list = parsed_allow_list({"10.0.0.0/8"});

    const PeerAddress native = peer_from_text("10.0.1.5", ArrivedAs::Ipv4);
    const PeerAddress mapped = peer_from_text("::ffff:10.0.1.5", ArrivedAs::Ipv4);
    CHECK(mapped.bytes == native.bytes);
    CHECK(list.allows(mapped));
    CHECK(list.allows(native));

    const PeerAddress mapped_without_the_flag = peer_from_text("::ffff:10.0.1.5", ArrivedAs::Ipv6);
    CHECK_FALSE(mapped_without_the_flag.is_v4);
    CHECK(list.allows(mapped_without_the_flag));

    CHECK(list.allows(peer_from_text("::ffff:10.255.255.255", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("::ffff:11.0.0.0", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("::ffff:192.168.1.7", ArrivedAs::Ipv6)));
}

TEST_CASE("an_ipv6_rule_does_not_match_an_ipv4_peer_merely_because_both_are_sixteen_bytes") {
    const CidrAllowList list = parsed_allow_list({"fd00::/8"});
    CHECK_FALSE(list.allows(peer_from_text("10.0.1.5", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("::ffff:10.0.1.5", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("253.0.0.1", ArrivedAs::Ipv4)));
}

TEST_CASE("a_rule_written_in_ipv4_mapped_notation_matches_a_native_ipv4_peer") {
    const CidrAllowList list = parsed_allow_list({std::string(kMappedTenNetworkEntry)});
    REQUIRE(list.rules().size() == 1);
    CHECK(list.rules().front().is_v4);

    CHECK(list.allows(peer_from_text("10.0.1.5", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("10.255.255.255", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("11.0.0.0", ArrivedAs::Ipv4)));
}

TEST_CASE("an_empty_allow_list_reports_empty_and_allows_every_peer") {
    const CidrAllowList unconfigured;
    CHECK(unconfigured.empty());
    CHECK(unconfigured.rules().empty());
    CHECK(unconfigured.allows(peer_from_text("10.0.1.5", ArrivedAs::Ipv4)));
    CHECK(unconfigured.allows(peer_from_text("203.0.113.7", ArrivedAs::Ipv4)));
    CHECK(unconfigured.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK(unconfigured.allows(unix_peer()));

    const CidrAllowList parsed = parsed_allow_list({});
    CHECK(parsed.empty());
    CHECK(parsed.allows(peer_from_text("203.0.113.7", ArrivedAs::Ipv4)));
}

TEST_CASE("a_list_holding_rules_of_both_families_denies_a_peer_outside_all_of_them") {
    const CidrAllowList list = parsed_allow_list({"10.0.0.0/8", "fd00::/8"});
    CHECK_FALSE(list.empty());
    REQUIRE(list.rules().size() == 2);

    CHECK(list.allows(peer_from_text("10.0.1.5", ArrivedAs::Ipv4)));
    CHECK(list.allows(peer_from_text("fd00::1", ArrivedAs::Ipv6)));
    CHECK_FALSE(list.allows(peer_from_text("192.168.1.7", ArrivedAs::Ipv4)));
    CHECK_FALSE(list.allows(peer_from_text("fc00::1", ArrivedAs::Ipv6)));
}

TEST_CASE("a_unix_domain_peer_is_allowed_by_a_list_no_network_address_of_its_own_would_satisfy") {
    const CidrAllowList list = parsed_allow_list({"203.0.113.7/32"});
    CHECK(list.allows(unix_peer()));
}

TEST_CASE("parse_rejects_an_ipv4_prefix_longer_than_the_thirty_two_bits_it_counts_over") {
    CHECK(rejection_of("10.0.0.0/33") == CidrError::BadPrefixLength);
    CHECK(rejection_of("10.0.0.0/128") == CidrError::BadPrefixLength);
    CHECK(rejection_of("192.168.1.7/255") == CidrError::BadPrefixLength);
    CHECK(parsed_allow_list({"10.0.0.0/32"}).rules().size() == 1);
}

TEST_CASE("parse_rejects_an_ipv6_prefix_longer_than_the_address_it_counts_over") {
    CHECK(rejection_of("fd00::/129") == CidrError::BadPrefixLength);
    CHECK(rejection_of("::1/255") == CidrError::BadPrefixLength);
    CHECK(parsed_allow_list({"fd00::/128"}).rules().size() == 1);
}

TEST_CASE("parse_rejects_an_entry_with_no_address_in_front_of_the_prefix") {
    CHECK(rejection_of("/8") == CidrError::MalformedEntry);
    CHECK(rejection_of("/128") == CidrError::MalformedEntry);
    CHECK(rejection_of("/") == CidrError::MalformedEntry);
}

TEST_CASE("parse_rejects_an_empty_entry") {
    CHECK(rejection_of("") == CidrError::MalformedEntry);
}

TEST_CASE("parse_rejects_an_entry_carrying_a_second_slash") {
    CHECK(rejection_of("10.0.0.0/8/8") == CidrError::MalformedEntry);
    CHECK(rejection_of("fd00::/8/") == CidrError::MalformedEntry);
}

TEST_CASE("parse_rejects_junk_trailing_the_prefix_length") {
    CHECK(rejection_of("10.0.0.0/8bogus") == CidrError::BadPrefixLength);
    CHECK(rejection_of("10.0.0.0/8 ") == CidrError::BadPrefixLength);
    CHECK(rejection_of("fd00::/64;") == CidrError::BadPrefixLength);
}

TEST_CASE("parse_rejects_a_non_numeric_prefix_length") {
    CHECK(rejection_of("10.0.0.0/eight") == CidrError::BadPrefixLength);
    CHECK(rejection_of("10.0.0.0/0x8") == CidrError::BadPrefixLength);
    CHECK(rejection_of("10.0.0.0/") == CidrError::BadPrefixLength);
    CHECK(rejection_of("fd00::/ 64") == CidrError::BadPrefixLength);
}

TEST_CASE("parse_rejects_a_negative_prefix_length_rather_than_wrapping_it") {
    CHECK(rejection_of("10.0.0.0/-1") == CidrError::BadPrefixLength);
    CHECK(rejection_of("10.0.0.0/-8") == CidrError::BadPrefixLength);
    CHECK(rejection_of("fd00::/-64") == CidrError::BadPrefixLength);
}

TEST_CASE("parse_rejects_a_prefix_length_written_with_a_leading_plus") {
    CHECK(rejection_of("10.0.0.0/+8") == CidrError::BadPrefixLength);
    CHECK(rejection_of("fd00::/+64") == CidrError::BadPrefixLength);
}

TEST_CASE("parse_rejects_an_address_that_is_neither_a_dotted_quad_nor_an_ipv6_literal") {
    CHECK(rejection_of("not-an-address/8") == CidrError::BadAddress);
    CHECK(rejection_of("10.0.0.256/8") == CidrError::BadAddress);
    CHECK(rejection_of("10.0.0/8") == CidrError::BadAddress);
    CHECK(rejection_of("10.0.0.0.0/8") == CidrError::BadAddress);
    CHECK(rejection_of("fd00:::1/64") == CidrError::BadAddress);
    CHECK(rejection_of("10.0.0.1junk") == CidrError::BadAddress);
    CHECK(rejection_of("010.0.0.1/32") == CidrError::BadAddress);
}

TEST_CASE("parse_stops_at_the_first_bad_entry_instead_of_keeping_the_ones_that_parsed") {
    const std::vector<std::string> entries{"10.0.0.0/8", "192.168.1.0/33", "fd00::/8"};
    const std::expected<CidrAllowList, CidrError> list = CidrAllowList::parse(entries);
    REQUIRE_FALSE(list.has_value());
    CHECK(list.error() == CidrError::BadPrefixLength);
}

TEST_CASE("to_string_renders_an_ipv4_peer_as_a_dotted_quad_followed_by_its_port") {
    CHECK(peer_from_text("10.0.1.5", ArrivedAs::Ipv4).to_string() == kIpv4PeerText);
    CHECK(peer_from_text("127.0.0.1", ArrivedAs::Ipv4).to_string() == kIpv4LoopbackPeerText);
}

TEST_CASE("to_string_brackets_an_ipv6_peer_so_the_host_and_the_port_stay_separable") {
    CHECK(peer_from_text("fd00::1", ArrivedAs::Ipv6).to_string() == kIpv6PeerText);
    CHECK(peer_from_text("::1", ArrivedAs::Ipv6).to_string() == kIpv6LoopbackPeerText);
}

TEST_CASE("to_string_renders_an_ipv4_mapped_peer_in_dotted_quad_form_even_without_the_v4_flag") {
    const PeerAddress mapped = peer_from_text("::ffff:10.0.1.5", ArrivedAs::Ipv6);
    CHECK_FALSE(mapped.is_v4);
    CHECK(mapped.to_string() == kIpv4PeerText);
}

TEST_CASE("to_string_answers_unix_for_a_unix_domain_peer_and_appends_nothing_to_it") {
    CHECK(unix_peer().to_string() == kUnixPeerText);
}

TEST_CASE("is_loopback_accepts_the_whole_ipv4_loopback_block_however_the_peer_arrived") {
    CHECK(peer_from_text("127.0.0.1", ArrivedAs::Ipv4).is_loopback());
    CHECK(peer_from_text("::ffff:127.0.0.1", ArrivedAs::Ipv4).is_loopback());
    CHECK(peer_from_text("::ffff:127.0.0.1", ArrivedAs::Ipv6).is_loopback());
    CHECK(peer_from_text("127.0.0.2", ArrivedAs::Ipv4).is_loopback());
    CHECK(peer_from_text("127.255.255.255", ArrivedAs::Ipv4).is_loopback());
}

TEST_CASE("is_loopback_accepts_the_ipv6_loopback_address_and_no_other_ipv6_address") {
    CHECK(peer_from_text("::1", ArrivedAs::Ipv6).is_loopback());
    CHECK_FALSE(peer_from_text("::2", ArrivedAs::Ipv6).is_loopback());
    CHECK_FALSE(peer_from_text("::", ArrivedAs::Ipv6).is_loopback());
    CHECK_FALSE(peer_from_text("100::1", ArrivedAs::Ipv6).is_loopback());
}

TEST_CASE("is_loopback_rejects_a_routable_peer_of_either_family") {
    CHECK_FALSE(peer_from_text("10.0.1.5", ArrivedAs::Ipv4).is_loopback());
    CHECK_FALSE(peer_from_text("203.0.113.7", ArrivedAs::Ipv4).is_loopback());
    CHECK_FALSE(peer_from_text("126.255.255.255", ArrivedAs::Ipv4).is_loopback());
    CHECK_FALSE(peer_from_text("128.0.0.1", ArrivedAs::Ipv4).is_loopback());
    CHECK_FALSE(peer_from_text("fd00::1", ArrivedAs::Ipv6).is_loopback());
    CHECK_FALSE(peer_from_text("::ffff:10.0.1.5", ArrivedAs::Ipv6).is_loopback());
}

TEST_CASE("is_loopback_reports_a_unix_domain_peer_as_local") {
    CHECK(unix_peer().is_loopback());
}

TEST_CASE("an_entry_carrying_an_embedded_nul_is_refused_rather_than_silently_truncated") {
    using namespace std::string_view_literals;
    CHECK(rejection_of("10.0.0.1\0junk"sv) == CidrError::BadAddress);
    CHECK(rejection_of("10.0.0.1\0/8"sv) == CidrError::BadAddress);
    CHECK(rejection_of("fd00::1\0/64"sv) == CidrError::BadAddress);
    CHECK(rejection_of("\0 10.0.0.1/8"sv) == CidrError::BadAddress);
}

} // TEST_SUITE("cidr")

} // namespace erikslund::http
