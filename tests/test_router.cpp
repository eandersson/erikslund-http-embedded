
#include <doctest/doctest.h>

#include <array>
#include <expected>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/assets.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/text.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kTestHost = "erikslund.test";

constexpr std::string_view kAllowField = "Allow";
constexpr std::string_view kAcceptEncodingField = "Accept-Encoding";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kContentEncodingField = "Content-Encoding";
constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kEtagField = "ETag";
constexpr std::string_view kIfNoneMatchField = "If-None-Match";
constexpr std::string_view kVaryField = "Vary";

constexpr std::string_view kPlainTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kCssContentType = "text/css; charset=utf-8";
constexpr std::string_view kJavaScriptContentType = "text/javascript; charset=utf-8";
constexpr std::string_view kGzipCoding = "gzip";

constexpr std::string_view kGetHeadAllow = "GET, HEAD";
constexpr std::string_view kGetHeadPostAllow = "GET, HEAD, POST";
constexpr std::string_view kPostAllow = "POST";
constexpr std::string_view kPostDeleteAllow = "POST, DELETE";

constexpr std::string_view kImmutableCacheControl = "public, max-age=31536000, immutable";

constexpr std::string_view kDefaultNotFoundBody = "Not Found\n";
constexpr std::string_view kInternalErrorBody = "Internal Server Error\n";
constexpr std::string_view kBadRequestBody = "Bad Request\n";

constexpr std::string_view kMetricsBody = "erikslund_http_up 1\n";
constexpr std::string_view kReadBody = "read\n";
constexpr std::string_view kWriteBody = "written\n";
constexpr std::string_view kClientBody = "client\n";
constexpr std::string_view kIndexBody = "index\n";
constexpr std::string_view kWildcardBody = "below the wildcard\n";
constexpr std::string_view kFallbackBody = "custom fallback\n";
constexpr std::string_view kUnreachableBody = "unreachable\n";
constexpr std::string_view kNamedStatBody = "named stat\n";
constexpr std::string_view kSectionBody = "section\n";
constexpr std::string_view kExactRouteBody = "exact route\n";
constexpr std::string_view kParameterRouteBody = "parameter route\n";
constexpr std::string_view kShortCircuitBody = "short circuited\n";

constexpr std::string_view kHandlerFailureDetail = "the database socket went away";

constexpr std::string_view kFirstMiddlewareName = "first";
constexpr std::string_view kSecondMiddlewareName = "second";
constexpr std::string_view kHandlerName = "handler";

struct ForeignFailure {};

constexpr std::string_view kStyleBytes = "html{color-scheme:dark light}\n";
constexpr EtagBuffer kStyleEtag = weak_etag_buffer(kStyleBytes);
constexpr Asset kStyleAsset = make_asset("/assets/app.css", kStyleBytes, kStyleEtag);
constexpr std::array<Asset, 1> kAbsoluteAssets{kStyleAsset};

constexpr std::string_view kScriptBytes = "export const ready = true;\n";
constexpr EtagBuffer kScriptEtag = weak_etag_buffer(kScriptBytes);
constexpr Asset kScriptAsset = make_asset("/app.mjs", kScriptBytes, kScriptEtag);
constexpr std::array<Asset, 1> kRelativeAssets{kScriptAsset};

constexpr std::string_view kCompressedStyleBytes = "pre-compressed style bytes";
constexpr EtagBuffer kCompressedStyleEtag = weak_etag_buffer(kCompressedStyleBytes);
constexpr Asset kCompressedStyleAsset =
    make_asset("/assets/app.css", kCssContentType, kCompressedStyleBytes, kCompressedStyleEtag,
               kGzipCoding);
constexpr std::array<Asset, 1> kCompressedAssets{kCompressedStyleAsset};

[[nodiscard]] std::string wire_request(std::string_view method, std::string_view target) {
    return std::format("{} {} HTTP/1.1\r\nHost: {}\r\n\r\n", method, target, kTestHost);
}

[[nodiscard]] std::string wire_request_with_field(std::string_view method, std::string_view target,
                                                  std::string_view field_name,
                                                  std::string_view field_value) {
    return std::format("{} {} HTTP/1.1\r\nHost: {}\r\n{}: {}\r\n\r\n", method, target, kTestHost,
                       field_name, field_value);
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

[[nodiscard]] Response dispatch_wire_with_field(const Router& router, std::string_view method,
                                                std::string_view target,
                                                std::string_view field_name,
                                                std::string_view field_value) {
    const std::string wire = wire_request_with_field(method, target, field_name, field_value);
    Request request = parse_or_fail(wire);
    return router.dispatch(request);
}

[[nodiscard]] Handler constant_handler(std::string body) {
    return [answer = std::move(body)](const Request&) { return Response::text(answer); };
}

[[nodiscard]] Handler unreachable_handler() {
    return constant_handler(std::string(kUnreachableBody));
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

} // namespace

TEST_CASE("resolves_an_exact_pattern_to_its_handler") {
    Router router;
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));

    const Response response = dispatch_wire(router, "GET", "/metrics");
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kMetricsBody);
    CHECK(header_value(response, kContentTypeField) == kPlainTextContentType);
}

TEST_CASE("resolves_a_parameter_pattern_and_binds_the_matched_segment") {
    std::string bound_address;
    bool undeclared_name_was_empty = false;

    Router router;
    router.get("/stats/client/{address}",
               [&bound_address, &undeclared_name_was_empty](const Request& request) {
                   bound_address = std::string(request.param("address"));
                   undeclared_name_was_empty = request.param("window").empty();
                   return Response::text(std::string(kClientBody));
               });

    const Response response = dispatch_wire(router, "GET", "/stats/client/bc1qexample");
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kClientBody);
    CHECK(bound_address == "bc1qexample");
    CHECK(undeclared_name_was_empty);
}

TEST_CASE("a_capture_never_spans_a_slash_and_never_binds_an_empty_segment") {
    Router router;
    router.get("/stats/client/{address}", constant_handler(std::string(kClientBody)));

    const Response two_segments = dispatch_wire(router, "GET", "/stats/client/one/two");
    CHECK(two_segments.status() == Status::NotFound);

    const Response empty_segment = dispatch_wire(router, "GET", "/stats/client/");
    CHECK(empty_segment.status() == Status::NotFound);
}

TEST_CASE("resolves_a_trailing_wildcard_to_everything_below_its_own_segment") {
    Router router;
    router.get("/files/*", constant_handler(std::string(kWildcardBody)));
    router.get("/files", constant_handler(std::string(kIndexBody)));

    const Response nested = dispatch_wire(router, "GET", "/files/deep/inside.txt");
    CHECK(nested.status() == Status::Ok);
    CHECK(nested.body() == kWildcardBody);

    const Response trailing_slash = dispatch_wire(router, "GET", "/files/");
    CHECK(trailing_slash.body() == kWildcardBody);

    const Response bare_prefix = dispatch_wire(router, "GET", "/files");
    CHECK(bare_prefix.body() == kIndexBody);
}

TEST_CASE("an_exact_route_wins_over_a_parameter_route_that_would_also_match") {
    Router router;
    router.get("/stats/{name}", constant_handler(std::string(kParameterRouteBody)));
    router.get("/stats/summary", constant_handler(std::string(kExactRouteBody)));

    const Response exact = dispatch_wire(router, "GET", "/stats/summary");
    CHECK(exact.body() == kExactRouteBody);

    const Response parameterised = dispatch_wire(router, "GET", "/stats/live");
    CHECK(parameterised.body() == kParameterRouteBody);
}

TEST_CASE("parameter_routes_are_tried_in_registration_order") {
    Router narrow_first;
    narrow_first.get("/stats/{name}", constant_handler(std::string(kNamedStatBody)));
    narrow_first.get("/{section}/{name}", constant_handler(std::string(kSectionBody)));
    const Response narrow_answer = dispatch_wire(narrow_first, "GET", "/stats/live");
    CHECK(narrow_answer.body() == kNamedStatBody);

    Router broad_first;
    broad_first.get("/{section}/{name}", constant_handler(std::string(kSectionBody)));
    broad_first.get("/stats/{name}", constant_handler(std::string(kNamedStatBody)));
    const Response broad_answer = dispatch_wire(broad_first, "GET", "/stats/live");
    CHECK(broad_answer.body() == kSectionBody);
}

TEST_CASE("rejects_a_pattern_that_does_not_start_with_a_slash") {
    Router router;
    CHECK_THROWS_AS(router.get("metrics", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("", unreachable_handler()), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("rejects_a_duplicate_exact_pattern_whose_method_mask_overlaps") {
    Router router;
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));

    CHECK_THROWS_AS(router.get("/metrics", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.route(Method::Head, "/metrics", unreachable_handler()), ServerError);
    CHECK(router.paths() == std::vector<std::string>{"/metrics"});
}

TEST_CASE("rejects_a_duplicate_parameter_pattern_whose_method_mask_overlaps") {
    Router router;
    router.get("/stats/{name}", constant_handler(std::string(kNamedStatBody)));
    CHECK_THROWS_AS(router.get("/stats/{name}", unreachable_handler()), ServerError);
}

TEST_CASE("accepts_a_second_registration_of_a_pattern_whose_method_mask_is_disjoint") {
    Router router;
    router.get("/thing", constant_handler(std::string(kReadBody)));
    router.post("/thing", constant_handler(std::string(kWriteBody)));

    const Response read_answer = dispatch_wire(router, "GET", "/thing");
    const Response write_answer = dispatch_wire(router, "POST", "/thing");
    CHECK(read_answer.body() == kReadBody);
    CHECK(write_answer.body() == kWriteBody);
}

TEST_CASE("rejects_a_capture_that_is_not_a_whole_path_segment") {
    Router router;
    CHECK_THROWS_AS(router.get("/user-{name}", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/{name}.json", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/stats/{a}{b}", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/stats/{}", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/stats/{name", unreachable_handler()), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("rejects_a_wildcard_that_is_not_the_final_whole_segment") {
    Router router;
    CHECK_THROWS_AS(router.get("/assets/*/icon.png", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/*/assets", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/assets/icon*", unreachable_handler()), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("rejects_a_pattern_holding_a_segment_no_request_path_can_carry") {
    Router router;
    CHECK_THROWS_AS(router.get("//", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/a//b", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/a/./b", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/a/.", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/a/../b", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/..", unreachable_handler()), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("still_accepts_the_root_and_the_trailing_slash_a_request_path_really_does_carry") {
    Router router;
    router.get("/", constant_handler(std::string(kReadBody)));
    router.get("/thing/", constant_handler(std::string(kWriteBody)));

    CHECK(dispatch_wire(router, "GET", "/").body() == kReadBody);
    CHECK(dispatch_wire(router, "GET", "/thing/").body() == kWriteBody);
    CHECK(dispatch_wire(router, "GET", "/thing").status() == Status::NotFound);
}

TEST_CASE("rejects_a_mount_prefix_holding_a_segment_no_request_path_can_carry") {
    Router router;
    const AssetBundle bundle(kAbsoluteAssets);
    CHECK_THROWS_AS(router.mount("/a//b", bundle), ServerError);
    CHECK_THROWS_AS(router.mount("/assets/./icons", bundle), ServerError);
    CHECK_THROWS_AS(router.mount("/assets/../icons", bundle), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("rejects_a_registration_with_no_method_or_an_empty_handler") {
    Router router;
    CHECK_THROWS_AS(router.route(Method::None, "/thing", unreachable_handler()), ServerError);
    CHECK_THROWS_AS(router.get("/thing", Handler{}), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("refuses_a_request_that_carries_no_method_before_any_handler_can_see_it") {
    bool middleware_ran = false;
    bool fallback_ran = false;

    Router router;
    router.use([&middleware_ran](const Request&) -> std::optional<Response> {
        middleware_ran = true;
        return std::nullopt;
    });
    router.get("/thing", constant_handler(std::string(kReadBody)));
    router.fallback([&fallback_ran](const Request&) {
        fallback_ran = true;
        return Response::text(std::string(kFallbackBody));
    });

    Request unparsed;
    const Response response = router.dispatch(unparsed);
    CHECK(response.status() == Status::BadRequest);
    CHECK(response.body() == kBadRequestBody);
    CHECK_FALSE(middleware_ran);
    CHECK_FALSE(fallback_ran);
}

TEST_CASE("rejects_an_empty_middleware_and_a_second_fallback") {
    Router router;
    CHECK_THROWS_AS(router.use(Middleware{}), ServerError);

    router.fallback(constant_handler(std::string(kFallbackBody)));
    CHECK_THROWS_AS(router.fallback(unreachable_handler()), ServerError);
}

TEST_CASE("rejects_a_mount_prefix_that_is_not_a_literal_absolute_path") {
    Router router;
    const AssetBundle bundle(kAbsoluteAssets);
    CHECK_THROWS_AS(router.mount("assets", bundle), ServerError);
    CHECK_THROWS_AS(router.mount("", bundle), ServerError);
    CHECK_THROWS_AS(router.mount("/assets/*", bundle), ServerError);
    CHECK_THROWS_AS(router.mount("/assets/{name}", bundle), ServerError);
    CHECK(router.paths().empty());
}

TEST_CASE("get_also_registers_head_so_a_handler_never_has_to_remember_the_case") {
    Router router;
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));

    const Response answered_get = dispatch_wire(router, "GET", "/metrics");
    const Response answered_head = dispatch_wire(router, "HEAD", "/metrics");
    CHECK(answered_get.status() == Status::Ok);
    CHECK(answered_head.status() == Status::Ok);
    CHECK(answered_head.body() == kMetricsBody);

    const Response answered_post = dispatch_wire(router, "POST", "/metrics");
    CHECK(answered_post.status() == Status::MethodNotAllowed);
    CHECK(header_value(answered_post, kAllowField) == kGetHeadAllow);
}

TEST_CASE("answers_405_with_an_allow_header_listing_every_method_the_path_accepts") {
    Router router;
    router.get("/thing", constant_handler(std::string(kReadBody)));
    router.post("/thing", constant_handler(std::string(kWriteBody)));

    const Response response = dispatch_wire(router, "DELETE", "/thing");
    CHECK_MESSAGE(response.status() == Status::MethodNotAllowed,
                  "a known URL with an unregistered verb is 405, never 404");
    CHECK(response.status() != Status::NotFound);
    REQUIRE(has_header(response, kAllowField));
    CHECK(header_value(response, kAllowField) == kGetHeadPostAllow);
}

TEST_CASE("the_allow_header_unions_an_exact_and_a_parameter_route_on_the_same_path") {
    Router router;
    router.post("/thing", constant_handler(std::string(kWriteBody)));
    router.del("/{anything}", constant_handler(std::string(kSectionBody)));

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(response.status() == Status::MethodNotAllowed);
    CHECK(header_value(response, kAllowField) == kPostDeleteAllow);
}

TEST_CASE("answers_405_for_a_parameter_route_whose_verb_does_not_match") {
    Router router;
    router.post("/stats/{name}", constant_handler(std::string(kWriteBody)));

    const Response response = dispatch_wire(router, "GET", "/stats/live");
    CHECK(response.status() == Status::MethodNotAllowed);
    CHECK(header_value(response, kAllowField) == kPostAllow);
}

TEST_CASE("prefers_405_over_the_fallback_for_a_path_the_table_knows") {
    Router router;
    router.post("/thing", constant_handler(std::string(kWriteBody)));
    router.fallback(constant_handler(std::string(kFallbackBody)));

    const Response known_path = dispatch_wire(router, "GET", "/thing");
    CHECK(known_path.status() == Status::MethodNotAllowed);
    CHECK(known_path.body() != kFallbackBody);
    CHECK(header_value(known_path, kAllowField) == kPostAllow);

    const Response unknown_path = dispatch_wire(router, "GET", "/nowhere");
    CHECK(unknown_path.body() == kFallbackBody);
}

TEST_CASE("answers_a_plain_404_when_nothing_matches_and_no_fallback_is_installed") {
    Router router;
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));

    const Response response = dispatch_wire(router, "GET", "/nowhere");
    CHECK(response.status() == Status::NotFound);
    CHECK(response.body() == kDefaultNotFoundBody);
    CHECK(header_value(response, kContentTypeField) == kPlainTextContentType);
    CHECK_FALSE(has_header(response, kAllowField));
}

TEST_CASE("a_custom_fallback_replaces_the_default_404") {
    Router router;
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));
    router.fallback([](const Request&) {
        return Response::html(std::string(kFallbackBody), Status::NotFound);
    });

    const Response response = dispatch_wire(router, "GET", "/nowhere");
    CHECK(response.status() == Status::NotFound);
    CHECK(response.body() == kFallbackBody);
    CHECK(header_value(response, kContentTypeField) == kHtmlContentType);

    const Response matched = dispatch_wire(router, "GET", "/metrics");
    CHECK(matched.body() == kMetricsBody);
}

TEST_CASE("a_fallback_never_sees_a_capture_bound_by_a_route_that_did_not_match") {
    bool fallback_ran = false;
    std::string fallback_saw;

    Router router;
    router.get("/stats/{name}/detail", constant_handler(std::string(kNamedStatBody)));
    router.fallback([&fallback_ran, &fallback_saw](const Request& request) {
        fallback_ran = true;
        fallback_saw = std::string(request.param("name"));
        return Response::html(std::string(kFallbackBody), Status::NotFound);
    });

    const Response response = dispatch_wire(router, "GET", "/stats/live");
    CHECK(response.status() == Status::NotFound);
    REQUIRE(fallback_ran);
    CHECK_MESSAGE(fallback_saw.empty(),
                  "a capture from a route that failed to match must not reach the fallback");
}

TEST_CASE("middleware_runs_in_registration_order_before_the_handler") {
    std::vector<std::string> call_order;

    Router router;
    router.use([&call_order](const Request&) -> std::optional<Response> {
        call_order.push_back(std::string(kFirstMiddlewareName));
        return std::nullopt;
    });
    router.use([&call_order](const Request&) -> std::optional<Response> {
        call_order.push_back(std::string(kSecondMiddlewareName));
        return std::nullopt;
    });
    router.get("/thing", [&call_order](const Request&) {
        call_order.push_back(std::string(kHandlerName));
        return Response::text(std::string(kReadBody));
    });

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(response.body() == kReadBody);

    const std::vector<std::string> expected_order{std::string(kFirstMiddlewareName),
                                                  std::string(kSecondMiddlewareName),
                                                  std::string(kHandlerName)};
    CHECK(call_order == expected_order);
}

TEST_CASE("a_middleware_returning_nullopt_falls_through_to_the_handler") {
    bool middleware_ran = false;

    Router router;
    router.use([&middleware_ran](const Request&) -> std::optional<Response> {
        middleware_ran = true;
        return std::nullopt;
    });
    router.get("/thing", constant_handler(std::string(kReadBody)));

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(middleware_ran);
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kReadBody);
}

TEST_CASE("the_first_middleware_to_return_a_response_short_circuits_the_rest") {
    std::vector<std::string> call_order;

    Router router;
    router.use([&call_order](const Request&) -> std::optional<Response> {
        call_order.push_back(std::string(kFirstMiddlewareName));
        return Response::text(std::string(kShortCircuitBody), Status::Unauthorized);
    });
    router.use([&call_order](const Request&) -> std::optional<Response> {
        call_order.push_back(std::string(kSecondMiddlewareName));
        return std::nullopt;
    });
    router.get("/thing", [&call_order](const Request&) {
        call_order.push_back(std::string(kHandlerName));
        return Response::text(std::string(kReadBody));
    });

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(response.status() == Status::Unauthorized);
    CHECK(response.body() == kShortCircuitBody);
    CHECK(call_order == std::vector<std::string>{std::string(kFirstMiddlewareName)});
}

TEST_CASE("middleware_answers_before_routing_so_it_covers_paths_no_route_claims") {
    Router router;
    router.use([](const Request&) -> std::optional<Response> {
        return Response::text(std::string(kShortCircuitBody), Status::ServiceUnavailable);
    });
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));

    const Response unrouted = dispatch_wire(router, "GET", "/nowhere");
    CHECK(unrouted.status() == Status::ServiceUnavailable);
    const Response routed = dispatch_wire(router, "GET", "/metrics");
    CHECK(routed.status() == Status::ServiceUnavailable);
}

TEST_CASE("mount_serves_a_bundled_asset_borrowed_straight_from_static_storage") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const Response response = dispatch_wire(router, "GET", "/assets/app.css");
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kStyleBytes);
    CHECK_MESSAGE(response.body().data() == kStyleBytes.data(),
                  "a mounted asset must be served from the bundle's own storage, never copied");
    CHECK(header_value(response, kContentTypeField) == kCssContentType);
    CHECK(header_value(response, kEtagField) == kStyleEtag.view());
    CHECK(header_value(response, kCacheControlField) == kImmutableCacheControl);
}

TEST_CASE("mount_falls_through_when_the_bundle_does_not_hold_the_path") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));
    router.fallback(constant_handler(std::string(kFallbackBody)));

    const Response response = dispatch_wire(router, "GET", "/assets/missing.css");
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kFallbackBody);
}

TEST_CASE("a_second_mount_gets_its_turn_when_the_first_bundle_misses") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));
    router.mount("/assets", AssetBundle(kRelativeAssets));

    const Response from_first = dispatch_wire(router, "GET", "/assets/app.css");
    CHECK(from_first.body().data() == kStyleBytes.data());

    const Response from_second = dispatch_wire(router, "GET", "/assets/app.mjs");
    CHECK(from_second.status() == Status::Ok);
    CHECK(from_second.body().data() == kScriptBytes.data());
}

TEST_CASE("mount_leaves_a_path_outside_its_prefix_alone") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const Response response = dispatch_wire(router, "GET", "/assetsfoo");
    CHECK(response.status() == Status::NotFound);
}

TEST_CASE("mount_resolves_a_bundle_whose_paths_are_relative_to_the_mount_point") {
    Router router;
    router.mount("/static", AssetBundle(kRelativeAssets));

    const Response response = dispatch_wire(router, "GET", "/static/app.mjs");
    CHECK(response.status() == Status::Ok);
    CHECK(response.body().data() == kScriptBytes.data());
    CHECK(header_value(response, kContentTypeField) == kJavaScriptContentType);
}

TEST_CASE("a_mounted_asset_answers_304_when_if_none_match_names_its_etag") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kIfNoneMatchField, kStyleEtag.view());
    CHECK(response.status() == Status::NotModified);
    CHECK(response.body().empty());
    CHECK(header_value(response, kEtagField) == kStyleEtag.view());
    CHECK(header_value(response, kCacheControlField) == kImmutableCacheControl);
}

TEST_CASE("a_mounted_asset_answers_the_full_body_when_if_none_match_names_something_else") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kIfNoneMatchField, kScriptEtag.view());
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kStyleBytes);
}

TEST_CASE("a_mounted_precompressed_asset_is_served_when_the_request_accepts_gzip") {
    Router router;
    router.mount("/assets", AssetBundle(kCompressedAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kAcceptEncodingField, kGzipCoding);
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kCompressedStyleBytes);
    CHECK(header_value(response, kContentEncodingField) == kGzipCoding);
    CHECK(header_value(response, kVaryField) == kAcceptEncodingField);
}

TEST_CASE("a_mounted_precompressed_asset_answers_406_when_gzip_has_zero_weight") {
    Router router;
    router.mount("/assets", AssetBundle(kCompressedAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kAcceptEncodingField, "gzip;q=0");
    CHECK(response.status() == Status::NotAcceptable);
    CHECK(response.body() == "Not Acceptable\n");
    CHECK(header_value(response, kVaryField) == kAcceptEncodingField);
}

TEST_CASE("a_mounted_precompressed_asset_answers_406_to_an_identity_only_request") {
    Router router;
    router.mount("/assets", AssetBundle(kCompressedAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kAcceptEncodingField, "identity");
    CHECK(response.status() == Status::NotAcceptable);
    CHECK(header_value(response, kVaryField) == kAcceptEncodingField);
}

TEST_CASE("a_mounted_precompressed_asset_keeps_accept_encoding_in_its_304") {
    Router router;
    router.mount("/assets", AssetBundle(kCompressedAssets));

    const Response response = dispatch_wire_with_field(router, "GET", "/assets/app.css",
                                                       kIfNoneMatchField,
                                                       kCompressedStyleEtag.view());
    CHECK(response.status() == Status::NotModified);
    CHECK(response.body().empty());
    CHECK(header_value(response, kEtagField) == kCompressedStyleEtag.view());
    CHECK(header_value(response, kCacheControlField) == kImmutableCacheControl);
    CHECK_MESSAGE(header_value(response, kVaryField) == kAcceptEncodingField,
                  "a cache must not reuse a compressed validator for an identity-only request");
}

TEST_CASE("a_mounted_bundle_answers_405_for_a_verb_that_would_change_it") {
    Router router;
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const Response response = dispatch_wire(router, "POST", "/assets/app.css");
    CHECK(response.status() == Status::MethodNotAllowed);
    CHECK(header_value(response, kAllowField) == kGetHeadAllow);
}

TEST_CASE("paths_returns_every_registered_pattern_sorted_and_deduplicated") {
    Router router;
    router.get("/zebra", constant_handler(std::string(kReadBody)));
    router.get("/alpha", constant_handler(std::string(kReadBody)));
    router.post("/alpha", constant_handler(std::string(kWriteBody)));
    router.get("/stats/{name}", constant_handler(std::string(kNamedStatBody)));
    router.mount("/assets", AssetBundle(kAbsoluteAssets));

    const std::vector<std::string> expected{"/alpha", "/assets/*", "/stats/{name}", "/zebra"};
    CHECK(router.paths() == expected);
}

TEST_CASE("a_throwing_handler_becomes_a_500_instead_of_escaping_dispatch") {
    Router router;
    router.get("/boom", [](const Request&) -> Response {
        throw std::runtime_error(std::string(kHandlerFailureDetail));
    });

    const Response response = dispatch_wire(router, "GET", "/boom");
    CHECK(response.status() == Status::InternalServerError);
    CHECK(response.body() == kInternalErrorBody);
    CHECK_MESSAGE(response.body().find(kHandlerFailureDetail) == std::string_view::npos,
                  "an exception message is internal detail and must not reach the peer");
}

TEST_CASE("a_throwing_middleware_becomes_a_500_without_reaching_the_handler") {
    bool handler_ran = false;

    Router router;
    router.use([](const Request&) -> std::optional<Response> {
        throw std::runtime_error(std::string(kHandlerFailureDetail));
    });
    router.get("/thing", [&handler_ran](const Request&) {
        handler_ran = true;
        return Response::text(std::string(kReadBody));
    });

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(response.status() == Status::InternalServerError);
    CHECK_FALSE(handler_ran);
}

TEST_CASE("a_handler_throwing_something_other_than_a_std_exception_still_becomes_a_500") {
    Router router;
    router.get("/boom", [](const Request&) -> Response { throw ForeignFailure{}; });

    const Response response = dispatch_wire(router, "GET", "/boom");
    CHECK(response.status() == Status::InternalServerError);
    CHECK(response.body() == kInternalErrorBody);
}

TEST_CASE("a_middleware_throwing_something_other_than_a_std_exception_still_becomes_a_500") {
    Router router;
    router.use([](const Request&) -> std::optional<Response> { throw ForeignFailure{}; });
    router.get("/thing", constant_handler(std::string(kReadBody)));

    const Response response = dispatch_wire(router, "GET", "/thing");
    CHECK(response.status() == Status::InternalServerError);
}

TEST_CASE("a_moved_router_still_dispatches_every_route_it_owned") {
    Router original;
    original.get("/metrics", constant_handler(std::string(kMetricsBody)));
    original.get("/stats/{name}", constant_handler(std::string(kNamedStatBody)));
    original.mount("/assets", AssetBundle(kAbsoluteAssets));
    original.fallback(constant_handler(std::string(kFallbackBody)));

    Router moved = std::move(original);

    const Response metrics = dispatch_wire(moved, "GET", "/metrics");
    const Response named = dispatch_wire(moved, "GET", "/stats/live");
    const Response asset = dispatch_wire(moved, "GET", "/assets/app.css");
    const Response unknown = dispatch_wire(moved, "GET", "/nowhere");
    CHECK(metrics.body() == kMetricsBody);
    CHECK(named.body() == kNamedStatBody);
    CHECK(asset.body() == kStyleBytes);
    CHECK(unknown.body() == kFallbackBody);

    const std::vector<std::string> expected{"/assets/*", "/metrics", "/stats/{name}"};
    CHECK(moved.paths() == expected);

    Router reassigned;
    reassigned = std::move(moved);
    const Response after_assignment = dispatch_wire(reassigned, "GET", "/metrics");
    CHECK(after_assignment.body() == kMetricsBody);
}

TEST_CASE("dispatches_the_whole_pipeline_offline_with_no_socket_and_no_reactor") {
    std::vector<std::string> observed_paths;

    Router router;
    router.use([&observed_paths](const Request& request) -> std::optional<Response> {
        observed_paths.push_back(std::string(request.path()));
        return std::nullopt;
    });
    router.get("/metrics", constant_handler(std::string(kMetricsBody)));
    router.get("/stats/client/{address}", constant_handler(std::string(kClientBody)));
    router.post("/thing", constant_handler(std::string(kWriteBody)));
    router.mount("/assets", AssetBundle(kAbsoluteAssets));
    router.fallback([](const Request&) {
        return Response::html(std::string(kFallbackBody), Status::NotFound);
    });

    const Response metrics = dispatch_wire(router, "GET", "/metrics");
    const Response client = dispatch_wire(router, "GET", "/stats/client/bc1qexample");
    const Response asset = dispatch_wire(router, "HEAD", "/assets/app.css");
    const Response wrong_verb = dispatch_wire(router, "GET", "/thing");
    const Response unknown = dispatch_wire(router, "GET", "/nowhere");

    CHECK(metrics.body() == kMetricsBody);
    CHECK(client.body() == kClientBody);
    CHECK(asset.body() == kStyleBytes);
    CHECK(wrong_verb.status() == Status::MethodNotAllowed);
    CHECK(unknown.status() == Status::NotFound);
    CHECK(unknown.body() == kFallbackBody);

    const std::vector<std::string> expected_paths{"/metrics", "/stats/client/bc1qexample",
                                                  "/assets/app.css", "/thing", "/nowhere"};
    CHECK(observed_paths == expected_paths);
}
