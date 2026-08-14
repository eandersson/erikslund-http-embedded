#include "erikslund/http/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/text.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kCounterSuffix = "_total";

constexpr std::string_view kCounterTypeName = "counter";
constexpr std::string_view kGaugeTypeName = "gauge";

constexpr std::string_view kBuildInfoName = "build_info";
constexpr std::string_view kBuildInfoLabelName = "version";
constexpr std::string_view kBuildInfoHelp =
    "Build version of the running binary. The value is always 1; the version is the label.";

constexpr std::string_view kLibraryBuildInfoName = "library_build_info";
constexpr std::string_view kLibraryBuildInfoLabelName = "library";
constexpr std::string_view kLibraryBuildInfoHelp =
    "Build version of a library linked into the running binary. The value is always 1; the "
    "library and its version are the labels.";

constexpr double kBuildInfoValue = 1.0;

constexpr size_t kEstimatedBytesPerSeries = 96;
constexpr size_t kEstimatedBytesPerMetadataPair = 128;

constexpr std::string_view kNotANumberText = "NaN";
constexpr std::string_view kPositiveInfinityText = "+Inf";
constexpr std::string_view kNegativeInfinityText = "-Inf";

[[nodiscard]] constexpr bool is_identifier_start(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

[[nodiscard]] constexpr bool is_identifier_character(char character) noexcept {
    return is_identifier_start(character) || (character >= '0' && character <= '9');
}

[[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept {
    if (name.empty() || !is_identifier_start(name.front()))
        return false;
    for (const char character : name)
        if (!is_identifier_character(character))
            return false;
    return true;
}

void require_valid_series(std::string_view name, const Labels& labels) {
    if (!is_valid_identifier(name))
        throw ServerError(std::format("invalid Prometheus metric name: '{}'", name));
    for (size_t index = 0; index < labels.size(); ++index) {
        const std::string& label_name = labels[index].first;
        if (!is_valid_identifier(label_name))
            throw ServerError(std::format("invalid Prometheus label name '{}' on metric '{}'",
                                          label_name, name));
        for (size_t previous = 0; previous < index; ++previous)
            if (labels[previous].first == label_name)
                throw ServerError(std::format("duplicate Prometheus label name '{}' on metric '{}'",
                                              label_name, name));
    }
}

void validate_and_sort_labels(std::string_view name, Labels& labels) {
    require_valid_series(name, labels);
    std::ranges::sort(labels);
}

[[nodiscard]] std::string qualified_name(std::string_view prefix, std::string_view name) {
    if (prefix.empty())
        return std::string(name);
    std::string full;
    full.reserve(prefix.size() + 1 + name.size());
    full += prefix;
    full += '_';
    full += name;
    return full;
}

[[nodiscard]] std::string escape_help(std::string_view help) {
    std::string out;
    out.reserve(help.size());
    for (const char character : help) {
        if (character == '\\')
            out += "\\\\";
        else if (character == '\n')
            out += "\\n";
        else
            out += character;
    }
    return out;
}

[[nodiscard]] std::string render_labels(const Labels& labels) {
    if (labels.empty())
        return {};
    std::string out;
    out += '{';
    bool first = true;
    for (const auto& label : labels) {
        if (!first)
            out += ',';
        first = false;
        out += label.first;
        out += "=\"";
        out += prometheus_label_escape(label.second);
        out += '"';
    }
    out += '}';
    return out;
}

[[nodiscard]] constexpr std::string_view kind_type_name(MetricKind kind) noexcept {
    return kind == MetricKind::Counter ? kCounterTypeName : kGaugeTypeName;
}

template <class SampleRange>
void require_one_kind_per_name(const SampleRange& samples, std::string_view full_name,
                               MetricKind kind) {
    for (const auto& sample : samples) {
        if (sample.name != full_name || sample.kind == kind)
            continue;
        throw ServerError(std::format(
            "metric '{}' is already registered as a {}; a metric name carries exactly one # TYPE "
            "line, so it cannot also be a {}",
            full_name, kind_type_name(sample.kind), kind_type_name(kind)));
    }
}

[[nodiscard]] std::string format_gauge_value(double value) {
    if (std::isnan(value))
        return std::string(kNotANumberText);
    if (std::isinf(value))
        return std::string(value > 0.0 ? kPositiveInfinityText : kNegativeInfinityText);
    return std::format("{}", value);
}

} // namespace

void Gauge::add(double delta) noexcept {
    double observed = value_.load(std::memory_order_relaxed);
    while (!value_.compare_exchange_weak(observed, observed + delta, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

MetricsRegistry::MetricsRegistry(std::string metric_prefix, size_t max_series)
    : prefix_(std::move(metric_prefix)), max_series_(max_series) {
    if (!prefix_.empty() && !is_valid_identifier(prefix_))
        throw ServerError(std::format("invalid Prometheus metric prefix: '{}'", prefix_));
}

MetricsRegistry::~MetricsRegistry() = default;

void MetricsRegistry::require_room_for_series() const {
    if (samples_.size() >= max_series_)
        throw ServerError(std::format("metric series limit of {} reached", max_series_));
}

Counter& MetricsRegistry::counter(std::string_view name, std::string help, Labels labels) {
    std::string full_name = qualified_name(prefix_, name);
    if (!full_name.ends_with(kCounterSuffix))
        full_name += kCounterSuffix;
    validate_and_sort_labels(full_name, labels);

    const std::lock_guard<std::mutex> registration_guard(mutex_);
    require_one_kind_per_name(samples_, full_name, MetricKind::Counter);
    for (const Sample& sample : samples_) {
        if (sample.name != full_name || sample.labels != labels)
            continue;
        if (sample.counter == nullptr)
            throw ServerError(
                std::format("metric '{}' is already registered as a gauge; a name plus a label "
                            "set names exactly one series",
                            full_name));
        return *sample.counter;
    }

    require_room_for_series();
    counter_storage_.push_back(std::make_unique<Counter>());
    Counter& created = *counter_storage_.back();
    samples_.push_back(Sample{.name = std::move(full_name),
                              .help = std::move(help),
                              .labels = std::move(labels),
                              .kind = MetricKind::Counter,
                              .counter = &created,
                              .producer = {}});
    return created;
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string help, Labels labels) {
    std::string full_name = qualified_name(prefix_, name);
    validate_and_sort_labels(full_name, labels);

    const std::lock_guard<std::mutex> registration_guard(mutex_);
    require_one_kind_per_name(samples_, full_name, MetricKind::Gauge);
    for (const Sample& sample : samples_) {
        if (sample.name != full_name || sample.labels != labels)
            continue;
        if (sample.gauge == nullptr)
            throw ServerError(
                std::format("metric '{}' is already registered as a counter or as a computed gauge",
                            full_name));
        return *sample.gauge;
    }

    require_room_for_series();
    gauge_storage_.push_back(std::make_unique<Gauge>());
    Gauge& created = *gauge_storage_.back();
    samples_.push_back(Sample{.name = std::move(full_name),
                              .help = std::move(help),
                              .labels = std::move(labels),
                              .kind = MetricKind::Gauge,
                              .gauge = &created,
                              .producer = {}});
    return created;
}

void MetricsRegistry::gauge_fn(std::string_view name, std::string help,
                               std::function<double()> producer, Labels labels) {
    std::string full_name = qualified_name(prefix_, name);
    validate_and_sort_labels(full_name, labels);

    const std::lock_guard<std::mutex> registration_guard(mutex_);
    require_one_kind_per_name(samples_, full_name, MetricKind::Gauge);
    for (Sample& sample : samples_) {
        if (sample.name != full_name || sample.labels != labels)
            continue;
        if (!sample.producer)
            throw ServerError(
                std::format("metric '{}' is already registered as a stored counter or gauge; a "
                            "computed gauge cannot share a series with it",
                            full_name));
        sample.help = std::move(help);
        sample.producer = std::move(producer);
        return;
    }

    require_room_for_series();
    samples_.push_back(Sample{.name = std::move(full_name),
                              .help = std::move(help),
                              .labels = std::move(labels),
                              .kind = MetricKind::Gauge,
                              .producer = std::move(producer)});
}

void MetricsRegistry::build_info(std::string version) {
    Labels labels;
    labels.emplace_back(std::string(kBuildInfoLabelName), std::move(version));
    Gauge& info =
        gauge(std::string(kBuildInfoName), std::string(kBuildInfoHelp), std::move(labels));
    info.set(kBuildInfoValue);
}

void MetricsRegistry::library_build_info(std::string library, std::string version) {
    Labels labels;
    labels.emplace_back(std::string(kLibraryBuildInfoLabelName), std::move(library));
    labels.emplace_back(std::string(kBuildInfoLabelName), std::move(version));
    Gauge& info = gauge(std::string(kLibraryBuildInfoName), std::string(kLibraryBuildInfoHelp),
                        std::move(labels));
    info.set(kBuildInfoValue);
}

std::string MetricsRegistry::prometheus() const {
    const MetricsSnapshot sampled = snapshot();

    size_t series_count = 0;
    for (const MetricSnapshot& metric : sampled)
        series_count += metric.series.size();

    std::string out;
    out.reserve(series_count * kEstimatedBytesPerSeries +
                sampled.size() * kEstimatedBytesPerMetadataPair);
    for (const MetricSnapshot& metric : sampled) {
        out += "# HELP ";
        out += metric.name;
        out += ' ';
        out += escape_help(metric.help);
        out += "\n# TYPE ";
        out += metric.name;
        out += ' ';
        out += kind_type_name(metric.kind);
        out += '\n';
        for (const MetricSeriesSnapshot& series : metric.series) {
            out += metric.name;
            out += render_labels(series.labels);
            out += ' ';
            out += series.value;
            out += '\n';
        }
    }
    return out;
}

MetricsSnapshot MetricsRegistry::snapshot() const {
    const std::lock_guard<std::mutex> registration_guard(mutex_);

    std::vector<std::string> label_text;
    label_text.reserve(samples_.size());
    for (const Sample& sample : samples_)
        label_text.push_back(render_labels(sample.labels));

    std::vector<size_t> order(samples_.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::sort(order, [this, &label_text](size_t left, size_t right) {
        if (samples_[left].name != samples_[right].name)
            return samples_[left].name < samples_[right].name;
        return label_text[left] < label_text[right];
    });

    MetricsSnapshot snapshot;
    snapshot.reserve(samples_.size());
    for (const size_t index : order) {
        const Sample& sample = samples_[index];
        if (snapshot.empty() || snapshot.back().name != sample.name)
            snapshot.push_back(MetricSnapshot{.name = sample.name,
                                              .help = sample.help,
                                              .kind = sample.kind,
                                              .series = {}});

        std::string value;
        ERIKSLUND_HTTP_ASSERT(sample.counter != nullptr || sample.gauge != nullptr ||
                              static_cast<bool>(sample.producer));
        if (sample.counter != nullptr)
            value = std::format("{}", sample.counter->value());
        else if (sample.gauge != nullptr)
            value = format_gauge_value(sample.gauge->value());
        else if (sample.producer)
            value = format_gauge_value(sample.producer());
        else
            value = format_gauge_value(0.0);
        snapshot.back().series.push_back(
            MetricSeriesSnapshot{.labels = sample.labels, .value = std::move(value)});
    }
    return snapshot;
}

} // namespace erikslund::http
