#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace erikslund::http {

// Keep this closed set in sync with kAllStatuses.
enum class Status : uint16_t {
    Ok = 200,
    NoContent = 204,
    MovedPermanently = 301,
    Found = 302,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    RequestTimeout = 408,
    LengthRequired = 411,
    ContentTooLarge = 413,
    UriTooLong = 414,
    MisdirectedRequest = 421,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
};

inline constexpr std::array<Status, 21> kAllStatuses{
    Status::Ok,
    Status::NoContent,
    Status::MovedPermanently,
    Status::Found,
    Status::NotModified,
    Status::BadRequest,
    Status::Unauthorized,
    Status::Forbidden,
    Status::NotFound,
    Status::MethodNotAllowed,
    Status::NotAcceptable,
    Status::RequestTimeout,
    Status::LengthRequired,
    Status::ContentTooLarge,
    Status::UriTooLong,
    Status::MisdirectedRequest,
    Status::TooManyRequests,
    Status::RequestHeaderFieldsTooLarge,
    Status::InternalServerError,
    Status::NotImplemented,
    Status::ServiceUnavailable};

[[nodiscard]] constexpr int status_code(Status status) noexcept {
    return static_cast<int>(static_cast<uint16_t>(status));
}

[[nodiscard]] constexpr std::string_view reason_phrase(Status status) noexcept {
    switch (status) {
    case Status::Ok:
        return "OK";
    case Status::NoContent:
        return "No Content";
    case Status::MovedPermanently:
        return "Moved Permanently";
    case Status::Found:
        return "Found";
    case Status::NotModified:
        return "Not Modified";
    case Status::BadRequest:
        return "Bad Request";
    case Status::Unauthorized:
        return "Unauthorized";
    case Status::Forbidden:
        return "Forbidden";
    case Status::NotFound:
        return "Not Found";
    case Status::MethodNotAllowed:
        return "Method Not Allowed";
    case Status::NotAcceptable:
        return "Not Acceptable";
    case Status::RequestTimeout:
        return "Request Timeout";
    case Status::LengthRequired:
        return "Length Required";
    case Status::ContentTooLarge:
        return "Content Too Large";
    case Status::UriTooLong:
        return "URI Too Long";
    case Status::MisdirectedRequest:
        return "Misdirected Request";
    case Status::TooManyRequests:
        return "Too Many Requests";
    case Status::RequestHeaderFieldsTooLarge:
        return "Request Header Fields Too Large";
    case Status::InternalServerError:
        return "Internal Server Error";
    case Status::NotImplemented:
        return "Not Implemented";
    case Status::ServiceUnavailable:
        return "Service Unavailable";
    }
    return {};
}

// Fixed classes bound request-metric cardinality.
[[nodiscard]] constexpr std::string_view status_class(Status status) noexcept {
    const int code = status_code(status);
    if (code < 200)
        return "1xx";
    if (code < 300)
        return "2xx";
    if (code < 400)
        return "3xx";
    if (code < 500)
        return "4xx";
    return "5xx";
}

// These statuses suppress both the body and Content-Length.
[[nodiscard]] constexpr bool is_bodiless(Status status) noexcept {
    return status == Status::NoContent || status == Status::NotModified;
}

static_assert([] {
    for (const Status status : kAllStatuses)
        if (reason_phrase(status).empty())
            return false;
    return true;
}(), "every Status enumerator needs a reason phrase");

static_assert(status_code(Status::Ok) == 200);
static_assert(status_class(Status::ServiceUnavailable) == "5xx");

} // namespace erikslund::http
