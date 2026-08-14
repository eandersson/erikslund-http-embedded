#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace erikslund::http {

// Monotonic and thread-safe. Its value carries no synchronization relationship.
class Counter {
public:
    Counter() = default;
    Counter(const Counter&) = delete("a Counter is identified by its address in the registry");
    Counter& operator=(const Counter&) = delete("a Counter is identified by its address");

    void increment(uint64_t amount = 1) noexcept {
        value_.fetch_add(amount, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> value_{0};
};

class Gauge {
public:
    Gauge() = default;
    Gauge(const Gauge&) = delete("a Gauge is identified by its address in the registry");
    Gauge& operator=(const Gauge&) = delete("a Gauge is identified by its address");

    void set(double value) noexcept { value_.store(value, std::memory_order_relaxed); }

    void add(double delta) noexcept;

    [[nodiscard]] double value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    static_assert(std::atomic<double>::is_always_lock_free,
                  "a locking gauge would put a mutex on the request path");
    std::atomic<double> value_{0.0};
};

using Labels = std::vector<std::pair<std::string, std::string>>;

enum class MetricKind : uint8_t { Counter, Gauge };

struct MetricSeriesSnapshot {
    // Text preserves full uint64 precision and Prometheus NaN/infinity spellings.
    Labels labels;
    std::string value;
};

struct MetricSnapshot {
    std::string name;
    std::string help;
    MetricKind kind = MetricKind::Counter;
    std::vector<MetricSeriesSnapshot> series;
};

using MetricsSnapshot = std::vector<MetricSnapshot>;

inline constexpr size_t kDefaultMaxMetricSeries = 4'096;

class MetricsRegistry {
public:
    // Prepends the prefix and an underscore to every metric name.
    explicit MetricsRegistry(std::string metric_prefix,
                             size_t max_series = kDefaultMaxMetricSeries);
    ~MetricsRegistry();
    MetricsRegistry(const MetricsRegistry&) = delete("handed out references point into this object");
    MetricsRegistry& operator=(const MetricsRegistry&) = delete("handed out references point in");

    // Returns a stable reference. Duplicate names and labels reuse the existing metric. Missing
    // counter suffixes are normalized to _total.
    [[nodiscard]] Counter& counter(std::string_view name, std::string help, Labels labels = {});

    [[nodiscard]] Gauge& gauge(std::string_view name, std::string help, Labels labels = {});

    // Runs producer under the registry lock while scraping; it must not re-enter the registry.
    void gauge_fn(std::string_view name, std::string help, std::function<double()> producer,
                  Labels labels = {});

    // Registers <prefix>_build_info for the service.
    void build_info(std::string version);

    // Registers a <prefix>_library_build_info series.
    void library_build_info(std::string library, std::string version);

    // Prometheus 0.0.4 exposition sorted by metric name.
    [[nodiscard]] std::string prometheus() const;

    // Samples every source once in prometheus() order.
    [[nodiscard]] MetricsSnapshot snapshot() const;

    [[nodiscard]] const std::string& prefix() const noexcept { return prefix_; }

private:
    void require_room_for_series() const;

    struct Sample {
        std::string name;
        std::string help;
        Labels labels;
        MetricKind kind = MetricKind::Counter;
        // Exactly one source is set.
        Counter* counter = nullptr;
        Gauge* gauge = nullptr;
        std::function<double()> producer;
    };

    std::string prefix_;
    size_t max_series_;

    mutable std::mutex mutex_;

    // Indirection keeps handed-out references stable; the pinned toolchain has no std::hive.
    std::vector<std::unique_ptr<Counter>> counter_storage_;
    std::vector<std::unique_ptr<Gauge>> gauge_storage_;

    std::vector<Sample> samples_;
};

} // namespace erikslund::http
