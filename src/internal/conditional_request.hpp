#pragma once

#include <string_view>

namespace erikslund::http::internal {

// Weak comparison against '*' or a comma-separated ETag list.
[[nodiscard]] bool if_none_match_matches(std::string_view field, std::string_view etag) noexcept;

} // namespace erikslund::http::internal
