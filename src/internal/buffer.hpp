#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "erikslund/http/contracts.hpp"

namespace erikslund::http::internal {

inline constexpr size_t kInitialBufferBytes = 4'096;
inline constexpr size_t kBufferGrowthFactor = 2;

// Reactor-thread-only buffer with a consumed-prefix cursor.
class Buffer {
public:
    Buffer() = default;
    explicit Buffer(size_t initial_capacity) : storage_(initial_capacity) {}

    // Invalidated by the next non-const call.
    [[nodiscard]] std::span<char> writable_tail(size_t minimum) {
        if (writable_capacity() < minimum)
            grow_to(std::max(write_cursor_ + minimum, storage_.size() * kBufferGrowthFactor));
        return std::span<char>(storage_.data() + write_cursor_, storage_.size() - write_cursor_);
    }

    void commit(size_t bytes) ERIKSLUND_HTTP_PRE(bytes <= writable_capacity()) {
        write_cursor_ += bytes;
    }

    [[nodiscard]] std::string_view readable() const noexcept {
        return std::string_view(storage_.data() + read_cursor_, write_cursor_ - read_cursor_);
    }

    void consume(size_t bytes) ERIKSLUND_HTTP_PRE(bytes <= size()) {
        read_cursor_ += bytes;
        if (read_cursor_ == write_cursor_)
            clear();
    }

    void compact() {
        if (read_cursor_ == 0)
            return;
        const size_t live = size();
        if (live > 0)
            std::copy_n(storage_.data() + read_cursor_, live, storage_.data());
        read_cursor_ = 0;
        write_cursor_ = live;
    }

    void compact_and_reclaim(size_t retained_capacity) {
        compact();
        const size_t target = std::max({size(), retained_capacity, kInitialBufferBytes});
        if (storage_.size() <= target)
            return;

        std::vector<char> reclaimed(target);
        if (!empty())
            std::copy_n(storage_.data(), size(), reclaimed.data());
        storage_.swap(reclaimed);
    }

    void clear() noexcept {
        read_cursor_ = 0;
        write_cursor_ = 0;
    }

    void append(std::string_view bytes) {
        const std::span<char> tail = writable_tail(bytes.size());
        std::copy_n(bytes.data(), bytes.size(), tail.data());
        commit(bytes.size());
    }

    [[nodiscard]] size_t size() const noexcept { return write_cursor_ - read_cursor_; }
    [[nodiscard]] bool empty() const noexcept { return write_cursor_ == read_cursor_; }
    [[nodiscard]] size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] size_t writable_capacity() const noexcept {
        return storage_.size() - write_cursor_;
    }

    [[nodiscard]] const char* read_pointer() const noexcept {
        return storage_.data() + read_cursor_;
    }

private:
    void grow_to(size_t target) {
        storage_.resize(std::max(target, kInitialBufferBytes));
    }

    std::vector<char> storage_;
    size_t read_cursor_ = 0;
    size_t write_cursor_ = 0;
};

} // namespace erikslund::http::internal
