
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/status_page.hpp"

using namespace erikslund::http;
using namespace std::string_view_literals;

namespace {

constexpr std::string_view kServiceName = "erikslund-http";
constexpr std::string_view kServiceVersion = "0.1.2";
constexpr int kProcessId = 4'242;

constexpr std::string_view kUptimeLabel = "uptime";
constexpr std::string_view kLinkText = "operator link";
constexpr std::string_view kSecondLinkText = "second";
constexpr std::string_view kKnownGoodHref = "/metrics";

constexpr std::string_view kScriptInjection = "<script>alert(1)</script>";
constexpr std::string_view kEscapedScriptInjection = "&lt;script&gt;alert(1)&lt;/script&gt;";
constexpr std::string_view kQuotedAmpersandValue = "\"drop\" & 'run' <now>";
constexpr std::string_view kEscapedQuotedAmpersandValue =
    "&quot;drop&quot; &amp; &#39;run&#39; &lt;now&gt;";

constexpr std::string_view kDoubleEscapedAmpersand = "&amp;amp;";

constexpr std::string_view kAttributeBreakoutValue = "\" onmouseover=\"alert(1)";
constexpr std::string_view kRawAttributeBreakout = "\" onmouseover=\"";

constexpr std::string_view kApostropheValue = "it's up";
constexpr std::string_view kEscapedApostropheValue = "it&#39;s up";

constexpr std::string_view kUnicodeValue = "n\u00e4tverk 42 \u00b5s";
constexpr std::string_view kUnicodePathHref = "/n\u00e4tverk";

constexpr std::string_view kAnchorOpening = "<a href=\"";
constexpr std::string_view kInertLinkOpening = "<span class=\"bad\">";
constexpr std::string_view kInertLinkClosing = "</span>";
constexpr std::string_view kLinkSeparator = " | ";
constexpr std::string_view kRawScriptOpening = "<script";

constexpr std::string_view kAttributeOpening = "=\"";

constexpr std::string_view kCspHashSourcePrefix = "'sha256-";
constexpr std::string_view kInlineAllowance = "unsafe-inline";

constexpr size_t kNoAnchors = 0;
constexpr size_t kOneAnchor = 1;
constexpr size_t kNoScriptElements = 0;
constexpr size_t kExactlyOneScriptElement = 1;

struct HostileHref {
    std::string_view href;
    std::string_view evasion;
};

constexpr auto kRefusedHrefs = std::to_array<HostileHref>({
    {"javascript:alert(document.domain)"sv, "the bare scheme"},
    {"javascript:alert('quoted')"sv, "a quoted payload"},
    {"data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg=="sv, "a base64 data uri"},
    {"data:text/html,<script>alert(1)</script>"sv, "a data uri spelled in the clear"},
    {"JaVaScRiPt:alert(1)"sv, "case folding"},
    {"JAVASCRIPT:alert(1)"sv, "upper case"},
    {"  javascript:alert(1)"sv, "a leading space"},
    {"\tjavascript:alert(1)"sv, "a leading tab"},
    {"\njavascript:alert(1)"sv, "a leading newline"},
    {"\rjavascript:alert(1)"sv, "a leading carriage return"},
    {"\0javascript:alert(1)"sv, "a leading nul"},
    {"java\tscript:alert(1)"sv, "a tab inside the scheme"},
    {"java\nscript:alert(1)"sv, "a newline inside the scheme"},
    {"java\rscript:alert(1)"sv, "a carriage return inside the scheme"},
    {"java\0script:alert(1)"sv, "a nul inside the scheme"},
    {"java\x01script:alert(1)"sv, "a control byte inside the scheme"},
    {"javascript\x7f:alert(1)"sv, "a delete byte before the colon"},
    {"javascript\t:alert(1)"sv, "a tab before the colon"},
    {"vbscript:msgbox(1)"sv, "another scripting scheme"},
    {"file:///etc/passwd"sv, "a local file"},
    {"blob:https://erikslund.test/1"sv, "a blob url"},
    {"about:blank"sv, "an about url"},
    {"mailto:hey@eandersson.net"sv, "a scheme with no origin"},
    {"//evil.example/status"sv, "a protocol-relative url"},
    {"/\\evil.example/status"sv, "a backslash a browser folds into a second slash"},
    {"\\\\evil.example\\status"sv, "a unc path"},
    {"http://"sv, "an http url with no authority"},
    {"http:///status"sv, "an http url with an empty authority"},
    {"https:/status"sv, "a scheme with a single slash"},
    {"http:javascript:alert(1)"sv, "a scheme prefix with no authority marker"},
    {"metrics"sv, "a bare relative reference"},
    {"/status onmouseover=x"sv, "a space inside an otherwise ordinary path"},
    {"/status\tonmouseover=x"sv, "a tab inside an otherwise ordinary path"},
    {"\" onmouseover=\"alert(1)"sv, "an attribute breakout"},
});

constexpr auto kAcceptedHrefs = std::to_array<std::string_view>({
    "/"sv,
    "/metrics"sv,
    "/healthz"sv,
    "/a/b?c=d#e"sv,
    "/a:b"sv,
    "http://erikslund.test:7777/status"sv,
    "https://erikslund.test/status"sv,
    "HTTPS://erikslund.test/status"sv,
    "HtTp://erikslund.test/"sv,
    kUnicodePathHref,
});

[[nodiscard]] size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return 0;
    size_t found = 0;
    for (size_t at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size()))
        ++found;
    return found;
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

[[nodiscard]] std::string anchor_markup(std::string_view href, std::string_view text) {
    return std::format("<a href=\"{}\">{}</a>", href, text);
}

[[nodiscard]] StatusPage fully_hostile_page() {
    StatusPage page{std::string(kScriptInjection), std::string(kQuotedAmpersandValue)};
    page.pid(kProcessId);
    page.state(std::string(kScriptInjection), State::Bad);
    page.section(kScriptInjection);
    page.row(kScriptInjection, kQuotedAmpersandValue);
    page.row(kQuotedAmpersandValue, kScriptInjection, State::Warn);
    page.link(kScriptInjection, kQuotedAmpersandValue);
    return page;
}

[[nodiscard]] StatusPage benign_page_of_the_same_shape() {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.pid(kProcessId);
    page.state(std::string(kUptimeLabel), State::Bad);
    page.section(kUptimeLabel);
    page.row(kUptimeLabel, kServiceVersion);
    page.row(kServiceVersion, kUptimeLabel, State::Warn);
    page.link(kKnownGoodHref, kLinkText);
    return page;
}

[[nodiscard]] StatusPage page_with_one_link(std::string_view href) {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.link(href, kLinkText);
    return page;
}

} // namespace

TEST_SUITE("status page adversarial") {

TEST_CASE("escapes_a_script_tag_in_the_service_name") {
    const StatusPage page{std::string(kScriptInjection), std::string(kServiceVersion)};
    const std::string rendered = page.render();

    CHECK_FALSE(rendered.contains(kRawScriptOpening));
    CHECK(rendered.contains(
        std::format("<title>{} v{}</title>", kEscapedScriptInjection, kServiceVersion)));
    CHECK(rendered.contains(
        std::format("<h1>{} <small>v{}</small></h1>", kEscapedScriptInjection, kServiceVersion)));
}

TEST_CASE("escapes_a_script_tag_in_a_row_label_and_in_a_row_value") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kScriptInjection, kQuotedAmpersandValue);
    page.row(kQuotedAmpersandValue, kScriptInjection, State::Bad);
    const std::string rendered = page.render();

    CHECK_FALSE(rendered.contains(kRawScriptOpening));
    CHECK(rendered.contains(
        plain_row_markup(kEscapedScriptInjection, kEscapedQuotedAmpersandValue)));
    CHECK(rendered.contains(classed_row_markup(kEscapedQuotedAmpersandValue,
                                               kEscapedScriptInjection,
                                               state_class(State::Bad))));
}

TEST_CASE("escapes_a_script_tag_in_the_state_headline_and_in_a_section_heading") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.state(std::string(kScriptInjection), State::Warn);
    page.section(kScriptInjection);
    const std::string rendered = page.render();

    CHECK_FALSE(rendered.contains(kRawScriptOpening));
    CHECK(rendered.contains(state_line_markup(kEscapedScriptInjection, state_class(State::Warn))));
    CHECK(rendered.contains(std::format(
        "  <tr><td colspan=\"2\"><strong>{}</strong></td></tr>\n", kEscapedScriptInjection)));
}

TEST_CASE("escapes_quotes_and_ampersands_without_double_escaping_the_ampersand") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kUptimeLabel, kQuotedAmpersandValue);
    const std::string rendered = page.render();

    CHECK(rendered.contains(plain_row_markup(kUptimeLabel, kEscapedQuotedAmpersandValue)));
    CHECK_MESSAGE(!rendered.contains(kDoubleEscapedAmpersand),
                  "an ampersand pass that ran after the others would show the operator the entity "
                  "instead of the character");
}

TEST_CASE("a_single_quote_becomes_the_numeric_reference_older_parsers_understand") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kUptimeLabel, kApostropheValue);

    CHECK_MESSAGE(page.render().contains(plain_row_markup(kUptimeLabel, kEscapedApostropheValue)),
                  "&apos; was never defined by HTML 4, so an older parser renders it literally");
}

TEST_CASE("utf8_values_pass_through_unchanged") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.row(kUptimeLabel, kUnicodeValue);

    CHECK(page.render().contains(plain_row_markup(kUptimeLabel, kUnicodeValue)));
}

TEST_CASE("no_raw_script_opening_survives_anywhere_on_a_fully_hostile_page") {
    const StatusPage page = fully_hostile_page();
    const std::string rendered = page.render();

    CHECK(count_occurrences(rendered, kRawScriptOpening) == kNoScriptElements);
    CHECK_FALSE(rendered.contains(kDoubleEscapedAmpersand));
    CHECK(rendered.contains(kEscapedScriptInjection));
    CHECK(rendered.contains(kEscapedQuotedAmpersandValue));
}

TEST_CASE("a_fully_hostile_page_with_live_updates_carries_exactly_one_script_element") {
    StatusPage page = fully_hostile_page();
    page.live_updates(true);
    const std::string rendered = page.render();

    CHECK_MESSAGE(count_occurrences(rendered, kRawScriptOpening) == kExactlyOneScriptElement,
                  "the only script on the page must be the one the class wrote itself");
}

TEST_CASE("no_href_outside_the_allowlist_reaches_the_page_in_any_spelling") {
    for (const HostileHref& probe : kRefusedHrefs) {
        const std::string rendered = page_with_one_link(probe.href).render();

        CHECK_MESSAGE(count_occurrences(rendered, kAnchorOpening) == kNoAnchors,
                      "an anchor survived ", probe.evasion);
        CHECK_MESSAGE(!rendered.contains(probe.href),
                      "the refused target itself reached the document for ", probe.evasion);
    }
}

TEST_CASE("a_same_origin_path_and_an_explicit_http_url_are_the_two_forms_that_link") {
    for (const std::string_view href : kAcceptedHrefs) {
        const std::string rendered = page_with_one_link(href).render();

        CHECK_MESSAGE(count_occurrences(rendered, kAnchorOpening) == kOneAnchor,
                      "the allowlist refused a target an operator legitimately writes: ", href);
        CHECK(rendered.contains(anchor_markup(href, kLinkText)));
    }
}

TEST_CASE("a_refused_href_renders_its_text_inert_rather_than_dropping_the_entry") {
    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.link(kRefusedHrefs.front().href, kLinkText);
    page.link(kKnownGoodHref, kSecondLinkText);
    const std::string rendered = page.render();

    CHECK_MESSAGE(rendered.contains(std::format("{}{}{}", kInertLinkOpening, kLinkText,
                                                kInertLinkClosing)),
                  "an operator who mistyped a link has to see that the entry is there and is not "
                  "clickable; one that silently vanished would teach nobody that it was refused");
    CHECK_MESSAGE(rendered.contains(kLinkSeparator),
                  "the refused entry still occupies its place in the footer, separators and all");
    CHECK(rendered.contains(anchor_markup(kKnownGoodHref, kSecondLinkText)));
}

TEST_CASE("an_empty_href_renders_its_text_inert_rather_than_linking_to_the_page_itself") {
    const std::string rendered = page_with_one_link("").render();

    CHECK(count_occurrences(rendered, kAnchorOpening) == kNoAnchors);
    CHECK(rendered.contains(std::format("{}{}{}", kInertLinkOpening, kLinkText,
                                        kInertLinkClosing)));
}

TEST_CASE("hostile_text_at_every_interpolation_point_opens_no_attribute_of_its_own") {
    const std::string hostile = fully_hostile_page().render();
    const std::string benign = benign_page_of_the_same_shape().render();

    CHECK_MESSAGE(count_occurrences(hostile, kAttributeOpening) ==
                      count_occurrences(benign, kAttributeOpening),
                  "the href is the only attribute this page writes from caller data, so two pages "
                  "of the same shape must open the same number of attributes whatever the strings");
}

TEST_CASE("no_caller_string_can_open_a_second_attribute_anywhere_on_the_page") {
    StatusPage page{std::string(kAttributeBreakoutValue), std::string(kAttributeBreakoutValue)};
    page.state(std::string(kAttributeBreakoutValue), State::Bad);
    page.section(kAttributeBreakoutValue);
    page.row(kAttributeBreakoutValue, kAttributeBreakoutValue, State::Warn);
    page.link(kAttributeBreakoutValue, kAttributeBreakoutValue);
    page.live_updates(true);

    CHECK_FALSE(page.render().contains(kRawAttributeBreakout));
}

TEST_CASE("the_live_page_policy_names_a_digest_instead_of_allowing_inline_script") {
    const std::string_view source = live_update_script_csp_source();

    CHECK(source.starts_with(kCspHashSourcePrefix));
    CHECK_MESSAGE(!source.contains(kInlineAllowance),
                  "an inline allowance is what lets a javascript: href run at all; a digest "
                  "authorises exactly the one script this class wrote and nothing else");
}

} // TEST_SUITE("status page adversarial")
