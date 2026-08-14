#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace erikslund::http {

inline constexpr uint64_t kAbbreviateThreshold = 10'000;

inline constexpr uint64_t kBytesPerKibibyte = 1024;

inline constexpr size_t kWeakEtagLength = 20;

// FNV-1a is used for lookup and ETags, never for security.
inline constexpr uint64_t kFnvOffsetBasis64 = 14695981039346656037ULL;
inline constexpr uint64_t kFnvPrime64 = 1099511628211ULL;

[[nodiscard]] constexpr uint64_t fnv1a_64(std::string_view input) noexcept {
    uint64_t hash = kFnvOffsetBasis64;
    for (const char character : input) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(character));
        hash *= kFnvPrime64;
    }
    return hash;
}

// HTTP field names are ASCII; locale must not affect matching.
[[nodiscard]] constexpr char ascii_to_lower(char character) noexcept {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character + ('a' - 'A'))
                                                  : character;
}

[[nodiscard]] constexpr bool equals_ignore_case(std::string_view left,
                                                std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (ascii_to_lower(left[index]) != ascii_to_lower(right[index]))
            return false;
    return true;
}

// Storage backing a compile-time Asset ETag.
struct EtagBuffer {
    std::array<char, kWeakEtagLength> characters{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(characters.data(), characters.size());
    }
};

[[nodiscard]] constexpr EtagBuffer weak_etag_buffer(uint64_t hash) noexcept {
    constexpr std::string_view kHexDigits = "0123456789abcdef";
    constexpr size_t kHexDigitCount = 16;
    constexpr size_t kBitsPerHexDigit = 4;

    EtagBuffer buffer{};
    buffer.characters[0] = 'W';
    buffer.characters[1] = '/';
    buffer.characters[2] = '"';
    for (size_t index = 0; index < kHexDigitCount; ++index) {
        const unsigned shift =
            static_cast<unsigned>((kHexDigitCount - 1 - index) * kBitsPerHexDigit);
        buffer.characters[3 + index] = kHexDigits[(hash >> shift) & 0xF];
    }
    buffer.characters[kWeakEtagLength - 1] = '"';
    return buffer;
}

[[nodiscard]] constexpr EtagBuffer weak_etag_buffer(std::string_view bytes) noexcept {
    return weak_etag_buffer(fnv1a_64(bytes));
}

[[nodiscard]] std::string html_escape(std::string_view input);

void html_escape_into(std::string& out, std::string_view input);

// Produces JSON string content without surrounding quotes.
[[nodiscard]] std::string json_escape(std::string_view input);

[[nodiscard]] std::string prometheus_label_escape(std::string_view input);

// Decodes percent escapes and '+'; rejects malformed escapes and NUL.
[[nodiscard]] std::optional<std::string> url_decode(std::string_view input);

[[nodiscard]] std::string format_duration(std::chrono::seconds total);

[[nodiscard]] std::string format_count(uint64_t value);

[[nodiscard]] std::string format_bytes(uint64_t bytes);

[[nodiscard]] std::string group_digits(uint64_t value);

[[nodiscard]] std::string weak_etag(std::string_view body);

// Compares equal-length secrets without revealing their matching-prefix length. Lengths are public.
[[nodiscard]] bool constant_time_equals(std::string_view left, std::string_view right);

// RFC 9110 IMF-fixdate in GMT and the C locale.
[[nodiscard]] std::string http_date(std::chrono::system_clock::time_point when);

static_assert(equals_ignore_case("Content-Length", "content-length"));
static_assert(weak_etag_buffer(fnv1a_64("")).view().size() == kWeakEtagLength);

} // namespace erikslund::http
