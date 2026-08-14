
#include "erikslund/http/build_config.hpp"

#if ERIKSLUND_HTTP_ZLIB

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

constexpr std::string_view kContentEncodingField = "Content-Encoding";
constexpr std::string_view kContentLengthField = "Content-Length";
constexpr std::string_view kVaryField = "Vary";
constexpr std::string_view kAcceptEncodingField = "Accept-Encoding";
constexpr std::string_view kGzipCoding = "gzip";

constexpr size_t kNegotiableBodyBytes = 4'096;

constexpr size_t kAcceptEncodingListLength = 200;

constexpr int kOkStatus = 200;

[[nodiscard]] std::string text_body_of_exactly(size_t bytes) {
    std::string body;
    body.reserve(bytes);
    unsigned word = 0;
    while (body.size() < bytes) {
        body += std::to_string(word);
        body.push_back(' ');
        ++word;
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

[[nodiscard]] Router make_adversarial_router() {
    Router router;
    router.get("/page", [](const Request&) {
        return Response::html(text_body_of_exactly(kNegotiableBodyBytes));
    });
    router.get("/dense", [](const Request&) {
        return Response::html(dense_body_of(kNegotiableBodyBytes));
    });
    router.get("/vary-cased", [](const Request&) {
        Response answer = Response::html(text_body_of_exactly(kNegotiableBodyBytes));
        answer.header(std::string(kVaryField), "accept-encoding");
        return answer;
    });
    return router;
}

TEST_CASE("a_media_type_that_merely_contains_a_compressible_one_is_not_compressible") {
    CHECK_FALSE(response_varies_on_accept_encoding("application/x-text/plain", {},
                                                   kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("texts/plain", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("image/png+xmlish", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("application/jsonp", {}, kNegotiableBodyBytes));
}

TEST_CASE("an_already_compressed_container_type_is_not_compressible") {
    CHECK_FALSE(response_varies_on_accept_encoding("application/gzip", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("application/zip", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("application/wasm", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("video/mp4", {}, kNegotiableBodyBytes));
}

TEST_CASE("a_content_type_that_is_nothing_but_parameters_is_not_compressible") {
    CHECK_FALSE(response_varies_on_accept_encoding("; charset=utf-8", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding(";", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("   ", {}, kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("\t", {}, kNegotiableBodyBytes));
}

TEST_CASE("a_content_encoding_of_only_whitespace_still_counts_as_already_encoded") {
    CHECK_FALSE(response_varies_on_accept_encoding("text/html", " ", kNegotiableBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("text/html", "identity", kNegotiableBodyBytes));
}

TEST_CASE("the_size_boundaries_are_exact_on_both_sides") {
    CHECK_FALSE(response_varies_on_accept_encoding("text/html", {}, 0));
    CHECK_FALSE(response_varies_on_accept_encoding("text/html", {},
                                                   kMinimumCompressibleBodyBytes - 1));
    CHECK(response_varies_on_accept_encoding("text/html", {}, kMinimumCompressibleBodyBytes));
    CHECK(response_varies_on_accept_encoding("text/html", {}, kMaximumCompressibleBodyBytes));
    CHECK_FALSE(response_varies_on_accept_encoding("text/html", {},
                                                   kMaximumCompressibleBodyBytes + 1));
}

TEST_CASE("the_compressor_refuses_every_size_the_negotiation_would_have_refused") {
    std::string compressed;
    CHECK_FALSE(gzip_compress({}, compressed));
    CHECK(compressed.empty());
    CHECK_FALSE(gzip_compress(text_body_of_exactly(kMinimumCompressibleBodyBytes - 1), compressed));
    CHECK(compressed.empty());
    CHECK(gzip_compress(text_body_of_exactly(kMinimumCompressibleBodyBytes), compressed));
    CHECK_FALSE(compressed.empty());
}

TEST_CASE("a_body_deflate_cannot_shrink_is_sent_unencoded_rather_than_larger") {
    const auto fixture = started_test_server(make_adversarial_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/dense", "Accept-Encoding: gzip\r\n"));
    REQUIRE(answer.has_value());
    REQUIRE(answer->complete);
    CHECK(answer->status_code == kOkStatus);
    CHECK_FALSE_MESSAGE(answer->has_header(kContentEncodingField),
                        "an encoding was declared for a body that was never encoded");
    CHECK(answer->body.size() == kNegotiableBodyBytes);
    CHECK(answer->header_value(kContentLengthField) == std::to_string(kNegotiableBodyBytes));
    CHECK(answer->header_value(kVaryField) == kAcceptEncodingField);
}

TEST_CASE("a_vary_that_already_names_accept_encoding_in_another_case_is_not_extended_twice") {
    const auto fixture = started_test_server(make_adversarial_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/vary-cased", "Accept-Encoding: gzip\r\n"));
    REQUIRE(answer.has_value());
    CHECK(answer->header_count(kVaryField) == 1);
    CHECK(answer->header_value(kVaryField) == "accept-encoding");
}

TEST_CASE("an_accept_encoding_that_names_gzip_only_inside_a_longer_token_does_not_enable_it") {
    const auto fixture = started_test_server(make_adversarial_router());

    SUBCASE("a token with gzip as a prefix") {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer =
            client.request(simple_request("GET", "/page", "Accept-Encoding: gzipped\r\n"));
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->has_header(kContentEncodingField));
    }

    SUBCASE("a token with gzip as a suffix") {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer =
            client.request(simple_request("GET", "/page", "Accept-Encoding: notgzip\r\n"));
        REQUIRE(answer.has_value());
        CHECK_FALSE(answer->has_header(kContentEncodingField));
    }
}

TEST_CASE("a_very_long_accept_encoding_list_still_resolves_to_one_answer") {
    const auto fixture = started_test_server(make_adversarial_router());
    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    std::string field = "Accept-Encoding: ";
    for (size_t entry = 0; entry < kAcceptEncodingListLength; ++entry) {
        field += "coding-";
        field += std::to_string(entry);
        field += ";q=0.5, ";
    }
    field += "gzip\r\n";

    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/page", field));
    REQUIRE(answer.has_value());
    REQUIRE(answer->complete);
    CHECK(answer->status_code == kOkStatus);
    CHECK(answer->header_value(kContentEncodingField) == kGzipCoding);
    CHECK(answer->header_value(kContentLengthField) == std::to_string(answer->body.size()));
}

TEST_CASE("a_client_that_hangs_up_before_reading_a_compressed_response_leaves_the_server_serving") {
    const auto fixture = started_test_server(make_adversarial_router());
    {
        TestClient abandoning;
        REQUIRE(abandoning.connect(fixture->port()));
        REQUIRE(abandoning.send_raw(simple_request("GET", "/page", "Accept-Encoding: gzip\r\n")));
        abandoning.close();
    }

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> answer =
        client.request(simple_request("GET", "/page", "Accept-Encoding: gzip\r\n"));
    REQUIRE(answer.has_value());
    CHECK(answer->status_code == kOkStatus);
    CHECK(answer->header_value(kContentEncodingField) == kGzipCoding);
}

} // namespace
} // namespace erikslund::http

#endif
