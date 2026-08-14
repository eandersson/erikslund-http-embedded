#pragma once
// Typed JSON through Glaze. Include only where serialization is needed.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/status.hpp"

#include <glaze/glaze.hpp>
#ifdef ERIKSLUND_HTTP_JSON_SCHEMA
#include <glaze/json/schema.hpp>
#endif

namespace erikslund::http {

enum class JsonError : uint8_t {
    NotJson,
    Malformed,
    TypeMismatch,
    TooLarge,
};

[[nodiscard]] std::string_view json_error_message(JsonError error) noexcept;

[[nodiscard]] Response json_error_response(JsonError error);

[[nodiscard]] std::expected<std::string_view, JsonError> json_body_of(const Request& request,
                                                                      size_t max_bytes);

inline constexpr size_t kDefaultMaxJsonBodyBytes = 262'144;

template <class T>
[[nodiscard]] Response json_response(const T& value, Status status = Status::Ok) {
    std::string buffer;
    const auto error = glz::write_json(value, buffer);
    if (error)
        return Response::json(R"({"error":"serialization failed"})",
                              Status::InternalServerError);
    return Response::json(std::move(buffer), status);
}

template <class T>
[[nodiscard]] Response json_response_pretty(const T& value, Status status = Status::Ok) {
    std::string buffer;
    const auto error = glz::write<glz::opts{.prettify = true}>(value, buffer);
    if (error)
        return Response::json(R"({"error":"serialization failed"})",
                              Status::InternalServerError);
    return Response::json(std::move(buffer), status);
}

// Glaze does not distinguish malformed JSON from a typed-shape mismatch. Validate again only on
// failure to classify it.
[[nodiscard]] inline JsonError json_read_failure_kind(std::string_view body) noexcept {
    return glz::validate_json(body) ? JsonError::Malformed : JsonError::TypeMismatch;
}

template <class T>
[[nodiscard]] std::expected<T, JsonError> parse_json_body(
    const Request& request, size_t max_bytes = kDefaultMaxJsonBodyBytes) {
    const auto body = json_body_of(request, max_bytes);
    if (!body)
        return std::unexpected(body.error());
    T value{};
    const auto error = glz::read_json(value, *body);
    if (error)
        return std::unexpected(json_read_failure_kind(*body));
    return value;
}

template <class Producer>
Router& Router::json_get(std::string_view pattern, Producer producer) {
    return get(pattern, [produce = std::move(producer)](const Request&) -> Response {
        return json_response(produce());
    });
}

#ifdef ERIKSLUND_HTTP_JSON_SCHEMA
// Opt-in because Glaze's schema header is expensive to compile.
template <class T>
[[nodiscard]] std::string json_schema_for() {
    return glz::write_json_schema<T>().value_or(std::string{});
}
#endif

} // namespace erikslund::http
