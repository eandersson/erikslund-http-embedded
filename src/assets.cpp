#include "erikslund/http/assets.hpp"

#include <cstddef>
#include <span>
#include <string_view>

#include "erikslund/http/text.hpp"

namespace erikslund::http {

namespace {

constexpr size_t kBinarySearchThreshold = 16;

[[nodiscard]] const Asset* binary_probe(std::span<const Asset> assets,
                                        std::string_view path) noexcept {
    size_t low = 0;
    size_t high = assets.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const int ordering = assets[middle].path.compare(path);
        if (ordering == 0)
            return &assets[middle];
        if (ordering < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return nullptr;
}

constexpr std::string_view kProbeBytes = "erikslund";
constexpr EtagBuffer kProbeEtag = weak_etag_buffer(kProbeBytes);
constexpr Asset kProbeAsset = make_asset("/probe.css", kProbeBytes, kProbeEtag);

static_assert(kProbeAsset.path == "/probe.css");
static_assert(kProbeAsset.content_type == "text/css; charset=utf-8");
static_assert(kProbeAsset.bytes == kProbeBytes);
static_assert(kProbeAsset.etag == weak_etag_buffer(fnv1a_64("erikslund")).view());
static_assert(kProbeAsset.etag.size() == kWeakEtagLength);
static_assert(kProbeAsset.content_encoding.empty());

static_assert(content_type_for_extension("/no-extension") == "application/octet-stream");
static_assert(content_type_for_extension("/upload.unknown") == "application/octet-stream");

} // namespace

const Asset* AssetBundle::find(std::string_view path) const noexcept {
    if (assets_.size() >= kBinarySearchThreshold) {
        const Asset* const probed = binary_probe(assets_, path);
        if (probed != nullptr)
            return probed;
    }
    for (const Asset& asset : assets_)
        if (asset.path == path)
            return &asset;
    return nullptr;
}

} // namespace erikslund::http
