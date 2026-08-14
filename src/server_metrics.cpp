#include "internal/server_metrics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/status.hpp"

namespace erikslund::http::internal {

ServerMetrics::ServerMetrics(MetricsRegistry& registry) {
    const std::string requests_help = "HTTP responses served, by status class";
    requests_2xx_ = &registry.counter("requests", requests_help, {{"status", "2xx"}});
    requests_3xx_ = &registry.counter("requests", requests_help, {{"status", "3xx"}});
    requests_4xx_ = &registry.counter("requests", requests_help, {{"status", "4xx"}});
    requests_5xx_ = &registry.counter("requests", requests_help, {{"status", "5xx"}});

    connections_accepted_ =
        &registry.counter("connections_accepted", "Connections accepted by the server");
    connections_active_ =
        &registry.gauge("connections_active", "Connections currently held open");

    const std::string rejected_help = "Connections closed before a response, by reason";
    rejected_limit_ =
        &registry.counter("connections_rejected", rejected_help, {{"reason", "limit"}});
    rejected_source_limit_ = &registry.counter("connections_rejected", rejected_help,
                                                {{"reason", "source_limit"}});
    rejected_cidr_ =
        &registry.counter("connections_rejected", rejected_help, {{"reason", "cidr"}});
    rejected_parse_ =
        &registry.counter("connections_rejected", rejected_help, {{"reason", "parse"}});
    rejected_tls_ =
        &registry.counter("connections_rejected", rejected_help, {{"reason", "tls"}});

    bytes_sent_ = &registry.counter("bytes_sent", "Response bytes written to sockets");
    request_duration_seconds_sum_ =
        &registry.gauge("request_duration_seconds_sum", "Cumulative request service time");
    request_duration_count_ =
        &registry.counter("request_duration_seconds_count", "Requests contributing to the sum");
    tls_handshakes_ = &registry.counter("tls_handshakes", "TLS handshakes completed");
    tls_handshake_failures_ =
        &registry.counter("tls_handshake_failures", "TLS handshakes that failed or timed out");
}

Counter& ServerMetrics::response_bucket(Status status) const noexcept {
    const std::string_view bucket = status_class(status);
    if (bucket == "2xx")
        return *requests_2xx_;
    if (bucket == "3xx")
        return *requests_3xx_;
    if (bucket == "4xx")
        return *requests_4xx_;
    return *requests_5xx_;
}

void ServerMetrics::connection_accepted() noexcept {
    connections_accepted_->increment();
}

void ServerMetrics::connections_active_changed(int64_t delta) noexcept {
    connections_active_->add(static_cast<double>(delta));
}

void ServerMetrics::connection_rejected(ConnectionRejection reason) noexcept {
    switch (reason) {
    case ConnectionRejection::Limit:
        rejected_limit_->increment();
        break;
    case ConnectionRejection::SourceLimit:
        rejected_source_limit_->increment();
        break;
    case ConnectionRejection::Cidr:
        rejected_cidr_->increment();
        break;
    case ConnectionRejection::Parse:
        rejected_parse_->increment();
        break;
    case ConnectionRejection::Tls:
        rejected_tls_->increment();
        break;
    }
}

void ServerMetrics::bytes_written(size_t bytes) noexcept {
    bytes_sent_->increment(bytes);
}

void ServerMetrics::tls_handshake_completed() noexcept {
    tls_handshakes_->increment();
}

void ServerMetrics::tls_handshake_failed() noexcept {
    tls_handshake_failures_->increment();
}

void ServerMetrics::response_completed(
    Status status, std::optional<std::chrono::duration<double>> service_time) noexcept {
    response_bucket(status).increment();
    request_duration_count_->increment();
    if (service_time.has_value())
        request_duration_seconds_sum_->add(service_time->count());
}

} // namespace erikslund::http::internal
