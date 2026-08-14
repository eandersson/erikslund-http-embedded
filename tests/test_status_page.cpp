
#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "erikslund/http/status_page.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kServiceName = "erikslund-http";
constexpr std::string_view kServiceVersion = "0.1.2";
constexpr int kProcessId = 4'242;

constexpr int kCanonicalRefreshSeconds = 5;
constexpr int kSlowRefreshSeconds = 30;
constexpr int kNoRefreshSeconds = 0;
constexpr int kNegativeRefreshSeconds = -1;

constexpr std::string_view kSystemUiFontStack = "font-family:system-ui,sans-serif";
constexpr std::string_view kContentColumnWidth = "max-width:46rem";
constexpr std::string_view kBodyTextColour = "color:#222";
constexpr std::string_view kLabelCellRule = "td:first-child{color:#777;width:14rem}";
constexpr std::string_view kOkColourRule = ".ok{color:#0a7d28}";
constexpr std::string_view kBadColourRule = ".bad{color:#c0392b}";
constexpr std::string_view kWarnColourRule = ".warn{color:#b8860b}";
constexpr std::string_view kLinkColourRule = "a{color:#2563eb;text-decoration:none}";
constexpr std::string_view kInlineIconLink = "<link rel=\"icon\" href=\"data:,\">";
constexpr std::string_view kViewportMeta =
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";

constexpr std::string_view kRefreshAttribute = "http-equiv=\"refresh\"";
constexpr std::string_view kNoscriptOpening = "<noscript>";
constexpr std::string_view kRawScriptOpening = "<script";
constexpr std::string_view kEventSourceCall = "new EventSource(\"/events\")";
constexpr std::string_view kLiveRowsTbody = "<tbody id=\"status-rows\">";
constexpr std::string_view kTbodyClosing = "</tbody>";
constexpr size_t kExactlyOneScriptElement = 1;
constexpr size_t kExactlyOneRefreshMeta = 1;

constexpr std::string_view kScriptElementOpening = "<script>";
constexpr std::string_view kScriptElementClosing = "</script>";

constexpr std::string_view kLiveScriptText =
    "\n"
    "(() => {\n"
    "  const rows = document.getElementById(\"status-rows\");\n"
    "  if (!rows || !window.EventSource) return;\n"
    "  const source = new EventSource(\"/events\");\n"
    "  source.addEventListener(\"status\", event => {\n"
    "    let updates;\n"
    "    try { updates = JSON.parse(event.data); } catch { return; }\n"
    "    if (!Array.isArray(updates)) return;\n"
    "    const nextRows = document.createDocumentFragment();\n"
    "    for (const update of updates) {\n"
    "      if (!update || typeof update.label !== \"string\" ||\n"
    "          typeof update.value !== \"string\") return;\n"
    "      const row = document.createElement(\"tr\");\n"
    "      const label = document.createElement(\"td\");\n"
    "      const value = document.createElement(\"td\");\n"
    "      label.textContent = update.label;\n"
    "      value.textContent = update.value;\n"
    "      if ([\"ok\", \"warn\", \"bad\"].includes(update.state))\n"
    "        value.className = update.state;\n"
    "      row.append(label, value);\n"
    "      nextRows.append(row);\n"
    "    }\n"
    "    rows.replaceChildren(nextRows);\n"
    "  });\n"
    "})();\n";
constexpr std::string_view kLiveScriptCspHashSource =
    "'sha256-QqFlhp2qcNvjz3NKBAPEc2Z6SzpfHaQzpX7XA1i95oo='";

constexpr std::string_view kUptimeLabel = "uptime";
constexpr std::string_view kUptimeValue = "1d 2h 3m 4s";
constexpr std::string_view kBackendLabel = "backend";
constexpr std::string_view kBackendValue = "ntp1";
constexpr std::string_view kSectionHeading = "listeners";
constexpr std::string_view kReadyHeadline = "READY";
constexpr std::string_view kDegradedHeadline = "NO HEALTHY BACKENDS";

constexpr std::string_view kMetricsHref = "/metrics";
constexpr std::string_view kMetricsText = "/metrics";
constexpr std::string_view kHealthzHref = "/healthz";
constexpr std::string_view kHealthzText = "/healthz";

[[nodiscard]] size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return 0;
    size_t found = 0;
    for (size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size()))
        ++found;
    return found;
}

[[nodiscard]] std::string canonical_empty_page(std::string_view service_name,
                                               std::string_view version, int refresh_seconds) {
    return std::format("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                       "<link rel=\"icon\" href=\"data:,\">\n"
                       "<meta http-equiv=\"refresh\" content=\"{}\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                       "<title>{} v{}</title>\n"
                       "<style>\n"
                       "{}\n"
                       "</style></head><body>\n"
                       "<h1>{} <small>v{}</small></h1>\n"
                       "<table>\n"
                       "</table>\n"
                       "</body></html>\n",
                       refresh_seconds, service_name, version, kStatusPageStyle, service_name,
                       version);
}

[[nodiscard]] std::string canonical_empty_page_without_refresh(std::string_view service_name,
                                                               std::string_view version) {
    return std::format("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                       "<link rel=\"icon\" href=\"data:,\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                       "<title>{} v{}</title>\n"
                       "<style>\n"
                       "{}\n"
                       "</style></head><body>\n"
                       "<h1>{} <small>v{}</small></h1>\n"
                       "<table>\n"
                       "</table>\n"
                       "</body></html>\n",
                       service_name, version, kStatusPageStyle, service_name, version);
}

[[nodiscard]] std::string plain_row_markup(std::string_view label, std::string_view value) {
    return std::format("  <tr><td>{}</td><td>{}</td></tr>\n", label, value);
}

[[nodiscard]] std::string classed_row_markup(std::string_view label, std::string_view value,
                                             std::string_view class_name) {
    return std::format("  <tr><td>{}</td><td class=\"{}\">{}</td></tr>\n", label, class_name,
                       value);
}

[[nodiscard]] std::string state_line_markup(std::string_view headline,
                                            std::string_view class_name) {
    return std::format("<p class=\"{}\"><strong>{}</strong></p>\n", class_name, headline);
}

} // namespace

TEST_CASE("the_library_default_refresh_matches_the_canonical_five_seconds") {
    CHECK(kDefaultRefreshSeconds == kCanonicalRefreshSeconds);
}

TEST_CASE("renders_the_canonical_document_byte_for_byte_with_no_rows") {
    const StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    CHECK(page.render() ==
          canonical_empty_page(kServiceName, kServiceVersion, kCanonicalRefreshSeconds));
}

TEST_CASE("carries_the_canonical_system_ui_stylesheet_unchanged") {
    const StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    const std::string rendered = page.render();

    CHECK(rendered.contains(kStatusPageStyle));
    CHECK(rendered.contains(kSystemUiFontStack));
    CHECK(rendered.contains(kContentColumnWidth));
    CHECK(rendered.contains(kBodyTextColour));
    CHECK(rendered.contains(kLabelCellRule));
}

TEST_CASE("pins_the_three_functional_status_colours_and_the_link_colour") {
    const StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    const std::string rendered = page.render();

    CHECK(rendered.contains(kOkColourRule));
    CHECK(rendered.contains(kBadColourRule));
    CHECK(rendered.contains(kWarnColourRule));
    CHECK(rendered.contains(kLinkColourRule));
}

TEST_CASE("emits_the_empty_inline_icon_so_a_browser_never_asks_for_a_favicon") {
    const StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    const std::string rendered = page.render();

    CHECK(rendered.contains(kInlineIconLink));
    CHECK(rendered.contains(kViewportMeta));
}

TEST_CASE("the_meta_refresh_appears_at_the_configured_interval") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.refresh_seconds(kSlowRefreshSeconds);

    CHECK(page.render() ==
          canonical_empty_page(kServiceName, kServiceVersion, kSlowRefreshSeconds));
}

TEST_CASE("the_meta_refresh_is_omitted_entirely_at_zero") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.refresh_seconds(kNoRefreshSeconds);
    const std::string rendered = page.render();

    CHECK_FALSE(rendered.contains(kRefreshAttribute));
    CHECK_MESSAGE(rendered.contains(kInlineIconLink),
                  "dropping the refresh must not drop the rest of the head");
}

TEST_CASE("a_two_argument_row_emits_an_unclassed_value_cell") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kUptimeLabel, kUptimeValue);

    CHECK(page.render().contains(plain_row_markup(kUptimeLabel, kUptimeValue)));
}

TEST_CASE("a_three_argument_row_colours_the_value_cell_and_leaves_the_label_muted") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kBackendLabel, kBackendValue, State::Warn);

    CHECK(page.render().contains(
        classed_row_markup(kBackendLabel, kBackendValue, state_class(State::Warn))));
}

TEST_CASE("state_class_maps_each_state_to_its_canonical_class_name") {
    CHECK(state_class(State::Ok) == "ok");
    CHECK(state_class(State::Warn) == "warn");
    CHECK(state_class(State::Bad) == "bad");
}

TEST_CASE("the_state_headline_wears_the_class_of_its_level") {
    StatusPage ready{std::string(kServiceName), std::string(kServiceVersion)};
    ready.state(std::string(kReadyHeadline), State::Ok);
    CHECK(ready.render().contains(state_line_markup(kReadyHeadline, state_class(State::Ok))));

    StatusPage broken{std::string(kServiceName), std::string(kServiceVersion)};
    broken.state(std::string(kDegradedHeadline), State::Bad);
    CHECK(broken.render().contains(
        state_line_markup(kDegradedHeadline, state_class(State::Bad))));
}

TEST_CASE("an_empty_state_headline_is_omitted_rather_than_rendered_empty") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.state("", State::Ok);

    CHECK_FALSE(page.render().contains("<strong>"));
}

TEST_CASE("a_section_spans_both_columns") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.section(kSectionHeading);
    page.row(kUptimeLabel, kUptimeValue);

    const std::string rendered = page.render();
    CHECK(rendered.contains(
        std::format("  <tr><td colspan=\"2\"><strong>{}</strong></td></tr>\n", kSectionHeading)));
    CHECK(rendered.contains(plain_row_markup(kUptimeLabel, kUptimeValue)));
}

TEST_CASE("footer_links_are_emitted_in_registration_order_separated_by_a_pipe") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.link(kMetricsHref, kMetricsText);
    page.link(kHealthzHref, kHealthzText);

    CHECK(page.render().contains(std::format("<p><a href=\"{}\">{}</a> | <a href=\"{}\">{}</a></p>",
                                             kMetricsHref, kMetricsText, kHealthzHref,
                                             kHealthzText)));
}

TEST_CASE("an_unset_pid_drops_the_pid_segment_instead_of_printing_a_wrong_one") {
    const StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    const std::string rendered = page.render();

    CHECK(rendered.contains(std::format("<h1>{} <small>v{}</small></h1>", kServiceName,
                                        kServiceVersion)));
    CHECK_FALSE(rendered.contains(" | pid "));
}

TEST_CASE("a_set_pid_appears_beside_the_version") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.pid(kProcessId);

    CHECK(page.render().contains(std::format("<h1>{} <small>v{} | pid {}</small></h1>",
                                             kServiceName, kServiceVersion, kProcessId)));
}

TEST_CASE("the_default_page_contains_no_script_at_all") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.state(std::string(kReadyHeadline), State::Ok);
    page.row(kUptimeLabel, kUptimeValue);
    page.link(kHealthzHref, kHealthzText);
    const std::string rendered = page.render();

    CHECK_MESSAGE(!rendered.contains(kRawScriptOpening),
                  "the page has to work in a text browser, in a curl pipe, and under a policy "
                  "that forbids inline script");
    CHECK_FALSE(rendered.contains(kNoscriptOpening));
    CHECK_FALSE(rendered.contains(kLiveRowsTbody));
}

TEST_CASE("live_updates_add_the_event_source_script_and_the_patchable_tbody") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kUptimeLabel, kUptimeValue);
    page.live_updates(true);
    const std::string rendered = page.render();

    CHECK(count_occurrences(rendered, kRawScriptOpening) == kExactlyOneScriptElement);
    CHECK(rendered.contains(kEventSourceCall));
    CHECK(rendered.contains(kLiveRowsTbody));
    CHECK(rendered.contains("JSON.parse(event.data)"));
    CHECK(rendered.contains("value.textContent = update.value"));
    CHECK_FALSE(rendered.contains("innerHTML"));
    CHECK(rendered.contains(kTbodyClosing));
    CHECK_MESSAGE(rendered.contains(plain_row_markup(kUptimeLabel, kUptimeValue)),
                  "the rows the script replaces must be inside the element it replaces them in");
}

TEST_CASE("live_updates_move_the_meta_refresh_inside_noscript") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.live_updates(true);
    const std::string rendered = page.render();

    CHECK(rendered.contains(
        std::format("<noscript><meta http-equiv=\"refresh\" content=\"{}\"></noscript>",
                    kCanonicalRefreshSeconds)));
    CHECK_MESSAGE(count_occurrences(rendered, kRefreshAttribute) == kExactlyOneRefreshMeta,
                  "a second, bare refresh would reload the page out from under the script");
}

TEST_CASE("live_updates_with_no_refresh_emit_the_script_and_no_noscript") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.refresh_seconds(kNoRefreshSeconds);
    page.live_updates(true);
    const std::string rendered = page.render();

    CHECK(count_occurrences(rendered, kRawScriptOpening) == kExactlyOneScriptElement);
    CHECK_FALSE(rendered.contains(kNoscriptOpening));
    CHECK_FALSE(rendered.contains(kRefreshAttribute));
}

TEST_CASE("live_updates_turned_back_off_leave_no_trace_of_the_script") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.live_updates(true);
    page.live_updates(false);

    CHECK(page.render() ==
          canonical_empty_page(kServiceName, kServiceVersion, kCanonicalRefreshSeconds));
}

TEST_CASE("a_chain_assigned_to_a_value_renders_the_same_page_as_a_chain_on_an_lvalue") {
    StatusPage built_on_an_lvalue{std::string(kServiceName), std::string(kServiceVersion)};
    built_on_an_lvalue.pid(kProcessId)
        .state(std::string(kReadyHeadline), State::Ok)
        .row(kUptimeLabel, kUptimeValue)
        .link(kHealthzHref, kHealthzText);

    const StatusPage built_on_a_temporary =
        StatusPage(std::string(kServiceName), std::string(kServiceVersion))
            .pid(kProcessId)
            .state(std::string(kReadyHeadline), State::Ok)
            .row(kUptimeLabel, kUptimeValue)
            .link(kHealthzHref, kHealthzText);

    CHECK(built_on_an_lvalue.render() == built_on_a_temporary.render());
}

TEST_CASE("a_negative_refresh_interval_is_clamped_to_no_refresh") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.refresh_seconds(kNegativeRefreshSeconds);
    const std::string rendered = page.render();

    CHECK_MESSAGE(!rendered.contains(kRefreshAttribute),
                  "content=\"-1\" reads as reload immediately, which is a request flood from a "
                  "page whose whole purpose is to be cheap");
    CHECK(rendered == canonical_empty_page_without_refresh(kServiceName, kServiceVersion));
}

TEST_CASE("the_live_script_policy_source_is_the_digest_of_the_script_the_page_ships") {
    CHECK_MESSAGE(live_update_script_csp_source() == kLiveScriptCspHashSource,
                  "the expected value was produced by sha256sum and openssl over the script's own "
                  "bytes, not by this library; a policy that agreed only with itself would pin "
                  "nothing, and a browser recomputes this digest before it runs the script");
}

TEST_CASE("the_page_carries_the_exact_bytes_the_live_script_digest_was_taken_over") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.live_updates(true);
    const std::string rendered = page.render();

    const size_t opening = rendered.find(kScriptElementOpening);
    REQUIRE(opening != std::string::npos);
    const size_t text_start = opening + kScriptElementOpening.size();
    const size_t closing = rendered.find(kScriptElementClosing, text_start);
    REQUIRE(closing != std::string::npos);

    CHECK_MESSAGE(rendered.substr(text_start, closing - text_start) == kLiveScriptText,
                  "a browser hashes the element's text content exactly as the document spells it, "
                  "so a stray byte between the tags stops the one authorised script from running");
}
