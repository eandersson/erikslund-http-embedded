
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <latch>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/server.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kPrefix = "erikslund_http";

constexpr std::string_view kRequestsName = "requests";
constexpr std::string_view kRequestsNameWithSuffix = "requests_total";
constexpr std::string_view kRequestsHelp = "Requests accepted on the operator surface.";
constexpr std::string_view kConnectionsName = "connections";
constexpr std::string_view kConnectionsHelp = "Connections currently open.";
constexpr std::string_view kUptimeName = "uptime_seconds";
constexpr std::string_view kUptimeHelp = "Seconds since the process started.";
constexpr std::string_view kBytesName = "bytes";
constexpr std::string_view kBytesNameWithSuffix = "bytes_total";
constexpr std::string_view kBytesHelp = "Bytes written to peers.";
constexpr std::string_view kFillerHelp = "A metric registered only to grow the registry.";

constexpr std::string_view kRequestsSeriesName = "erikslund_http_requests_total";
constexpr std::string_view kConnectionsSeriesName = "erikslund_http_connections";
constexpr std::string_view kUptimeSeriesName = "erikslund_http_uptime_seconds";
constexpr std::string_view kBytesSeriesName = "erikslund_http_bytes_total";
constexpr std::string_view kBuildInfoSeriesName = "erikslund_http_build_info";
constexpr std::string_view kDigitLeadSeriesName = "erikslund_http_5xx";

constexpr std::string_view kCodeLabelName = "code";
constexpr std::string_view kPathLabelName = "path";
constexpr std::string_view kZoneLabelName = "zone";
constexpr std::string_view kVersionLabelName = "version";
constexpr std::string_view kOkCode = "200";
constexpr std::string_view kNotFoundCode = "404";
constexpr std::string_view kServerErrorCode = "500";
constexpr std::string_view kZoneValue = "north";

constexpr std::string_view kHelpLinePrefix = "# HELP ";
constexpr std::string_view kTypeLinePrefix = "# TYPE ";
constexpr std::string_view kCounterTypeName = "counter";
constexpr std::string_view kGaugeTypeName = "gauge";
constexpr std::string_view kDoubledCounterSuffix = "_total_total";

constexpr std::string_view kNameWithDash = "requests-received";
constexpr std::string_view kNameWithSpace = "requests received";
constexpr std::string_view kNameStartingWithADigit = "5xx";
constexpr std::string_view kNameWithRecordingRuleColon = "job:requests";
constexpr std::string_view kInvalidPrefix = "erikslund-http";
constexpr std::string_view kLabelNameWithDash = "status-code";
constexpr std::string_view kEmptyLabelName = "";

constexpr uint64_t kFirstIncrement = 1;
constexpr uint64_t kSecondIncrement = 41;
constexpr uint64_t kExpectedStableTotal = kFirstIncrement + kSecondIncrement;
constexpr uint64_t kExplicitIncrementAmount = 7;
constexpr uint64_t kTwiceTheExplicitAmount = kExplicitIncrementAmount * 2;

constexpr double kGaugeInitialValue = 3.0;
constexpr double kGaugePositiveDelta = 1.5;
constexpr double kGaugeAfterAdd = 4.5;
constexpr double kGaugeNegativeDelta = -2.5;
constexpr double kGaugeAfterSubtract = 2.0;
constexpr double kConnectionCount = 3.0;
constexpr double kFillerGaugeValue = 1.0;

constexpr double kGaugeRatioValue = 0.5;

constexpr std::string_view kGaugeAfterAddText = "4.5";
constexpr std::string_view kConnectionCountText = "3";

constexpr double kProducerFirstValue = 7.0;
constexpr double kProducerSecondValue = 11.0;
constexpr std::string_view kProducerFirstText = "7";
constexpr std::string_view kProducerSecondText = "11";

constexpr std::string_view kBuildVersion = "0.1.2-test";
constexpr std::string_view kOtherBuildVersion = "0.2.0-test";

constexpr std::string_view kBuildInfoValueText = "1";

constexpr size_t kLaterRegistrationCount = 50;

constexpr size_t kIncrementThreadCount = 8;
constexpr uint64_t kIncrementsPerThread = 25'000;
constexpr uint64_t kExpectedConcurrentTotal = kIncrementThreadCount * kIncrementsPerThread;

constexpr size_t kDistinctMetricNameCount = 2;
constexpr size_t kSeriesCount = 4;
constexpr size_t kOneSeries = 1;
constexpr size_t kTwoSeries = 2;
constexpr size_t kThreeSeries = 3;
constexpr size_t kOneMetadataLine = 1;

constexpr std::string_view kHostileLabelValue = "back\\slash \"quoted\"\nnext";
constexpr std::string_view kEscapedHostileLabelValue = "back\\\\slash \\\"quoted\\\"\\nnext";

constexpr std::string_view kLoneQuote = "\"";
constexpr std::string_view kEscapedLoneQuote = "\\\"";
constexpr std::string_view kWrongOrderLoneQuote = "\\\\\"";

constexpr std::string_view kHostileHelpText = "a\\b\"c\nd";
constexpr std::string_view kEscapedHostileHelpText = "a\\\\b\"c\\nd";

constexpr std::string_view kHelpTextImitatingMetadata = "see # HELP and # TYPE elsewhere";

constexpr std::string_view kNotANumberText = "NaN";
constexpr std::string_view kPositiveInfinityText = "+Inf";
constexpr std::string_view kNegativeInfinityText = "-Inf";

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t line_start = 0;
    while (line_start < text.size()) {
        size_t line_end = text.find('\n', line_start);
        if (line_end == std::string_view::npos)
            line_end = text.size();
        lines.emplace_back(text.substr(line_start, line_end - line_start));
        line_start = line_end + 1;
    }
    return lines;
}

[[nodiscard]] size_t count_lines_starting_with(std::string_view text, std::string_view prefix) {
    size_t matches = 0;
    for (const std::string& line : split_lines(text))
        if (line.starts_with(prefix))
            ++matches;
    return matches;
}

[[nodiscard]] bool has_line(std::string_view text, std::string_view wanted) {
    for (const std::string& line : split_lines(text))
        if (line == wanted)
            return true;
    return false;
}

[[nodiscard]] std::vector<std::string> typed_metric_names(std::string_view exposition) {
    std::vector<std::string> names;
    for (const std::string& line : split_lines(exposition)) {
        if (!line.starts_with(kTypeLinePrefix))
            continue;
        const std::string_view rest = std::string_view(line).substr(kTypeLinePrefix.size());
        names.emplace_back(rest.substr(0, rest.find(' ')));
    }
    return names;
}

[[nodiscard]] std::vector<std::string> series_lines(std::string_view exposition) {
    std::vector<std::string> series;
    for (const std::string& line : split_lines(exposition))
        if (!line.empty() && !line.starts_with('#'))
            series.push_back(line);
    return series;
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

[[nodiscard]] Labels one_label(std::string_view name, std::string_view value) {
    Labels labels;
    labels.emplace_back(std::string(name), std::string(value));
    return labels;
}

[[nodiscard]] Labels two_labels(std::string_view first_name, std::string_view first_value,
                                std::string_view second_name, std::string_view second_value) {
    Labels labels;
    labels.emplace_back(std::string(first_name), std::string(first_value));
    labels.emplace_back(std::string(second_name), std::string(second_value));
    return labels;
}

void register_filler_metrics(MetricsRegistry& registry, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        Counter& filler_counter =
            registry.counter(std::format("filler_counter_{}", index), std::string(kFillerHelp));
        filler_counter.increment();
        Gauge& filler_gauge =
            registry.gauge(std::format("filler_gauge_{}", index), std::string(kFillerHelp));
        filler_gauge.set(kFillerGaugeValue);
    }
}

} // namespace

TEST_CASE("a_counter_starts_at_zero_and_increments_by_one") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));

    CHECK(requests.value() == 0);
    requests.increment();
    requests.increment();
    CHECK(requests.value() == 2);
}

TEST_CASE("a_counter_increments_by_an_explicit_amount") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& bytes = registry.counter(std::string(kBytesName), std::string(kBytesHelp));

    bytes.increment(kExplicitIncrementAmount);
    bytes.increment(kExplicitIncrementAmount);
    CHECK(bytes.value() == kTwiceTheExplicitAmount);
}

TEST_CASE("a_gauge_sets_a_value_and_add_moves_it_in_both_directions") {
    MetricsRegistry registry{std::string(kPrefix)};
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));

    connections.set(kGaugeInitialValue);
    CHECK(connections.value() == kGaugeInitialValue);
    connections.add(kGaugePositiveDelta);
    CHECK(connections.value() == kGaugeAfterAdd);
    connections.add(kGaugeNegativeDelta);
    CHECK(connections.value() == kGaugeAfterSubtract);
}

TEST_CASE("registering_the_same_name_and_label_set_twice_returns_the_same_counter") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& first = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                      one_label(kCodeLabelName, kOkCode));
    Counter& second = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                       one_label(kCodeLabelName, kOkCode));

    const bool same_object = &first == &second;
    CHECK_MESSAGE(same_object, "two subsystems registering one series must share one counter");

    first.increment();
    second.increment();
    CHECK(first.value() == 2);
    CHECK(series_lines(registry.prometheus()).size() == kOneSeries);
}

TEST_CASE("a_name_spelled_with_and_without_the_total_suffix_names_one_series") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& without_suffix = registry.counter(std::string(kBytesName), std::string(kBytesHelp));
    Counter& with_suffix =
        registry.counter(std::string(kBytesNameWithSuffix), std::string(kBytesHelp));

    const bool same_object = &without_suffix == &with_suffix;
    CHECK_MESSAGE(same_object,
                  "the appended suffix must resolve to the series the caller already registered");
}

TEST_CASE("a_counter_reference_survives_fifty_later_registrations") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment(kFirstIncrement);

    register_filler_metrics(registry, kLaterRegistrationCount);

    requests.increment(kSecondIncrement);
    CHECK(requests.value() == kExpectedStableTotal);

    Counter& looked_up_again =
        registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    const bool same_object = &requests == &looked_up_again;
    CHECK_MESSAGE(same_object, "a later lookup must find the object the first caller is holding");
    CHECK(looked_up_again.value() == kExpectedStableTotal);

    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kRequestsSeriesName, kExpectedStableTotal)));
}

TEST_CASE("a_gauge_reference_survives_fifty_later_registrations") {
    MetricsRegistry registry{std::string(kPrefix)};
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kGaugeInitialValue);

    register_filler_metrics(registry, kLaterRegistrationCount);

    connections.add(kGaugePositiveDelta);
    CHECK(connections.value() == kGaugeAfterAdd);
    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kConnectionsSeriesName, kGaugeAfterAddText)));
}

TEST_CASE("gauge_fn_is_evaluated_at_scrape_time_and_not_at_registration_time") {
    MetricsRegistry registry{std::string(kPrefix)};
    double live_value = kProducerFirstValue;
    size_t producer_calls = 0;

    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [&live_value, &producer_calls] {
                          ++producer_calls;
                          return live_value;
                      });

    CHECK_MESSAGE(producer_calls == 0,
                  "registration must not read the state the producer captures");

    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kUptimeSeriesName, kProducerFirstText)));
    CHECK(producer_calls == 1);

    live_value = kProducerSecondValue;
    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kUptimeSeriesName, kProducerSecondText)));
    CHECK_MESSAGE(producer_calls == 2, "each scrape must read the live state exactly once");
}

TEST_CASE("a_computed_gauge_is_typed_as_a_gauge_and_carries_its_help") {
    MetricsRegistry registry{std::string(kPrefix)};
    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [] { return kProducerFirstValue; });

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition,
                   std::format("{}{} {}", kHelpLinePrefix, kUptimeSeriesName, kUptimeHelp)));
    CHECK(has_line(exposition,
                   std::format("{}{} {}", kTypeLinePrefix, kUptimeSeriesName, kGaugeTypeName)));
}

TEST_CASE("re_registering_a_computed_gauge_replaces_the_producer_rather_than_adding_a_series") {
    MetricsRegistry registry{std::string(kPrefix)};
    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [] { return kProducerFirstValue; });
    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [] { return kProducerSecondValue; });

    const std::string exposition = registry.prometheus();
    CHECK(series_lines(exposition).size() == kOneSeries);
    CHECK_MESSAGE(has_line(exposition,
                           std::format("{} {}", kUptimeSeriesName, kProducerSecondText)),
                  "a subsystem registered again must not leave the registry calling the old one");
}

TEST_CASE("the_total_suffix_is_appended_when_the_caller_omitted_it") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment();

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition, std::format("{}{} {}", kTypeLinePrefix, kRequestsSeriesName,
                                           kCounterTypeName)));
    CHECK(has_line(exposition, std::format("{} 1", kRequestsSeriesName)));
}

TEST_CASE("the_total_suffix_is_not_doubled_when_the_caller_supplied_it") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& bytes = registry.counter(std::string(kBytesNameWithSuffix), std::string(kBytesHelp));
    bytes.increment();

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition, std::format("{} 1", kBytesSeriesName)));
    CHECK_FALSE(exposition.contains(kDoubledCounterSuffix));
}

TEST_CASE("a_gauge_name_never_gains_the_total_suffix") {
    MetricsRegistry registry{std::string(kPrefix)};
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kConnectionCount);

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition,
                   std::format("{} {}", kConnectionsSeriesName, kConnectionCountText)));
    CHECK(has_line(exposition, std::format("{}{} {}", kTypeLinePrefix, kConnectionsSeriesName,
                                           kGaugeTypeName)));
    CHECK_FALSE(exposition.contains(kCounterTypeName));
}

TEST_CASE("emits_exactly_one_help_and_one_type_line_per_metric_name") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& answered = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kCodeLabelName, kOkCode));
    Counter& missing = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                        one_label(kCodeLabelName, kNotFoundCode));
    Counter& failed = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                       one_label(kCodeLabelName, kServerErrorCode));
    answered.increment();
    missing.increment();
    failed.increment();
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kConnectionCount);

    const std::string exposition = registry.prometheus();
    const size_t help_lines = count_lines_starting_with(exposition, kHelpLinePrefix);
    const size_t type_lines = count_lines_starting_with(exposition, kTypeLinePrefix);

    CHECK_MESSAGE(help_lines == type_lines,
                  "the exposition format pairs every # HELP with exactly one # TYPE");
    CHECK_MESSAGE(help_lines == kDistinctMetricNameCount,
                  "repeating the metadata pair per series is what fails a strict parser");
    CHECK(series_lines(exposition).size() == kSeriesCount);

    const std::vector<std::string> names = typed_metric_names(exposition);
    const bool every_name_is_typed_once = std::ranges::adjacent_find(names) == names.end();
    CHECK(every_name_is_typed_once);
}

TEST_CASE("the_series_of_one_name_arrive_together_ordered_by_their_label_set") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& failed = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                       one_label(kCodeLabelName, kServerErrorCode));
    Counter& answered = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kCodeLabelName, kOkCode));
    Counter& missing = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                        one_label(kCodeLabelName, kNotFoundCode));
    failed.increment();
    answered.increment();
    missing.increment();

    const std::vector<std::string> series = series_lines(registry.prometheus());
    REQUIRE(series.size() == kThreeSeries);
    CHECK(series[0] ==
          std::format("{}{{{}=\"{}\"}} 1", kRequestsSeriesName, kCodeLabelName, kOkCode));
    CHECK(series[1] ==
          std::format("{}{{{}=\"{}\"}} 1", kRequestsSeriesName, kCodeLabelName, kNotFoundCode));
    CHECK(series[2] ==
          std::format("{}{{{}=\"{}\"}} 1", kRequestsSeriesName, kCodeLabelName, kServerErrorCode));
}

TEST_CASE("label_values_are_escaped_backslash_then_quote_then_newline") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kPathLabelName, kHostileLabelValue));
    requests.increment();

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition, std::format("{}{{{}=\"{}\"}} 1", kRequestsSeriesName,
                                           kPathLabelName, kEscapedHostileLabelValue)));
    CHECK_MESSAGE(series_lines(exposition).size() == kOneSeries,
                  "an escaped newline must stay inside the series line it belongs to");
}

TEST_CASE("a_lone_quote_in_a_label_value_gains_exactly_one_backslash") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kPathLabelName, kLoneQuote));
    requests.increment();

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition, std::format("{}{{{}=\"{}\"}} 1", kRequestsSeriesName,
                                           kPathLabelName, kEscapedLoneQuote)));
    const std::string wrong_order =
        std::format("{}=\"{}\"", kPathLabelName, kWrongOrderLoneQuote);
    CHECK_MESSAGE(!exposition.contains(wrong_order),
                  "escaping the quote before the backslash ends the label value early");
}

TEST_CASE("help_text_escapes_the_backslash_and_the_newline_and_leaves_the_quote_literal") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests =
        registry.counter(std::string(kRequestsName), std::string(kHostileHelpText));
    requests.increment();

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition, std::format("{}{} {}", kHelpLinePrefix, kRequestsSeriesName,
                                           kEscapedHostileHelpText)));
    CHECK_MESSAGE(count_lines_starting_with(exposition, kHelpLinePrefix) == kOneMetadataLine,
                  "an unescaped newline in help text would split the document");
}

TEST_CASE("build_info_carries_the_version_as_a_label_and_the_value_one") {
    MetricsRegistry registry{std::string(kPrefix)};
    registry.build_info(std::string(kBuildVersion));

    const std::string exposition = registry.prometheus();
    CHECK(has_line(exposition,
                   std::format("{}{{{}=\"{}\"}} {}", kBuildInfoSeriesName, kVersionLabelName,
                               kBuildVersion, kBuildInfoValueText)));
    CHECK(has_line(exposition, std::format("{}{} {}", kTypeLinePrefix, kBuildInfoSeriesName,
                                           kGaugeTypeName)));
}

TEST_CASE("the_exposition_is_sorted_by_metric_name_whatever_order_registration_happened_in") {
    MetricsRegistry registry{std::string(kPrefix)};
    Gauge& uptime = registry.gauge(std::string(kUptimeName), std::string(kUptimeHelp));
    uptime.set(kGaugeInitialValue);
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment();
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kConnectionCount);
    registry.build_info(std::string(kBuildVersion));

    const std::vector<std::string> names = typed_metric_names(registry.prometheus());
    CHECK(join_with_space(names) ==
          std::format("{} {} {} {}", kBuildInfoSeriesName, kConnectionsSeriesName,
                      kRequestsSeriesName, kUptimeSeriesName));
    CHECK(std::ranges::is_sorted(names));
}

TEST_CASE("two_scrapes_with_no_mutation_in_between_are_byte_identical") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kCodeLabelName, kOkCode));
    requests.increment();
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kConnectionCount);
    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [] { return kProducerFirstValue; });
    registry.build_info(std::string(kBuildVersion));

    const std::string first_scrape = registry.prometheus();
    const std::string second_scrape = registry.prometheus();
    CHECK_MESSAGE(first_scrape == second_scrape,
                  "an operator diffing two scrapes must see only what actually changed");
}

TEST_CASE("an_empty_prefix_leaves_the_metric_name_unqualified") {
    MetricsRegistry registry("");
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment();

    CHECK(registry.prefix().empty());
    CHECK(has_line(registry.prometheus(), std::format("{} 1", kRequestsNameWithSuffix)));
}

TEST_CASE("the_prefix_reads_back_from_the_registry") {
    const MetricsRegistry registry{std::string(kPrefix)};
    CHECK(registry.prefix() == kPrefix);
}

TEST_CASE("a_registry_with_no_metrics_renders_an_empty_document") {
    const MetricsRegistry registry{std::string(kPrefix)};
    CHECK(registry.prometheus().empty());
}

TEST_CASE("a_counter_incremented_from_several_jthreads_totals_exactly") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));

    std::latch start_line(static_cast<std::ptrdiff_t>(kIncrementThreadCount));
    std::vector<std::jthread> writers;
    writers.reserve(kIncrementThreadCount);

    for (size_t writer_index = 0; writer_index < kIncrementThreadCount; ++writer_index)
        writers.emplace_back([&requests, &start_line] {
            start_line.arrive_and_wait();
            for (uint64_t step = 0; step < kIncrementsPerThread; ++step)
                requests.increment();
        });

    writers.clear();

    CHECK(requests.value() == kExpectedConcurrentTotal);
    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kRequestsSeriesName, kExpectedConcurrentTotal)));
}

TEST_SUITE("metrics adversarial") {

TEST_CASE("rejects_a_metric_name_that_is_not_a_prometheus_identifier") {
    MetricsRegistry registry{std::string(kPrefix)};
    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kNameWithDash),
                                                       std::string(kRequestsHelp))),
                    ServerError);
    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kNameWithSpace),
                                                       std::string(kRequestsHelp))),
                    ServerError);
}

TEST_CASE("rejects_a_leading_digit_only_where_no_prefix_hides_it") {
    MetricsRegistry prefixed{std::string(kPrefix)};
    Gauge& rescued_by_the_prefix =
        prefixed.gauge(std::string(kNameStartingWithADigit), std::string(kConnectionsHelp));
    rescued_by_the_prefix.set(kConnectionCount);
    CHECK(has_line(prefixed.prometheus(),
                   std::format("{} {}", kDigitLeadSeriesName, kConnectionCountText)));

    MetricsRegistry unprefixed("");
    CHECK_THROWS_AS(static_cast<void>(unprefixed.gauge(std::string(kNameStartingWithADigit),
                                                       std::string(kConnectionsHelp))),
                    ServerError);
}

TEST_CASE("rejects_a_metric_name_carrying_the_colon_reserved_for_recording_rules") {
    MetricsRegistry registry{std::string(kPrefix)};
    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kNameWithRecordingRuleColon),
                                                       std::string(kRequestsHelp))),
                    ServerError);
}

TEST_CASE("rejects_a_label_name_that_is_not_a_prometheus_identifier") {
    MetricsRegistry registry{std::string(kPrefix)};
    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kRequestsName),
                                                       std::string(kRequestsHelp),
                                                       one_label(kLabelNameWithDash, kOkCode))),
                    ServerError);
    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kRequestsName),
                                                       std::string(kRequestsHelp),
                                                       one_label(kEmptyLabelName, kOkCode))),
                    ServerError);
}

TEST_CASE("rejects_a_label_set_that_repeats_one_name") {
    MetricsRegistry registry{std::string(kPrefix)};
    CHECK_THROWS_AS(
        static_cast<void>(registry.counter(
            std::string(kRequestsName), std::string(kRequestsHelp),
            two_labels(kZoneLabelName, kZoneValue, kZoneLabelName, "south"))),
        ServerError);
    CHECK_MESSAGE(registry.prometheus().empty(),
                  "a rejected duplicate label must not leave a malformed series behind");
}

TEST_CASE("rejects_an_invalid_metric_prefix_at_construction") {
    CHECK_THROWS_AS(MetricsRegistry(std::string(kInvalidPrefix)), ServerError);
}

TEST_CASE("rejects_a_second_registration_that_would_change_a_series_kind") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment();

    CHECK_THROWS_AS(static_cast<void>(registry.gauge(std::string(kRequestsNameWithSuffix),
                                                     std::string(kRequestsHelp))),
                    ServerError);
    CHECK_THROWS_AS(registry.gauge_fn(std::string(kRequestsNameWithSuffix),
                                      std::string(kRequestsHelp),
                                      [] { return kProducerFirstValue; }),
                    ServerError);
}

TEST_CASE("rejects_a_stored_gauge_on_a_series_that_is_already_computed") {
    MetricsRegistry registry{std::string(kPrefix)};
    registry.gauge_fn(std::string(kUptimeName), std::string(kUptimeHelp),
                      [] { return kProducerFirstValue; });

    CHECK_THROWS_AS(static_cast<void>(registry.gauge(std::string(kUptimeName),
                                                     std::string(kUptimeHelp))),
                    ServerError);
}

TEST_CASE("a_rejected_registration_leaves_the_registry_intact_and_usable") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp));
    requests.increment();

    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kNameWithDash),
                                                       std::string(kRequestsHelp))),
                    ServerError);

    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));
    connections.set(kConnectionCount);
    const std::string exposition = registry.prometheus();
    CHECK(series_lines(exposition).size() == kTwoSeries);
    CHECK_FALSE(exposition.contains(kNameWithDash));
}

TEST_CASE("a_non_finite_gauge_renders_as_the_spelling_the_exposition_format_defines") {
    MetricsRegistry registry{std::string(kPrefix)};
    Gauge& connections =
        registry.gauge(std::string(kConnectionsName), std::string(kConnectionsHelp));

    connections.set(std::numeric_limits<double>::quiet_NaN());
    CHECK_MESSAGE(has_line(registry.prometheus(),
                           std::format("{} {}", kConnectionsSeriesName, kNotANumberText)),
                  "no Prometheus parser accepts the \"nan\" that std::format writes");

    connections.set(std::numeric_limits<double>::infinity());
    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kConnectionsSeriesName, kPositiveInfinityText)));

    connections.set(-std::numeric_limits<double>::infinity());
    CHECK(has_line(registry.prometheus(),
                   std::format("{} {}", kConnectionsSeriesName, kNegativeInfinityText)));
}

TEST_CASE("help_text_that_imitates_a_metadata_line_adds_no_metadata_line") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& requests =
        registry.counter(std::string(kRequestsName), std::string(kHelpTextImitatingMetadata));
    requests.increment();

    const std::string exposition = registry.prometheus();
    CHECK(count_lines_starting_with(exposition, kHelpLinePrefix) == kOneMetadataLine);
    CHECK(count_lines_starting_with(exposition, kTypeLinePrefix) == kOneMetadataLine);
    CHECK(series_lines(exposition).size() == kOneSeries);
}

TEST_CASE("two_label_sets_that_differ_only_in_order_never_become_two_series") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& forward = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                        two_labels(kCodeLabelName, kOkCode, kZoneLabelName,
                                                   kZoneValue));
    forward.increment();

    bool the_reordered_set_was_refused = false;
    try {
        Counter& reordered = registry.counter(std::string(kRequestsName),
                                              std::string(kRequestsHelp),
                                              two_labels(kZoneLabelName, kZoneValue,
                                                         kCodeLabelName, kOkCode));
        reordered.increment();
    } catch (const ServerError&) {
        the_reordered_set_was_refused = true;
    }

    const bool the_series_reached_the_wire_once =
        the_reordered_set_was_refused || series_lines(registry.prometheus()).size() == kOneSeries;
    CHECK_MESSAGE(the_series_reached_the_wire_once,
                  "a reordered label set must fold onto the series that already exists, or be "
                  "refused outright; it must not reach the wire as a second line");
}

TEST_CASE("one_metric_name_never_carries_two_kinds") {
    MetricsRegistry registry{std::string(kPrefix)};
    Counter& answered = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                         one_label(kCodeLabelName, kOkCode));
    answered.increment();

    bool the_conflicting_kind_was_refused = false;
    try {
        Gauge& ratio = registry.gauge(std::string(kRequestsNameWithSuffix),
                                      std::string(kRequestsHelp),
                                      one_label(kCodeLabelName, kNotFoundCode));
        ratio.set(kGaugeRatioValue);
    } catch (const ServerError&) {
        the_conflicting_kind_was_refused = true;
    }

    CHECK_MESSAGE(the_conflicting_kind_was_refused,
                  "a metric name carries exactly one type line, so it must carry exactly one kind");
}

TEST_CASE("a_second_build_info_adds_a_series_and_still_only_one_metadata_pair") {
    MetricsRegistry registry{std::string(kPrefix)};
    registry.build_info(std::string(kBuildVersion));
    registry.build_info(std::string(kOtherBuildVersion));

    const std::string exposition = registry.prometheus();
    CHECK_MESSAGE(series_lines(exposition).size() == kTwoSeries,
                  "the version IS the label, so a second call is a second series");
    CHECK(count_lines_starting_with(exposition, kHelpLinePrefix) == kOneMetadataLine);
    CHECK(count_lines_starting_with(exposition, kTypeLinePrefix) == kOneMetadataLine);
}

TEST_CASE("the_series_limit_bounds_dynamic_label_cardinality") {
    constexpr size_t kSeriesLimit = 2;
    MetricsRegistry registry(std::string(kPrefix), kSeriesLimit);

    Counter& ok = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                   one_label(kCodeLabelName, kOkCode));
    Counter& missing = registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                                        one_label(kCodeLabelName, kNotFoundCode));
    ok.increment();
    missing.increment();

    CHECK_THROWS_AS(static_cast<void>(registry.counter(std::string(kRequestsName),
                                                       std::string(kRequestsHelp),
                                                       one_label(kCodeLabelName, "500"))),
                    ServerError);
    CHECK(&registry.counter(std::string(kRequestsName), std::string(kRequestsHelp),
                            one_label(kCodeLabelName, kOkCode)) == &ok);
    CHECK(series_lines(registry.prometheus()).size() == kSeriesLimit);
}

} // TEST_SUITE("metrics adversarial")
