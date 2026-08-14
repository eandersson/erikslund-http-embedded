
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"

namespace erikslund::http {
namespace {

constexpr RequestLimits kDefaultLimits{};

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kBlockTerminator = "\r\n\r\n";
constexpr char kNulByte = '\0';

constexpr size_t kSeedBodyBytes = 11;
constexpr std::string_view kSeedHeaderBlock = "POST /a/b/c?x=%41&y=b+c HTTP/1.1\r\n"
                                              "Host: pool.example\r\n"
                                              "Content-Type: text/plain\r\n"
                                              "Content-Length: 11\r\n"
                                              "\r\n";
constexpr std::string_view kSeedBody = "hello world";
static_assert(kSeedBody.size() == kSeedBodyBytes,
              "the Content-Length in kSeedHeaderBlock must match kSeedBody");

constexpr std::string_view kSeedGet = "GET /status HTTP/1.1\r\nHost: pool.example\r\n\r\n";

constexpr std::string_view kVersionSuffix = " HTTP/1.1\r\nHost: pool.example\r\n\r\n";

constexpr std::string_view kHostFieldLine = "Host: pool.example\r\n";
constexpr std::string_view kSeedAuthority = "pool.example";

constexpr std::string_view kBytesNoAuthorityMayCarry = "@/\\?#\"'<>%";
constexpr unsigned char kSpaceByte = 0x20;
constexpr unsigned char kDeleteByte = 0x7F;

constexpr size_t kGenerousBlockBytes = 8'000'000;

constexpr size_t kTinyHeaderFieldCount = 10'000;
constexpr size_t kMegabyteHeaderValueBytes = 1'048'576;
constexpr size_t kHundredKilobyteTargetBytes = 102'400;

constexpr size_t kOneTooManyFields = kMaxParsedHeaders + 1;

constexpr size_t kPaddingTargetBytes = 100;
constexpr size_t kPaddingValueBytes = 200;
constexpr size_t kSmallHeaderCount = 4;
constexpr size_t kSmallBodyBytes = 4;
constexpr size_t kUnhonourableHeaderCount = kMaxParsedHeaders * 4;

constexpr size_t kExtraPrologueCount = 8;

constexpr uint64_t kFuzzSeed = 20'240'917;
constexpr size_t kFuzzRounds = 4'000;
constexpr size_t kMaxMutationsPerRound = 4;

constexpr unsigned kXorShiftA = 12;
constexpr unsigned kXorShiftB = 25;
constexpr unsigned kXorShiftC = 27;
constexpr uint64_t kXorShiftMultiplier = 2'685'821'657'736'338'717ULL;
constexpr unsigned kBitsPerByte = 8;

[[nodiscard]] std::expected<ParsedRequest, ParseError> parse(std::string_view bytes) {
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

struct RejectionCase {
    const char* description;
    std::string bytes;
    ParseError expected;
};

void check_each_rejection(std::span<const RejectionCase> cases) {
    for (const RejectionCase& scenario : cases) {
        const auto result = parse(scenario.bytes);
        REQUIRE_MESSAGE(!result.has_value(), scenario.description, " was accepted");
        CHECK_MESSAGE(result.error() == scenario.expected, scenario.description, " -> ",
                      outcome_name(result));
    }
}

void check_all_rejected_with(std::span<const std::string> requests, ParseError expected) {
    for (const std::string& request : requests) {
        const auto result = parse(request);
        REQUIRE_MESSAGE(!result.has_value(), "accepted: ", request);
        CHECK_MESSAGE(result.error() == expected, request, " -> ", outcome_name(result));
    }
}

[[nodiscard]] std::string get_with_target(std::string_view target) {
    std::string request = "GET ";
    request += target;
    request += kVersionSuffix;
    return request;
}

[[nodiscard]] std::string get_with_field(std::string_view field_line) {
    std::string request = "GET /status HTTP/1.1\r\n";
    request += field_line;
    request += kCrlf;
    request += kHostFieldLine;
    request += kCrlf;
    return request;
}

[[nodiscard]] std::string get_with_host(std::string_view host_value) {
    std::string request = "GET /status HTTP/1.1\r\nHost: ";
    request += host_value;
    request += kBlockTerminator;
    return request;
}

[[nodiscard]] std::string post_with_fields(std::string_view field_lines, std::string_view body) {
    std::string request = "POST /submit HTTP/1.1\r\nHost: pool.example\r\n";
    request += field_lines;
    request += kCrlf;
    request += body;
    return request;
}

[[nodiscard]] std::string with_nul_between(std::string_view before, std::string_view after) {
    std::string joined(before);
    joined.push_back(kNulByte);
    joined += after;
    return joined;
}

[[nodiscard]] std::string repeated(size_t count, char character) {
    return std::string(count, character);
}

[[nodiscard]] std::string seed_request() {
    std::string request(kSeedHeaderBlock);
    request += kSeedBody;
    return request;
}

[[nodiscard]] size_t request_line_bytes_of(std::string_view request) {
    const size_t end = request.find(kCrlf);
    return end == std::string_view::npos ? request.size() : end;
}

[[nodiscard]] bool has_parent_segment(std::string_view path) {
    while (!path.empty()) {
        const size_t separator = path.find('/');
        const std::string_view segment =
            separator == std::string_view::npos ? path : path.substr(0, separator);
        if (segment == "..")
            return true;
        if (separator == std::string_view::npos)
            return false;
        path = path.substr(separator + 1);
    }
    return false;
}

[[nodiscard]] bool view_lies_inside(std::string_view part, std::string_view whole) {
    return part.data() >= whole.data() && part.data() + part.size() <= whole.data() + whole.size();
}

[[nodiscard]] bool is_safely_reflectable_authority_byte(char character) {
    const auto value = static_cast<unsigned char>(character);
    if (value <= kSpaceByte || value >= kDeleteByte)
        return false;
    return !kBytesNoAuthorityMayCarry.contains(character);
}

[[nodiscard]] const char* broken_promise_of(const ParsedRequest& parsed, std::string_view bytes) {
    const Request& request = parsed.request;
    if (parsed.consumed_bytes > bytes.size())
        return "consumed_bytes ran past the buffer";
    if (parsed.body_offset > parsed.consumed_bytes)
        return "body_offset sits past consumed_bytes";
    if (request.body().size() != parsed.consumed_bytes - parsed.body_offset)
        return "the body length disagrees with the reported offsets";
    if (!request.body().empty() && !view_lies_inside(request.body(), bytes))
        return "the body views memory outside the buffer";
    if (!view_lies_inside(request.raw_target(), bytes))
        return "the raw target views memory outside the buffer";
    if (request.method() == Method::None)
        return "a parsed request carries no method";
    if (request.path().empty() || request.path().front() != '/')
        return "the path does not begin at the root";
    if (request.path().contains(kNulByte))
        return "the path carries a NUL";
    if (request.path().contains('%'))
        return "the path carries an escape a second decoder would resolve";
    if (has_parent_segment(request.path()))
        return "the path carries a parent segment";
    if (!request.authority().empty() && !view_lies_inside(request.authority(), bytes))
        return "the authority views memory outside the buffer";
    for (const char character : request.authority())
        if (!is_safely_reflectable_authority_byte(character))
            return "the authority carries a byte a handler could not put in a URL unescaped";
    if (request.headers().size() > kMaxParsedHeaders)
        return "the header table overflowed its fixed capacity";
    for (const HeaderView& field : request.headers()) {
        if (field.name.empty())
            return "a field with no name reached the header table";
        if (!view_lies_inside(field.name, bytes) || !view_lies_inside(field.value, bytes))
            return "a header views memory outside the buffer";
    }
    return nullptr;
}

class SeededGenerator {
public:
    explicit SeededGenerator(uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] uint64_t next() noexcept {
        state_ ^= state_ >> kXorShiftA;
        state_ ^= state_ << kXorShiftB;
        state_ ^= state_ >> kXorShiftC;
        return state_ * kXorShiftMultiplier;
    }

    [[nodiscard]] size_t below(size_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<size_t>(next() % bound);
    }

private:
    uint64_t state_;
};

enum class MutationKind : uint8_t { FlipByte, Truncate, DuplicateSpan, InsertStructuralByte };

constexpr size_t kMutationKindCount = 4;

constexpr size_t kMaxDuplicatedSpanBytes = 24;

constexpr auto kStructuralBytes = std::to_array<char>(
    {'\r', '\n', '\0', '\t', '\x7f', ' ', ':', ';', ',', '%', '/', '?', '#', '.'});

void mutate_once(std::string& bytes, SeededGenerator& generator) {
    if (bytes.empty())
        return;
    switch (static_cast<MutationKind>(generator.below(kMutationKindCount))) {
    case MutationKind::FlipByte: {
        const size_t index = generator.below(bytes.size());
        const auto bit = static_cast<unsigned>(generator.below(kBitsPerByte));
        bytes[index] = static_cast<char>(static_cast<unsigned char>(bytes[index]) ^ (1U << bit));
        break;
    }
    case MutationKind::Truncate:
        bytes.resize(generator.below(bytes.size()));
        break;
    case MutationKind::DuplicateSpan: {
        const size_t begin = generator.below(bytes.size());
        const size_t length =
            std::min(bytes.size() - begin, generator.below(kMaxDuplicatedSpanBytes) + 1);
        bytes.insert(begin, bytes.substr(begin, length));
        break;
    }
    case MutationKind::InsertStructuralByte: {
        const size_t index = generator.below(bytes.size() + 1);
        bytes.insert(index, 1, kStructuralBytes[generator.below(kStructuralBytes.size())]);
        break;
    }
    }
}


TEST_CASE("refuses_a_parent_segment_in_every_position_a_target_can_put_one") {
    const std::string cases[] = {
        get_with_target("/.."),
        get_with_target("/../etc/passwd"),
        get_with_target("/a/../b"),
        get_with_target("/a/.."),
        get_with_target("/a/../../b"),
        get_with_target("/a/b/../.."),
        get_with_target("/..//a"),
        get_with_target("//../a"),
        get_with_target("/./../a"),
        get_with_target("/a/./../b"),
        get_with_target("/../"),
        get_with_target("/a/../"),
        get_with_target("/a/..?x=1"),
        get_with_target("http://pool.example/../etc"),
        get_with_target("http://pool.example/a/../b"),
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);

    const std::string unrouted[] = {get_with_target(".."), get_with_target("../etc")};
    check_all_rejected_with(unrouted, ParseError::MalformedRequestLine);
}

TEST_CASE("refuses_every_percent_encoded_path_spelling") {
    const std::string cases[] = {
        get_with_target("/%2e%2e"),     get_with_target("/%2E%2E"),
        get_with_target("/%2e%2e/etc"), get_with_target("/.%2e/etc"),
        get_with_target("/%2e./etc"),   get_with_target("/a/%2e%2e/b"),
        get_with_target("/a/%2E./b"),   get_with_target("/a/%2e%2e"),
        get_with_target("/a/%2e/b"),    get_with_target("/a%2Db%20c"),
        get_with_target("/%41"),        get_with_target("/a%2fb"),
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);
}

TEST_CASE("refuses_an_escape_that_would_decode_into_another_escape") {
    const std::string cases[] = {
        get_with_target("/%252e%252e"),     get_with_target("/%252e%252e%252fetc"),
        get_with_target("/a/%252e%252e/b"), get_with_target("/%2525"),
        get_with_target("/a%25b"),          get_with_target("/%2500"),
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);

    const std::string in_query_wire = get_with_target("/status?note=%252e%252e");
    const auto in_query = parse(in_query_wire);
    REQUIRE_MESSAGE(in_query.has_value(), "unexpected ", outcome_name(in_query));
    REQUIRE(in_query->request.query_param("note").has_value());
    CHECK(*in_query->request.query_param("note") == "%2e%2e");
}

TEST_CASE("refuses_malformed_and_control_percent_encodings") {
    const std::string path_cases[] = {
        get_with_target("/%00"),
        get_with_target("/status%00.txt"),
        get_with_target("/a/%00/b"),
        get_with_target("/%2f"),
        get_with_target("/%2F"),
        get_with_target("/a%2fb"),
        get_with_target("/a%2f%2e%2e%2fb"),
        get_with_target("/a%0db"),
        get_with_target("/a%0ab"),
        get_with_target("/a%7fb"),
        get_with_target("/a%1fb"),
        get_with_target("/a%"),
        get_with_target("/a%2"),
        get_with_target("/a%zz"),
        get_with_target("/a%2g"),
        get_with_target("/a%%41"),
    };
    check_all_rejected_with(path_cases, ParseError::MalformedRequestLine);

    const std::string query_cases[] = {
        get_with_target("/status?x=%00"),
        get_with_target("/status?x=%2"),
        get_with_target("/status?x=%zz"),
    };
    check_all_rejected_with(query_cases, ParseError::BadPercentEncoding);
}

TEST_CASE("refuses_an_encoded_control_byte_in_a_query_component") {
    const std::string cases[] = {
        get_with_target("/echo?v=a%0d%0aX-Injected:%20yes"),
        get_with_target("/echo?v=%0d"),
        get_with_target("/echo?v=%0A"),
        get_with_target("/echo?v=one%0d%0aSet-Cookie:%20admin=1"),
        get_with_target("/echo?%0d%0aX-Injected:%20yes=v"),
        get_with_target("/echo?a%0db=v"),
        get_with_target("/echo?v=%1b%5b2J"),
        get_with_target("/echo?v=%7f"),
        get_with_target("/echo?v=%09"),
        get_with_target("/echo?v=%01"),
        get_with_target("/echo?v=%1f"),
    };
    check_all_rejected_with(cases, ParseError::BadPercentEncoding);
}

TEST_CASE("refuses_backslashes_that_a_proxy_may_treat_as_path_separators") {
    const std::string cases[] = {
        get_with_target("/admin\\settings"),
        get_with_target("/\\admin"),
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);
}

TEST_CASE("still_decodes_every_query_byte_that_is_not_a_control_character") {
    const std::string wire = get_with_target(
        "/echo?slash=a%2fb&percent=%2525&plus=a+b&space=a%20b&utf8=caf%c3%a9&at=%40");
    const auto parsed = parse(wire);
    REQUIRE_MESSAGE(parsed.has_value(), "unexpected ", outcome_name(parsed));

    const Request& request = parsed->request;
    CHECK(*request.query_param("slash") == "a/b");
    CHECK(*request.query_param("percent") == "%25");
    CHECK(*request.query_param("plus") == "a b");
    CHECK(*request.query_param("space") == "a b");
    CHECK(*request.query_param("utf8") == "caf\xc3\xa9");
    CHECK(*request.query_param("at") == "@");
}

TEST_CASE("refuses_a_target_carrying_a_control_character_a_fragment_or_a_form_it_cannot_route") {
    const std::string cases[] = {
        get_with_target("/a\tb"),
        get_with_target("/a\x01\x62"),
        get_with_target("/a\x7f"),
        get_with_target("/status#section"),
        get_with_target("/status?x=1#section"),
        get_with_target("*"),
        get_with_target("pool.example:443"),
        get_with_target("ftp://pool.example/x"),
        get_with_target("a/b"),
        get_with_target("://pool.example/x"),
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);

    const auto high_bytes = parse(get_with_target("/caf\xc3\xa9"));
    CHECK_MESSAGE(high_bytes.has_value(), "unexpected ", outcome_name(high_bytes));
}


TEST_CASE("refuses_whitespace_between_a_field_name_and_its_colon") {
    const std::string cases[] = {
        get_with_field("Host : pool.example"),
        get_with_field("Host\t: pool.example"),
        get_with_field("Host  : pool.example"),
        get_with_field("Content-Length : 5"),
        get_with_field("Transfer-Encoding : chunked"),
    };
    check_all_rejected_with(cases, ParseError::MalformedHeader);
}

TEST_CASE("refuses_an_obs_fold_continuation_line") {
    const std::string cases[] = {
        get_with_field("X-Trace: one\r\n more"),
        get_with_field("X-Trace: one\r\n\tmore"),
        get_with_field("X-Trace: one\r\n  Content-Length: 5"),
        get_with_field(" X-Trace: one"),
        get_with_field("\tX-Trace: one"),
    };
    check_all_rejected_with(cases, ParseError::MalformedHeader);
}

TEST_CASE("refuses_a_field_name_that_is_empty_or_carries_a_byte_outside_the_token_set") {
    const std::string cases[] = {
        get_with_field(": pool.example"),
        get_with_field(":"),
        get_with_field(":: x"),
        get_with_field("Ho st: x"),
        get_with_field("Host\x01: x"),
        get_with_field("Host\x7f: x"),
        get_with_field(with_nul_between("Ho", "st: x")),
        get_with_field("Host(comment): x"),
        get_with_field("Host@example: x"),
        get_with_field("Host,Other: x"),
        get_with_field("Host/Other: x"),
        get_with_field("Host[0]: x"),
        get_with_field("\"Host\": x"),
        get_with_field("Host\\Other: x"),
        get_with_field("Host{}: x"),
        get_with_field("Host<>: x"),
        get_with_field("Host=x: y"),
        get_with_field("Host pool.example"),
        get_with_field("garbage"),
    };
    check_all_rejected_with(cases, ParseError::MalformedHeader);

    const std::string token_specials_wire = get_with_field("X-Odd!#$%&'*+-.^_`|~: kept");
    const auto token_specials = parse(token_specials_wire);
    REQUIRE_MESSAGE(token_specials.has_value(), "unexpected ", outcome_name(token_specials));
    REQUIRE(token_specials->request.header("x-odd!#$%&'*+-.^_`|~").has_value());
    CHECK(*token_specials->request.header("X-ODD!#$%&'*+-.^_`|~") == "kept");
}

TEST_CASE("ends_a_field_name_at_the_first_colon_so_no_name_can_contain_one") {
    const std::string wire = get_with_field("X-Trace:Host: pool.example");
    const auto result = parse(wire);
    REQUIRE_MESSAGE(result.has_value(), "unexpected ", outcome_name(result));
    REQUIRE(result->request.header("x-trace").has_value());
    CHECK(*result->request.header("x-trace") == "Host: pool.example");
    CHECK_FALSE(result->request.header("x-trace:host").has_value());
    CHECK(result->request.headers().size() == 2u);
    CHECK(result->request.authority() == kSeedAuthority);
}

TEST_CASE("refuses_a_control_character_in_a_field_value") {
    const std::string cases[] = {
        get_with_field("X-Trace: a\rb"),
        get_with_field("X-Trace: a\nb"),
        get_with_field("X-Trace: a\r b"),
        get_with_field(with_nul_between("X-Trace: a", "b")),
        get_with_field("X-Trace: a\x01\x62"),
        get_with_field("X-Trace: a\x7f"),
        get_with_field("X-Trace: \x1b[31m"),
    };
    check_all_rejected_with(cases, ParseError::MalformedHeader);

    const std::string obs_text_wire = get_with_field("X-Operator: caf\xc3\xa9");
    const auto obs_text = parse(obs_text_wire);
    REQUIRE_MESSAGE(obs_text.has_value(), "unexpected ", outcome_name(obs_text));
    REQUIRE(obs_text->request.header("x-operator").has_value());
    CHECK(*obs_text->request.header("x-operator") == "caf\xc3\xa9");
}


TEST_CASE("refuses_two_content_length_fields_that_disagree_about_the_body") {
    const auto refused =
        parse(post_with_fields("Content-Length: 5\r\nContent-Length: 6\r\n", "hello!"));
    REQUIRE_MESSAGE(!refused.has_value(), "two lengths were reconciled instead of refused");
    CHECK_MESSAGE(refused.error() == ParseError::MalformedHeader, "-> ", outcome_name(refused));

    const auto listed = parse(post_with_fields("Content-Length: 5, 6\r\n", "hello!"));
    REQUIRE(!listed.has_value());
    CHECK(listed.error() == ParseError::MalformedHeader);

    const std::string agreeing_wire =
        post_with_fields("Content-Length: 5\r\nContent-Length: 5\r\n", "hello");
    const auto accepted = parse(agreeing_wire);
    REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
    CHECK(accepted->request.body() == "hello");
}

TEST_CASE("refuses_a_content_length_that_is_not_a_bare_decimal_number") {
    const std::string cases[] = {
        post_with_fields("Content-Length: -1\r\n", ""),
        post_with_fields("Content-Length: +5\r\n", "hello"),
        post_with_fields("Content-Length: five\r\n", "hello"),
        post_with_fields("Content-Length: 5x\r\n", "hello"),
        post_with_fields("Content-Length: 0x5\r\n", "hello"),
        post_with_fields("Content-Length: 1 1\r\n", "hello"),
        post_with_fields("Content-Length: 5.0\r\n", "hello"),
        post_with_fields("Content-Length: 1_1\r\n", "hello"),
        post_with_fields("Content-Length:\r\n", ""),
        post_with_fields("Content-Length: \r\n", ""),
        post_with_fields("Content-Length: 0011\r\n", "hello world"),
        post_with_fields("Content-Length: 05\r\n", "hello"),
        post_with_fields("Content-Length: 00\r\n", ""),
    };
    check_all_rejected_with(cases, ParseError::MalformedHeader);

    const std::string zero_wire = post_with_fields("Content-Length: 0\r\n", "");
    const auto zero = parse(zero_wire);
    REQUIRE_MESSAGE(zero.has_value(), "unexpected ", outcome_name(zero));
    CHECK(zero->request.body().empty());
    const std::string five_wire = post_with_fields("Content-Length: 5\r\n", "hello");
    const auto five = parse(five_wire);
    REQUIRE_MESSAGE(five.has_value(), "unexpected ", outcome_name(five));
    CHECK(five->request.body() == "hello");
}

TEST_CASE("answers_a_content_length_too_large_to_frame_a_body_with_the_error_about_size") {
    const std::string cases[] = {
        post_with_fields("Content-Length: 18446744073709551616\r\n", ""),
        post_with_fields("Content-Length: 99999999999999999999999999999999\r\n", ""),
        post_with_fields("Content-Length: 18446744073709551615\r\n", ""),
        post_with_fields("Content-Length: 4294967296\r\n", ""),
    };
    check_all_rejected_with(cases, ParseError::BodyTooLarge);
}

TEST_CASE("refuses_every_transfer_encoding_that_would_frame_the_body_a_second_way") {
    const RejectionCase cases[] = {
        {"chunked alone", "POST /submit HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"chunked in another casing",
         "POST /submit HTTP/1.1\r\ntransfer-encoding: CHUNKED\r\n\r\n0\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"chunked behind another coding",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"a coding list spread over two fields",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: identity\r\n"
         "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"an empty coding list nobody can agree the meaning of",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: \r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"a coding list that is only separators",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: ,,\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"chunked declared after a length",
         "POST /submit HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello",
         ParseError::UnsupportedTransferEncoding},
        {"chunked declared before a length",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello",
         ParseError::UnsupportedTransferEncoding},
        {"the obsolete identity coding",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n",
         ParseError::UnsupportedTransferEncoding},
        {"a length alongside the obsolete identity coding",
         "POST /submit HTTP/1.1\r\nTransfer-Encoding: identity\r\nContent-Length: 5\r\n\r\nhello",
         ParseError::UnsupportedTransferEncoding},
    };
    check_each_rejection(cases);
}


TEST_CASE("refuses_an_http_1_1_request_that_names_no_authority_at_all") {
    const RejectionCase cases[] = {
        {"a bare request line and nothing else", "GET /status HTTP/1.1\r\n\r\n",
         ParseError::MissingHost},
        {"fields, but not that one",
         "GET /status HTTP/1.1\r\nAccept: text/plain\r\nX-Trace: 1\r\n\r\n",
         ParseError::MissingHost},
        {"a declared body, still no authority",
         "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", ParseError::MissingHost},
        {"an absolute-form target instead of the field",
         "GET http://pool.example/status HTTP/1.1\r\n\r\n", ParseError::MissingHost},
    };
    check_each_rejection(cases);
}

TEST_CASE("refuses_a_second_host_field_whatever_the_two_of_them_say") {
    const RejectionCase cases[] = {
        {"two that disagree",
         "GET /status HTTP/1.1\r\nHost: front.example\r\nHost: real.example\r\n\r\n",
         ParseError::MalformedHost},
        {"two that agree",
         "GET /status HTTP/1.1\r\nHost: pool.example\r\nHost: pool.example\r\n\r\n",
         ParseError::MalformedHost},
        {"two spelled in different cases",
         "GET /status HTTP/1.1\r\nHost: pool.example\r\nhOsT: pool.example\r\n\r\n",
         ParseError::MalformedHost},
        {"a real one and an empty one",
         "GET /status HTTP/1.1\r\nHost: pool.example\r\nHost: \r\n\r\n",
         ParseError::MalformedHost},
        {"one of them split off an absolute-form target",
         "GET http://pool.example/status HTTP/1.1\r\nHost: pool.example\r\nHost: pool.example\r\n"
         "\r\n",
         ParseError::MalformedHost},
    };
    check_each_rejection(cases);
}

TEST_CASE("refuses_a_host_that_is_not_one_hostname_and_at_most_one_port") {
    const std::string cases[] = {
        get_with_host(""),
        get_with_host("pool example"),
        get_with_host("pool\texample"),
        get_with_host("user@pool.example"),
        get_with_host("@pool.example"),
        get_with_host("pool.example@evil.example"),
        get_with_host("pool.example/admin"),
        get_with_host("pool.example?x=1"),
        get_with_host("pool.example#fragment"),
        get_with_host("pool.example\\admin"),
        get_with_host("http://pool.example"),
        get_with_host("//pool.example"),
        get_with_host("pool.example:"),
        get_with_host("pool.example:0"),
        get_with_host("pool.example:080"),
        get_with_host("pool.example:65536"),
        get_with_host("pool.example:99999"),
        get_with_host("pool.example:8o80"),
        get_with_host("pool.example:80:80"),
        get_with_host("pool.example:-1"),
        get_with_host("pool.example: 80"),
        get_with_host("[::1"),
        get_with_host("::1]"),
        get_with_host("2001:db8::1"),
        get_with_host("[]"),
        get_with_host("[::1]trailing"),
        get_with_host("[::1]:"),
        get_with_host("[::1]:0"),
        get_with_host("[gg::1]"),
        get_with_host("[::::]"),
        get_with_host("[1:2:3]"),
        get_with_host("[1.2.3.4]"),
        get_with_host("[fe80::1%25eth0]"),
        get_with_host(".pool.example"),
        get_with_host("pool..example"),
        get_with_host("pool.example.."),
        get_with_host("pool%2eexample"),
        get_with_host("pool.example,other.example"),
        get_with_host("pool.example'"),
        get_with_host("*"),
        get_with_host("caf\xc3\xa9.example"),
    };
    check_all_rejected_with(cases, ParseError::MalformedHost);

    const RejectionCase control_bytes[] = {
        {"a CR inside the host", "GET /status HTTP/1.1\r\nHost: pool\rexample\r\n\r\n",
         ParseError::MalformedHeader},
        {"a NUL inside the host",
         with_nul_between("GET /status HTTP/1.1\r\nHost: pool", "example\r\n\r\n"),
         ParseError::MalformedHeader},
    };
    check_each_rejection(control_bytes);
}

TEST_CASE("accepts_every_authority_shape_a_real_client_sends_and_hands_it_back_unchanged") {
    const std::string_view hosts[] = {
        "pool.example",   "POOL.EXAMPLE",      "pool.example.",      "localhost",
        "box_1.lan",      "a-b.example",       "xn--bcher-kva.example",
        "pool.example:1", "pool.example:8080", "pool.example:65535", "192.168.1.5",
        "192.168.1.5:8080", "[::1]",           "[::1]:8443",         "[2001:db8::1]",
        "[2001:db8::1]:80", "[::ffff:192.168.1.5]",
    };
    for (const std::string_view host : hosts) {
        const std::string wire = get_with_host(host);
        const auto result = parse(wire);
        REQUIRE_MESSAGE(result.has_value(), host, " -> ", outcome_name(result));
        CHECK_MESSAGE(result->request.authority() == host, "authority() rewrote ", host);
    }
}

TEST_CASE("takes_the_authority_from_an_absolute_form_target_and_refuses_a_host_contradicting_it") {
    const std::string agreeing_wire =
        "GET http://real.example/admin HTTP/1.1\r\nHost: real.example\r\n\r\n";
    const auto agreeing = parse(agreeing_wire);
    REQUIRE_MESSAGE(agreeing.has_value(), "unexpected ", outcome_name(agreeing));
    CHECK(agreeing->request.authority() == "real.example");
    CHECK(agreeing->request.path() == "/admin");

    const std::string spelling_wire =
        "GET http://REAL.example/admin HTTP/1.1\r\nHost: real.EXAMPLE\r\n\r\n";
    const auto spelling = parse(spelling_wire);
    REQUIRE_MESSAGE(spelling.has_value(), "unexpected ", outcome_name(spelling));
    CHECK(spelling->request.authority() == "REAL.example");

    const RejectionCase cases[] = {
        {"a different name entirely",
         "GET http://real.example/admin HTTP/1.1\r\nHost: front.example\r\n\r\n",
         ParseError::MalformedHost},
        {"the same name with a port on one side only",
         "GET http://real.example:8080/admin HTTP/1.1\r\nHost: real.example\r\n\r\n",
         ParseError::MalformedHost},
        {"a subdomain of the name the target gave",
         "GET http://real.example/admin HTTP/1.1\r\nHost: admin.real.example\r\n\r\n",
         ParseError::MalformedHost},
        {"an authority in the target that is not a host",
         "GET http://user@real.example/admin HTTP/1.1\r\nHost: user@real.example\r\n\r\n",
         ParseError::MalformedHost},
    };
    check_each_rejection(cases);
}

TEST_CASE("refuses_an_absolute_form_target_that_names_no_authority_at_all") {
    const std::string cases[] = {
        "GET http:///status HTTP/1.1\r\nHost: pool.example\r\n\r\n",
        "GET http:// HTTP/1.1\r\nHost: pool.example\r\n\r\n",
        "GET http://?x=1 HTTP/1.1\r\nHost: pool.example\r\n\r\n",
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);
}

TEST_CASE("tolerates_a_missing_host_only_on_the_version_that_predates_the_field") {
    const std::string absent_wire = "GET /status HTTP/1.0\r\n\r\n";
    const auto absent = parse(absent_wire);
    REQUIRE_MESSAGE(absent.has_value(), "unexpected ", outcome_name(absent));
    CHECK(absent->request.authority().empty());

    const std::string present_wire = "GET /status HTTP/1.0\r\nHost: pool.example\r\n\r\n";
    const auto present = parse(present_wire);
    REQUIRE_MESSAGE(present.has_value(), "unexpected ", outcome_name(present));
    CHECK(present->request.authority() == kSeedAuthority);

    const std::string legacy_absolute = "GET http://pool.example/status HTTP/1.0\r\n\r\n";
    const auto absolute = parse(legacy_absolute);
    REQUIRE_MESSAGE(absolute.has_value(), "unexpected ", outcome_name(absolute));
    CHECK(absolute->request.authority() == kSeedAuthority);
    CHECK(absolute->request.path() == "/status");

    const RejectionCase cases[] = {
        {"a 1.0 request with a host nothing can read",
         "GET /status HTTP/1.0\r\nHost: pool example\r\n\r\n", ParseError::MalformedHost},
        {"a 1.0 request with two of them",
         "GET /status HTTP/1.0\r\nHost: pool.example\r\nHost: pool.example\r\n\r\n",
         ParseError::MalformedHost},
    };
    check_each_rejection(cases);
}


TEST_CASE("refuses_a_request_line_missing_a_field_or_carrying_one_too_many") {
    const RejectionCase cases[] = {
        {"no method", " /status HTTP/1.1\r\n\r\n", ParseError::MalformedRequestLine},
        {"no target", "GET  HTTP/1.1\r\n\r\n", ParseError::MalformedRequestLine},
        {"no version", "GET /status\r\n\r\n", ParseError::MalformedRequestLine},
        {"a space where the version should be", "GET /status \r\n\r\n",
         ParseError::MalformedRequestLine},
        {"only a method", "GET\r\n\r\n", ParseError::MalformedRequestLine},
        {"a target where the method should be", "/status HTTP/1.1\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"two spaces between method and target", "GET  /status HTTP/1.1\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"two spaces before the version", "GET /status  HTTP/1.1\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a trailing space after the version", "GET /status HTTP/1.1 \r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a fourth field", "GET /status HTTP/1.1 extra\r\n\r\n", ParseError::MalformedRequestLine},
        {"tabs instead of spaces", "GET\t/status\tHTTP/1.1\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a lowercase method", "get /status HTTP/1.1\r\n\r\n", ParseError::UnknownMethod},
        {"a method nobody here serves", "TRACE /status HTTP/1.1\r\n\r\n",
         ParseError::UnknownMethod},
        {"a tunnelling method", "CONNECT pool.example:443 HTTP/1.1\r\n\r\n",
         ParseError::UnknownMethod},
        {"a method of pure punctuation", "@@@ /status HTTP/1.1\r\n\r\n", ParseError::UnknownMethod},
    };
    check_each_rejection(cases);
}

TEST_CASE("refuses_a_request_line_terminated_with_a_bare_lf_instead_of_a_crlf") {
    const RejectionCase cases[] = {
        {"a bare LF closing the request line", "GET /status HTTP/1.1\n\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a bare LF before a CRLF-framed field", "GET /status HTTP/1.1\nHost: pool.example\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a bare CR closing the request line", "GET /status HTTP/1.1\r\r\n\r\n",
         ParseError::MalformedRequestLine},
        {"a second request line folded in behind an LF",
         "GET /status HTTP/1.1\nGET /admin HTTP/1.1\r\n\r\n", ParseError::MalformedRequestLine},
        {"a NUL closing the request line", with_nul_between("GET /status HTTP/1.1", "\r\n\r\n"),
         ParseError::MalformedRequestLine},
    };
    check_each_rejection(cases);

    const auto lf_framed = parse("GET /status HTTP/1.1\nHost: pool.example\n\n");
    REQUIRE(!lf_framed.has_value());
    CHECK_MESSAGE(lf_framed.error() == ParseError::NeedsMoreData, "-> ", outcome_name(lf_framed));
}

TEST_CASE("refuses_a_version_that_is_not_exactly_one_digit_a_dot_and_one_digit") {
    const std::string cases[] = {
        "GET /status HTTP/\r\n\r\n",      "GET /status HTTP/1\r\n\r\n",
        "GET /status HTTP/1.\r\n\r\n",    "GET /status HTTP/.1\r\n\r\n",
        "GET /status HTTP/11\r\n\r\n",    "GET /status HTTP/1.10\r\n\r\n",
        "GET /status HTTP/1.1.1\r\n\r\n", "GET /status HTTP/x.y\r\n\r\n",
        "GET /status HTTP/1.1x\r\n\r\n",  "GET /status http/1.1\r\n\r\n",
        "GET /status Http/1.1\r\n\r\n",   "GET /status HTTPS/1.1\r\n\r\n",
        "GET /status FOO\r\n\r\n",        "GET /status 1.1\r\n\r\n",
    };
    check_all_rejected_with(cases, ParseError::MalformedRequestLine);
}

TEST_CASE("answers_unsupported_version_only_for_a_version_whose_shape_it_understood") {
    const std::string cases[] = {
        "GET /status HTTP/0.9\r\n\r\n", "GET /status HTTP/1.2\r\n\r\n",
        "GET /status HTTP/2.0\r\n\r\n", "GET /status HTTP/3.0\r\n\r\n",
        "GET /status HTTP/9.9\r\n\r\n", "GET /status HTTP/0.0\r\n\r\n",
    };
    check_all_rejected_with(cases, ParseError::UnsupportedVersion);
}

TEST_CASE("tolerates_one_empty_line_before_a_request_and_refuses_a_second") {
    std::string one(kCrlf);
    one += kSeedGet;
    const auto tolerated = parse(one);
    REQUIRE_MESSAGE(tolerated.has_value(), "unexpected ", outcome_name(tolerated));
    CHECK(tolerated->consumed_bytes == one.size());

    std::string two(kCrlf);
    two += kCrlf;
    two += kSeedGet;
    const auto refused = parse(two);
    REQUIRE_MESSAGE(!refused.has_value(), "a second empty line was skipped");
    CHECK_MESSAGE(refused.error() == ParseError::MalformedRequestLine, "-> ",
                  outcome_name(refused));

    std::string many;
    for (size_t index = 0; index < kExtraPrologueCount; ++index)
        many += kCrlf;
    many += kSeedGet;
    const auto flooded = parse(many);
    REQUIRE(!flooded.has_value());
    CHECK(flooded.error() == ParseError::MalformedRequestLine);
}


TEST_CASE("answers_each_exceeded_limit_with_the_error_that_names_that_limit") {
    SUBCASE("a request line one byte over its budget is the target error") {
        const std::string at_limit = get_with_target("/" + repeated(kPaddingTargetBytes, 'a'));
        const std::string over_limit =
            get_with_target("/" + repeated(kPaddingTargetBytes + 1, 'a'));
        RequestLimits limits;
        limits.max_request_line_bytes = request_line_bytes_of(at_limit);
        limits.max_target_bytes = limits.max_request_line_bytes;

        const auto accepted = parse_request(at_limit, limits);
        REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
        const auto refused = parse_request(over_limit, limits);
        REQUIRE_MESSAGE(!refused.has_value(), "a request line one byte over budget was accepted");
        CHECK_MESSAGE(refused.error() == ParseError::TargetTooLong, "-> ", outcome_name(refused));
    }

    SUBCASE("a target one byte over its budget is the target error") {
        const std::string target = "/" + repeated(kPaddingTargetBytes, 'a');
        RequestLimits limits;
        limits.max_target_bytes = target.size();

        const auto accepted = parse_request(get_with_target(target), limits);
        REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
        const auto refused = parse_request(get_with_target(target + "a"), limits);
        REQUIRE_MESSAGE(!refused.has_value(), "a target one byte over budget was accepted");
        CHECK_MESSAGE(refused.error() == ParseError::TargetTooLong, "-> ", outcome_name(refused));
    }

    SUBCASE("a header block one byte over its budget is the header error") {
        const std::string request = get_with_field("X-Pad: " + repeated(kPaddingValueBytes, 'p'));
        RequestLimits at_limit;
        at_limit.max_header_block_bytes = request.size();
        RequestLimits over_limit;
        over_limit.max_header_block_bytes = request.size() - 1;

        const auto accepted = parse_request(request, at_limit);
        REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
        const auto refused = parse_request(request, over_limit);
        REQUIRE_MESSAGE(!refused.has_value(), "a header block one byte over budget was accepted");
        CHECK_MESSAGE(refused.error() == ParseError::TooManyHeaders, "-> ", outcome_name(refused));
    }

    SUBCASE("one field more than the count allows is the header error") {
        RequestLimits limits;
        limits.max_header_count = kSmallHeaderCount;
        std::string at_limit = "GET /status HTTP/1.1\r\n";
        at_limit += kHostFieldLine;
        for (size_t index = 0; index + 1 < kSmallHeaderCount; ++index)
            at_limit += "X-Pad: 1\r\n";
        const std::string over_limit = at_limit + "X-Pad: 1\r\n" + std::string(kCrlf);
        at_limit += kCrlf;

        const auto accepted = parse_request(at_limit, limits);
        REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
        CHECK(accepted->request.headers().size() == kSmallHeaderCount);
        const auto refused = parse_request(over_limit, limits);
        REQUIRE_MESSAGE(!refused.has_value(), "one field over the count was accepted");
        CHECK_MESSAGE(refused.error() == ParseError::TooManyHeaders, "-> ", outcome_name(refused));
    }

    SUBCASE("a declared body one byte over its budget is the body error") {
        RequestLimits limits;
        limits.max_body_bytes = kSmallBodyBytes;
        const std::string at_limit =
            post_with_fields("Content-Length: " + std::to_string(kSmallBodyBytes) + "\r\n",
                             repeated(kSmallBodyBytes, 'b'));
        const std::string over_limit =
            post_with_fields("Content-Length: " + std::to_string(kSmallBodyBytes + 1) + "\r\n",
                             repeated(kSmallBodyBytes + 1, 'b'));

        const auto accepted = parse_request(at_limit, limits);
        REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
        CHECK(accepted->request.body().size() == kSmallBodyBytes);
        const auto refused = parse_request(over_limit, limits);
        REQUIRE_MESSAGE(!refused.has_value(), "a body one byte over budget was accepted");
        CHECK_MESSAGE(refused.error() == ParseError::BodyTooLarge, "-> ", outcome_name(refused));
    }
}

TEST_CASE("refuses_a_header_count_above_the_fixed_table_however_the_limit_was_configured") {
    RequestLimits limits;
    limits.max_header_count = kUnhonourableHeaderCount;
    limits.max_header_block_bytes = kGenerousBlockBytes;

    std::string request = "GET /status HTTP/1.1\r\n";
    for (size_t index = 0; index < kOneTooManyFields; ++index)
        request += "X-Pad: 1\r\n";
    request += kCrlf;

    const auto result = parse_request(request, limits);
    REQUIRE_MESSAGE(!result.has_value(), "the header table was allowed to overflow");
    CHECK_MESSAGE(result.error() == ParseError::TooManyHeaders, "-> ", outcome_name(result));
}

TEST_CASE("answers_a_hundred_kilobyte_target_with_the_error_about_the_target") {
    const std::string request = get_with_target("/" + repeated(kHundredKilobyteTargetBytes, 'a'));
    const auto result = parse(request);
    REQUIRE_MESSAGE(!result.has_value(), "a 100 KB target was accepted");
    CHECK_MESSAGE(result.error() == ParseError::TargetTooLong, "-> ", outcome_name(result));

    const auto without_terminator = parse("GET /" + repeated(kHundredKilobyteTargetBytes, 'a'));
    REQUIRE(!without_terminator.has_value());
    CHECK_MESSAGE(without_terminator.error() == ParseError::TargetTooLong, "-> ",
                  outcome_name(without_terminator));
}

TEST_CASE("survives_ten_thousand_tiny_header_fields") {
    std::string request = "GET /status HTTP/1.1\r\n";
    for (size_t index = 0; index < kTinyHeaderFieldCount; ++index)
        request += "X: 1\r\n";
    request += kCrlf;

    const auto refused = parse(request);
    REQUIRE_MESSAGE(!refused.has_value(), "ten thousand fields were accepted");
    CHECK_MESSAGE(refused.error() == ParseError::TooManyHeaders, "-> ", outcome_name(refused));

    RequestLimits generous;
    generous.max_header_block_bytes = kGenerousBlockBytes;
    const auto by_count = parse_request(request, generous);
    REQUIRE(!by_count.has_value());
    CHECK_MESSAGE(by_count.error() == ParseError::TooManyHeaders, "-> ", outcome_name(by_count));
}

TEST_CASE("survives_a_single_header_line_of_a_megabyte") {
    std::string request = "GET /status HTTP/1.1\r\n";
    request += kHostFieldLine;
    request += "X-Pad: ";
    request += repeated(kMegabyteHeaderValueBytes, 'p');
    request += kBlockTerminator;

    const auto refused = parse(request);
    REQUIRE_MESSAGE(!refused.has_value(), "a megabyte header line was accepted at default limits");
    CHECK_MESSAGE(refused.error() == ParseError::TooManyHeaders, "-> ", outcome_name(refused));

    RequestLimits generous;
    generous.max_header_block_bytes = kGenerousBlockBytes;
    const auto accepted = parse_request(request, generous);
    REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
    REQUIRE(accepted->request.header("x-pad").has_value());
    CHECK(accepted->request.header("x-pad")->size() == kMegabyteHeaderValueBytes);
}

TEST_CASE("refuses_a_query_with_more_parameters_than_the_table_can_hold") {
    std::string flooded = "/status";
    for (size_t index = 0; index < kOneTooManyFields; ++index) {
        flooded += index == 0 ? '?' : '&';
        flooded += "a" + std::to_string(index) + "=1";
    }
    const auto refused = parse(get_with_target(flooded));
    REQUIRE_MESSAGE(!refused.has_value(), "a query flood was silently truncated");
    CHECK_MESSAGE(refused.error() == ParseError::TargetTooLong, "-> ", outcome_name(refused));

    std::string full = "/status";
    for (size_t index = 0; index < kMaxParsedHeaders; ++index) {
        full += index == 0 ? '?' : '&';
        full += "a" + std::to_string(index) + "=1";
    }
    const std::string full_wire = get_with_target(full);
    const auto accepted = parse(full_wire);
    REQUIRE_MESSAGE(accepted.has_value(), "unexpected ", outcome_name(accepted));
    REQUIRE(accepted->request.query_param("a0").has_value());
    REQUIRE(accepted->request.query_param("a63").has_value());
    CHECK(*accepted->request.query_param("a63") == "1");
}


TEST_CASE("refuses_a_nul_byte_at_every_structural_position_of_a_request") {
    const std::string valid(kSeedGet);
    size_t accepted_count = 0;
    size_t first_accepted_position = 0;
    for (size_t position = 0; position < valid.size(); ++position) {
        std::string mutated = valid;
        mutated.insert(position, 1, kNulByte);
        if (!parse(mutated).has_value())
            continue;
        if (accepted_count++ == 0)
            first_accepted_position = position;
    }
    CHECK_MESSAGE(accepted_count == 0u, "a NUL at offset ", first_accepted_position,
                  " was parsed rather than refused (", accepted_count, " positions in all)");

    const RejectionCase positions[] = {
        {"a NUL inside the method", with_nul_between("GET", " /status HTTP/1.1\r\n\r\n"),
         ParseError::UnknownMethod},
        {"a NUL inside the target", with_nul_between("GET /sta", "tus HTTP/1.1\r\n\r\n"),
         ParseError::MalformedRequestLine},
        {"a NUL inside the version", with_nul_between("GET /status HTTP/1", ".1\r\n\r\n"),
         ParseError::MalformedRequestLine},
        {"a NUL inside a field name",
         with_nul_between("GET /status HTTP/1.1\r\nHo", "st: x\r\n\r\n"),
         ParseError::MalformedHeader},
        {"a NUL inside a field value",
         with_nul_between("GET /status HTTP/1.1\r\nHost: ", "x\r\n\r\n"),
         ParseError::MalformedHeader},
        {"a NUL splitting the terminator",
         with_nul_between("GET /status HTTP/1.1\r\nHost: x\r\n", "\r\n"),
         ParseError::NeedsMoreData},
    };
    check_each_rejection(positions);

    const std::string with_body =
        post_with_fields("Content-Length: 3\r\n", with_nul_between("a", "b"));
    const auto parsed_body = parse(with_body);
    REQUIRE_MESSAGE(parsed_body.has_value(), "unexpected ", outcome_name(parsed_body));
    CHECK(parsed_body->request.body().size() == 3u);
    CHECK(parsed_body->request.body() == with_nul_between("a", "b"));
}

TEST_CASE("returns_a_parse_or_incomplete_at_every_truncation_point_and_never_a_third_answer") {
    const std::string whole = seed_request();
    size_t wrong_outcome_count = 0;
    size_t first_wrong_length = 0;
    const char* first_wrong_outcome = "";
    for (size_t length = 0; length < whole.size(); ++length) {
        const std::string_view prefix = std::string_view(whole).substr(0, length);
        const auto result = parse(prefix);
        const char* wrong = nullptr;
        if (result.has_value())
            wrong = broken_promise_of(*result, prefix);
        else if (result.error() != ParseError::NeedsMoreData)
            wrong = outcome_name(result);
        if (wrong == nullptr)
            continue;
        if (wrong_outcome_count++ == 0) {
            first_wrong_length = length;
            first_wrong_outcome = wrong;
        }
    }
    CHECK_MESSAGE(wrong_outcome_count == 0u, "a prefix of ", first_wrong_length, " bytes answered ",
                  first_wrong_outcome, " instead of NeedsMoreData");

    const auto complete = parse(whole);
    REQUIRE_MESSAGE(complete.has_value(), "unexpected ", outcome_name(complete));
    CHECK(complete->request.body() == kSeedBody);
    CHECK(complete->consumed_bytes == whole.size());
}

TEST_CASE("answers_every_seeded_mutation_of_a_valid_request_without_breaking_a_promise") {
    SeededGenerator generator(kFuzzSeed);
    const std::string seed = seed_request();

    size_t broken_count = 0;
    size_t first_broken_round = 0;
    const char* first_broken_promise = "";
    size_t accepted_count = 0;
    for (size_t round = 0; round < kFuzzRounds; ++round) {
        std::string mutated = seed;
        const size_t mutations = generator.below(kMaxMutationsPerRound) + 1;
        for (size_t applied = 0; applied < mutations; ++applied)
            mutate_once(mutated, generator);

        const auto result = parse_request(mutated, kDefaultLimits);
        if (!result.has_value())
            continue;
        ++accepted_count;
        const char* const broken = broken_promise_of(*result, mutated);
        if (broken == nullptr)
            continue;
        if (broken_count++ == 0) {
            first_broken_round = round;
            first_broken_promise = broken;
        }
    }

    CHECK_MESSAGE(broken_count == 0u, "round ", first_broken_round, " parsed into a request where ",
                  first_broken_promise, " (", broken_count, " rounds in all)");
    CHECK_MESSAGE(accepted_count > 0u,
                  "every mutation was refused, so no parsed request was ever inspected");
    CHECK_MESSAGE(accepted_count < kFuzzRounds,
                  "every mutation was accepted, so no mutation reached the refusing path");
}

} // namespace
} // namespace erikslund::http
