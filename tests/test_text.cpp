#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "erikslund/http/text.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kOneQuote = R"raw(")raw";
constexpr std::string_view kOneBackslash = R"raw(\)raw";
constexpr std::string_view kBackslashThenQuote = R"raw(\")raw";
constexpr std::string_view kEscapedQuote = R"raw(\")raw";
constexpr std::string_view kDoubledBackslash = R"raw(\\)raw";
constexpr std::string_view kDoubledBackslashThenEscapedQuote = R"raw(\\\")raw";
constexpr std::string_view kEscapedNewline = R"raw(\n)raw";

constexpr std::string_view kQuoteBeforeBackslashMistake = R"raw(\\\\")raw";

constexpr std::string_view kWeakEtagPrefix = R"raw(W/")raw";
constexpr std::string_view kUnicodeEscapePrefix = R"raw(\u00)raw";

constexpr std::string_view kEmptyBodyEtag = R"raw(W/"cbf29ce484222325")raw";

constexpr unsigned int kFirstPrintableCodePoint = 0x20;
constexpr unsigned int kDeleteCodePoint = 0x7F;
constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

constexpr size_t kUnicodeEscapeLength = 6;

constexpr size_t kEtagHexOffset = 3;
constexpr size_t kEtagHexDigitCount = 16;

constexpr std::string_view kTextAroundANul{"a\0b", 3};
constexpr std::string_view kSecretAroundANul{"tok\0en", 6};
constexpr std::string_view kSameSecretAroundANul{"tok\0en", 6};
constexpr std::string_view kSecretDifferingAfterTheNul{"tok\0eN", 6};

constexpr uint64_t kFnvOfLetterA = 0xaf63'dc4c'8601'ec8cULL;
constexpr uint64_t kFnvOfFoobar = 0x8594'4171'f739'67e8ULL;

constexpr std::string_view kEmbeddedAssetBody = "<!doctype html><title>erikslund</title>";
constexpr EtagBuffer kEmbeddedAssetEtag = weak_etag_buffer(kEmbeddedAssetBody);
constexpr std::string_view kEmbeddedAssetEtagText = R"raw(W/"c564b207ebba2e95")raw";

constexpr uint64_t kOneMebibyte = kBytesPerKibibyte * kBytesPerKibibyte;
constexpr uint64_t kOneGibibyte = kOneMebibyte * kBytesPerKibibyte;

constexpr uint64_t kHeaderExampleCount = 88'531;
constexpr uint64_t kHeaderExampleByteCount = 1'440 * kBytesPerKibibyte;

constexpr uint64_t kWidestUint64 = std::numeric_limits<uint64_t>::max();
constexpr std::string_view kWidestUint64Grouped = "18,446,744,073,709,551,615";

constexpr size_t kImfFixdateLength = 29;

constexpr std::chrono::seconds kFixedInstant{1'700'000'000};
constexpr std::string_view kFixedInstantAsHttpDate = "Tue, 14 Nov 2023 22:13:20 GMT";

constexpr std::chrono::seconds kSingleDigitDayInstant{1'709'596'800};
constexpr std::string_view kSingleDigitDayAsHttpDate = "Tue, 05 Mar 2024 00:00:00 GMT";

[[nodiscard]] bool every_quote_is_escaped(std::string_view escaped) {
    for (size_t index = 0; index < escaped.size(); ++index) {
        if (escaped[index] != '"')
            continue;
        size_t preceding_backslashes = 0;
        while (preceding_backslashes < index &&
               escaped[index - 1 - preceding_backslashes] == '\\')
            ++preceding_backslashes;
        if (preceding_backslashes % 2 == 0)
            return false;
    }
    return true;
}

[[nodiscard]] std::string unicode_escape_of(std::string_view two_hex_digits) {
    return std::string(kUnicodeEscapePrefix) + std::string(two_hex_digits);
}

[[nodiscard]] bool is_lowercase_hex(std::string_view text) {
    for (const char character : text)
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            return false;
    return true;
}

} // namespace

TEST_SUITE("text") {

TEST_CASE("html_escape_escapes_the_ampersand_first_so_an_entity_is_never_escaped_twice") {
    CHECK(html_escape("&lt;") == "&amp;lt;");
    CHECK(html_escape("&lt;") != "&amp;amp;lt;");
    CHECK(html_escape("&") == "&amp;");
    CHECK(html_escape("a & b") == "a &amp; b");
}

TEST_CASE("html_escape_escapes_both_quote_forms_so_an_attribute_cannot_be_closed") {
    CHECK(html_escape(kOneQuote) == "&quot;");
    CHECK(html_escape("'") == "&#39;");
    CHECK(html_escape("<a href='x' title=\"y\">") ==
          "&lt;a href=&#39;x&#39; title=&quot;y&quot;&gt;");
}

TEST_CASE("html_escape_leaves_text_without_metacharacters_byte_identical") {
    CHECK(html_escape("") == "");
    CHECK(html_escape("plain operator text 123") == "plain operator text 123");
    CHECK(html_escape("\xE2\x9C\x93 fj\xC3\xA4ll") == "\xE2\x9C\x93 fj\xC3\xA4ll");
}

TEST_CASE("html_escape_into_appends_to_the_buffer_it_is_given") {
    std::string page = "<td>";
    html_escape_into(page, "a<b");
    CHECK(page == "<td>a&lt;b");
    html_escape_into(page, "&c");
    CHECK(page == "<td>a&lt;b&amp;c");
}

TEST_CASE("json_escape_escapes_the_quote_and_the_backslash") {
    CHECK(json_escape(kOneQuote) == kEscapedQuote);
    CHECK(json_escape(kOneBackslash) == kDoubledBackslash);
    CHECK(json_escape(kBackslashThenQuote) == kDoubledBackslashThenEscapedQuote);
    CHECK(json_escape("") == "");
    CHECK(json_escape("plain") == "plain");
}

TEST_CASE("json_escape_renders_a_control_character_as_a_lowercase_unicode_escape") {
    CHECK(json_escape("\n") == unicode_escape_of("0a"));
    CHECK(json_escape("\t") == unicode_escape_of("09"));
    CHECK(json_escape("\r") == unicode_escape_of("0d"));
    CHECK(json_escape("\x1f") == unicode_escape_of("1f"));
    CHECK(json_escape(std::string_view("\0", 1)) == unicode_escape_of("00"));
    CHECK(json_escape(" ") == " ");
}

TEST_CASE("json_escape_leaves_printable_ascii_and_utf8_alone") {
    CHECK(json_escape("a/b") == "a/b");
    CHECK(json_escape("\xE2\x9C\x93") == "\xE2\x9C\x93");
    CHECK(json_escape("\x7f") == "\x7f");
}

TEST_CASE("json_escape_replaces every malformed utf8 byte sequence") {
    constexpr std::string_view replacement = "\xEF\xBF\xBD";
    CHECK(json_escape("\xFF") == replacement);
    CHECK(json_escape("\xC0\xAF") == std::string(replacement) + std::string(replacement));
    CHECK(json_escape("\xE2(") == std::string(replacement) + "(");
    CHECK(json_escape("\xED\xA0\x80") ==
          std::string(replacement) + std::string(replacement) + std::string(replacement));
    CHECK(json_escape("\xF4\x90\x80\x80") ==
          std::string(replacement) + std::string(replacement) + std::string(replacement) +
              std::string(replacement));
}

TEST_CASE("prometheus_label_escape_escapes_the_backslash_before_the_quote") {
    CHECK(prometheus_label_escape(kOneBackslash) == kDoubledBackslash);
    CHECK(prometheus_label_escape(kOneQuote) == kEscapedQuote);
    CHECK(prometheus_label_escape("\n") == kEscapedNewline);
    CHECK(prometheus_label_escape(kBackslashThenQuote) == kDoubledBackslashThenEscapedQuote);
    CHECK(prometheus_label_escape(kBackslashThenQuote) != kQuoteBeforeBackslashMistake);
}

TEST_CASE("prometheus_label_escape_leaves_an_ordinary_label_value_untouched") {
    CHECK(prometheus_label_escape("") == "");
    CHECK(prometheus_label_escape("GET") == "GET");
    CHECK(prometheus_label_escape("/api/v1/status") == "/api/v1/status");
}

TEST_CASE("prometheus_label_escape_replaces malformed utf8 without changing valid text") {
    CHECK(prometheus_label_escape("ok \xE2\x9C\x93") == "ok \xE2\x9C\x93");
    CHECK(prometheus_label_escape("bad\xFFlabel") ==
          std::string("bad") + std::string(kReplacementCharacter) + "label");
}

TEST_CASE("prometheus_label_escape_replaces_ascii_controls and delete") {
    for (unsigned int code_point = 0; code_point < kFirstPrintableCodePoint; ++code_point) {
        const char raw_byte = static_cast<char>(code_point);
        const std::string escaped = prometheus_label_escape(std::string_view(&raw_byte, 1));
        CAPTURE(code_point);
        CHECK(escaped == (raw_byte == '\n' ? kEscapedNewline : kReplacementCharacter));
    }

    const char delete_byte = static_cast<char>(kDeleteCodePoint);
    CHECK(prometheus_label_escape(std::string_view(&delete_byte, 1)) == kReplacementCharacter);
}

TEST_CASE("url_decode_decodes_percent_escapes_and_reads_plus_as_a_space") {
    const std::optional<std::string> spaced = url_decode("a%20b");
    REQUIRE(spaced.has_value());
    CHECK(spaced.value() == "a b");

    const std::optional<std::string> plussed = url_decode("a+b");
    REQUIRE(plussed.has_value());
    CHECK(plussed.value() == "a b");

    const std::optional<std::string> mixed = url_decode("name=erik+andersson&city=%C3%85re");
    REQUIRE(mixed.has_value());
    CHECK(mixed.value() == "name=erik andersson&city=\xC3\x85re");
}

TEST_CASE("url_decode_accepts_either_case_of_hex_digit_and_leaves_unescaped_text_alone") {
    const std::optional<std::string> upper = url_decode("%4A");
    const std::optional<std::string> lower = url_decode("%4a");
    REQUIRE(upper.has_value());
    REQUIRE(lower.has_value());
    CHECK(upper.value() == "J");
    CHECK(lower.value() == upper.value());

    const std::optional<std::string> untouched = url_decode("/status/detail");
    REQUIRE(untouched.has_value());
    CHECK(untouched.value() == "/status/detail");

    const std::optional<std::string> empty = url_decode("");
    REQUIRE(empty.has_value());
    CHECK(empty.value() == "");

    const std::optional<std::string> literal_plus = url_decode("%2B");
    REQUIRE(literal_plus.has_value());
    CHECK(literal_plus.value() == "+");
}

TEST_CASE("format_duration_drops_the_leading_units_that_are_zero") {
    using std::chrono::days;
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;

    CHECK(format_duration(seconds{0}) == "0s");
    CHECK(format_duration(seconds{1}) == "1s");
    CHECK(format_duration(minutes{1} - seconds{1}) == "59s");
    CHECK(format_duration(minutes{1}) == "1m 0s");
    CHECK(format_duration(hours{1} - seconds{1}) == "59m 59s");
    CHECK(format_duration(hours{1}) == "1h 0m 0s");
    CHECK(format_duration(days{1} - seconds{1}) == "23h 59m 59s");
    CHECK(format_duration(days{1}) == "1d 0h 0m 0s");
    CHECK(format_duration(days{1} + hours{1} + minutes{1} + seconds{1}) == "1d 1h 1m 1s");
}

TEST_CASE("format_count_leaves_a_value_at_or_below_the_threshold_unabbreviated") {
    CHECK(format_count(0) == "0");
    CHECK(format_count(7) == "7");
    CHECK(format_count(999) == "999");
    CHECK(format_count(1'000) == "1,000");
    CHECK(format_count(kAbbreviateThreshold - 1) == "9,999");
    CHECK(format_count(kAbbreviateThreshold) == "10,000");
}

TEST_CASE("format_count_abbreviates_above_the_threshold_and_still_carries_the_exact_figure") {
    CHECK(format_count(kAbbreviateThreshold + 1) == "10.0K (10,001)");
    CHECK(format_count(kHeaderExampleCount) == "88.5K (88,531)");
    CHECK(format_count(1'000'000) == "1.0M (1,000,000)");
    CHECK(format_count(1'234'567) == "1.2M (1,234,567)");
    CHECK(format_count(999'999).ends_with("(999,999)"));
}

TEST_CASE("format_bytes_switches_from_bytes_to_kibibytes_at_a_kibibyte") {
    CHECK(format_bytes(0) == "0 B");
    CHECK(format_bytes(1) == "1 B");
    CHECK(format_bytes(kBytesPerKibibyte - 1) == "1023 B");
    CHECK(format_bytes(kBytesPerKibibyte) == "1.0 KiB");
    CHECK(format_bytes(kBytesPerKibibyte + kBytesPerKibibyte / 2) == "1.5 KiB");
}

TEST_CASE("format_bytes_scales_through_the_binary_units_and_never_overstates_the_unit") {
    CHECK(format_bytes(kOneMebibyte) == "1.0 MiB");
    CHECK(format_bytes(kHeaderExampleByteCount) == "1.4 MiB");
    CHECK(format_bytes(kOneMebibyte + kOneMebibyte / 2) == "1.5 MiB");
    CHECK(format_bytes(kOneGibibyte) == "1.0 GiB");
    CHECK(format_bytes(kOneMebibyte - 1).ends_with(" KiB"));
}

TEST_CASE("group_digits_separates_every_third_digit_counting_from_the_right") {
    CHECK(group_digits(0) == "0");
    CHECK(group_digits(7) == "7");
    CHECK(group_digits(999) == "999");
    CHECK(group_digits(1'000) == "1,000");
    CHECK(group_digits(10'000) == "10,000");
    CHECK(group_digits(kHeaderExampleCount) == "88,531");
    CHECK(group_digits(100'000) == "100,000");
    CHECK(group_digits(1'234'567) == "1,234,567");
}

TEST_CASE("weak_etag_is_stable_for_the_same_body_and_wears_the_weak_validator_shape") {
    const std::string etag = weak_etag("hello");
    CHECK(etag.size() == kWeakEtagLength);
    CHECK(etag.starts_with(kWeakEtagPrefix));
    CHECK(etag.ends_with(kOneQuote));
    CHECK(is_lowercase_hex(std::string_view(etag).substr(kEtagHexOffset, kEtagHexDigitCount)));

    CHECK(weak_etag("hello") == etag);
    CHECK(weak_etag("hellp") != etag);
    CHECK(weak_etag("") == kEmptyBodyEtag);
}

TEST_CASE("weak_etag_agrees_with_the_validator_the_compiler_computed_for_the_same_bytes") {
    static_assert(kEmbeddedAssetEtag.view().size() == kWeakEtagLength);
    static_assert(kEmbeddedAssetEtag.view() == kEmbeddedAssetEtagText);

    const std::string compile_time_etag(kEmbeddedAssetEtag.view());
    CHECK(weak_etag(kEmbeddedAssetBody) == compile_time_etag);
}

TEST_CASE("fnv1a_64_evaluates_at_compile_time_and_reproduces_the_published_vectors") {
    constexpr uint64_t kHashOfEmpty = fnv1a_64("");
    constexpr uint64_t kHashOfLetterA = fnv1a_64("a");
    constexpr uint64_t kHashOfFoobar = fnv1a_64("foobar");
    static_assert(kHashOfEmpty == kFnvOffsetBasis64);
    static_assert(kHashOfLetterA == kFnvOfLetterA);
    static_assert(kHashOfFoobar == kFnvOfFoobar);
    static_assert(fnv1a_64("erikslund") != fnv1a_64("erikslund-http"));

    CHECK(fnv1a_64("a") == kFnvOfLetterA);
    CHECK(fnv1a_64("foobar") == kFnvOfFoobar);
}

TEST_CASE("equals_ignore_case_evaluates_at_compile_time_and_folds_only_ascii_letters") {
    constexpr bool kHeaderNameMatches = equals_ignore_case("Content-Length", "content-length");
    static_assert(kHeaderNameMatches);
    static_assert(equals_ignore_case("", ""));
    static_assert(equals_ignore_case("GZIP", "gzip"));
    static_assert(!equals_ignore_case("gzip", "gzip "));
    static_assert(!equals_ignore_case("gzip", "deflate"));
    static_assert(!equals_ignore_case("[", "{"));
    static_assert(!equals_ignore_case("@", "`"));
    static_assert(!equals_ignore_case("_", "?"));

    CHECK(equals_ignore_case("Transfer-Encoding", "TRANSFER-ENCODING"));
    CHECK_FALSE(equals_ignore_case("Content-Length", "Content-Length "));
}

TEST_CASE("constant_time_equals_reports_equality_only_for_the_same_bytes") {
    CHECK(constant_time_equals("", ""));
    CHECK(constant_time_equals("s3cret-bearer-token", "s3cret-bearer-token"));
    CHECK_FALSE(constant_time_equals("s3cret-bearer-token", "s3cret-bearer-tokeN"));
    CHECK_FALSE(constant_time_equals("Bearer", "bearer"));
}

TEST_CASE("constant_time_equals_reports_inequality_for_inputs_of_different_length") {
    CHECK_FALSE(constant_time_equals("token", "token-extra"));
    CHECK_FALSE(constant_time_equals("token-extra", "token"));
    CHECK_FALSE(constant_time_equals("", "x"));
    CHECK_FALSE(constant_time_equals("x", ""));
}

TEST_CASE("http_date_renders_a_fixed_instant_as_an_imf_fixdate_in_gmt") {
    const std::chrono::system_clock::time_point when(kFixedInstant);
    CHECK(http_date(when) == kFixedInstantAsHttpDate);
    CHECK(http_date(when).size() == kImfFixdateLength);
    CHECK(http_date(when).ends_with(" GMT"));
}

TEST_CASE("http_date_zero_pads_a_single_digit_day") {
    const std::chrono::system_clock::time_point when(kSingleDigitDayInstant);
    CHECK(http_date(when) == kSingleDigitDayAsHttpDate);
    CHECK(http_date(when).size() == kImfFixdateLength);
}

TEST_CASE("http_date_renders_the_epoch_itself") {
    const std::chrono::system_clock::time_point epoch;
    CHECK(http_date(epoch) == "Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST_CASE("http_date_truncates_a_sub_second_instant_rather_than_rounding_it_up") {
    constexpr std::chrono::milliseconds kJustUnderTwoSeconds{1'999};
    const std::chrono::system_clock::time_point when(kJustUnderTwoSeconds);
    CHECK(http_date(when) == "Thu, 01 Jan 1970 00:00:01 GMT");
}

} // TEST_SUITE("text")

TEST_SUITE("text_adversarial") {

TEST_CASE("html_escape_neutralises_every_metacharacter_in_a_script_payload") {
    constexpr std::string_view kPayload = R"raw(<script>alert('xss' & "1")</script>)raw";
    constexpr std::string_view kNeutralised =
        "&lt;script&gt;alert(&#39;xss&#39; &amp; &quot;1&quot;)&lt;/script&gt;";
    const std::string escaped = html_escape(kPayload);
    CHECK(escaped == kNeutralised);
    CHECK(escaped.find('<') == std::string::npos);
    CHECK(escaped.find('>') == std::string::npos);
    CHECK(escaped.find('"') == std::string::npos);
    CHECK(escaped.find('\'') == std::string::npos);
}

TEST_CASE("html_escape_escapes_an_already_escaped_entity_a_second_time") {
    CHECK(html_escape("&amp;") == "&amp;amp;");
    CHECK(html_escape("&#39;") == "&amp;#39;");
}

TEST_CASE("html_escape_does_not_stop_at_an_embedded_nul") {
    const std::string escaped = html_escape(kTextAroundANul);
    CHECK(escaped.size() == kTextAroundANul.size());
    CHECK(escaped == std::string(kTextAroundANul));
}

TEST_CASE("json_escape_escapes_the_whole_c0_control_range_and_leaks_no_raw_byte") {
    for (unsigned int code_point = 0; code_point < kFirstPrintableCodePoint; ++code_point) {
        const char raw_byte = static_cast<char>(code_point);
        const std::string escaped = json_escape(std::string_view(&raw_byte, 1));
        CAPTURE(code_point);
        REQUIRE(escaped.size() == kUnicodeEscapeLength);
        CHECK(escaped.starts_with(kUnicodeEscapePrefix));
        CHECK(escaped.find(raw_byte) == std::string::npos);
        CHECK(is_lowercase_hex(std::string_view(escaped).substr(kUnicodeEscapePrefix.size())));
    }
}

TEST_CASE("json_escape_cannot_be_closed_early_by_a_quote_or_a_trailing_backslash") {
    constexpr std::string_view kPayload = R"raw(he said "hi"\)raw";
    constexpr std::string_view kEscaped = R"raw(he said \"hi\"\\)raw";
    const std::string escaped = json_escape(kPayload);
    CHECK(escaped == kEscaped);
    CHECK(every_quote_is_escaped(escaped));
}

TEST_CASE("prometheus_label_escape_cannot_break_out_of_the_label_value") {
    constexpr std::string_view kPayload = "job\" attack=\"1\nevil_metric 1";
    const std::string escaped = prometheus_label_escape(kPayload);
    CHECK(escaped.find('\n') == std::string::npos);
    CHECK(every_quote_is_escaped(escaped));
    CHECK(escaped.find(kEscapedNewline) != std::string::npos);
}

TEST_CASE("url_decode_rejects_an_escape_truncated_by_the_end_of_the_input") {
    CHECK_FALSE(url_decode("%").has_value());
    CHECK_FALSE(url_decode("%A").has_value());
    CHECK_FALSE(url_decode("/status?token=%4").has_value());
    CHECK_FALSE(url_decode("100%").has_value());
}

TEST_CASE("url_decode_rejects_an_escape_whose_digits_are_not_hex") {
    CHECK_FALSE(url_decode("%ZZ").has_value());
    CHECK_FALSE(url_decode("%2Z").has_value());
    CHECK_FALSE(url_decode("%Z2").has_value());
    CHECK_FALSE(url_decode("%%41").has_value());
    CHECK_FALSE(url_decode("%-1").has_value());
    CHECK_FALSE(url_decode("% 20").has_value());
}

TEST_CASE("url_decode_rejects_a_nul_however_it_arrives") {
    CHECK_FALSE(url_decode("%00").has_value());
    CHECK_FALSE(url_decode("/status%00.txt").has_value());
    CHECK_FALSE(url_decode("%00%41").has_value());
    CHECK_FALSE(url_decode(kTextAroundANul).has_value());
}

TEST_CASE("url_decode_decodes_exactly_once_so_a_double_encoded_nul_stays_text") {
    const std::optional<std::string> once = url_decode("%2500");
    REQUIRE(once.has_value());
    CHECK(once.value() == "%00");
    CHECK(once.value().find('\0') == std::string::npos);
}

TEST_CASE("url_decode_hands_traversal_and_high_bytes_to_the_caller_unchanged") {
    const std::optional<std::string> traversal = url_decode("%2e%2e%2fetc%2fpasswd");
    REQUIRE(traversal.has_value());
    CHECK(traversal.value() == "../etc/passwd");

    constexpr unsigned char kHighByte = 0xFF;
    const std::optional<std::string> high = url_decode("%FF");
    REQUIRE(high.has_value());
    REQUIRE(high.value().size() == 1);
    CHECK(static_cast<unsigned char>(high.value().front()) == kHighByte);

    const std::optional<std::string> control = url_decode("%01");
    REQUIRE(control.has_value());
    CHECK(control.value() == "\x01");
}

TEST_CASE("format_duration_clamps_a_backwards_clock_to_zero_and_survives_the_widest_duration") {
    using std::chrono::seconds;
    CHECK(format_duration(seconds{-1}) == "0s");
    CHECK(format_duration(seconds{-90'061}) == "0s");
    CHECK(format_duration(seconds::min()) == "0s");
    CHECK(format_duration(seconds::max()) == "106751991167300d 15h 30m 7s");
}

TEST_CASE("the_humanising_helpers_survive_the_widest_uint64") {
    CHECK(group_digits(kWidestUint64) == kWidestUint64Grouped);
    CHECK(format_count(kWidestUint64) == "18.4E (18,446,744,073,709,551,615)");
    CHECK(format_bytes(kWidestUint64) == "16.0 EiB");
}

TEST_CASE("weak_etag_is_defined_for_an_empty_body_and_for_a_body_full_of_nuls") {
    CHECK(weak_etag("") == kEmptyBodyEtag);
    CHECK(weak_etag("").size() == kWeakEtagLength);

    constexpr size_t kNulBodyLength = 64;
    const std::string nul_body(kNulBodyLength, '\0');
    const std::string nul_etag = weak_etag(nul_body);
    CHECK(nul_etag.size() == kWeakEtagLength);
    CHECK(nul_etag != weak_etag(""));
}

TEST_CASE("constant_time_equals_compares_the_bytes_after_a_nul_as_well") {
    CHECK(constant_time_equals(kSecretAroundANul, kSameSecretAroundANul));
    CHECK_FALSE(constant_time_equals(kSecretAroundANul, kSecretDifferingAfterTheNul));
    CHECK_FALSE(constant_time_equals(kSecretAroundANul, "tok"));
}

TEST_CASE("http_date_renders_an_instant_before_the_epoch_rather_than_wrapping_it") {
    const std::chrono::system_clock::time_point before_epoch(std::chrono::seconds{-1});
    CHECK(http_date(before_epoch) == "Wed, 31 Dec 1969 23:59:59 GMT");
    CHECK(http_date(before_epoch).size() == kImfFixdateLength);
}

} // TEST_SUITE("text_adversarial")
