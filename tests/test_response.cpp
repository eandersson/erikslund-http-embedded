
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/response.hpp"
#include "erikslund/http/status.hpp"
#include "erikslund/http/text.hpp"

using namespace erikslund::http;

namespace {

constexpr std::string_view kPlainTextContentType = "text/plain; charset=utf-8";
constexpr std::string_view kHtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view kJsonContentType = "application/json";
constexpr std::string_view kPrometheusContentType = "text/plain; version=0.0.4; charset=utf-8";
constexpr std::string_view kCssContentType = "text/css; charset=utf-8";
constexpr std::string_view kEventStreamContentType = "text/event-stream";

constexpr std::string_view kContentTypeField = "Content-Type";
constexpr std::string_view kLowercaseContentTypeField = "content-type";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kContentEncodingField = "Content-Encoding";
constexpr std::string_view kEtagField = "ETag";
constexpr std::string_view kLocationField = "Location";
constexpr std::string_view kServerTimingField = "Server-Timing";

constexpr std::string_view kNoStoreDirective = "no-store";
constexpr std::chrono::seconds kCacheWindow{3'600};
constexpr std::string_view kCacheWindowDirective = "max-age=3600";
constexpr std::string_view kGzipEncoding = "gzip";
constexpr std::string_view kWeakEtagPrefix = "W/\"";

constexpr size_t kExactlyOneField = 1;

constexpr std::string_view kOwnedBody =
    "erikslund-http: a body long enough that it cannot fit in a small-string buffer\n";

constexpr std::string_view kHtmlBody = "<!doctype html><title>erikslund</title><p>status</p>\n";
constexpr std::string_view kJsonBody = R"({"uptime_seconds":42,"ready":true})";
constexpr std::string_view kMetricsBody = "erikslund_http_up 1\n";

constexpr std::string_view kBorrowedBody = "html{color-scheme:dark light}\n";

constexpr std::string_view kRedirectTarget = "/status/";

constexpr std::string_view kSplittingValue = "0.4\r\nX-Injected: yes";
constexpr std::string_view kSplittingFieldName = "X-Bad\r\nX-Injected";
constexpr std::string_view kSpacedFieldName = "X Bad";

constexpr std::string_view kFirstTimingValue = "router;dur=0.4";
constexpr std::string_view kSecondTimingValue = "router;dur=1.9";
constexpr std::string_view kStreamChunk = "data: first\n\n";

class SingleChunkStream final : public StreamSource {
public:
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string& out) override {
        out += kStreamChunk;
        return Pull::Finished;
    }

    void on_attached(std::shared_ptr<StreamNotifier> notifier) override {
        notifier_ = std::move(notifier);
    }

    void on_detached() noexcept override { notifier_.reset(); }

private:
    std::shared_ptr<StreamNotifier> notifier_;
};

[[nodiscard]] bool has_header(const Response& response, std::string_view name) {
    return response.headers().contains(std::string(name));
}

[[nodiscard]] std::string_view header_value(const Response& response, std::string_view name) {
    const std::string key(name);
    if (!response.headers().contains(key))
        return {};
    return response.headers().at(key);
}

[[nodiscard]] size_t count_fields_named(const Response& response, std::string_view name) {
    size_t matches = 0;
    for (const std::string& existing_name : response.headers().keys())
        if (equals_ignore_case(existing_name, name))
            ++matches;
    return matches;
}

} // namespace

TEST_CASE("text_sets_a_200_and_the_utf8_plain_text_content_type") {
    const Response response = Response::text(std::string(kOwnedBody));
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kOwnedBody);
    CHECK(header_value(response, kContentTypeField) == kPlainTextContentType);
    CHECK_FALSE(response.is_stream());
}

TEST_CASE("html_sets_a_200_and_the_utf8_html_content_type") {
    const Response response = Response::html(std::string(kHtmlBody));
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kHtmlBody);
    CHECK(header_value(response, kContentTypeField) == kHtmlContentType);
}

TEST_CASE("json_sets_a_200_and_the_application_json_content_type") {
    const Response response = Response::json(std::string(kJsonBody));
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kJsonBody);
    CHECK(header_value(response, kContentTypeField) == kJsonContentType);
}

TEST_CASE("prometheus_sets_the_versioned_exposition_content_type") {
    const Response response = Response::prometheus(std::string(kMetricsBody));
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kMetricsBody);
    CHECK(header_value(response, kContentTypeField) == kPrometheusContentType);
}

TEST_CASE("every_body_factory_carries_the_status_it_was_given_and_keeps_its_media_type") {
    const Response not_found = Response::text(std::string(kOwnedBody), Status::NotFound);
    const Response unauthorized = Response::html(std::string(kHtmlBody), Status::Unauthorized);
    const Response too_many = Response::json(std::string(kJsonBody), Status::TooManyRequests);

    CHECK(not_found.status() == Status::NotFound);
    CHECK(unauthorized.status() == Status::Unauthorized);
    CHECK(too_many.status() == Status::TooManyRequests);

    CHECK(header_value(not_found, kContentTypeField) == kPlainTextContentType);
    CHECK(header_value(unauthorized, kContentTypeField) == kHtmlContentType);
    CHECK(header_value(too_many, kContentTypeField) == kJsonContentType);
}

TEST_CASE("empty_declares_no_content_type_for_a_body_of_zero_bytes") {
    const Response response = Response::empty(Status::NoContent);
    CHECK(response.status() == Status::NoContent);
    CHECK(response.body().empty());
    CHECK(response.headers().empty());
}

TEST_CASE("borrowed_serves_the_callers_own_bytes_without_copying_them") {
    const Response response = Response::borrowed(kBorrowedBody, kCssContentType);
    CHECK(response.status() == Status::Ok);
    CHECK(response.body() == kBorrowedBody);
    CHECK(header_value(response, kContentTypeField) == kCssContentType);
    CHECK_MESSAGE(response.body().data() == kBorrowedBody.data(),
                  "a borrowed body must be the caller's bytes, not a copy of them");
}

TEST_CASE("borrowed_omits_the_content_type_when_it_is_given_none") {
    const Response response = Response::borrowed(kBorrowedBody, std::string_view{});
    CHECK(response.body().data() == kBorrowedBody.data());
    CHECK_FALSE(has_header(response, kContentTypeField));
}

TEST_CASE("an_owned_body_survives_a_move_of_the_response") {
    Response original = Response::text(std::string(kOwnedBody));

    const Response moved = std::move(original);

    CHECK_MESSAGE(moved.body() == kOwnedBody, "an owned body must travel with the Response");
    CHECK(moved.status() == Status::Ok);
    CHECK(header_value(moved, kContentTypeField) == kPlainTextContentType);
}

TEST_CASE("an_owned_body_survives_move_assignment_over_a_live_response") {
    Response overwritten = Response::html(std::string(kHtmlBody));
    Response source = Response::text(std::string(kOwnedBody));

    overwritten = std::move(source);

    CHECK(overwritten.body() == kOwnedBody);
    CHECK(header_value(overwritten, kContentTypeField) == kPlainTextContentType);
    CHECK(count_fields_named(overwritten, kContentTypeField) == kExactlyOneField);
}

TEST_CASE("a_borrowed_body_still_points_at_the_original_bytes_after_a_move") {
    Response original = Response::borrowed(kBorrowedBody, kCssContentType);
    const Response moved = std::move(original);
    CHECK(moved.body() == kBorrowedBody);
    CHECK(moved.body().data() == kBorrowedBody.data());
}

TEST_CASE("redirect_sets_location_and_defaults_to_302_found") {
    const Response response = Response::redirect(std::string(kRedirectTarget));
    CHECK(response.status() == Status::Found);
    CHECK(header_value(response, kLocationField) == kRedirectTarget);
    CHECK(response.body().empty());
    CHECK_FALSE(has_header(response, kContentTypeField));
}

TEST_CASE("redirect_carries_the_permanent_status_when_asked_for_one") {
    const Response response =
        Response::redirect(std::string(kRedirectTarget), Status::MovedPermanently);
    CHECK(response.status() == Status::MovedPermanently);
    CHECK(header_value(response, kLocationField) == kRedirectTarget);
}

TEST_CASE("stream_marks_the_response_and_takes_the_content_type_from_the_source") {
    const std::shared_ptr<StreamSource> source = std::make_shared<SingleChunkStream>();
    const Response response = Response::stream(source);

    CHECK(response.is_stream());
    CHECK(response.stream_source() == source);
    CHECK(response.status() == Status::Ok);
    CHECK(header_value(response, kContentTypeField) == kEventStreamContentType);
    CHECK(header_value(response, kCacheControlField) == kNoStoreDirective);
    CHECK(response.body().empty());
}

TEST_CASE("etag_from_body_matches_weak_etag_of_the_body") {
    const Response response = Response::json(std::string(kJsonBody)).etag_from_body();
    CHECK(header_value(response, kEtagField) == weak_etag(kJsonBody));
    CHECK(header_value(response, kEtagField).starts_with(kWeakEtagPrefix));
}

TEST_CASE("etag_from_body_hashes_a_borrowed_body_the_same_way_as_an_owned_one") {
    const Response borrowed_body =
        Response::borrowed(kBorrowedBody, kCssContentType).etag_from_body();
    const Response owned_body = Response::text(std::string(kBorrowedBody)).etag_from_body();
    CHECK(header_value(borrowed_body, kEtagField) == weak_etag(kBorrowedBody));
    CHECK(header_value(owned_body, kEtagField) == header_value(borrowed_body, kEtagField));
}

TEST_CASE("etag_from_body_leaves_a_stream_without_a_validator") {
    Response response = Response::stream(std::make_shared<SingleChunkStream>());
    response.etag_from_body();
    CHECK_FALSE(has_header(response, kEtagField));
}

TEST_CASE("etag_sets_the_validator_verbatim") {
    const Response response =
        Response::text(std::string(kOwnedBody)).etag(weak_etag(kBorrowedBody));
    CHECK(header_value(response, kEtagField) == weak_etag(kBorrowedBody));
}

TEST_CASE("header_replaces_a_same_named_field_instead_of_appending_a_duplicate") {
    Response response = Response::text(std::string(kOwnedBody));
    response.header(std::string(kServerTimingField), std::string(kFirstTimingValue));
    response.header(std::string(kServerTimingField), std::string(kSecondTimingValue));

    CHECK(header_value(response, kServerTimingField) == kSecondTimingValue);
    CHECK(count_fields_named(response, kServerTimingField) == kExactlyOneField);
}

TEST_CASE("header_replaces_a_field_that_differs_only_in_case_and_keeps_the_first_spelling") {
    Response response = Response::html(std::string(kHtmlBody));

    response.header(std::string(kLowercaseContentTypeField), std::string(kJsonContentType));

    CHECK(count_fields_named(response, kContentTypeField) == kExactlyOneField);
    CHECK(has_header(response, kContentTypeField));
    CHECK_FALSE(has_header(response, kLowercaseContentTypeField));
    CHECK(header_value(response, kContentTypeField) == kJsonContentType);
}

TEST_CASE("a_field_value_carrying_a_crlf_is_dropped_rather_than_written_or_terminated_over") {
    Response response = Response::json(std::string(kJsonBody));

    response.header(std::string(kServerTimingField), std::string(kSplittingValue));

    CHECK_FALSE(has_header(response, kServerTimingField));
    CHECK(header_value(response, kContentTypeField) == kJsonContentType);
    CHECK(response.body() == kJsonBody);
}

TEST_CASE("a_rejected_field_leaves_an_existing_field_of_the_same_name_as_it_was") {
    Response response = Response::html(std::string(kHtmlBody));

    response.header(std::string(kContentTypeField), std::string(kSplittingValue));

    CHECK(header_value(response, kContentTypeField) == kHtmlContentType);
    CHECK(count_fields_named(response, kContentTypeField) == kExactlyOneField);
}

TEST_CASE("a_field_name_that_is_not_a_token_is_dropped") {
    Response response = Response::text(std::string(kOwnedBody));

    response.header(std::string(kSplittingFieldName), std::string(kFirstTimingValue));
    response.header(std::string(), std::string(kFirstTimingValue));
    response.header(std::string(kSpacedFieldName), std::string(kFirstTimingValue));

    CHECK(response.headers().keys() ==
          std::vector<std::string>{std::string(kContentTypeField)});
}

TEST_CASE("no_store_after_cache_for_wins_and_leaves_one_cache_control_field") {
    const Response response =
        Response::text(std::string(kOwnedBody)).cache_for(kCacheWindow).no_store();
    CHECK(header_value(response, kCacheControlField) == kNoStoreDirective);
    CHECK(count_fields_named(response, kCacheControlField) == kExactlyOneField);
}

TEST_CASE("cache_for_after_no_store_wins_and_leaves_one_cache_control_field") {
    const Response response =
        Response::text(std::string(kOwnedBody)).no_store().cache_for(kCacheWindow);
    CHECK(header_value(response, kCacheControlField) == kCacheWindowDirective);
    CHECK(count_fields_named(response, kCacheControlField) == kExactlyOneField);
}

TEST_CASE("cache_for_emits_whole_seconds_because_max_age_has_no_other_unit") {
    const Response response = Response::text(std::string(kOwnedBody)).cache_for(kCacheWindow);
    CHECK(header_value(response, kCacheControlField) == kCacheWindowDirective);
}

TEST_CASE("the_fluent_mutators_chain_on_a_temporary_and_on_an_lvalue_alike") {
    const Response chained_on_temporary = Response::html(std::string(kHtmlBody))
                                              .header(std::string(kServerTimingField),
                                                      std::string(kFirstTimingValue))
                                              .content_encoding(std::string(kGzipEncoding))
                                              .no_store()
                                              .etag_from_body();

    Response chained_on_lvalue = Response::html(std::string(kHtmlBody));
    chained_on_lvalue.header(std::string(kServerTimingField), std::string(kFirstTimingValue))
        .content_encoding(std::string(kGzipEncoding))
        .no_store()
        .etag_from_body();

    CHECK(chained_on_temporary.headers().keys() == chained_on_lvalue.headers().keys());
    CHECK(chained_on_temporary.headers().values() == chained_on_lvalue.headers().values());
    CHECK(chained_on_temporary.body() == kHtmlBody);
    CHECK(chained_on_lvalue.body() == kHtmlBody);
    CHECK(header_value(chained_on_temporary, kCacheControlField) == kNoStoreDirective);
    CHECK(header_value(chained_on_temporary, kContentEncodingField) == kGzipEncoding);
    CHECK(header_value(chained_on_temporary, kEtagField) == weak_etag(kHtmlBody));
}

TEST_CASE("the_header_table_iterates_sorted_so_the_emitted_bytes_are_deterministic") {
    Response response = Response::text(std::string(kOwnedBody));
    response.header(std::string(kServerTimingField), std::string(kFirstTimingValue));
    response.content_encoding(std::string(kGzipEncoding));
    response.no_store();

    const std::vector<std::string> expected_names{
        std::string(kCacheControlField), std::string(kContentEncodingField),
        std::string(kContentTypeField), std::string(kServerTimingField)};
    CHECK(std::ranges::is_sorted(response.headers().keys()));
    CHECK(response.headers().keys() == expected_names);
}
