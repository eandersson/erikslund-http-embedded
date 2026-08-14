#include "erikslund/http/observability.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/metrics.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/sse.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/status_page.hpp"
#include "erikslund/http/text.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kStatusPath = "/";
constexpr std::string_view kStatusAliasPath = "/status";
constexpr std::string_view kMetricsPath = "/metrics";
constexpr std::string_view kMetricsJsonPath = "/metrics.json";
constexpr std::string_view kHealthPath = "/health";
constexpr std::string_view kHealthzPath = "/healthz";
constexpr std::string_view kFaviconPath = "/favicon.ico";

constexpr std::string_view kHealthyBody = "ok\n";
constexpr std::string_view kDegradedBody = "degraded\n";

constexpr std::string_view kReadyHeadline = "READY";
constexpr std::string_view kDegradedHeadline = "DEGRADED";

constexpr std::string_view kUptimeRowLabel = "uptime";

constexpr std::string_view kLivePolicyBeforeScriptSource =
    "default-src 'self'; style-src 'unsafe-inline'; script-src ";
constexpr std::string_view kLivePolicyAfterScriptSource =
    "; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'; "
    "object-src 'none'";

[[nodiscard]] std::string live_content_security_policy() {
    const std::string_view hash_source = live_update_script_csp_source();
    std::string policy;
    policy.reserve(kLivePolicyBeforeScriptSource.size() + hash_source.size() +
                   kLivePolicyAfterScriptSource.size());
    policy += kLivePolicyBeforeScriptSource;
    policy += hash_source;
    policy += kLivePolicyAfterScriptSource;
    return policy;
}

constexpr size_t kJsonSizeFactor = 2;
constexpr size_t kJsonScaffoldBytes = 64;

struct PageContext {
    std::string service_name;
    std::string version;
    std::string content_security_policy;
    std::function<StatusPage()> status_page;
    std::function<bool()> healthy;
    std::chrono::steady_clock::time_point started_at;
    int process_id = 0;
    bool metrics_mounted = false;
    bool live_updates = false;
};

[[nodiscard]] bool is_healthy(const std::function<bool()>& predicate) {
    return !predicate || predicate();
}

[[nodiscard]] StatusPage default_status_page(const PageContext& context) {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - context.started_at);

    StatusPage page(context.service_name, context.version);
    page.pid(context.process_id);
    const bool healthy = is_healthy(context.healthy);
    page.state(std::string(healthy ? kReadyHeadline : kDegradedHeadline),
               healthy ? State::Ok : State::Bad);
    page.row(kUptimeRowLabel, format_duration(uptime));
    page.link(kStatusPath, kStatusPath);
    if (context.metrics_mounted)
        page.link(kMetricsPath, kMetricsPath);
    page.link(kHealthzPath, kHealthzPath);
    return page;
}

[[nodiscard]] Response render_status_page(const PageContext& context) {
    StatusPage page = context.status_page ? context.status_page() : default_status_page(context);
    if (context.live_updates)
        page.live_updates(true);

    Response response = Response::html(page.render());
    if (context.live_updates)
        response.header("Content-Security-Policy", context.content_security_policy);
    return response;
}

[[nodiscard]] std::string labels_to_json(const Labels& labels) {
    std::string out = "{";
    bool first = true;
    for (const auto& label : labels) {
        if (!first)
            out += ',';
        first = false;
        out += '"';
        out += json_escape(label.first);
        out += "\":\"";
        out += json_escape(label.second);
        out += '"';
    }
    out += '}';
    return out;
}

[[nodiscard]] std::string value_to_json(std::string_view token)
    ERIKSLUND_HTTP_POST(result: !result.empty()) {
    constexpr std::string_view kJsonNull = "null";
    if (token.empty() || token.starts_with('+'))
        return std::string(kJsonNull);
    for (const char character : token) {
        const bool is_number_character = (character >= '0' && character <= '9') ||
                                         character == '-' || character == '.' ||
                                         character == 'e' || character == 'E' || character == '+';
        if (!is_number_character)
            return std::string(kJsonNull);
    }
    return std::string(token);
}

[[nodiscard]] std::string snapshot_to_json(std::string_view prefix,
                                           const MetricsSnapshot& snapshot) {
    size_t series_count = 0;
    for (const MetricSnapshot& metric : snapshot)
        series_count += metric.series.size();
    std::string out;
    out.reserve(series_count * kJsonSizeFactor * kJsonScaffoldBytes + kJsonScaffoldBytes);
    out += R"({"prefix":")";
    out += json_escape(prefix);
    out += R"(","metrics":[)";
    bool first_metric = true;
    for (const MetricSnapshot& metric : snapshot) {
        if (!first_metric)
            out += ',';
        first_metric = false;
        out += R"({"name":")";
        out += json_escape(metric.name);
        out += R"(","type":")";
        out += metric.kind == MetricKind::Counter ? "counter" : "gauge";
        out += R"(","help":")";
        out += json_escape(metric.help);
        out += R"(","series":[)";
        bool first_series = true;
        for (const MetricSeriesSnapshot& entry : metric.series) {
            if (!first_series)
                out += ',';
            first_series = false;
            out += R"({"labels":)";
            out += labels_to_json(entry.labels);
            out += R"(,"value":)";
            out += value_to_json(entry.value);
            out += '}';
        }
        out += "]}";
    }
    out += "]}\n";
    return out;
}

} // namespace

void mount_observability(Router& router, ObservabilityOptions options) {
    std::function<bool()> health_predicate = options.healthy;

    auto context = std::make_shared<PageContext>();
    context->service_name = std::move(options.service_name);
    context->version = std::move(options.version);
    context->status_page = std::move(options.status_page);
    context->healthy = std::move(options.healthy);
    context->started_at = std::chrono::steady_clock::now();
    context->process_id = static_cast<int>(::getpid());
    context->metrics_mounted = options.metrics != nullptr;
    context->live_updates = options.live != nullptr;
    if (context->live_updates)
        context->content_security_policy = live_content_security_policy();

    router.get(kStatusPath, [context](const Request&) { return render_status_page(*context); });
    router.get(kStatusAliasPath,
               [context](const Request&) { return render_status_page(*context); });

    const auto health_handler = [health_predicate](const Request&) {
        return is_healthy(health_predicate)
                   ? Response::text(std::string(kHealthyBody))
                   : Response::text(std::string(kDegradedBody), Status::ServiceUnavailable);
    };
    router.get(kHealthPath, health_handler);
    router.get(kHealthzPath, health_handler);

    if (options.metrics != nullptr) {
        MetricsRegistry* registry = options.metrics;
        router.get(kMetricsPath, [registry](const Request&) {
            return Response::prometheus(registry->prometheus());
        });

        router.get(kMetricsJsonPath, [registry](const Request&) {
            return Response::json(snapshot_to_json(registry->prefix(), registry->snapshot()));
        });
    }

    if (options.live != nullptr)
        router.route(Method::Get, kDefaultEventsPath, options.live->handler());

    router.get(kFaviconPath, [](const Request&) { return Response::empty(Status::NoContent); });
}

} // namespace erikslund::http
