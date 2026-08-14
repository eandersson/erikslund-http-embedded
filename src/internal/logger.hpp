#pragma once

#include <chrono>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

#include "erikslund/http/server.hpp"

namespace erikslund::http::internal {

inline constexpr unsigned kPeerLogBurst = 16;
inline constexpr std::chrono::seconds kPeerLogWindow{10};

// Makes application logging a containment boundary. Peer-triggered diagnostics share one limiter
// across every reactor so adding workers cannot multiply log volume.
class Logger {
public:
    explicit Logger(LogSink sink) : sink_(std::move(sink)) {}

    void write(LogLevel level, std::string_view message) const noexcept;
    void write_peer(LogLevel level, std::string_view message) noexcept;

    template <class... Args>
    void writef(LogLevel level, std::format_string<Args...> format, Args&&... args) const noexcept {
        try {
            write(level, std::format(format, std::forward<Args>(args)...));
        } catch (...) {
            write(LogLevel::Error, "log message formatting failed");
        }
    }

    template <class... Args>
    void write_peerf(LogLevel level, std::format_string<Args...> format,
                     Args&&... args) noexcept {
        try {
            write_peer(level, std::format(format, std::forward<Args>(args)...));
        } catch (...) {
            write_peer(LogLevel::Error, "peer log message formatting failed");
        }
    }

private:
    LogSink sink_;

    std::mutex peer_mutex_;
    std::chrono::steady_clock::time_point peer_window_started_{};
    unsigned peer_messages_ = 0;
    bool peer_suppression_reported_ = false;
};

} // namespace erikslund::http::internal
