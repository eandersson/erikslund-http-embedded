#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "erikslund/http/method.hpp"
#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/status.hpp"

namespace erikslund::http {

// Excess headers are rejected, never truncated.
inline constexpr size_t kMaxParsedHeaders = 64;

inline constexpr size_t kMaxPathParameters = 8;

inline constexpr size_t kDefaultMaxRequestLineBytes = 8'192;
inline constexpr size_t kDefaultMaxHeaderBlockBytes = 16'384;
inline constexpr size_t kDefaultMaxTargetBytes = 2'048;
inline constexpr size_t kDefaultMaxBodyBytes = 1'048'576;

struct HeaderView {
    std::string_view name{};
    std::string_view value{};
};

struct RequestLimits {
    size_t max_request_line_bytes = kDefaultMaxRequestLineBytes;
    size_t max_header_block_bytes = kDefaultMaxHeaderBlockBytes;
    size_t max_header_count = kMaxParsedHeaders;
    size_t max_target_bytes = kDefaultMaxTargetBytes;
    size_t max_body_bytes = kDefaultMaxBodyBytes;
};

struct ParsedRequest;

namespace internal {
class RequestParser;
}

enum class ParseError : uint8_t {
    NeedsMoreData,
    MalformedRequestLine,
    UnsupportedVersion,
    UnknownMethod,
    TooManyHeaders,
    MalformedHeader,
    MissingHost,
    MalformedHost,
    TargetTooLong,
    BadPercentEncoding,
    UnsupportedTransferEncoding,
    BodyTooLarge,
};

[[nodiscard]] constexpr Status status_for(ParseError error) noexcept {
    switch (error) {
    case ParseError::TooManyHeaders:
        return Status::RequestHeaderFieldsTooLarge;
    case ParseError::TargetTooLong:
        return Status::UriTooLong;
    case ParseError::BodyTooLarge:
        return Status::ContentTooLarge;
    case ParseError::UnsupportedTransferEncoding:
        return Status::NotImplemented;
    case ParseError::UnsupportedVersion:
        return Status::MisdirectedRequest;
    case ParseError::NeedsMoreData:
    case ParseError::MalformedRequestLine:
    case ParseError::UnknownMethod:
    case ParseError::MalformedHeader:
    case ParseError::MissingHost:
    case ParseError::MalformedHost:
    case ParseError::BadPercentEncoding:
        return Status::BadRequest;
    }
    return Status::BadRequest;
}

// Accessors borrow the current connection read buffer. Copy decoded values that must outlive the
// request.
class Request {
public:
    Request();
    ~Request();
    Request(const Request&) = delete("a Request views the connection's read buffer; copy the "
                                     "decoded values you need instead");
    Request& operator=(const Request&) = delete("a Request views the connection's read buffer");
    Request(Request&&) noexcept;
    Request& operator=(Request&&) noexcept;

    [[nodiscard]] Method method() const noexcept { return method_; }

    // Original, undecoded request target.
    [[nodiscard]] std::string_view raw_target() const noexcept { return raw_target_; }

    // Percent-decoded and normalized. Always starts with '/', contains no escapes or NUL, and
    // cannot traverse above root.
    [[nodiscard]] std::string_view path() const noexcept { return path_; }

    // Undecoded text after '?', excluding the separator.
    [[nodiscard]] std::string_view query() const noexcept { return query_; }

    // Validated host and optional port. Absolute-form targets take precedence over Host. Empty only
    // for HTTP/1.0 requests that provide neither.
    [[nodiscard]] std::string_view authority() const noexcept { return authority_; }

    // Percent-decoded and free of control bytes. An empty value differs from an absent key.
    [[nodiscard]] std::optional<std::string_view> query_param(std::string_view name) const;

    // Case-insensitive; the first occurrence wins. Prefer authority() over header("Host").
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

    // Returns an empty view for an unbound name.
    [[nodiscard]] std::string_view param(std::string_view name) const noexcept;

    [[nodiscard]] std::string_view body() const noexcept { return body_; }

    [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }
    [[nodiscard]] bool wants_gzip() const noexcept { return wants_gzip_; }
    [[nodiscard]] bool is_secure() const noexcept { return is_secure_; }

    // Present only after successful client-certificate verification.
    [[nodiscard]] std::optional<std::string_view> client_certificate_subject() const;

    [[nodiscard]] const PeerAddress& peer() const noexcept { return peer_; }

    [[nodiscard]] std::chrono::steady_clock::time_point received_at() const noexcept {
        return received_at_;
    }

    [[nodiscard]] std::span<const HeaderView> headers() const noexcept { return headers_; }

    void set_peer(const PeerAddress& peer) noexcept;
    void set_secure(bool secure) noexcept;
    void set_client_certificate_subject(std::string subject);
    void set_received_at(std::chrono::steady_clock::time_point when) noexcept;

    void bind_path_parameter(std::string_view name, std::string_view value);
    void clear_path_parameters() noexcept;

    // Keeps decode-arena capacity for the next keep-alive request.
    void reset() noexcept;

private:
    friend class internal::RequestParser;

    Method method_ = Method::None;
    std::string_view raw_target_;
    std::string_view path_;
    std::string_view query_;
    std::string_view authority_;
    std::string_view body_;
    std::inplace_vector<HeaderView, kMaxParsedHeaders> headers_;

    // Stable allocation keeps decoded views valid when Request moves.
    std::unique_ptr<std::string> decode_arena_;

    std::inplace_vector<HeaderView, kMaxParsedHeaders> query_parameters_;
    std::inplace_vector<HeaderView, kMaxPathParameters> path_parameters_;

    PeerAddress peer_;
    std::chrono::steady_clock::time_point received_at_{};
    std::string client_certificate_subject_;
    bool keep_alive_ = false;
    bool wants_gzip_ = false;
    bool is_secure_ = false;
    bool has_client_certificate_ = false;
};

struct ParsedRequest {
    Request request;
    // Request line, headers, and body.
    size_t consumed_bytes = 0;
    size_t body_offset = 0;
};

// Returned Request views borrow bytes. NeedsMoreData means the prefix is valid.
// Transfer-Encoding, ambiguous Content-Length, and ambiguous authority are rejected to keep every
// HTTP hop in agreement.
[[nodiscard]] std::expected<ParsedRequest, ParseError> parse_request(std::string_view bytes,
                                                                     const RequestLimits& limits);

} // namespace erikslund::http
