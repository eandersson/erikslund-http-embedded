#include "erikslund/http/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "erikslund/http/contracts.hpp"

namespace erikslund::http {

namespace {

constexpr size_t kEscapeHeadroomDivisor = 8;

constexpr std::string_view kLowerHexDigits = "0123456789abcdef";
constexpr unsigned int kBitsPerHexDigit = 4;
constexpr size_t kLowHexDigitMask = 0x0F;
constexpr int kHexRadix = 16;
constexpr int kDecimalRadix = 10;
constexpr int kInvalidHexDigit = -1;

constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kDelete = 0x7F;
constexpr unsigned char kFirstTwoByteLead = 0xC2;
constexpr unsigned char kLastTwoByteLead = 0xDF;
constexpr unsigned char kFirstThreeByteLead = 0xE0;
constexpr unsigned char kLastThreeByteLead = 0xEF;
constexpr unsigned char kFirstFourByteLead = 0xF0;
constexpr unsigned char kLastFourByteLead = 0xF4;
constexpr unsigned char kContinuationMin = 0x80;
constexpr unsigned char kContinuationMax = 0xBF;
constexpr unsigned char kE0SecondMin = 0xA0;
constexpr unsigned char kSurrogateLead = 0xED;
constexpr unsigned char kEdSecondMax = 0x9F;
constexpr unsigned char kF0SecondMin = 0x90;
constexpr unsigned char kF4SecondMax = 0x8F;

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

constexpr size_t kPercentEscapeLength = 3;

constexpr char kDigitGroupSeparator = ',';
constexpr size_t kDigitGroupSize = 3;

constexpr size_t kMaxUint64Digits = 20;

constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kMinutesPerHour = 60;
constexpr int64_t kHoursPerDay = 24;
constexpr int64_t kSecondsPerHour = kSecondsPerMinute * kMinutesPerHour;
constexpr int64_t kSecondsPerDay = kSecondsPerHour * kHoursPerDay;

constexpr double kCountAbbreviationBase = 1'000.0;
constexpr auto kCountSuffixes = std::to_array<std::string_view>({"K", "M", "G", "T", "P", "E"});

constexpr double kKibibyteDivisor = static_cast<double>(kBytesPerKibibyte);
constexpr auto kBinaryUnitSuffixes =
    std::to_array<std::string_view>({"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"});

constexpr int kAbbreviatedFractionDigits = 1;

[[nodiscard]] constexpr int hex_digit_value(char character) noexcept {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + kDecimalRadix;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + kDecimalRadix;
    return kInvalidHexDigit;
}

[[nodiscard]] constexpr bool is_continuation(unsigned char byte) noexcept {
    return byte >= kContinuationMin && byte <= kContinuationMax;
}

[[nodiscard]] size_t valid_utf8_sequence_length(std::string_view input, size_t index) noexcept {
    const auto byte = [&input](size_t at) { return static_cast<unsigned char>(input[at]); };
    const unsigned char first = byte(index);
    const size_t remaining = input.size() - index;

    if (first < kContinuationMin)
        return 1;
    if (first >= kFirstTwoByteLead && first <= kLastTwoByteLead)
        return remaining >= 2 && is_continuation(byte(index + 1)) ? 2 : 0;
    if (first >= kFirstThreeByteLead && first <= kLastThreeByteLead) {
        if (remaining < 3 || !is_continuation(byte(index + 2)))
            return 0;
        const unsigned char second = byte(index + 1);
        if (!is_continuation(second))
            return 0;
        if (first == kFirstThreeByteLead && second < kE0SecondMin)
            return 0;
        if (first == kSurrogateLead && second > kEdSecondMax)
            return 0;
        return 3;
    }
    if (first >= kFirstFourByteLead && first <= kLastFourByteLead) {
        if (remaining < 4 || !is_continuation(byte(index + 2)) ||
            !is_continuation(byte(index + 3)))
            return 0;
        const unsigned char second = byte(index + 1);
        if (!is_continuation(second))
            return 0;
        if (first == kFirstFourByteLead && second < kF0SecondMin)
            return 0;
        if (first == kLastFourByteLead && second > kF4SecondMax)
            return 0;
        return 4;
    }
    return 0;
}

void append_utf8_or_replacement(std::string& out, std::string_view input, size_t& index) {
    const size_t sequence_length = valid_utf8_sequence_length(input, index);
    if (sequence_length == 0) {
        out += kReplacementCharacter;
        ++index;
        return;
    }
    out.append(input.substr(index, sequence_length));
    index += sequence_length;
}

} // namespace

void html_escape_into(std::string& out, std::string_view input) {
    out.reserve(out.size() + input.size() + input.size() / kEscapeHeadroomDivisor);

    for (const char character : input) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += character;
        }
    }
}

std::string html_escape(std::string_view input) {
    std::string escaped;
    html_escape_into(escaped, input);
    return escaped;
}

std::string json_escape(std::string_view input) {
    std::string escaped;
    escaped.reserve(input.size() + input.size() / kEscapeHeadroomDivisor);
    for (size_t index = 0; index < input.size();) {
        const char character = input[index];
        const unsigned char byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            escaped += '\\';
            escaped += character;
            ++index;
        } else if (byte < kFirstPrintableAscii) {
            escaped += "\\u00";
            escaped += kLowerHexDigits[static_cast<size_t>(byte) >> kBitsPerHexDigit];
            escaped += kLowerHexDigits[static_cast<size_t>(byte) & kLowHexDigitMask];
            ++index;
        } else {
            append_utf8_or_replacement(escaped, input, index);
        }
    }
    return escaped;
}

std::string prometheus_label_escape(std::string_view input) {
    std::string escaped;
    escaped.reserve(input.size() + input.size() / kEscapeHeadroomDivisor);

    for (size_t index = 0; index < input.size();) {
        const char character = input[index];
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= kContinuationMin) {
            append_utf8_or_replacement(escaped, input, index);
            continue;
        }
        if ((byte < kFirstPrintableAscii && character != '\n') || byte == kDelete) {
            escaped += kReplacementCharacter;
            ++index;
            continue;
        }
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        default: escaped += character;
        }
        ++index;
    }
    return escaped;
}

std::optional<std::string> url_decode(std::string_view input) {
    std::string decoded;
    decoded.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        const char character = input[index];
        if (character == '+') {
            decoded += ' ';
        } else if (character == '%') {
            if (index + kPercentEscapeLength > input.size())
                return std::nullopt;
            const int high_digit = hex_digit_value(input[index + 1]);
            const int low_digit = hex_digit_value(input[index + 2]);
            if (high_digit == kInvalidHexDigit || low_digit == kInvalidHexDigit)
                return std::nullopt;
            const int decoded_byte = high_digit * kHexRadix + low_digit;
            if (decoded_byte == 0)
                return std::nullopt;
            decoded += static_cast<char>(decoded_byte);
            index += kPercentEscapeLength - 1;
        } else if (character == '\0') {
            return std::nullopt;
        } else {
            decoded += character;
        }
    }

    ERIKSLUND_HTTP_ASSERT(decoded.size() <= input.size());
    return decoded;
}

std::string format_duration(std::chrono::seconds total) {
    const int64_t seconds_total = std::max<int64_t>(total.count(), 0);
    const int64_t days = seconds_total / kSecondsPerDay;
    const int64_t hours = seconds_total / kSecondsPerHour % kHoursPerDay;
    const int64_t minutes = seconds_total / kSecondsPerMinute % kMinutesPerHour;
    const int64_t seconds = seconds_total % kSecondsPerMinute;

    if (days > 0)
        return std::format("{}d {}h {}m {}s", days, hours, minutes, seconds);
    if (hours > 0)
        return std::format("{}h {}m {}s", hours, minutes, seconds);
    if (minutes > 0)
        return std::format("{}m {}s", minutes, seconds);
    return std::format("{}s", seconds);
}

std::string format_count(uint64_t value) {
    if (value <= kAbbreviateThreshold)
        return group_digits(value);

    double scaled = static_cast<double>(value) / kCountAbbreviationBase;
    size_t suffix_index = 0;
    while (scaled >= kCountAbbreviationBase && suffix_index + 1 < kCountSuffixes.size()) {
        scaled /= kCountAbbreviationBase;
        ++suffix_index;
    }

    return std::format("{:.{}f}{} ({})", scaled, kAbbreviatedFractionDigits,
                       kCountSuffixes[suffix_index], group_digits(value));
}

std::string format_bytes(uint64_t bytes) {
    if (bytes < kBytesPerKibibyte)
        return std::format("{} B", bytes);

    double scaled = static_cast<double>(bytes) / kKibibyteDivisor;
    size_t suffix_index = 0;
    while (scaled >= kKibibyteDivisor && suffix_index + 1 < kBinaryUnitSuffixes.size()) {
        scaled /= kKibibyteDivisor;
        ++suffix_index;
    }
    return std::format("{:.{}f} {}", scaled, kAbbreviatedFractionDigits,
                       kBinaryUnitSuffixes[suffix_index]);
}

std::string group_digits(uint64_t value) {
    std::array<char, kMaxUint64Digits> digits{};
    const std::to_chars_result conversion =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);

    ERIKSLUND_HTTP_ASSERT(conversion.ec == std::errc{});

    const size_t digit_count = static_cast<size_t>(conversion.ptr - digits.data());
    const size_t separator_count = (digit_count - 1) / kDigitGroupSize;

    const auto fill_from_the_right = [&digits, digit_count](char* buffer, size_t size) {
        size_t write_index = size;
        size_t digits_in_group = 0;
        for (size_t read_index = digit_count; read_index > 0; --read_index) {
            if (digits_in_group == kDigitGroupSize) {
                buffer[--write_index] = kDigitGroupSeparator;
                digits_in_group = 0;
            }
            buffer[--write_index] = digits[read_index - 1];
            ++digits_in_group;
        }
        return size;
    };

    std::string grouped;
    grouped.resize_and_overwrite(digit_count + separator_count, fill_from_the_right);
    return grouped;
}

std::string weak_etag(std::string_view body) {
    const EtagBuffer buffer = weak_etag_buffer(body);
    return std::string(buffer.view());
}

bool constant_time_equals(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;

    volatile unsigned char difference = 0;
    for (size_t index = 0; index < left.size(); ++index) {
        const unsigned char left_byte = static_cast<unsigned char>(left[index]);
        const unsigned char right_byte = static_cast<unsigned char>(right[index]);
        difference = static_cast<unsigned char>(difference | (left_byte ^ right_byte));
    }
    return difference == 0;
}

std::string http_date(std::chrono::system_clock::time_point when) {
    const std::chrono::sys_seconds truncated = std::chrono::floor<std::chrono::seconds>(when);

    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", truncated);
}

static_assert(fnv1a_64("") == kFnvOffsetBasis64);
static_assert(fnv1a_64("erikslund") != fnv1a_64("erikslund-http"));
static_assert(equals_ignore_case("GZIP", "gzip"));
static_assert(!equals_ignore_case("gzip", "gzip "));

} // namespace erikslund::http
