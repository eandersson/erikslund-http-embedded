#include "internal/response_encoder.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "erikslund/http/method.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/text.hpp"
#include "internal/compress.hpp"
#include "internal/conditional_request.hpp"

namespace erikslund::http::internal {
namespace {

constexpr size_t kMaxDecimalDigits = 24;
constexpr std::string_view kCrLf = "\r\n";
constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kAsciiDelete = 0x7F;

constexpr std::string_view kHtmlContentSecurityPolicy =
    "default-src 'self'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'; "
    "form-action 'none'; object-src 'none'";
constexpr std::string_view kHtmlContentTypePrefix = "text/html";

constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kContentEncodingField = "Content-Encoding";
constexpr std::string_view kVaryField = "Vary";
constexpr std::string_view kAcceptEncodingField = "Accept-Encoding";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kContentLocationField = "Content-Location";
constexpr std::string_view kDateField = "Date";
constexpr std::string_view kEtagField = "ETag";
constexpr std::string_view kExpiresField = "Expires";
constexpr std::string_view kIfNoneMatchField = "If-None-Match";
constexpr std::string_view kGzipCoding = "gzip";
constexpr std::string_view kFieldListSeparator = ", ";

constexpr std::string_view kContentLengthField = "Content-Length";
constexpr std::string_view kConnectionField = "Connection";
constexpr std::string_view kTransferEncodingField = "Transfer-Encoding";
constexpr std::string_view kKeepAliveToken = "keep-alive";
constexpr std::string_view kCloseToken = "close";

[[nodiscard]] bool has_control_character(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < kFirstPrintableAscii || byte == kAsciiDelete)
            return true;
    }
    return false;
}

void append_number(Buffer& buffer, uint64_t value) {
    std::array<char, kMaxDecimalDigits> digits{};
    const std::to_chars_result converted =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (converted.ec == std::errc())
        buffer.append(std::string_view(digits.data(),
                                       static_cast<size_t>(converted.ptr - digits.data())));
}

void append_field(Buffer& buffer, std::string_view name, std::string_view value) {
    buffer.append(name);
    buffer.append(": ");
    buffer.append(value);
    buffer.append(kCrLf);
}

[[nodiscard]] std::string_view trim_optional_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool is_framing_field(std::string_view name) noexcept {
    return equals_ignore_case(name, kContentLengthField) ||
           equals_ignore_case(name, kConnectionField) ||
           equals_ignore_case(name, kTransferEncodingField);
}

[[nodiscard]] bool list_names(std::string_view value, std::string_view member) noexcept {
    while (!value.empty()) {
        const size_t comma = value.find(',');
        const std::string_view entry = trim_optional_whitespace(
            comma == std::string_view::npos ? value : value.substr(0, comma));
        if (equals_ignore_case(entry, member))
            return true;
        if (comma == std::string_view::npos)
            return false;
        value.remove_prefix(comma + 1);
    }
    return false;
}

template <class Append>
void append_accept_encoding_vary(std::string_view declared, Append append) {
    if (declared.empty()) {
        append(kAcceptEncodingField);
        return;
    }
    append(declared);
    if (!list_names(declared, kAcceptEncodingField)) {
        append(kFieldListSeparator);
        append(kAcceptEncodingField);
    }
}

[[nodiscard]] Response not_modified_from(const Response& source) {
    Response not_modified = Response::empty(Status::NotModified);

    constexpr std::array<std::string_view, 5> kPreservedFields{
        kCacheControlField, kContentLocationField, kDateField, kEtagField, kExpiresField};
    for (const std::string_view field : kPreservedFields)
        if (const std::optional<std::string_view> value = source.find_header(field))
            not_modified.header(std::string(field), std::string(*value));

    const std::optional<std::string_view> declared_vary = source.find_header(kVaryField);
    const bool varies_on_accept_encoding = response_varies_on_accept_encoding(
        source.find_header(kContentTypeField).value_or(std::string_view{}),
        source.find_header(kContentEncodingField).value_or(std::string_view{}),
        source.body().size());
    if (varies_on_accept_encoding) {
        std::string merged_vary;
        const std::string_view existing = declared_vary.value_or(std::string_view{});
        merged_vary.reserve(existing.size() + kFieldListSeparator.size() +
                            kAcceptEncodingField.size());
        append_accept_encoding_vary(existing, [&merged_vary](std::string_view part) {
            merged_vary.append(part);
        });
        not_modified.header(std::string(kVaryField), std::move(merged_vary));
    } else if (declared_vary.has_value()) {
        not_modified.header(std::string(kVaryField), std::string(*declared_vary));
    }
    return not_modified;
}

} // namespace

void apply_conditional_request(const Request& request, Response& response) {
    const bool is_retrieval = request.method() == Method::Get || request.method() == Method::Head;
    if (!is_retrieval || response.status() != Status::Ok || response.is_stream())
        return;

    const std::optional<std::string_view> etag = response.find_header(kEtagField);
    const std::optional<std::string_view> condition = request.header(kIfNoneMatchField);
    if (etag.has_value() && !etag->empty() && condition.has_value() &&
        if_none_match_matches(*condition, *etag))
        response = not_modified_from(response);
}

EncodedResponse encode_response(const Request& request, const Response& response,
                                const ResponseEncodingOptions& options, Buffer& head,
                                std::string& compressed_body) {
    const HeaderMap& headers = response.headers();
    const Status status = response.status();
    const bool streaming = response.is_stream();
    const std::string_view handler_body = streaming ? std::string_view{} : response.body();
    const std::string_view content_type =
        response.find_header(kContentTypeField).value_or(std::string_view{});
    const std::string_view content_encoding =
        response.find_header(kContentEncodingField).value_or(std::string_view{});

    const bool varies_on_accept_encoding =
        !is_bodiless(status) &&
        response_varies_on_accept_encoding(content_type, content_encoding, handler_body.size());
    const bool compressed = varies_on_accept_encoding && request.wants_gzip() &&
                            gzip_compress(handler_body, compressed_body);
    const std::string_view body = compressed ? std::string_view(compressed_body) : handler_body;
    const bool suppress_body =
        streaming || request.method() == Method::Head || is_bodiless(status);

    head.clear();
    head.append("HTTP/1.1 ");
    append_number(head, static_cast<uint64_t>(status_code(status)));
    head.append(" ");
    head.append(reason_phrase(status));
    head.append(kCrLf);

    for (const auto& [name, value] : headers) {
        if (is_framing_field(name))
            continue;
        if (varies_on_accept_encoding && equals_ignore_case(name, kVaryField))
            continue;
        append_field(head, name, value);
    }

    if (compressed)
        append_field(head, kContentEncodingField, kGzipCoding);
    if (varies_on_accept_encoding) {
        const std::string_view declared_vary =
            response.find_header(kVaryField).value_or(std::string_view{});
        head.append(kVaryField);
        head.append(": ");
        append_accept_encoding_vary(declared_vary,
                                    [&head](std::string_view part) { head.append(part); });
        head.append(kCrLf);
    }

    if (!response.find_header(kDateField).has_value())
        append_field(head, kDateField, http_date(std::chrono::system_clock::now()));
    if (!options.server_header.empty() && !has_control_character(options.server_header) &&
        !response.find_header("Server").has_value())
        append_field(head, "Server", options.server_header);
    if (!response.find_header(kCacheControlField).has_value())
        append_field(head, kCacheControlField, "no-store");
    if (!response.find_header("X-Content-Type-Options").has_value())
        append_field(head, "X-Content-Type-Options", "nosniff");
    if (!response.find_header("Referrer-Policy").has_value())
        append_field(head, "Referrer-Policy", "no-referrer");
    if (content_type.starts_with(kHtmlContentTypePrefix) &&
        !response.find_header("Content-Security-Policy").has_value())
        append_field(head, "Content-Security-Policy", kHtmlContentSecurityPolicy);
    if (options.secure && options.strict_transport_security &&
        !response.find_header("Strict-Transport-Security").has_value())
        append_field(head, "Strict-Transport-Security",
                     std::format("max-age={}", options.hsts_max_age_seconds));

    if (streaming) {
        append_field(head, kConnectionField, kCloseToken);
    } else if (is_bodiless(status)) {
        append_field(head, kConnectionField,
                     options.keep_alive ? kKeepAliveToken : kCloseToken);
    } else {
        head.append(kContentLengthField);
        head.append(": ");
        append_number(head, body.size());
        head.append(kCrLf);
        append_field(head, kConnectionField,
                     options.keep_alive ? kKeepAliveToken : kCloseToken);
    }
    head.append(kCrLf);
    return EncodedResponse{.body = body, .suppress_body = suppress_body};
}

} // namespace erikslund::http::internal
