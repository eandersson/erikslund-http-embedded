#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "internal/buffer.hpp"

namespace erikslund::http::internal {

struct ResponseEncodingOptions {
    std::string_view server_header;
    bool keep_alive = false;
    bool secure = false;
    bool strict_transport_security = false;
    uint64_t hsts_max_age_seconds = 0;
};

struct EncodedResponse {
    std::string_view body;
    bool suppress_body = false;
};

void apply_conditional_request(const Request& request, Response& response);

// The only response-framing implementation.
[[nodiscard]] EncodedResponse encode_response(const Request& request, const Response& response,
                                              const ResponseEncodingOptions& options, Buffer& head,
                                              std::string& compressed_body);

} // namespace erikslund::http::internal
