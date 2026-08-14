#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "erikslund/http/text.hpp"

namespace erikslund::http::test {

inline constexpr std::string_view kHeaderTerminator = "\r\n\r\n";
inline constexpr std::string_view kCrLf = "\r\n";
inline constexpr int kNoContentStatus = 204;
inline constexpr int kNotModifiedStatus = 304;

[[nodiscard]] inline std::string_view trim_ascii_spaces(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

struct HttpResponse {
    std::string version;
    int status_code = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string raw_head;
    bool complete = false;

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
        for (const auto& [field_name, field_value] : headers)
            if (equals_ignore_case(field_name, name))
                return std::string_view(field_value);
        return std::nullopt;
    }

    [[nodiscard]] bool has_header(std::string_view name) const {
        return header(name).has_value();
    }

    [[nodiscard]] std::string_view header_value(std::string_view name) const {
        return header(name).value_or(std::string_view{});
    }

    [[nodiscard]] size_t header_count(std::string_view name) const {
        size_t found = 0;
        for (const auto& [field_name, field_value] : headers)
            if (equals_ignore_case(field_name, name))
                ++found;
        return found;
    }
};

enum class BodyExpectation : uint8_t { FromHeaders, HeadRequest };

[[nodiscard]] inline bool parse_response_head(std::string_view head, HttpResponse& out) {
    const size_t line_end = head.find(kCrLf);
    if (line_end == std::string_view::npos)
        return false;

    const std::string_view status_line = head.substr(0, line_end);
    const size_t after_version = status_line.find(' ');
    if (after_version == std::string_view::npos)
        return false;
    out.version = std::string(status_line.substr(0, after_version));

    const std::string_view rest = status_line.substr(after_version + 1);
    const size_t after_code = rest.find(' ');
    const std::string_view code =
        after_code == std::string_view::npos ? rest : rest.substr(0, after_code);
    int value = 0;
    const std::from_chars_result parsed =
        std::from_chars(code.data(), code.data() + code.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size())
        return false;
    out.status_code = value;
    out.reason = after_code == std::string_view::npos ? std::string{}
                                                      : std::string(rest.substr(after_code + 1));

    std::string_view fields = head.substr(line_end + kCrLf.size());
    while (!fields.empty()) {
        const size_t field_end = fields.find(kCrLf);
        if (field_end == std::string_view::npos)
            return false;
        const std::string_view line = fields.substr(0, field_end);
        fields.remove_prefix(field_end + kCrLf.size());
        if (line.empty())
            break;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            return false;
        out.headers.emplace_back(std::string(line.substr(0, colon)),
                                 std::string(trim_ascii_spaces(line.substr(colon + 1))));
    }
    return true;
}

} // namespace erikslund::http::test
