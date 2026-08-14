
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::HttpResponse;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TestServer;

constexpr std::string_view kProbeField = "X-Probe";
constexpr std::string_view kProbeValue = "ordinary";
constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kEchoField = "X-Echo";
constexpr std::string_view kInjectedField = "X-Injected";
constexpr std::string_view kPlainTextPrefix = "text/plain";
constexpr std::string_view kBody = "ok\n";

constexpr std::string_view kEchoPath = "/echo";
constexpr std::string_view kInjectPath = "/inject";
constexpr std::string_view kStreamPath = "/stream";
constexpr std::string_view kEchoParameter = "v";

constexpr std::string_view kSplittingContentType = "text/plain\r\nX-Injected: yes";

constexpr std::string_view kHarmlessTarget = "/echo?v=harmless";
constexpr std::string_view kHarmlessValue = "harmless";

constexpr std::string_view kSplittingTarget = "/echo?v=a%0d%0aX-Injected:%20yes";

constexpr std::string_view kHandlerSuppliedSplit = "a\r\nX-Injected: yes";

constexpr int kOkStatus = 200;
constexpr int kBadRequestStatus = 400;

constexpr size_t kNoFields = 0;
constexpr size_t kExactlyOneField = 1;

constexpr std::chrono::seconds kNegativeCacheWindow{-1};
constexpr std::string_view kZeroCacheDirective = "max-age=0";

constexpr std::array<std::string_view, 8> kRejectedValues{
    "a\rb", "a\nb", "a\r\nX-Injected: yes", "\r\n\r\n<html>",
    "a\vb", "a\fb", "a\x1b" "b",           "a\x7f" "b",
};

constexpr std::array<std::string_view, 8> kRejectedNames{
    "X-Bad\r\nX-Injected", "X Bad",     "X:Bad",   "X-Bad\t",
    "X-Bad,Worse",         "X-\x7f",    "X-Bad\n", "",
};

[[nodiscard]] std::string_view header_value(const Response& response, std::string_view name) {
    const std::string key(name);
    if (!response.headers().contains(key))
        return {};
    return response.headers().at(key);
}

[[nodiscard]] Response probe_response() {
    Response response = Response::text(std::string(kBody));
    response.header(std::string(kProbeField), std::string(kProbeValue));
    return response;
}

[[nodiscard]] Router echoing_router() {
    Router router;
    router.get(kEchoPath, [](const Request& request) {
        Response response = Response::text(std::string(kBody));
        if (const std::optional<std::string_view> echoed = request.query_param(kEchoParameter))
            response.header(std::string(kEchoField), std::string(*echoed));
        return response;
    });
    router.get(kInjectPath, [](const Request&) {
        Response response = Response::text(std::string(kBody));
        response.header(std::string(kEchoField), std::string(kHandlerSuppliedSplit));
        return response;
    });
    return router;
}

class SplittingContentTypeStream final : public StreamSource {
public:
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kSplittingContentType;
    }

    [[nodiscard]] Pull pull(std::string&) override { return Pull::Idle; }

    void on_attached(std::shared_ptr<StreamNotifier>) override {}

    void on_detached() noexcept override {}
};

[[nodiscard]] Router streaming_router() {
    Router router;
    router.get(kStreamPath, [](const Request&) {
        return Response::stream(std::make_shared<SplittingContentTypeStream>());
    });
    return router;
}

[[nodiscard]] std::optional<HttpResponse> one_request(const TestServer& fixture,
                                                      std::string_view target) {
    TestClient client;
    if (!client.connect(fixture.port()))
        return std::nullopt;
    if (!client.send_raw(simple_request("GET", target)))
        return std::nullopt;
    return client.read_response();
}

} // namespace

TEST_CASE("a_control_character_anywhere_in_a_field_value_drops_the_field_and_nothing_else") {
    for (const std::string_view hostile : kRejectedValues) {
        Response response = probe_response();
        response.header(std::string(kEchoField), std::string(hostile));

        CHECK_MESSAGE(!response.headers().contains(std::string(kEchoField)),
                      "a value carrying a control character was written: ", hostile);
        CHECK(header_value(response, kProbeField) == kProbeValue);
        CHECK(header_value(response, kContentTypeField).starts_with(kPlainTextPrefix));
    }
}

TEST_CASE("a_field_name_outside_the_token_set_drops_the_field_and_nothing_else") {
    for (const std::string_view hostile : kRejectedNames) {
        Response response = probe_response();
        response.header(std::string(hostile), std::string(kProbeValue));

        CHECK_MESSAGE(!response.headers().contains(std::string(hostile)),
                      "a name outside the token set was written: ", hostile);
        CHECK(header_value(response, kProbeField) == kProbeValue);
        CHECK(header_value(response, kContentTypeField).starts_with(kPlainTextPrefix));
    }
}

TEST_CASE("a_rejected_value_never_clears_the_field_it_failed_to_replace") {
    Response response = probe_response();
    response.header(std::string(kProbeField), std::string(kHandlerSuppliedSplit));
    CHECK(header_value(response, kProbeField) == kProbeValue);
    CHECK(header_value(response, kContentTypeField).starts_with(kPlainTextPrefix));
}

TEST_CASE("an_empty_redirect_target_produces_a_response_with_no_location_field") {
    const Response response = Response::redirect(std::string());
    CHECK(response.status() == Status::Found);
    CHECK(response.headers().size() == kNoFields);
}

TEST_CASE("a_negative_cache_window_becomes_a_zero_second_one") {
    const Response response = Response::text(std::string(kBody)).cache_for(kNegativeCacheWindow);
    CHECK(header_value(response, kCacheControlField) == kZeroCacheDirective);
}

TEST_CASE("a_handler_echoing_a_query_parameter_cannot_split_the_response_or_stop_the_server") {
    const auto fixture = started_test_server(echoing_router());

    const std::optional<HttpResponse> control = one_request(*fixture, kHarmlessTarget);
    REQUIRE(control.has_value());
    CHECK(control->status_code == kOkStatus);
    CHECK(control->header_value(kEchoField) == kHarmlessValue);

    const std::optional<HttpResponse> split = one_request(*fixture, kSplittingTarget);
    REQUIRE_MESSAGE(split.has_value(), "the server stopped answering after the encoded CRLF");
    CHECK(split->status_code == kBadRequestStatus);
    CHECK(split->header_count(kInjectedField) == kNoFields);
    CHECK(split->raw_head.find(kInjectedField) == std::string::npos);

    const std::optional<HttpResponse> handler_split = one_request(*fixture, kInjectPath);
    REQUIRE_MESSAGE(handler_split.has_value(), "the server stopped answering after a handler CRLF");
    CHECK(handler_split->status_code == kOkStatus);
    CHECK(handler_split->header_count(kEchoField) == kNoFields);
    CHECK(handler_split->header_count(kInjectedField) == kNoFields);
    CHECK(handler_split->raw_head.find(kInjectedField) == std::string::npos);
    CHECK(handler_split->body == kBody);

    const std::optional<HttpResponse> after = one_request(*fixture, kHarmlessTarget);
    REQUIRE(after.has_value());
    CHECK(after->status_code == kOkStatus);
    CHECK(after->header_count(kEchoField) == kExactlyOneField);
    CHECK(after->header_value(kEchoField) == kHarmlessValue);
}

TEST_CASE("a_stream_source_naming_a_split_media_type_puts_no_second_field_on_the_wire") {
    const auto fixture = started_test_server(streaming_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", kStreamPath)));

    const std::optional<HttpResponse> head = client.read_head();
    REQUIRE_MESSAGE(head.has_value(), "the server never answered the stream request");
    CHECK(head->status_code == kOkStatus);
    CHECK_MESSAGE(head->header_count(kInjectedField) == kNoFields,
                  "the media type a source named became a second header field");
    CHECK(head->raw_head.find(kInjectedField) == std::string::npos);
    CHECK_MESSAGE(head->header_count(kContentTypeField) == kNoFields,
                  "a media type that cannot be written truthfully is better omitted than repaired");
}

TEST_CASE("server_start_refuses_a_programmatic_server_header_that_could_split_a_response") {
    ServerOptions options = test::loopback_options();
    options.server_header = "erikslund-http\r\nX-Injected: yes";
    TestServer fixture(echoing_router(), std::move(options));

    CHECK_THROWS_AS(fixture.start(), ServerError);
}

} // namespace erikslund::http
