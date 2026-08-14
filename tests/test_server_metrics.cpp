
#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/metrics.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kServicePrefix = "my_service";
constexpr std::string_view kServiceVersion = "1.4.2";

constexpr std::string_view kBuildInfoSeriesName = "my_service_build_info";
constexpr std::string_view kLibraryBuildInfoSeriesName = "my_service_library_build_info";

constexpr std::string_view kLibraryLabelName = "library";
constexpr std::string_view kVersionLabelName = "version";
constexpr std::string_view kLibraryLabelValue = "erikslund-http";

constexpr std::string_view kOtherLibraryName = "erikslund-solo-pool";
constexpr std::string_view kOtherLibraryVersion = "2.0.0";

constexpr std::string_view kInfoValueText = "1";

constexpr std::string_view kHelpLinePrefix = "# HELP ";
constexpr std::string_view kTypeLinePrefix = "# TYPE ";
constexpr std::string_view kGaugeTypeName = "gauge";

constexpr size_t kNoSeries = 0;
constexpr size_t kOneSeries = 1;
constexpr size_t kOneMetadataPair = 1;
constexpr size_t kTwoSeries = 2;

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const size_t break_position = text.find('\n');
        lines.emplace_back(text.substr(0, break_position));
        if (break_position == std::string_view::npos)
            break;
        text.remove_prefix(break_position + 1);
    }
    return lines;
}

[[nodiscard]] bool has_line(std::string_view exposition, std::string_view wanted) {
    for (const std::string& line : split_lines(exposition))
        if (line == wanted)
            return true;
    return false;
}

[[nodiscard]] size_t series_count(std::string_view exposition, std::string_view series_name) {
    size_t found = 0;
    for (const std::string& line : split_lines(exposition)) {
        if (!std::string_view(line).starts_with(series_name))
            continue;
        const char after = line[series_name.size()];
        if (after == '{' || after == ' ')
            ++found;
    }
    return found;
}

[[nodiscard]] size_t metadata_line_count(std::string_view exposition, std::string_view line_prefix,
                                         std::string_view series_name) {
    const std::string wanted = std::format("{}{} ", line_prefix, series_name);
    size_t found = 0;
    for (const std::string& line : split_lines(exposition))
        if (std::string_view(line).starts_with(wanted))
            ++found;
    return found;
}

[[nodiscard]] std::string info_line(std::string_view series_name, std::string_view labels) {
    return std::format("{}{{{}}} {}", series_name, labels, kInfoValueText);
}

[[nodiscard]] std::string library_labels(std::string_view library, std::string_view version) {
    return std::format("{}=\"{}\",{}=\"{}\"", kLibraryLabelName, library, kVersionLabelName,
                       version);
}

} // namespace

TEST_CASE("install_metrics_leaves_build_info_to_the_service_that_owns_the_registry") {
    REQUIRE_MESSAGE(kVersion != kServiceVersion,
                    "the library and the service must carry different versions for this case to "
                    "be able to tell them apart");

    MetricsRegistry registry{std::string(kServicePrefix)};
    Server server(Router{}, ServerOptions{});
    server.install_metrics(registry);

    const std::string exposition = registry.prometheus();
    CHECK_MESSAGE(series_count(exposition, kBuildInfoSeriesName) == kNoSeries,
                  std::format("build_info belongs to the service, whose version the library cannot "
                              "know:\n{}",
                              exposition));
    CHECK(metadata_line_count(exposition, kHelpLinePrefix, kBuildInfoSeriesName) == kNoSeries);
    CHECK(metadata_line_count(exposition, kTypeLinePrefix, kBuildInfoSeriesName) == kNoSeries);
}

TEST_CASE("the_service_version_is_the_only_build_info_a_scrape_carries_after_install_metrics") {
    MetricsRegistry registry{std::string(kServicePrefix)};
    registry.build_info(std::string(kServiceVersion));
    Server server(Router{}, ServerOptions{});
    server.install_metrics(registry);

    const std::string exposition = registry.prometheus();
    CHECK_MESSAGE(series_count(exposition, kBuildInfoSeriesName) == kOneSeries,
                  std::format("two versions under one metric name is a dashboard that shows "
                              "whichever series it happened to pick:\n{}",
                              exposition));
    CHECK(has_line(exposition, info_line(kBuildInfoSeriesName,
                                         std::format("{}=\"{}\"", kVersionLabelName,
                                                     kServiceVersion))));
}

TEST_CASE("install_metrics_publishes_the_library_version_under_a_name_of_its_own") {
    MetricsRegistry registry{std::string(kServicePrefix)};
    Server server(Router{}, ServerOptions{});
    server.install_metrics(registry);

    const std::string exposition = registry.prometheus();
    CHECK_MESSAGE(has_line(exposition, info_line(kLibraryBuildInfoSeriesName,
                                                 library_labels(kLibraryLabelValue, kVersion))),
                  std::format("the version of the library serving the surface has to stay "
                              "discoverable in a scrape:\n{}",
                              exposition));
    CHECK(has_line(exposition, std::format("{}{} {}", kTypeLinePrefix,
                                           kLibraryBuildInfoSeriesName, kGaugeTypeName)));
}

TEST_CASE("a_second_library_publishing_its_version_is_a_second_series_not_a_second_metric") {
    MetricsRegistry registry{std::string(kServicePrefix)};
    Server server(Router{}, ServerOptions{});
    server.install_metrics(registry);
    registry.library_build_info(std::string(kOtherLibraryName), std::string(kOtherLibraryVersion));

    const std::string exposition = registry.prometheus();
    CHECK(series_count(exposition, kLibraryBuildInfoSeriesName) == kTwoSeries);
    CHECK(has_line(exposition, info_line(kLibraryBuildInfoSeriesName,
                                         library_labels(kOtherLibraryName, kOtherLibraryVersion))));
    CHECK_MESSAGE(metadata_line_count(exposition, kHelpLinePrefix, kLibraryBuildInfoSeriesName) ==
                      kOneMetadataPair,
                  "repeating the metadata per series is what makes a strict parser reject the "
                  "whole document");
    CHECK(metadata_line_count(exposition, kTypeLinePrefix, kLibraryBuildInfoSeriesName) ==
          kOneMetadataPair);
}
