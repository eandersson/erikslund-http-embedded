#include "erikslund/http/response.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/text.hpp"

namespace erikslund::http {
namespace {

constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kEtagField = "ETag";
constexpr std::string_view kLocationField = "Location";

constexpr std::string_view kPlainTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kJsonContentType = "application/json";

constexpr std::string_view kPrometheusContentType = "text/plain; version=0.0.4; charset=utf-8";

constexpr std::string_view kNoStoreCacheControl = "no-store";

constexpr char kHorizontalTab = '\t';
constexpr unsigned char kSpaceByte = 0x20;
constexpr unsigned char kDeleteByte = 0x7F;

constexpr int64_t kNoCacheWindowSeconds = 0;

constexpr size_t kMaxLoggedFieldNameBytes = 64;

constexpr unsigned kMaxLoggedFieldDrops = 16;

constexpr std::string_view kInvalidNameReason =
    "the name is empty or carries a character outside the RFC 9110 token set";
constexpr std::string_view kInvalidValueReason =
    "the value carries a control character, which would split the response";
constexpr std::string_view kEmptyLocationReason =
    "the redirect target is empty, and an empty Location is one no client can act on";

constexpr std::string_view kTokenSpecialCharacters = "!#$%&'*+-.^_`|~";

[[nodiscard]] bool is_valid_field_name(std::string_view name) noexcept {
    if (name.empty())
        return false;
    for (const char character : name) {
        const bool is_alphanumeric = (character >= 'a' && character <= 'z') ||
                                     (character >= 'A' && character <= 'Z') ||
                                     (character >= '0' && character <= '9');
        if (!is_alphanumeric && !kTokenSpecialCharacters.contains(character))
            return false;
    }
    return true;
}

[[nodiscard]] bool is_valid_field_value(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character != kHorizontalTab && (byte < kSpaceByte || byte == kDeleteByte))
            return false;
    }
    return true;
}

const LogSink& response_log_sink() {
    static const LogSink sink = stderr_log_sink();
    return sink;
}

void log_dropped_field(std::string_view reason, std::string_view name) noexcept {
    static std::atomic<unsigned> dropped_fields_logged{0};
    if (dropped_fields_logged.fetch_add(1, std::memory_order_relaxed) >= kMaxLoggedFieldDrops)
        return;
    try {
        response_log_sink()(LogLevel::Warning,
                            std::format("response: dropped the field \"{}\": {}",
                                        json_escape(name.substr(0, kMaxLoggedFieldNameBytes)),
                                        reason));
    } catch (...) {
        return;
    }
}

} // namespace

Response::Response() = default;

Response Response::text(std::string body, Status status) {
    Response response;
    response.status_ = status;
    response.body_.emplace<std::string>(std::move(body));
    response.set_header(std::string(kContentTypeField), std::string(kPlainTextContentType));
    return response;
}

Response Response::html(std::string body, Status status) {
    Response response;
    response.status_ = status;
    response.body_.emplace<std::string>(std::move(body));
    response.set_header(std::string(kContentTypeField), std::string(kHtmlContentType));
    return response;
}

Response Response::json(std::string body, Status status) {
    Response response;
    response.status_ = status;
    response.body_.emplace<std::string>(std::move(body));
    response.set_header(std::string(kContentTypeField), std::string(kJsonContentType));
    return response;
}

Response Response::prometheus(std::string body) {
    Response response;
    response.status_ = Status::Ok;
    response.body_.emplace<std::string>(std::move(body));
    response.set_header(std::string(kContentTypeField), std::string(kPrometheusContentType));
    return response;
}

Response Response::borrowed(std::string_view body, std::string_view content_type, Status status) {
    Response response;
    response.status_ = status;
    response.body_.emplace<std::string_view>(body);
    if (!content_type.empty())
        response.set_header(std::string(kContentTypeField), std::string(content_type));
    return response;
}

Response Response::empty(Status status) {
    Response response;
    response.status_ = status;
    return response;
}

Response Response::redirect(std::string location, Status status) {
    Response response;
    response.status_ = status;
    if (location.empty()) {
        log_dropped_field(kEmptyLocationReason, kLocationField);
        return response;
    }
    response.set_header(std::string(kLocationField), std::move(location));
    return response;
}

Response Response::stream(std::shared_ptr<StreamSource> source) {
    ERIKSLUND_HTTP_ASSERT(source != nullptr);
    Response response;
    response.status_ = Status::Ok;
    if (source == nullptr)
        return response;

    const std::string_view content_type = source->content_type();
    if (!content_type.empty())
        response.set_header(std::string(kContentTypeField), std::string(content_type));

    response.set_header(std::string(kCacheControlField), std::string(kNoStoreCacheControl));
    response.stream_ = std::move(source);
    return response;
}

std::string_view Response::body() const noexcept {
    if (const std::string* owned = std::get_if<std::string>(&body_))
        return *owned;
    if (const std::string_view* borrowed_bytes = std::get_if<std::string_view>(&body_))
        return *borrowed_bytes;
    return {};
}

std::optional<std::string_view> Response::find_header(std::string_view name) const noexcept {
    for (const auto& [field_name, field_value] : headers_)
        if (equals_ignore_case(field_name, name))
            return field_value;
    return std::nullopt;
}

void Response::set_header(std::string name, std::string value) {
    if (!is_valid_field_name(name)) {
        log_dropped_field(kInvalidNameReason, name);
        return;
    }
    if (!is_valid_field_value(value)) {
        log_dropped_field(kInvalidValueReason, name);
        return;
    }

    for (const std::string& existing_name : headers_.keys()) {
        if (!equals_ignore_case(existing_name, name))
            continue;
        std::string canonical_name = existing_name;
        headers_.insert_or_assign(std::move(canonical_name), std::move(value));
        return;
    }
    headers_.insert_or_assign(std::move(name), std::move(value));
}

void Response::apply_etag_from_body() {
    if (is_stream())
        return;
    set_header(std::string(kEtagField), weak_etag(body()));
}

void Response::apply_cache_for(std::chrono::seconds max_age) {
    const int64_t seconds = std::max<int64_t>(max_age.count(), kNoCacheWindowSeconds);

    set_header(std::string(kCacheControlField), std::format("max-age={}", seconds));
}

} // namespace erikslund::http
