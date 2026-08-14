#pragma once

#include <cstddef>
#include <expected>
#include <inplace_vector>
#include <string_view>

#include "erikslund/http/request.hpp"

namespace erikslund::http::internal {

// Retains a validated request head while its body arrives. Calls for one request must receive the
// same growing byte prefix; returned Request views borrow that prefix.
class RequestParser {
public:
    [[nodiscard]] std::expected<ParsedRequest, ParseError> parse(std::string_view bytes,
                                                                 const RequestLimits& limits);

    [[nodiscard]] bool waiting_for_body() const noexcept { return head_complete_; }

    void reset() noexcept;

private:
    struct BufferViewOffset {
        size_t value = 0;
        bool borrowed = false;
    };

    struct HeaderOffsets {
        BufferViewOffset name;
        BufferViewOffset value;
    };

    [[nodiscard]] std::expected<void, ParseError> parse_head(std::string_view bytes,
                                                             const RequestLimits& limits);
    void remember_buffer_offsets();
    void rebase_borrowed_views(std::string_view bytes) noexcept;

    Request request_;
    const char* buffer_base_ = nullptr;
    BufferViewOffset raw_target_offset_;
    BufferViewOffset path_offset_;
    BufferViewOffset query_offset_;
    BufferViewOffset authority_offset_;
    std::inplace_vector<HeaderOffsets, kMaxParsedHeaders> header_offsets_;
    size_t body_offset_ = 0;
    size_t body_bytes_ = 0;
    bool head_complete_ = false;
};

} // namespace erikslund::http::internal
