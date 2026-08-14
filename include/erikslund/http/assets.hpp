#pragma once

#include <span>
#include <string_view>

#include "erikslund/http/text.hpp"

namespace erikslund::http {

// All fields borrow static storage.
struct Asset {
    std::string_view path{};
    std::string_view content_type{};
    std::string_view bytes{};
    // Includes the W/ prefix and quotes.
    std::string_view etag{};
    // Empty means identity encoding.
    std::string_view content_encoding{};
};

// Unknown extensions use application/octet-stream.
[[nodiscard]] constexpr std::string_view content_type_for_extension(std::string_view path) noexcept {
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return "application/octet-stream";
    const std::string_view extension = path.substr(dot);
    if (extension == ".html" || extension == ".htm")
        return "text/html; charset=utf-8";
    if (extension == ".css")
        return "text/css; charset=utf-8";
    if (extension == ".js" || extension == ".mjs")
        return "text/javascript; charset=utf-8";
    if (extension == ".json")
        return "application/json";
    if (extension == ".svg")
        return "image/svg+xml";
    if (extension == ".png")
        return "image/png";
    if (extension == ".ico")
        return "image/vnd.microsoft.icon";
    if (extension == ".woff2")
        return "font/woff2";
    if (extension == ".txt")
        return "text/plain; charset=utf-8";
    if (extension == ".map")
        return "application/json";
    return "application/octet-stream";
}

// The EtagBuffer must outlive the returned Asset.
[[nodiscard]] constexpr Asset make_asset(std::string_view path, std::string_view content_type,
                                         std::string_view bytes, const EtagBuffer& etag,
                                         std::string_view content_encoding = {}) noexcept {
    return Asset{path, content_type, bytes, etag.view(), content_encoding};
}

[[nodiscard]] constexpr Asset make_asset(std::string_view path, std::string_view bytes,
                                         const EtagBuffer& etag,
                                         std::string_view content_encoding = {}) noexcept {
    return Asset{path, content_type_for_extension(path), bytes, etag.view(), content_encoding};
}

// Non-owning view over static assets.
class AssetBundle {
public:
    AssetBundle() = default;
    explicit AssetBundle(std::span<const Asset> assets) noexcept : assets_(assets) {}

    [[nodiscard]] const Asset* find(std::string_view path) const noexcept;

    [[nodiscard]] std::span<const Asset> all() const noexcept { return assets_; }
    [[nodiscard]] bool empty() const noexcept { return assets_.empty(); }

private:
    std::span<const Asset> assets_;
};

} // namespace erikslund::http
