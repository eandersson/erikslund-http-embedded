
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/tls.hpp"

using namespace erikslund::http;
using namespace std::string_view_literals;

namespace {

constexpr std::string_view kCommonNameType = "CN";
constexpr std::string_view kOrganizationalUnitType = "OU";
constexpr std::string_view kDomainComponentType = "DC";

constexpr std::string_view kAdminName = "admin";
constexpr std::string_view kGuestName = "guest";
constexpr std::string_view kOperationsUnit = "operations";

constexpr std::string_view kCraftedIdentity = R"(CN=guest\,CN\3Dadmin)";
constexpr std::string_view kCraftedCommonName = "guest,CN=admin";
constexpr std::string_view kComposedIdentity = "CN=admin,CN=guest";

constexpr std::string_view kSearchedRelativeName = "CN=admin";

} // namespace

TEST_CASE("an_empty_identity_matches_no_attribute_a_caller_can_ask_for") {
    CHECK_FALSE(client_identity_has_attribute(""sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute(""sv, kCommonNameType, ""sv));
    CHECK_FALSE(client_identity_has_attribute(""sv, ""sv, ""sv));
}

TEST_CASE("a_string_that_cannot_be_read_as_a_distinguished_name_matches_nothing") {
    CHECK_FALSE(client_identity_has_attribute("CN"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("admin"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute(",CN=admin"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("+CN=admin"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute(",,,"sv, kCommonNameType, kAdminName));

    CHECK_FALSE(client_identity_has_attribute("CN=admin,OU"sv, kOrganizationalUnitType,
                                              kOperationsUnit));
    CHECK(client_identity_has_attribute("CN=admin,OU"sv, kCommonNameType, kAdminName));
}

TEST_CASE("a_value_that_spells_a_relative_name_is_never_read_as_one") {
    CHECK_FALSE(client_identity_has_attribute(kCraftedIdentity, kCommonNameType, kAdminName));
    CHECK(client_identity_has_attribute(kComposedIdentity, kCommonNameType, kAdminName));
    CHECK(client_identity_has_attribute(kComposedIdentity, kCommonNameType, kGuestName));

    CHECK(client_identity_has_attribute(kCraftedIdentity, kCommonNameType, kCraftedCommonName));

    CHECK(kComposedIdentity.find(kSearchedRelativeName) != std::string_view::npos);
    CHECK(kCraftedIdentity.find(kSearchedRelativeName) == std::string_view::npos);
}

TEST_CASE("a_value_that_only_begins_or_ends_with_the_expected_one_does_not_match") {
    CHECK_FALSE(client_identity_has_attribute("CN=administrator"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=superadmin"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=adm"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=admin admin"sv, kCommonNameType, kAdminName));
    CHECK(client_identity_has_attribute("CN=admin"sv, kCommonNameType, kAdminName));
}

TEST_CASE("an_escaped_separator_inside_a_value_does_not_end_the_value_it_sits_in") {
    CHECK(client_identity_has_attribute(R"(CN=a\,b)"sv, kCommonNameType, "a,b"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\+b)"sv, kCommonNameType, "a+b"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\3Db)"sv, kCommonNameType, "a=b"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\2Cb)"sv, kCommonNameType, "a,b"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\3db)"sv, kCommonNameType, "a=b"sv));

    CHECK_FALSE(client_identity_has_attribute(R"(CN=a\,b)"sv, kCommonNameType, "b"sv));
    CHECK_FALSE(client_identity_has_attribute(R"(CN=a\,b)"sv, kCommonNameType, "a"sv));
}

TEST_CASE("a_malformed_or_truncated_escape_is_read_as_text_rather_than_past_the_end") {
    CHECK(client_identity_has_attribute(R"(CN=a\)"sv, kCommonNameType, "a\\"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\3)"sv, kCommonNameType, "a3"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\zz)"sv, kCommonNameType, "azz"sv));
    CHECK(client_identity_has_attribute(R"(CN=a\3z)"sv, kCommonNameType, "a3z"sv));
    CHECK(client_identity_has_attribute(R"(CN=\)"sv, kCommonNameType, "\\"sv));
    CHECK(client_identity_has_attribute(R"(CN=admin,OU=a\)"sv, kOrganizationalUnitType, "a\\"sv));
}

TEST_CASE("searches_every_entry_of_a_multi_valued_relative_name_and_every_name_after_it") {
    constexpr std::string_view kMultiValued = "CN=admin+OU=operations,DC=example";
    CHECK(client_identity_has_attribute(kMultiValued, kCommonNameType, kAdminName));
    CHECK(client_identity_has_attribute(kMultiValued, kOrganizationalUnitType, kOperationsUnit));
    CHECK(client_identity_has_attribute(kMultiValued, kDomainComponentType, "example"sv));
    CHECK_FALSE(client_identity_has_attribute(kMultiValued, kCommonNameType, kOperationsUnit));
    CHECK_FALSE(client_identity_has_attribute(kMultiValued, kOrganizationalUnitType, kAdminName));
}

TEST_CASE("matches_the_attribute_type_case_insensitively_and_the_value_byte_for_byte") {
    CHECK(client_identity_has_attribute("CN=admin"sv, "cn"sv, kAdminName));
    CHECK(client_identity_has_attribute("cn=admin"sv, "CN"sv, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=Admin"sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=admin"sv, "OU"sv, kAdminName));
}

TEST_CASE("an_attribute_whose_value_is_empty_is_matched_only_by_an_equally_empty_expectation") {
    CHECK(client_identity_has_attribute("CN="sv, kCommonNameType, ""sv));
    CHECK_FALSE(client_identity_has_attribute("CN="sv, kCommonNameType, kAdminName));
    CHECK_FALSE(client_identity_has_attribute("CN=admin"sv, kCommonNameType, ""sv));
}
