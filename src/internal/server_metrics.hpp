#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/status.hpp"

namespace erikslund::http::internal {

enum class ConnectionRejection : uint8_t { Limit, SourceLimit, Cidr, Parse, Tls };

class ServerMetrics {
public:
    explicit ServerMetrics(MetricsRegistry& registry);

    void connection_accepted() noexcept;
    void connections_active_changed(int64_t delta) noexcept;
    void connection_rejected(ConnectionRejection reason) noexcept;
    void bytes_written(size_t bytes) noexcept;
    void tls_handshake_completed() noexcept;
    void tls_handshake_failed() noexcept;
    void response_completed(
        Status status,
        std::optional<std::chrono::duration<double>> service_time = std::nullopt) noexcept;

private:
    [[nodiscard]] Counter& response_bucket(Status status) const noexcept;

    Counter* requests_2xx_ = nullptr;
    Counter* requests_3xx_ = nullptr;
    Counter* requests_4xx_ = nullptr;
    Counter* requests_5xx_ = nullptr;
    Counter* connections_accepted_ = nullptr;
    Gauge* connections_active_ = nullptr;
    Counter* rejected_limit_ = nullptr;
    Counter* rejected_source_limit_ = nullptr;
    Counter* rejected_cidr_ = nullptr;
    Counter* rejected_parse_ = nullptr;
    Counter* rejected_tls_ = nullptr;
    Counter* bytes_sent_ = nullptr;
    Gauge* request_duration_seconds_sum_ = nullptr;
    Counter* request_duration_count_ = nullptr;
    Counter* tls_handshakes_ = nullptr;
    Counter* tls_handshake_failures_ = nullptr;
};

} // namespace erikslund::http::internal
