
#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/observability.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/sse.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/status_page.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kServiceName = "erikslund-http";
constexpr std::string_view kServiceVersion = "0.1.2";
constexpr std::string_view kTestHost = "erikslund.test";

constexpr std::string_view kGetMethod = "GET";
constexpr std::string_view kHeadMethod = "HEAD";
constexpr std::string_view kPostMethod = "POST";

constexpr std::string_view kStatusPath = "/";
constexpr std::string_view kStatusAliasPath = "/status";
constexpr std::string_view kMetricsPath = "/metrics";
constexpr std::string_view kMetricsJsonPath = "/metrics.json";
constexpr std::string_view kHealthPath = "/health";
constexpr std::string_view kHealthzPath = "/healthz";
constexpr std::string_view kFaviconPath = "/favicon.ico";
constexpr std::string_view kEventsPath = "/events";
constexpr std::string_view kUnknownPath = "/admin";

constexpr std::string_view kBaseRouteSet = "/ /favicon.ico /health /healthz /status";
constexpr std::string_view kRouteSetWithMetricsAndJson =
    "/ /favicon.ico /health /healthz /metrics /metrics.json /status";
constexpr std::string_view kRouteSetWithEvents = "/ /events /favicon.ico /health /healthz /status";

constexpr std::string_view kHealthyBody = "ok\n";
constexpr std::string_view kDegradedBody = "degraded\n";

constexpr std::string_view kNotFoundBody = "Not Found\n";
constexpr std::string_view kInternalErrorBody = "Internal Server Error\n";

constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kPlainTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kPrometheusContentType = "text/plain; version=0.0.4; charset=utf-8";
constexpr std::string_view kJsonContentType = "application/json";

constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kContentSecurityPolicyField = "Content-Security-Policy";
constexpr std::string_view kAllowField = "Allow";
constexpr std::string_view kGetOnlyAllow = "GET";
constexpr std::string_view kGetHeadAllow = "GET, HEAD";

constexpr std::string_view kLiveContentSecurityPolicy =
    "default-src 'self'; style-src 'unsafe-inline'; "
    "script-src 'sha256-QqFlhp2qcNvjz3NKBAPEc2Z6SzpfHaQzpX7XA1i95oo='; "
    "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'; "
    "object-src 'none'";

constexpr std::string_view kMetricsPrefix = "erikslund_http";
constexpr std::string_view kRequestsName = "requests";
constexpr std::string_view kRequestsHelp = "Requests accepted on the operator surface.";
constexpr std::string_view kRequestsSeriesLine = "erikslund_http_requests_total 1";
constexpr std::string_view kJsonPrefixField = R"("prefix":"erikslund_http")";
constexpr std::string_view kJsonMetricNameField = R"("name":"erikslund_http_requests_total")";
constexpr std::string_view kJsonCounterTypeField = R"("type":"counter")";

constexpr std::string_view kPathLabelName = "path";
constexpr std::string_view kHostileLabelValue = "back\\slash \"quoted\"\nnext";
constexpr std::string_view kHostileLabelAsJson =
    "\"path\":\"back\\\\slash \\\"quoted\\\"\\u000anext\"";

constexpr std::string_view kReadyStateLine = "<p class=\"ok\"><strong>READY</strong></p>";
constexpr std::string_view kDegradedStateLine = "<p class=\"bad\"><strong>DEGRADED</strong></p>";
constexpr std::string_view kUptimeRowOpening = "  <tr><td>uptime</td><td>";
constexpr std::string_view kStatusLinkMarkup = "<a href=\"/\">/</a>";
constexpr std::string_view kMetricsLinkMarkup = "<a href=\"/metrics\">/metrics</a>";
constexpr std::string_view kHealthzLinkMarkup = "<a href=\"/healthz\">/healthz</a>";

constexpr std::string_view kEventSourceCall = "new EventSource(\"/events\")";
constexpr std::string_view kLiveRowsTbody = "<tbody id=\"status-rows\">";
constexpr std::string_view kRawScriptOpening = "<script";

constexpr std::string_view kFactoryFailureDetail = "the snapshot mutex was already poisoned";
constexpr std::string_view kProbeFailureDetail = "the backend table went away";

constexpr std::string_view kScriptInjection = "<script>alert(1)</script>";
constexpr std::string_view kEscapedScriptInjection = "&lt;script&gt;alert(1)&lt;/script&gt;";

constexpr std::string_view kFixedRowLabel = "listeners";
constexpr std::string_view kFixedRowValue = "127.0.0.1:8080";

constexpr size_t kProbeCount = 5;
constexpr size_t kNoFactoryCalls = 0;
constexpr size_t kOneFactoryCall = 1;
constexpr size_t kNoPredicateCalls = 0;
constexpr size_t kNoSubscribers = 0;

[[nodiscard]] std::string wire_request(std::string_view method, std::string_view target) {
    return std::format("{} {} HTTP/1.1\r\nHost: {}\r\n\r\n", method, target, kTestHost);
}

[[nodiscard]] Request parse_or_fail(const std::string& wire) {
    std::expected<ParsedRequest, ParseError> parsed = parse_request(wire, RequestLimits{});
    REQUIRE(parsed.has_value());
    return std::move(parsed->request);
}

[[nodiscard]] Response dispatch_wire(const Router& router, std::string_view method,
                                     std::string_view target) {
    const std::string wire = wire_request(method, target);
    Request request = parse_or_fail(wire);
    return router.dispatch(request);
}

[[nodiscard]] bool has_header(const Response& response, std::string_view name) {
    return response.headers().contains(std::string(name));
}

[[nodiscard]] std::string_view header_value(const Response& response, std::string_view name) {
    const std::string key(name);
    if (!response.headers().contains(key))
        return {};
    return response.headers().at(key);
}

[[nodiscard]] std::string join_with_space(const std::vector<std::string>& parts) {
    std::string joined;
    for (const std::string& part : parts) {
        if (!joined.empty())
            joined += ' ';
        joined += part;
    }
    return joined;
}

[[nodiscard]] ObservabilityOptions named_options() {
    ObservabilityOptions options;
    options.service_name = std::string(kServiceName);
    options.version = std::string(kServiceVersion);
    return options;
}

[[nodiscard]] StatusPage fixed_page() {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kFixedRowLabel, kFixedRowValue);
    return page;
}

} // namespace

TEST_CASE("mounts_exactly_the_page_and_probe_routes_when_nothing_else_is_supplied") {
    Router router;
    mount_observability(router, named_options());

    CHECK(join_with_space(router.paths()) == kBaseRouteSet);
}

TEST_CASE("mounts_the_metrics_routes_only_when_a_registry_is_supplied") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    Router router;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(router, std::move(options));

    CHECK(join_with_space(router.paths()) == kRouteSetWithMetricsAndJson);
}

TEST_CASE("mounts_the_events_route_only_when_a_channel_is_supplied") {
    SseChannel channel;
    Router router;
    ObservabilityOptions options = named_options();
    options.live = &channel;
    mount_observability(router, std::move(options));

    CHECK(join_with_space(router.paths()) == kRouteSetWithEvents);
}

TEST_CASE("the_status_page_and_its_alias_return_utf8_html") {
    Router router;
    mount_observability(router, named_options());

    const Response root = dispatch_wire(router, kGetMethod, kStatusPath);
    CHECK(root.status() == Status::Ok);
    CHECK(header_value(root, kContentTypeField) == kHtmlContentType);

    const Response alias = dispatch_wire(router, kGetMethod, kStatusAliasPath);
    CHECK(alias.status() == Status::Ok);
    CHECK(header_value(alias, kContentTypeField) == kHtmlContentType);
}

TEST_CASE("the_status_alias_renders_the_same_document_as_the_root") {
    Router router;
    ObservabilityOptions options = named_options();
    options.status_page = [] { return fixed_page(); };
    mount_observability(router, std::move(options));

    const Response root = dispatch_wire(router, kGetMethod, kStatusPath);
    const Response alias = dispatch_wire(router, kGetMethod, kStatusAliasPath);
    CHECK_MESSAGE(root.body() == alias.body(),
                  "one immutable context makes the two routes the same page by construction");
}

TEST_CASE("the_status_page_carries_the_canonical_stylesheet_through_the_router") {
    Router router;
    mount_observability(router, named_options());

    const Response page = dispatch_wire(router, kGetMethod, kStatusPath);
    const std::string body(page.body());
    CHECK(body.contains(kStatusPageStyle));
    CHECK(body.contains(kReadyStateLine));
    CHECK(body.contains(kUptimeRowOpening));
    CHECK(body.contains(kStatusLinkMarkup));
    CHECK(body.contains(kHealthzLinkMarkup));
}

TEST_CASE("the_default_page_links_metrics_only_when_metrics_are_mounted") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};

    Router without_metrics;
    mount_observability(without_metrics, named_options());
    const Response plain = dispatch_wire(without_metrics, kGetMethod, kStatusPath);
    CHECK_MESSAGE(!std::string(plain.body()).contains(kMetricsLinkMarkup),
                  "a footer link to a route that answers 404 is worse than no link");

    Router with_metrics;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(with_metrics, std::move(options));
    const Response linked = dispatch_wire(with_metrics, kGetMethod, kStatusPath);
    CHECK(std::string(linked.body()).contains(kMetricsLinkMarkup));
}

TEST_CASE("the_default_page_headlines_degraded_when_the_predicate_says_so") {
    Router router;
    ObservabilityOptions options = named_options();
    options.healthy = [] { return false; };
    mount_observability(router, std::move(options));

    const std::string body(dispatch_wire(router, kGetMethod, kStatusPath).body());
    CHECK(body.contains(kDegradedStateLine));
    CHECK_FALSE(body.contains(kReadyStateLine));
}

TEST_CASE("the_metrics_route_returns_the_registry_under_the_0_0_4_content_type") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    Router router;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(router, std::move(options));

    requests.increment();
    const Response scrape = dispatch_wire(router, kGetMethod, kMetricsPath);
    CHECK(scrape.status() == Status::Ok);
    CHECK(header_value(scrape, kContentTypeField) == kPrometheusContentType);
    CHECK_MESSAGE(scrape.body() == registry.prometheus(),
                  "the endpoint must serve the registry itself, not a rendering of its own");
    CHECK(std::string(scrape.body()).contains(kRequestsSeriesLine));
}

TEST_CASE("the_metrics_route_is_rebuilt_per_scrape_rather_than_cached") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    Router router;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(router, std::move(options));

    const Response before = dispatch_wire(router, kGetMethod, kMetricsPath);
    requests.increment();
    const Response after = dispatch_wire(router, kGetMethod, kMetricsPath);
    CHECK(before.body() != after.body());
    CHECK(std::string(after.body()).contains(kRequestsSeriesLine));
}

TEST_CASE("the_metrics_json_route_mirrors_the_registry") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    Router router;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(router, std::move(options));
    requests.increment();

    const Response mirror = dispatch_wire(router, kGetMethod, kMetricsJsonPath);
    CHECK(mirror.status() == Status::Ok);
    CHECK(header_value(mirror, kContentTypeField) == kJsonContentType);
    const std::string body(mirror.body());
    CHECK(body.contains(kJsonPrefixField));
    CHECK(body.contains(kJsonMetricNameField));
    CHECK(body.contains(kJsonCounterTypeField));
}

TEST_CASE("the_metrics_json_mirror_undoes_the_exposition_escapes_and_applies_its_own") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    Labels labels;
    labels.emplace_back(std::string(kPathLabelName), std::string(kHostileLabelValue));
    Counter& requests =
        registry.counter(std::string(kRequestsName), std::string(kRequestsHelp), labels);
    Router router;
    ObservabilityOptions options = named_options();
    options.metrics = &registry;
    mount_observability(router, std::move(options));
    requests.increment();

    const Response mirror = dispatch_wire(router, kGetMethod, kMetricsJsonPath);
    REQUIRE(mirror.status() == Status::Ok);
    CHECK_MESSAGE(std::string(mirror.body()).contains(kHostileLabelAsJson),
                  "deriving the mirror from the exposition is what keeps the two endpoints "
                  "agreeing, which only works if the round trip is lossless");
}

TEST_CASE("the_metrics_route_is_absent_when_no_registry_is_supplied") {
    Router router;
    mount_observability(router, named_options());

    const Response scrape = dispatch_wire(router, kGetMethod, kMetricsPath);
    CHECK_MESSAGE(scrape.status() == Status::NotFound,
                  "an endpoint that always returns nothing is worse than a 404, because a scrape "
                  "configured against it looks healthy while collecting no data");
    CHECK(scrape.body() == kNotFoundBody);
}

TEST_CASE("health_and_healthz_both_answer_200_and_ok") {
    Router router;
    ObservabilityOptions options = named_options();
    options.healthy = [] { return true; };
    mount_observability(router, std::move(options));

    for (const std::string_view path : {kHealthPath, kHealthzPath}) {
        const Response probe = dispatch_wire(router, kGetMethod, path);
        CHECK(probe.status() == Status::Ok);
        CHECK(probe.body() == kHealthyBody);
        CHECK(header_value(probe, kContentTypeField) == kPlainTextContentType);
    }
}

TEST_CASE("health_and_healthz_both_answer_503_and_degraded_from_the_predicate") {
    Router router;
    ObservabilityOptions options = named_options();
    options.healthy = [] { return false; };
    mount_observability(router, std::move(options));

    for (const std::string_view path : {kHealthPath, kHealthzPath}) {
        const Response probe = dispatch_wire(router, kGetMethod, path);
        CHECK(probe.status() == Status::ServiceUnavailable);
        CHECK(probe.body() == kDegradedBody);
    }
}

TEST_CASE("an_absent_health_predicate_still_reports_the_process_as_alive") {
    Router router;
    mount_observability(router, named_options());

    const Response probe = dispatch_wire(router, kGetMethod, kHealthzPath);
    CHECK_MESSAGE(probe.status() == Status::Ok,
                  "the process accepted the connection and wrote a response, which is itself a "
                  "useful liveness answer");
    CHECK(probe.body() == kHealthyBody);
}

TEST_CASE("the_health_probe_never_builds_a_status_page") {
    auto factory_calls = std::make_shared<size_t>(0);
    Router router;
    ObservabilityOptions options = named_options();
    options.status_page = [factory_calls] {
        ++*factory_calls;
        return fixed_page();
    };
    mount_observability(router, std::move(options));

    for (size_t probe_index = 0; probe_index < kProbeCount; ++probe_index) {
        const Response health = dispatch_wire(router, kGetMethod, kHealthPath);
        const Response healthz = dispatch_wire(router, kGetMethod, kHealthzPath);
        CHECK(health.status() == Status::Ok);
        CHECK(healthz.status() == Status::Ok);
    }

    CHECK_MESSAGE(*factory_calls == kNoFactoryCalls,
                  "a probe runs every few seconds forever, and making it pay for a full snapshot "
                  "is how an observability surface becomes the load it was meant to observe");

    const Response page = dispatch_wire(router, kGetMethod, kStatusPath);
    CHECK(page.status() == Status::Ok);
    CHECK_MESSAGE(*factory_calls == kOneFactoryCall,
                  "the page route is the only route that may build a snapshot");
}

TEST_CASE("favicon_returns_204_with_no_body_and_no_content_type") {
    Router router;
    mount_observability(router, named_options());

    const Response icon = dispatch_wire(router, kGetMethod, kFaviconPath);
    CHECK(icon.status() == Status::NoContent);
    CHECK(icon.body().empty());
    CHECK_MESSAGE(!has_header(icon, kContentTypeField),
                  "a body of zero bytes has no media type to declare");
}

TEST_CASE("head_on_the_status_page_reaches_the_get_handler") {
    Router router;
    mount_observability(router, named_options());

    const Response page = dispatch_wire(router, kHeadMethod, kStatusPath);
    CHECK(page.status() == Status::Ok);
    CHECK(header_value(page, kContentTypeField) == kHtmlContentType);
    CHECK_MESSAGE(!page.body().empty(),
                  "the writer suppresses the body of a HEAD, not the router, so the handler still "
                  "produces the bytes Content-Length has to describe");
}

TEST_CASE("live_updates_add_the_event_source_script_and_a_policy_that_authorises_only_it") {
    SseChannel channel;
    Router router;
    ObservabilityOptions options = named_options();
    options.live = &channel;
    mount_observability(router, std::move(options));

    const Response page = dispatch_wire(router, kGetMethod, kStatusPath);
    const std::string body(page.body());
    CHECK(body.contains(kEventSourceCall));
    CHECK(body.contains(kLiveRowsTbody));
    CHECK(header_value(page, kContentSecurityPolicyField) == kLiveContentSecurityPolicy);
}

TEST_CASE("a_page_without_live_updates_carries_no_script_and_no_policy_override") {
    Router router;
    mount_observability(router, named_options());

    const Response page = dispatch_wire(router, kGetMethod, kStatusPath);
    CHECK_FALSE(std::string(page.body()).contains(kRawScriptOpening));
    CHECK_MESSAGE(!has_header(page, kContentSecurityPolicyField),
                  "the writer's own default policy is the right one for a page with no script");
}

TEST_CASE("the_events_route_refuses_head_with_405_and_an_allow_header") {
    SseChannel channel;
    Router router;
    ObservabilityOptions options = named_options();
    options.live = &channel;
    mount_observability(router, std::move(options));

    const Response probe = dispatch_wire(router, kHeadMethod, kEventsPath);
    CHECK_MESSAGE(probe.status() == Status::MethodNotAllowed,
                  "answering a HEAD would consume a subscriber slot nothing will ever read from");
    CHECK(header_value(probe, kAllowField) == kGetOnlyAllow);
    CHECK(channel.subscriber_count() == kNoSubscribers);
}

TEST_SUITE("observability adversarial") {

TEST_CASE("mounting_twice_on_one_router_is_refused") {
    Router router;
    mount_observability(router, named_options());

    CHECK_THROWS_AS(mount_observability(router, named_options()), ServerError);
}

TEST_CASE("a_status_page_factory_that_throws_becomes_a_500_that_says_nothing") {
    Router router;
    ObservabilityOptions options = named_options();
    options.status_page = []() -> StatusPage {
        throw std::runtime_error(std::string(kFactoryFailureDetail));
    };
    mount_observability(router, std::move(options));

    const Response page = dispatch_wire(router, kGetMethod, kStatusPath);
    CHECK(page.status() == Status::InternalServerError);
    CHECK(page.body() == kInternalErrorBody);
    CHECK_MESSAGE(!std::string(page.body()).contains(kFactoryFailureDetail),
                  "an exception string is internal detail and never travels to a peer");
}

TEST_CASE("a_health_predicate_that_throws_becomes_a_500_rather_than_killing_the_worker") {
    Router router;
    ObservabilityOptions options = named_options();
    options.healthy = []() -> bool { throw std::runtime_error(std::string(kProbeFailureDetail)); };
    mount_observability(router, std::move(options));

    const Response probe = dispatch_wire(router, kGetMethod, kHealthPath);
    CHECK(probe.status() == Status::InternalServerError);
    CHECK_FALSE(std::string(probe.body()).contains(kProbeFailureDetail));
}

TEST_CASE("a_hostile_service_name_reaches_the_page_escaped") {
    Router router;
    ObservabilityOptions options = named_options();
    options.service_name = std::string(kScriptInjection);
    mount_observability(router, std::move(options));

    const std::string body(dispatch_wire(router, kGetMethod, kStatusPath).body());
    CHECK_MESSAGE(!body.contains(kRawScriptOpening),
                  "a service name comes from configuration, which is not the same as trusted");
    CHECK(body.contains(kEscapedScriptInjection));
}

TEST_CASE("an_unmounted_path_under_the_observability_surface_is_a_404") {
    Router router;
    mount_observability(router, named_options());

    const Response response = dispatch_wire(router, kGetMethod, kUnknownPath);
    CHECK(response.status() == Status::NotFound);
    CHECK(response.body() == kNotFoundBody);
}

TEST_CASE("a_post_to_the_status_page_is_405_with_an_allow_header_and_not_404") {
    Router router;
    mount_observability(router, named_options());

    const Response response = dispatch_wire(router, kPostMethod, kStatusPath);
    CHECK_MESSAGE(response.status() == Status::MethodNotAllowed,
                  "a 404 would send a client hunting for a routing bug that does not exist");
    CHECK(header_value(response, kAllowField) == kGetHeadAllow);
}

TEST_CASE("a_post_to_the_health_probe_is_405_and_never_evaluates_the_predicate") {
    auto predicate_calls = std::make_shared<size_t>(0);
    Router router;
    ObservabilityOptions options = named_options();
    options.healthy = [predicate_calls] {
        ++*predicate_calls;
        return true;
    };
    mount_observability(router, std::move(options));

    const Response response = dispatch_wire(router, kPostMethod, kHealthzPath);
    CHECK(response.status() == Status::MethodNotAllowed);
    CHECK_MESSAGE(*predicate_calls == kNoPredicateCalls,
                  "the router answers a wrong verb before any handler runs");
}

} // TEST_SUITE("observability adversarial")
