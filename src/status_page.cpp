#include "erikslund/http/status_page.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "erikslund/http/text.hpp"
#include "internal/sha256.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kDocumentPrologue =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n";

constexpr std::string_view kInlineIconLink = "<link rel=\"icon\" href=\"data:,\">\n";

constexpr std::string_view kViewportMeta =
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";

constexpr std::string_view kBodyOpening = "</style></head><body>\n";
constexpr std::string_view kDocumentEpilogue = "</body></html>\n";

constexpr std::string_view kLiveRowsElementId = "status-rows";

constexpr size_t kEstimatedScaffoldBytes = 512;
constexpr size_t kEstimatedRowOverheadBytes = 48;
constexpr size_t kEstimatedLinkOverheadBytes = 32;

constexpr std::string_view kScriptElementOpening = "<script>";
constexpr std::string_view kScriptElementClosing = "</script>\n";
constexpr std::string_view kLiveUpdateScriptText =
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

static_assert(kDefaultEventsPath == "/events",
              "the inline live-update script hard-codes the events path; keep the two in step");
static_assert(kLiveRowsElementId == "status-rows",
              "the inline live-update script hard-codes the table body id; keep the two in step");

constexpr std::string_view kCspHashSourcePrefix = "'sha256-";
constexpr std::string_view kCspHashSourceSuffix = "'";
constexpr size_t kCspHashSourceLength =
    kCspHashSourcePrefix.size() + kBase64DigestLength + kCspHashSourceSuffix.size();

struct CspHashSource {
    std::array<char, kCspHashSourceLength> characters{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(characters.data(), characters.size());
    }
};

[[nodiscard]] constexpr CspHashSource csp_hash_source_of(std::string_view script_text) noexcept {
    const Base64Digest encoded = base64_of_digest(sha256(script_text));
    CspHashSource source{};
    size_t written = 0;
    for (const char character : kCspHashSourcePrefix)
        source.characters[written++] = character;
    for (const char character : encoded.view())
        source.characters[written++] = character;
    for (const char character : kCspHashSourceSuffix)
        source.characters[written++] = character;
    return source;
}

constexpr CspHashSource kLiveUpdateScriptCspSource = csp_hash_source_of(kLiveUpdateScriptText);

constexpr std::string_view kHttpSchemePrefix = "http://";
constexpr std::string_view kHttpsSchemePrefix = "https://";
constexpr char kPathSeparator = '/';
constexpr char kFoldedPathSeparator = '\\';
constexpr uint8_t kSpaceByte = 0x20;
constexpr uint8_t kDeleteByte = 0x7F;

[[nodiscard]] bool has_only_url_bytes(std::string_view href) noexcept {
    for (const char character : href) {
        const auto byte = static_cast<uint8_t>(character);
        if (byte <= kSpaceByte || byte == kDeleteByte)
            return false;
    }
    return true;
}

[[nodiscard]] bool starts_with_ignore_case(std::string_view text,
                                           std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool has_authority_after(std::string_view href, std::string_view scheme) noexcept {
    const std::string_view authority = href.substr(scheme.size());
    return !authority.empty() && authority.front() != kPathSeparator;
}

[[nodiscard]] bool is_allowed_link_href(std::string_view href) noexcept {
    if (href.empty() || !has_only_url_bytes(href))
        return false;
    if (href.contains(kFoldedPathSeparator))
        return false;
    if (starts_with_ignore_case(href, kHttpSchemePrefix))
        return has_authority_after(href, kHttpSchemePrefix);
    if (starts_with_ignore_case(href, kHttpsSchemePrefix))
        return has_authority_after(href, kHttpsSchemePrefix);
    if (href.front() != kPathSeparator)
        return false;
    return href.size() == 1 || href[1] != kPathSeparator;
}

constexpr std::string_view kRefusedLinkOpening = "<span class=\"bad\">";
constexpr std::string_view kRefusedLinkClosing = "</span>";

} // namespace

StatusPage::StatusPage(std::string service_name, std::string version)
    : service_name_(std::move(service_name)), version_(std::move(version)) {}

void StatusPage::set_pid(int process_id) { process_id_ = process_id; }

void StatusPage::set_refresh_seconds(int seconds) {
    refresh_seconds_ = seconds > 0 ? seconds : 0;
}

void StatusPage::set_state(std::string headline, State level) {
    state_headline_ = std::move(headline);
    state_level_ = level;
}

void StatusPage::add_row(std::string_view label, std::string_view value, State level,
                         bool show_level_color) {
    elements_.push_back(Element{.kind = ElementKind::Row,
                                .label = std::string(label),
                                .value = std::string(value),
                                .level = level,
                                .show_level_color = show_level_color});
}

void StatusPage::add_section(std::string_view heading) {
    elements_.push_back(
        Element{.kind = ElementKind::Section, .label = std::string(heading), .value = {}});
}

void StatusPage::add_link(std::string_view href, std::string_view text) {
    links_.push_back(Link{.href = std::string(href), .text = std::string(text)});
}

void StatusPage::set_live_updates(bool enabled) { live_updates_ = enabled; }

std::string StatusPage::render() const {
    size_t estimated_bytes = kEstimatedScaffoldBytes + kStatusPageStyle.size() +
                             service_name_.size() + version_.size() + state_headline_.size();
    for (const Element& element : elements_)
        estimated_bytes += element.label.size() + element.value.size() + kEstimatedRowOverheadBytes;
    for (const Link& entry : links_)
        estimated_bytes += entry.href.size() + entry.text.size() + kEstimatedLinkOverheadBytes;
    if (live_updates_)
        estimated_bytes += kScriptElementOpening.size() + kLiveUpdateScriptText.size() +
                           kScriptElementClosing.size();

    std::string out;
    out.reserve(estimated_bytes);

    out += kDocumentPrologue;
    out += kInlineIconLink;

    if (refresh_seconds_ > 0) {
        const std::string refresh_meta =
            std::format("<meta http-equiv=\"refresh\" content=\"{}\">", refresh_seconds_);
        if (live_updates_)
            out += "<noscript>" + refresh_meta + "</noscript>";
        else
            out += refresh_meta;
        out += '\n';
    }

    out += kViewportMeta;

    out += "<title>";
    html_escape_into(out, service_name_);
    out += " v";
    html_escape_into(out, version_);
    out += "</title>\n<style>\n";
    out += kStatusPageStyle;
    out += '\n';
    out += kBodyOpening;

    out += "<h1>";
    html_escape_into(out, service_name_);
    out += " <small>v";
    html_escape_into(out, version_);
    if (process_id_ != 0)
        out += std::format(" | pid {}", process_id_);
    out += "</small></h1>\n";

    if (!state_headline_.empty()) {
        out += "<p class=\"";
        out += state_class(state_level_);
        out += "\"><strong>";
        html_escape_into(out, state_headline_);
        out += "</strong></p>\n";
    }

    out += "<table>\n";
    if (live_updates_) {
        out += "<tbody id=\"";
        out += kLiveRowsElementId;
        out += "\">\n";
    }

    for (const Element& element : elements_) {
        if (element.kind == ElementKind::Section) {
            out += "  <tr><td colspan=\"2\"><strong>";
            html_escape_into(out, element.label);
            out += "</strong></td></tr>\n";
            continue;
        }
        out += "  <tr><td>";
        html_escape_into(out, element.label);
        out += "</td><td";
        if (element.show_level_color) {
            out += " class=\"";
            out += state_class(element.level);
            out += '"';
        }
        out += '>';
        html_escape_into(out, element.value);
        out += "</td></tr>\n";
    }

    if (live_updates_)
        out += "</tbody>\n";
    out += "</table>\n";

    if (!links_.empty()) {
        out += "<p>";
        bool first = true;
        for (const Link& entry : links_) {
            if (!first)
                out += " | ";
            first = false;
            if (!is_allowed_link_href(entry.href)) {
                out += kRefusedLinkOpening;
                html_escape_into(out, entry.text);
                out += kRefusedLinkClosing;
                continue;
            }
            out += "<a href=\"";
            html_escape_into(out, entry.href);
            out += "\">";
            html_escape_into(out, entry.text);
            out += "</a>";
        }
        out += "</p>\n";
    }

    if (live_updates_) {
        out += kScriptElementOpening;
        out += kLiveUpdateScriptText;
        out += kScriptElementClosing;
    }
    out += kDocumentEpilogue;
    return out;
}

std::string_view live_update_script_csp_source() noexcept {
    return kLiveUpdateScriptCspSource.view();
}

} // namespace erikslund::http
