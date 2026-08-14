#include "internal/conditional_request.hpp"

#include <string_view>

namespace erikslund::http::internal {
namespace {

[[nodiscard]] std::string_view trim_optional_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string_view without_weak_marker(std::string_view tag) noexcept {
    constexpr std::string_view kWeakMarker = "W/";
    if (tag.starts_with(kWeakMarker))
        tag.remove_prefix(kWeakMarker.size());
    return tag;
}

} // namespace

bool if_none_match_matches(std::string_view field, std::string_view etag) noexcept {
    if (etag.empty())
        return false;
    constexpr std::string_view kAnyEntity = "*";
    const std::string_view wanted = without_weak_marker(etag);

    size_t cursor = 0;
    while (cursor <= field.size()) {
        const size_t comma = field.find(',', cursor);
        const size_t length =
            comma == std::string_view::npos ? std::string_view::npos : comma - cursor;
        const std::string_view candidate =
            trim_optional_whitespace(field.substr(cursor, length));
        if (candidate == kAnyEntity ||
            (!candidate.empty() && without_weak_marker(candidate) == wanted))
            return true;
        if (comma == std::string_view::npos)
            break;
        cursor = comma + 1;
    }
    return false;
}

} // namespace erikslund::http::internal
