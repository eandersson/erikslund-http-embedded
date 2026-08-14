#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace erikslund::http {

enum class Method : uint8_t {
    None = 0,
    Get = 1 << 0,
    Head = 1 << 1,
    Post = 1 << 2,
    Put = 1 << 3,
    Patch = 1 << 4,
    Delete = 1 << 5,
    Options = 1 << 6,
};

[[nodiscard]] constexpr Method operator|(Method left, Method right) noexcept {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<Method>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

[[nodiscard]] constexpr Method operator&(Method left, Method right) noexcept {
    return static_cast<Method>(static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
}

constexpr Method& operator|=(Method& left, Method right) noexcept {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool contains(Method methods, Method required) noexcept {
    return (static_cast<uint8_t>(methods) & static_cast<uint8_t>(required)) ==
           static_cast<uint8_t>(required);
}

inline constexpr std::array<Method, 7> kAllMethods{Method::Get,    Method::Head,   Method::Post,
                                                   Method::Put,    Method::Patch,  Method::Delete,
                                                   Method::Options};

// Returns an empty view for None and multi-method masks.
[[nodiscard]] constexpr std::string_view method_name(Method method) noexcept {
    switch (method) {
    case Method::Get:
        return "GET";
    case Method::Head:
        return "HEAD";
    case Method::Post:
        return "POST";
    case Method::Put:
        return "PUT";
    case Method::Patch:
        return "PATCH";
    case Method::Delete:
        return "DELETE";
    case Method::Options:
        return "OPTIONS";
    case Method::None:
        return {};
    }
    return {};
}

// Method tokens are case-sensitive.
[[nodiscard]] constexpr std::optional<Method> method_from_token(std::string_view token) noexcept {
    for (const Method candidate : kAllMethods)
        if (method_name(candidate) == token)
            return candidate;
    return std::nullopt;
}

static_assert(method_from_token("GET") == Method::Get);
static_assert(!method_from_token("get").has_value());
static_assert(contains(Method::Get | Method::Head, Method::Head));

} // namespace erikslund::http
