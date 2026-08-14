

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/metrics.hpp"
#include "erikslund/http/reflect.hpp"
#include "erikslund/http/status_page.hpp"

namespace erikslund::http {

TEST_CASE("the reflection helpers are compiled exactly when the build enables them") {
    CHECK(kReflectionEnabled == (ERIKSLUND_HTTP_REFLECTION != 0));
    if constexpr (!kReflectionEnabled)
        MESSAGE("reflection is off in THIS CONFIGURATION, so the cases below this one were not "
                "compiled into the binary at all -- they are absent, not skipped at runtime");
}

#if ERIKSLUND_HTTP_REFLECTION

namespace {

struct ReactorSnapshot {
    uint64_t active_connections = 0;
    uint64_t requests_total = 0;
    double mean_latency_ms = 0.0;
    std::string worker_name{};
};

constexpr std::string_view kMetricPrefix = "erikslund_http_test";

[[nodiscard]] ReactorSnapshot sample_snapshot() {
    return ReactorSnapshot{.active_connections = 7,
                           .requests_total = 1'204,
                           .mean_latency_ms = 1.5,
                           .worker_name = "reactor-0"};
}

[[nodiscard]] std::string row_markup(std::string_view label, std::string_view value) {
    return std::format("<tr><td>{}</td><td>{}</td></tr>", label, value);
}

} // namespace

TEST_CASE("add_rows_from derives one row label per member from the member's own name") {
    StatusPage page("erikslund-http", "0.1.2");
    StatusPage& returned = add_rows_from(page, sample_snapshot());
    CHECK(&returned == &page);

    const std::string rendered = page.render();
    CHECK(rendered.find(row_markup("active connections", "7")) != std::string::npos);
    CHECK(rendered.find(row_markup("requests total", "1204")) != std::string::npos);
    CHECK(rendered.find(row_markup("mean latency ms", "1.5")) != std::string::npos);
    CHECK(rendered.find(row_markup("worker name", "reactor-0")) != std::string::npos);

    CHECK(rendered.find("active_connections") == std::string::npos);
    CHECK(rendered.find("mean_latency_ms") == std::string::npos);
}

TEST_CASE("add_rows_from emits the rows in declaration order") {
    StatusPage page("erikslund-http", "0.1.2");
    add_rows_from(page, sample_snapshot());
    const std::string rendered = page.render();

    const size_t active = rendered.find("<tr><td>active connections</td>");
    const size_t requests = rendered.find("<tr><td>requests total</td>");
    const size_t latency = rendered.find("<tr><td>mean latency ms</td>");
    const size_t worker = rendered.find("<tr><td>worker name</td>");

    REQUIRE(active != std::string::npos);
    REQUIRE(requests != std::string::npos);
    REQUIRE(latency != std::string::npos);
    REQUIRE(worker != std::string::npos);
    CHECK(active < requests);
    CHECK(requests < latency);
    CHECK(latency < worker);
}

TEST_CASE("add_rows_from walks only the declared members and not the implicit special members") {
    constexpr size_t kDeclaredMemberCount = 4;
    constexpr std::string_view kRowOpening = "<tr><td>";

    StatusPage page("erikslund-http", "0.1.2");
    add_rows_from(page, sample_snapshot());
    const std::string rendered = page.render();

    size_t rows = 0;
    for (size_t at = rendered.find(kRowOpening); at != std::string::npos;
         at = rendered.find(kRowOpening, at + kRowOpening.size()))
        ++rows;
    CHECK(rows == kDeclaredMemberCount);
}

TEST_CASE("a reflected row is escaped by the same path every other row goes through") {
    ReactorSnapshot hostile = sample_snapshot();
    hostile.worker_name = R"(<script>alert("x")</script>)";

    StatusPage page("erikslund-http", "0.1.2");
    add_rows_from(page, hostile);
    const std::string rendered = page.render();

    CHECK(rendered.find("<script>") == std::string::npos);
    CHECK(rendered.find("&lt;script&gt;") != std::string::npos);
    CHECK(rendered.find("&quot;x&quot;") != std::string::npos);
}

TEST_CASE("register_gauges_from names one gauge per arithmetic member after the member") {
    MetricsRegistry registry{std::string(kMetricPrefix)};
    register_gauges_from<ReactorSnapshot>(registry, "reactor state",
                                          [] { return sample_snapshot(); });

    const std::string exposition = registry.prometheus();

    CHECK(exposition.find("erikslund_http_test_active_connections 7\n") != std::string::npos);
    CHECK(exposition.find("erikslund_http_test_mean_latency_ms 1.5\n") != std::string::npos);

    CHECK(exposition.find("erikslund_http_test_requests_total 1204\n") != std::string::npos);
    CHECK(exposition.find("# TYPE erikslund_http_test_requests_total gauge") != std::string::npos);

    CHECK(exposition.find("active connections") == std::string::npos);

    CHECK(exposition.find("# TYPE erikslund_http_test_active_connections gauge") !=
          std::string::npos);
    CHECK(exposition.find("# HELP erikslund_http_test_active_connections reactor state") !=
          std::string::npos);
}

TEST_CASE("register_gauges_from skips a member no prometheus sample could carry") {
    MetricsRegistry registry{std::string(kMetricPrefix)};
    register_gauges_from<ReactorSnapshot>(registry, "reactor state",
                                          [] { return sample_snapshot(); });

    const std::string exposition = registry.prometheus();
    CHECK(exposition.find("worker_name") == std::string::npos);
    CHECK(exposition.find("reactor-0") == std::string::npos);
}

TEST_CASE("a reflected gauge is computed at scrape time rather than captured once") {
    const auto live_connections = std::make_shared<uint64_t>(1);

    MetricsRegistry registry{std::string(kMetricPrefix)};
    register_gauges_from<ReactorSnapshot>(registry, "reactor state", [live_connections] {
        ReactorSnapshot snapshot = sample_snapshot();
        snapshot.active_connections = *live_connections;
        return snapshot;
    });

    CHECK(registry.prometheus().find("erikslund_http_test_active_connections 1\n") !=
          std::string::npos);

    *live_connections = 42;
    CHECK(registry.prometheus().find("erikslund_http_test_active_connections 42\n") !=
          std::string::npos);
}

#endif // ERIKSLUND_HTTP_REFLECTION

} // namespace erikslund::http
