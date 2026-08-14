
#define ERIKSLUND_HTTP_JSON_SCHEMA 1

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "erikslund/http/json.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/status.hpp"

namespace erikslund::http {

namespace {

struct RequestFixture {
    std::unique_ptr<std::string> wire{};
    Request request{};
};

[[nodiscard]] std::string wire_for(std::string_view content_type, std::string_view body) {
    std::string wire = "POST /api/config HTTP/1.1\r\nHost: box.lan\r\n";
    if (!content_type.empty()) {
        wire += "Content-Type: ";
        wire += content_type;
        wire += "\r\n";
    }
    wire += "Content-Length: ";
    wire += std::to_string(body.size());
    wire += "\r\n\r\n";
    wire += body;
    return wire;
}

[[nodiscard]] RequestFixture posted(std::string_view content_type, std::string_view body) {
    RequestFixture fixture;
    fixture.wire = std::make_unique<std::string>(wire_for(content_type, body));
    auto parsed = parse_request(*fixture.wire, RequestLimits{});
    REQUIRE_MESSAGE(parsed.has_value(), "the test's own request bytes must parse");
    fixture.request = std::move(parsed->request);
    return fixture;
}

enum class Health : uint8_t { Ready, Degraded, Down };

struct RouteSnapshot {
    std::string pattern{};
    uint32_t hits = 0;
};

struct ServiceSnapshot {
    std::string service_name{};
    std::vector<uint32_t> recent_latencies_ms{};
    std::optional<std::string> last_error{};
    Health health = Health::Ready;
    RouteSnapshot busiest_route{};
    std::vector<RouteSnapshot> routes{};
};

[[nodiscard]] ServiceSnapshot populated_snapshot() {
    return ServiceSnapshot{
        .service_name = "erikslund-http",
        .recent_latencies_ms = {3, 11, 4},
        .last_error = std::optional<std::string>{"upstream refused the connection"},
        .health = Health::Degraded,
        .busiest_route = RouteSnapshot{.pattern = "/metrics", .hits = 4'211},
        .routes = {RouteSnapshot{.pattern = "/", .hits = 91},
                   RouteSnapshot{.pattern = "/metrics", .hits = 4'211}}};
}

} // namespace

TEST_CASE("every json error carries its own non-empty explanatory sentence") {
    constexpr std::array<JsonError, 4> kEveryError{JsonError::NotJson, JsonError::Malformed,
                                                   JsonError::TypeMismatch, JsonError::TooLarge};
    for (const JsonError error : kEveryError)
        CHECK_FALSE(json_error_message(error).empty());

    for (size_t left = 0; left < kEveryError.size(); ++left)
        for (size_t right = left + 1; right < kEveryError.size(); ++right)
            CHECK(json_error_message(kEveryError[left]) != json_error_message(kEveryError[right]));
}

TEST_CASE("only an oversized body answers 413 and every other json error answers 400") {
    CHECK(json_error_response(JsonError::TooLarge).status() == Status::ContentTooLarge);
    CHECK(json_error_response(JsonError::NotJson).status() == Status::BadRequest);
    CHECK(json_error_response(JsonError::Malformed).status() == Status::BadRequest);
    CHECK(json_error_response(JsonError::TypeMismatch).status() == Status::BadRequest);
}

TEST_CASE("a json error response is itself a json document") {
    const Response response = json_error_response(JsonError::Malformed);
    REQUIRE(response.headers().contains("Content-Type"));
    CHECK(response.headers().at("Content-Type") == "application/json");
    CHECK(response.body().starts_with(R"({"error":")"));
    CHECK(response.body().ends_with(R"("})"));
    CHECK(response.body().find(json_error_message(JsonError::Malformed)) != std::string_view::npos);
}

TEST_CASE("a body with no content type is rejected as not json rather than parsed anyway") {
    const RequestFixture fixture = posted("", R"({"service_name":"x"})");
    const auto body = json_body_of(fixture.request, kDefaultMaxJsonBodyBytes);
    REQUIRE_FALSE(body.has_value());
    CHECK(body.error() == JsonError::NotJson);
}

TEST_CASE("a body labelled text plain is rejected as not json") {
    const RequestFixture fixture = posted("text/plain; charset=utf-8", R"({"service_name":"x"})");
    const auto body = json_body_of(fixture.request, kDefaultMaxJsonBodyBytes);
    REQUIRE_FALSE(body.has_value());
    CHECK(body.error() == JsonError::NotJson);
}

TEST_CASE("an absent body is rejected as not json rather than as a malformed document") {
    const RequestFixture fixture = posted("application/json", "");
    const auto body = json_body_of(fixture.request, kDefaultMaxJsonBodyBytes);
    REQUIRE_FALSE(body.has_value());
    CHECK(body.error() == JsonError::NotJson);
}

TEST_CASE("the json media type is matched without regard to letter case or parameters") {
    const RequestFixture plain = posted("application/json", "{}");
    CHECK(json_body_of(plain.request, kDefaultMaxJsonBodyBytes).has_value());

    const RequestFixture mixed_case = posted("Application/JSON", "{}");
    CHECK(json_body_of(mixed_case.request, kDefaultMaxJsonBodyBytes).has_value());

    const RequestFixture parameterised = posted("application/json; charset=utf-8", "{}");
    CHECK(json_body_of(parameterised.request, kDefaultMaxJsonBodyBytes).has_value());

    const RequestFixture padded = posted("  application/json  ", "{}");
    CHECK(json_body_of(padded.request, kDefaultMaxJsonBodyBytes).has_value());
}

TEST_CASE("a structured syntax suffix media type is read by the same json reader") {
    const RequestFixture merge_patch = posted("application/merge-patch+json", "{}");
    CHECK(json_body_of(merge_patch.request, kDefaultMaxJsonBodyBytes).has_value());

    const RequestFixture problem = posted("application/problem+json", "{}");
    CHECK(json_body_of(problem.request, kDefaultMaxJsonBodyBytes).has_value());

    const RequestFixture bare_suffix = posted("+json", "{}");
    const auto body = json_body_of(bare_suffix.request, kDefaultMaxJsonBodyBytes);
    REQUIRE_FALSE(body.has_value());
    CHECK(body.error() == JsonError::NotJson);
}

TEST_CASE("the route's own size ceiling is enforced before the reader sees a byte") {
    constexpr size_t kRouteCeilingBytes = 8;
    const RequestFixture fixture = posted("application/json", R"({"a":"bbbb"})");
    REQUIRE(fixture.request.body().size() > kRouteCeilingBytes);

    const auto body = json_body_of(fixture.request, kRouteCeilingBytes);
    REQUIRE_FALSE(body.has_value());
    CHECK(body.error() == JsonError::TooLarge);
}

TEST_CASE("a body exactly on the route ceiling is accepted") {
    const RequestFixture fixture = posted("application/json", "{}");
    const auto body = json_body_of(fixture.request, fixture.request.body().size());
    REQUIRE(body.has_value());
    CHECK(*body == "{}");
}

TEST_CASE("a nested aggregate round trips through pure compile-time reflection") {
    const ServiceSnapshot original = populated_snapshot();
    const Response response = json_response(original);

    REQUIRE(response.status() == Status::Ok);
    REQUIRE(response.headers().contains("Content-Type"));
    CHECK(response.headers().at("Content-Type") == "application/json");

    const RequestFixture fixture = posted("application/json", response.body());
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE_MESSAGE(parsed.has_value(), "the library's own output must be readable by its reader");

    CHECK(parsed->service_name == original.service_name);
    CHECK(parsed->recent_latencies_ms == original.recent_latencies_ms);
    CHECK(parsed->last_error == original.last_error);
    CHECK(parsed->health == original.health);
    CHECK(parsed->busiest_route.pattern == original.busiest_route.pattern);
    CHECK(parsed->busiest_route.hits == original.busiest_route.hits);
    REQUIRE(parsed->routes.size() == original.routes.size());
    for (size_t index = 0; index < original.routes.size(); ++index) {
        CHECK(parsed->routes[index].pattern == original.routes[index].pattern);
        CHECK(parsed->routes[index].hits == original.routes[index].hits);
    }

    const Response reserialized = json_response(*parsed);
    CHECK(reserialized.body() == response.body());
}

TEST_CASE("an engaged optional member is written and an empty one is omitted entirely") {
    ServiceSnapshot snapshot = populated_snapshot();
    const Response engaged = json_response(snapshot);
    CHECK(engaged.body().find(R"("last_error":)") != std::string_view::npos);

    snapshot.last_error.reset();
    const Response empty = json_response(snapshot);
    CHECK(empty.body().find(R"("last_error")") == std::string_view::npos);
}

TEST_CASE("an enum member is serialized as its ordinal rather than its name") {
    ServiceSnapshot snapshot = populated_snapshot();
    snapshot.health = Health::Down;
    const Response response = json_response(snapshot);
    CHECK(response.body().find(R"("health":2)") != std::string_view::npos);
}

TEST_CASE("the prettified writer produces the same document with indentation") {
    const ServiceSnapshot original = populated_snapshot();
    const Response compact = json_response(original);
    const Response pretty = json_response_pretty(original);

    CHECK(pretty.body().size() > compact.body().size());
    CHECK(pretty.body().find('\n') != std::string_view::npos);

    const RequestFixture fixture = posted("application/json", pretty.body());
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE(parsed.has_value());
    const Response recompacted = json_response(*parsed);
    CHECK(recompacted.body() == compact.body());
}

TEST_CASE("a non-default status is carried through to the response") {
    const Response response = json_response(populated_snapshot(), Status::ServiceUnavailable);
    CHECK(response.status() == Status::ServiceUnavailable);
}

TEST_CASE("parse_json_body fills the aggregate from a well-formed body") {
    const RequestFixture fixture = posted(
        "application/json",
        R"({"service_name":"probe","recent_latencies_ms":[1,2],"health":1,)"
        R"("busiest_route":{"pattern":"/","hits":5},"routes":[]})");

    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE(parsed.has_value());
    CHECK(parsed->service_name == "probe");
    CHECK(parsed->recent_latencies_ms == std::vector<uint32_t>{1, 2});
    CHECK_FALSE(parsed->last_error.has_value());
    CHECK(parsed->health == Health::Degraded);
    CHECK(parsed->busiest_route.hits == 5);
    CHECK(parsed->routes.empty());
}

TEST_CASE("parse_json_body reports a wrong content type as not json") {
    const RequestFixture fixture = posted("text/plain", R"({"service_name":"probe"})");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == JsonError::NotJson);
}

TEST_CASE("parse_json_body reports an oversized body as too large") {
    constexpr size_t kRouteCeilingBytes = 4;
    const RequestFixture fixture = posted("application/json", R"({"service_name":"probe"})");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request, kRouteCeilingBytes);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == JsonError::TooLarge);
}

TEST_CASE("parse_json_body reports a document that is not well-formed as malformed") {
    const RequestFixture fixture = posted("application/json", R"({"service_name":"probe")");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == JsonError::Malformed);
}

TEST_CASE("parse_json_body reports a type mismatch distinctly from a malformed document") {
    const RequestFixture fixture =
        posted("application/json", R"({"busiest_route":{"pattern":"/","hits":"many"}})");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE_FALSE(parsed.has_value());
    CHECK_MESSAGE(parsed.error() == JsonError::TypeMismatch,
                  "a well-formed document with a wrong field type is not a malformed document");
}

TEST_CASE("parse_json_body refuses an unknown key rather than ignoring it") {
    const RequestFixture fixture = posted("application/json", R"({"service_nmae":"probe"})");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    CHECK_FALSE(parsed.has_value());
}

TEST_CASE("an empty json object parses into a fully defaulted aggregate") {
    const RequestFixture fixture = posted("application/json", "{}");
    const auto parsed = parse_json_body<ServiceSnapshot>(fixture.request);
    REQUIRE(parsed.has_value());
    CHECK(parsed->service_name.empty());
    CHECK(parsed->recent_latencies_ms.empty());
    CHECK_FALSE(parsed->last_error.has_value());
    CHECK(parsed->health == Health::Ready);
}

TEST_CASE("json_schema_for describes every member of an aggregate") {
    const std::string schema = json_schema_for<ServiceSnapshot>();
    REQUIRE_FALSE(schema.empty());

    CHECK(schema.find(R"("title":)") != std::string::npos);
    CHECK(schema.find("ServiceSnapshot") != std::string::npos);
    CHECK(schema.find(R"("service_name")") != std::string::npos);
    CHECK(schema.find(R"("recent_latencies_ms")") != std::string::npos);
    CHECK(schema.find(R"("last_error")") != std::string::npos);
    CHECK(schema.find(R"("health")") != std::string::npos);
    CHECK(schema.find(R"("busiest_route")") != std::string::npos);
    CHECK(schema.find(R"("routes")") != std::string::npos);

    CHECK(schema.find(R"("additionalProperties":false)") != std::string::npos);

    CHECK(schema.find(R"("$defs")") != std::string::npos);
    CHECK(schema.find("RouteSnapshot") != std::string::npos);
    CHECK(schema.find("#/$defs/") != std::string::npos);
}

TEST_CASE("the generated schema does not constrain an enum member") {
    const std::string schema = json_schema_for<ServiceSnapshot>();
    CHECK(schema.find(R"("enum")") == std::string::npos);
}

} // namespace erikslund::http
