
#include "internal/compress.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/text.hpp"

#if ERIKSLUND_HTTP_ZLIB
#define ZLIB_CONST
#include <zlib.h>
#endif

namespace erikslund::http::internal {

#if ERIKSLUND_HTTP_ZLIB

namespace {

constexpr int kMaximumWindowBits = 15;
constexpr int kGzipFramingOffset = 16;
constexpr int kGzipWindowBits = kMaximumWindowBits + kGzipFramingOffset;

constexpr int kMemoryLevel = 8;

constexpr int kCompressionLevel = Z_DEFAULT_COMPRESSION;

constexpr char kMediaTypeParameterSeparator = ';';
constexpr char kSpace = ' ';
constexpr char kHorizontalTab = '\t';

constexpr std::string_view kTextMediaTypePrefix = "text/";

constexpr std::array<std::string_view, 2> kCompressibleMediaTypeSuffixes{"+json", "+xml"};

constexpr std::array<std::string_view, 4> kCompressibleMediaTypes{
    "application/json", "application/javascript", "application/x-javascript", "application/xml"};

[[nodiscard]] bool starts_with_ignore_case(std::string_view text,
                                           std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool ends_with_ignore_case(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           equals_ignore_case(text.substr(text.size() - suffix.size()), suffix);
}

[[nodiscard]] std::string_view media_type_of(std::string_view content_type) noexcept {
    const size_t parameters = content_type.find(kMediaTypeParameterSeparator);
    std::string_view media_type =
        parameters == std::string_view::npos ? content_type : content_type.substr(0, parameters);
    while (!media_type.empty() &&
           (media_type.front() == kSpace || media_type.front() == kHorizontalTab))
        media_type.remove_prefix(1);
    while (!media_type.empty() &&
           (media_type.back() == kSpace || media_type.back() == kHorizontalTab))
        media_type.remove_suffix(1);
    return media_type;
}

[[nodiscard]] bool media_type_is_compressible(std::string_view content_type) noexcept {
    const std::string_view media_type = media_type_of(content_type);
    if (media_type.empty())
        return false;
    if (starts_with_ignore_case(media_type, kTextMediaTypePrefix))
        return true;
    for (const std::string_view suffix : kCompressibleMediaTypeSuffixes)
        if (ends_with_ignore_case(media_type, suffix))
            return true;
    for (const std::string_view candidate : kCompressibleMediaTypes)
        if (equals_ignore_case(media_type, candidate))
            return true;
    return false;
}

class DeflateStream {
public:
    DeflateStream() = default;

    ~DeflateStream() {
        if (initialised_)
            static_cast<void>(deflateEnd(&stream_));
    }

    DeflateStream(const DeflateStream&) = delete("a z_stream owns zlib's internal allocation");
    DeflateStream& operator=(const DeflateStream&) =
        delete("a z_stream owns zlib's internal allocation");

    [[nodiscard]] bool initialise() noexcept {
        if (deflateInit2(&stream_, kCompressionLevel, Z_DEFLATED, kGzipWindowBits, kMemoryLevel,
                         Z_DEFAULT_STRATEGY) != Z_OK)
            return false;
        initialised_ = true;
        return true;
    }

    [[nodiscard]] z_stream& stream() noexcept { return stream_; }

private:
    z_stream stream_{};
    bool initialised_ = false;
};

} // namespace

bool response_varies_on_accept_encoding(std::string_view content_type,
                                        std::string_view content_encoding,
                                        size_t body_bytes) noexcept {
    if (!content_encoding.empty())
        return false;
    if (body_bytes < kMinimumCompressibleBodyBytes || body_bytes > kMaximumCompressibleBodyBytes)
        return false;
    return media_type_is_compressible(content_type);
}

bool gzip_compress(std::string_view body, std::string& out) {
    out.clear();
    if (body.size() < kMinimumCompressibleBodyBytes || body.size() > kMaximumCompressibleBodyBytes)
        return false;

    DeflateStream deflater;
    if (!deflater.initialise())
        return false;

    z_stream& stream = deflater.stream();
    const auto bound = static_cast<size_t>(deflateBound(&stream, static_cast<uLong>(body.size())));
    out.resize(bound);

    stream.next_in = reinterpret_cast<const Bytef*>(body.data());
    stream.avail_in = static_cast<uInt>(body.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(bound);

    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        out.clear();
        return false;
    }
    out.resize(bound - static_cast<size_t>(stream.avail_out));

    if (out.size() >= body.size()) {
        out.clear();
        return false;
    }
    return true;
}

#else

bool response_varies_on_accept_encoding(std::string_view, std::string_view, size_t) noexcept {
    return false;
}

bool gzip_compress(std::string_view, std::string& out) {
    out.clear();
    return false;
}

#endif

} // namespace erikslund::http::internal
