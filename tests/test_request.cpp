
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "erikslund/http/method.hpp"
#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/status.hpp"
#include "internal/request_parser.hpp"

using namespace erikslund::http;

namespace {

constexpr RequestLimits kDefaultLimits{};

constexpr std::string_view kMinimalGet = "GET /status HTTP/1.1\r\nHost: pool.example\r\n\r\n";

constexpr size_t kPostBodyBytes = 11;
constexpr std::string_view kPostHeaderBlock = "POST /submit?name=erik+lund&note=a%2Fb HTTP/1.1\r\n"
                                              "Host: pool.example\r\n"
                                              "Content-Type: text/plain\r\n"
                                              "Content-Length: 11\r\n"
                                              "\r\n";
constexpr std::string_view kPostBody = "hello world";
static_assert(kPostBody.size() == kPostBodyBytes,
              "the Content-Length in kPostHeaderBlock must match kPostBody");

constexpr uint16_t kExamplePeerPort = 51'234;

constexpr std::array<std::string_view, kMaxPathParameters> kPathParameterNames{
    {"first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth"}};

[[nodiscard]] std::expected<ParsedRequest, ParseError> parse_with_default_limits(
    std::string_view bytes) {
    return parse_request(bytes, kDefaultLimits);
}

[[nodiscard]] const char* outcome_name(const std::expected<ParsedRequest, ParseError>& result) {
    if (result.has_value())
        return "<parsed>";
    switch (result.error()) {
    case ParseError::NeedsMoreData:
        return "NeedsMoreData";
    case ParseError::MalformedRequestLine:
        return "MalformedRequestLine";
    case ParseError::UnsupportedVersion:
        return "UnsupportedVersion";
    case ParseError::UnknownMethod:
        return "UnknownMethod";
    case ParseError::TooManyHeaders:
        return "TooManyHeaders";
    case ParseError::MalformedHeader:
        return "MalformedHeader";
    case ParseError::MissingHost:
        return "MissingHost";
    case ParseError::MalformedHost:
        return "MalformedHost";
    case ParseError::TargetTooLong:
        return "TargetTooLong";
    case ParseError::BadPercentEncoding:
        return "BadPercentEncoding";
    case ParseError::UnsupportedTransferEncoding:
        return "UnsupportedTransferEncoding";
    case ParseError::BodyTooLarge:
        return "BodyTooLarge";
    }
    return "<a ParseError outside the enumeration>";
}

[[nodiscard]] std::string get_request(std::string_view version, std::string_view extra_fields) {
    std::string request = "GET /status ";
    request += version;
    request += "\r\nHost: pool.example\r\n";
    request += extra_fields;
    request += "\r\n";
    return request;
}

struct ConnectionCase {
    const char* description;
    std::string_view version;
    std::string_view fields;
    bool expected_keep_alive;
};

constexpr ConnectionCase kConnectionCases[] = {
    {"HTTP/1.1 is persistent by default", "HTTP/1.1", "", true},
    {"HTTP/1.1 told to close", "HTTP/1.1", "Connection: close\r\n", false},
    {"HTTP/1.1 told to Close in another casing", "HTTP/1.1", "Connection: Close\r\n", false},
    {"HTTP/1.0 closes by default", "HTTP/1.0", "", false},
    {"HTTP/1.0 asking to persist", "HTTP/1.0", "Connection: keep-alive\r\n", true},
    {"HTTP/1.0 asking to persist inside a list", "HTTP/1.0", "Connection: keep-alive, foo\r\n",
     true},
    {"HTTP/1.0 asking to persist in another casing", "HTTP/1.0", "Connection: Keep-Alive\r\n",
     true},
    {"close wins over a contradictory keep-alive", "HTTP/1.1",
     "Connection: keep-alive, close\r\n", false},
    {"a list spread over two fields", "HTTP/1.0", "Connection: foo\r\nConnection: keep-alive\r\n",
     true},
    {"an option this server does not know", "HTTP/1.1", "Connection: te\r\n", true},
};

struct EncodingCase {
    const char* description;
    std::string_view fields;
    bool expected_gzip;
};

constexpr EncodingCase kEncodingCases[] = {
    {"no Accept-Encoding at all", "", false},
    {"a bare gzip", "Accept-Encoding: gzip\r\n", true},
    {"gzip inside a list", "Accept-Encoding: br, gzip, deflate\r\n", true},
    {"the legacy x-gzip spelling", "Accept-Encoding: x-gzip\r\n", true},
    {"an uppercase GZIP", "Accept-Encoding: GZIP\r\n", true},
    {"a bare wildcard", "Accept-Encoding: *\r\n", true},
    {"gzip refused with q=0", "Accept-Encoding: gzip;q=0\r\n", false},
    {"gzip refused with a longer zero weight", "Accept-Encoding: gzip;q=0.000\r\n", false},
    {"gzip kept at q=0.5", "Accept-Encoding: gzip;q=0.5\r\n", true},
    {"a wildcard refused with q=0", "Accept-Encoding: identity, *;q=0\r\n", false},
    {"a coding this server does not offer", "Accept-Encoding: deflate\r\n", false},
    {"an empty field value", "Accept-Encoding:\r\n", false},
    {"a named gzip outranks a permissive wildcard", "Accept-Encoding: gzip;q=0, *\r\n", false},
};

} // namespace

TEST_CASE("parses_a_well_formed_get_request_into_its_method_target_and_path") {
    const auto result = parse_with_default_limits(kMinimalGet);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& request = result->request;
    CHECK(request.method() == Method::Get);
    CHECK(request.raw_target() == "/status");
    CHECK(request.path() == "/status");
    CHECK(request.query().empty());
    CHECK(request.body().empty());
    CHECK(request.headers().size() == 1u);
    REQUIRE(request.header("Host").has_value());
    CHECK(*request.header("Host") == "pool.example");
    CHECK(result->body_offset == kMinimalGet.size());
    CHECK(result->consumed_bytes == kMinimalGet.size());
}

TEST_CASE("parses_a_head_request_and_reports_an_empty_body") {
    constexpr std::string_view kHead = "HEAD /status HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    const auto result = parse_with_default_limits(kHead);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.method() == Method::Head);
    CHECK(result->request.path() == "/status");
    CHECK(result->request.body().empty());
    CHECK(result->consumed_bytes == kHead.size());
}

TEST_CASE("parses_a_post_request_and_hands_out_the_declared_body") {
    std::string request(kPostHeaderBlock);
    request += kPostBody;
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    CHECK(parsed.method() == Method::Post);
    CHECK(parsed.path() == "/submit");
    CHECK(parsed.query() == "name=erik+lund&note=a%2Fb");
    CHECK(parsed.body() == kPostBody);
    CHECK(parsed.body().size() == kPostBodyBytes);
    REQUIRE(parsed.header("content-type").has_value());
    CHECK(*parsed.header("content-type") == "text/plain");
}

TEST_CASE("an_incremental_parser_keeps_one_validated_head_across_buffer_growth") {
    internal::RequestParser parser;
    std::string bytes(kPostHeaderBlock);
    bytes += kPostBody.substr(0, 3);

    const auto incomplete = parser.parse(bytes, kDefaultLimits);
    REQUIRE_FALSE(incomplete.has_value());
    CHECK(incomplete.error() == ParseError::NeedsMoreData);
    REQUIRE(parser.waiting_for_body());

    const char* const original_buffer = bytes.data();
    bytes.reserve(bytes.capacity() + kDefaultMaxBodyBytes);
    REQUIRE(bytes.data() != original_buffer);
    bytes += kPostBody.substr(3);

    const auto completed = parser.parse(bytes, kDefaultLimits);
    REQUIRE_MESSAGE(completed.has_value(), "unexpected ", outcome_name(completed));
    CHECK_FALSE(parser.waiting_for_body());
    CHECK(completed->request.raw_target() == "/submit?name=erik+lund&note=a%2Fb");
    CHECK(completed->request.path() == "/submit");
    CHECK(completed->request.query() == "name=erik+lund&note=a%2Fb");
    CHECK(completed->request.authority() == "pool.example");
    CHECK(completed->request.body() == kPostBody);
    REQUIRE(completed->request.header("content-type").has_value());
    CHECK(*completed->request.header("content-type") == "text/plain");
    REQUIRE(completed->request.query_param("note").has_value());
    CHECK(*completed->request.query_param("note") == "a/b");
}

TEST_CASE("reports_a_body_offset_that_points_at_the_first_body_byte_of_the_callers_buffer") {
    std::string request(kPostHeaderBlock);
    request += kPostBody;
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->body_offset == kPostHeaderBlock.size());
    CHECK(result->consumed_bytes == request.size());
    CHECK(result->request.body().data() == request.data() + result->body_offset);
}

TEST_CASE("consumes_exactly_one_request_so_a_pipelined_second_request_starts_at_the_right_byte") {
    const std::string first = "GET /alpha HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    const std::string second =
        "POST /beta HTTP/1.1\r\nHost: pool.example\r\nContent-Length: 3\r\n\r\nxyz";
    const std::string stream = first + second;

    const auto leading = parse_with_default_limits(stream);
    REQUIRE_MESSAGE(leading.has_value(), "unexpected ", outcome_name(leading));
    CHECK(leading->request.path() == "/alpha");
    CHECK(leading->consumed_bytes == first.size());

    const auto trailing =
        parse_with_default_limits(std::string_view(stream).substr(leading->consumed_bytes));
    REQUIRE_MESSAGE(trailing.has_value(), "unexpected ", outcome_name(trailing));
    CHECK(trailing->request.path() == "/beta");
    CHECK(trailing->request.body() == "xyz");
    CHECK(trailing->consumed_bytes == second.size());
    CHECK(leading->consumed_bytes + trailing->consumed_bytes == stream.size());
}

TEST_CASE("returns_incomplete_at_every_truncation_point_of_a_valid_get") {
    size_t wrong_outcome_count = 0;
    size_t first_wrong_length = 0;
    const char* first_wrong_outcome = "";
    for (size_t length = 0; length < kMinimalGet.size(); ++length) {
        const auto result = parse_with_default_limits(kMinimalGet.substr(0, length));
        if (!result.has_value() && result.error() == ParseError::NeedsMoreData)
            continue;
        if (wrong_outcome_count++ == 0) {
            first_wrong_length = length;
            first_wrong_outcome = outcome_name(result);
        }
    }
    CHECK_MESSAGE(wrong_outcome_count == 0u, "a prefix of ", first_wrong_length, " bytes returned ",
                  first_wrong_outcome, " instead of NeedsMoreData");
    CHECK(parse_with_default_limits(kMinimalGet).has_value());
}

TEST_CASE("returns_incomplete_at_every_truncation_point_of_a_valid_post_with_a_body") {
    std::string request(kPostHeaderBlock);
    request += kPostBody;
    const std::string_view whole = request;

    size_t wrong_outcome_count = 0;
    size_t first_wrong_length = 0;
    const char* first_wrong_outcome = "";
    for (size_t length = 0; length < whole.size(); ++length) {
        const auto result = parse_with_default_limits(whole.substr(0, length));
        if (!result.has_value() && result.error() == ParseError::NeedsMoreData)
            continue;
        if (wrong_outcome_count++ == 0) {
            first_wrong_length = length;
            first_wrong_outcome = outcome_name(result);
        }
    }
    CHECK_MESSAGE(wrong_outcome_count == 0u, "a prefix of ", first_wrong_length, " bytes returned ",
                  first_wrong_outcome, " instead of NeedsMoreData");
    CHECK(parse_with_default_limits(whole).has_value());
}

TEST_CASE("returns_incomplete_at_every_truncation_point_of_a_valid_absolute_form_request") {
    constexpr std::string_view kAbsolute = "GET http://pool.example/status?window=1h HTTP/1.1\r\n"
                                           "Host: pool.example\r\n"
                                           "Accept-Encoding: gzip\r\n\r\n";
    size_t wrong_outcome_count = 0;
    size_t first_wrong_length = 0;
    const char* first_wrong_outcome = "";
    for (size_t length = 0; length < kAbsolute.size(); ++length) {
        const auto result = parse_with_default_limits(kAbsolute.substr(0, length));
        if (!result.has_value() && result.error() == ParseError::NeedsMoreData)
            continue;
        if (wrong_outcome_count++ == 0) {
            first_wrong_length = length;
            first_wrong_outcome = outcome_name(result);
        }
    }
    CHECK_MESSAGE(wrong_outcome_count == 0u, "a prefix of ", first_wrong_length, " bytes returned ",
                  first_wrong_outcome, " instead of NeedsMoreData");
    CHECK(parse_with_default_limits(kAbsolute).has_value());
}

TEST_CASE("finds_a_header_whatever_case_the_client_spelled_the_name_in") {
    const std::string request = get_request("HTTP/1.1", "cOnTeNt-TyPe: application/json\r\n");
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    REQUIRE(parsed.header("content-type").has_value());
    CHECK(*parsed.header("content-type") == "application/json");
    REQUIRE(parsed.header("CONTENT-TYPE").has_value());
    CHECK(*parsed.header("CONTENT-TYPE") == "application/json");
    REQUIRE(parsed.header("Content-Type").has_value());
    CHECK(*parsed.header("Content-Type") == "application/json");
    CHECK(parsed.headers().back().name == "cOnTeNt-TyPe");
}

TEST_CASE("answers_nullopt_for_a_header_the_request_did_not_send") {
    const auto result = parse_with_default_limits(kMinimalGet);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK_FALSE(result->request.header("Authorization").has_value());
    CHECK_FALSE(result->request.header("").has_value());
}

TEST_CASE("keeps_the_first_of_two_fields_with_the_same_name") {
    const std::string request = get_request("HTTP/1.1", "X-Trace: one\r\nX-Trace: two\r\n");
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    REQUIRE(result->request.header("x-trace").has_value());
    CHECK(*result->request.header("x-trace") == "one");
    CHECK(result->request.headers().size() == 3u);
}

TEST_CASE("reports_every_header_in_the_order_it_arrived") {
    const std::string request = get_request("HTTP/1.1", "X-One: 1\r\nX-Two: 2\r\nX-Three: 3\r\n");
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const std::span<const HeaderView> headers = result->request.headers();
    REQUIRE(headers.size() == 4u);
    CHECK(headers[0].name == "Host");
    CHECK(headers[1].name == "X-One");
    CHECK(headers[2].name == "X-Two");
    CHECK(headers[3].name == "X-Three");
    CHECK(headers[3].value == "3");
}

TEST_CASE("trims_optional_whitespace_around_a_field_value_but_keeps_the_inner_spacing") {
    const std::string request = get_request("HTTP/1.1", "X-Pad: \t inner value \t\r\n");
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    REQUIRE(result->request.header("x-pad").has_value());
    CHECK(*result->request.header("x-pad") == "inner value");
}

TEST_CASE("accepts_an_http_1_0_request_that_carries_no_header_fields_at_all") {
    constexpr std::string_view kBare = "GET /status HTTP/1.0\r\n\r\n";
    const auto result = parse_with_default_limits(kBare);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.headers().empty());
    CHECK(result->request.path() == "/status");
    CHECK(result->consumed_bytes == kBare.size());
}

TEST_CASE("tolerates_one_empty_line_before_the_request_line") {
    std::string request = "\r\n";
    request += kMinimalGet;
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.path() == "/status");
    CHECK(result->consumed_bytes == request.size());
    CHECK(result->body_offset == request.size());
}

TEST_CASE("exposes_the_query_string_undecoded_and_without_the_question_mark") {
    const std::string request = "GET /status?window=1h&format=json HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.path() == "/status");
    CHECK(result->request.query() == "window=1h&format=json");
    CHECK(result->request.raw_target() == "/status?window=1h&format=json");
}

TEST_CASE("percent_decodes_a_query_parameter_and_reads_plus_as_a_space") {
    const std::string request =
        "GET /status?name=erik+lund&note=a%2Fb&hex=%41%42 HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    REQUIRE(parsed.query_param("name").has_value());
    CHECK(*parsed.query_param("name") == "erik lund");
    REQUIRE(parsed.query_param("note").has_value());
    CHECK(*parsed.query_param("note") == "a/b");
    REQUIRE(parsed.query_param("hex").has_value());
    CHECK(*parsed.query_param("hex") == "AB");
}

TEST_CASE("distinguishes_a_query_key_with_no_value_from_a_key_that_is_absent") {
    const std::string request = "GET /status?flag&empty= HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    REQUIRE(parsed.query_param("flag").has_value());
    CHECK(parsed.query_param("flag")->empty());
    REQUIRE(parsed.query_param("empty").has_value());
    CHECK(parsed.query_param("empty")->empty());
    CHECK_FALSE(parsed.query_param("missing").has_value());
}

TEST_CASE("treats_query_keys_as_case_sensitive_unlike_header_names") {
    const std::string request = "GET /status?Limit=1 HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    REQUIRE(result->request.query_param("Limit").has_value());
    CHECK(*result->request.query_param("Limit") == "1");
    CHECK_FALSE(result->request.query_param("limit").has_value());
}

TEST_CASE("skips_an_empty_query_pair_and_a_pair_with_no_name") {
    const std::string request = "GET /status?&=orphan&keep=1& HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    REQUIRE(result->request.query_param("keep").has_value());
    CHECK(*result->request.query_param("keep") == "1");
    CHECK_FALSE(result->request.query_param("").has_value());
}

TEST_CASE("refuses_duplicate_slashes_in_the_path") {
    const std::string request = "GET /a//b///c HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ParseError::MalformedRequestLine);
}

TEST_CASE("refuses_a_leading_double_separator") {
    const std::string request = "GET //pool.example/status HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ParseError::MalformedRequestLine);
}

TEST_CASE("refuses_dot_segments_instead_of_silently_normalizing_them") {
    const std::string requests[] = {
        "GET /./status HTTP/1.1\r\nHost: h\r\n\r\n",
        "GET /a/./b HTTP/1.1\r\nHost: h\r\n\r\n",
        "GET /status/. HTTP/1.1\r\nHost: h\r\n\r\n",
        "GET /././ HTTP/1.1\r\nHost: h\r\n\r\n",
    };
    for (const std::string& request : requests) {
        const auto result = parse_with_default_limits(request);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ParseError::MalformedRequestLine);
    }
}

TEST_CASE("keeps_a_trailing_separator_that_the_client_actually_sent") {
    const std::string request = "GET /status/ HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.path() == "/status/");
}

TEST_CASE("keeps_a_dot_that_is_only_part_of_a_segment") {
    const std::string request = "GET /a/..b/.c/d. HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.path() == "/a/..b/.c/d.");
}

TEST_CASE("hands_out_a_canonical_path_without_touching_the_decode_arena") {
    const std::string request(kMinimalGet);
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const std::string_view path = result->request.path();
    CHECK(path == "/status");
    CHECK(path.data() >= request.data());
    CHECK(path.data() + path.size() <= request.data() + request.size());
}

TEST_CASE("decodes_query_parameters_without_copying_the_canonical_path") {
    const std::string request =
        "GET /a/b/c/AB?x=%41&y=b+c&z=%2F HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    CHECK(parsed.path() == "/a/b/c/AB");
    CHECK(parsed.query() == "x=%41&y=b+c&z=%2F");
    REQUIRE(parsed.query_param("x").has_value());
    CHECK(*parsed.query_param("x") == "A");
    REQUIRE(parsed.query_param("y").has_value());
    CHECK(*parsed.query_param("y") == "b c");
    REQUIRE(parsed.query_param("z").has_value());
    CHECK(*parsed.query_param("z") == "/");
}

TEST_CASE("refuses_percent_encoding_in_a_path_segment") {
    const std::string request = "GET /a%2Db%20c HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ParseError::MalformedRequestLine);
}

TEST_CASE("keeps_a_plus_in_a_path_as_an_ordinary_byte") {
    const std::string with_plus = "GET /a+b HTTP/1.1\r\nHost: h\r\n\r\n";
    const auto plus_result = parse_with_default_limits(with_plus);
    REQUIRE_MESSAGE(plus_result.has_value(), "unexpected ", outcome_name(plus_result));
    CHECK(plus_result->request.path() == "/a+b");
}

TEST_CASE("accepts_an_absolute_form_target_by_stripping_the_scheme_and_authority") {
    const std::string request = "GET http://pool.example:8080/status?window=1h HTTP/1.1\r\n"
                                "Host: pool.example:8080\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    const Request& parsed = result->request;
    CHECK(parsed.path() == "/status");
    CHECK(parsed.query() == "window=1h");
    CHECK(parsed.raw_target() == "http://pool.example:8080/status?window=1h");
    CHECK(parsed.authority() == "pool.example:8080");

    const std::string secure =
        "GET HTTPS://pool.example/status HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    const auto secure_result = parse_with_default_limits(secure);
    REQUIRE_MESSAGE(secure_result.has_value(), "unexpected ", outcome_name(secure_result));
    CHECK(secure_result->request.path() == "/status");
}

TEST_CASE("synthesises_a_root_path_for_an_absolute_form_target_that_carries_none") {
    const std::string request = "GET http://pool.example HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    const auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.path() == "/");
    CHECK(result->request.query().empty());

    const std::string query_only =
        "GET http://pool.example?x=1 HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    const auto query_result = parse_with_default_limits(query_only);
    REQUIRE_MESSAGE(query_result.has_value(), "unexpected ", outcome_name(query_result));
    CHECK(query_result->request.path() == "/");
    REQUIRE(query_result->request.query_param("x").has_value());
    CHECK(*query_result->request.query_param("x") == "1");
}

TEST_CASE("exposes_the_authority_the_request_is_addressed_to_rather_than_only_the_raw_field") {
    const std::string from_field = "GET /status HTTP/1.1\r\nHost: pool.example:8080\r\n\r\n";
    const auto field_result = parse_with_default_limits(from_field);
    REQUIRE_MESSAGE(field_result.has_value(), "unexpected ", outcome_name(field_result));
    CHECK(field_result->request.authority() == "pool.example:8080");

    const std::string from_target =
        "GET http://pool.example:8080/status HTTP/1.1\r\nHost: pool.example:8080\r\n\r\n";
    const auto target_result = parse_with_default_limits(from_target);
    REQUIRE_MESSAGE(target_result.has_value(), "unexpected ", outcome_name(target_result));
    CHECK(target_result->request.authority() == "pool.example:8080");
    CHECK(target_result->request.path() == "/status");

    const std::string legacy = "GET /status HTTP/1.0\r\n\r\n";
    const auto legacy_result = parse_with_default_limits(legacy);
    REQUIRE_MESSAGE(legacy_result.has_value(), "unexpected ", outcome_name(legacy_result));
    CHECK(legacy_result->request.authority().empty());
}

TEST_CASE("keeps_a_http_1_1_connection_alive_unless_the_request_says_otherwise") {
    for (const ConnectionCase& scenario : kConnectionCases) {
        const std::string request = get_request(scenario.version, scenario.fields);
        const auto result = parse_with_default_limits(request);
        REQUIRE_MESSAGE(result.has_value(), scenario.description, " -> ", outcome_name(result));
        CHECK_MESSAGE(result->request.keep_alive() == scenario.expected_keep_alive,
                      scenario.description);
    }
}

TEST_CASE("reads_accept_encoding_before_it_offers_gzip") {
    for (const EncodingCase& scenario : kEncodingCases) {
        const std::string request = get_request("HTTP/1.1", scenario.fields);
        const auto result = parse_with_default_limits(request);
        REQUIRE_MESSAGE(result.has_value(), scenario.description, " -> ", outcome_name(result));
        CHECK_MESSAGE(result->request.wants_gzip() == scenario.expected_gzip,
                      scenario.description);
    }
}

TEST_CASE("stamps_a_steady_clock_instant_the_caller_can_measure_a_deadline_from") {
    const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
    const auto result = parse_with_default_limits(kMinimalGet);
    const std::chrono::steady_clock::time_point after = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    CHECK(result->request.received_at() >= before);
    CHECK(result->request.received_at() <= after);
}

TEST_CASE("a_moved_request_keeps_its_decoded_views_and_leaves_the_source_empty") {
    const std::string request = "GET /a/b/c?x=%41 HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));

    Request moved = std::move(result->request);
    CHECK(moved.method() == Method::Get);
    CHECK(moved.path() == "/a/b/c");
    CHECK(moved.authority() == "pool.example");
    REQUIRE(moved.query_param("x").has_value());
    CHECK(*moved.query_param("x") == "A");
    REQUIRE(moved.header("host").has_value());
    CHECK(*moved.header("host") == "pool.example");

    CHECK(result->request.method() == Method::None);
    CHECK(result->request.path().empty());
    CHECK(result->request.authority().empty());
    CHECK(result->request.raw_target().empty());
    CHECK(result->request.headers().empty());
    CHECK_FALSE(result->request.query_param("x").has_value());
    CHECK_FALSE(result->request.keep_alive());
}

TEST_CASE("reset_returns_a_request_to_the_state_it_had_after_construction") {
    const std::string request = "GET /a/b?x=1 HTTP/1.1\r\nHost: pool.example\r\n\r\n";
    auto result = parse_with_default_limits(request);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    Request& parsed = result->request;
    parsed.set_secure(true);
    parsed.set_client_certificate_subject("CN=operator");
    parsed.bind_path_parameter("window", "1h");

    parsed.reset();
    CHECK(parsed.method() == Method::None);
    CHECK(parsed.path().empty());
    CHECK(parsed.query().empty());
    CHECK(parsed.authority().empty());
    CHECK(parsed.body().empty());
    CHECK(parsed.headers().empty());
    CHECK_FALSE(parsed.query_param("x").has_value());
    CHECK(parsed.param("window").empty());
    CHECK_FALSE(parsed.keep_alive());
    CHECK_FALSE(parsed.wants_gzip());
    CHECK_FALSE(parsed.is_secure());
    CHECK_FALSE(parsed.client_certificate_subject().has_value());
    CHECK(parsed.peer().port == 0u);
}

TEST_CASE("a_default_constructed_request_reports_nothing") {
    const Request request;
    CHECK(request.method() == Method::None);
    CHECK(request.raw_target().empty());
    CHECK(request.path().empty());
    CHECK(request.query().empty());
    CHECK(request.authority().empty());
    CHECK(request.body().empty());
    CHECK(request.headers().empty());
    CHECK_FALSE(request.header("Host").has_value());
    CHECK_FALSE(request.query_param("x").has_value());
    CHECK(request.param("window").empty());
    CHECK_FALSE(request.keep_alive());
    CHECK_FALSE(request.wants_gzip());
    CHECK_FALSE(request.is_secure());
    CHECK_FALSE(request.client_certificate_subject().has_value());
}

TEST_CASE("carries_the_transport_context_the_parser_cannot_see") {
    auto result = parse_with_default_limits(kMinimalGet);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    Request& parsed = result->request;
    CHECK_FALSE(parsed.is_secure());
    CHECK_FALSE(parsed.client_certificate_subject().has_value());

    PeerAddress peer;
    peer.is_v4 = true;
    peer.port = kExamplePeerPort;
    parsed.set_peer(peer);
    parsed.set_secure(true);
    parsed.set_client_certificate_subject("CN=operator");

    CHECK(parsed.peer().port == kExamplePeerPort);
    CHECK(parsed.peer().is_v4);
    CHECK(parsed.is_secure());
    REQUIRE(parsed.client_certificate_subject().has_value());
    CHECK(*parsed.client_certificate_subject() == "CN=operator");

    parsed.set_client_certificate_subject("");
    REQUIRE(parsed.client_certificate_subject().has_value());
    CHECK(parsed.client_certificate_subject()->empty());
}

TEST_CASE("binds_path_parameters_and_answers_an_unbound_name_with_an_empty_view") {
    auto result = parse_with_default_limits(kMinimalGet);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    Request& parsed = result->request;
    CHECK(parsed.param("window").empty());

    for (const std::string_view name : kPathParameterNames)
        parsed.bind_path_parameter(name, "bound");
    for (const std::string_view name : kPathParameterNames)
        CHECK(parsed.param(name) == "bound");

    parsed.bind_path_parameter("overflowing", "dropped");
    CHECK(parsed.param("overflowing").empty());

    parsed.clear_path_parameters();
    CHECK(parsed.param(kPathParameterNames.front()).empty());
}

TEST_CASE("maps_every_terminal_parse_error_to_the_status_the_connection_answers_with") {
    CHECK(status_for(ParseError::TooManyHeaders) == Status::RequestHeaderFieldsTooLarge);
    CHECK(status_for(ParseError::TargetTooLong) == Status::UriTooLong);
    CHECK(status_for(ParseError::BodyTooLarge) == Status::ContentTooLarge);
    CHECK(status_for(ParseError::UnsupportedTransferEncoding) == Status::NotImplemented);
    CHECK(status_for(ParseError::UnsupportedVersion) == Status::MisdirectedRequest);
    CHECK(status_for(ParseError::MalformedRequestLine) == Status::BadRequest);
    CHECK(status_for(ParseError::UnknownMethod) == Status::BadRequest);
    CHECK(status_for(ParseError::MalformedHeader) == Status::BadRequest);
    CHECK(status_for(ParseError::BadPercentEncoding) == Status::BadRequest);
    CHECK(status_for(ParseError::NeedsMoreData) == Status::BadRequest);
}
