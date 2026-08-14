#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace erikslund::http::internal {

// Smaller bodies already fit in one ordinary TCP segment.
inline constexpr size_t kMinimumCompressibleBodyBytes = 1'024;

// Bounds simultaneous plain and compressed storage.
inline constexpr size_t kMaximumCompressibleBodyBytes = 8'388'608;

// Describes the resource, not one request, so Vary remains correct.
[[nodiscard]] bool response_varies_on_accept_encoding(std::string_view content_type,
                                                      std::string_view content_encoding,
                                                      size_t body_bytes) noexcept;

// On failure or expansion, returns false and leaves out empty.
[[nodiscard]] bool gzip_compress(std::string_view body, std::string& out);

} // namespace erikslund::http::internal
