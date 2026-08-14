#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "internal/buffer.hpp"
#include "internal/response_encoder.hpp"

namespace erikslund::http::internal {

TEST_CASE("the_response_encoder_drops_an_invalid_server_header_even_without_startup_validation") {
    const std::string wire = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
    auto parsed = parse_request(wire, RequestLimits{});
    REQUIRE(parsed.has_value());

    Buffer head;
    std::string compressed_body;
    const Response response = Response::text("ok\n");
    const ResponseEncodingOptions options{
        .server_header = "erikslund-http\r\nX-Injected: yes",
    };

    static_cast<void>(encode_response(parsed->request, response, options, head, compressed_body));

    CHECK_FALSE(head.readable().contains("\r\nServer:"));
    CHECK_FALSE(head.readable().contains("\r\nX-Injected:"));
}

} // namespace erikslund::http::internal
