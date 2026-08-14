#include "erikslund/http/json.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/text.hpp"


namespace erikslund::http {

namespace {

constexpr std::string_view kJsonMediaType = "application/json";

constexpr std::string_view kJsonStructuredSuffix = "+json";

constexpr std::string_view kOptionalWhitespace = " \t";

[[nodiscard]] constexpr std::string_view trim_optional_whitespace(std::string_view text) noexcept {
    const size_t first = text.find_first_not_of(kOptionalWhitespace);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(kOptionalWhitespace) - first + 1);
}

[[nodiscard]] bool is_json_media_type(std::string_view content_type) noexcept {
    const std::string_view media_type =
        trim_optional_whitespace(content_type.substr(0, content_type.find(';')));
    if (equals_ignore_case(media_type, kJsonMediaType))
        return true;
    if (media_type.size() <= kJsonStructuredSuffix.size())
        return false;
    return equals_ignore_case(media_type.substr(media_type.size() - kJsonStructuredSuffix.size()),
                              kJsonStructuredSuffix);
}

} // namespace

std::string_view json_error_message(JsonError error) noexcept {
    switch (error) {
    case JsonError::NotJson:
        return "expected a request body with Content-Type: application/json";
    case JsonError::Malformed:
        return "request body is not well-formed JSON";
    case JsonError::TypeMismatch:
        return "request body does not match the expected field types";
    case JsonError::TooLarge:
        return "request body exceeds the JSON size limit for this route";
    }
    return "invalid JSON request body";
}

Response json_error_response(JsonError error) {
    const Status status =
        error == JsonError::TooLarge ? Status::ContentTooLarge : Status::BadRequest;

    std::string body = R"({"error":")";
    body += json_escape(json_error_message(error));
    body += R"("})";
    return Response::json(std::move(body), status);
}

std::expected<std::string_view, JsonError> json_body_of(const Request& request, size_t max_bytes) {
    ERIKSLUND_HTTP_ASSERT(max_bytes > 0);

    const auto content_type = request.header("Content-Type");
    if (!content_type || !is_json_media_type(*content_type))
        return std::unexpected(JsonError::NotJson);

    const std::string_view body = request.body();

    if (body.size() > max_bytes)
        return std::unexpected(JsonError::TooLarge);

    if (body.empty())
        return std::unexpected(JsonError::NotJson);

    ERIKSLUND_HTTP_ASSERT(body.size() <= max_bytes);
    return body;
}

} // namespace erikslund::http
