
#include "erikslund/http/build_config.hpp"

#if ERIKSLUND_HTTP_ZLIB

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#define ZLIB_CONST
#include <zlib.h>

#include <doctest/doctest.h>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "internal/compress.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using internal::gzip_compress;
using internal::kMaximumCompressibleBodyBytes;
using internal::kMinimumCompressibleBodyBytes;
using internal::response_varies_on_accept_encoding;
using test::HttpResponse;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TestServer;

constexpr int kGzipWindowBits = 15 + 16;
constexpr size_t kInflateChunkBytes = 4'096;

constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kJsonContentType = "application/json";
constexpr std::string_view kSvgContentType = "image/svg+xml";
constexpr std::string_view kPngContentType = "image/png";
constexpr std::string_view kOctetStreamContentType = "application/octet-stream";

constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kContentEncodingField = "Content-Encoding";
constexpr std::string_view kContentLengthField = "Content-Length";
constexpr std::string_view kVaryField = "Vary";
constexpr std::string_view kAcceptEncodingField = "Accept-Encoding";
constexpr std::string_view kAcceptLanguageField = "Accept-Language";
constexpr std::string_view kCombinedVaryValue = "Accept-Language, Accept-Encoding";
constexpr std::string_view kGzipCoding = "gzip";

constexpr std::string_view kGzipRequestHeader = "Accept-Encoding: gzip\r\n";
constexpr std::string_view kIdentityRequestHeader = "Accept-Encoding: identity\r\n";

constexpr size_t kCompressibleBodyBytes = 4'096;

constexpr size_t kJustUnderMinimumBytes = kMinimumCompressibleBodyBytes - 1;
constexpr size_t kJustOverMinimumBytes = kMinimumCompressibleBodyBytes + 1;

constexpr int kOkStatus = 200;
constexpr int kNotModifiedStatus = 304;

[[nodiscard]] std::string html_body_of_exactly(size_t bytes) {
    std::string body = "<!doctype html><title>erikslund status</title><table>\n";
    unsigned row = 0;
    while (body.size() < bytes) {
        body += std::format("<tr><td>worker-{}</td><td>accepted</td><td>{} requests</td></tr>\n",
                            row, (row * 37) + 11);
        ++row;
    }
    body.resize(bytes);
    return body;
}

[[nodiscard]] std::string dense_body_of(size_t bytes) {
    constexpr uint64_t kMultiplier = 6'364'136'223'846'793'005ULL;
    constexpr uint64_t kIncrement = 1'442'695'040'888'963'407ULL;
    constexpr unsigned kHighByteShift = 56;

    std::string body;
    body.reserve(bytes);
    uint64_t state = 1;
    for (size_t index = 0; index < bytes; ++index) {
        state = (state * kMultiplier) + kIncrement;
        body.push_back(static_cast<char>(state >> kHighByteShift));
    }
    return body;
}

[[nodiscard]] std::optional<std::string> gunzip(std::string_view compressed) {
    z_stream stream{};
    if (inflateInit2(&stream, kGzipWindowBits) != Z_OK)
        return std::nullopt;

    stream.next_in = reinterpret_cast<const Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());

    std::string plain;
    std::array<char, kInflateChunkBytes> chunk{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            static_cast<void>(inflateEnd(&stream));
            return std::nullopt;
        }
        plain.append(chunk.data(), chunk.size() - stream.avail_out);
    }
    static_cast<void>(inflateEnd(&stream));
    return plain;
}

[[nodiscard]] Router make_compression_router() {
    Router router;
    router.get("/page", [](const Request&) {
        return Response::html(html_body_of_exactly(kCompressibleBodyBytes));
    });
    router.get("/tiny", [](const Request&) {
        return Response::html(html_body_of_exactly(kJustUnderMinimumBytes));
    });
    router.get("/image", [](const Request&) {
        Response answer = Response::text(dense_body_of(kCompressibleBodyBytes));
        answer.header(std::string(kContentTypeField), std::string(kPngContentType));
        return answer;
    });
    router.get("/precompressed", [](const Request&) {
        Response answer = Response::html(html_body_of_exactly(kCompressibleBodyBytes));
        answer.content_encoding(std::string(kGzipCoding));
        return answer;
    });
    router.get("/varies-on-language", [](const Request&) {
        Response answer = Response::html(html_body_of_exactly(kCompressibleBodyBytes));
        answer.header(std::string(kVaryField), std::string(kAcceptLanguageField));
        answer.etag_from_body();
        return answer;
    });
    return router;
}

TEST_CASE("a_compressible_body_is_compressed_and_round_trips_through_a_real_gunzip") {
    const std::string plain = html_body_of_exactly(kCompressibleBodyBytes);
    std::string compressed;

    REQUIRE(gzip_compress(plain, compressed));
    CHECK_MESSAGE(compressed.size() < plain.size(),
                  "a compressed body that is not smaller has cost CPU for nothing");

    const std::optional<std::string> restored = gunzip(compressed);
    REQUIRE_MESSAGE(restored.has_value(),
                    "the bytes did not inflate as a gzip member, so the framing or the CRC "
                    "is wrong");
    CHECK_MESSAGE(*restored == plain, "the round trip changed the body");
}

TEST_CASE("the_compressed_form_begins_with_the_gzip_magic_and_the_deflate_method") {
    constexpr char kFirstMagicByte = static_cast<char>(0x1F);
    constexpr char kSecondMagicByte = static_cast<char>(0x8B);
    constexpr char kDeflateMethodByte = 0x08;
    constexpr size_t kGzipHeaderBytes = 3;

    const std::string plain = html_body_of_exactly(kCompressibleBodyBytes);
    std::string compressed;

    REQUIRE(gzip_compress(plain, compressed));
    REQUIRE(compressed.size() > kGzipHeaderBytes);
    CHECK(compressed[0] == kFirstMagicByte);
    CHECK(compressed[1] == kSecondMagicByte);
    CHECK(compressed[2] == kDeflateMethodByte);
}

TEST_CASE("a_body_below_the_minimum_size_is_never_compressed") {
    const std::string plain = html_body_of_exactly(kJustUnderMinimumBytes);
    REQUIRE(plain.size() < kMinimumCompressibleBodyBytes);

    std::string compressed;
    CHECK_FALSE(gzip_compress(plain, compressed));
    CHECK_MESSAGE(compressed.empty(),
                  "a refused compression must leave the output buffer untouched");
    CHECK_FALSE(response_varies_on_accept_encoding(kHtmlContentType, {}, plain.size()));
}

TEST_CASE("the_minimum_size_boundary_admits_the_first_body_that_reaches_it") {
    CHECK_FALSE(response_varies_on_accept_encoding(kHtmlContentType, {}, kJustUnderMinimumBytes));
    CHECK(response_varies_on_accept_encoding(kHtmlContentType, {}, kMinimumCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding(kHtmlContentType, {}, kJustOverMinimumBytes));
}

TEST_CASE("a_body_above_the_maximum_size_is_never_compressed") {
    CHECK(response_varies_on_accept_encoding(kHtmlContentType, {}, kMaximumCompressibleBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding(kHtmlContentType, {},
                                                   kMaximumCompressibleBodyBytes + 1));
}

TEST_CASE("an_image_is_never_compressed_however_large_it_is") {
    CHECK_FALSE(response_varies_on_accept_encoding(kPngContentType, {}, kCompressibleBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("image/vnd.microsoft.icon", {},
                                                   kCompressibleBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("font/woff2", {}, kCompressibleBodyBytes));
    CHECK_FALSE(
        response_varies_on_accept_encoding(kOctetStreamContentType, {}, kCompressibleBodyBytes));
}

TEST_CASE("an_svg_is_compressed_even_though_it_is_an_image_media_type") {
    CHECK(response_varies_on_accept_encoding(kSvgContentType, {}, kCompressibleBodyBytes));
}

TEST_CASE("text_json_and_javascript_are_compressible_whatever_parameters_follow_the_media_type") {
    CHECK(response_varies_on_accept_encoding("text/plain; charset=utf-8", {},
                                             kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("text/css", {}, kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("TEXT/HTML; CHARSET=UTF-8", {},
                                             kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding(kJsonContentType, {}, kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("application/json ; charset=utf-8", {},
                                             kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("text/javascript; charset=utf-8", {},
                                             kCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("application/manifest+json", {},
                                             kCompressibleBodyBytes));
}

TEST_CASE("a_body_with_no_declared_content_type_is_never_compressed") {
    CHECK_FALSE(response_varies_on_accept_encoding({}, {}, kCompressibleBodyBytes));
}

TEST_CASE("an_asset_that_already_carries_a_content_encoding_is_left_alone") {
    CHECK_FALSE(response_varies_on_accept_encoding(kHtmlContentType, kGzipCoding,
                                                   kCompressibleBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding(kSvgContentType, "br", kCompressibleBodyBytes));
}

TEST_CASE("an_incompressible_body_is_refused_rather_than_sent_larger_than_it_arrived") {
    const std::string dense = dense_body_of(kCompressibleBodyBytes);
    std::string compressed;

    CHECK_FALSE_MESSAGE(gzip_compress(dense, compressed),
                        "deflate over dense bytes adds framing, and sending more bytes than the "
                        "handler produced is worse than not compressing at all");
    CHECK(compressed.empty());
}

TEST_CASE("a_gzip_capable_client_receives_a_compressed_body_with_content_encoding_and_vary") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/page", kGzipRequestHeader));
    REQUIRE(answer.has_value());
    REQUIRE(answer->complete);
    CHECK(answer->status_code == kOkStatus);
    CHECK(answer->header_value(kContentEncodingField) == kGzipCoding);
    CHECK(answer->header_value(kVaryField) == kAcceptEncodingField);
    CHECK_MESSAGE(answer->header_count(kContentEncodingField) == 1,
                  "two Content-Encoding fields would frame the body twice");

    CHECK(answer->header_value(kContentLengthField) == std::to_string(answer->body.size()));

    const std::optional<std::string> restored = gunzip(answer->body);
    REQUIRE(restored.has_value());
    CHECK(*restored == html_body_of_exactly(kCompressibleBodyBytes));
}

TEST_CASE("a_client_that_did_not_offer_gzip_always_receives_the_body_as_the_handler_wrote_it") {
    const auto fixture = started_test_server(make_compression_router());
    const std::string expected = html_body_of_exactly(kCompressibleBodyBytes);

    SUBCASE("with no Accept-Encoding at all") {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer = client.request(simple_request("GET", "/page"));
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->has_header(kContentEncodingField));
        CHECK(answer->body == expected);
    }

    SUBCASE("with an Accept-Encoding that names only identity") {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer =
            client.request(simple_request("GET", "/page", kIdentityRequestHeader));
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->has_header(kContentEncodingField));
        CHECK(answer->body == expected);
    }

    SUBCASE("with gzip explicitly refused by a zero quality value") {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer =
            client.request(simple_request("GET", "/page", "Accept-Encoding: gzip;q=0, *\r\n"));
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->has_header(kContentEncodingField));
        CHECK(answer->body == expected);
    }
}

TEST_CASE("a_negotiable_response_carries_vary_even_when_the_client_asked_for_no_encoding") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer = client.request(simple_request("GET", "/page"));
    REQUIRE(answer.has_value());
    CHECK(answer->header_value(kVaryField) == kAcceptEncodingField);
}

TEST_CASE("an_image_response_is_sent_uncompressed_to_a_client_that_offered_gzip") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/image", kGzipRequestHeader));
    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->has_header(kContentEncodingField));
    CHECK_MESSAGE(!answer->has_header(kVaryField),
                  "a response that can never be negotiated must not tell a cache that it can");
    CHECK(answer->body.size() == kCompressibleBodyBytes);
}

TEST_CASE("a_small_response_is_sent_uncompressed_to_a_client_that_offered_gzip") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/tiny", kGzipRequestHeader));
    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->has_header(kContentEncodingField));
    CHECK(answer->body == html_body_of_exactly(kJustUnderMinimumBytes));
}

TEST_CASE("a_pre_compressed_asset_keeps_its_own_encoding_and_is_not_gzipped_twice") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/precompressed", kGzipRequestHeader));
    REQUIRE(answer.has_value());
    CHECK(answer->header_count(kContentEncodingField) == 1);
    CHECK(answer->header_value(kContentEncodingField) == kGzipCoding);
    CHECK(answer->body == html_body_of_exactly(kCompressibleBodyBytes));
}

TEST_CASE("a_vary_the_handler_set_keeps_its_own_field_and_gains_accept_encoding") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/varies-on-language", kGzipRequestHeader));
    REQUIRE(answer.has_value());
    CHECK_MESSAGE(answer->header_count(kVaryField) == 1,
                  "one field carrying the whole list, not two a cache has to combine");
    CHECK(answer->header_value(kVaryField) == kCombinedVaryValue);
}

TEST_CASE("a_derived_304_keeps_every_vary_dimension_of_the_compressible_response") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> full =
        client.request(simple_request("GET", "/varies-on-language", kGzipRequestHeader));
    REQUIRE(full.has_value());
    REQUIRE(full->has_header("ETag"));
    CHECK(full->header_value(kVaryField) == kCombinedVaryValue);

    std::string conditional_headers(kGzipRequestHeader);
    conditional_headers += "If-None-Match: ";
    conditional_headers += full->header_value("ETag");
    conditional_headers += "\r\n";
    const std::optional<HttpResponse> conditional = client.request(
        simple_request("GET", "/varies-on-language", conditional_headers));
    REQUIRE(conditional.has_value());
    CHECK(conditional->status_code == kNotModifiedStatus);
    CHECK(conditional->body.empty());
    CHECK_MESSAGE(conditional->header_value(kVaryField) == kCombinedVaryValue,
                  "a cache must key the 304 on both language and content coding");
}

TEST_CASE("head_declares_the_same_length_and_coding_the_matching_get_would_have_sent") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> full =
        client.request(simple_request("GET", "/page", kGzipRequestHeader));
    REQUIRE(full.has_value());

    const std::optional<HttpResponse> head =
        client.request(simple_request("HEAD", "/page", kGzipRequestHeader),
                       test::kDefaultResponseTimeout, test::BodyExpectation::HeadRequest);
    REQUIRE(head.has_value());
    CHECK(head->body.empty());
    CHECK(head->header_value(kContentEncodingField) == kGzipCoding);
    CHECK(head->header_value(kContentLengthField) == full->header_value(kContentLengthField));
    CHECK(head->header_value(kVaryField) == full->header_value(kVaryField));
}

TEST_CASE("a_keep_alive_connection_alternates_between_compressed_and_plain_answers") {
    const auto fixture = started_test_server(make_compression_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::string expected = html_body_of_exactly(kCompressibleBodyBytes);

    const std::optional<HttpResponse> compressed =
        client.request(simple_request("GET", "/page", kGzipRequestHeader));
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->header_value(kContentEncodingField) == kGzipCoding);

    const std::optional<HttpResponse> plain = client.request(simple_request("GET", "/page"));
    REQUIRE(plain.has_value());
    REQUIRE(plain->complete);
    CHECK_FALSE(plain->has_header(kContentEncodingField));
    CHECK(plain->body == expected);

    const std::optional<HttpResponse> again =
        client.request(simple_request("GET", "/page", kGzipRequestHeader));
    REQUIRE(again.has_value());
    REQUIRE(again->complete);
    CHECK(again->header_value(kContentEncodingField) == kGzipCoding);
    const std::optional<std::string> restored = gunzip(again->body);
    REQUIRE(restored.has_value());
    CHECK(*restored == expected);
}

} // namespace
} // namespace erikslund::http

#endif
