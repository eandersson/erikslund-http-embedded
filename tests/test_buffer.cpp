#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "internal/buffer.hpp"

namespace erikslund::http::internal {
namespace {

constexpr size_t kLargeRequestBytes = 1'048'576;
constexpr size_t kRetainedRequestHeadBytes = 24'576;
constexpr std::string_view kPipelinedRequest = "GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n";

} // namespace

TEST_CASE("reclaiming an empty oversized buffer returns it to the retained ceiling") {
    Buffer buffer;
    buffer.append(std::string(kLargeRequestBytes, 'x'));
    REQUIRE(buffer.capacity() >= kLargeRequestBytes);

    buffer.consume(buffer.size());
    buffer.compact_and_reclaim(kRetainedRequestHeadBytes);

    CHECK(buffer.empty());
    CHECK(buffer.capacity() == kRetainedRequestHeadBytes);
}

TEST_CASE("reclaiming an oversized buffer preserves a pipelined request") {
    Buffer buffer;
    buffer.append(std::string(kLargeRequestBytes, 'x'));
    buffer.append(kPipelinedRequest);
    buffer.consume(kLargeRequestBytes);

    buffer.compact_and_reclaim(kRetainedRequestHeadBytes);

    CHECK(buffer.readable() == kPipelinedRequest);
    CHECK(buffer.capacity() == kRetainedRequestHeadBytes);
}

TEST_CASE("reclaiming never makes a buffer smaller than its live bytes") {
    const std::string live(kRetainedRequestHeadBytes * 2, 'x');
    Buffer buffer;
    buffer.append(live);

    buffer.compact_and_reclaim(kInitialBufferBytes);

    CHECK(buffer.readable() == live);
    CHECK(buffer.capacity() == live.size());
}

} // namespace erikslund::http::internal
