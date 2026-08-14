
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/sse.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::BodyExpectation;
using test::CapturedLog;
using test::HttpResponse;
using test::loopback_options;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TestServer;
using test::wait_until;

constexpr std::string_view kHelloBody = "hello, world\n";
constexpr std::string_view kHtmlBody = "<!doctype html><title>operator</title>\n";
constexpr std::string_view kConditionalBody = "conditional payload\n";
constexpr std::string_view kNotFoundBody = "Not Found\n";
constexpr std::string_view kEchoedBody = "hello";
constexpr std::string_view kPlainTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kConditionalCacheControl = "public, max-age=60";
constexpr std::string_view kConditionalContentLocation = "/etagged";
constexpr std::string_view kConditionalDate = "Thu, 13 Aug 2026 11:59:00 GMT";
constexpr std::string_view kConditionalExpires = "Thu, 13 Aug 2026 12:00:00 GMT";
constexpr std::string_view kConditionalVary = "Accept-Language";

constexpr std::string_view kHandlerFieldValue = "kept";

constexpr std::string_view kExpectedContentSecurityPolicy =
    "default-src 'self'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'; "
    "form-action 'none'; object-src 'none'";

constexpr int kOkStatus = 200;
constexpr int kNotModifiedStatus = 304;
constexpr int kNotFoundStatus = 404;
constexpr int kMethodNotAllowedStatus = 405;

constexpr std::chrono::milliseconds kIdleObservationWindow{300};

constexpr std::chrono::milliseconds kInterWritePause{150};

constexpr std::string_view kEventsPath = "/events";
constexpr std::string_view kEventStreamContentType = "text/event-stream";
constexpr std::string_view kEventName = "status";
constexpr std::string_view kEventPayload = "READY";
constexpr std::string_view kExpectedEventFrame = "event: status\ndata: READY\n\n";
constexpr unsigned kSubscriberCount = 2;

constexpr std::chrono::milliseconds kStreamFrameBudget{5'000};
constexpr std::chrono::milliseconds kStreamReadSlice{100};

constexpr std::chrono::milliseconds kShutdownBound{3'000};

constexpr unsigned kKeepAliveRequestCount = 3;
constexpr unsigned kRequestCap = 3;

constexpr std::string_view kStreamMarker = "marker-bytes-no-head-may-receive\n";

constexpr std::string_view kMetricsPrefix = "erikslund_http";
constexpr std::string_view kActiveMetricName = "connections_active";
constexpr std::string_view kMetricsHelpText = "read back by the connection-accounting cases";

constexpr size_t kHeldConnectionCount = 5;

[[nodiscard]] Router make_test_router() {
    Router router;
    router.get("/hello", [](const Request&) { return Response::text(std::string(kHelloBody)); });
    router.get("/page", [](const Request&) { return Response::html(std::string(kHtmlBody)); });
    router.get("/etagged", [](const Request&) {
        Response answer = Response::text(std::string(kConditionalBody));
        answer.etag_from_body();
        answer.header("Cache-Control", std::string(kConditionalCacheControl));
        answer.header("Content-Location", std::string(kConditionalContentLocation));
        answer.header("Date", std::string(kConditionalDate));
        answer.header("Expires", std::string(kConditionalExpires));
        answer.header("Vary", std::string(kConditionalVary));
        return answer;
    });
    router.get("/secure", [](const Request& request) {
        return Response::text(request.is_secure() ? std::string("secure\n")
                                                  : std::string("plain\n"));
    });
    router.post("/echo",
                [](const Request& request) { return Response::text(std::string(request.body())); });
    router.get("/own-framing", [](const Request&) {
        Response answer = Response::text(std::string(kHelloBody));
        answer.header("Content-Length", "0");
        answer.header("Connection", "close");
        answer.header("Transfer-Encoding", "chunked");
        answer.header("X-Handler-Field", std::string(kHandlerFieldValue));
        return answer;
    });
    return router;
}

[[nodiscard]] std::vector<std::string> field_names_except(const HttpResponse& response,
                                                          std::string_view excluded) {
    std::vector<std::string> names;
    names.reserve(response.headers.size());
    for (const std::pair<std::string, std::string>& field : response.headers) {
        if (equals_ignore_case(field.first, excluded))
            continue;
        names.push_back(field.first);
    }
    std::ranges::sort(names);
    return names;
}

[[nodiscard]] bool logged_anything_at(const CapturedLog& log, LogLevel level) {
    return log.contains(level, std::string_view{});
}

[[nodiscard]] bool stream_delivers(TestClient& client, std::string_view needle) {
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + kStreamFrameBudget;
    while (std::chrono::steady_clock::now() < deadline) {
        received += client.read_some(kStreamReadSlice);
        if (received.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

[[nodiscard]] std::string joined_log(const CapturedLog& log) {
    std::string text;
    for (const std::string& line : log.lines()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

struct StreamLedger {
    std::atomic<int> handed_out{0};
    std::atomic<int> attached{0};
    std::atomic<int> released{0};
};

class MarkerStream final : public StreamSource {
public:
    explicit MarkerStream(StreamLedger& ledger) noexcept : ledger_(ledger) {}

    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string& out) override {
        if (written_)
            return Pull::Idle;
        written_ = true;
        out += kStreamMarker;
        return Pull::Wrote;
    }

    void on_attached(std::shared_ptr<StreamNotifier>) override {
        ledger_.attached.fetch_add(1, std::memory_order_relaxed);
    }

    void on_detached() noexcept override {
        ledger_.released.fetch_add(1, std::memory_order_relaxed);
    }

private:
    StreamLedger& ledger_;
    bool written_ = false;
};

enum class ThrowingCallback : uint8_t { Attach, Pull };

class ThrowingStream final : public StreamSource {
public:
    ThrowingStream(ThrowingCallback callback, std::atomic<int>& released) noexcept
        : callback_(callback), released_(released) {}

    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string&) override {
        if (callback_ == ThrowingCallback::Pull)
            throw 7;
        return Pull::Idle;
    }

    void on_attached(std::shared_ptr<StreamNotifier>) override {
        if (callback_ == ThrowingCallback::Attach)
            throw 7;
    }

    void on_detached() noexcept override {
        released_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    ThrowingCallback callback_;
    std::atomic<int>& released_;
};

} // namespace

TEST_CASE("serves_a_get_with_the_right_status_body_and_content_length") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE_MESSAGE(response.has_value(), "the server never returned a parseable response");
    REQUIRE(response->complete);

    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == kHelloBody);
    CHECK(response->header_value("Content-Type") == kPlainTextContentType);
    CHECK_MESSAGE(response->header_value("Content-Length") == std::to_string(kHelloBody.size()),
                  "Content-Length must count the bytes actually sent");
    CHECK_MESSAGE(response->header_count("Content-Length") == 1,
                  "a second Content-Length is a framing contradiction two hops resolve apart");
    CHECK(response->has_header("Date"));
    CHECK(response->header_value("Server") == kDefaultServerHeader);
}

TEST_CASE("emits_exactly_one_of_each_framing_field_even_when_the_handler_set_its_own") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("GET", "/own-framing"));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);

    CHECK_MESSAGE(response->header_count("Content-Length") == 1,
                  std::format("the head carried {} Content-Length lines:\n{}",
                              response->header_count("Content-Length"), response->raw_head));
    CHECK_MESSAGE(response->header_count("Connection") == 1,
                  std::format("the head carried {} Connection lines:\n{}",
                              response->header_count("Connection"), response->raw_head));
    CHECK(response->header_count("Transfer-Encoding") == 0);

    CHECK(response->header_value("Content-Length") == std::to_string(kHelloBody.size()));
    CHECK(response->body == kHelloBody);

    CHECK(response->header_value("X-Handler-Field") == kHandlerFieldValue);
}

TEST_CASE("a_cross_thread_publish_reaches_each_streaming_connection_over_its_own_socket") {
    SseChannel channel;

    Router router;
    router.get(kEventsPath, channel.handler());
    const auto fixture = started_test_server(std::move(router));

    TestClient first;
    TestClient second;
    REQUIRE(first.connect(fixture->port()));
    REQUIRE(second.connect(fixture->port()));
    REQUIRE(first.send_raw(simple_request("GET", kEventsPath)));
    REQUIRE(second.send_raw(simple_request("GET", kEventsPath)));

    const std::optional<HttpResponse> first_head = first.read_head();
    const std::optional<HttpResponse> second_head = second.read_head();
    REQUIRE(first_head.has_value());
    REQUIRE(second_head.has_value());
    CHECK(first_head->status_code == kOkStatus);
    CHECK(first_head->header_value("Content-Type") == kEventStreamContentType);
    CHECK(second_head->header_value("Content-Type") == kEventStreamContentType);

    REQUIRE(wait_until([&channel] { return channel.subscriber_count() == kSubscriberCount; }));

    channel.publish(kEventName, kEventPayload);

    CHECK_MESSAGE(stream_delivers(first, kExpectedEventFrame),
                  "the first subscriber never received the published frame");
    CHECK_MESSAGE(stream_delivers(second, kExpectedEventFrame),
                  "the second subscriber never received the published frame");
}

TEST_CASE("a_keep_alive_idle_tuned_down_for_slowloris_does_not_disconnect_a_healthy_subscriber") {
    SseChannel channel;

    Router router;
    router.get(kEventsPath, channel.handler());

    constexpr std::chrono::milliseconds kHardenedKeepAliveIdle{200};
    ServerOptions options = loopback_options();
    options.keep_alive_idle = kHardenedKeepAliveIdle;

    const auto fixture = started_test_server(std::move(router), std::move(options));

    TestClient subscriber;
    REQUIRE(subscriber.connect(fixture->port()));
    REQUIRE(subscriber.send_raw(simple_request("GET", kEventsPath)));
    const std::optional<HttpResponse> head = subscriber.read_head();
    REQUIRE(head.has_value());
    CHECK(head->status_code == kOkStatus);
    REQUIRE(wait_until([&channel] { return channel.subscriber_count() == 1; }));

    std::this_thread::sleep_for(kHardenedKeepAliveIdle * 8);

    CHECK_MESSAGE(channel.subscriber_count() == 1,
                  "the subscription was reclaimed on the keep-alive budget rather than its own");
    channel.publish(kEventName, kEventPayload);
    CHECK_MESSAGE(stream_delivers(subscriber, kExpectedEventFrame),
                  "a subscriber that outlived the keep-alive budget stopped receiving frames");
}

TEST_CASE("answers_head_with_the_same_headers_as_get_and_an_empty_body") {
    const auto fixture = started_test_server(make_test_router());

    TestClient get_client;
    REQUIRE(get_client.connect(fixture->port()));
    const std::optional<HttpResponse> from_get =
        get_client.request(simple_request("GET", "/hello"));
    REQUIRE(from_get.has_value());
    REQUIRE(from_get->complete);

    TestClient head_client;
    REQUIRE(head_client.connect(fixture->port()));
    const std::optional<HttpResponse> from_head = head_client.request(
        simple_request("HEAD", "/hello"), test::kDefaultResponseTimeout,
        BodyExpectation::HeadRequest);
    REQUIRE(from_head.has_value());

    CHECK(from_head->status_code == kOkStatus);
    CHECK_MESSAGE(from_head->body.empty(), "a HEAD response must carry no body at all");
    CHECK_MESSAGE(from_head->header_value("Content-Length") ==
                      from_get->header_value("Content-Length"),
                  "HEAD must report the length the matching GET would have sent");
    CHECK(field_names_except(*from_head, "Date") == field_names_except(*from_get, "Date"));
    CHECK_MESSAGE(head_client.buffered().empty(),
                  "no bytes may follow the header block of a HEAD response");
}

TEST_CASE("answers_an_unknown_path_with_404") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("GET", "/no-such-route"));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);
    CHECK(response->status_code == kNotFoundStatus);
    CHECK(response->body == kNotFoundBody);
    CHECK(response->header_value("Content-Length") == std::to_string(kNotFoundBody.size()));
}

TEST_CASE("answers_a_known_path_with_the_wrong_method_with_405_and_an_allow_header") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("DELETE", "/hello"));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);
    CHECK(response->status_code == kMethodNotAllowedStatus);
    CHECK(response->header_value("Allow") == "GET, HEAD");
}

TEST_CASE("serves_several_requests_on_one_keep_alive_connection_and_leaves_it_open") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    for (unsigned index = 0; index < kKeepAliveRequestCount; ++index) {
        REQUIRE(client.send_raw(simple_request("GET", "/hello")));
        const std::optional<HttpResponse> response = client.read_response();
        REQUIRE_MESSAGE(response.has_value(),
                        std::format("request {} on the reused connection went unanswered", index));
        CHECK(response->status_code == kOkStatus);
        CHECK(response->body == kHelloBody);
        CHECK(response->header_value("Connection") == "keep-alive");
    }

    CHECK_MESSAGE(!client.wait_for_close(kIdleObservationWindow),
                  "the connection must still be open after the last keep-alive response");
    CHECK_MESSAGE(client.buffered().empty(),
                  "an idle keep-alive connection must not be sent anything at all");
}

TEST_CASE("closes_the_connection_when_the_request_asked_for_connection_close") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("GET", "/hello", "Connection: close\r\n"));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);
    CHECK(response->status_code == kOkStatus);
    CHECK(response->header_value("Connection") == "close");
    CHECK_MESSAGE(client.wait_for_close(),
                  "the server must close after answering a Connection: close request");
}

TEST_CASE("answers_two_pipelined_requests_in_order_on_one_write") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", "/hello") + simple_request("GET", "/page")));

    const std::optional<HttpResponse> first = client.read_response();
    REQUIRE_MESSAGE(first.has_value(), "the first pipelined request went unanswered");
    const std::optional<HttpResponse> second = client.read_response();
    REQUIRE_MESSAGE(second.has_value(), "the second pipelined request went unanswered");

    CHECK(first->status_code == kOkStatus);
    CHECK(first->body == kHelloBody);
    CHECK_MESSAGE(second->body == kHtmlBody, "pipelined responses must arrive in request order");
}

TEST_CASE("sends_the_default_security_headers_on_every_response") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE(response.has_value());
    CHECK(response->header_value("X-Content-Type-Options") == "nosniff");
    CHECK(response->header_value("Referrer-Policy") == "no-referrer");
    CHECK_MESSAGE(response->header_value("Cache-Control") == "no-store",
                  "an operator surface serves live state; a cached answer is one acted on wrongly");
}

TEST_CASE("sends_the_content_security_policy_only_on_an_html_response") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> html = client.request(simple_request("GET", "/page"));
    REQUIRE(html.has_value());
    CHECK(html->header_value("Content-Type") == kHtmlContentType);
    CHECK(html->header_value("Content-Security-Policy") == kExpectedContentSecurityPolicy);

    const std::optional<HttpResponse> plain = client.request(simple_request("GET", "/hello"));
    REQUIRE(plain.has_value());
    CHECK_MESSAGE(!plain->has_header("Content-Security-Policy"),
                  "a CSP on a non-document response protects nothing and confuses caches");
}

TEST_CASE("answers_a_matching_if_none_match_with_304_and_no_body") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> full = client.request(simple_request("GET", "/etagged"));
    REQUIRE(full.has_value());
    CHECK(full->status_code == kOkStatus);
    CHECK(full->body == kConditionalBody);
    REQUIRE_MESSAGE(full->has_header("ETag"), "the route under test must publish a validator");
    const std::string etag(full->header_value("ETag"));

    const std::optional<HttpResponse> conditional = client.request(
        simple_request("GET", "/etagged", "If-None-Match: " + etag + "\r\n"));
    REQUIRE(conditional.has_value());
    CHECK(conditional->status_code == kNotModifiedStatus);
    CHECK_MESSAGE(conditional->body.empty(), "a 304 must carry no body");
    CHECK_MESSAGE(!conditional->has_header("Content-Length"),
                  "a 304 has no body, so a Content-Length would be a framing contradiction");
    CHECK_MESSAGE(conditional->header_value("ETag") == etag,
                  "the validator has to travel with the 304 or a cache cannot refresh its record");
    CHECK(conditional->header_value("Cache-Control") == kConditionalCacheControl);
    CHECK(conditional->header_value("Content-Location") == kConditionalContentLocation);
    CHECK(conditional->header_value("Date") == kConditionalDate);
    CHECK(conditional->header_value("Expires") == kConditionalExpires);
    CHECK_MESSAGE(conditional->header_value("Vary") == kConditionalVary,
                  "the 304 must preserve the cache key dimensions of its full response");
}

TEST_CASE("answers_a_non_matching_if_none_match_with_the_full_200_body") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(
        simple_request("GET", "/etagged", "If-None-Match: W/\"0000000000000000\"\r\n"));
    REQUIRE(response.has_value());
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == kConditionalBody);
}

TEST_CASE("a_matching_if_none_match_never_rewrites_an_unsafe_method_after_its_handler_runs") {
    unsigned calls = 0;
    Router router;
    router.post("/etagged-write", [&calls](const Request&) {
        ++calls;
        Response answer = Response::text("write completed\n");
        answer.etag("W/\"write-state\"");
        return answer;
    });
    const auto fixture = started_test_server(std::move(router));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request(
        "POST", "/etagged-write", "If-None-Match: W/\"write-state\"\r\n"));
    REQUIRE(response.has_value());
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == "write completed\n");
    CHECK_MESSAGE(calls == 1,
                  "an unsafe handler owns precondition evaluation before it mutates state");
}

TEST_CASE("stops_serving_a_connection_once_max_requests_per_connection_is_reached") {
    ServerOptions options = loopback_options();
    options.max_requests_per_connection = kRequestCap;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    for (unsigned index = 0; index + 1 < kRequestCap; ++index) {
        REQUIRE(client.send_raw(simple_request("GET", "/hello")));
        const std::optional<HttpResponse> response = client.read_response();
        REQUIRE_MESSAGE(response.has_value(),
                        std::format("request {} below the cap went unanswered", index));
        CHECK(response->header_value("Connection") == "keep-alive");
    }

    const std::optional<HttpResponse> last = client.request(simple_request("GET", "/hello"));
    REQUIRE(last.has_value());
    CHECK(last->status_code == kOkStatus);
    CHECK_MESSAGE(last->header_value("Connection") == "close",
                  "the response that reaches the cap has to announce the close it is about to do");
    CHECK_MESSAGE(client.wait_for_close(),
                  "a connection at its request cap must be reclaimed, not left to the peer");
}

TEST_CASE("closes_a_peer_outside_the_cidr_allowlist_without_sending_a_response") {
    ServerOptions options = loopback_options();
    options.allow_cidrs = {"10.0.0.0/8"};
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    static_cast<void>(client.send_raw(simple_request("GET", "/hello")));

    const std::string received = client.read_until_closed();
    CHECK_MESSAGE(received.empty(),
                  "a filtered peer must learn nothing, not even that it was filtered");
    CHECK_MESSAGE(client.saw_end_of_stream(),
                  "a filtered peer must be closed rather than left hanging");
    CHECK_MESSAGE(!logged_anything_at(fixture->log(), LogLevel::Warning),
                  std::format("rejecting a scanner is routine and must not fill the log:\n{}",
                              joined_log(fixture->log())));
}

TEST_CASE("shuts_down_cleanly_while_a_keep_alive_connection_is_open") {
    TestServer fixture(make_test_router());
    fixture.start();

    TestClient client;
    REQUIRE(client.connect(fixture.port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE(response.has_value());
    CHECK(response->header_value("Connection") == "keep-alive");

    const auto began = std::chrono::steady_clock::now();
    fixture.server().stop();
    fixture.server().wait();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);

    CHECK_MESSAGE(client.wait_for_close(),
                  "an in-flight keep-alive peer must see FIN, not a silent stall");
    CHECK_MESSAGE(elapsed < kShutdownBound,
                  std::format("stop() plus wait() must not sit out an epoll timeout: took {} ms",
                              elapsed.count()));
    CHECK_MESSAGE(!logged_anything_at(fixture.log(), LogLevel::Error),
                  std::format("a clean shutdown must log no error:\n{}",
                              joined_log(fixture.log())));
}

TEST_CASE("a throwing startup log sink cannot escape or stop the server") {
    ServerOptions options = loopback_options();
    options.log = [](LogLevel, std::string_view) { throw std::runtime_error("sink failed"); };
    Server server(make_test_router(), std::move(options));

    CHECK_NOTHROW(server.start());

    TestClient client;
    REQUIRE(client.connect(server.port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE(response.has_value());
    CHECK(response->status_code == kOkStatus);

    server.stop();
    CHECK_NOTHROW(server.wait());
}

TEST_CASE("the default listener binds only to IPv6 loopback") {
    CHECK(std::string_view(kDefaultBindAddress) == "::1");
    CHECK(Listener{}.bind_address == "::1");
    REQUIRE(ServerOptions::on_port(0).listeners.size() == 1);
    CHECK(ServerOptions::on_port(0).listeners.front().bind_address == "::1");
}

TEST_CASE("serves_both_of_two_listeners_bound_to_separate_ephemeral_ports") {
    ServerOptions options = loopback_options();
    Listener second;
    second.bind_address = test::kLoopbackIpv4;
    second.port = 0;
    options.listeners.push_back(std::move(second));
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    const uint16_t first_port = fixture->port(0);
    const uint16_t second_port = fixture->port(1);
    REQUIRE(first_port != 0);
    REQUIRE(second_port != 0);
    CHECK_MESSAGE(first_port != second_port,
                  "two listeners that both asked for port 0 must not be handed the same port");
    CHECK(fixture->port() == first_port);

    for (const uint16_t port : {first_port, second_port}) {
        TestClient client;
        REQUIRE(client.connect(port));
        const std::optional<HttpResponse> response =
            client.request(simple_request("GET", "/hello"));
        REQUIRE_MESSAGE(response.has_value(),
                        std::format("the listener on port {} served nothing", port));
        CHECK(response->status_code == kOkStatus);
        CHECK(response->body == kHelloBody);
    }
}

TEST_CASE("reports_is_secure_as_false_on_a_plaintext_listener") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/secure"));
    REQUIRE(response.has_value());
    CHECK(response->body == "plain\n");
    CHECK_MESSAGE(!response->has_header("Strict-Transport-Security"),
                  "HSTS on a cleartext response is a promise the listener cannot keep");
}

TEST_CASE("answers_a_pipelined_request_that_follows_a_body_delivered_in_a_second_write") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("POST", "/echo", "Content-Length: 5\r\n")));
    std::this_thread::sleep_for(kInterWritePause);
    REQUIRE(client.send_raw(std::string(kEchoedBody) + simple_request("GET", "/hello")));

    const std::optional<HttpResponse> first = client.read_response();
    REQUIRE_MESSAGE(first.has_value(), "the request whose body arrived second went unanswered");
    CHECK(first->status_code == kOkStatus);
    CHECK(first->body == kEchoedBody);

    const std::optional<HttpResponse> second = client.read_response();
    REQUIRE_MESSAGE(second.has_value(),
                    "a request pipelined behind a body must not wait for bytes that will never "
                    "arrive -- it is already sitting in the server's own read buffer");
    CHECK(second->status_code == kOkStatus);
    CHECK(second->body == kHelloBody);
}

TEST_CASE("answers_head_on_a_streaming_route_with_the_head_alone_and_never_opens_the_stream") {
    StreamLedger ledger;

    Router router = make_test_router();
    router.get(kEventsPath, [&ledger](const Request&) {
        ledger.handed_out.fetch_add(1, std::memory_order_relaxed);
        return Response::stream(std::make_shared<MarkerStream>(ledger));
    });
    const auto fixture = started_test_server(std::move(router));

    TestClient head_client;
    REQUIRE(head_client.connect(fixture->port()));
    const std::optional<HttpResponse> from_head =
        head_client.request(simple_request("HEAD", kEventsPath), test::kDefaultResponseTimeout,
                            BodyExpectation::HeadRequest);
    REQUIRE(from_head.has_value());
    CHECK(from_head->status_code == kOkStatus);
    CHECK(from_head->header_value("Content-Type") == kEventStreamContentType);
    CHECK(from_head->header_value("Connection") == "close");
    CHECK_FALSE_MESSAGE(from_head->has_header("Content-Length"),
                        "a stream has no length to declare and a HEAD must not invent one");
    CHECK_MESSAGE(from_head->body.empty(), "a HEAD response carries no body");
    CHECK_MESSAGE(head_client.read_some(kStreamReadSlice).empty(),
                  "bytes followed the head of a HEAD, so the source was pulled into a response "
                  "that may not carry one");
    CHECK_MESSAGE(head_client.wait_for_close(),
                  "the head announced Connection: close and the connection stayed open");
    CHECK_MESSAGE(ledger.attached.load() == 0,
                  "a HEAD was enrolled in a stream that can never send it anything");
    CHECK_MESSAGE(
        wait_until([&ledger] { return ledger.released.load() == ledger.handed_out.load(); }),
        "the source handed to a HEAD was dropped without being released, so whatever produced it "
        "counts the subscription for ever");

    TestClient get_client;
    REQUIRE(get_client.connect(fixture->port()));
    REQUIRE(get_client.send_raw(simple_request("GET", kEventsPath)));
    const std::optional<HttpResponse> from_get = get_client.read_head();
    REQUIRE(from_get.has_value());
    CHECK(from_get->status_code == kOkStatus);
    CHECK(field_names_except(*from_head, "Date") == field_names_except(*from_get, "Date"));
    CHECK_MESSAGE(stream_delivers(get_client, kStreamMarker),
                  "the GET received no stream bytes, so the HEAD above proves nothing");
    CHECK(ledger.attached.load() == 1);
}

TEST_CASE("an_unknown_exception_from_a_stream_callback_closes_only_its_connection") {
    for (const ThrowingCallback callback : {ThrowingCallback::Attach, ThrowingCallback::Pull}) {
        std::atomic<int> released{0};
        Router router = make_test_router();
        router.get(kEventsPath, [callback, &released](const Request&) {
            return Response::stream(std::make_shared<ThrowingStream>(callback, released));
        });
        const auto fixture = started_test_server(std::move(router));

        TestClient failed_stream;
        REQUIRE(failed_stream.connect(fixture->port()));
        REQUIRE(failed_stream.send_raw(simple_request("GET", kEventsPath)));
        const std::optional<HttpResponse> head = failed_stream.read_head();
        REQUIRE(head.has_value());
        CHECK(head->status_code == kOkStatus);
        CHECK(failed_stream.wait_for_close());
        CHECK(wait_until([&released] { return released.load() == 1; }));

        TestClient next_request;
        REQUIRE(next_request.connect(fixture->port()));
        const std::optional<HttpResponse> response =
            next_request.request(simple_request("GET", "/hello"));
        REQUIRE(response.has_value());
        CHECK(response->status_code == kOkStatus);
        CHECK(response->body == kHelloBody);
    }
}

TEST_CASE("connections_active_returns_to_zero_when_a_server_is_destroyed_holding_connections") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    const Gauge& active =
        registry.gauge(std::string(kActiveMetricName), std::string(kMetricsHelpText));

    std::vector<TestClient> clients(kHeldConnectionCount);
    {
        TestServer fixture(make_test_router());
        fixture.server().install_metrics(registry);
        fixture.start();

        for (TestClient& client : clients) {
            REQUIRE(client.connect(fixture.port()));
            const std::optional<HttpResponse> answer =
                client.request(simple_request("GET", "/hello"));
            REQUIRE(answer.has_value());
            REQUIRE(answer->header_value("Connection") == "keep-alive");
        }

        REQUIRE_MESSAGE(
            wait_until([&active] {
                return active.value() == static_cast<double>(kHeldConnectionCount);
            }),
            std::format("the gauge reads {} while {} keep-alive connections are open, so what it "
                        "does at teardown cannot be measured",
                        active.value(), kHeldConnectionCount));

        fixture.server().stop();
        fixture.server().wait();
    }

    CHECK_MESSAGE(active.value() == 0.0,
                  std::format("{} connections were open when the Server was destroyed and the "
                              "gauge still reports {} of them",
                              kHeldConnectionCount, active.value()));
}

} // namespace erikslund::http
