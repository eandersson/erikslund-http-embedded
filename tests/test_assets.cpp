
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "erikslund/http/assets.hpp"
#include "erikslund/http/text.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kStylesheetBytes = "body{color:#222}";
constexpr EtagBuffer kStylesheetEtag = weak_etag_buffer(kStylesheetBytes);
constexpr Asset kStylesheet = make_asset("/assets/app.css", kStylesheetBytes, kStylesheetEtag);

static_assert(kStylesheet.etag == weak_etag_buffer(fnv1a_64("body{color:#222}")).view());
static_assert(kStylesheet.etag.size() == kWeakEtagLength);
static_assert(kStylesheet.etag.starts_with(R"(W/")"));
static_assert(kStylesheet.etag.ends_with(R"(")"));
static_assert(kStylesheet.path == "/assets/app.css");
static_assert(kStylesheet.bytes == kStylesheetBytes);
static_assert(kStylesheet.content_type == "text/css; charset=utf-8");
static_assert(kStylesheet.content_encoding.empty());

static_assert(weak_etag_buffer(std::string_view("body{color:#222}")).view() !=
              weak_etag_buffer(std::string_view("body{color:#223}")).view());

constexpr std::string_view kCompressedBytes = "pre-compressed at build time";
constexpr EtagBuffer kCompressedEtag = weak_etag_buffer(kCompressedBytes);
constexpr Asset kCompressed =
    make_asset("/assets/app.js", "text/javascript; charset=utf-8", kCompressedBytes,
               kCompressedEtag, "gzip");
static_assert(kCompressed.content_encoding == "gzip");
static_assert(kCompressed.content_type == "text/javascript; charset=utf-8");

constexpr size_t kEtagPrefixLength = 3;
constexpr size_t kEtagHexDigitCount = 16;
constexpr std::string_view kHexDigits = "0123456789abcdef";
static_assert(kEtagPrefixLength + kEtagHexDigitCount + 1 == kWeakEtagLength);

constexpr size_t kBinaryProbeThresholdCount = 16;
constexpr size_t kSmallBundleCount = 4;
constexpr size_t kLargeBundleCount = 24;
static_assert(kSmallBundleCount < kBinaryProbeThresholdCount);
static_assert(kLargeBundleCount > kBinaryProbeThresholdCount);

constexpr std::array<std::string_view, kLargeBundleCount> kBundlePaths{
    "/assets/a00.css", "/assets/a01.js", "/assets/a02.json",
    "/assets/a03.svg", "/assets/a04.png", "/assets/a05.ico",
    "/assets/a06.woff2", "/assets/a07.txt", "/assets/a08.map",
    "/assets/a09.html", "/assets/a10.htm", "/assets/a11.mjs",
    "/assets/a12.css", "/assets/a13.js", "/assets/a14.json",
    "/assets/a15.svg", "/assets/a16.png", "/assets/a17.ico",
    "/assets/a18.woff2", "/assets/a19.txt", "/assets/a20.map",
    "/assets/a21.html", "/assets/a22.htm", "/assets/a23.mjs"};

template <size_t Count>
[[nodiscard]] constexpr std::array<EtagBuffer, Count> etag_buffers_for(
    const std::array<std::string_view, Count>& paths) noexcept {
    std::array<EtagBuffer, Count> buffers{};
    for (size_t index = 0; index < Count; ++index)
        buffers[index] = weak_etag_buffer(paths[index]);
    return buffers;
}

template <size_t Count>
[[nodiscard]] constexpr std::array<Asset, Count> assets_for(
    const std::array<std::string_view, Count>& paths,
    const std::array<EtagBuffer, Count>& etags) noexcept {
    std::array<Asset, Count> assets{};
    for (size_t index = 0; index < Count; ++index)
        assets[index] = make_asset(paths[index], paths[index], etags[index]);
    return assets;
}

template <size_t Count>
[[nodiscard]] constexpr std::array<Asset, Count> reversed(
    const std::array<Asset, Count>& assets) noexcept {
    std::array<Asset, Count> flipped{};
    for (size_t index = 0; index < Count; ++index)
        flipped[index] = assets[Count - 1 - index];
    return flipped;
}

constexpr std::array<EtagBuffer, kLargeBundleCount> kBundleEtags = etag_buffers_for(kBundlePaths);
constexpr std::array<Asset, kLargeBundleCount> kAscendingAssets =
    assets_for(kBundlePaths, kBundleEtags);
constexpr std::array<Asset, kLargeBundleCount> kDescendingAssets = reversed(kAscendingAssets);

static_assert(kAscendingAssets.front().path == "/assets/a00.css");
static_assert(kDescendingAssets.front().path == "/assets/a23.mjs");

[[nodiscard]] AssetBundle small_bundle() {
    return AssetBundle(std::span<const Asset>(kAscendingAssets.data(), kSmallBundleCount));
}

[[nodiscard]] AssetBundle sorted_bundle() {
    return AssetBundle(std::span<const Asset>(kAscendingAssets));
}

[[nodiscard]] AssetBundle unsorted_bundle() {
    return AssetBundle(std::span<const Asset>(kDescendingAssets));
}

} // namespace

TEST_CASE("the extension table names a type for every extension an operator surface serves") {
    static_assert(content_type_for_extension("/index.html") == "text/html; charset=utf-8");
    static_assert(content_type_for_extension("/index.htm") == "text/html; charset=utf-8");
    static_assert(content_type_for_extension("/app.css") == "text/css; charset=utf-8");
    static_assert(content_type_for_extension("/app.js") == "text/javascript; charset=utf-8");
    static_assert(content_type_for_extension("/app.mjs") == "text/javascript; charset=utf-8");
    static_assert(content_type_for_extension("/data.json") == "application/json");
    static_assert(content_type_for_extension("/icon.svg") == "image/svg+xml");
    static_assert(content_type_for_extension("/icon.png") == "image/png");
    static_assert(content_type_for_extension("/favicon.ico") == "image/vnd.microsoft.icon");
    static_assert(content_type_for_extension("/inter.woff2") == "font/woff2");
    static_assert(content_type_for_extension("/robots.txt") == "text/plain; charset=utf-8");
    static_assert(content_type_for_extension("/app.js.map") == "application/json");

    CHECK(content_type_for_extension("/index.html") == "text/html; charset=utf-8");
    CHECK(content_type_for_extension("/index.htm") == "text/html; charset=utf-8");
    CHECK(content_type_for_extension("/app.css") == "text/css; charset=utf-8");
    CHECK(content_type_for_extension("/app.js") == "text/javascript; charset=utf-8");
    CHECK(content_type_for_extension("/app.mjs") == "text/javascript; charset=utf-8");
    CHECK(content_type_for_extension("/data.json") == "application/json");
    CHECK(content_type_for_extension("/icon.svg") == "image/svg+xml");
    CHECK(content_type_for_extension("/icon.png") == "image/png");
    CHECK(content_type_for_extension("/favicon.ico") == "image/vnd.microsoft.icon");
    CHECK(content_type_for_extension("/inter.woff2") == "font/woff2");
    CHECK(content_type_for_extension("/robots.txt") == "text/plain; charset=utf-8");
    CHECK(content_type_for_extension("/app.js.map") == "application/json");
}

TEST_CASE("an unknown extension is served as a binary blob and never as html") {
    constexpr std::array<std::string_view, 8> kUnknownPaths{
        "/upload.unknown", "/archive.tar.gz", "/report.pdf",     "/notes.md",
        "/page.php",       "/page.xhtml",     "/config.yaml",    "/data.xml"};

    for (const std::string_view path : kUnknownPaths) {
        CHECK(content_type_for_extension(path) == "application/octet-stream");
        CHECK_MESSAGE(content_type_for_extension(path) != "text/html; charset=utf-8",
                      "guessing html for unknown bytes is an XSS vector");
    }

    static_assert(content_type_for_extension("/upload.unknown") == "application/octet-stream");
    static_assert(content_type_for_extension("/page.php") != "text/html; charset=utf-8");
}

TEST_CASE("a path with no extension at all is served as a binary blob") {
    static_assert(content_type_for_extension("/no-extension") == "application/octet-stream");
    static_assert(content_type_for_extension("/") == "application/octet-stream");
    static_assert(content_type_for_extension("") == "application/octet-stream");

    CHECK(content_type_for_extension("/no-extension") == "application/octet-stream");
    CHECK(content_type_for_extension("/") == "application/octet-stream");
    CHECK(content_type_for_extension("") == "application/octet-stream");
    CHECK(content_type_for_extension("/trailing.") == "application/octet-stream");
    CHECK(content_type_for_extension("/assets/.hidden") == "application/octet-stream");
}

TEST_CASE("only the final dot of a path selects the type") {
    CHECK(content_type_for_extension("/v1.2/readme") == "application/octet-stream");
    CHECK(content_type_for_extension("/v1.2/page.html") == "text/html; charset=utf-8");
    CHECK(content_type_for_extension("/bundle.min.js") == "text/javascript; charset=utf-8");
    CHECK(content_type_for_extension("/payload.html.txt") == "text/plain; charset=utf-8");

    CHECK(content_type_for_extension("/payload.html.unknown") == "application/octet-stream");
}

TEST_CASE("the extension table is matched case sensitively and fails closed on the mismatch") {
    CHECK(content_type_for_extension("/INDEX.HTML") == "application/octet-stream");
    CHECK(content_type_for_extension("/APP.CSS") == "application/octet-stream");
    CHECK(content_type_for_extension("/INDEX.HTML") != "text/html; charset=utf-8");
}

TEST_CASE("make_asset derives the content type from the path and keeps an explicit one") {
    CHECK(kStylesheet.content_type == "text/css; charset=utf-8");
    CHECK(kCompressed.content_type == "text/javascript; charset=utf-8");
    CHECK(kCompressed.content_encoding == "gzip");

    CHECK(kStylesheet.content_encoding.empty());
}

TEST_CASE("a weak etag is the documented twenty characters of quoted hexadecimal") {
    CHECK(kStylesheet.etag.size() == kWeakEtagLength);
    CHECK(kStylesheet.etag.starts_with(R"(W/")"));
    CHECK(kStylesheet.etag.ends_with(R"(")"));

    const std::string_view digits = kStylesheet.etag.substr(kEtagPrefixLength, kEtagHexDigitCount);
    CHECK(digits.size() == kEtagHexDigitCount);
    for (const char digit : digits)
        CHECK(kHexDigits.find(digit) != std::string_view::npos);

    const std::string runtime_etag = weak_etag(kStylesheetBytes);
    CHECK(runtime_etag == kStylesheet.etag);
}

TEST_CASE("an empty bundle finds nothing and reports itself empty") {
    const AssetBundle bundle;
    CHECK(bundle.empty());
    CHECK(bundle.all().empty());
    CHECK(bundle.find("/assets/a00.css") == nullptr);
    CHECK(bundle.find("") == nullptr);
    CHECK(bundle.find("/") == nullptr);
}

TEST_CASE("a bundle below the probe threshold finds every asset it holds") {
    const AssetBundle bundle = small_bundle();
    REQUIRE(bundle.all().size() == kSmallBundleCount);
    CHECK_FALSE(bundle.empty());

    for (size_t index = 0; index < kSmallBundleCount; ++index) {
        const Asset* const found = bundle.find(kBundlePaths[index]);
        REQUIRE(found != nullptr);
        CHECK(found->path == kBundlePaths[index]);
        CHECK(found->bytes == kBundlePaths[index]);
        CHECK(found->etag == kBundleEtags[index].view());
    }
}

TEST_CASE("a bundle below the probe threshold returns null for a path it does not hold") {
    const AssetBundle bundle = small_bundle();
    CHECK(bundle.find("/assets/a04.png") == nullptr);
    CHECK(bundle.find("/assets/missing.css") == nullptr);
    CHECK(bundle.find("") == nullptr);
}

TEST_CASE("a sorted bundle above the probe threshold finds every asset it holds") {
    const AssetBundle bundle = sorted_bundle();
    REQUIRE(bundle.all().size() == kLargeBundleCount);

    for (const std::string_view path : kBundlePaths) {
        const Asset* const found = bundle.find(path);
        REQUIRE_MESSAGE(found != nullptr, "a sorted bundle must answer for every path it holds");
        CHECK(found->path == path);
    }
    CHECK(bundle.find("/assets/a24.css") == nullptr);
    CHECK(bundle.find("/assets/a00.cs") == nullptr);
}

TEST_CASE("an unsorted bundle above the probe threshold still finds every asset it holds") {
    const AssetBundle bundle = unsorted_bundle();
    REQUIRE(bundle.all().size() == kLargeBundleCount);

    for (const std::string_view path : kBundlePaths) {
        const Asset* const found = bundle.find(path);
        REQUIRE_MESSAGE(found != nullptr, "an unsorted bundle must not lose an asset to the probe");
        CHECK(found->path == path);
    }
    CHECK(bundle.find("/assets/a24.css") == nullptr);
}

TEST_CASE("find matches the whole path and never a prefix or a suffix of one") {
    const AssetBundle bundle = sorted_bundle();
    CHECK(bundle.find("/assets/a00") == nullptr);
    CHECK(bundle.find("/assets/a00.css/") == nullptr);
    CHECK(bundle.find("assets/a00.css") == nullptr);
    CHECK(bundle.find("/assets/a00.css?v=2") == nullptr);
    CHECK(bundle.find("/ASSETS/A00.CSS") == nullptr);
    CHECK(bundle.find("/assets/a00.css") != nullptr);
}

TEST_CASE("find returns a pointer into the bundle's own storage rather than a copy") {
    constexpr size_t kProbedIndex = 7;
    const AssetBundle bundle = sorted_bundle();
    const Asset* const found = bundle.find(kBundlePaths[kProbedIndex]);
    REQUIRE(found != nullptr);
    CHECK(found == &kAscendingAssets[kProbedIndex]);
    CHECK(found->bytes.data() == kBundlePaths[kProbedIndex].data());
}

} // namespace erikslund::http
