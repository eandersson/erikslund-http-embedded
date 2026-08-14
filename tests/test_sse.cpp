
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <doctest/doctest.h>

#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/sse.hpp"
#include "erikslund/http/status.hpp"
#include "internal/connection.hpp"

namespace erikslund::http {

namespace {

struct RequestFixture {
    std::unique_ptr<std::string> wire{};
    Request request{};
};

[[nodiscard]] RequestFixture request_for(std::string_view method_token) {
    RequestFixture fixture;
    fixture.wire = std::make_unique<std::string>(
        std::format("{} /events HTTP/1.1\r\nHost: box.lan\r\n\r\n", method_token));
    auto parsed = parse_request(*fixture.wire, RequestLimits{});
    REQUIRE_MESSAGE(parsed.has_value(), "the test's own request bytes must parse");
    fixture.request = std::move(parsed->request);
    return fixture;
}

[[nodiscard]] Response subscribe(const Handler& handler) {
    const RequestFixture fixture = request_for("GET");
    return handler(fixture.request);
}

[[nodiscard]] SseOptions quiet_options() {
    return SseOptions{.heartbeat_interval = std::chrono::seconds::zero(),
                      .retry_hint = std::chrono::seconds::zero()};
}

[[nodiscard]] StreamSource& source_of(const Response& response) {
    REQUIRE(response.is_stream());
    REQUIRE(response.stream_source() != nullptr);
    return *response.stream_source();
}

[[nodiscard]] std::string drain(const Response& response) {
    std::string out;
    StreamSource& source = source_of(response);
    while (source.pull(out) == StreamSource::Pull::Wrote) {
    }
    return out;
}

[[nodiscard]] StreamSource::Pull pull_once(const Response& response, std::string& out) {
    return source_of(response).pull(out);
}

[[nodiscard]] std::string overflow_payload(size_t index) {
    return std::format("payload-{:04}", index);
}

[[nodiscard]] size_t framed_bytes(std::string_view payload) {
    constexpr size_t kDataPrefixBytes = 6;
    constexpr size_t kLineAndDispatchBytes = 2;
    return kDataPrefixBytes + payload.size() + kLineAndDispatchBytes;
}

} // namespace

TEST_CASE("a single line payload becomes one data line and a dispatching blank line") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "hello");
    CHECK(drain(stream) == "data: hello\n\n");
}

TEST_CASE("a multi line payload becomes one data line per line") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "first\nsecond\nthird");
    CHECK(drain(stream) == "data: first\ndata: second\ndata: third\n\n");
}

TEST_CASE("a crlf payload counts one line break and not two") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "first\r\nsecond");
    CHECK(drain(stream) == "data: first\ndata: second\n\n");
}

TEST_CASE("a lone carriage return ends a line the same way a newline does") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "first\rsecond");
    CHECK(drain(stream) == "data: first\ndata: second\n\n");
}

TEST_CASE("a trailing newline emits the empty final line it describes") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "first\n");
    CHECK(drain(stream) == "data: first\ndata: \n\n");
}

TEST_CASE("an empty payload still emits exactly one data line") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "");
    CHECK(drain(stream) == "data: \n\n");
}

TEST_CASE("a named event emits an event line before its data") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("status", "READY");
    CHECK(drain(stream) == "event: status\ndata: READY\n\n");
}

TEST_CASE("an unnamed event emits no event line at all") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "READY");
    CHECK(drain(stream).find("event:") == std::string::npos);
}

TEST_CASE("a line break in an event name becomes a space so it cannot forge fields") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("tick\ndata: forged\nretry: 1", "real");
    CHECK(drain(stream) == "event: tick data: forged retry: 1\ndata: real\n\n");
}

TEST_CASE("a carriage return in an event name becomes a space too") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("tick\rforged", "real");
    CHECK(drain(stream) == "event: tick forged\ndata: real\n\n");
}

TEST_CASE("publish_json frames a payload exactly the way publish does") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response through_json = subscribe(handler);

    channel.publish_json("snapshot", R"({"ready":true})");
    CHECK(drain(through_json) == "event: snapshot\ndata: {\"ready\":true}\n\n");
}

TEST_CASE("the retry hint is the first thing on the wire and is written in milliseconds") {
    SseChannel channel;
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    CHECK(drain(stream) == "retry: 5000\n\n");
}

TEST_CASE("a configured retry hint is converted from seconds to milliseconds") {
    constexpr std::chrono::seconds kRetryHint{30};
    SseChannel channel(SseOptions{.retry_hint = kRetryHint});
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    CHECK(drain(stream) == "retry: 30000\n\n");
}

TEST_CASE("a zero retry hint queues no retry frame at all") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Idle);
    CHECK(out.empty());
}

TEST_CASE("the retry frame precedes the first published event") {
    SseChannel channel(SseOptions{.retry_hint = std::chrono::seconds{5}});
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "first");
    CHECK(drain(stream) == "retry: 5000\n\ndata: first\n\n");
}

TEST_CASE("a heartbeat is a comment frame the client ignores and an intermediary does not") {
    constexpr std::chrono::seconds kHeartbeatInterval{1};
    constexpr std::chrono::milliseconds kPollInterval{50};
    constexpr std::chrono::seconds kPatience{10};

    SseChannel channel(SseOptions{.heartbeat_interval = kHeartbeatInterval,
                                  .retry_hint = std::chrono::seconds::zero()});
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    StreamSource& source = source_of(stream);
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + kPatience;
    while (out.empty() && std::chrono::steady_clock::now() < deadline) {
        if (source.pull(out) != StreamSource::Pull::Wrote)
            std::this_thread::sleep_for(kPollInterval);
    }

    CHECK_MESSAGE(out == ": heartbeat\n\n", "an idle stream must produce a comment frame");
}

TEST_CASE("the default heartbeat fits well inside the liveness budget a stream is given") {
    const std::chrono::milliseconds budget = kDefaultStreamIdleTimeout;
    const auto heartbeat =
        std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultHeartbeatInterval);

    CHECK_MESSAGE(heartbeat < budget,
                  std::format("the default heartbeat is {} ms against a {} ms liveness budget",
                              heartbeat.count(), budget.count()));
}

TEST_CASE("a stream response declares the event stream type and forbids intermediary buffering") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    REQUIRE(stream.is_stream());
    CHECK(stream.status() == Status::Ok);
    REQUIRE(stream.headers().contains("Content-Type"));
    CHECK(stream.headers().at("Content-Type") == "text/event-stream");
    REQUIRE(stream.headers().contains("Cache-Control"));
    CHECK(stream.headers().at("Cache-Control") == "no-store");
    REQUIRE(stream.headers().contains("X-Accel-Buffering"));
    CHECK(stream.headers().at("X-Accel-Buffering") == "no");
}

TEST_CASE("a head request is answered without opening a stream") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const RequestFixture fixture = request_for("HEAD");
    const Response probe = handler(fixture.request);

    CHECK_FALSE(probe.is_stream());
    CHECK(probe.status() == Status::Ok);
    REQUIRE(probe.headers().contains("Content-Type"));
    CHECK(probe.headers().at("Content-Type") == "text/event-stream");
    CHECK(channel.subscriber_count() == 0);
}

TEST_CASE("subscriber_count reports the subscriptions the channel is actually feeding") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    CHECK(channel.subscriber_count() == 0);

    const Response first = subscribe(handler);
    CHECK(channel.subscriber_count() == 1);

    const Response second = subscribe(handler);
    CHECK(channel.subscriber_count() == 2);
}

TEST_CASE("publishing to a channel with no subscribers is harmless") {
    SseChannel channel(quiet_options());
    channel.publish("status", "READY");
    channel.publish_json("snapshot", R"({"ready":true})");
    channel.publish("", "");
    CHECK(channel.subscriber_count() == 0);
}

TEST_CASE("every subscriber receives every published event") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response first = subscribe(handler);
    const Response second = subscribe(handler);

    channel.publish("status", "READY");
    CHECK(drain(first) == "event: status\ndata: READY\n\n");
    CHECK(drain(second) == "event: status\ndata: READY\n\n");
}

TEST_CASE("a burst of events leaves the connection in one pull rather than one write per event") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "one");
    channel.publish("", "two");
    channel.publish("", "three");

    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Wrote);
    CHECK(out == "data: one\n\ndata: two\n\ndata: three\n\n");
    CHECK(pull_once(stream, out) == StreamSource::Pull::Idle);
}

TEST_CASE("close_all detaches every subscriber") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response first = subscribe(handler);
    const Response second = subscribe(handler);
    REQUIRE(channel.subscriber_count() == 2);

    channel.close_all();
    CHECK(channel.subscriber_count() == 0);
}

TEST_CASE("close_all lets a subscriber finish what is already queued") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", "last");
    channel.close_all();

    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Wrote);
    CHECK(out == "data: last\n\n");
    CHECK(pull_once(stream, out) == StreamSource::Pull::Finished);
}

TEST_CASE("a publish after close_all reaches nobody") {
    SseChannel channel(quiet_options());
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);
    channel.close_all();

    channel.publish("", "too late");

    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Finished);
    CHECK(out.empty());
    CHECK(channel.subscriber_count() == 0);
}

TEST_CASE("close_all is safe to call twice and on a channel nobody ever subscribed to") {
    SseChannel channel(quiet_options());
    channel.close_all();
    channel.close_all();
    CHECK(channel.subscriber_count() == 0);
}

TEST_CASE("a_handler_outliving_its_channel_fails_closed") {
    Handler handler;
    {
        SseChannel channel(quiet_options());
        handler = channel.handler();
    }

    const Response response = subscribe(handler);
    CHECK_FALSE(response.is_stream());
    CHECK(response.status() == Status::ServiceUnavailable);
    CHECK(response.body() == "sse: channel closed\n");
}

TEST_CASE("the channel refuses a subscription past its ceiling rather than one it cannot feed") {
    constexpr size_t kCeiling = 2;
    SseChannel channel(SseOptions{.max_subscribers = kCeiling});
    const Handler handler = channel.handler();

    const Response first = subscribe(handler);
    const Response second = subscribe(handler);
    REQUIRE(first.is_stream());
    REQUIRE(second.is_stream());
    REQUIRE(channel.subscriber_count() == kCeiling);

    const Response refused = subscribe(handler);
    CHECK_FALSE(refused.is_stream());
    CHECK(refused.status() == Status::ServiceUnavailable);
    CHECK(refused.body() == "sse: subscriber limit reached\n");
    REQUIRE(refused.headers().contains("Retry-After"));
    CHECK(refused.headers().at("Retry-After") == "5");
    CHECK(channel.subscriber_count() == kCeiling);
}

TEST_CASE("a slow subscriber sheds its oldest frames and keeps the newest") {
    constexpr size_t kBudgetFrames = 2;
    const size_t frame_bytes = framed_bytes(overflow_payload(0));

    SseOptions options = quiet_options();
    options.max_queued_bytes_per_subscriber = kBudgetFrames * frame_bytes;

    SseChannel channel(options);
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    for (size_t index = 0; index < kBudgetFrames + 1; ++index)
        channel.publish("", overflow_payload(index));

    CHECK(channel.subscriber_count() == 1);

    const std::string queued = drain(stream);
    CHECK(queued.size() == kBudgetFrames * frame_bytes);
    CHECK(queued.find(overflow_payload(0)) == std::string::npos);
    CHECK(queued.find(overflow_payload(1)) != std::string::npos);
    CHECK(queued.find(overflow_payload(2)) != std::string::npos);
}

TEST_CASE("a subscriber that never reads is disconnected rather than queued without bound") {
    constexpr size_t kBudgetFrames = 2;
    constexpr size_t kPublishCount = 64;
    const size_t frame_bytes = framed_bytes(overflow_payload(0));

    SseOptions options = quiet_options();
    options.max_queued_bytes_per_subscriber = kBudgetFrames * frame_bytes;

    SseChannel channel(options);
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    for (size_t index = 0; index < kPublishCount; ++index)
        channel.publish("", overflow_payload(index));

    CHECK_MESSAGE(channel.subscriber_count() == 0,
                  "a peer that never drains must be dropped, not fed forever");

    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Finished);
    CHECK(out.empty());
}

TEST_CASE("an_event_larger_than_the_queue_budget_disconnects_the_subscriber") {
    constexpr size_t kOneByteBudget = 1;

    SseOptions options = quiet_options();
    options.max_queued_bytes_per_subscriber = kOneByteBudget;

    SseChannel channel(options);
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", overflow_payload(0));

    CHECK(channel.subscriber_count() == 0);
    std::string out;
    CHECK(pull_once(stream, out) == StreamSource::Pull::Finished);
    CHECK(out.empty());
}

TEST_CASE("publish_refuses_an_event_larger_than_the_channel_limit") {
    constexpr std::string_view kAllowedPayload = "tiny";
    SseOptions options = quiet_options();
    options.max_event_bytes = framed_bytes(kAllowedPayload);

    SseChannel channel(options);
    const Handler handler = channel.handler();
    const Response stream = subscribe(handler);

    channel.publish("", kAllowedPayload);
    CHECK(drain(stream) == "data: tiny\n\n");
    CHECK_THROWS_AS(channel.publish("", "one byte too long"), ServerError);
    CHECK_THROWS_AS(channel.publish("", "\n"), ServerError);
}

} // namespace erikslund::http
