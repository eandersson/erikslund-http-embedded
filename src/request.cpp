
#include "erikslund/http/request.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/text.hpp"
#include "internal/request_parser.hpp"

// Request views borrow either bytes or the request's decode arena. Keep bytes alive and unchanged
// until the response is complete.

namespace erikslund::http {
namespace {

inline constexpr std::string_view kCrlf = "\r\n";
inline constexpr std::string_view kHeaderTerminator = "\r\n\r\n";
inline constexpr std::string_view kVersionPrefix = "HTTP/";
inline constexpr std::string_view kHttpVersion11 = "HTTP/1.1";
inline constexpr std::string_view kHttpVersion10 = "HTTP/1.0";
inline constexpr size_t kVersionNumberBytes = 3;
inline constexpr size_t kVersionSeparatorOffset = 1;
inline constexpr char kVersionSeparator = '.';
inline constexpr std::string_view kRootPath = "/";
inline constexpr std::string_view kSchemeSeparator = "://";
inline constexpr std::string_view kHttpScheme = "http";
inline constexpr std::string_view kHttpsScheme = "https";
inline constexpr std::string_view kAuthorityTerminators = "/?";
inline constexpr std::string_view kCurrentSegment = ".";
inline constexpr std::string_view kParentSegment = "..";
inline constexpr std::string_view kTokenSpecialCharacters = "!#$%&'*+-.^_`|~";

inline constexpr std::string_view kHostName = "Host";
inline constexpr std::string_view kContentLengthName = "Content-Length";
inline constexpr std::string_view kTransferEncodingName = "Transfer-Encoding";
inline constexpr std::string_view kConnectionName = "Connection";
inline constexpr std::string_view kAcceptEncodingName = "Accept-Encoding";
inline constexpr std::string_view kGzipCoding = "gzip";
inline constexpr std::string_view kLegacyGzipCoding = "x-gzip";
inline constexpr std::string_view kAnyCoding = "*";
inline constexpr std::string_view kCloseOption = "close";
inline constexpr std::string_view kKeepAliveOption = "keep-alive";
inline constexpr std::string_view kQualityParameterName = "q";

inline constexpr char kSpace = ' ';
inline constexpr char kHorizontalTab = '\t';
inline constexpr char kPathSeparator = '/';
inline constexpr char kQueryMarker = '?';
inline constexpr char kFragmentMarker = '#';
inline constexpr char kFieldSeparator = ':';
inline constexpr char kListSeparator = ',';
inline constexpr char kParameterSeparator = ';';
inline constexpr char kQueryPairSeparator = '&';
inline constexpr char kQueryValueSeparator = '=';
inline constexpr char kPercentMarker = '%';
inline constexpr char kPlusMarker = '+';

inline constexpr char kPortSeparator = ':';
inline constexpr char kLabelSeparator = '.';
inline constexpr char kUserInfoMarker = '@';
inline constexpr char kIpLiteralOpen = '[';
inline constexpr char kIpLiteralClose = ']';
inline constexpr std::string_view kRegisteredNameSpecials = "-._~";
inline constexpr uint32_t kMaxPortNumber = 65'535;
inline constexpr size_t kMaxPortDigits = 5;

inline constexpr unsigned char kSpaceByte = 0x20;
inline constexpr unsigned char kDeleteByte = 0x7F;
inline constexpr uint8_t kHexLetterOffset = 10;
inline constexpr unsigned kHexDigitBits = 4;

[[nodiscard]] constexpr std::optional<char> byte_at(std::string_view bytes, size_t index) noexcept {
    if (index >= bytes.size())
        return std::nullopt;
    return bytes[index];
}

[[nodiscard]] constexpr std::string_view slice(std::string_view bytes, size_t offset,
                                               size_t length) noexcept {
    if (offset > bytes.size())
        return {};
    return bytes.substr(offset, length);
}

[[nodiscard]] constexpr bool is_decimal_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] constexpr std::optional<uint8_t> hex_value(char character) noexcept {
    if (character >= '0' && character <= '9')
        return static_cast<uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<uint8_t>(character - 'a' + kHexLetterOffset);
    if (character >= 'A' && character <= 'F')
        return static_cast<uint8_t>(character - 'A' + kHexLetterOffset);
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view trim_optional_whitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == kSpace || value.front() == kHorizontalTab))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == kSpace || value.back() == kHorizontalTab))
        value.remove_suffix(1);
    return value;
}

// RFC 9110 tchar.
[[nodiscard]] constexpr bool is_token_character(char character) noexcept {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9'))
        return true;
    return kTokenSpecialCharacters.contains(character);
}

[[nodiscard]] constexpr bool is_valid_target_byte(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    if (value <= kSpaceByte || value == kDeleteByte)
        return false;
    return character != kFragmentMarker;
}

// Reject line terminators and NUL so echoed values cannot split a response.
[[nodiscard]] constexpr bool is_valid_field_value_byte(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    return character == kHorizontalTab || (value >= kSpaceByte && value != kDeleteByte);
}

// Treat repeated and delimited list fields identically.
template <class Visitor>
void for_each_list_member(std::string_view value, char separator, Visitor visit) {
    while (!value.empty()) {
        const size_t next = value.find(separator);
        const std::string_view member = trim_optional_whitespace(
            next == std::string_view::npos ? value : value.substr(0, next));
        if (!member.empty())
            visit(member);
        if (next == std::string_view::npos)
            return;
        value.remove_prefix(next + 1);
    }
}

// The arena is reserved before any borrowed views are created.
[[nodiscard]] std::string_view arena_view(const std::string& arena, size_t offset, size_t length)
    ERIKSLUND_HTTP_PRE(offset + length <= arena.size()) {
    return std::string_view(arena.data() + offset, length);
}

[[nodiscard]] bool decode_query_component_into(std::string& arena, std::string_view raw)
    ERIKSLUND_HTTP_PRE(arena.size() + raw.size() <= arena.capacity()) {
    for (size_t index = 0; index < raw.size(); ++index) {
        const char character = raw[index];
        if (character == kPlusMarker) {
            arena.push_back(kSpace);
            continue;
        }
        if (character != kPercentMarker) {
            arena.push_back(character);
            continue;
        }
        const std::optional<char> high = byte_at(raw, index + 1);
        const std::optional<char> low = byte_at(raw, index + 2);
        if (!high || !low)
            return false;
        const std::optional<uint8_t> high_value = hex_value(*high);
        const std::optional<uint8_t> low_value = hex_value(*low);
        if (!high_value || !low_value)
            return false;
        const auto decoded =
            static_cast<unsigned char>((*high_value << kHexDigitBits) | *low_value);
        if (decoded < kSpaceByte || decoded == kDeleteByte)
            return false;
        arena.push_back(static_cast<char>(decoded));
        index += 2;
    }
    return true;
}

[[nodiscard]] constexpr bool path_is_canonical(std::string_view raw_path) noexcept {
    if (raw_path.empty() || raw_path.front() != kPathSeparator || raw_path.contains('\\'))
        return false;
    std::string_view rest = raw_path.substr(1);
    while (!rest.empty()) {
        const size_t separator = rest.find(kPathSeparator);
        const std::string_view segment =
            separator == std::string_view::npos ? rest : rest.substr(0, separator);
        if (segment.empty() || segment == kCurrentSegment || segment == kParentSegment ||
            segment.contains(kPercentMarker))
            return false;
        if (separator == std::string_view::npos)
            return true;
        rest = rest.substr(separator + 1);
    }
    return true;
}

// Eager decoding prevents later arena growth from invalidating existing views.
[[nodiscard]] std::expected<void, ParseError> decode_query_into(
    std::string& arena, std::string_view raw_query,
    std::inplace_vector<HeaderView, kMaxParsedHeaders>& out) {
    std::string_view rest = raw_query;
    while (!rest.empty()) {
        const size_t next_pair = rest.find(kQueryPairSeparator);
        const std::string_view pair =
            next_pair == std::string_view::npos ? rest : rest.substr(0, next_pair);
        rest = next_pair == std::string_view::npos ? std::string_view{}
                                                   : rest.substr(next_pair + 1);
        if (pair.empty())
            continue;

        const size_t assignment = pair.find(kQueryValueSeparator);
        const std::string_view raw_name =
            assignment == std::string_view::npos ? pair : pair.substr(0, assignment);
        const std::string_view raw_value =
            assignment == std::string_view::npos ? std::string_view{} : pair.substr(assignment + 1);
        if (raw_name.empty())
            continue;

        if (out.size() == out.capacity())
            return std::unexpected(ParseError::TargetTooLong);

        const size_t name_begin = arena.size();
        if (!decode_query_component_into(arena, raw_name))
            return std::unexpected(ParseError::BadPercentEncoding);
        const size_t value_begin = arena.size();
        if (!decode_query_component_into(arena, raw_value))
            return std::unexpected(ParseError::BadPercentEncoding);
        out.push_back(HeaderView{arena_view(arena, name_begin, value_begin - name_begin),
                                 arena_view(arena, value_begin, arena.size() - value_begin)});
    }
    return {};
}

struct TargetParts {
    std::string_view path;
    std::string_view query;
    // RFC 9112 gives absolute-form authority precedence over Host.
    std::string_view authority;
};

[[nodiscard]] std::expected<TargetParts, ParseError> split_target(std::string_view target) {
    for (const char character : target)
        if (!is_valid_target_byte(character))
            return std::unexpected(ParseError::MalformedRequestLine);

    std::string_view remainder = target;
    std::string_view authority;
    if (!remainder.starts_with(kPathSeparator)) {
        const size_t scheme_end = remainder.find(kSchemeSeparator);
        if (scheme_end == std::string_view::npos)
            return std::unexpected(ParseError::MalformedRequestLine);
        const std::string_view scheme = remainder.substr(0, scheme_end);
        if (!equals_ignore_case(scheme, kHttpScheme) && !equals_ignore_case(scheme, kHttpsScheme))
            return std::unexpected(ParseError::MalformedRequestLine);
        remainder.remove_prefix(scheme_end + kSchemeSeparator.size());
        const size_t authority_end = remainder.find_first_of(kAuthorityTerminators);
        authority = remainder.substr(0, authority_end);
        remainder = authority_end == std::string_view::npos ? std::string_view{}
                                                            : remainder.substr(authority_end);
        if (authority.empty())
            return std::unexpected(ParseError::MalformedRequestLine);
    }

    const size_t query_begin = remainder.find(kQueryMarker);
    const std::string_view path =
        query_begin == std::string_view::npos ? remainder : remainder.substr(0, query_begin);
    const std::string_view query = query_begin == std::string_view::npos
                                       ? std::string_view{}
                                       : remainder.substr(query_begin + 1);
    if (path.empty())
        return TargetParts{kRootPath, query, authority};
    if (!path.starts_with(kPathSeparator))
        return std::unexpected(ParseError::MalformedRequestLine);
    return TargetParts{path, query, authority};
}

// Validate the complete RFC 9112 version shape before distinguishing unsupported from malformed.
[[nodiscard]] constexpr bool version_is_well_formed(std::string_view version) noexcept {
    if (!version.starts_with(kVersionPrefix))
        return false;
    const std::string_view number = version.substr(kVersionPrefix.size());
    return number.size() == kVersionNumberBytes && is_decimal_digit(number.front()) &&
           number[kVersionSeparatorOffset] == kVersionSeparator && is_decimal_digit(number.back());
}

struct RequestLine {
    Method method = Method::None;
    std::string_view target;
    bool is_version_11 = false;
};

[[nodiscard]] std::expected<RequestLine, ParseError> parse_request_line(std::string_view line) {
    const size_t method_end = line.find(kSpace);
    if (method_end == std::string_view::npos || method_end == 0)
        return std::unexpected(ParseError::MalformedRequestLine);
    const size_t target_end = line.find(kSpace, method_end + 1);
    if (target_end == std::string_view::npos)
        return std::unexpected(ParseError::MalformedRequestLine);
    // Extra request-line whitespace creates disagreement between strict and trimming hops.
    if (line.find(kSpace, target_end + 1) != std::string_view::npos)
        return std::unexpected(ParseError::MalformedRequestLine);

    const std::string_view target = slice(line, method_end + 1, target_end - method_end - 1);
    if (target.empty())
        return std::unexpected(ParseError::MalformedRequestLine);

    const std::optional<Method> method = method_from_token(line.substr(0, method_end));
    if (!method)
        return std::unexpected(ParseError::UnknownMethod);

    const std::string_view version = line.substr(target_end + 1);
    if (!version_is_well_formed(version))
        return std::unexpected(ParseError::MalformedRequestLine);
    if (version != kHttpVersion11 && version != kHttpVersion10)
        return std::unexpected(ParseError::UnsupportedVersion);

    return RequestLine{*method, target, version == kHttpVersion11};
}

[[nodiscard]] std::expected<void, ParseError> parse_header_fields(
    std::string_view field_lines, size_t max_header_count,
    std::inplace_vector<HeaderView, kMaxParsedHeaders>& out) {
    const size_t effective_maximum = std::min(max_header_count, out.capacity());

    while (!field_lines.empty()) {
        const size_t line_end = field_lines.find(kCrlf);
        if (line_end == std::string_view::npos)
            return std::unexpected(ParseError::MalformedHeader);
        const std::string_view line = field_lines.substr(0, line_end);
        field_lines.remove_prefix(line_end + kCrlf.size());

        // Reject obsolete folding rather than disagreeing with an unfolding proxy.
        if (line.starts_with(kSpace) || line.starts_with(kHorizontalTab))
            return std::unexpected(ParseError::MalformedHeader);

        const size_t colon = line.find(kFieldSeparator);
        if (colon == std::string_view::npos || colon == 0)
            return std::unexpected(ParseError::MalformedHeader);

        // RFC 9112 forbids whitespace before ':' to keep hops in agreement.
        const std::string_view name = line.substr(0, colon);
        for (const char character : name)
            if (!is_token_character(character))
                return std::unexpected(ParseError::MalformedHeader);

        const std::string_view value = trim_optional_whitespace(line.substr(colon + 1));
        for (const char character : value)
            if (!is_valid_field_value_byte(character))
                return std::unexpected(ParseError::MalformedHeader);

        if (out.size() >= effective_maximum)
            return std::unexpected(ParseError::TooManyHeaders);
        ERIKSLUND_HTTP_ASSERT(out.size() < out.capacity());
        out.push_back(HeaderView{name, value});
    }
    return {};
}

enum class DecimalError : uint8_t { NotANumber, OutOfRange };

[[nodiscard]] std::expected<uint64_t, DecimalError> parse_decimal(std::string_view text) noexcept {
    if (text.empty())
        return std::unexpected(DecimalError::NotANumber);
    for (const char character : text)
        if (!is_decimal_digit(character))
            return std::unexpected(DecimalError::NotANumber);
    if (text.size() > 1 && text.front() == '0')
        return std::unexpected(DecimalError::NotANumber);
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::unexpected(DecimalError::OutOfRange);
    return value;
}

// Reconciles the request-framing fields.
[[nodiscard]] std::expected<size_t, ParseError> resolve_content_length(
    std::span<const HeaderView> headers, size_t max_body_bytes) {
    bool saw_transfer_encoding = false;
    std::optional<uint64_t> declared_length;

    for (const HeaderView& field : headers) {
        if (equals_ignore_case(field.name, kTransferEncodingName)) {
            saw_transfer_encoding = true;
            continue;
        }
        if (!equals_ignore_case(field.name, kContentLengthName))
            continue;
        const std::expected<uint64_t, DecimalError> parsed = parse_decimal(field.value);
        if (!parsed) {
            return std::unexpected(parsed.error() == DecimalError::OutOfRange
                                       ? ParseError::BodyTooLarge
                                       : ParseError::MalformedHeader);
        }
        // Different lengths let adjacent hops disagree about the next request boundary.
        if (declared_length && *declared_length != *parsed)
            return std::unexpected(ParseError::MalformedHeader);
        declared_length = *parsed;
    }

    // Never partially interpret transfer coding; that would create a second framing rule.
    if (saw_transfer_encoding)
        return std::unexpected(ParseError::UnsupportedTransferEncoding);

    const uint64_t length = declared_length.value_or(0);
    if (length > max_body_bytes)
        return std::unexpected(ParseError::BodyTooLarge);
    return static_cast<size_t>(length);
}

[[nodiscard]] bool port_is_well_formed(std::string_view port) noexcept {
    if (port.empty() || port.size() > kMaxPortDigits || port.front() == '0')
        return false;
    for (const char character : port)
        if (!is_decimal_digit(character))
            return false;
    uint32_t value = 0;
    const auto result = std::from_chars(port.data(), port.data() + port.size(), value);
    if (result.ec != std::errc{} || result.ptr != port.data() + port.size())
        return false;
    return value <= kMaxPortNumber;
}

[[nodiscard]] constexpr bool registered_name_is_well_formed(std::string_view host) noexcept {
    std::string_view rest = host;
    if (rest.ends_with(kLabelSeparator))
        rest.remove_suffix(1);
    while (true) {
        const size_t separator = rest.find(kLabelSeparator);
        const std::string_view label =
            separator == std::string_view::npos ? rest : rest.substr(0, separator);
        if (label.empty())
            return false;
        for (const char character : label) {
            if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                is_decimal_digit(character))
                continue;
            if (!kRegisteredNameSpecials.contains(character))
                return false;
        }
        if (separator == std::string_view::npos)
            return true;
        rest = rest.substr(separator + 1);
    }
}

[[nodiscard]] bool ip_literal_is_well_formed(std::string_view literal) noexcept {
    if (!literal.starts_with(kIpLiteralOpen) || !literal.ends_with(kIpLiteralClose) ||
        literal.size() <= 2)
        return false;
    const std::string_view body = literal.substr(1, literal.size() - 2);
    if (body.size() >= INET6_ADDRSTRLEN)
        return false;

    std::array<char, INET6_ADDRSTRLEN> address{};
    std::ranges::copy(body, address.begin());
    in6_addr parsed{};
    return ::inet_pton(AF_INET6, address.data(), &parsed) == 1;
}

// Parses RFC 9112 uri-host with an optional port. Rejects ambiguous userinfo.
[[nodiscard]] bool authority_is_well_formed(std::string_view authority) noexcept {
    if (authority.empty() || authority.contains(kUserInfoMarker))
        return false;

    std::string_view host = authority;
    std::optional<std::string_view> port;
    if (authority.starts_with(kIpLiteralOpen)) {
        const size_t literal_end = authority.find(kIpLiteralClose);
        if (literal_end == std::string_view::npos)
            return false;
        host = authority.substr(0, literal_end + 1);
        const std::string_view tail = authority.substr(literal_end + 1);
        if (!tail.empty()) {
            if (tail.front() != kPortSeparator)
                return false;
            port = tail.substr(1);
        }
        if (!ip_literal_is_well_formed(host))
            return false;
    } else {
        const size_t separator = authority.find(kPortSeparator);
        if (separator != std::string_view::npos) {
            host = authority.substr(0, separator);
            port = authority.substr(separator + 1);
        }
        if (!registered_name_is_well_formed(host))
            return false;
    }
    return !port || port_is_well_formed(*port);
}

// Reconciles HTTP/1.1 Host with absolute-form authority so adjacent hops address the same server.
[[nodiscard]] std::expected<std::string_view, ParseError> resolve_authority(
    std::span<const HeaderView> headers, std::string_view target_authority, bool is_version_11) {
    std::optional<std::string_view> host;
    for (const HeaderView& field : headers) {
        if (!equals_ignore_case(field.name, kHostName))
            continue;
        if (host)
            return std::unexpected(ParseError::MalformedHost);
        host = field.value;
    }

    if (!host && is_version_11)
        return std::unexpected(ParseError::MissingHost);
    if (host && !authority_is_well_formed(*host))
        return std::unexpected(ParseError::MalformedHost);
    if (target_authority.empty())
        return host.value_or(std::string_view{});

    if (!authority_is_well_formed(target_authority))
        return std::unexpected(ParseError::MalformedHost);
    if (host && !equals_ignore_case(*host, target_authority))
        return std::unexpected(ParseError::MalformedHost);
    return target_authority;
}

[[nodiscard]] bool resolve_keep_alive(std::span<const HeaderView> headers, bool is_version_11) {
    bool close_requested = false;
    bool keep_alive_requested = false;
    for (const HeaderView& field : headers) {
        if (!equals_ignore_case(field.name, kConnectionName))
            continue;
        for_each_list_member(field.value, kListSeparator, [&](std::string_view option) {
            if (equals_ignore_case(option, kCloseOption))
                close_requested = true;
            else if (equals_ignore_case(option, kKeepAliveOption))
                keep_alive_requested = true;
        });
    }
    // On contradiction, close wins.
    if (close_requested)
        return false;
    if (is_version_11)
        return true;
    return keep_alive_requested;
}

[[nodiscard]] constexpr bool weight_is_zero(std::string_view weight) noexcept {
    bool saw_digit = false;
    for (const char character : weight) {
        if (character == '.')
            continue;
        if (character < '0' || character > '9')
            return false;
        if (character != '0')
            return false;
        saw_digit = true;
    }
    return saw_digit;
}

[[nodiscard]] bool quality_is_zero(std::string_view parameters) {
    bool is_zero = false;
    for_each_list_member(parameters, kParameterSeparator, [&](std::string_view parameter) {
        const size_t assignment = parameter.find(kQueryValueSeparator);
        if (assignment == std::string_view::npos)
            return;
        const std::string_view name = trim_optional_whitespace(parameter.substr(0, assignment));
        if (!equals_ignore_case(name, kQualityParameterName))
            return;
        is_zero = weight_is_zero(trim_optional_whitespace(parameter.substr(assignment + 1)));
    });
    return is_zero;
}

[[nodiscard]] bool resolve_wants_gzip(std::span<const HeaderView> headers) {
    std::optional<bool> gzip_acceptable;
    std::optional<bool> wildcard_acceptable;
    for (const HeaderView& field : headers) {
        if (!equals_ignore_case(field.name, kAcceptEncodingName))
            continue;
        for_each_list_member(field.value, kListSeparator, [&](std::string_view member) {
            const size_t parameters = member.find(kParameterSeparator);
            const std::string_view coding = trim_optional_whitespace(
                parameters == std::string_view::npos ? member : member.substr(0, parameters));
            const bool acceptable = parameters == std::string_view::npos ||
                                    !quality_is_zero(member.substr(parameters + 1));
            if (equals_ignore_case(coding, kGzipCoding) ||
                equals_ignore_case(coding, kLegacyGzipCoding))
                gzip_acceptable = acceptable;
            else if (coding == kAnyCoding)
                wildcard_acceptable = acceptable;
        });
    }
    return gzip_acceptable.value_or(wildcard_acceptable.value_or(false));
}

} // namespace

Request::Request() = default;
Request::~Request() = default;

Request::Request(Request&& other) noexcept
    : method_(other.method_),
      raw_target_(other.raw_target_),
      path_(other.path_),
      query_(other.query_),
      authority_(other.authority_),
      body_(other.body_),
      headers_(other.headers_),
      decode_arena_(std::move(other.decode_arena_)),
      query_parameters_(other.query_parameters_),
      path_parameters_(other.path_parameters_),
      peer_(other.peer_),
      received_at_(other.received_at_),
      client_certificate_subject_(std::move(other.client_certificate_subject_)),
      keep_alive_(other.keep_alive_),
      wants_gzip_(other.wants_gzip_),
      is_secure_(other.is_secure_),
      has_client_certificate_(other.has_client_certificate_) {
    other.reset();
}

Request& Request::operator=(Request&& other) noexcept {
    if (this == &other)
        return *this;
    method_ = other.method_;
    raw_target_ = other.raw_target_;
    path_ = other.path_;
    query_ = other.query_;
    authority_ = other.authority_;
    body_ = other.body_;
    headers_ = other.headers_;
    decode_arena_ = std::move(other.decode_arena_);
    query_parameters_ = other.query_parameters_;
    path_parameters_ = other.path_parameters_;
    peer_ = other.peer_;
    received_at_ = other.received_at_;
    client_certificate_subject_ = std::move(other.client_certificate_subject_);
    keep_alive_ = other.keep_alive_;
    wants_gzip_ = other.wants_gzip_;
    is_secure_ = other.is_secure_;
    has_client_certificate_ = other.has_client_certificate_;
    other.reset();
    return *this;
}

std::optional<std::string_view> Request::query_param(std::string_view name) const {
    for (const HeaderView& parameter : query_parameters_)
        if (parameter.name == name)
            return parameter.value;
    return std::nullopt;
}

std::optional<std::string_view> Request::header(std::string_view name) const {
    for (const HeaderView& field : headers_)
        if (equals_ignore_case(field.name, name))
            return field.value;
    return std::nullopt;
}

std::string_view Request::param(std::string_view name) const noexcept {
    for (const HeaderView& parameter : path_parameters_)
        if (parameter.name == name)
            return parameter.value;
    return {};
}

std::optional<std::string_view> Request::client_certificate_subject() const {
    if (!has_client_certificate_)
        return std::nullopt;
    return std::string_view(client_certificate_subject_);
}

void Request::set_peer(const PeerAddress& remote_address) noexcept {
    peer_ = remote_address;
}

void Request::set_secure(bool secure) noexcept {
    is_secure_ = secure;
}

void Request::set_client_certificate_subject(std::string subject) {
    client_certificate_subject_ = std::move(subject);
    has_client_certificate_ = true;
}

void Request::set_received_at(std::chrono::steady_clock::time_point when) noexcept {
    received_at_ = when;
}

void Request::bind_path_parameter(std::string_view name, std::string_view value) {
    if (path_parameters_.size() == path_parameters_.capacity())
        return;
    path_parameters_.push_back(HeaderView{name, value});
}

void Request::clear_path_parameters() noexcept {
    path_parameters_.clear();
}

void Request::reset() noexcept {
    method_ = Method::None;
    raw_target_ = {};
    path_ = {};
    query_ = {};
    authority_ = {};
    body_ = {};
    headers_.clear();
    query_parameters_.clear();
    path_parameters_.clear();
    if (decode_arena_)
        decode_arena_->clear();
    peer_ = PeerAddress{};
    received_at_ = {};
    client_certificate_subject_.clear();
    keep_alive_ = false;
    wants_gzip_ = false;
    is_secure_ = false;
    has_client_certificate_ = false;
}

std::expected<ParsedRequest, ParseError> internal::RequestParser::parse(
    std::string_view bytes, const RequestLimits& limits) {
    if (!head_complete_) {
        const std::expected<void, ParseError> head = parse_head(bytes, limits);
        if (!head)
            return std::unexpected(head.error());
    }

    if (bytes.size() < body_offset_)
        return std::unexpected(ParseError::NeedsMoreData);

    if (bytes.data() != buffer_base_)
        rebase_borrowed_views(bytes);

    if (bytes.size() - body_offset_ < body_bytes_)
        return std::unexpected(ParseError::NeedsMoreData);

    request_.body_ = slice(bytes, body_offset_, body_bytes_);
    ParsedRequest parsed;
    parsed.request = std::move(request_);
    parsed.body_offset = body_offset_;
    parsed.consumed_bytes = body_offset_ + body_bytes_;
    reset();
    return parsed;
}

std::expected<void, ParseError> internal::RequestParser::parse_head(
    std::string_view bytes, const RequestLimits& limits) {
    // RFC 9112 permits one leading empty line, not an unbounded stream of them.
    const size_t prologue_bytes = bytes.starts_with(kCrlf) ? kCrlf.size() : 0;
    const std::string_view request_bytes = bytes.substr(prologue_bytes);

    const size_t request_line_end = request_bytes.find(kCrlf);
    const size_t request_line_bytes =
        request_line_end == std::string_view::npos ? request_bytes.size() : request_line_end;
    if (request_line_bytes > limits.max_request_line_bytes)
        return std::unexpected(ParseError::TargetTooLong);

    const size_t terminator = request_bytes.find(kHeaderTerminator);
    if (terminator == std::string_view::npos) {
        if (request_bytes.size() > limits.max_header_block_bytes)
            return std::unexpected(ParseError::TooManyHeaders);
        return std::unexpected(ParseError::NeedsMoreData);
    }

    const size_t header_block_bytes = terminator + kHeaderTerminator.size();
    if (header_block_bytes > limits.max_header_block_bytes)
        return std::unexpected(ParseError::TooManyHeaders);

    ERIKSLUND_HTTP_ASSERT(request_line_end != std::string_view::npos &&
                          request_line_end <= terminator);

    const std::expected<RequestLine, ParseError> line =
        parse_request_line(slice(request_bytes, 0, request_line_end));
    if (!line)
        return std::unexpected(line.error());
    if (line->target.size() > limits.max_target_bytes)
        return std::unexpected(ParseError::TargetTooLong);

    Request request;
    const size_t fields_begin = request_line_end + kCrlf.size();
    const size_t fields_end = terminator + kCrlf.size();
    const std::expected<void, ParseError> fields =
        parse_header_fields(slice(request_bytes, fields_begin, fields_end - fields_begin),
                            limits.max_header_count, request.headers_);
    if (!fields)
        return std::unexpected(fields.error());

    const std::expected<size_t, ParseError> content_length =
        resolve_content_length(request.headers_, limits.max_body_bytes);
    if (!content_length)
        return std::unexpected(content_length.error());

    const std::expected<TargetParts, ParseError> target = split_target(line->target);
    if (!target)
        return std::unexpected(target.error());

    const std::expected<std::string_view, ParseError> authority =
        resolve_authority(request.headers_, target->authority, line->is_version_11);
    if (!authority)
        return std::unexpected(authority.error());
    if (!path_is_canonical(target->path))
        return std::unexpected(ParseError::MalformedRequestLine);

    if (!target->query.empty()) {
        request.decode_arena_ = std::make_unique<std::string>();
        request.decode_arena_->reserve(target->query.size());
    }
    [[maybe_unused]] const char* const arena_origin =
        request.decode_arena_ ? request.decode_arena_->data() : nullptr;

    request.path_ = target->path;
    if (!target->query.empty()) {
        const std::expected<void, ParseError> parameters =
            decode_query_into(*request.decode_arena_, target->query, request.query_parameters_);
        if (!parameters)
            return std::unexpected(parameters.error());
    }
    ERIKSLUND_HTTP_ASSERT(!request.decode_arena_ || request.decode_arena_->data() == arena_origin);

    request.method_ = line->method;
    request.raw_target_ = line->target;
    request.query_ = target->query;
    request.authority_ = *authority;
    request.keep_alive_ = resolve_keep_alive(request.headers_, line->is_version_11);
    request.wants_gzip_ = resolve_wants_gzip(request.headers_);
    request.received_at_ = std::chrono::steady_clock::now();

    request_ = std::move(request);
    body_offset_ = prologue_bytes + header_block_bytes;
    body_bytes_ = *content_length;
    buffer_base_ = bytes.data();
    remember_buffer_offsets();
    head_complete_ = true;
    return {};
}

void internal::RequestParser::remember_buffer_offsets() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(buffer_base_);
    const uintptr_t limit = base + body_offset_;
    const auto offset_of = [base, limit](std::string_view view) {
        BufferViewOffset offset;
        if (view.empty())
            return offset;
        const uintptr_t address = reinterpret_cast<uintptr_t>(view.data());
        if (address >= base && address < limit) {
            offset.value = static_cast<size_t>(address - base);
            offset.borrowed = true;
        }
        return offset;
    };

    raw_target_offset_ = offset_of(request_.raw_target_);
    path_offset_ = offset_of(request_.path_);
    query_offset_ = offset_of(request_.query_);
    authority_offset_ = offset_of(request_.authority_);
    header_offsets_.clear();
    for (const HeaderView& header : request_.headers_)
        header_offsets_.push_back(HeaderOffsets{offset_of(header.name), offset_of(header.value)});
}

void internal::RequestParser::rebase_borrowed_views(std::string_view bytes) noexcept {
    const char* const new_base = bytes.data();
    const auto rebase = [new_base](std::string_view& view, BufferViewOffset offset) {
        if (offset.borrowed)
            view = std::string_view(new_base + offset.value, view.size());
    };
    rebase(request_.raw_target_, raw_target_offset_);
    rebase(request_.path_, path_offset_);
    rebase(request_.query_, query_offset_);
    rebase(request_.authority_, authority_offset_);
    ERIKSLUND_HTTP_ASSERT(header_offsets_.size() == request_.headers_.size());
    for (size_t index = 0; index < request_.headers_.size(); ++index) {
        rebase(request_.headers_[index].name, header_offsets_[index].name);
        rebase(request_.headers_[index].value, header_offsets_[index].value);
    }
    buffer_base_ = new_base;
}

void internal::RequestParser::reset() noexcept {
    request_.reset();
    buffer_base_ = nullptr;
    raw_target_offset_ = {};
    path_offset_ = {};
    query_offset_ = {};
    authority_offset_ = {};
    header_offsets_.clear();
    body_offset_ = 0;
    body_bytes_ = 0;
    head_complete_ = false;
}

std::expected<ParsedRequest, ParseError> parse_request(std::string_view bytes,
                                                       const RequestLimits& limits) {
    internal::RequestParser parser;
    return parser.parse(bytes, limits);
}

} // namespace erikslund::http
